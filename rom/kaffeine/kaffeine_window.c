/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Windows of 68k code: their screens, their fields, their IDCMP ports and
    the IDCMP messages that come through them. A window opens on a screen
    of the program, one that it opened or locked, whose native screen goes
    to Intuition, or on the default public screen; any other screen stops
    the program. The IDCMP port of a window is a 68k MsgPort with the signal
    bit of the native port, so that a program can Wait() for it. Each
    native IntuiMessage that a program takes gets a 68k IntuiMessage with
    every field, its window as the 68k Window, and the fields of that
    window brought up to date; replying frees it. WaitPort()
    shows the first message of the port as a 68k message of a type of its
    own, which becomes the program's when GetMsg() takes that message, so
    that both give the same 68k message; the bridge never reads the native
    message behind a 68k one that WaitPort() showed, because something else
    may have taken it from the port in the meantime. GadTools hands out a
    copy of its own of each message, again with a type of its own;
    GT_GetIMsg() and GT_ReplyIMsg() work as AROS's do, through GetMsg(),
    GT_FilterIMsg(), GT_PostFilterIMsg() and ReplyMsg(), so that the bridge
    sees each original go back too. CloseWindow() gives Intuition and
    GadTools back every message of the window that the program has not
    replied, since the program cannot reply it once its 68k message is
    gone.
*/

#include "kaffeine_intern.h"
#include "kaffeine_async_wait.h"
#include "kaffeine_messages.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_layers.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"
#include "kaffeine_taglist.h"
#include "kaffeine_window.h"
#include "kaffeine_workbench.h"
#include "kaffeine_sched.h"

#include <aros/debug.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k MsgPort and IntuiMessage of the NDK */
enum
{
    G_LN_SUCC = 0, G_LN_PRED = 4, G_LN_TYPE = 8, G_LN_PRI = 9, G_MN_LENGTH = 18,
    G_MP_FLAGS = 14, G_MP_SIGBIT = 15, G_MP_SIGTASK = 16, G_MP_MSGLIST = 20,
    G_LH_TAIL = 4, G_LH_TAILPRED = 8,
    G_IM_CLASS = 20, G_IM_CODE = 24, G_IM_QUALIFIER = 26, G_IM_IADDRESS = 28,
    G_IM_MOUSEX = 32, G_IM_MOUSEY = 34, G_IM_SECONDS = 36, G_IM_MICROS = 40,
    G_IM_IDCMPWINDOW = 44
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

static ULONG shared_identity(struct KaffeineContext *, struct MsgPort *);
static void shared_open_failed(struct KaffeineContext *, struct Window *);

/* Stop the program with return code 20. The thunk returns it, so that it
   reaches D0, from which the CPU loop takes the exit code. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->callback_failed = TRUE; /* authoritative even when an enclosing void thunk returns 0 */
    ctx->exit_code = 20;
    return 20;
}

/* ── Opening windows ─────────────────────────────────────────────── */

BOOL Kaffeine_WindowGadgets(struct KaffeineContext *ctx, ULONG gadgets, const char *call,
                           struct Gadget **native)
{
    *native = NULL;
    if (!gadgets)
        return TRUE;
    /* NULL is a list that stays off the window, or one that stopped the
       program */
    if (ctx->window_gadgets)
        return (*native = ctx->window_gadgets->prepare(ctx, gadgets, call)) != NULL ||
               ctx->running;
    if (Kaffeine_ShadowNative(ctx, gadgets, KAFFEINE_SHADOW_GADGET))
    {
        refuse(ctx, "a window with GadTools gadgets that the bridge does not know");
        return FALSE;
    }
    bug("[kaffeine] %s: the gadgets at %08lx are left out\n", call, (unsigned long)gadgets);
    return TRUE;
}

/* The tags of a list that take the place of fields of a NewWindow */
enum { SEEN_SCREEN = 1, SEEN_TITLE = 2, SEEN_GADGETS = 4 };

/* Kaffeine_WindowTags() up to its gadgets: the last WA_Gadgets is left in
   *guest, at item *last, as TAG_IGNORE; *seen holds which of the tags
   above the list has. */
static struct TagItem *window_tags(struct KaffeineContext *ctx, ULONG list, ULONG *size,
                                   ULONG *screen, ULONG *guest, ULONG *last, ULONG *seen)
{
    struct KaffeineTagWalk walk;
    ULONG item, tag, data, n = 0, i = 0, bad = 0;
    struct TagItem *tags;
    int step;

    *screen = *guest = *last = *seen = 0;
    Kaffeine_TagWalkInit(&walk, list);
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
        n++;
    if (step == KAFFEINE_TAG_BAD)
    {
        refuse(ctx, "OpenWindowTagList(): a tag list outside guest memory or without an end");
        return NULL;
    }
    *size = (n + 1) * sizeof(*tags);
    if (!(tags = AllocMem(*size, MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    Kaffeine_TagWalkInit(&walk, list);
    for (; i < n && Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM; i++)
    {
        tags[i].ti_Tag = tag = m68k_read32(ctx, item);
        data = m68k_read32(ctx, item + 4);
#ifdef WA_UserPort
        if (tag == WA_UserPort)
        {
            refuse(ctx, "OpenWindowTagList(): WA_UserPort needs a typed native port");
            FreeMem(tags, *size);
            return NULL;
        }
#endif
        if (tag == WA_Title || tag == WA_ScreenTitle || tag == WA_PubScreenName)
        {
            tags[i].ti_Data = (IPTR)m68k_to_host(ctx, data);
            if (tag == WA_Title)
                *seen |= SEEN_TITLE;
        }
        else if (tag == WA_CustomScreen || tag == WA_PubScreen)
        {
            /* NULL stays NULL, the default public screen for WA_PubScreen */
            tags[i].ti_Data = (IPTR)Kaffeine_ScreenNative(ctx, data);
            if (data && !tags[i].ti_Data && !bad)
                bad = tag;
            *screen = data;
            *seen |= SEEN_SCREEN;
        }
        else if (tag == WA_Gadgets)
        {
            /* Intuition takes the last one */
            *guest = data;
            *last = i;
            *seen |= SEEN_GADGETS;
            tags[i].ti_Tag = TAG_IGNORE;
        }
        else
            tags[i].ti_Data = data;
    }
    tags[n].ti_Tag = TAG_DONE;
    /* Intuition would read a Screen at any other address. */
    if (bad)
    {
        refuse(ctx, bad == WA_PubScreen ?
                    "OpenWindowTagList(): WA_PubScreen is not a screen of the program" :
                    "OpenWindowTagList(): WA_CustomScreen is not a screen of the program");
        FreeMem(tags, *size);
        return NULL;
    }
    return tags;
}

/* The native list of the gadgets guest of a new window, as WA_Gadgets at
   item last of tags when the list came from there. FALSE, with tags
   freed, when the program stops. */
static BOOL window_tags_gadgets(struct KaffeineContext *ctx, struct TagItem *tags, ULONG size,
                                ULONG guest, ULONG last, BOOL tagged, struct Gadget **gadgets)
{
    (void)size; /* Some host FreeMem macros do not evaluate their size argument. */
    if (!Kaffeine_WindowGadgets(ctx, guest, "OpenWindowTagList()", gadgets))
    {
        if (tags)
            FreeMem(tags, size);
        return FALSE;
    }
    if (*gadgets && tagged)
    {
        tags[last].ti_Tag = WA_Gadgets;
        tags[last].ti_Data = (IPTR)*gadgets;
    }
    return TRUE;
}

struct TagItem *Kaffeine_WindowTags(struct KaffeineContext *ctx, ULONG list, ULONG *size,
                                   ULONG *screen, struct Gadget **gadgets)
{
    ULONG guest, last, seen;
    struct TagItem *tags;

    *gadgets = NULL;
    tags = window_tags(ctx, list, size, screen, &guest, &last, &seen);
    if (!tags || !window_tags_gadgets(ctx, tags, *size, guest, last, TRUE, gadgets))
        return NULL;
    return tags;
}

/* The 68k Window of a window that has just opened, or 0 when none opened.
   Its WScreen is the 68k screen that the program named for it when the
   window lies there, and a GadTools list of the program that it took is
   on it; without guest memory for the 68k Window the window goes again,
   and such a list comes off it first. */
static ULONG window_opened(struct KaffeineContext *ctx, struct Window *w, ULONG screen,
                           struct Gadget *gadgets)
{
    ULONG guest;

    if (!w)
    {
        /* what the bridge made of the list for it goes */
        if (gadgets)
            ctx->window_gadgets->dropped(ctx);
        return 0;
    }
    if (!(guest = Kaffeine_WindowCreate(ctx, w)))
    {
        /* a GadTools list that it took and laid out comes off it first */
        if (gadgets)
        {
            ctx->window_gadgets->opened(ctx, w, gadgets);
            ctx->window_gadgets->closing(ctx, w);
        }
        shared_open_failed(ctx, w);
        CloseWindow(w);
        return 0;
    }
    if (screen && w->WScreen == Kaffeine_ScreenNative(ctx, screen))
        m68k_write32(ctx, guest + M68K_WIN_WSCREEN, screen);
    if (gadgets)
        ctx->window_gadgets->opened(ctx, w, gadgets);
    return guest;
}

/* A string that ends inside guest memory, else NULL */
static UBYTE *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!guest_range(ctx, addr, 1) || !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return ctx->mem + addr;
}

/* OpenWindowTagList() with a NewWindow: Intuition gets a native
   ExtNewWindow of its own, whose Extension is NULL, so that it never reads
   past it whatever WFLG_NW_EXTENDED says, and the tags of A1 as one flat
   native list. Without A1 the Extension of an ExtNewWindow is that list,
   left out as 68k Intuition leaves it out when it is odd or no memory
   (openwindow.c). As in Intuition, WA_Gadgets, WA_Title and the screen
   tags take the place of FirstGadget, Title and Type with Screen, which
   are not read then; the Screen of WBENCHSCREEN is not read either.
   Otherwise the program stops before Intuition for a NewWindow outside
   guest memory, a Title without an end there, a Screen that is not a
   screen of the program, gadgets that the bridge refuses, and a BitMap,
   which the bridge cannot give Intuition yet. The legacy CheckMark is
   unused by native AROS OpenWindow(); the native copy keeps it NULL. */
static IPTR open_newwindow(struct KaffeineContext *ctx, ULONG nw, ULONG list)
{
    struct ExtNewWindow enw;
    struct TagItem *tags = NULL;
    struct Gadget *gadgets = NULL;
    struct Screen *scr = NULL;
    struct Window *w;
    UBYTE *title = NULL;
    ULONG size = 0, screen = 0, guest = 0, last = 0, seen = 0, flags, addr;
    UWORD type;

    if (!guest_range(ctx, nw, M68K_SIZEOF_NEWWINDOW))
        return refuse(ctx, "OpenWindowTagList(): a NewWindow outside guest memory");
    flags = m68k_read32(ctx, nw + M68K_NW_FLAGS);
    if (flags & WFLG_NW_EXTENDED)
    {
        if (!guest_range(ctx, nw, M68K_SIZEOF_EXTNEWWINDOW))
            return refuse(ctx, "OpenWindowTagList(): an ExtNewWindow outside guest memory");
        if (!list)
        {
            list = m68k_read32(ctx, nw + M68K_ENW_EXTENSION);
            if ((list & 1) || !guest_range(ctx, list, 4))
            {
                if (list)
                    bug("[kaffeine] OpenWindowTagList(): the Extension %08lx is left out\n",
                        (unsigned long)list);
                list = 0;
            }
        }
    }
    if (m68k_read32(ctx, nw + M68K_NW_BITMAP))
        return refuse(ctx, "OpenWindowTagList(): a NewWindow with a BitMap");
    if (list && !(tags = window_tags(ctx, list, &size, &screen, &guest, &last, &seen)))
        return ctx->running ? 0 : (IPTR)ctx->exit_code;

    type = m68k_read16(ctx, nw + M68K_NW_TYPE);
    if (!(seen & SEEN_SCREEN) && type != WBENCHSCREEN &&
        (screen = m68k_read32(ctx, nw + M68K_NW_SCREEN)) &&
        !(scr = Kaffeine_ScreenNative(ctx, screen)))
    {
        if (tags)
            FreeMem(tags, size);
        return refuse(ctx, "OpenWindowTagList(): Screen is not a screen of the program");
    }
    if (!(seen & SEEN_TITLE) && (addr = m68k_read32(ctx, nw + M68K_NW_TITLE)) &&
        !(title = guest_string(ctx, addr)))
    {
        if (tags)
            FreeMem(tags, size);
        return refuse(ctx, "OpenWindowTagList(): a Title outside guest memory");
    }
    if (!(seen & SEEN_GADGETS))
        guest = m68k_read32(ctx, nw + M68K_NW_FIRSTGADGET);
    if (!window_tags_gadgets(ctx, tags, size, guest, last, (seen & SEEN_GADGETS) != 0, &gadgets))
        return ctx->exit_code;

    memset(&enw, 0, sizeof(enw));
    enw.LeftEdge = (WORD)m68k_read16(ctx, nw + M68K_NW_LEFTEDGE);
    enw.TopEdge = (WORD)m68k_read16(ctx, nw + M68K_NW_TOPEDGE);
    enw.Width = (WORD)m68k_read16(ctx, nw + M68K_NW_WIDTH);
    enw.Height = (WORD)m68k_read16(ctx, nw + M68K_NW_HEIGHT);
    enw.DetailPen = ctx->mem[nw + M68K_NW_DETAILPEN];
    enw.BlockPen = ctx->mem[nw + M68K_NW_BLOCKPEN];
    enw.IDCMPFlags = m68k_read32(ctx, nw + M68K_NW_IDCMPFLAGS);
    enw.Flags = flags;
    enw.FirstGadget = seen & SEEN_GADGETS ? NULL : gadgets;
    enw.Title = title;
    enw.Screen = scr;
    enw.MinWidth = (WORD)m68k_read16(ctx, nw + M68K_NW_MINWIDTH);
    enw.MinHeight = (WORD)m68k_read16(ctx, nw + M68K_NW_MINHEIGHT);
    enw.MaxWidth = m68k_read16(ctx, nw + M68K_NW_MAXWIDTH);
    enw.MaxHeight = m68k_read16(ctx, nw + M68K_NW_MAXHEIGHT);
    enw.Type = type;
    w = OpenWindowTagList((struct NewWindow *)&enw, tags);
    if (tags)
        FreeMem(tags, size);
    return window_opened(ctx, w, screen, gadgets);
}

/* intuition -606: OpenWindowTagList(A0=newWindow, A1=tagList) -> D0: the
   68k Window. Without a NewWindow Intuition gets NULL, and with it
   WA_AutoAdjust, as before. */
IPTR Kaffeine_OpenWindowTagList(struct KaffeineContext *ctx, void *cpu)
{
    ULONG size = 0, screen;
    struct Gadget *gadgets;
    struct TagItem *tags;
    struct Window *w;

    if (THUNK_A(0))
        return open_newwindow(ctx, THUNK_A(0), THUNK_A(1));
    if (!(tags = Kaffeine_WindowTags(ctx, THUNK_A(1), &size, &screen, &gadgets)))
        return ctx->running ? 0 : (IPTR)ctx->exit_code;
    w = OpenWindowTagList(NULL, tags);
    FreeMem(tags, size);
    return window_opened(ctx, w, screen, gadgets);
}

/* intuition -204: OpenWindow(A0=newWindow) -> D0: the 68k Window. The
   fields of the NewWindow go to Intuition as tags, and its Type picks the
   screen as Intuition does: WBENCHSCREEN the Workbench, whatever Screen
   holds; PUBLICSCREEN the public screen in Screen, the default one for
   NULL; any other type the screen in Screen, or the Workbench for NULL, as
   before. A Screen that is not a screen of the program stops the program,
   as does a NewWindow outside guest memory. */
IPTR Kaffeine_OpenWindow(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nw = THUNK_A(0), screen;
    struct Screen *scr = NULL;
    struct Gadget *gadgets;
    UWORD type;

    if (!nw)
        return 0;
    if (!guest_range(ctx, nw, M68K_SIZEOF_NEWWINDOW))
        return refuse(ctx, "OpenWindow(): a NewWindow outside guest memory");
    if (!Kaffeine_WindowGadgets(ctx, m68k_read32(ctx, nw + M68K_NW_FIRSTGADGET), "OpenWindow()",
                               &gadgets))
        return ctx->exit_code;
    type = m68k_read16(ctx, nw + M68K_NW_TYPE);
    screen = type == WBENCHSCREEN ? 0 : m68k_read32(ctx, nw + M68K_NW_SCREEN);
    if (screen && !(scr = Kaffeine_ScreenNative(ctx, screen)))
        return refuse(ctx, "OpenWindow(): Screen is not a screen of the program");

    struct TagItem tags[] =
    {
        { WA_Left, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_LEFTEDGE) },
        { WA_Top, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_TOPEDGE) },
        { WA_Width, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_WIDTH) },
        { WA_Height, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_HEIGHT) },
        { WA_DetailPen, ctx->mem[nw + M68K_NW_DETAILPEN] },
        { WA_BlockPen, ctx->mem[nw + M68K_NW_BLOCKPEN] },
        { WA_IDCMP, m68k_read32(ctx, nw + M68K_NW_IDCMPFLAGS) },
        { WA_Flags, m68k_read32(ctx, nw + M68K_NW_FLAGS) },
        { WA_Title, (IPTR)m68k_to_host(ctx, m68k_read32(ctx, nw + M68K_NW_TITLE)) },
        { type == PUBLICSCREEN ? WA_PubScreen : scr ? WA_CustomScreen : TAG_IGNORE, (IPTR)scr },
        { WA_MinWidth, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_MINWIDTH) },
        { WA_MinHeight, (IPTR)(WORD)m68k_read16(ctx, nw + M68K_NW_MINHEIGHT) },
        { WA_MaxWidth, m68k_read16(ctx, nw + M68K_NW_MAXWIDTH) },
        { WA_MaxHeight, m68k_read16(ctx, nw + M68K_NW_MAXHEIGHT) },
        { gadgets ? WA_Gadgets : TAG_IGNORE, (IPTR)gadgets },
        { TAG_DONE, 0 }
    };

    return window_opened(ctx, OpenWindowTagList(NULL, tags), screen, gadgets);
}

/* ── Windows ─────────────────────────────────────────────────────── */

void Kaffeine_WindowSync(struct KaffeineContext *ctx, ULONG mw, struct Window *w)
{
    ULONG shared = shared_identity(ctx, w->UserPort);
    if (shared) m68k_write32(ctx, mw + M68K_WIN_USERPORT, shared);
    m68k_write16(ctx, mw + M68K_WIN_LEFTEDGE, (UWORD)w->LeftEdge);
    m68k_write16(ctx, mw + M68K_WIN_TOPEDGE, (UWORD)w->TopEdge);
    m68k_write16(ctx, mw + M68K_WIN_WIDTH, (UWORD)w->Width);
    m68k_write16(ctx, mw + M68K_WIN_HEIGHT, (UWORD)w->Height);
    m68k_write16(ctx, mw + M68K_WIN_MOUSEY, (UWORD)w->MouseY);
    m68k_write16(ctx, mw + M68K_WIN_MOUSEX, (UWORD)w->MouseX);
    m68k_write16(ctx, mw + M68K_WIN_MINWIDTH, (UWORD)w->MinWidth);
    m68k_write16(ctx, mw + M68K_WIN_MINHEIGHT, (UWORD)w->MinHeight);
    m68k_write16(ctx, mw + M68K_WIN_MAXWIDTH, w->MaxWidth);
    m68k_write16(ctx, mw + M68K_WIN_MAXHEIGHT, w->MaxHeight);
    /* a WFLG_BACKDROP that the program changed for its next depth call
       stays (kaffeine_layers.c) */
    m68k_write32(ctx, mw + M68K_WIN_FLAGS,
                 ctx->layer_ops ? ctx->layer_ops->window_flags(ctx, mw, w->Flags) : w->Flags);
    ctx->mem[mw + M68K_WIN_BORDERLEFT] = (UBYTE)w->BorderLeft;
    ctx->mem[mw + M68K_WIN_BORDERTOP] = (UBYTE)w->BorderTop;
    ctx->mem[mw + M68K_WIN_BORDERRIGHT] = (UBYTE)w->BorderRight;
    ctx->mem[mw + M68K_WIN_BORDERBOTTOM] = (UBYTE)w->BorderBottom;
    m68k_write32(ctx, mw + M68K_WIN_IDCMPFLAGS, w->IDCMPFlags);
    ctx->mem[mw + M68K_WIN_DETAILPEN] = w->DetailPen;
    ctx->mem[mw + M68K_WIN_BLOCKPEN] = w->BlockPen;
    m68k_write16(ctx, mw + M68K_WIN_GZZMOUSEX, (UWORD)w->GZZMouseX);
    m68k_write16(ctx, mw + M68K_WIN_GZZMOUSEY, (UWORD)w->GZZMouseY);
    m68k_write16(ctx, mw + M68K_WIN_GZZWIDTH, (UWORD)w->GZZWidth);
    m68k_write16(ctx, mw + M68K_WIN_GZZHEIGHT, (UWORD)w->GZZHeight);
}

/* A guest address for a native pointer into guest memory, else 0. */
static ULONG guest_address(struct KaffeineContext *ctx, const void *ptr)
{
    const UBYTE *p = (const UBYTE *)ptr;

    return p && p > ctx->mem && p < ctx->mem + ctx->mem_size ? (ULONG)(p - ctx->mem) : 0;
}

/* ── IDCMP ports ─────────────────────────────────────────────────── */

ULONG Kaffeine_UserPortCreate(struct KaffeineContext *ctx, struct MsgPort *port)
{
    ULONG guest = shared_identity(ctx, port), list, task = 0;

    if (guest) return guest; /* preserve ordinary FIFO routing, no USERPORT shadow */
    guest = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_MSGPORT, MEMF_PUBLIC | MEMF_CLEAR);
    list = guest + G_MP_MSGLIST;

    if (!guest)
        return 0;
    if (!Kaffeine_ShadowAdd(ctx, guest, port, KAFFEINE_SHADOW_USERPORT))
    {
        Kaffeine_HeapFree(ctx, guest, M68K_SIZEOF_MSGPORT);
        return 0;
    }
    if (guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    ctx->mem[guest + G_LN_TYPE] = NT_MSGPORT;
    ctx->mem[guest + G_MP_FLAGS] = PA_SIGNAL;
    ctx->mem[guest + G_MP_SIGBIT] = port->mp_SigBit;
    m68k_write32(ctx, guest + G_MP_SIGTASK, task);
    m68k_write32(ctx, list, list + G_LH_TAIL);
    m68k_write32(ctx, list + G_LH_TAILPRED, list);
    return guest;
}

void Kaffeine_UserPortDelete(struct KaffeineContext *ctx, ULONG guest)
{
    if (!Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_USERPORT))
        return;
    Kaffeine_ShadowForget(ctx, guest, KAFFEINE_SHADOW_USERPORT);
    Kaffeine_HeapFree(ctx, guest, M68K_SIZEOF_MSGPORT);
}

/* ── IntuiMessages ───────────────────────────────────────────────── */

/* The program's gadget for a native one: the 68k mirror of a GadTools
   gadget, or the gadget that the program built of a copy, or 0 */
static ULONG message_gadget(struct KaffeineContext *ctx, const void *native)
{
    return ctx->window_gadgets ? ctx->window_gadgets->gadget(ctx, native)
                               : Kaffeine_ShadowGuest(ctx, native, KAFFEINE_SHADOW_GADGET);
}

/* IAddress as the program knows it: its 68k Window, the 68k mirror of a
   GadTools gadget of the program or a gadget that it built itself, or 0
   for any other native object and for what is no object, such as the tag
   list of IDCMP_IDCMPUPDATE */
static ULONG message_address(struct KaffeineContext *ctx, struct IntuiMessage *im, ULONG window)
{
    if (!im->IAddress || im->Class == IDCMP_IDCMPUPDATE)
        return 0;
    if (im->IAddress == (APTR)im->IDCMPWindow)
        return window;
    return message_gadget(ctx, im->IAddress);
}

/* A gadget message whose gadget is neither on its window, nor in a
   requester there, nor a gadget of the bridge any more: the program freed
   it after the message came. GadTools reads the gadget of a GADGETDOWN,
   and compares the one of a GADGETUP with the gadget that it keeps active
   since a GADGETDOWN. */
static BOOL message_stale(struct KaffeineContext *ctx, struct IntuiMessage *im)
{
    struct Window *w = im->IDCMPWindow;
    struct Requester *r;
    struct Gadget *g;

    if (!w || !im->IAddress || (im->Class != IDCMP_GADGETDOWN && im->Class != IDCMP_GADGETUP) ||
        message_gadget(ctx, im->IAddress))
        return FALSE;
    for (g = w->FirstGadget; g; g = g->NextGadget)
        if (g == (struct Gadget *)im->IAddress)
            return FALSE;
    for (r = w->FirstRequest; r; r = r->OlderRequest)
        for (g = r->ReqGadget; g; g = g->NextGadget)
            if (g == (struct Gadget *)im->IAddress)
                return FALSE;
    return TRUE;
}

/* A stale message goes back to Intuition, and the program does not see
   it. GadTools does not see a GADGETDOWN either, and sees a GADGETUP
   without its gadget, so that it forgets the one that it keeps active. */
static void message_drop(struct IntuiMessage *im)
{
    struct IntuiMessage *copy;

    if (im->Class == IDCMP_GADGETUP)
    {
        im->IAddress = NULL;
        if ((copy = GT_FilterIMsg(im)))
            GT_PostFilterIMsg(copy);
    }
}

/* The 68k IntuiMessage of that type of a native one, made at the first
   sight and filled in again each time, after the mirrors of the gadgets
   on its window follow what the input changed. 0 without memory. */
static ULONG message_guest(struct KaffeineContext *ctx, struct IntuiMessage *im, UWORD type)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, im, type);
    ULONG window = Kaffeine_ShadowGuest(ctx, im->IDCMPWindow, KAFFEINE_SHADOW_WINDOW);
    UWORD i;

    /* The CHECKED flags and the NextSelect codes of the menu strip that
       Intuition set for this message, for the program to read. */
    if (ctx->menu_sync && (im->Class == IDCMP_MENUPICK || im->Class == IDCMP_MENUVERIFY ||
                           im->Class == IDCMP_MENUHELP))
        ctx->menu_sync(ctx, im);
    if (ctx->window_gadgets && im->IDCMPWindow)
        ctx->window_gadgets->sync(ctx, im->IDCMPWindow, im->IAddress);
    if (!guest)
    {
        guest = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_INTUIMESSAGE, MEMF_PUBLIC | MEMF_CLEAR);
        if (!guest)
            return 0;
        if (!Kaffeine_ShadowAdd(ctx, guest, im, type))
        {
            Kaffeine_HeapFree(ctx, guest, M68K_SIZEOF_INTUIMESSAGE);
            return 0;
        }
    }
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == guest && ctx->shadow_map[i].type == type)
        {
            ctx->shadow_map[i].idcmp_window = window;
            break;
        }
    ctx->mem[guest + G_LN_TYPE] = im->ExecMessage.mn_Node.ln_Type;
    ctx->mem[guest + G_LN_PRI] = (UBYTE)im->ExecMessage.mn_Node.ln_Pri;
    m68k_write16(ctx, guest + G_MN_LENGTH, M68K_SIZEOF_INTUIMESSAGE);
    m68k_write32(ctx, guest + G_IM_CLASS, im->Class);
    m68k_write16(ctx, guest + G_IM_CODE, im->Code);
    m68k_write16(ctx, guest + G_IM_QUALIFIER, im->Qualifier);
    m68k_write32(ctx, guest + G_IM_IADDRESS, message_address(ctx, im, window));
    m68k_write16(ctx, guest + G_IM_MOUSEX, (UWORD)im->MouseX);
    m68k_write16(ctx, guest + G_IM_MOUSEY, (UWORD)im->MouseY);
    m68k_write32(ctx, guest + G_IM_SECONDS, im->Seconds);
    m68k_write32(ctx, guest + G_IM_MICROS, im->Micros);
    m68k_write32(ctx, guest + G_IM_IDCMPWINDOW, window);
    if (window)
        Kaffeine_WindowSync(ctx, window, im->IDCMPWindow);
    /* the layers of its screen as they are now; not for a VERIFY message,
       whose reply Intuition may be waiting for (kaffeine_layers.c) */
    if (window && ctx->layer_ops &&
        !(im->Class & (IDCMP_SIZEVERIFY | IDCMP_REQVERIFY | IDCMP_MENUVERIFY)))
        ctx->layer_ops->window_synced(ctx, window, im->IDCMPWindow);
    return guest;
}

static void message_forget(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{
    Kaffeine_ShadowForget(ctx, guest, type);
    Kaffeine_HeapFree(ctx, guest, M68K_SIZEOF_INTUIMESSAGE);
}

/* The native message of a 68k IntuiMessage of that type, with the Code that
   the program may set for IDCMP_MENUVERIFY. Never for one that WaitPort()
   showed. */
static struct IntuiMessage *message_native(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{
    struct IntuiMessage *im = Kaffeine_ShadowNative(ctx, guest, type);

    if (im && im->Class == IDCMP_MENUVERIFY)
        im->Code = m68k_read16(ctx, guest + G_IM_CODE);
    return im;
}

/* Ownership is native metadata: the guest may edit its IDCMPWindow field,
   and a WAIT snapshot's native message may already have been taken/freed. */
static ULONG message_of(struct KaffeineContext *ctx, ULONG mw, UWORD type)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].type == type &&
            ctx->shadow_map[i].idcmp_window == mw)
            return ctx->shadow_map[i].m68k_addr;
    return 0;
}

/* Free what WaitPort() showed of the messages in the port of the 68k window
   mw, whose native messages may be gone already. */
static void message_unshow(struct KaffeineContext *ctx, ULONG mw)
{
    ULONG guest;

    while ((guest = message_of(ctx, mw, KAFFEINE_SHADOW_WAITMESSAGE)))
        message_forget(ctx, guest, KAFFEINE_SHADOW_WAITMESSAGE);
}

/* A native message just taken from the IDCMP port of a 68k window: what
   WaitPort() showed of it becomes a 68k message of the program, and what it
   showed of a message that has left the port before it is freed. */
static void message_take(struct KaffeineContext *ctx, struct IntuiMessage *im)
{
    ULONG mw = Kaffeine_ShadowGuest(ctx, im->IDCMPWindow, KAFFEINE_SHADOW_WINDOW);
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].type == KAFFEINE_SHADOW_WAITMESSAGE &&
            ctx->shadow_map[i].native_ptr == im)
        {
            ctx->shadow_map[i].type = KAFFEINE_SHADOW_INTUIMESSAGE;
            break;
        }
    if (mw)
        message_unshow(ctx, mw);
}

/* Reply a native message of Intuition, and free its 68k message if the
   program has one. */
static void message_reply(struct KaffeineContext *ctx, struct IntuiMessage *im)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, im, KAFFEINE_SHADOW_INTUIMESSAGE);

    if (guest)
        message_forget(ctx, guest, KAFFEINE_SHADOW_INTUIMESSAGE);
    ReplyMsg(&im->ExecMessage);
}

/* Give a copy back to GadTools, and reply the original that it gives. */
static void message_unfilter(struct KaffeineContext *ctx, struct IntuiMessage *copy)
{
    struct IntuiMessage *im = GT_PostFilterIMsg(copy);

    if (im)
        message_reply(ctx, im);
}

/* Give Intuition and GadTools back every message of a window that the
   program has not replied, and free the 68k messages of the window, for
   CloseWindow(): the copies of GadTools with their originals, and the
   messages that the program took. What is still in the port Intuition's
   CloseWindow() replies itself. */
static void window_reply_messages(struct KaffeineContext *ctx, ULONG mw)
{
    struct IntuiMessage *im;
    ULONG guest;

    message_unshow(ctx, mw);
    while ((guest = message_of(ctx, mw, KAFFEINE_SHADOW_GTMESSAGE)))
    {
        im = message_native(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE);
        message_forget(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE);
        message_unfilter(ctx, im);
    }
    while ((guest = message_of(ctx, mw, KAFFEINE_SHADOW_INTUIMESSAGE)))
    {
        im = message_native(ctx, guest, KAFFEINE_SHADOW_INTUIMESSAGE);
        message_forget(ctx, guest, KAFFEINE_SHADOW_INTUIMESSAGE);
        ReplyMsg(&im->ExecMessage);
    }
}

/* Ordinary guest ports stay ordinary queues. Intuition alone writes these
   separate native inboxes; no native Message or port is ever a guest node.
   Like AppPortFill, the ordinary guest FIFO has priority over the inbox. */
#define MAX_SHARED_IDCMP_PORTS 16
struct KaffeineIDCMPPort
{
    struct KaffeineIDCMPPort *next;
    struct MsgPort inbox;
    ULONG guest, owner;
    unsigned users;
    UBYTE bit;
    BOOL retiring;
};

static struct KaffeineIDCMPPort *shared_guest(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineIDCMPPort *p;
    for (p = ctx->idcmp_ports; p; p = p->next)
        if (p->guest == guest) return p;
    return NULL;
}

static struct KaffeineIDCMPPort *shared_native(struct KaffeineContext *ctx, struct MsgPort *port)
{
    struct KaffeineIDCMPPort *p;
    for (p = ctx->idcmp_ports; p; p = p->next)
        if (&p->inbox == port) return p;
    return NULL;
}

static ULONG shared_identity(struct KaffeineContext *ctx, struct MsgPort *port)
{
    struct KaffeineIDCMPPort *p = shared_native(ctx, port);
    return p ? p->guest : 0;
}

static BOOL shared_queue(struct KaffeineContext *ctx, ULONG guest, ULONG *owner,
                          UBYTE *bit, struct KaffeineQueue *q)
{
    unsigned i;
    if (ctx->broker_call || !ctx->exec_task || FindTask(NULL) != ctx->exec_task ||
        !Kaffeine_HeapGuestRange(ctx, guest, M68K_SIZEOF_MSGPORT) ||
        !Kaffeine_QueueRead(ctx, guest, q) || ctx->mem[guest + G_MP_FLAGS] != PA_SIGNAL)
        return FALSE;
    *owner = m68k_read32(ctx, guest + G_MP_SIGTASK);
    *bit = ctx->mem[guest + G_MP_SIGBIT];
    if (*bit > 31 || !(ctx->guest_signals & (1UL << *bit)) ||
        (Kaffeine_AsyncSignalMask(ctx) & (1UL << *bit))) return FALSE;
    if (ctx->sched)
    {
        if (!ctx->task_ops || !ctx->task_ops->known_task || !ctx->task_ops->owns_signal ||
            !ctx->task_ops->known_task(ctx, *owner) ||
            !ctx->task_ops->owns_signal(ctx, *owner, 1UL << *bit)) return FALSE;
    }
    else if (!*owner || *owner != ctx->guest_exec_task || !(ctx->guest_signals & (1UL << *bit)))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_IO_PORTS; i++)
        if (ctx->io_ports[i].guest == guest && ctx->io_ports[i].sigbit != (BYTE)*bit)
            return FALSE;
    return Kaffeine_AppPortMatches(ctx, guest, ctx->exec_task, *bit);
}

static void shared_destroy(struct KaffeineContext *ctx, struct KaffeineIDCMPPort *p)
{
    struct KaffeineIDCMPPort **at = &ctx->idcmp_ports;
    struct Message *msg;
    while (*at != p) at = &(*at)->next;
    *at = p->next;
    p->inbox.mp_Flags = PA_IGNORE;
    p->inbox.mp_SigTask = NULL;
    while ((msg = GetMsg(&p->inbox))) message_reply(ctx, (struct IntuiMessage *)msg);
    /* AllocMem, not CreateMsgPort: the guest still owns its signal bit. */
    FreeMem(p, sizeof(*p));
}

/* Remove only this window's native nodes, leaving every other window's
   FIFO and signal intact. No native producer can see the detached window. */
static void shared_strip(struct KaffeineContext *ctx, struct KaffeineIDCMPPort *p,
                          struct Window *w)
{
    struct Node *node;
    struct IntuiMessage *im;
    for (;;)
    {
        im = NULL;
        Forbid();
        for (node = p->inbox.mp_MsgList.lh_Head; node->ln_Succ; node = node->ln_Succ)
            if (((struct IntuiMessage *)node)->IDCMPWindow == w)
            {
                node->ln_Pred->ln_Succ = node->ln_Succ;
                node->ln_Succ->ln_Pred = node->ln_Pred;
                im = (struct IntuiMessage *)node;
                break;
            }
        Permit();
        if (!im) break;
        message_reply(ctx, im);
    }
}

static void shared_open_failed(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineIDCMPPort *p = shared_native(ctx, w->UserPort);
    if (!p) return;
    Forbid();
    w->UserPort = NULL;
    ModifyIDCMP(w, 0);
    w->IDCMPFlags = 0;
    Permit();
    shared_strip(ctx, p, w);
}

static void shared_unbind(struct KaffeineContext *ctx, ULONG mw, struct Window *w,
                           struct KaffeineIDCMPPort *p)
{
    window_reply_messages(ctx, mw);
    shared_strip(ctx, p, w);
    if (p->users) p->users--;
    if (!p->users && !p->retiring) shared_destroy(ctx, p);
}

static void shared_detach(struct KaffeineContext *ctx, ULONG mw, struct Window *w)
{
    struct KaffeineIDCMPPort *p = shared_native(ctx, w->UserPort);
    if (!p) return;
    Forbid();
    w->UserPort = NULL; /* ModifyIDCMP/CloseWindow must never delete a borrowed port. */
    if (!ModifyIDCMP(w, 0)) refuse(ctx, "shared IDCMP cleanup could not disable its window");
    w->IDCMPFlags = 0;
    Permit();
    m68k_write32(ctx, mw + M68K_WIN_USERPORT, 0);
    m68k_write32(ctx, mw + M68K_WIN_IDCMPFLAGS, 0);
    shared_unbind(ctx, mw, w, p);
}

static void shared_port_gone(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineIDCMPPort *p = shared_guest(ctx, guest);
    UWORD i;
    if (!p) return;
    p->retiring = TRUE;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].type == KAFFEINE_SHADOW_WINDOW)
        {
            struct Window *w = ctx->shadow_map[i].native_ptr;
            if (w->UserPort == &p->inbox)
            {
                shared_detach(ctx, ctx->shadow_map[i].m68k_addr, w);
                i = (UWORD)-1; /* message retirement can move shadow entries */
            }
        }
    shared_destroy(ctx, p);
}

static void shared_signal_gone(struct KaffeineContext *ctx, BYTE bit)
{
    struct KaffeineIDCMPPort *p;
    for (;;)
    {
        for (p = ctx->idcmp_ports; p && p->bit != (UBYTE)bit; p = p->next) ;
        if (!p) break;
        shared_port_gone(ctx, p->guest);
    }
}

static void shared_task_gone(struct KaffeineContext *ctx, ULONG owner)
{
    struct KaffeineIDCMPPort *p;
    for (;;)
    {
        for (p = ctx->idcmp_ports; p && p->owner != owner; p = p->next) ;
        if (!p) break;
        shared_port_gone(ctx, p->guest);
    }
}

static BOOL shared_release(struct KaffeineContext *ctx, ULONG addr, ULONG size, BOOL commit)
{
    struct KaffeineIDCMPPort *p = ctx->idcmp_ports;
    struct KaffeineQueue q;
    while (p)
    {
        struct KaffeineIDCMPPort *next = p->next;
        if (Kaffeine_Overlap(addr, size, p->guest, M68K_SIZEOF_MSGPORT))
        {
            if (Kaffeine_LayerInfoLocked(ctx))
            {
                refuse(ctx, "shared IDCMP retirement inside a LayerInfo lock");
                return FALSE;
            }
            /* Query first, without changing a multi-block release. Queued
               ordinary IPC must be consumed before its port can be freed. */
            if (ctx->broker_call || FindTask(NULL) != ctx->exec_task ||
                !Kaffeine_QueueRead(ctx, p->guest, &q) || q.count) return FALSE;
            if (commit) shared_port_gone(ctx, p->guest);
        }
        p = next;
    }
    return TRUE;
}

static void shared_cleanup(struct KaffeineContext *ctx)
{
    while (ctx->idcmp_ports) shared_port_gone(ctx, ctx->idcmp_ports->guest);
}

static struct KaffeineIDCMPPort *shared_create(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineIDCMPPort *p;
    struct KaffeineQueue q;
    ULONG owner;
    UBYTE bit;
    unsigned n = 0;
    if (!shared_queue(ctx, guest, &owner, &bit, &q))
    {
        refuse(ctx, "ModifyIDCMP(): not a live ordinary port with its owner's signal");
        return NULL;
    }
    if ((p = shared_guest(ctx, guest)))
    {
        if (p->owner == owner && p->bit == bit && !p->retiring) return p;
        shared_port_gone(ctx, guest);
        refuse(ctx, "ModifyIDCMP(): shared port identity changed");
        return NULL;
    }
    for (p = ctx->idcmp_ports; p; p = p->next) n++;
    if (n == MAX_SHARED_IDCMP_PORTS || !(p = AllocMem(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    p->guest = guest; p->owner = owner; p->bit = bit;
    p->inbox.mp_Node.ln_Type = NT_MSGPORT;
    p->inbox.mp_Flags = PA_SIGNAL;
    p->inbox.mp_SigTask = ctx->exec_task;
    p->inbox.mp_SigBit = bit;
    p->inbox.mp_MsgList.lh_Head = (struct Node *)&p->inbox.mp_MsgList.lh_Tail;
    p->inbox.mp_MsgList.lh_TailPred = (struct Node *)&p->inbox.mp_MsgList.lh_Head;
    p->next = ctx->idcmp_ports;
    ctx->idcmp_ports = p;
    ctx->idcmp_port_gone = shared_port_gone;
    ctx->idcmp_signal_gone = shared_signal_gone;
    ctx->idcmp_task_gone = shared_task_gone;
    ctx->idcmp_release = shared_release;
    ctx->idcmp_cleanup = shared_cleanup;
    return p;
}

/* 0: ordinary/default port, 1: native IDCMP is next, 2: guest FIFO result,
   -1: fail closed. App carriers share the same signal and are polled before
   waiting, so an AppMessage cannot be stranded behind an empty IDCMP inbox. */
static int shared_ready(struct KaffeineContext *ctx, ULONG guest, BOOL wait,
                         struct MsgPort **native, IPTR *result)
{
    struct KaffeineIDCMPPort *p = shared_guest(ctx, guest);
    struct KaffeineQueue q;
    ULONG owner;
    UBYTE bit;
    if (!p) return 0;
    if (!shared_queue(ctx, guest, &owner, &bit, &q) || owner != p->owner || bit != p->bit)
    {
        shared_port_gone(ctx, guest);
        *result = refuse(ctx, "shared IDCMP port changed or its owner retired");
        return -1;
    }
    if (!Kaffeine_AsyncPoll(ctx)) { *result = 20; return -1; }
    if (!q.count) Kaffeine_AppPortFill(ctx, guest, FALSE);
    if (!Kaffeine_QueueRead(ctx, guest, &q))
    {
        *result = refuse(ctx, "shared IDCMP guest FIFO is malformed");
        return -1;
    }
    if (q.count)
    {
        *result = Kaffeine_GuestGetMsg(ctx, guest, wait);
        return 2;
    }
    *native = &p->inbox;
    return 1;
}

/* The first message of a 68k port, which nothing but the program itself
   can put there so far. */
static ULONG guest_first(struct KaffeineContext *ctx, ULONG port, ULONG *next)
{
    ULONG head;

    if (!guest_range(ctx, port, M68K_SIZEOF_MSGPORT))
        return 0;
    head = m68k_read32(ctx, port + G_MP_MSGLIST);
    if (!guest_range(ctx, head, 14))
        return 0;
    *next = m68k_read32(ctx, head + G_LN_SUCC);
    if (!*next || !guest_range(ctx, *next, 8))
        return 0;
    return head;
}

/* exec -384: WaitPort(A0=port) -> D0=message. For a window's IDCMP port the
   native wait, then the 68k message that GetMsg() will give for the first
   message. A port that App objects send to waits for the next AppMessage
   (kaffeine_workbench.c). Any other 68k port with no message stops the
   program: no message can reach it yet. With AddTask() children, a guest
   task that waits for an empty IDCMP port lets the others run, and this
   runs again for it once the port's signal arrives (kaffeine_sched.c). */
IPTR Kaffeine_WaitPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), first, next, mw;
    struct MsgPort *port = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_USERPORT);
    struct IntuiMessage *im;
    IPTR ready;
    int shared;

    for (;;)
    {
        shared = shared_ready(ctx, guest, TRUE, &port, &ready);
        if (shared == 2 || shared < 0) return ready;
        if (!shared || port->mp_MsgList.lh_TailPred != (struct Node *)&port->mp_MsgList)
            break;
        if (ctx->sched &&
            ctx->task_ops->block_port(ctx, cpu, port, Kaffeine_WaitPort, &ready))
            return ready;
        /* Both private IDCMP and Workbench inboxes use this validated bit.
           WaitPort(native) alone would miss the other inbox's arrival. */
        Wait((1UL << port->mp_SigBit) | Kaffeine_AsyncSignalMask(ctx));
        if (!Kaffeine_AsyncPoll(ctx)) return 20;
    }

    if (port)
    {
        IPTR blocked;

        if (ctx->sched &&
            ctx->task_ops->block_port(ctx, cpu, port, Kaffeine_WaitPort, &blocked))
            return blocked;
        im = (struct IntuiMessage *)Kaffeine_AsyncWaitPort(ctx, port);
        if (!im)
            return ctx->callback_failed ? 20 : 0;
        mw = Kaffeine_ShadowGuest(ctx, im->IDCMPWindow, KAFFEINE_SHADOW_WINDOW);
        /* what it showed before of a message that has left the port */
        if (mw && !Kaffeine_ShadowGuest(ctx, im, KAFFEINE_SHADOW_WAITMESSAGE))
            message_unshow(ctx, mw);
        first = message_guest(ctx, im, KAFFEINE_SHADOW_WAITMESSAGE);
        return !first && shared ? refuse(ctx, "WaitPort(): no shared IDCMP mirror memory") : first;
    }
    /* A scheduled App port also yields the guest CPU. Retry the whole
       typed wait so a native App carrier is copied before checking the
       ordinary guest FIFO, rather than blocking the driver in WaitPort. */
    if (ctx->sched && Kaffeine_AppPortFill(ctx, guest, FALSE))
    {
        struct KaffeineQueue q;
        IPTR blocked;
        if (ctx->callback_failed) return 20;
        if (!Kaffeine_QueueRead(ctx, guest, &q))
            return refuse(ctx, "WaitPort(): malformed App guest FIFO");
        if (q.count) return Kaffeine_GuestGetMsg(ctx, guest, TRUE);
        if (ctx->task_ops->block_guest_port &&
            ctx->task_ops->block_guest_port(ctx, guest, Kaffeine_WaitPort, &blocked))
            return blocked;
        return refuse(ctx, "WaitPort(): App port cannot signal this guest task");
    }
    if (!Kaffeine_AppPortFill(ctx, guest, TRUE))
        return Kaffeine_GuestGetMsg(ctx, guest, TRUE);
    if (ctx->callback_failed) return 20;
    first = guest_first(ctx, guest, &next);
    if (first)
        return first;
    bug("[kaffeine] WaitPort(%08lx): not an IDCMP port, and empty\n", (unsigned long)guest);
    return refuse(ctx, "WaitPort(): no message can reach a 68k port yet");
}

/* exec -372: GetMsg(A0=port) -> D0=message. The 68k message of a window's
   IDCMP port, or the first node of a 68k port, where the next AppMessage
   of the port of App objects comes when there is none. */
IPTR Kaffeine_GetMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), first, next;
    struct MsgPort *port = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_USERPORT);
    struct IntuiMessage *im;
    ULONG msg;
    IPTR ready;
    int shared = shared_ready(ctx, guest, FALSE, &port, &ready);

    if (shared == 2 || shared < 0) return ready;

    if (port)
    {
        im = (struct IntuiMessage *)GetMsg(port);
        if (!im)
            return 0;
        message_take(ctx, im);
        msg = message_guest(ctx, im, KAFFEINE_SHADOW_INTUIMESSAGE);
        if (!msg)
        {
            /* Without guest memory the message is lost to the program, but
               Intuition gets it back. */
            bug("[kaffeine] GetMsg(): no guest memory for an IntuiMessage\n");
            ReplyMsg(&im->ExecMessage);
            if (shared) return refuse(ctx, "GetMsg(): no shared IDCMP mirror memory");
        }
        return msg;
    }
    if (!Kaffeine_AppPortFill(ctx, guest, FALSE))
        return Kaffeine_GuestGetMsg(ctx, guest, FALSE);
    first = guest_first(ctx, guest, &next);
    if (!first)
        return 0;
    m68k_write32(ctx, guest + G_MP_MSGLIST, next);
    m68k_write32(ctx, next + G_LN_PRED, guest + G_MP_MSGLIST);
    return first;
}

/* exec -378: ReplyMsg(A1=message). A 68k IntuiMessage from GetMsg() goes
   back to Intuition and is freed, as a 68k AppMessage goes back to
   Workbench. Ordinary guest messages use the bounded guest queue adapter. */
IPTR Kaffeine_ReplyMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct IntuiMessage *im = message_native(ctx, guest, KAFFEINE_SHADOW_INTUIMESSAGE);

    if (!im && Kaffeine_AppReplyMsg(ctx, guest))
        return 0;

    if (!im)
    {
        /* Other shadow protocols retain their ownership rules. */
        UWORD i;
        for (i = 0; i < ctx->num_shadows; ++i)
            if (ctx->shadow_map[i].m68k_addr == guest) return 0;
        return Kaffeine_GuestReplyMsg(ctx, guest);
    }
    message_reply(ctx, im);
    return 0;
}

/* ── Opening, closing and IDCMP flags ────────────────────────────── */

ULONG Kaffeine_WindowCreate(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineIDCMPPort *peer = shared_native(ctx, w->UserPort);
    ULONG mw = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_WINDOW, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG rp = 0, port = 0, screen_guest;
    struct Screen *bitmap_screen = NULL;

    if (!mw)
        return 0;
    if (!Kaffeine_ShadowAdd(ctx, mw, w, KAFFEINE_SHADOW_WINDOW))
    {
        Kaffeine_HeapFree(ctx, mw, M68K_SIZEOF_WINDOW);
        return 0;
    }
    Kaffeine_WindowSync(ctx, mw, w);
    /* A window without its 68k RastPort or IDCMP port would hand the
       program NULL where Intuition has an object. */
    if ((w->RPort && !(rp = Kaffeine_RastPortCreate(ctx, w->RPort))) ||
        (w->UserPort && !(port = Kaffeine_UserPortCreate(ctx, w->UserPort))))
    {
        Kaffeine_RastPortDelete(ctx, rp);
        Kaffeine_ShadowForget(ctx, mw, KAFFEINE_SHADOW_WINDOW);
        Kaffeine_HeapFree(ctx, mw, M68K_SIZEOF_WINDOW);
        return 0;
    }
    m68k_write32(ctx, mw + M68K_WIN_RPORT, rp);
    m68k_write32(ctx, mw + M68K_WIN_USERPORT, port);
    /* a screen that the program opened or locked; 0 for a screen that it
       has no 68k Screen of */
    screen_guest = Kaffeine_ScreenGuest(ctx, w->WScreen);
    m68k_write32(ctx, mw + M68K_WIN_WSCREEN, screen_guest);
    /* Share the existing typed screen BitMap handle, only when the live
       owned screen table and the actual native RastPort agree. This
       exposes its metadata without publishing any native plane pointer.
       RastPortOut() preserves this field across later drawing calls. */
    if (rp && screen_guest &&
        Kaffeine_ScreenBitMapOf(ctx, screen_guest + M68K_SCR_BITMAP, &bitmap_screen) ==
            KAFFEINE_SCREEN_BITMAP_OWNED && bitmap_screen == w->WScreen &&
        w->RPort->BitMap && w->RPort->BitMap == bitmap_screen->RastPort.BitMap)
        m68k_write32(ctx, rp + M68K_RP_BITMAP, screen_guest + M68K_SCR_BITMAP);
    m68k_write32(ctx, mw + M68K_WIN_TITLE, guest_address(ctx, w->Title));
    /* WLayer and the Layer of the RastPort: the 68k Layer of the window's
       layer, without which the window does not come either */
    if (ctx->layer_ops && !ctx->layer_ops->window_opened(ctx, mw, w))
    {
        Kaffeine_UserPortDelete(ctx, port);
        Kaffeine_RastPortDelete(ctx, rp);
        Kaffeine_ShadowForget(ctx, mw, KAFFEINE_SHADOW_WINDOW);
        Kaffeine_HeapFree(ctx, mw, M68K_SIZEOF_WINDOW);
        return 0;
    }
    if (peer) peer->users++;
    return mw;
}

static void window_delete(struct KaffeineContext *ctx, ULONG mw, BOOL layers_done)
{
    struct Window *w = Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW);

    if (!w)
        return;
    /* the program's layer locks and its 68k Layer go while the native
       layer is still there */
    if (!layers_done && ctx->layer_ops)
        ctx->layer_ops->window_closing(ctx, mw, w);
    shared_detach(ctx, mw, w);
    window_reply_messages(ctx, mw);
    Kaffeine_RastPortDelete(ctx, Kaffeine_ShadowGuest(ctx, w->RPort, KAFFEINE_SHADOW_RASTPORT));
    Kaffeine_UserPortDelete(ctx, Kaffeine_ShadowGuest(ctx, w->UserPort, KAFFEINE_SHADOW_USERPORT));
    Kaffeine_ShadowForget(ctx, mw, KAFFEINE_SHADOW_REQUESTER_WINDOW);
    Kaffeine_ShadowForget(ctx, mw, KAFFEINE_SHADOW_WINDOW);
    Kaffeine_HeapFree(ctx, mw, M68K_SIZEOF_WINDOW);
    /* the 68k Screen of a public screen that the program has unlocked may
       go with its last window */
    if (ctx->screen_release)
        ctx->screen_release(ctx, w->WScreen);
}

void Kaffeine_WindowDelete(struct KaffeineContext *ctx, ULONG mw)
{
    window_delete(ctx, mw, FALSE);
}

void Kaffeine_WindowPrepareClose(struct KaffeineContext *ctx, ULONG mw)
{
    struct Window *w = Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW);

    if (w && ctx->layer_ops)
        ctx->layer_ops->window_closing(ctx, mw, w);
}

void Kaffeine_WindowDeletePrepared(struct KaffeineContext *ctx, ULONG mw)
{
    window_delete(ctx, mw, TRUE);
}

/* intuition -72: CloseWindow(A0=window). The messages that the program has
   not replied go back, and the 68k Window, its RastPort, its IDCMP port and
   the 68k messages go, while the native window still names them. */
static void close_window(struct KaffeineContext *ctx, ULONG mw)
{
    struct Window *w = Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW);
    BOOL requester = Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_REQUESTER_WINDOW) != NULL;

    if (!w)
        return;
    /* Menu and gadget removal may wait on Intuition. Release the guest's
       native layer locks before any of those native teardown calls. */
    Kaffeine_WindowPrepareClose(ctx, mw);
    if (ctx->layer_ops && ctx->layer_ops->has_locks && ctx->layer_ops->has_locks(ctx))
    {
        bug("[kaffeine] CloseWindow(): another window layer is locked, stopping (20)\n");
        ctx->running = FALSE;
        ctx->exit_code = 20;
        return;
    }
    /* the program's requesters end while the native window is still open */
    if (ctx->requests_closing)
        ctx->requests_closing(ctx, mw, w);
    if (ctx->menu_closing)
        ctx->menu_closing(ctx, w);
    if (ctx->window_gadgets)
        ctx->window_gadgets->closing(ctx, w);
    Kaffeine_WindowDeletePrepared(ctx, mw);
    if (requester)
        FreeSysRequest(w);
    else
        CloseWindow(w);
}

IPTR Kaffeine_CloseWindow(struct KaffeineContext *ctx, void *cpu)
{
    close_window(ctx, THUNK_A(0));
    return !ctx->running && ctx->exit_code == 20 ? 20 : 0;
}

void Kaffeine_WindowsCleanup(struct KaffeineContext *ctx)
{
    UWORD i = 0;

    if (ctx->layer_ops && ctx->layer_ops->release_locks)
        ctx->layer_ops->release_locks(ctx);

    /* Removing a shadow moves another entry into its slot. Restart after
       each close because messages, ports and RastPorts are removed too. */
    while (i < ctx->num_shadows)
    {
        if (ctx->shadow_map[i].type == KAFFEINE_SHADOW_WINDOW)
        {
            close_window(ctx, ctx->shadow_map[i].m68k_addr);
            i = 0;
        }
        else
            i++;
    }
    shared_cleanup(ctx);
}

/* Stage a private inbox without handing Intuition a guest pointer. Failure
   restores the previous native binding and flags before any peer is freed. */
static IPTR shared_modify(struct KaffeineContext *ctx, ULONG mw, struct Window *w,
                           struct KaffeineIDCMPPort *old_peer, ULONG user, ULONG flags)
{
    struct MsgPort *old = w->UserPort, *created;
    struct KaffeineIDCMPPort *peer = NULL;
    ULONG old_flags = w->IDCMPFlags, port = 0;
    ULONG old_guest = old_peer ? old_peer->guest : Kaffeine_ShadowGuest(ctx, old, KAFFEINE_SHADOW_USERPORT);
    struct Message *msg;
    BOOL okay;

    if (flags && user && !(peer = shared_create(ctx, user)))
        return ctx->running ? FALSE : (IPTR)ctx->exit_code;
    if (peer && peer == old_peer)
    {
        okay = ModifyIDCMP(w, flags);
        m68k_write32(ctx, mw + M68K_WIN_USERPORT, peer->guest);
        m68k_write32(ctx, mw + M68K_WIN_IDCMPFLAGS, w->IDCMPFlags);
        return okay ? TRUE : FALSE;
    }
    Forbid();
    w->UserPort = peer ? &peer->inbox : NULL;
    if (!peer && flags) w->IDCMPFlags = 0; /* request an ordinary owned port */
    okay = ModifyIDCMP(w, flags);
    created = w->UserPort;
    if (!okay)
    {
        w->UserPort = old;
        w->IDCMPFlags = old_flags;
    }
    Permit();
    if (okay && !peer && created)
    {
        port = Kaffeine_UserPortCreate(ctx, created);
        if (!port)
        {
            while ((msg = GetMsg(created))) message_reply(ctx, (struct IntuiMessage *)msg);
            ModifyIDCMP(w, 0);
            Forbid();
            w->UserPort = old;
            w->IDCMPFlags = old_flags;
            Permit();
            okay = FALSE;
        }
    }
    if (!okay)
    {
        if (peer && !peer->users) shared_destroy(ctx, peer);
        m68k_write32(ctx, mw + M68K_WIN_USERPORT, old_guest);
        m68k_write32(ctx, mw + M68K_WIN_IDCMPFLAGS, old_flags);
        return FALSE;
    }
    if (peer) { peer->users++; port = peer->guest; }
    if (old_peer) shared_unbind(ctx, mw, w, old_peer);
    else if (old)
    {
        window_reply_messages(ctx, mw);
        while ((msg = GetMsg(old))) message_reply(ctx, (struct IntuiMessage *)msg);
        Kaffeine_UserPortDelete(ctx, old_guest);
        DeleteMsgPort(old); /* only the former window-owned native port */
    }
    m68k_write32(ctx, mw + M68K_WIN_USERPORT, port);
    m68k_write32(ctx, mw + M68K_WIN_IDCMPFLAGS, w->IDCMPFlags);
    return TRUE;
}

/* intuition -150: ModifyIDCMP(A0=window, D0=flags) -> D0=success. A window
   that gets flags gets its IDCMP port as a 68k MsgPort, and loses it with
   them. An ordinary guest UserPort is borrowed through a private peer. */
IPTR Kaffeine_ModifyIDCMP(struct KaffeineContext *ctx, void *cpu)
{
    ULONG window = THUNK_A(0), flags = THUNK_D(0), user, old_flags;
    struct Window *w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW);
    struct MsgPort *old;
    struct KaffeineIDCMPPort *peer;
    BOOL ok;

    if (!w)
        return refuse(ctx, "ModifyIDCMP(): not a window of the program");
    user = m68k_read32(ctx, window + M68K_WIN_USERPORT);
    old = w->UserPort;
    peer = shared_native(ctx, old);
    if (peer || (flags && user && user != Kaffeine_ShadowGuest(ctx, old, KAFFEINE_SHADOW_USERPORT)))
        return shared_modify(ctx, window, w, peer, user, flags);
    if (!flags) user = Kaffeine_ShadowGuest(ctx, old, KAFFEINE_SHADOW_USERPORT);
    old_flags = w->IDCMPFlags;
    ok = ModifyIDCMP(w, flags);
    if (w->UserPort != old)
    {
        ULONG port = w->UserPort ? Kaffeine_UserPortCreate(ctx, w->UserPort) : 0;

        if (w->UserPort && !port)
        {
            /* No 68k port for the new one: undo, and fail as Intuition does
               when it cannot make the port. */
            bug("[kaffeine] ModifyIDCMP(): no guest memory for the IDCMP port\n");
            ModifyIDCMP(w, old_flags);
            ok = FALSE;
        }
        else
        {
            /* Intuition's ModifyIDCMP() deletes the port with whatever is
               in it. */
            message_unshow(ctx, window);
            Kaffeine_UserPortDelete(ctx, user);
            user = port;
        }
    }
    m68k_write32(ctx, window + M68K_WIN_USERPORT, user);
    m68k_write32(ctx, window + M68K_WIN_IDCMPFLAGS, w->IDCMPFlags);
    return ok ? TRUE : FALSE;
}

/* ── GadTools ────────────────────────────────────────────────────── */

/* gadtools -72: GT_GetIMsg(A0=iport) -> D0, as AROS's: each message from
   GetMsg() through GT_FilterIMsg(), which gives GadTools' copy for the
   program or NULL for a message of GadTools only, which goes back. */
IPTR Kaffeine_GT_GetIMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    struct MsgPort *port = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_USERPORT);
    struct IntuiMessage *im, *copy;
    ULONG msg;
    IPTR ready;
    int shared = shared_ready(ctx, guest, FALSE, &port, &ready);

    if (shared == 2 || shared < 0) return ready;

    if (!port)
        return 0;
    while ((im = (struct IntuiMessage *)GetMsg(port)))
    {
        message_take(ctx, im);
        if (message_stale(ctx, im))
        {
            message_drop(im);
            message_reply(ctx, im);
            continue;
        }
        if (ctx->window_gadgets)
            ctx->window_gadgets->filtering(ctx, im);
        copy = GT_FilterIMsg(im);
        if (!copy)
        {
            /* a message for GadTools only, which may have changed gadgets */
            if (ctx->window_gadgets && im->IDCMPWindow)
                ctx->window_gadgets->sync(ctx, im->IDCMPWindow, im->IAddress);
            message_reply(ctx, im);
            continue;
        }
        msg = message_guest(ctx, copy, KAFFEINE_SHADOW_GTMESSAGE);
        if (msg)
            return msg;
        /* Without guest memory the message is lost to the program, but
           Intuition gets it back. */
        bug("[kaffeine] GT_GetIMsg(): no guest memory for an IntuiMessage\n");
        message_unfilter(ctx, copy);
        if (shared) return refuse(ctx, "GT_GetIMsg(): no shared IDCMP mirror memory");
    }
    return 0;
}

/* gadtools -78: GT_ReplyIMsg(A1=imsg), as AROS's: GT_PostFilterIMsg()
   frees GadTools' copy and gives the original, which goes back. */
IPTR Kaffeine_GT_ReplyIMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct IntuiMessage *copy = message_native(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE);

    if (!copy)
    {
        UWORD i;
        if (Kaffeine_AppReplyMsg(ctx, guest)) return 0;
        /* GT_GetIMsg also returns ordinary guest IPC on a shared port.
           Its reply stays the bounded guest protocol, never GadTools. */
        for (i = 0; i < ctx->num_shadows; i++)
            if (ctx->shadow_map[i].m68k_addr == guest) break;
        if (i == ctx->num_shadows && guest)
            return Kaffeine_GuestReplyMsg(ctx, guest);
        if (guest)
            bug("[kaffeine] GT_ReplyIMsg(%08lx): not a message from GT_GetIMsg(), ignored\n",
                (unsigned long)guest);
        return 0;
    }
    message_forget(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE);
    message_unfilter(ctx, copy);
    return 0;
}

/* gadtools -102: GT_FilterIMsg(A1=imsg) -> D0: the copy of a message from
   GetMsg() for the program, or 0 when it was for GadTools only. */
IPTR Kaffeine_GT_FilterIMsg(struct KaffeineContext *ctx, void *cpu)
{
    struct IntuiMessage *im = message_native(ctx, THUNK_A(1), KAFFEINE_SHADOW_INTUIMESSAGE);
    struct IntuiMessage *copy;
    ULONG msg;

    if (!im)
        return refuse(ctx, "GT_FilterIMsg(): not an IntuiMessage from GetMsg()");
    /* the program replies the original, as one for GadTools only */
    if (message_stale(ctx, im))
    {
        message_drop(im);
        return 0;
    }
    if (ctx->window_gadgets)
        ctx->window_gadgets->filtering(ctx, im);
    copy = GT_FilterIMsg(im);
    if (!copy)
    {
        /* a message for GadTools only, which may have changed gadgets */
        if (ctx->window_gadgets && im->IDCMPWindow)
            ctx->window_gadgets->sync(ctx, im->IDCMPWindow, im->IAddress);
        return 0;
    }
    msg = message_guest(ctx, copy, KAFFEINE_SHADOW_GTMESSAGE);
    if (!msg)
    {
        /* The program replies the original then, as one for GadTools only. */
        bug("[kaffeine] GT_FilterIMsg(): no guest memory for an IntuiMessage\n");
        GT_PostFilterIMsg(copy);
    }
    return msg;
}

/* gadtools -108: GT_PostFilterIMsg(A1=modimsg) -> D0: the original message,
   which the program then replies. GadTools frees its copy. */
IPTR Kaffeine_GT_PostFilterIMsg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1), msg;
    struct IntuiMessage *copy = message_native(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE), *im;

    if (!copy)
        return 0;
    message_forget(ctx, guest, KAFFEINE_SHADOW_GTMESSAGE);
    im = GT_PostFilterIMsg(copy);
    if (!im)
        return 0;
    msg = message_guest(ctx, im, KAFFEINE_SHADOW_INTUIMESSAGE);
    if (!msg)
    {
        /* The program cannot reply it: Intuition gets it back. */
        bug("[kaffeine] GT_PostFilterIMsg(): no guest memory for an IntuiMessage\n");
        ReplyMsg(&im->ExecMessage);
    }
    return msg;
}
