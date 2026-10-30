/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    workbench.library of 68k code: the AppWindows, AppIcons and AppMenuItems
    of the native Workbench, whose AppMessages reach the program's own port.
    Behind a 68k port that the program gives to workbench.library stand two
    native ports. Workbench puts its AppMessages to the first, which copies
    each into a carrier with duplicates of its locks and replies it at once,
    in the task of the sender, as Workbench of AmigaOS does not wait for the
    program: the SendAppWindowMessage() of AROS waits for the reply holding
    the lock of Workbench, which RemoveAppWindow() and the other App calls
    take, so a reply that waited for the program could stop both. The
    carriers go to the second port, which has the signal of the 68k port and
    the task of the program, so that a Wait() on that signal wakes up for an
    AppMessage as on AmigaOS; GetMsg() and WaitPort() of the 68k port give a
    68k copy of each, with its WBArgs and names in guest memory and the
    duplicated locks as the bridge passes every lock, and ReplyMsg() of the
    copy frees the copy and the carrier with its locks. The program gets a
    guest token for each App object.
    An AppIcon takes a DiskObject that the program built in its own memory,
    copied into native memory with its images, and gives NULL for one that
    AmigaOS 3.2.3 does not take. A DiskObject that the bridge's icon.library
    gave the program, a snapshot without images, gets a native clone of the
    object behind it instead (see "icon.library DiskObjects" below). Tags
    are not bridged yet: a list with any tag stops the program. When the
    program deletes a port or frees its signal, and at its end, the App
    objects on the port go and its AppMessages go back. Native data that an
    App object gave Workbench goes only after Workbench has let the object
    go.
*/

#include "kaffeine_intern.h"
#include "kaffeine_locks.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks.h"
#include "kaffeine_workbench.h"

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/dos.h>
#include <proto/exec.h>
/* Select the SDK macros that also expose an explicit library base. */
#define ICON_NOLIBINLINE
#include <proto/icon.h>
#include <proto/workbench.h>
#include <string.h>

/* The plain icon.library calls of the SDK go through the global IconBase,
   which kaffeine_icon.c and every other context share. An AppIcon calls
   icon.library through a base of its own instead, with the forms of the
   SDK that take the base as their first argument. */
#if !defined(__DupDiskObjectA_WB) || !defined(__FreeDiskObject_WB)
#error "AppIcons need the base-taking icon.library calls of the SDK"
#endif
/* DupDiskObjectA() is of icon.library 44 */
#define ICON_VERSION 44

/* The 68k Message, MsgPort, AppMessage, WBArg, DiskObject, Gadget and
   Image of the NDK */
enum
{
    G_LN_SUCC = 0, G_LN_PRED = 4, G_LN_TYPE = 8, G_MN_LENGTH = 18,
    G_MP_FLAGS = 14, G_MP_SIGBIT = 15, G_MP_MSGLIST = 20, G_MSGPORT_SIZE = 34,
    G_LH_TAIL = 4, G_LH_TAILPRED = 8,
    G_AM_TYPE = 20, G_AM_USERDATA = 22, G_AM_ID = 26, G_AM_NUMARGS = 30,
    G_AM_ARGLIST = 34, G_AM_VERSION = 38, G_AM_CLASS = 40, G_AM_MOUSEX = 42,
    G_AM_MOUSEY = 44, G_AM_SECONDS = 46, G_AM_MICROS = 50, G_APPMESSAGE_SIZE = 86,
    G_WBARG_LOCK = 0, G_WBARG_NAME = 4, G_WBARG_SIZE = 8,
    G_DO_MAGIC = 0, G_DO_VERSION = 2, G_DO_GADGET = 4, G_DO_TYPE = 48,
    G_DO_DEFAULTTOOL = 50, G_DO_TOOLTYPES = 54, G_DO_CURRENTX = 58,
    G_DO_CURRENTY = 62, G_DO_DRAWERDATA = 66, G_DO_TOOLWINDOW = 70,
    G_DO_STACKSIZE = 74, G_DISKOBJECT_SIZE = 78,
    G_GG_NEXTGADGET = 0, G_GG_LEFTEDGE = 4, G_GG_TOPEDGE = 6, G_GG_WIDTH = 8,
    G_GG_HEIGHT = 10, G_GG_FLAGS = 12, G_GG_ACTIVATION = 14, G_GG_GADGETTYPE = 16,
    G_GG_GADGETRENDER = 18, G_GG_SELECTRENDER = 22, G_GG_GADGETTEXT = 26,
    G_GG_MUTUALEXCLUDE = 30, G_GG_SPECIALINFO = 34, G_GG_GADGETID = 38,
    G_GG_USERDATA = 40,
    G_IM_LEFTEDGE = 0, G_IM_TOPEDGE = 2, G_IM_WIDTH = 4, G_IM_HEIGHT = 6,
    G_IM_DEPTH = 8, G_IM_IMAGEDATA = 10, G_IM_PLANEPICK = 14,
    G_IM_PLANEONOFF = 15, G_IMAGE_SIZE = 20
};

/* The kinds of App objects */
enum { APP_WINDOW = 1, APP_ICON, APP_MENUITEM };

/* Longer than these, a name, a tool type list or an image is none that an
   AppIcon or an AppMessage means. */
#define MAX_TEXT      1024
#define MAX_TOOLTYPES 256
#define MAX_IMAGE     1024
#define MAX_ARGS      4096

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* The length of the guest string at addr, ~0 for none of at most max bytes */
static ULONG guest_length(struct KaffeineContext *ctx, ULONG addr, ULONG max)
{
    const UBYTE *end;

    if (!guest_range(ctx, addr, 1))
        return ~(ULONG)0;
    end = memchr(ctx->mem + addr, 0, ctx->mem_size - addr < max + 1 ? ctx->mem_size - addr : max + 1);
    return end ? (ULONG)(end - (ctx->mem + addr)) : ~(ULONG)0;
}

/* Stop the program with return code 20, which the thunk returns so that it
   reaches D0, as the other refusals of the bridge do. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* A tag list that the bridge takes: none, or one that ends at once */
static BOOL no_tags(struct KaffeineContext *ctx, ULONG tags)
{
    return !tags || (guest_range(ctx, tags, 4) && m68k_read32(ctx, tags) == TAG_DONE);
}

/* ── Ports ───────────────────────────────────────────────────────── */

/* The native side of a 68k port: front, which Workbench gets and whose
   function (PA_CALL of AROS) replies each AppMessage at once, and inbox,
   with the signal of the 68k port, where the carriers wait for the
   program */
struct AppPort
{
    struct MsgPort front;
    struct MsgPort inbox;
    BOOL detached; /* the guest port has gone; reply native deliveries only */
};

/* An AppMessage that front has replied: its fields, and its WBArgs with
   their names and duplicates of their locks, which the carrier keeps until
   the program replies the 68k copy */
struct Carrier
{
    struct Message message;
    struct AppMessage am;
    struct WBArg args[];
};

/* A carrier of am, in the task of the sender, which DupLock() needs to be
   a process; NULL out of memory */
static struct Carrier *carry(struct AppMessage *am)
{
    ULONG args = am->am_NumArgs > 0 && am->am_ArgList ? (ULONG)am->am_NumArgs : 0, size, i;
    struct Task *task = FindTask(NULL);
    struct Carrier *carrier;
    char *names;

    if (args > MAX_ARGS)
        args = 0;
    size = sizeof(*carrier) + args * sizeof(struct WBArg);
    for (i = 0; i < args; i++)
        if (am->am_ArgList[i].wa_Name)
            size += strlen((const char *)am->am_ArgList[i].wa_Name) + 1;
    if (!(carrier = (struct Carrier *)AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    carrier->am = *am;
    carrier->am.am_NumArgs = args;
    carrier->am.am_ArgList = args ? carrier->args : NULL;
    names = (char *)&carrier->args[args];
    for (i = 0; i < args; i++)
    {
        if (am->am_ArgList[i].wa_Lock && task->tc_Node.ln_Type == NT_PROCESS)
            carrier->args[i].wa_Lock = DupLock(am->am_ArgList[i].wa_Lock);
        if (am->am_ArgList[i].wa_Name)
        {
            strcpy(names, (const char *)am->am_ArgList[i].wa_Name);
            carrier->args[i].wa_Name = (BYTE *)names;
            names += strlen(names) + 1;
        }
    }
    return carrier;
}

static void free_carrier(struct Carrier *carrier)
{
    ULONG i;

    for (i = 0; i < (ULONG)carrier->am.am_NumArgs; i++)
        if (carrier->args[i].wa_Lock)
            UnLock(carrier->args[i].wa_Lock);
    FreeVec(carrier);
}

/* PutMsg() to front calls this in the task of the sender: a carrier of each
   AppMessage goes to inbox, and the AppMessage goes back at once */
AROS_UFH2S(void, take_appmessages,
          AROS_UFHA(struct MsgPort *, front, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct AppPort *port = (struct AppPort *)front;
    struct Message *msg;
    struct Carrier *carrier;

    while ((msg = GetMsg(front)))
    {
        if (!port->detached)
        {
            if ((carrier = carry((struct AppMessage *)msg)))
                PutMsg(&port->inbox, &carrier->message);
            else
                bug("[kaffeine] AppMessage: no memory for a carrier, replied\n");
        }
        ReplyMsg(msg);
    }

    AROS_USERFUNC_EXIT
}

static int port_index(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_MAX_APP_PORTS; i++)
        if (ctx->app_ports[i].guest == guest)
            return i;
    return -1;
}

/* The native ports behind a 68k port of the program, made with the port's
   signal, which must be one that the program allocated, on first use; -1
   for a port that the bridge cannot stand behind */
static int app_port(struct KaffeineContext *ctx, ULONG guest)
{
    struct AppPort *native;
    int i = port_index(ctx, guest);
    UBYTE sig;

    if (i >= 0)
        return i;
    if (!guest_range(ctx, guest, G_MSGPORT_SIZE) ||
        (ctx->mem[guest + G_MP_FLAGS] & PF_ACTION) != PA_SIGNAL)
        return -1;
    sig = ctx->mem[guest + G_MP_SIGBIT];
    if (sig > 31 || !(ctx->guest_signals & (1UL << sig)))
        return -1;
    for (i = 0; i < KAFFEINE_MAX_APP_PORTS && ctx->app_ports[i].guest; i++)
        ;
    if (i == KAFFEINE_MAX_APP_PORTS ||
        !(native = (struct AppPort *)AllocMem(sizeof(*native), MEMF_PUBLIC | MEMF_CLEAR)))
        return -1;
    native->front.mp_Node.ln_Type = NT_MSGPORT;
    native->front.mp_Flags = PA_CALL;
    native->front.mp_SigTask = (APTR)take_appmessages;
    NEWLIST(&native->front.mp_MsgList);
    native->inbox.mp_Node.ln_Type = NT_MSGPORT;
    native->inbox.mp_Flags = PA_SIGNAL;
    native->inbox.mp_SigBit = sig;
    native->inbox.mp_SigTask = FindTask(NULL);
    NEWLIST(&native->inbox.mp_MsgList);
    ctx->app_ports[i].guest = guest;
    ctx->app_ports[i].native = &native->front;
    return i;
}

static struct MsgPort *inbox(struct KaffeineContext *ctx, int p)
{
    return &((struct AppPort *)ctx->app_ports[p].native)->inbox;
}

BOOL Kaffeine_AppPortMatches(struct KaffeineContext *ctx, ULONG guest, struct Task *driver, UBYTE bit)
{
    int p = port_index(ctx, guest);
    struct MsgPort *box;
    if (p < 0) return TRUE;
    box = inbox(ctx, p);
    return box->mp_Flags == PA_SIGNAL && box->mp_SigTask == driver && box->mp_SigBit == bit;
}

/* ── App objects ─────────────────────────────────────────────────── */

static int object_index(struct KaffeineContext *ctx, ULONG guest, UBYTE kind)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_MAX_APP_OBJECTS; i++)
        if (ctx->app_objects[i].guest == guest && ctx->app_objects[i].kind == kind)
            return i;
    return -1;
}

/* A free slot and its guest token, or -1 */
static int new_object(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_APP_OBJECTS && ctx->app_objects[i].guest; i++)
        ;
    if (i == KAFFEINE_MAX_APP_OBJECTS)
        return -1;
    if (!(ctx->app_objects[i].guest = Kaffeine_HeapAlloc(ctx, 4, MEMF_PUBLIC | MEMF_CLEAR)))
        return -1;
    return i;
}

/* The App object goes, after Workbench has let it go: its token, the clone
   of an icon.library DiskObject through the base that the object holds,
   then that base, and its native copies */
static void drop_object(struct KaffeineContext *ctx, int i)
{
    Kaffeine_HeapFree(ctx, ctx->app_objects[i].guest, 4);
    if (ctx->app_objects[i].clone)
        __FreeDiskObject_WB(ctx->app_objects[i].icon_base, ctx->app_objects[i].clone);
    if (ctx->app_objects[i].icon_base)
        CloseLibrary(ctx->app_objects[i].icon_base);
    if (ctx->app_objects[i].copy)
        FreeVec(ctx->app_objects[i].copy);
    memset(&ctx->app_objects[i], 0, sizeof(ctx->app_objects[i]));
}

/* An App object with native data that Workbench would not let go at the
   end of its port: the token goes, and the data stay, with the reference
   to icon.library of a clone, as Workbench may still read them */
static void abandon_object(struct KaffeineContext *ctx, int i)
{
    bug("[kaffeine] Workbench keeps an App object, leaving its native data\n");
    Kaffeine_HeapFree(ctx, ctx->app_objects[i].guest, 4);
    memset(&ctx->app_objects[i], 0, sizeof(ctx->app_objects[i]));
}

/* The App object goes from Workbench: TRUE when Workbench says so */
static BOOL remove_native(struct KaffeineContext *ctx, int i)
{
    APTR native = ctx->app_objects[i].native;

    switch (ctx->app_objects[i].kind)
    {
    case APP_WINDOW:
        return RemoveAppWindow((struct AppWindow *)native);
    case APP_ICON:
        return RemoveAppIcon((struct AppIcon *)native);
    case APP_MENUITEM:
        return RemoveAppMenuItem((struct AppMenuItem *)native);
    }
    return FALSE;
}

/* The native copy of the guest string at addr, in a block that the App
   object keeps, or NULL for none */
static STRPTR copy_text(struct KaffeineContext *ctx, ULONG addr, APTR *copy)
{
    ULONG length = guest_length(ctx, addr, MAX_TEXT);
    STRPTR text;

    if (length == ~(ULONG)0 || !(text = (STRPTR)AllocVec(length + 1, MEMF_PUBLIC)))
        return NULL;
    CopyMem(ctx->mem + addr, text, length + 1);
    *copy = text;
    return text;
}

/* ── DiskObjects ─────────────────────────────────────────────────── */

#define ALIGN8(x) (((x) + 7) & ~(ULONG)7)

/* The bytes of the native data of the guest Image at image: Depth planes
   of rows of words, as DupDiskObject() copies them; ~0 for an image that
   is none, or whose data is not in guest memory. *taken gets the bytes of
   the planes that drawing it takes, the only ones that the program gives:
   one for each plane that PlanePick puts data in, as intuition.h says, and
   at most Depth, as DrawImage() takes them; the others stay zero. */
static ULONG image_data_size(struct KaffeineContext *ctx, ULONG image, ULONG *taken)
{
    UWORD width, height, depth;
    UBYTE pick;
    ULONG plane, planes = 0, i;

    if (!guest_range(ctx, image, G_IMAGE_SIZE))
        return ~(ULONG)0;
    width = m68k_read16(ctx, image + G_IM_WIDTH);
    height = m68k_read16(ctx, image + G_IM_HEIGHT);
    depth = m68k_read16(ctx, image + G_IM_DEPTH);
    pick = ctx->mem[image + G_IM_PLANEPICK];
    if (!width || !height || !depth || width > MAX_IMAGE || height > MAX_IMAGE || depth > 8)
        return ~(ULONG)0;
    for (i = 0; i < 8; i++)
        if (pick & (1 << i))
            planes++;
    plane = ((width + 15) >> 4) * 2 * height;
    *taken = plane * (planes < depth ? planes : depth);
    if (*taken && !guest_range(ctx, m68k_read32(ctx, image + G_IM_IMAGEDATA), *taken))
        return ~(ULONG)0;
    return plane * depth;
}

/* An Image in guest memory that AddAppIconA() of AmigaOS 3.2.3 does not
   take, giving NULL: one without a width, a height, planes or data, or with
   more than 8 planes */
static BOOL image_fails(struct KaffeineContext *ctx, ULONG image)
{
    UWORD depth;

    if (!guest_range(ctx, image, G_IMAGE_SIZE))
        return FALSE;
    depth = m68k_read16(ctx, image + G_IM_DEPTH);
    return !m68k_read16(ctx, image + G_IM_WIDTH) || !m68k_read16(ctx, image + G_IM_HEIGHT) ||
           !depth || depth > 8 || !m68k_read32(ctx, image + G_IM_IMAGEDATA);
}

/* A DiskObject in guest memory that AddAppIconA() of AmigaOS 3.2.3 does not
   take, giving NULL: one without a render Image, with GFLG_GADGHIMAGE (which
   GADGHNONE has too) and no select Image, or with an Image that it does not
   take; AROS gives NULL without a render Image too */
static BOOL diskobject_fails(struct KaffeineContext *ctx, ULONG dobj)
{
    ULONG gadget = dobj + G_DO_GADGET, render, select = 0;
    UWORD flags;

    if (!guest_range(ctx, dobj, G_DISKOBJECT_SIZE))
        return FALSE;
    flags = m68k_read16(ctx, gadget + G_GG_FLAGS);
    render = m68k_read32(ctx, gadget + G_GG_GADGETRENDER);
    if (flags & GFLG_GADGHIMAGE)
        select = m68k_read32(ctx, gadget + G_GG_SELECTRENDER);
    return !render || ((flags & GFLG_GADGHIMAGE) && !select) || image_fails(ctx, render) ||
           (select && image_fails(ctx, select));
}

/* The native Image of a guest one, with its data, at *at in the cleared
   block */
static struct Image *copy_image(struct KaffeineContext *ctx, ULONG image, UBYTE **at)
{
    struct Image *native = (struct Image *)*at;
    ULONG taken, size = image_data_size(ctx, image, &taken), data = m68k_read32(ctx, image + G_IM_IMAGEDATA);
    UWORD *words;
    ULONG i;

    *at += ALIGN8(sizeof(*native));
    native->LeftEdge = (WORD)m68k_read16(ctx, image + G_IM_LEFTEDGE);
    native->TopEdge = (WORD)m68k_read16(ctx, image + G_IM_TOPEDGE);
    native->Width = (WORD)m68k_read16(ctx, image + G_IM_WIDTH);
    native->Height = (WORD)m68k_read16(ctx, image + G_IM_HEIGHT);
    native->Depth = (WORD)m68k_read16(ctx, image + G_IM_DEPTH);
    native->PlanePick = ctx->mem[image + G_IM_PLANEPICK];
    native->PlaneOnOff = ctx->mem[image + G_IM_PLANEONOFF];
    /* the words as values, which the host keeps in its own byte order */
    words = (UWORD *)*at;
    for (i = 0; i < taken / 2; i++)
        words[i] = m68k_read16(ctx, data + 2 * i);
    native->ImageData = words;
    *at += ALIGN8(size);
    return native;
}

/* A native copy, in one block that *copy gets, of the DiskObject that the
   program built in its memory at dobj, with the images of its gadget, its
   default tool and its tool types, and the AppIcon's text before it; NULL
   for a DiskObject that is none, or out of memory */
static struct DiskObject *copy_diskobject(struct KaffeineContext *ctx, ULONG dobj, ULONG text,
                                          STRPTR *native_text, APTR *copy)
{
    ULONG gadget = dobj + G_DO_GADGET, render = 0, select = 0, size, tool, types, count = 0;
    ULONG text_length, tool_length = 0, render_size = 0, select_size = 0, taken, i;
    UWORD flags;
    struct DiskObject *native;
    UBYTE *block, *at;

    if (!guest_range(ctx, dobj, G_DISKOBJECT_SIZE) ||
        (text_length = guest_length(ctx, text, MAX_TEXT)) == ~(ULONG)0)
        return NULL;
    flags = m68k_read16(ctx, gadget + G_GG_FLAGS);
    render = m68k_read32(ctx, gadget + G_GG_GADGETRENDER);
    if ((render_size = image_data_size(ctx, render, &taken)) == ~(ULONG)0)
        return NULL;
    /* the select image of GFLG_GADGHIMAGE, which GADGHNONE has too */
    if (flags & GFLG_GADGHIMAGE)
    {
        select = m68k_read32(ctx, gadget + G_GG_SELECTRENDER);
        if ((select_size = image_data_size(ctx, select, &taken)) == ~(ULONG)0)
            return NULL;
    }
    tool = m68k_read32(ctx, dobj + G_DO_DEFAULTTOOL);
    if (tool && (tool_length = guest_length(ctx, tool, MAX_TEXT)) == ~(ULONG)0)
        return NULL;
    types = m68k_read32(ctx, dobj + G_DO_TOOLTYPES);
    size = ALIGN8(text_length + 1) + ALIGN8(sizeof(*native)) + 2 * ALIGN8(sizeof(struct Image)) +
           ALIGN8(render_size) + ALIGN8(select_size) + ALIGN8(tool_length + 1);
    if (types)
    {
        for (count = 0;; count++)
        {
            ULONG type = m68k_read32(ctx, types + 4 * count), length;

            if (count == MAX_TOOLTYPES || !guest_range(ctx, types + 4 * count, 4))
                return NULL;
            if (!type)
                break;
            if ((length = guest_length(ctx, type, MAX_TEXT)) == ~(ULONG)0)
                return NULL;
            size += length + 1;
        }
        size += ALIGN8((count + 1) * sizeof(STRPTR));
    }
    if (!(block = (UBYTE *)AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;

    at = block;
    CopyMem(ctx->mem + text, at, text_length + 1);
    *native_text = (STRPTR)at;
    at += ALIGN8(text_length + 1);
    native = (struct DiskObject *)at;
    at += ALIGN8(sizeof(*native));
    /* The autodoc of AddAppIconA() gives a DiskObject that the program
       builds neither the magic of an icon nor GFLG_GADGIMAGE, its
       GadgetRender being an Image all the same; the copy has both, without
       which icon.library of AROS draws no image. */
    native->do_Magic = WB_DISKMAGIC;
    native->do_Version = WB_DISKVERSION;
    native->do_Gadget.LeftEdge = (WORD)m68k_read16(ctx, gadget + G_GG_LEFTEDGE);
    native->do_Gadget.TopEdge = (WORD)m68k_read16(ctx, gadget + G_GG_TOPEDGE);
    native->do_Gadget.Width = (WORD)m68k_read16(ctx, gadget + G_GG_WIDTH);
    native->do_Gadget.Height = (WORD)m68k_read16(ctx, gadget + G_GG_HEIGHT);
    native->do_Gadget.Flags = flags | GFLG_GADGIMAGE;
    native->do_Gadget.Activation = m68k_read16(ctx, gadget + G_GG_ACTIVATION);
    native->do_Gadget.GadgetType = m68k_read16(ctx, gadget + G_GG_GADGETTYPE);
    native->do_Gadget.GadgetID = m68k_read16(ctx, gadget + G_GG_GADGETID);
    native->do_Gadget.GadgetRender = copy_image(ctx, render, &at);
    if (select)
        native->do_Gadget.SelectRender = copy_image(ctx, select, &at);
    native->do_Type = ctx->mem[dobj + G_DO_TYPE];
    native->do_CurrentX = (LONG)m68k_read32(ctx, dobj + G_DO_CURRENTX);
    native->do_CurrentY = (LONG)m68k_read32(ctx, dobj + G_DO_CURRENTY);
    native->do_StackSize = (LONG)m68k_read32(ctx, dobj + G_DO_STACKSIZE);
    if (tool)
    {
        CopyMem(ctx->mem + tool, at, tool_length + 1);
        native->do_DefaultTool = (STRPTR)at;
        at += ALIGN8(tool_length + 1);
    }
    if (types)
    {
        STRPTR *array = (STRPTR *)at;
        UBYTE *strings = at + ALIGN8((count + 1) * sizeof(STRPTR));

        for (i = 0; i < count; i++)
        {
            ULONG type = m68k_read32(ctx, types + 4 * i), length = guest_length(ctx, type, MAX_TEXT);

            CopyMem(ctx->mem + type, strings, length + 1);
            array[i] = (STRPTR)strings;
            strings += length + 1;
        }
        native->do_ToolTypes = array;
    }
    *copy = block;
    return native;
}

/* ── icon.library DiskObjects ────────────────────────────────────── */

/* The DiskObjects of the bridge's icon.library are metadata-only snapshots
   in guest memory, without images, with the native object behind them in
   ctx->icons (kaffeine_icon.c). Only the snapshot at the exact address
   that icon.library gave, while the program owns it, stands for its native
   object: an address inside one, a copy of one or one that the program
   freed is a DiskObject of the program's own like any other, however
   alike its bytes. Its AppIcon gets a DupDiskObjectA() clone of the native
   object, with its images, their data and its strings, as AddAppIconA()
   keeps the DiskObject until RemoveAppIcon(); the App object keeps the
   clone with a reference to icon.library of its own, so that the program
   may free the snapshot and close icon.library while the AppIcon lives.
   The guest bytes are never taken as a native DiskObject. Of the snapshot
   the program may change do_CurrentX and do_CurrentY, which the clone
   takes; every other field of the classic DiskObject and its Gadget must
   have the value that icon.library gave, its pointers must stay within the
   snapshot at the text of the native strings, and GadgetRender and
   SelectRender stay NULL (a snapshot that the program gave a GadgetRender
   Image is one that it built, which wb_AddAppIconA() copies). */

/* The owner entry of the snapshot at exactly dobj, or NULL */
static struct KaffeineIcon *owned_snapshot(struct KaffeineContext *ctx, ULONG dobj)
{
    int i;

    for (i = 0; dobj && i < KAFFEINE_MAX_ICONS; i++)
        if (ctx->icons[i].guest == dobj && ctx->icons[i].native)
            return &ctx->icons[i];
    return NULL;
}

/* The guest string pointer at field of the snapshot: NULL for no native
   string s, else within the snapshot at the text of s */
static BOOL snapshot_text(struct KaffeineContext *ctx, const struct KaffeineIcon *icon,
                          ULONG addr, CONST_STRPTR s)
{
    ULONG length, offset = addr - icon->guest;

    if (!s || !addr)
        return !s && !addr;
    length = strlen((const char *)s);
    return addr >= icon->guest && offset < icon->size && icon->size - offset > length &&
           !memcmp(ctx->mem + addr, s, length + 1);
}

/* The snapshot as icon.library gave it, but for its position */
static BOOL snapshot_unchanged(struct KaffeineContext *ctx, const struct KaffeineIcon *icon)
{
    const struct DiskObject *native = icon->native;
    const struct Gadget *g = &native->do_Gadget;
    ULONG dobj = icon->guest, gadget = dobj + G_DO_GADGET, types, offset, count, i;

    if (icon->size < G_DISKOBJECT_SIZE || !guest_range(ctx, dobj, icon->size))
        return FALSE;
    if (m68k_read16(ctx, dobj + G_DO_MAGIC) != native->do_Magic ||
        m68k_read16(ctx, dobj + G_DO_VERSION) != native->do_Version ||
        m68k_read32(ctx, gadget + G_GG_NEXTGADGET) ||
        m68k_read16(ctx, gadget + G_GG_LEFTEDGE) != (UWORD)g->LeftEdge ||
        m68k_read16(ctx, gadget + G_GG_TOPEDGE) != (UWORD)g->TopEdge ||
        m68k_read16(ctx, gadget + G_GG_WIDTH) != (UWORD)g->Width ||
        m68k_read16(ctx, gadget + G_GG_HEIGHT) != (UWORD)g->Height ||
        m68k_read16(ctx, gadget + G_GG_FLAGS) != g->Flags ||
        m68k_read16(ctx, gadget + G_GG_ACTIVATION) != g->Activation ||
        m68k_read16(ctx, gadget + G_GG_GADGETTYPE) != g->GadgetType ||
        m68k_read32(ctx, gadget + G_GG_GADGETRENDER) ||
        m68k_read32(ctx, gadget + G_GG_SELECTRENDER) ||
        m68k_read32(ctx, gadget + G_GG_GADGETTEXT) ||
        m68k_read32(ctx, gadget + G_GG_MUTUALEXCLUDE) != (ULONG)g->MutualExclude ||
        m68k_read32(ctx, gadget + G_GG_SPECIALINFO) ||
        m68k_read16(ctx, gadget + G_GG_GADGETID) != g->GadgetID ||
        m68k_read32(ctx, gadget + G_GG_USERDATA) ||
        ctx->mem[dobj + G_DO_TYPE] != native->do_Type ||
        m68k_read32(ctx, dobj + G_DO_DRAWERDATA) ||
        m68k_read32(ctx, dobj + G_DO_STACKSIZE) != (ULONG)native->do_StackSize ||
        !snapshot_text(ctx, icon, m68k_read32(ctx, dobj + G_DO_DEFAULTTOOL), native->do_DefaultTool) ||
        !snapshot_text(ctx, icon, m68k_read32(ctx, dobj + G_DO_TOOLWINDOW), native->do_ToolWindow))
        return FALSE;
    types = m68k_read32(ctx, dobj + G_DO_TOOLTYPES);
    if (!native->do_ToolTypes)
        return !types;
    for (count = 0; count < MAX_TOOLTYPES && native->do_ToolTypes[count]; count++)
        ;
    offset = types - dobj;
    if (count == MAX_TOOLTYPES || types < dobj || offset >= icon->size || (icon->size - offset) / 4 <= count)
        return FALSE;
    for (i = 0; i < count; i++)
        if (!snapshot_text(ctx, icon, m68k_read32(ctx, types + 4 * i), native->do_ToolTypes[i]))
            return FALSE;
    return !m68k_read32(ctx, types + 4 * count);
}

/* The AppIcon of the snapshot of icon, with the arguments of
   AddAppIconA(): its text and token, then a reference to icon.library and
   the clone, which go back in the opposite order when the next of them or
   Workbench fails, giving 0 without stopping the program */
static IPTR add_snapshot_icon(struct KaffeineContext *ctx, void *cpu, int port,
                              const struct KaffeineIcon *icon)
{
    struct Library *base;
    struct DiskObject *clone;
    struct AppIcon *native;
    BPTR lock;
    STRPTR text;
    APTR copy = NULL;
    int slot;

    if (!Kaffeine_LockResolve(ctx, THUNK_A(2), &lock)) return 20;
    if (!snapshot_unchanged(ctx, icon))
        return refuse(ctx, "AddAppIconA(): a DiskObject of icon.library that the program changed "
                           "in more than its position");
    if (guest_length(ctx, THUNK_A(0), MAX_TEXT) == ~(ULONG)0)
        return refuse(ctx, "AddAppIconA(): no text of at most 1024 bytes in guest memory");
    if (!(text = copy_text(ctx, THUNK_A(0), &copy)))
        return 0;
    if ((slot = new_object(ctx)) < 0)
    {
        FreeVec(copy);
        return 0;
    }
    if (!(base = OpenLibrary("icon.library", ICON_VERSION)))
        goto no_base;
    /* no tags: images, their data, the strings and the drawer data are all
       duplicated, as the AppIcon outlives the native original */
    if (!(clone = __DupDiskObjectA_WB(base, icon->native, NULL)))
        goto no_clone;
    /* the position of the snapshot, in the 32 bits of the classic fields */
    clone->do_CurrentX = (LONG)m68k_read32(ctx, icon->guest + G_DO_CURRENTX);
    clone->do_CurrentY = (LONG)m68k_read32(ctx, icon->guest + G_DO_CURRENTY);
    if (!(native = AddAppIconA(THUNK_D(0), THUNK_D(1), text, ctx->app_ports[port].native,
                               lock, clone, NULL)))
    {
        __FreeDiskObject_WB(base, clone);
        goto no_clone;
    }
    ctx->app_objects[slot].kind = APP_ICON;
    ctx->app_objects[slot].port = (UBYTE)port;
    ctx->app_objects[slot].native = native;
    ctx->app_objects[slot].copy = copy;
    ctx->app_objects[slot].clone = clone;
    ctx->app_objects[slot].icon_base = base;
    return ctx->app_objects[slot].guest;

no_clone:
    CloseLibrary(base);
no_base:
    Kaffeine_HeapFree(ctx, ctx->app_objects[slot].guest, 4);
    ctx->app_objects[slot].guest = 0;
    FreeVec(copy);
    return 0;
}

/* ── Thunks ──────────────────────────────────────────────────────── */

/* The new App object of a native one, or 0, with its native copies going
   when Workbench gave none */
static IPTR add_object(struct KaffeineContext *ctx, int slot, UBYTE kind, int port,
                       APTR native, APTR copy)
{
    if (!native)
    {
        Kaffeine_HeapFree(ctx, ctx->app_objects[slot].guest, 4);
        ctx->app_objects[slot].guest = 0;
        if (copy)
            FreeVec(copy);
        return 0;
    }
    ctx->app_objects[slot].kind = kind;
    ctx->app_objects[slot].port = (UBYTE)port;
    ctx->app_objects[slot].native = native;
    ctx->app_objects[slot].copy = copy;
    return ctx->app_objects[slot].guest;
}

/* -48: AddAppWindowA(D0=id, D1=userdata, A0=window, A1=msgport, A2=taglist) */
static IPTR wb_AddAppWindowA(struct KaffeineContext *ctx, void *cpu)
{
    struct Window *window = Kaffeine_ShadowNative(ctx, THUNK_A(0), KAFFEINE_SHADOW_WINDOW);
    int port, slot;

    if (!window)
        return refuse(ctx, "AddAppWindowA(): not a window of the program");
    if (!no_tags(ctx, THUNK_A(2)))
        return refuse(ctx, "AddAppWindowA(): its tags are not bridged yet");
    if ((port = app_port(ctx, THUNK_A(1))) < 0)
        return refuse(ctx, "AddAppWindowA(): no port with a signal of the program");
    if ((slot = new_object(ctx)) < 0)
        return 0;
    return add_object(ctx, slot, APP_WINDOW, port,
                      AddAppWindowA(THUNK_D(0), THUNK_D(1), window,
                                    ctx->app_ports[port].native, NULL), NULL);
}

/* -60: AddAppIconA(D0=id, D1=userdata, A0=text, A1=msgport, A2=lock,
   A3=diskobj, A4=taglist) */
static IPTR wb_AddAppIconA(struct KaffeineContext *ctx, void *cpu)
{
    struct DiskObject *dobj;
    BPTR lock;
    struct KaffeineIcon *icon;
    STRPTR text = NULL;
    APTR copy = NULL;
    int port, slot;

    if (!Kaffeine_LockResolve(ctx, THUNK_A(2), &lock)) return 20;
    if (!no_tags(ctx, THUNK_A(4)))
        return refuse(ctx, "AddAppIconA(): its tags are not bridged yet");
    if ((port = app_port(ctx, THUNK_A(1))) < 0)
        return refuse(ctx, "AddAppIconA(): no port with a signal of the program");
    /* a snapshot of icon.library, which has no image of its own */
    if ((icon = owned_snapshot(ctx, THUNK_A(3))) &&
        !m68k_read32(ctx, THUNK_A(3) + G_DO_GADGET + G_GG_GADGETRENDER))
        return add_snapshot_icon(ctx, cpu, port, icon);
    if (diskobject_fails(ctx, THUNK_A(3)))
    {
        bug("[kaffeine] AddAppIconA(): a DiskObject that AmigaOS 3.2.3 does not take, NULL\n");
        return 0;
    }
    if (!(dobj = copy_diskobject(ctx, THUNK_A(3), THUNK_A(0), &text, &copy)))
        return refuse(ctx, "AddAppIconA(): no DiskObject of the program's own with its images "
                           "and text in guest memory");
    if ((slot = new_object(ctx)) < 0)
    {
        FreeVec(copy);
        return 0;
    }
    return add_object(ctx, slot, APP_ICON, port,
                      AddAppIconA(THUNK_D(0), THUNK_D(1), text, ctx->app_ports[port].native,
                                  lock, dobj, NULL), copy);
}

/* -72: AddAppMenuItemA(D0=id, D1=userdata, A0=text, A1=msgport, A2=taglist) */
static IPTR wb_AddAppMenuItemA(struct KaffeineContext *ctx, void *cpu)
{
    STRPTR text;
    APTR copy = NULL;
    int port, slot;

    if (!no_tags(ctx, THUNK_A(2)))
        return refuse(ctx, "AddAppMenuItemA(): its tags are not bridged yet");
    if ((port = app_port(ctx, THUNK_A(1))) < 0)
        return refuse(ctx, "AddAppMenuItemA(): no port with a signal of the program");
    if (!(text = copy_text(ctx, THUNK_A(0), &copy)))
        return refuse(ctx, "AddAppMenuItemA(): no text of at most 1024 bytes in guest memory");
    if ((slot = new_object(ctx)) < 0)
    {
        FreeVec(copy);
        return 0;
    }
    return add_object(ctx, slot, APP_MENUITEM, port,
                      AddAppMenuItemA(THUNK_D(0), THUNK_D(1), text, ctx->app_ports[port].native, NULL),
                      copy);
}

/* -54, -66, -78: RemoveAppWindow(), RemoveAppIcon(), RemoveAppMenuItem()
   (A0=object): FALSE for a token that is none of that kind. An object
   that Workbench does not let go stays with the native data that it gave
   Workbench, for another removal or the end of its port. */
static IPTR remove_object(struct KaffeineContext *ctx, ULONG guest, UBYTE kind)
{
    int i = object_index(ctx, guest, kind);
    BOOL removed;

    if (i < 0)
        return guest ? refuse(ctx, "RemoveApp...(): not an App object of the program of that kind")
                     : FALSE;
    removed = remove_native(ctx, i);
    if (!removed)
    {
        bug("[kaffeine] RemoveApp...(): Workbench keeps the object, which stays\n");
        return FALSE;
    }
    drop_object(ctx, i);
    return removed;
}

static IPTR wb_RemoveAppWindow(struct KaffeineContext *ctx, void *cpu)
{
    return remove_object(ctx, THUNK_A(0), APP_WINDOW);
}

static IPTR wb_RemoveAppIcon(struct KaffeineContext *ctx, void *cpu)
{
    return remove_object(ctx, THUNK_A(0), APP_ICON);
}

static IPTR wb_RemoveAppMenuItem(struct KaffeineContext *ctx, void *cpu)
{
    return remove_object(ctx, THUNK_A(0), APP_MENUITEM);
}

/* -90: WBInfo(A0=lock, A1=name, A2=screen): the lock as the bridge passes
   every lock, the name in guest memory, a screen of the program */
static IPTR wb_WBInfo(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name = THUNK_A(1);
    BPTR lock;
    struct Screen *screen = THUNK_A(2) ? Kaffeine_ScreenNative(ctx, THUNK_A(2)) : NULL;

    if (!Kaffeine_LockResolve(ctx, THUNK_A(0), &lock)) return 20;
    if (THUNK_A(2) && !screen)
        return refuse(ctx, "WBInfo(): not a screen of the program");
    if (guest_length(ctx, name, MAX_TEXT) == ~(ULONG)0)
        return refuse(ctx, "WBInfo(): no name of at most 1024 bytes in guest memory");
    return WBInfo(lock, (CONST_STRPTR)(ctx->mem + name), screen);
}

const struct M68KThunkEntry kaffeine_thunks_workbench[] = {
    { 48, wb_AddAppWindowA },
    { 54, wb_RemoveAppWindow },
    { 60, wb_AddAppIconA },
    { 66, wb_RemoveAppIcon },
    { 72, wb_AddAppMenuItemA },
    { 78, wb_RemoveAppMenuItem },
    { 90, wb_WBInfo },
};
const ULONG kaffeine_thunks_workbench_count =
    sizeof(kaffeine_thunks_workbench) / sizeof(kaffeine_thunks_workbench[0]);

/* ── AppMessages ─────────────────────────────────────────────────── */

/* The 68k copy of the AppMessage of a carrier of port p, registered for its
   reply, or 0 without guest memory or a free entry */
static ULONG copy_message(struct KaffeineContext *ctx, int p, struct Carrier *carrier)
{
    struct AppMessage *am = &carrier->am;
    ULONG args = am->am_NumArgs > 0 ? (ULONG)am->am_NumArgs : 0, size, guest, names, i;
    int slot;

    for (slot = 0; slot < KAFFEINE_MAX_APP_MESSAGES && ctx->app_messages[slot].guest; slot++)
        ;
    if (slot == KAFFEINE_MAX_APP_MESSAGES)
        return 0;
    if (args > MAX_ARGS || (args && !am->am_ArgList))
        args = 0;
    size = ALIGN8(G_APPMESSAGE_SIZE) + args * G_WBARG_SIZE;
    for (i = 0; i < args; i++)
        if (am->am_ArgList[i].wa_Name)
            size += strlen((const char *)am->am_ArgList[i].wa_Name) + 1;
    if (!(guest = Kaffeine_HeapAlloc(ctx, size, MEMF_PUBLIC | MEMF_CLEAR)))
        return 0;

    ctx->mem[guest + G_LN_TYPE] = NT_MESSAGE;
    m68k_write16(ctx, guest + G_MN_LENGTH, G_APPMESSAGE_SIZE);
    m68k_write16(ctx, guest + G_AM_TYPE, am->am_Type);
    m68k_write32(ctx, guest + G_AM_USERDATA, (ULONG)am->am_UserData);
    m68k_write32(ctx, guest + G_AM_ID, (ULONG)am->am_ID);
    m68k_write32(ctx, guest + G_AM_NUMARGS, args);
    m68k_write16(ctx, guest + G_AM_VERSION, am->am_Version);
    m68k_write16(ctx, guest + G_AM_CLASS, am->am_Class);
    m68k_write16(ctx, guest + G_AM_MOUSEX, (UWORD)am->am_MouseX);
    m68k_write16(ctx, guest + G_AM_MOUSEY, (UWORD)am->am_MouseY);
    m68k_write32(ctx, guest + G_AM_SECONDS, am->am_Seconds);
    m68k_write32(ctx, guest + G_AM_MICROS, am->am_Micros);
    names = guest + ALIGN8(G_APPMESSAGE_SIZE) + args * G_WBARG_SIZE;
    if (args)
        m68k_write32(ctx, guest + G_AM_ARGLIST, guest + ALIGN8(G_APPMESSAGE_SIZE));
    for (i = 0; i < args; i++)
    {
        ULONG arg = guest + ALIGN8(G_APPMESSAGE_SIZE) + i * G_WBARG_SIZE;
        const char *name = (const char *)am->am_ArgList[i].wa_Name;

        ULONG lock = Kaffeine_LockBorrow(ctx, am->am_ArgList[i].wa_Lock,
                                         KAFFEINE_LOCK_WBARG, guest);
        if (am->am_ArgList[i].wa_Lock && !lock)
        {
            Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_WBARG, guest);
            Kaffeine_HeapFree(ctx, guest, 0);
            return 0;
        }
        m68k_write32(ctx, arg + G_WBARG_LOCK, lock);
        if (name)
        {
            ULONG length = strlen(name) + 1;

            CopyMem(name, ctx->mem + names, length);
            m68k_write32(ctx, arg + G_WBARG_NAME, names);
            names += length;
        }
    }
    ctx->app_messages[slot].guest = guest;
    ctx->app_messages[slot].port = (UBYTE)p;
    ctx->app_messages[slot].native = &carrier->message;
    return guest;
}

BOOL Kaffeine_AppPortFill(struct KaffeineContext *ctx, ULONG port, BOOL wait)
{
    int p = port_index(ctx, port);
    struct MsgPort *box;
    struct Carrier *carrier;
    ULONG list, copy, tail;

    if (p < 0)
        return FALSE;
    box = inbox(ctx, p);
    list = port + G_MP_MSGLIST;
    /* the program's own messages first, and a copy that it has not taken */
    if (m68k_read32(ctx, list) != list + G_LH_TAIL)
        return TRUE;
    if (wait)
        WaitPort(box);
    if (!(carrier = (struct Carrier *)GetMsg(box)))
        return TRUE;
    if (!(copy = copy_message(ctx, p, carrier)))
    {
        /* without guest memory or a free entry the message is lost to the
           program, which then finds its port empty */
        bug("[kaffeine] AppMessage: no guest memory or entry for a copy, lost\n");
        free_carrier(carrier);
        return TRUE;
    }
    tail = m68k_read32(ctx, list + G_LH_TAILPRED);
    m68k_write32(ctx, copy + G_LN_SUCC, list + G_LH_TAIL);
    m68k_write32(ctx, copy + G_LN_PRED, tail);
    m68k_write32(ctx, tail + G_LN_SUCC, copy);
    m68k_write32(ctx, list + G_LH_TAILPRED, copy);
    return TRUE;
}

/* Take the copy at message out of the list of its port, if it is still
   there because the program replies it without GetMsg() */
static void unlink_copy(struct KaffeineContext *ctx, ULONG port, ULONG message)
{
    ULONG list = port + G_MP_MSGLIST, node = m68k_read32(ctx, list), n;

    for (n = 0; node && node != list + G_LH_TAIL && n < KAFFEINE_MAX_APP_MESSAGES; n++)
    {
        if (node == message)
        {
            ULONG succ = m68k_read32(ctx, node + G_LN_SUCC), pred = m68k_read32(ctx, node + G_LN_PRED);

            m68k_write32(ctx, pred + G_LN_SUCC, succ);
            m68k_write32(ctx, succ + G_LN_PRED, pred);
            return;
        }
        node = m68k_read32(ctx, node + G_LN_SUCC);
    }
}

static void reply_copy(struct KaffeineContext *ctx, int slot)
{
    ULONG guest = ctx->app_messages[slot].guest;

    unlink_copy(ctx, ctx->app_ports[ctx->app_messages[slot].port].guest, guest);
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_WBARG, guest);
    free_carrier((struct Carrier *)ctx->app_messages[slot].native);
    Kaffeine_HeapFree(ctx, guest, 0);
    memset(&ctx->app_messages[slot], 0, sizeof(ctx->app_messages[slot]));
}

BOOL Kaffeine_AppReplyMsg(struct KaffeineContext *ctx, ULONG message)
{
    int slot;

    for (slot = 0; message && slot < KAFFEINE_MAX_APP_MESSAGES; slot++)
        if (ctx->app_messages[slot].guest == message)
        {
            reply_copy(ctx, slot);
            return TRUE;
        }
    return FALSE;
}

/* ── Ends ────────────────────────────────────────────────────────── */

/* The App objects on port p go, then its copies and its carriers, and its
   native ports go: after the App objects, nothing puts to them */
static void port_gone(struct KaffeineContext *ctx, int p)
{
    struct AppPort *port = (struct AppPort *)ctx->app_ports[p].native;
    struct Message *msg;
    BOOL retained = FALSE;
    int i;

    /* No delivery after this point may signal the departing task or make
       another carrier. The front callback needs neither ctx nor guest
       memory when detached. A sender already in the callback can still
       put a carrier on the native inbox, which remains valid below. */
    port->detached = TRUE;
    port->inbox.mp_Flags = PA_IGNORE;
    port->inbox.mp_SigTask = NULL;
    for (i = 0; i < KAFFEINE_MAX_APP_OBJECTS; i++)
        if (ctx->app_objects[i].guest && ctx->app_objects[i].port == p)
        {
            if (remove_native(ctx, i))
                drop_object(ctx, i);
            else
            {
                abandon_object(ctx, i);
                retained = TRUE;
            }
        }
    for (i = 0; i < KAFFEINE_MAX_APP_MESSAGES; i++)
        if (ctx->app_messages[i].guest && ctx->app_messages[i].port == p)
            reply_copy(ctx, i);
    while ((msg = GetMsg(inbox(ctx, p))))
        free_carrier((struct Carrier *)msg);
    /* RemoveApp...() normally succeeds for a live AROS object. If it
       refuses, Workbench still owns the front pointer as well as the
       icon/label data: retain the detached port, too. Later deliveries
       are replied without touching the task, its signal or guest memory. */
    if (!retained)
        FreeMem(port, sizeof(*port));
    memset(&ctx->app_ports[p], 0, sizeof(ctx->app_ports[p]));
}

void Kaffeine_AppPortGone(struct KaffeineContext *ctx, ULONG port)
{
    int p = port_index(ctx, port);

    if (p >= 0)
        port_gone(ctx, p);
}

void Kaffeine_AppSignalGone(struct KaffeineContext *ctx, BYTE sigbit)
{
    int p;

    for (p = 0; p < KAFFEINE_MAX_APP_PORTS; p++)
        if (ctx->app_ports[p].guest && inbox(ctx, p)->mp_SigBit == (UBYTE)sigbit)
            port_gone(ctx, p);
}

void Kaffeine_WorkbenchCleanup(struct KaffeineContext *ctx)
{
    int p;

    for (p = 0; p < KAFFEINE_MAX_APP_PORTS; p++)
        if (ctx->app_ports[p].guest)
        {
            bug("[kaffeine] removing App objects that the 68k program left\n");
            port_gone(ctx, p);
        }
}
