/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public screens of 68k code. Each native screen that the program locks
    has one 68k Screen, which every LockPubScreen() of that screen hands
    out and fills in again: the place, the size, the mouse, the flags, the
    sizes of the bars and borders, the font as a 68k TextAttr, the display
    fields of the ViewPort, the sizes and depth of the BitMap that the
    screen draws into, and the pens. Its RastPort's BitMap is the embedded
    68k BitMap, without planes, a handle that BltBitMap() takes for a screen
    that the program opened (kaffeine_layers.c). The embedded ViewPort stands for the native one in calls such as
    GetVPModeID(), and the embedded RastPort is a 68k RastPort, which
    follows the native one around the graphics calls as the RastPort of a
    window does. Its 68k TextFont stays for the whole program, as the font
    of a window does, and the bridge holds the native font open until the
    end of the program, so that the TextFont never stands for a font that
    has gone; a screen whose font it cannot hold gets no 68k Screen. The
    name of the TextAttr is the one of that TextFont, as AskFont() of AROS
    gives it, so that it also holds in a copy of the TextAttr that the
    program keeps. Native pointers, such as windows, titles, ColorMap
    tables, layers and planes, are not shown to 68k code and stay
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

    OpenScreen() and OpenScreenTagList() give Intuition a native NewScreen
    and one flat native tag list, the tags of A1, else the Extension of an
    ExtNewScreen, as AROS takes them, with only tags whose data the bridge
    knows: scalars, the title and the TextAttr as native copies that stay
    while the screen is open, since Intuition keeps both, the pens as native
    UWORDs and SA_ErrorCode as a native ULONG, written to the program's one
    afterwards. Anything else, a NewScreen with Gadgets or a CustomBitMap,
    and input outside guest memory stop the program before Intuition is
    called. An opened screen gets a 68k Screen as a locked one does, of its
    own shadow type, and its entry in the table of screens, which LockPubScreen()
    of the same screen finds, so that the program sees one 68k Screen of it.
    That 68k Screen stays until CloseScreen() of the program closes the
    screen; when Intuition refuses, everything stays as it was. Screens that
    the program leaves open close at its end, unless a window of the program
    is still on them.

    GetScreenDrawInfo() of a 68k Screen of the program gives a 68k copy of
    the DrawInfo that Intuition keeps in the screen, one for each native
    DrawInfo, which nested gets hand out again and the last free retires.
    The copy is the classic DrawInfo of 50 bytes, dri_Version at most 1,
    in a mirror that guest FreeMem() cannot take, with its own big-endian
    pens (at most 64, then ~0) and the 68k TextFont of dri_Font, whose
    native font the bridge holds as that of a screen. Intuition has no 68k
    Images: dri_CheckMark and dri_AmigaKey stay 0, as do the reserved
    longwords. While the program holds a copy, its 68k Screen stays, and a
    screen that it did not open also keeps a native visitor lock of the
    bridge, so that neither goes with the program's locks or windows;
    CloseScreen() refuses a screen with copies. Each get and free reaches
    Intuition once, as the very pair, and a free of anything else stops the
    program first.

    Pens that ObtainBestPenA() got for the program (kaffeine_palette.c)
    keep the 68k Screen of their ColorMap in the same way, with one visitor
    lock of the bridge for all of them on a screen that the program did not
    open; CloseScreen() refuses a screen with any, and they go back at the
    end of the program before anything else here.
*/

#include "kaffeine_intern.h"
#include "kaffeine_fontattr.h"
#include "kaffeine_layers.h"
#include "kaffeine_offsets.h"
#include "kaffeine_palette.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"
#include "kaffeine_taglist.h"

#include <aros/debug.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#include <string.h>

/* The 68k NewScreen, ExtNewScreen, Screen, ViewPort, BitMap, RastPort,
   TextAttr, List and PubScreenNode of the NDK */
enum
{
    G_NS_FONT = 16, G_NS_GADGETS = 24, G_NS_CUSTOMBITMAP = 28, G_NS_EXTENSION = 32,
    G_EXTNEWSCREEN_SIZE = 36, G_SCR_DEFAULTTITLE = 26,
    G_SCR_MOUSEY = 16, G_SCR_MOUSEX = 18, G_SCR_BARHEIGHT = 30, G_SCR_BARVBORDER = 31,
    G_SCR_BARHBORDER = 32, G_SCR_MENUVBORDER = 33, G_SCR_MENUHBORDER = 34,
    G_SCR_WBORTOP = 35, G_SCR_WBORLEFT = 36, G_SCR_WBORRIGHT = 37, G_SCR_WBORBOTTOM = 38,
    G_SCR_FONT = 40, G_SCR_DETAILPEN = 330, G_SCR_BLOCKPEN = 331,
    G_SCR_SAVECOLOR0 = 332,
    G_VP_DWIDTH = 24, G_VP_DHEIGHT = 26, G_VP_DXOFFSET = 28, G_VP_DYOFFSET = 30,
    G_VP_MODES = 32, G_VP_SPRITEPRIORITIES = 34, G_VP_EXTENDEDMODES = 35,
    G_BM_BYTESPERROW = 0, G_BM_ROWS = 2, G_BM_FLAGS = 4, G_BM_DEPTH = 5, G_RP_FONT = 52,
    G_TA_NAME = 0, G_TA_YSIZE = 4, G_TA_STYLE = 6, G_TA_FLAGS = 7,
    G_LH_HEAD = 0, G_LH_TAIL = 4, G_LH_TAILPRED = 8, G_LH_TYPE = 12, G_LIST_ROOM = 16,
    G_LN_SUCC = 0, G_LN_PRED = 4, G_LN_TYPE = 8, G_LN_PRI = 9, G_LN_NAME = 10,
    G_PSN_SCREEN = 14, G_PSN_FLAGS = 18, G_PSN_SIZE = 20, G_PSN_VISITORCOUNT = 22,
    G_PSN_SIGTASK = 24, G_PSN_SIGBIT = 28, G_PSN_SIZEOF = 30,
    /* the classic DrawInfo storage; version 1 without native menu Images */
    G_DRI_VERSION = 0, G_DRI_NUMPENS = 2, G_DRI_PENS = 4, G_DRI_FONT = 8, G_DRI_DEPTH = 12,
    G_DRI_RESOLUTION_X = 14, G_DRI_RESOLUTION_Y = 16, G_DRI_FLAGS = 18,
    G_DRI_CHECKMARK = 22, G_DRI_AMIGAKEY = 26, G_DRI_RESERVED = 30, G_DRAWINFO_SIZE = 50,
    /* Version 2 promises menu Images; expose version 1 until those have
       classic counterparts, so callers can take their older image path. */
    G_DRI_CLASSIC_VERSION = 1
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
/* The longest title of a screen that the program opens */
#define MAX_TITLE 255
/* SA_Pens: at most this many pens before the ~0 that ends them; Intuition
   reads no more than NUMDRIPENS of them. */
#define MAX_PENS 64
/* The most user tags of one OpenScreenTagList() */
#define MAX_SCREEN_TAGS 256
/* Gets of one DrawInfo that the program holds; Intuition counts none, the
   bridge gives an excess one back. */
#define MAX_DRAWINFO_GETS 0x7FFF
/* The pens of a DrawInfo copy, before the ~0 that ends them */
#define MAX_DRAWINFO_PENS 64

/* What Intuition points to while a screen that the program opened is open:
   SA_Font or the Font of the NewScreen, with its name and tags, and the
   title */
struct KaffeineScreenKeep
{
    struct TTextAttr font;
    struct TagItem font_tags[KAFFEINE_FONTATTR_TAGS + 1];
    char font_name[MAX_FONT_NAME + 1];
    char title[MAX_TITLE + 1];
};

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
static void screen_fill(struct KaffeineContext *ctx, ULONG guest, ULONG font, ULONG colormap,
                        struct Screen *scr)
{
    UBYTE *g = ctx->mem + guest;
    ULONG vp = guest + M68K_SCR_VIEWPORT, bm = guest + M68K_SCR_BITMAP;
    const struct BitMap *draw = scr->RastPort.BitMap;

    /* A borrowed, screen-owned ColorMap identity. Native tables, HIDD
       pointers and allocation metadata never enter the guest arena. */
    m68k_write32(ctx, vp + 4, colormap);
    if (colormap && scr->ViewPort.ColorMap)
    {
        const struct ColorMap *cm = scr->ViewPort.ColorMap;
        memset(ctx->mem + colormap, 0, 52);
        ctx->mem[colormap] = cm->Flags;
        ctx->mem[colormap + 1] = cm->Type;
        m68k_write16(ctx, colormap + 2, cm->Count);
        if (ctx->video_extra_guest)
            m68k_write32(ctx, colormap + 8, ctx->video_extra_guest(ctx, scr, colormap));
        m68k_write32(ctx, colormap + 20, vp);
        m68k_write32(ctx, colormap + 36, cm->VPModeID);
    }

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
    /* The BitMap that the screen draws into, that of its RastPort, not the
       obsolete copy: its scalars, with pad and planes 0, as the planes of
       a HIDD BitMap are no guest memory. RastPort.BitMap names it, and
       graphics calls leave that field as the program wrote it. It is a
       typed handle: BltBitMap() (kaffeine_layers.c) finds the native
       BitMap by this address alone (Kaffeine_ScreenBitMapOf()), and only
       for a screen that the program opened. */
    memset(g + M68K_SCR_BITMAP, 0, M68K_SIZEOF_BITMAP);
    if (draw)
    {
        m68k_write16(ctx, bm + G_BM_BYTESPERROW, draw->BytesPerRow);
        m68k_write16(ctx, bm + G_BM_ROWS, draw->Rows);
        ctx->mem[bm + G_BM_FLAGS] = draw->Flags;
        ctx->mem[bm + G_BM_DEPTH] = draw->Depth;
    }
    m68k_write32(ctx, guest + M68K_SCR_RASTPORT + M68K_RP_BITMAP, draw ? bm : 0);
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
   held. type is the shadow type of the 68k Screen: KAFFEINE_SHADOW_SCREEN
   for a screen that the program opened, else KAFFEINE_SHADOW_PUBSCREEN. */
static ULONG screen_create(struct KaffeineContext *ctx, struct Screen *scr, ULONG *font,
                           ULONG *colormap, UWORD type)
{
    const char *name = scr->Font ? (const char *)scr->Font->ta_Name : NULL;
    /* AskFont() points the name into the TextFont of the RastPort */
    BOOL own_name = name && !(scr->RastPort.Font &&
                              name == scr->RastPort.Font->tf_Message.mn_Node.ln_Name);
    ULONG length = own_name ? name_length(name, MAX_FONT_NAME) : 0;
    ULONG size = M68K_SIZEOF_SCREEN, guest, rp, cm_offset;
    int held;

    *font = 0;
    *colormap = 0;
    if (scr->Font)
        size += M68K_SIZEOF_TEXTATTR + (own_name ? length + 1 : 0);
    cm_offset = (size + 1) & ~1UL;
    if (scr->ViewPort.ColorMap)
        size = cm_offset + 52;
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
    if (!Kaffeine_ShadowAdd(ctx, guest, scr, type) ||
        !Kaffeine_ShadowAdd(ctx, guest + M68K_SCR_VIEWPORT, &scr->ViewPort, KAFFEINE_SHADOW_VIEWPORT) ||
        !Kaffeine_RastPortAttach(ctx, rp, &scr->RastPort) ||
        /* a RastPort without the 68k TextFont of its font would show NULL */
        (scr->RastPort.Font && !m68k_read32(ctx, rp + G_RP_FONT)))
    {
        Kaffeine_RastPortDetach(ctx, rp);
        Kaffeine_ShadowForget(ctx, guest + M68K_SCR_VIEWPORT, KAFFEINE_SHADOW_VIEWPORT);
        Kaffeine_ShadowForget(ctx, guest, type);
        Kaffeine_MirrorRelease(ctx, guest);
        font_unhold(ctx, held);
        return 0;
    }
    Kaffeine_LayerSemaphoreInit(ctx, guest + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
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
    if (scr->ViewPort.ColorMap)
        *colormap = guest + cm_offset;
    return guest;
}

/* Whether a 68k Window of the program lies on that native screen */
static BOOL screen_windows(struct KaffeineContext *ctx, const struct Screen *scr)
{
    UWORD s;

    for (s = 0; s < ctx->num_shadows; s++)
        if (ctx->shadow_map[s].type == KAFFEINE_SHADOW_WINDOW &&
            ((struct Window *)ctx->shadow_map[s].native_ptr)->WScreen == scr)
            return TRUE;
    return FALSE;
}

/* A native screen is the program's while it is open from OpenScreen() of
   the program, while the program holds a lock on it, has a window there,
   a VisualInfo or a DrawInfo copy of it; while the program holds the list
   of public screens, under which Intuition closes no public screen, every
   68k Screen stays, also when there was no memory for the copy. */
static BOOL screen_visuals(struct KaffeineContext *ctx, struct Screen *screen)
{
    int i;
    for (i = 0; i < KAFFEINE_MAX_VISUALINFOS; i++)
        if (ctx->visualinfos[i].native && ctx->visualinfos[i].screen == screen)
            return TRUE;
    return FALSE;
}

/* Whether a DrawInfo copy of the program keeps that native screen */
static BOOL screen_drawinfos(struct KaffeineContext *ctx, const struct Screen *screen)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_DRAWINFOS; i++)
        if (ctx->drawinfos[i].guest && ctx->drawinfos[i].screen == screen)
            return TRUE;
    return FALSE;
}

static BOOL screen_used(struct KaffeineContext *ctx, int i)
{
    /* pen leases (kaffeine_palette.c) keep it like a DrawInfo copy */
    return ctx->pubscreens[i].owned || ctx->pubscreens[i].locks || ctx->pubscreen_list_locks ||
           ctx->pubscreens[i].pens ||
           screen_windows(ctx, ctx->pubscreens[i].native) ||
           screen_visuals(ctx, ctx->pubscreens[i].native) ||
           screen_drawinfos(ctx, ctx->pubscreens[i].native);
}

/* Whether the 68k Screen of the entry is one that the bridge made, and
   fills in again */
static BOOL screen_mirrored(struct KaffeineContext *ctx, int i)
{
    return ctx->pubscreens[i].owned ||
           Kaffeine_ShadowNative(ctx, ctx->pubscreens[i].guest, KAFFEINE_SHADOW_PUBSCREEN);
}

/* An entry without pen leases, as a new one starts and every one ends;
   only kaffeine_palette.c gives the leases themselves back. */
static void screen_pens_clear(struct KaffeineContext *ctx, int i)
{
    memset(ctx->pubscreens[i].pen_refs, 0, sizeof(ctx->pubscreens[i].pen_refs));
    ctx->pubscreens[i].pens = 0;
    ctx->pubscreens[i].pen_lock = FALSE;
}

/* The entry goes, with the 68k Screen that the bridge made for it; for a
   screen that the program opened, only once Intuition has closed it. No
   entry with pen leases goes: they keep it in use, CloseScreen() refuses
   them and the cleanup gives them back first. */
static void screen_forget(struct KaffeineContext *ctx, int i)
{
    ULONG guest = ctx->pubscreens[i].guest;
    UWORD type = ctx->pubscreens[i].owned ? KAFFEINE_SHADOW_SCREEN : KAFFEINE_SHADOW_PUBSCREEN;

    if (ctx->video_screen_retire)
        ctx->video_screen_retire(ctx, ctx->pubscreens[i].native);
    /* the layer snapshot of the screen goes with its last holder, before
       the 68k Screen that holds its LayerInfo */
    if (ctx->layer_ops)
        ctx->layer_ops->screen_closing(ctx, ctx->pubscreens[i].native);
    if (Kaffeine_ShadowNative(ctx, guest, type))
    {
        /* a BitMap handle that the program kept names nothing now, and
           describes no planar BitMap either */
        memset(ctx->mem + guest + M68K_SCR_BITMAP, 0, M68K_SIZEOF_BITMAP);
        if (ctx->pubscreens[i].colormap)
            memset(ctx->mem + ctx->pubscreens[i].colormap, 0, 52);
        m68k_write32(ctx, guest + M68K_SCR_VIEWPORT + 4, 0);
        m68k_write32(ctx, guest + M68K_SCR_RASTPORT + M68K_RP_BITMAP, 0);
        Kaffeine_RastPortDetach(ctx, guest + M68K_SCR_RASTPORT);
        Kaffeine_ShadowForget(ctx, guest + M68K_SCR_VIEWPORT, KAFFEINE_SHADOW_VIEWPORT);
        Kaffeine_ShadowForget(ctx, guest, type);
        Kaffeine_MirrorRelease(ctx, guest);
    }
    ctx->pubscreens[i].native = NULL;
    ctx->pubscreens[i].guest = 0;
    ctx->pubscreens[i].font = 0;
    ctx->pubscreens[i].colormap = 0;
    ctx->pubscreens[i].locks = 0;
    ctx->pubscreens[i].owned = FALSE;
    ctx->pubscreens[i].keep = NULL;
    if (ctx->pubscreens[i].pens)
    {
        bug("[kaffeine] screen %d forgotten with %u pen leases\n", i, (unsigned)ctx->pubscreens[i].pens);
        ctx->pen_leases -= ctx->pubscreens[i].pens;
    }
    screen_pens_clear(ctx, i);
}

/* A screen that the program opened has closed: its entry and 68k Screen
   go, and what Intuition pointed to */
static void screen_closed(struct KaffeineContext *ctx, int i)
{
    struct KaffeineScreenKeep *keep = ctx->pubscreens[i].keep;

    screen_forget(ctx, i);
    FreeVec(keep);
}

/* ctx->screen_release: a 68k Window of the program on that screen has
   gone. */
static void screen_release(struct KaffeineContext *ctx, struct Screen *scr)
{
    int i = scr ? screen_index(ctx, scr, 0) : -1;

    if (i >= 0 && !screen_used(ctx, i))
        screen_forget(ctx, i);
}

/* A free entry of the table, or -1 */
static int screen_free_entry(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (!ctx->pubscreens[i].guest)
            return i;
    return -1;
}

/* The entry of a screen that the program has just locked: the one it has,
   also that of a screen that the program opened, else a new one with a 68k
   Screen of its own, or with a 68k Screen of type KAFFEINE_SHADOW_SCREEN
   that the bridge did not make here, which stays as it is; -1 without
   memory or room. */
static int screen_entry(struct KaffeineContext *ctx, struct Screen *scr)
{
    int i = screen_index(ctx, scr, 0);
    ULONG guest, font = 0, colormap = 0;

    if (i >= 0)
        return i;
    if ((i = screen_free_entry(ctx)) < 0)
    {
        bug("[kaffeine] public screen table full, no 68k Screen for %p\n", scr);
        return -1;
    }
    guest = Kaffeine_ShadowGuest(ctx, scr, KAFFEINE_SHADOW_SCREEN);
    if (!guest && !(guest = screen_create(ctx, scr, &font, &colormap, KAFFEINE_SHADOW_PUBSCREEN)))
        return -1;
    ctx->pubscreens[i].native = scr;
    ctx->pubscreens[i].guest = guest;
    ctx->pubscreens[i].font = font;
    ctx->pubscreens[i].colormap = colormap;
    ctx->pubscreens[i].locks = 0;
    ctx->pubscreens[i].owned = FALSE;
    ctx->pubscreens[i].keep = NULL;
    screen_pens_clear(ctx, i);
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
    /* a 68k Screen that the bridge did not make stays as it is */
    if (screen_mirrored(ctx, i))
        screen_fill(ctx, ctx->pubscreens[i].guest, ctx->pubscreens[i].font,
                    ctx->pubscreens[i].colormap, scr);
    if (ctx->layer_ops)
        ctx->layer_ops->screen_mirrored(ctx, scr);
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
    /* Retire borrowed snapshots while this last native visitor is alive. */
    screen_release(ctx, scr);
    UnlockPubScreen(NULL, scr);
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

/* ── Screens that the program opens ──────────────────────────────── */

/* How the bridge takes each tag of OpenScreenTagList(): a LONG, sign
   extended to the native IPTR as Intuition compares SA_Width with ~0; an
   ULONG, BOOL or pen; SA_Type, which must not ask for a CustomBitMap; the
   tags with data that the bridge converts; and the others, refused. */
enum
{
    ST_REFUSED, ST_SIGNED, ST_UNSIGNED, ST_TYPE, ST_BITMAP, ST_FONT, ST_TITLE, ST_PENS,
    ST_ERROR, ST_PUBNAME, ST_DCLIP
};

static int screen_tag(ULONG tag)
{
    switch (tag)
    {
    case SA_Left: case SA_Top: case SA_Width: case SA_Height: case SA_Depth:
    case SA_SysFont: case SA_Overscan: case SA_ColorMapEntries:
        return ST_SIGNED;
    case SA_DetailPen: case SA_BlockPen: case SA_DisplayID: case SA_ShowTitle:
    case SA_Behind: case SA_Quiet: case SA_AutoScroll: case SA_FullPalette:
    case SA_Draggable: case SA_Exclusive: case SA_SharePens: case SA_Interleaved:
    case SA_LikeWorkbench: case SA_MinimizeISG:
        return ST_UNSIGNED;
    case SA_Type:
        return ST_TYPE;
    case SA_BitMap:
        return ST_BITMAP;
    case SA_Font:
        return ST_FONT;
    case SA_Title:
        return ST_TITLE;
    case SA_Pens:
        return ST_PENS;
    case SA_ErrorCode:
        return ST_ERROR;
    case SA_PubName:
        return ST_PUBNAME;
    case SA_DClip:
        return ST_DCLIP;
    default:
        /* SA_Colors, SA_Colors32, SA_PubTask, SA_PubSig, SA_BackFill,
           SA_VideoControl, SA_Parent, SA_FrontChild, SA_BackChild and
           anything unknown */
        return ST_REFUSED;
    }
}

/* One OpenScreen() or OpenScreenTagList(): the guest values that count,
   as Intuition takes them (the last SA_Font, SA_Title and SA_Pens over the
   NewScreen, the first SA_ErrorCode and SA_PubName), and their native
   copies for the call */
struct ScreenOpen
{
    ULONG list, items;
    ULONG font, title, pens, error, pubname, dclip;
    BOOL error_tag, pubname_tag;
    ULONG error_value;
    UWORD pen_array[MAX_PENS + 1];
    struct KaffeineFontAttr font_in;
    struct Rectangle clip;
};

/* Stop the program for input of OpenScreen() or OpenScreenTagList() that
   the bridge does not take */
static IPTR open_refuse(struct KaffeineContext *ctx, const char *call, const char *why)
{
    bug("[kaffeine] %s: %s, stopping (20)\n", call, why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* No screen: the error goes to SA_ErrorCode of the program */
static IPTR open_failed(struct KaffeineContext *ctx, struct ScreenOpen *o, ULONG error)
{
    if (o->error)
        m68k_write32(ctx, o->error, error);
    return 0;
}

/* A string that ends inside guest memory within max characters */
static BOOL bounded_string(struct KaffeineContext *ctx, ULONG addr, ULONG max)
{
    ULONG room;

    if (!guest_range(ctx, addr, 1))
        return FALSE;
    room = ctx->mem_size - addr;
    return memchr(ctx->mem + addr, 0, room < max + 1 ? room : max + 1) != NULL;
}

/* The tags, walked once without writing anything: what they hold, and why
   the bridge refuses them */
static const char *open_tags(struct KaffeineContext *ctx, struct ScreenOpen *o)
{
    struct KaffeineTagWalk walk;
    ULONG item, tag, data;
    int step;

    Kaffeine_TagWalkInit(&walk, o->list);
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        if (o->items == MAX_SCREEN_TAGS)
            return "more tags than the bridge takes";
        o->items++;
        tag = m68k_read32(ctx, item);
        data = m68k_read32(ctx, item + 4);
        switch (screen_tag(tag))
        {
        case ST_REFUSED:
            bug("[kaffeine] screen tag %08lx refused\n", (unsigned long)tag);
            return "a tag that the bridge does not convert";
        case ST_TYPE:
            if (data & CUSTOMBITMAP)
                return "SA_Type with CUSTOMBITMAP";
            break;
        case ST_BITMAP:
            if (data)
                return "SA_BitMap";
            break;
        case ST_FONT:
            o->font = data;
            break;
        case ST_TITLE:
            o->title = data;
            break;
        case ST_PENS:
            o->pens = data;
            break;
        case ST_DCLIP:
            o->dclip = data;
            break;
        case ST_ERROR:
            if (!o->error_tag)
                o->error = data;
            o->error_tag = TRUE;
            break;
        case ST_PUBNAME:
            if (!o->pubname_tag)
                o->pubname = data;
            o->pubname_tag = TRUE;
            break;
        }
    }
    return step == KAFFEINE_TAG_END ? NULL : "a tag list outside guest memory or without an end";
}

/* The guest values that count, checked and read into native storage */
static const char *open_values(struct KaffeineContext *ctx, struct ScreenOpen *o)
{
    ULONG i;

    if (o->font && (!Kaffeine_FontAttrIn(ctx, o->font, &o->font_in) ||
                    !bounded_string(ctx, m68k_read32(ctx, o->font), MAX_FONT_NAME)))
        return "a TextAttr that the bridge does not take";
    if (o->title && !bounded_string(ctx, o->title, MAX_TITLE))
        return "a title outside guest memory or longer than 255 characters";
    if (o->pubname && !bounded_string(ctx, o->pubname, MAXPUBSCREENNAME))
        return "SA_PubName outside guest memory or longer than MAXPUBSCREENNAME";
    if (o->error && !guest_range(ctx, o->error, 4))
        return "SA_ErrorCode outside guest memory";
    if (o->dclip)
    {
        if (!guest_range(ctx, o->dclip, 8))
            return "SA_DClip outside guest memory";
        o->clip.MinX = (WORD)m68k_read16(ctx, o->dclip);
        o->clip.MinY = (WORD)m68k_read16(ctx, o->dclip + 2);
        o->clip.MaxX = (WORD)m68k_read16(ctx, o->dclip + 4);
        o->clip.MaxY = (WORD)m68k_read16(ctx, o->dclip + 6);
    }
    if (o->pens)
    {
        for (i = 0; i <= MAX_PENS; i++)
        {
            if (!guest_range(ctx, o->pens + 2 * i, 2))
                return "SA_Pens outside guest memory";
            if ((o->pen_array[i] = m68k_read16(ctx, o->pens + 2 * i)) == (UWORD)~0)
                break;
        }
        if (i > MAX_PENS)
            return "SA_Pens without ~0 within 64 pens";
    }
    if (o->error)
        o->error_value = m68k_read32(ctx, o->error);
    return NULL;
}

/* The native copies that Intuition keeps while the screen is open */
static void open_keep(struct KaffeineContext *ctx, struct ScreenOpen *o,
                      struct KaffeineScreenKeep *keep)
{
    if (o->font)
    {
        keep->font = o->font_in.attr;
        strcpy(keep->font_name, (const char *)o->font_in.attr.tta_Name);
        keep->font.tta_Name = (STRPTR)keep->font_name;
        if (o->font_in.attr.tta_Tags)
        {
            memcpy(keep->font_tags, o->font_in.tags, sizeof(keep->font_tags));
            keep->font.tta_Tags = keep->font_tags;
        }
    }
    if (o->title)
        strcpy(keep->title, (const char *)ctx->mem + o->title);
}

/* The native tag list of the checked guest one: the same user tags in the
   same order, each with the native data of the value that counts */
static void open_native_tags(struct KaffeineContext *ctx, struct ScreenOpen *o,
                             struct KaffeineScreenKeep *keep, struct TagItem *tags)
{
    struct KaffeineTagWalk walk;
    ULONG item, data, n;

    Kaffeine_TagWalkInit(&walk, o->list);
    for (n = 0; n < o->items && Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM; n++)
    {
        tags[n].ti_Tag = m68k_read32(ctx, item);
        data = m68k_read32(ctx, item + 4);
        switch (screen_tag(tags[n].ti_Tag))
        {
        case ST_SIGNED:
            tags[n].ti_Data = (IPTR)(LONG)data;
            break;
        case ST_FONT:
            tags[n].ti_Data = o->font ? (IPTR)&keep->font : 0;
            break;
        case ST_TITLE:
            tags[n].ti_Data = o->title ? (IPTR)keep->title : 0;
            break;
        case ST_PENS:
            tags[n].ti_Data = o->pens ? (IPTR)o->pen_array : 0;
            break;
        case ST_DCLIP:
            /* OpenScreen copies the rectangle into its ViewPort extra. */
            tags[n].ti_Data = o->dclip ? (IPTR)&o->clip : 0;
            break;
        case ST_ERROR:
            tags[n].ti_Data = o->error ? (IPTR)&o->error_value : 0;
            break;
        case ST_PUBNAME:
            /* Intuition copies the name */
            tags[n].ti_Data = o->pubname ? (IPTR)(ctx->mem + o->pubname) : 0;
            break;
        default:
            tags[n].ti_Data = data;
            break;
        }
    }
    tags[n].ti_Tag = TAG_DONE;
}

/* OpenScreen() and OpenScreenTagList() of the program: ns is the 68k
   NewScreen or 0, list the 68k tags of OpenScreenTagList() or 0. */
static IPTR screen_open(struct KaffeineContext *ctx, ULONG ns, ULONG list, const char *call)
{
    struct ScreenOpen o;
    struct NewScreen native_ns;
    struct KaffeineScreenKeep *keep;
    struct TagItem *tags = NULL;
    struct Screen *scr;
    const char *why;
    UWORD type = 0;
    ULONG guest, font, colormap;
    int i;

    memset(&o, 0, sizeof(o));
    if (ns)
    {
        if (!guest_range(ctx, ns, M68K_SIZEOF_NEWSCREEN))
            return open_refuse(ctx, call, "a NewScreen outside guest memory");
        type = m68k_read16(ctx, ns + M68K_NS_TYPE);
        /* AROS copies an ExtNewScreen whole, also when A1 has the tags */
        if ((type & NS_EXTENDED) && !guest_range(ctx, ns, G_EXTNEWSCREEN_SIZE))
            return open_refuse(ctx, call, "an ExtNewScreen outside guest memory");
        if (m68k_read32(ctx, ns + G_NS_GADGETS))
            return open_refuse(ctx, call, "Gadgets in the NewScreen");
        if ((type & CUSTOMBITMAP) || m68k_read32(ctx, ns + G_NS_CUSTOMBITMAP))
            return open_refuse(ctx, call, "a CustomBitMap");
        o.font = m68k_read32(ctx, ns + G_NS_FONT);
        o.title = m68k_read32(ctx, ns + M68K_NS_DEFAULTTITLE);
        /* the tags of A1 go before the Extension */
        if (!list && (type & NS_EXTENDED))
            list = m68k_read32(ctx, ns + G_NS_EXTENSION);
    }
    o.list = list;
    if ((why = open_tags(ctx, &o)) || (why = open_values(ctx, &o)))
        return open_refuse(ctx, call, why);

    /* room for the 68k Screen before Intuition opens anything */
    if (screen_free_entry(ctx) < 0 || ctx->num_shadows > KAFFEINE_MAX_SHADOWS - 3)
    {
        bug("[kaffeine] %s: screen or shadow table full\n", call);
        return open_failed(ctx, &o, OSERR_NOMEM);
    }
    keep = AllocVec(sizeof(*keep), MEMF_ANY | MEMF_CLEAR);
    if (keep && o.items)
        tags = AllocVec((o.items + 1) * sizeof(*tags), MEMF_ANY | MEMF_CLEAR);
    if (!keep || (o.items && !tags))
    {
        FreeVec(tags);
        FreeVec(keep);
        return open_failed(ctx, &o, OSERR_NOMEM);
    }
    open_keep(ctx, &o, keep);
    if (tags)
        open_native_tags(ctx, &o, keep, tags);
    if (ns)
    {
        memset(&native_ns, 0, sizeof(native_ns));
        native_ns.LeftEdge = (WORD)m68k_read16(ctx, ns + M68K_NS_LEFTEDGE);
        native_ns.TopEdge = (WORD)m68k_read16(ctx, ns + M68K_NS_TOPEDGE);
        native_ns.Width = (WORD)m68k_read16(ctx, ns + M68K_NS_WIDTH);
        native_ns.Height = (WORD)m68k_read16(ctx, ns + M68K_NS_HEIGHT);
        native_ns.Depth = (WORD)m68k_read16(ctx, ns + M68K_NS_DEPTH);
        native_ns.DetailPen = ctx->mem[ns + M68K_NS_DETAILPEN];
        native_ns.BlockPen = ctx->mem[ns + M68K_NS_BLOCKPEN];
        native_ns.ViewModes = m68k_read16(ctx, ns + M68K_NS_VIEWMODES);
        /* the tags go to Intuition as its own argument */
        native_ns.Type = type & ~NS_EXTENDED;
        native_ns.Font = o.font ? (struct TextAttr *)&keep->font : NULL;
        native_ns.DefaultTitle = o.title ? (UBYTE *)keep->title : NULL;
    }
    scr = OpenScreenTagList(ns ? &native_ns : NULL, tags);
    FreeVec(tags);
    if (!scr)
    {
        FreeVec(keep);
        /* the error of Intuition, or what the program had there */
        return open_failed(ctx, &o, o.error_value);
    }
    if (!(guest = screen_create(ctx, scr, &font, &colormap, KAFFEINE_SHADOW_SCREEN)))
    {
        bug("[kaffeine] %s: no 68k Screen for %p, closed again\n", call, scr);
        if (CloseScreen(scr))
            FreeVec(keep);
        else
            bug("[kaffeine] %s: Intuition did not close %p again\n", call, scr);
        return open_failed(ctx, &o, OSERR_NOMEM);
    }
    screen_fill(ctx, guest, font, colormap, scr);
    /* the program's own text, as Intuition keeps the pointer it got */
    m68k_write32(ctx, guest + M68K_SCR_TITLE, o.title);
    m68k_write32(ctx, guest + G_SCR_DEFAULTTITLE, o.title);
    i = screen_free_entry(ctx);
    ctx->pubscreens[i].native = scr;
    ctx->pubscreens[i].guest = guest;
    ctx->pubscreens[i].font = font;
    ctx->pubscreens[i].colormap = colormap;
    ctx->pubscreens[i].locks = 0;
    ctx->pubscreens[i].owned = TRUE;
    ctx->pubscreens[i].keep = keep;
    screen_pens_clear(ctx, i);
    ctx->screen_release = screen_release;
    if (ctx->layer_ops)
        ctx->layer_ops->screen_mirrored(ctx, scr);
    return guest;
}

/* intuition -198: OpenScreen(A0=newScreen) -> D0: the 68k Screen of the
   screen that Intuition opened, or 0; 0 for NULL, which Intuition would
   read. As OpenScreenTagList() with the tags of an ExtNewScreen. */
IPTR Kaffeine_OpenScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG ns = THUNK_A(0);

    return ns ? screen_open(ctx, ns, 0, "OpenScreen()") : 0;
}

/* intuition -612: OpenScreenTagList(A0=newScreen, A1=tagList) -> D0: the
   68k Screen of the screen that Intuition opened, or 0. Intuition takes
   the defaults of a NULL NewScreen, the tags of A1 over the Extension of
   an ExtNewScreen, and each tag over the NewScreen. 0 also when the bridge
   has no memory or room for the 68k Screen; then the screen closes again
   and SA_ErrorCode gets OSERR_NOMEM. */
IPTR Kaffeine_OpenScreenTagList(struct KaffeineContext *ctx, void *cpu)
{
    return screen_open(ctx, THUNK_A(0), THUNK_A(1), "OpenScreenTagList()");
}

/* intuition -66: CloseScreen(A0=screen) -> D0: TRUE when the screen closed,
   and for NULL, as from Intuition; FALSE when Intuition refuses, as with
   windows or visitors on it, and then the 68k Screen stays as it was.
   Intuition gets only a screen that the program opened and has not closed,
   and of which it holds no VisualInfo, DrawInfo copy or pen of its
   ColorMap; anything else gives FALSE, changing nothing, so the program
   releases its pens first. While a BltBitMap() of the program runs, which
   may have the BitMap of the screen, the program stops instead. */
IPTR Kaffeine_CloseScreen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG screen = THUNK_A(0);
    int i;

    if (!screen)
        return TRUE;
    if (ctx->bitmap_blits)
        return refuse(ctx, "CloseScreen(): during a blit");
    i = screen_index(ctx, NULL, screen);
    if (i < 0 || !ctx->pubscreens[i].owned)
    {
        bug("[kaffeine] CloseScreen(%08lx): not a screen that the program opened\n",
            (unsigned long)screen);
        return FALSE;
    }
    /* a VisualInfo, a DrawInfo copy or a pen lease of the program still
       uses it */
    if (screen_visuals(ctx, ctx->pubscreens[i].native) ||
        screen_drawinfos(ctx, ctx->pubscreens[i].native) || ctx->pubscreens[i].pens)
        return FALSE;
    /* no 68k Layer may stand for a layer of a screen that closes; when
       Intuition refuses, the next snapshot is taken afresh */
    if (ctx->layer_ops)
        ctx->layer_ops->screen_closing(ctx, ctx->pubscreens[i].native);
    if (!CloseScreen(ctx->pubscreens[i].native))
    {
        if (ctx->layer_ops)
            ctx->layer_ops->screen_mirrored(ctx, ctx->pubscreens[i].native);
        return FALSE;
    }
    screen_closed(ctx, i);
    return TRUE;
}

/* ── DrawInfo ────────────────────────────────────────────────────── */

/* These generated consumers would read a classic guest DrawInfo as a
   native object. Images and pointer-valued attributes need their own
   conversions; until then, stop before either native API is called. */
IPTR Kaffeine_DrawImageState(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "DrawImageState(): unsupported Image/DrawInfo conversion");
}

IPTR Kaffeine_GetDrawInfoAttr(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "GetDrawInfoAttr(): unsupported DrawInfo attribute conversion");
}

/* The entry of a DrawInfo copy, by its 68k address, or -1 */
static int drawinfo_index(struct KaffeineContext *ctx, ULONG guest)
{
    int k;

    for (k = 0; guest && k < KAFFEINE_MAX_DRAWINFOS; k++)
        if (ctx->drawinfos[k].guest == guest)
            return k;
    return -1;
}

/* The entry of the copy of that native DrawInfo of that screen, or -1 */
static int drawinfo_find(struct KaffeineContext *ctx, const struct Screen *scr,
                         const struct DrawInfo *dri)
{
    int k;

    for (k = 0; k < KAFFEINE_MAX_DRAWINFOS; k++)
        if (ctx->drawinfos[k].guest && ctx->drawinfos[k].screen == scr &&
            ctx->drawinfos[k].native == dri)
            return k;
    return -1;
}

/* A free entry, or -1 */
static int drawinfo_free_entry(struct KaffeineContext *ctx)
{
    int k;

    for (k = 0; k < KAFFEINE_MAX_DRAWINFOS; k++)
        if (!ctx->drawinfos[k].guest)
            return k;
    return -1;
}

/* A native visitor lock of the bridge on a screen that the program did not
   open, taken by its name in the list of public screens, as Intuition
   locks a public screen; FALSE for a screen that is not public there or is
   private, which the bridge cannot keep. Also for the pen leases of
   kaffeine_palette.c. */
BOOL Kaffeine_ScreenPublicHold(struct Screen *screen)
{
    struct List *list = LockPubScreenList();
    struct Screen *held = NULL;
    struct Node *node;
    ULONG n = 0;

    for (node = list->lh_Head; node->ln_Succ && n++ < MAX_LISTED; node = node->ln_Succ)
        if (((struct PubScreenNode *)node)->psn_Screen == screen && node->ln_Name)
        {
            /* Native Intuition increments the signed WORD without an
               overflow check. Inspect it while its list lock is held. */
            WORD visitors = ((struct PubScreenNode *)node)->psn_VisitorCount;

            if (visitors >= 0 && visitors < MAX_LOCKS)
                held = LockPubScreen((CONST_STRPTR)node->ln_Name);
            break;
        }
    UnlockPubScreenList();
    if (held && held != screen)
        UnlockPubScreen(NULL, held);
    return held == screen;
}

/* The classic 68k DrawInfo of a native one, in its cleared mirror of
   G_DRAWINFO_SIZE bytes and, when there are pens, room for pens + 1 UWORDs
   after it: the scalars, the pens as big-endian UWORDs ended by ~0, and
   the 68k TextFont of dri_Font. Intuition's Images, dri_Screen, dri_Prefs
   and its reserved fields are native and stay 0, as do the five reserved
   longwords of the classic DrawInfo. */
static void drawinfo_fill(struct KaffeineContext *ctx, ULONG guest, const struct DrawInfo *dri,
                          UWORD pens, ULONG font)
{
    ULONG at = guest + G_DRAWINFO_SIZE;
    UWORD n;

    m68k_write16(ctx, guest + G_DRI_VERSION, dri->dri_Version < G_DRI_CLASSIC_VERSION
                                             ? dri->dri_Version : G_DRI_CLASSIC_VERSION);
    m68k_write16(ctx, guest + G_DRI_NUMPENS, pens);
    if (dri->dri_Pens)
    {
        for (n = 0; n < pens; n++)
            m68k_write16(ctx, at + 2 * n, dri->dri_Pens[n]);
        m68k_write16(ctx, at + 2 * pens, (UWORD)~0);
        m68k_write32(ctx, guest + G_DRI_PENS, at);
    }
    m68k_write32(ctx, guest + G_DRI_FONT, font);
    m68k_write16(ctx, guest + G_DRI_DEPTH, dri->dri_Depth);
    m68k_write16(ctx, guest + G_DRI_RESOLUTION_X, dri->dri_Resolution.X);
    m68k_write16(ctx, guest + G_DRI_RESOLUTION_Y, dri->dri_Resolution.Y);
    m68k_write32(ctx, guest + G_DRI_FLAGS, dri->dri_Flags);
}

/* The last get of a copy is freed: the copy goes, then the 68k Screen when
   nothing else keeps it, while the visitor lock of the bridge still holds,
   then that lock. The 68k TextFont and the held native font stay for the
   program, as those of a screen do. */
static void drawinfo_retire(struct KaffeineContext *ctx, int k)
{
    struct Screen *scr = ctx->drawinfos[k].screen;
    BOOL locked = ctx->drawinfos[k].public_lock;

    Kaffeine_MirrorRelease(ctx, ctx->drawinfos[k].guest);
    memset(&ctx->drawinfos[k], 0, sizeof(ctx->drawinfos[k]));
    screen_release(ctx, scr);
    if (locked)
        UnlockPubScreen(NULL, scr);
}

/* At the end of the program, before any screen lock or font goes: every
   get that the program left reaches FreeScreenDrawInfo() as its pair, and
   every copy retires. Nothing is left for a second call. */
static void drawinfos_cleanup(struct KaffeineContext *ctx)
{
    ULONG left = 0;
    int k;

    for (k = 0; k < KAFFEINE_MAX_DRAWINFOS; k++)
    {
        if (!ctx->drawinfos[k].guest)
            continue;
        for (; ctx->drawinfos[k].refs; ctx->drawinfos[k].refs--, left++)
            FreeScreenDrawInfo(ctx->drawinfos[k].screen, ctx->drawinfos[k].native);
        drawinfo_retire(ctx, k);
    }
    if (left)
        bug("[kaffeine] %lu DrawInfo gets that the program left, freed\n", (unsigned long)left);
}

/* intuition -690: GetScreenDrawInfo(A0=screen) -> D0: the 68k copy of the
   native DrawInfo of a 68k Screen of the program, the same for each get of
   that DrawInfo until its last free. 0 for NULL, which Intuition would
   read, and when Intuition gives none; also 0, with Intuition's get freed
   again and nothing kept, when the table of 16 copies is full, the program
   holds 32767 gets of the copy, a screen that it did not open cannot be
   held by a visitor lock, or there is no memory for the copy, no hold of
   its font or no 68k TextFont of it. Any other screen stops the program
   before Intuition. */
IPTR Kaffeine_GetScreenDrawInfo(struct KaffeineContext *ctx, void *cpu)
{
    ULONG screen = THUNK_A(0), guest = 0, font = 0, size;
    struct Screen *scr = NULL;
    struct DrawInfo *dri;
    BOOL holding = FALSE;
    UWORD pens;
    int i, k, held = -1;

    if (!screen)
        return 0;
    i = screen_index(ctx, NULL, screen);
    if (i < 0 || !(scr = ctx->pubscreens[i].native) || Kaffeine_ScreenNative(ctx, screen) != scr)
        return refuse(ctx, "GetScreenDrawInfo(): not a screen of the program");
    if (!(dri = GetScreenDrawInfo(scr)))
        return 0;
    if ((k = drawinfo_find(ctx, scr, dri)) >= 0)
    {
        if (ctx->drawinfos[k].refs == MAX_DRAWINFO_GETS)
        {
            bug("[kaffeine] GetScreenDrawInfo(): too many gets of %p, given back\n", dri);
            FreeScreenDrawInfo(scr, dri);
            return 0;
        }
        ctx->drawinfos[k].refs++;
        return ctx->drawinfos[k].guest;
    }
    pens = !dri->dri_Pens ? 0 : dri->dri_NumPens < MAX_DRAWINFO_PENS ? dri->dri_NumPens
                                                                     : MAX_DRAWINFO_PENS;
    size = G_DRAWINFO_SIZE;
    if (dri->dri_Pens)
        size += 2 * ((ULONG)pens + 1);
    k = drawinfo_free_entry(ctx);
    /* everything that can fail before anything is published */
    if (k < 0)
        bug("[kaffeine] DrawInfo table full, no copy of %p\n", dri);
    else if (!ctx->pubscreens[i].owned && !(holding = Kaffeine_ScreenPublicHold(scr)))
        bug("[kaffeine] GetScreenDrawInfo(): screen %p cannot be held, no copy\n", scr);
    else if (!(guest = Kaffeine_MirrorAlloc(ctx, size)))
        bug("[kaffeine] GetScreenDrawInfo(): no guest memory for the copy\n");
    /* the native font first, so that no 68k TextFont stands for a font
       that the bridge does not hold */
    else if (!font_hold(ctx, dri->dri_Font, &held))
        bug("[kaffeine] GetScreenDrawInfo(): font %p not held, no copy\n", dri->dri_Font);
    else if (dri->dri_Font && !(font = Kaffeine_FontGuest(ctx, dri->dri_Font)))
    {
        bug("[kaffeine] GetScreenDrawInfo(): no 68k TextFont for %p, no copy\n", dri->dri_Font);
        font_unhold(ctx, held);
    }
    else
    {
        memset(ctx->mem + guest, 0, size);
        drawinfo_fill(ctx, guest, dri, pens, font);
        /* the 68k TextFont stays for the program, as that of a screen */
        for (i = 0; font && i < KAFFEINE_MAX_FONTS; i++)
            if (ctx->fonts[i].guest == font)
                ctx->fonts[i].pinned = TRUE;
        ctx->drawinfos[k].screen = scr;
        ctx->drawinfos[k].native = dri;
        ctx->drawinfos[k].guest = guest;
        ctx->drawinfos[k].refs = 1;
        ctx->drawinfos[k].public_lock = holding;
        return guest;
    }
    /* nothing of it stays; Intuition's get goes back while the screen is
       still held */
    Kaffeine_MirrorRelease(ctx, guest);
    FreeScreenDrawInfo(scr, dri);
    if (holding)
        UnlockPubScreen(NULL, scr);
    return 0;
}

/* intuition -696: FreeScreenDrawInfo(A0=screen, A1=drawInfo): one get of
   the copy goes back to Intuition as the native pair, and the copy retires
   with the last one. NULL for drawInfo changes nothing, as it gives
   Intuition nothing to free. A drawInfo that is no copy the program holds,
   also one that it has freed, or a screen that is not the 68k Screen of
   the copy stops the program before Intuition. */
IPTR Kaffeine_FreeScreenDrawInfo(struct KaffeineContext *ctx, void *cpu)
{
    ULONG screen = THUNK_A(0), drawinfo = THUNK_A(1);
    int i, k;

    if (!drawinfo)
        return 0;
    if ((k = drawinfo_index(ctx, drawinfo)) < 0)
        return refuse(ctx, "FreeScreenDrawInfo(): not a DrawInfo that the program holds");
    i = screen_index(ctx, ctx->drawinfos[k].screen, 0);
    if (i < 0 || ctx->pubscreens[i].guest != screen)
        return refuse(ctx, "FreeScreenDrawInfo(): not the screen of that DrawInfo");
    FreeScreenDrawInfo(ctx->drawinfos[k].screen, ctx->drawinfos[k].native);
    if (!--ctx->drawinfos[k].refs)
        drawinfo_retire(ctx, k);
    return 0;
}

/* ── The end of the program ──────────────────────────────────────── */

void Kaffeine_ScreensCleanup(struct KaffeineContext *ctx)
{
    ULONG left = 0;
    int i;

    /* End read-only VPE borrows before any final native screen hold goes. */
    if (ctx->video_screen_retire)
        for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; ++i)
            if (ctx->pubscreens[i].native)
                ctx->video_screen_retire(ctx, ctx->pubscreens[i].native);
    /* the pens first, while every ColorMap of them is still there: the
       program's own screens and locks, and the visitor locks of the leases,
       go back only after them */
    Kaffeine_PensCleanup(ctx);
    /* the DrawInfo copies, which keep 68k Screens, visitor locks and
       the screens that the program opened */
    drawinfos_cleanup(ctx);
    /* every layer snapshot goes before the locks that keep screens open */
    if (ctx->layer_ops)
        ctx->layer_ops->screen_closing(ctx, NULL);
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
    /* after the locks, which make the program a visitor; a screen with a
       window of the program stays open, as the window does */
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].owned)
        {
            if (screen_windows(ctx, ctx->pubscreens[i].native))
                bug("[kaffeine] screen %p that the program left has its windows, left open\n",
                    ctx->pubscreens[i].native);
            else if (CloseScreen(ctx->pubscreens[i].native))
            {
                bug("[kaffeine] screen %p that the program left, closed\n", ctx->pubscreens[i].native);
                screen_closed(ctx, i);
            }
            else
                bug("[kaffeine] screen %p that the program left, Intuition did not close it\n",
                    ctx->pubscreens[i].native);
        }
    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->pubscreen_fonts[i])
        {
            CloseFont(ctx->pubscreen_fonts[i]);
            ctx->pubscreen_fonts[i] = NULL;
        }
}
