/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public screens of 68k code. Each native screen that the program locks
    has one 68k Screen, which every LockPubScreen() of that screen hands
    out and fills in again: the place, the size, the mouse, the flags, the
    sizes of the bars and borders, the font as a 68k TextAttr, the display
    fields of the ViewPort, the sizes and depth of the BitMap, and the pens.
    The embedded ViewPort stands for the native one in calls such as
    GetVPModeID(), and the embedded RastPort is a 68k RastPort, which
    follows the native one around the graphics calls as the RastPort of a
    window does. Its 68k TextFont stays for the whole program, as the font
    of a window does, and the bridge holds the native font open until the
    end of the program, so that the TextFont never stands for a font that
    has gone; a screen whose font it cannot hold gets no 68k Screen. The
    name of the TextAttr is the one of that TextFont, as AskFont() of AROS
    gives it, so that it also holds in a copy of the TextAttr that the
    program keeps. Native pointers, such as the windows, the titles, the
    ColorMap, the layers and the planes, are not shown to 68k code and stay
    0. A screen that the program opened keeps the 68k Screen of
    OpenScreen().

    A 68k Screen stays while the program holds a lock on its screen, has a
    window there or holds the list of public screens, so that WScreen of a
    window stays valid after the program has unlocked the screen, as on
    AmigaOS; it goes with the last of them. Intuition does not close a
    screen while any of them holds (CloseScreen() refuses a public screen
    with visitors or windows and waits for the list), so a 68k Screen never
    stands for a screen that has gone. UnlockPubScreen() gives Intuition
    only a lock that the program took, since Intuition counts down whatever
    it gets, and the locks and the list lock that the program leaves go
    back to Intuition at its end.

    LockPubScreenList() gives a 68k copy of the list, made under the lock
    and filled in again at each nested lock, in which psn_Screen is the
    program's 68k Screen of that screen, or 0 for a screen that the program
    has none of; the NDK warns that a copy's screen pointer cannot be used
    safely anyway. NextPubScreen() and GetDefaultPubScreen() write names
    into guest memory, where the name and its NUL must fit.
*/

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"

#include <aros/debug.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k Screen, ViewPort, BitMap, RastPort, TextAttr, List and
   PubScreenNode of the NDK */
enum
{
    G_SCR_MOUSEY = 16, G_SCR_MOUSEX = 18, G_SCR_BARHEIGHT = 30, G_SCR_BARVBORDER = 31,
    G_SCR_BARHBORDER = 32, G_SCR_MENUVBORDER = 33, G_SCR_MENUHBORDER = 34,
    G_SCR_WBORTOP = 35, G_SCR_WBORLEFT = 36, G_SCR_WBORRIGHT = 37, G_SCR_WBORBOTTOM = 38,
    G_SCR_FONT = 40, G_SCR_BITMAP = 184, G_SCR_DETAILPEN = 330, G_SCR_BLOCKPEN = 331,
    G_SCR_SAVECOLOR0 = 332,
    G_VP_DWIDTH = 24, G_VP_DHEIGHT = 26, G_VP_DXOFFSET = 28, G_VP_DYOFFSET = 30,
    G_VP_MODES = 32, G_VP_SPRITEPRIORITIES = 34, G_VP_EXTENDEDMODES = 35,
    G_BM_BYTESPERROW = 0, G_BM_ROWS = 2, G_BM_FLAGS = 4, G_BM_DEPTH = 5, G_RP_FONT = 52,
    G_TA_NAME = 0, G_TA_YSIZE = 4, G_TA_STYLE = 6, G_TA_FLAGS = 7,
    G_LH_HEAD = 0, G_LH_TAIL = 4, G_LH_TAILPRED = 8, G_LH_TYPE = 12, G_LIST_ROOM = 16,
    G_LN_SUCC = 0, G_LN_PRED = 4, G_LN_TYPE = 8, G_LN_PRI = 9, G_LN_NAME = 10,
    G_PSN_SCREEN = 14, G_PSN_FLAGS = 18, G_PSN_SIZE = 20, G_PSN_VISITORCOUNT = 22,
    G_PSN_SIGTASK = 24, G_PSN_SIGBIT = 28, G_PSN_SIZEOF = 30
};

/* Intuition counts the locks of a screen in a WORD; a program alone does
   not take it past this. */
#define MAX_LOCKS 0x7FFF
/* The same for the nested locks of the list, an Exec semaphore */
#define MAX_LIST_LOCKS 0x7FFF
/* More public screens than this are not copied. */
#define MAX_LISTED 256
/* A font name is copied up to this length. */
#define MAX_FONT_NAME 255

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* A string that ends inside guest memory, or NULL */
static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!guest_range(ctx, addr, 1) || !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return (const char *)ctx->mem + addr;
}

/* Stop the program with return code 20. The thunk returns it, so that it
   reaches D0, from which the CPU loop takes the exit code. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* Names compare as Intuition compares them, without regard to the case of
   ASCII letters. */
static BOOL same_name(const char *a, const char *b)
{
    UBYTE x, y;

    do
    {
        x = (UBYTE)*a++;
        y = (UBYTE)*b++;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
    } while (x == y && x);
    return x == y;
}

/* The length of a name, at most max characters */
static ULONG name_length(const char *name, ULONG max)
{
    const char *end = name ? memchr(name, 0, max) : NULL;

    return !name ? 0 : end ? (ULONG)(end - name) : max;
}

/* Whether a public screen name, at most MAXPUBSCREENNAME characters, and
   its NUL fit into guest memory there */
static BOOL name_fits(struct KaffeineContext *ctx, ULONG guest, const char *name)
{
    return guest_range(ctx, guest, name_length(name, MAXPUBSCREENNAME) + 1);
}

/* A public screen name into guest memory where it fits */
static void copy_name(struct KaffeineContext *ctx, ULONG guest, const char *name)
{
    ULONG length = name_length(name, MAXPUBSCREENNAME);

    if (length)
        memcpy(ctx->mem + guest, name, length);
    ctx->mem[guest + length] = 0;
}

/* ── The 68k Screens ─────────────────────────────────────────────── */

/* The entry of a native screen, or of a 68k Screen, or -1 */
static int screen_index(struct KaffeineContext *ctx, const struct Screen *native, ULONG guest)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].guest && (native ? ctx->pubscreens[i].native == native
                                                : ctx->pubscreens[i].guest == guest))
            return i;
    return -1;
}

/* The fields that the program reads, from the native screen */
static void screen_fill(struct KaffeineContext *ctx, ULONG guest, ULONG font, struct Screen *scr)
{
    UBYTE *g = ctx->mem + guest;
    ULONG vp = guest + M68K_SCR_VIEWPORT, bm = guest + G_SCR_BITMAP;

    m68k_write16(ctx, guest + M68K_SCR_LEFTEDGE, (UWORD)scr->LeftEdge);
    m68k_write16(ctx, guest + M68K_SCR_TOPEDGE, (UWORD)scr->TopEdge);
    m68k_write16(ctx, guest + M68K_SCR_WIDTH, (UWORD)scr->Width);
    m68k_write16(ctx, guest + M68K_SCR_HEIGHT, (UWORD)scr->Height);
    m68k_write16(ctx, guest + G_SCR_MOUSEY, (UWORD)scr->MouseY);
    m68k_write16(ctx, guest + G_SCR_MOUSEX, (UWORD)scr->MouseX);
    m68k_write16(ctx, guest + M68K_SCR_FLAGS, scr->Flags);
    g[G_SCR_BARHEIGHT] = (UBYTE)scr->BarHeight;
    g[G_SCR_BARVBORDER] = (UBYTE)scr->BarVBorder;
    g[G_SCR_BARHBORDER] = (UBYTE)scr->BarHBorder;
    g[G_SCR_MENUVBORDER] = (UBYTE)scr->MenuVBorder;
    g[G_SCR_MENUHBORDER] = (UBYTE)scr->MenuHBorder;
    g[G_SCR_WBORTOP] = (UBYTE)scr->WBorTop;
    g[G_SCR_WBORLEFT] = (UBYTE)scr->WBorLeft;
    g[G_SCR_WBORRIGHT] = (UBYTE)scr->WBorRight;
    g[G_SCR_WBORBOTTOM] = (UBYTE)scr->WBorBottom;
    /* the font of a screen stays while it is open; the name is written once */
    if (font && scr->Font)
    {
        m68k_write32(ctx, guest + G_SCR_FONT, font);
        m68k_write16(ctx, font + G_TA_YSIZE, scr->Font->ta_YSize);
        /* a TTextAttr's tags are not copied */
        ctx->mem[font + G_TA_STYLE] = scr->Font->ta_Style & ~FSF_TAGGED;
        ctx->mem[font + G_TA_FLAGS] = scr->Font->ta_Flags;
    }
    m68k_write16(ctx, vp + G_VP_DWIDTH, (UWORD)scr->ViewPort.DWidth);
    m68k_write16(ctx, vp + G_VP_DHEIGHT, (UWORD)scr->ViewPort.DHeight);
    m68k_write16(ctx, vp + G_VP_DXOFFSET, (UWORD)scr->ViewPort.DxOffset);
    m68k_write16(ctx, vp + G_VP_DYOFFSET, (UWORD)scr->ViewPort.DyOffset);
    m68k_write16(ctx, vp + G_VP_MODES, scr->ViewPort.Modes);
    ctx->mem[vp + G_VP_SPRITEPRIORITIES] = scr->ViewPort.SpritePriorities;
    ctx->mem[vp + G_VP_EXTENDEDMODES] = scr->ViewPort.ExtendedModes;
    /* what OpenScreen() of AROS puts into the obsolete BitMap, without the
       planes */
    m68k_write16(ctx, bm + G_BM_BYTESPERROW, scr->BitMap_OBSOLETE.BytesPerRow);
    m68k_write16(ctx, bm + G_BM_ROWS, scr->BitMap_OBSOLETE.Rows);
    ctx->mem[bm + G_BM_FLAGS] = scr->BitMap_OBSOLETE.Flags;
    ctx->mem[bm + G_BM_DEPTH] = scr->BitMap_OBSOLETE.Depth;
    g[G_SCR_DETAILPEN] = scr->DetailPen;
    g[G_SCR_BLOCKPEN] = scr->BlockPen;
    m68k_write16(ctx, guest + G_SCR_SAVECOLOR0, scr->SaveColor0);
}

/* Hold the native font of the RastPort of a screen open for the rest of
   the program, as its 68k TextFont stays, once for each font: through
   OpenFont() of the attributes that AskFont() gives for that font, as
   AROS's OpenScreen() makes the TextAttr of the screen, when OpenFont()
   gives that very font. TRUE when it is held, also from before, and for
   no font; *taken is the entry that this call filled, or -1. FALSE when
   the table is full or OpenFont() gives another font or none. */
static BOOL font_hold(struct KaffeineContext *ctx, struct TextFont *tf, int *taken)
{
    struct TTextAttr ta;
    struct TextFont *opened;
    int i, slot = -1;

    *taken = -1;
    if (!tf)
        return TRUE;
    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
    {
        if (ctx->pubscreen_fonts[i] == tf)
            return TRUE;
        if (!ctx->pubscreen_fonts[i] && slot < 0)
            slot = i;
    }
    if (slot < 0)
    {
        bug("[kaffeine] public screen font table full, %p not held\n", tf);
        return FALSE;
    }
    /* a TTextAttr without tags, as an extended font has FSF_TAGGED */
    ta.tta_Name = tf->tf_Message.mn_Node.ln_Name;
    ta.tta_YSize = tf->tf_YSize;
    ta.tta_Style = tf->tf_Style;
    ta.tta_Flags = tf->tf_Flags;
    ta.tta_Tags = NULL;
    opened = OpenFont((struct TextAttr *)&ta);
    if (opened != tf)
    {
        if (opened)
            CloseFont(opened);
        bug("[kaffeine] font %p of a public screen not held\n", tf);
        return FALSE;
    }
    ctx->pubscreen_fonts[slot] = tf;
    *taken = slot;
    return TRUE;
}

/* Give back the font that font_hold() took for a 68k Screen that failed */
static void font_unhold(struct KaffeineContext *ctx, int taken)
{
    if (taken < 0)
        return;
    CloseFont(ctx->pubscreen_fonts[taken]);
    ctx->pubscreen_fonts[taken] = NULL;
}

/* A new 68k Screen of a native one, all or none: its block with the
   TextAttr, and with the font name when it is not the one of the TextFont
   of the RastPort, the ViewPort and the RastPort registered, the 68k
   TextFont of the RastPort, and its native font held; 0 without memory or
   room in the shadow table or the font tables, and when the font cannot be
   held. */
static ULONG screen_create(struct KaffeineContext *ctx, struct Screen *scr, ULONG *font)
{
    const char *name = scr->Font ? (const char *)scr->Font->ta_Name : NULL;
    /* AskFont() points the name into the TextFont of the RastPort */
    BOOL own_name = name && !(scr->RastPort.Font &&
                              name == scr->RastPort.Font->tf_Message.mn_Node.ln_Name);
    ULONG length = own_name ? name_length(name, MAX_FONT_NAME) : 0;
    ULONG size = M68K_SIZEOF_SCREEN, guest, rp;
    int held;

    *font = 0;
    if (scr->Font)
        size += M68K_SIZEOF_TEXTATTR + (own_name ? length + 1 : 0);
    if (ctx->num_shadows > KAFFEINE_MAX_SHADOWS - 3)
    {
        bug("[kaffeine] shadow table full, no 68k Screen for %p\n", scr);
        return 0;
    }
    /* the font first, so that no 68k TextFont stays for a font that the
       bridge does not hold */
    if (!font_hold(ctx, scr->RastPort.Font, &held))
        return 0;
    if (!(guest = Kaffeine_MirrorAlloc(ctx, size)))
    {
        font_unhold(ctx, held);
        return 0;
    }
    rp = guest + M68K_SCR_RASTPORT;
    if (!Kaffeine_ShadowAdd(ctx, guest, scr, KAFFEINE_SHADOW_PUBSCREEN) ||
        !Kaffeine_ShadowAdd(ctx, guest + M68K_SCR_VIEWPORT, &scr->ViewPort, KAFFEINE_SHADOW_VIEWPORT) ||
        !Kaffeine_RastPortAttach(ctx, rp, &scr->RastPort) ||
        /* a RastPort without the 68k TextFont of its font would show NULL */
        (scr->RastPort.Font && !m68k_read32(ctx, rp + G_RP_FONT)))
    {
        Kaffeine_RastPortDetach(ctx, rp);
        Kaffeine_ShadowForget(ctx, guest + M68K_SCR_VIEWPORT, KAFFEINE_SHADOW_VIEWPORT);
        Kaffeine_ShadowForget(ctx, guest, KAFFEINE_SHADOW_PUBSCREEN);
        Kaffeine_MirrorRelease(ctx, guest);
        font_unhold(ctx, held);
        return 0;
    }
    if (scr->Font)
    {
        *font = guest + M68K_SIZEOF_SCREEN;
        if (own_name)
        {
            memcpy(ctx->mem + *font + M68K_SIZEOF_TEXTATTR, name, length);
            ctx->mem[*font + M68K_SIZEOF_TEXTATTR + length] = 0;
            m68k_write32(ctx, *font + G_TA_NAME, *font + M68K_SIZEOF_TEXTATTR);
        }
        else if (name)
            m68k_write32(ctx, *font + G_TA_NAME,
                         m68k_read32(ctx, m68k_read32(ctx, rp + G_RP_FONT) + G_LN_NAME));
    }
    return guest;
}

/* A native screen is the program's while it holds a lock on it or has a
   window there; while the program holds the list of public screens, under
   which Intuition closes no public screen, every 68k Screen stays, also
   when there was no memory for the copy. */
static BOOL screen_used(struct KaffeineContext *ctx, int i)
{
    UWORD s;

    if (ctx->pubscreens[i].locks || ctx->pubscreen_list_locks)
        return TRUE;
    for (s = 0; s < ctx->num_shadows; s++)
        if (ctx->shadow_map[s].type == KAFFEINE_SHADOW_WINDOW &&
            ((struct Window *)ctx->shadow_map[s].native_ptr)->WScreen == ctx->pubscreens[i].native)
            return TRUE;
    return FALSE;
}

/* The entry goes, with the 68k Screen that the bridge made for it */
static void screen_forget(struct KaffeineContext *ctx, int i)
{
    ULONG guest = ctx->pubscreens[i].guest;

    if (Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_PUBSCREEN))
    {
        Kaffeine_RastPortDetach(ctx, guest + M68K_SCR_RASTPORT);
        Kaffeine_ShadowForget(ctx, guest + M68K_SCR_VIEWPORT, KAFFEINE_SHADOW_VIEWPORT);
        Kaffeine_ShadowForget(ctx, guest, KAFFEINE_SHADOW_PUBSCREEN);
        Kaffeine_MirrorRelease(ctx, guest);
    }
    ctx->pubscreens[i].native = NULL;
    ctx->pubscreens[i].guest = 0;
    ctx->pubscreens[i].font = 0;
    ctx->pubscreens[i].locks = 0;
}

/* ctx->screen_release: a 68k Window of the program on that screen has
   gone. */
static void screen_release(struct KaffeineContext *ctx, struct Screen *scr)
{
    int i = scr ? screen_index(ctx, scr, 0) : -1;

    if (i >= 0 && !screen_used(ctx, i))
        screen_forget(ctx, i);
}

/* The entry of a screen that the program has just locked: the one it has,
   else a new one with a 68k Screen of its own, or with the 68k Screen of a
   screen that the program opened; -1 without memory or room. */
static int screen_entry(struct KaffeineContext *ctx, struct Screen *scr)
{
    int i = screen_index(ctx, scr, 0);
    ULONG guest, font = 0;

    if (i >= 0)
        return i;
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS && ctx->pubscreens[i].guest; i++)
        ;
    if (i == KAFFEINE_MAX_PUBSCREENS)
    {
        bug("[kaffeine] public screen table full, no 68k Screen for %p\n", scr);
        return -1;
    }
    guest = Kaffeine_ShadowGuest(ctx, scr, KAFFEINE_SHADOW_SCREEN);
    if (!guest && !(guest = screen_create(ctx, scr, &font)))
        return -1;
    ctx->pubscreens[i].native = scr;
    ctx->pubscreens[i].guest = guest;
    ctx->pubscreens[i].font = font;
    ctx->pubscreens[i].locks = 0;
    ctx->screen_release = screen_release;
    return i;
}

/* The public screen of that name, found as Intuition finds it, or NULL */
static struct Screen *named_screen(const char *name)
{
    struct List *list = LockPubScreenList();
    struct Screen *scr = NULL;
    struct Node *node;

    for (node = list->lh_Head; node->ln_Succ; node = node->ln_Succ)
        if (node->ln_Name && same_name(node->ln_Name, name))
        {
            scr = ((struct PubScreenNode *)node)->psn_Screen;
            break;
        }
    UnlockPubScreenList();
    return scr;
}

/* intuition -510: LockPubScreen(A0=name) -> D0: the 68k Screen of the
   screen that Intuition locked, the same at every lock of that screen, and
   its fields filled in again; 0 when Intuition locks none, and when there
   is no memory or room for a 68k Screen, its font cannot be held, or the
   program holds 32767 locks of the screen already: then the lock goes back
   at once. A name that does not end in guest memory stops the program. */
IPTR Kaffeine_LockPubScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name = THUNK_A(0);
    const char *text = NULL;
    struct Screen *scr;
    int i;

    if (name && !(text = guest_string(ctx, name)))
        return refuse(ctx, "LockPubScreen(): a name outside guest memory");
    if (!(scr = LockPubScreen((CONST_STRPTR)text)))
        return 0;
    i = screen_entry(ctx, scr);
    if (i < 0 || ctx->pubscreens[i].locks == MAX_LOCKS)
    {
        UnlockPubScreen(NULL, scr);
        return 0;
    }
    ctx->pubscreens[i].locks++;
    /* the 68k Screen of a screen that the program opened stays as it is */
    if (Kaffeine_ShadowNative(ctx, ctx->pubscreens[i].guest, KAFFEINE_SHADOW_PUBSCREEN))
        screen_fill(ctx, ctx->pubscreens[i].guest, ctx->pubscreens[i].font, scr);
    return ctx->pubscreens[i].guest;
}

/* intuition -516: UnlockPubScreen(A0=name, A1=screen). Intuition gets back
   one lock that the program took: of the public screen of that name, found
   as Intuition finds it, else of the 68k Screen in A1. A name or a screen
   that the program holds no lock of changes nothing, so that the locks of
   others stay. The 68k Screen goes with the last lock, unless a window of
   the program is still there or the program holds the list of public
   screens. A name that does not end in guest memory stops the program. */
IPTR Kaffeine_UnlockPubScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name = THUNK_A(0), screen = THUNK_A(1);
    const char *text = NULL;
    struct Screen *scr;
    int i;

    if (name && !(text = guest_string(ctx, name)))
        return refuse(ctx, "UnlockPubScreen(): a name outside guest memory");
    if (text)
        i = (scr = named_screen(text)) ? screen_index(ctx, scr, 0) : -1;
    else
        i = screen ? screen_index(ctx, NULL, screen) : -1;
    if (i < 0 || !ctx->pubscreens[i].locks)
    {
        D(bug("[kaffeine] UnlockPubScreen(): no lock of the program, nothing to unlock\n"));
        return 0;
    }
    scr = ctx->pubscreens[i].native;
    ctx->pubscreens[i].locks--;
    UnlockPubScreen(NULL, scr);
    screen_release(ctx, scr);
    return 0;
}

/* ── The list of public screens ──────────────────────────────────── */

/* The nodes of the native list that the copy holds */
static ULONG list_nodes(struct List *list)
{
    struct Node *ln;
    ULONG n = 0;

    for (ln = list->lh_Head; ln->ln_Succ && n < MAX_LISTED; ln = ln->ln_Succ)
        n++;
    return n;
}

/* A 68k copy of the native list in one block: the List, the nodes with
   their links, then their names; 0 without memory. */
static ULONG list_copy(struct KaffeineContext *ctx, struct List *list, UWORD *nodes)
{
    ULONG n = list_nodes(list), i, size = G_LIST_ROOM + n * G_PSN_SIZEOF, guest, node, names;
    struct Node *ln;

    for (ln = list->lh_Head, i = 0; i < n; ln = ln->ln_Succ, i++)
        size += name_length(ln->ln_Name, MAXPUBSCREENNAME) + 1;
    if (!(guest = Kaffeine_MirrorAlloc(ctx, size)))
        return 0;
    node = guest + G_LIST_ROOM;
    names = node + n * G_PSN_SIZEOF;
    for (ln = list->lh_Head, i = 0; i < n; ln = ln->ln_Succ, i++, node += G_PSN_SIZEOF)
    {
        m68k_write32(ctx, node + G_LN_SUCC, i + 1 < n ? node + G_PSN_SIZEOF : guest + G_LH_TAIL);
        m68k_write32(ctx, node + G_LN_PRED, i ? node - G_PSN_SIZEOF : guest + G_LH_HEAD);
        m68k_write32(ctx, node + G_LN_NAME, names);
        copy_name(ctx, names, ln->ln_Name);
        names += name_length(ln->ln_Name, MAXPUBSCREENNAME) + 1;
    }
    m68k_write32(ctx, guest + G_LH_HEAD, n ? guest + G_LIST_ROOM : guest + G_LH_TAIL);
    m68k_write32(ctx, guest + G_LH_TAILPRED,
                 n ? guest + G_LIST_ROOM + (n - 1) * G_PSN_SIZEOF : guest + G_LH_HEAD);
    ctx->mem[guest + G_LH_TYPE] = list->lh_Type;
    *nodes = (UWORD)n;
    return guest;
}

/* The fields of the nodes of the copy that change while the program holds
   the list, everything but the links and the names */
static void list_fill(struct KaffeineContext *ctx, struct List *list)
{
    ULONG node = ctx->pubscreen_list + G_LIST_ROOM, i;
    struct Node *ln = list->lh_Head;

    for (i = 0; i < ctx->pubscreen_list_nodes; i++, ln = ln->ln_Succ, node += G_PSN_SIZEOF)
    {
        struct PubScreenNode *psn = (struct PubScreenNode *)ln;

        ctx->mem[node + G_LN_TYPE] = ln->ln_Type;
        ctx->mem[node + G_LN_PRI] = (UBYTE)ln->ln_Pri;
        m68k_write32(ctx, node + G_PSN_SCREEN, Kaffeine_ScreenGuest(ctx, psn->psn_Screen));
        m68k_write16(ctx, node + G_PSN_FLAGS, psn->psn_Flags);
        m68k_write16(ctx, node + G_PSN_SIZE, (UWORD)psn->psn_Size);
        m68k_write16(ctx, node + G_PSN_VISITORCOUNT, (UWORD)psn->psn_VisitorCount);
        ctx->mem[node + G_PSN_SIGBIT] = psn->psn_SigBit;
    }
}

/* intuition -522: LockPubScreenList() -> D0: a 68k copy of the list of
   public screens, made under the lock that the program now holds, the same
   for nested locks and filled in again at each: each node with its name,
   at most MAXPUBSCREENNAME characters, its type, priority, flags, size,
   visitor count and signal bit, psn_Screen the program's 68k Screen of
   that screen or 0, and psn_SigTask 0. The nodes stay where they are,
   with their names, and keep their fields when the native list has
   changed. 0 without memory for the copy, while the program holds the
   lock all the same. The program stops at 32767 nested locks. */
IPTR Kaffeine_LockPubScreenList(struct KaffeineContext *ctx, void *cpu)
{
    struct List *list;

    (void)cpu;
    if (ctx->pubscreen_list_locks == MAX_LIST_LOCKS)
        return refuse(ctx, "LockPubScreenList(): too many nested locks");
    list = LockPubScreenList();
    ctx->pubscreen_list_locks++;
    if (!ctx->pubscreen_list)
        ctx->pubscreen_list = list_copy(ctx, list, &ctx->pubscreen_list_nodes);
    if (ctx->pubscreen_list && list_nodes(list) == ctx->pubscreen_list_nodes)
        list_fill(ctx, list);
    return ctx->pubscreen_list;
}

/* intuition -528: UnlockPubScreenList(). Intuition gets back a list lock
   that the program holds, and nothing when it holds none; with the last
   one the copy goes, and so do the 68k Screens that only it kept. */
IPTR Kaffeine_UnlockPubScreenList(struct KaffeineContext *ctx, void *cpu)
{
    int i;

    (void)cpu;
    if (!ctx->pubscreen_list_locks)
        return 0;
    UnlockPubScreenList();
    if (--ctx->pubscreen_list_locks)
        return 0;
    Kaffeine_MirrorRelease(ctx, ctx->pubscreen_list);
    ctx->pubscreen_list = 0;
    ctx->pubscreen_list_nodes = 0;
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].guest && !screen_used(ctx, i))
            screen_forget(ctx, i);
    return 0;
}

/* intuition -534: NextPubScreen(A0=screen, A1=namebuf) -> D0: namebuf with
   the name of the public screen after that screen of the program, or of
   the first one for NULL, at most MAXPUBSCREENNAME characters, as
   Intuition walks the list; 0 when there is none: after the last one, and
   after a screen that is not public, where Intuition would read through
   NULL; also 0, as from Intuition, without writing for a NULL namebuf. A
   screen that is not a screen of the program, and a name that does not
   fit into guest memory at namebuf with its NUL, stop the program. */
IPTR Kaffeine_NextPubScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG screen = THUNK_A(0), buffer = THUNK_A(1);
    struct Screen *scr = NULL;
    struct List *list;
    struct Node *node;
    IPTR result = 0;
    BOOL outside = FALSE;

    if (screen && !(scr = Kaffeine_ScreenNative(ctx, screen)))
        return refuse(ctx, "NextPubScreen(): not a screen of the program");
    list = LockPubScreenList();
    node = list->lh_Head;
    if (scr)
    {
        while (node->ln_Succ && ((struct PubScreenNode *)node)->psn_Screen != scr)
            node = node->ln_Succ;
        /* the tail of the list for a screen that is not there */
        if (node->ln_Succ)
            node = node->ln_Succ;
    }
    if (node->ln_Succ && buffer)
    {
        outside = !name_fits(ctx, buffer, node->ln_Name);
        if (!outside)
        {
            copy_name(ctx, buffer, node->ln_Name);
            result = buffer;
        }
    }
    UnlockPubScreenList();
    return outside ? refuse(ctx, "NextPubScreen(): a name outside guest memory") : result;
}

/* intuition -582: GetDefaultPubScreen(A0=nameBuffer): the name of the
   default public screen, "Workbench" when there is none, as Intuition
   writes it; nothing for NULL. The function is VOID in the NDK, so D0 is 0
   and the screen that AROS returns stays native. A name that does not fit
   into guest memory at nameBuffer with its NUL stops the program. */
IPTR Kaffeine_GetDefaultPubScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG buffer = THUNK_A(0);
    /* AROS keeps a public screen name in MAXPUBSCREENNAME + 1 bytes. */
    UBYTE name[MAXPUBSCREENNAME + 1];

    if (!buffer)
        return 0;
    name[0] = 0;
    GetDefaultPubScreen(name);
    name[MAXPUBSCREENNAME] = 0;
    if (!name_fits(ctx, buffer, (const char *)name))
        return refuse(ctx, "GetDefaultPubScreen(): a name outside guest memory");
    copy_name(ctx, buffer, (const char *)name);
    return 0;
}

/* ── The end of the program ──────────────────────────────────────── */

void Kaffeine_ScreensCleanup(struct KaffeineContext *ctx)
{
    ULONG left = 0;
    int i;

    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        for (; ctx->pubscreens[i].locks; ctx->pubscreens[i].locks--, left++)
            UnlockPubScreen(NULL, ctx->pubscreens[i].native);
    if (left)
        bug("[kaffeine] %lu public screen locks that the program left, released\n",
            (unsigned long)left);
    if (ctx->pubscreen_list_locks)
        bug("[kaffeine] the list of public screens that the program left locked, released\n");
    for (; ctx->pubscreen_list_locks; ctx->pubscreen_list_locks--)
        UnlockPubScreenList();
    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->pubscreen_fonts[i])
        {
            CloseFont(ctx->pubscreen_fonts[i]);
            ctx->pubscreen_fonts[i] = NULL;
        }
}
