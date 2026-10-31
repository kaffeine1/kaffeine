/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    GadTools menus of 68k code. CreateMenusA() reads the 68k NewMenu array
    and its tags, lets native GadTools build the strip from native entries
    whose strings stay in guest memory, and mirrors the native strip in
    guest memory: a 68k Menu with the UserData of GadTools after it for
    each Menu, a 68k MenuItem with its UserData for each item, 68k
    IntuiTexts with their TextAttrs, a 68k Image for each separator, and
    guest copies of the strings that GadTools adds itself, such as the mark
    of a submenu. Each strip keeps its own table of native objects and
    their 68k mirrors, so a menu of any size takes no entry of the shadow
    table.

    The mirror follows the native strip after every call that may change
    it, and before an IDCMP_MENUPICK, IDCMP_MENUVERIFY or IDCMP_MENUHELP
    message reaches the program, so that CHECKED and NextSelect are right.
    The CHECKED and ITEMENABLED flags of items and MENUENABLED of menus that
    the program sets itself go to the native strip before SetMenuStrip(),
    ResetMenuStrip() and the layout calls, as ResetMenuStrip() allows; the
    UserData stays the program's. A VisualInfo is a small 68k handle with
    the native one behind it, which DrawBevelBoxA() also draws with.

    A strip that the program built itself stays the program's: its Menus
    and MenuItems are what ItemAddress() returns and what the program
    reads. SetMenuStrip() checks all of it and gives Intuition a native
    copy of it, with copies of its texts, strings and fonts, and of its
    images, whose data stays in guest memory. Windows that show the same
    strip share the copy; once none shows it, it waits for a
    ResetMenuStrip(), one of the last few to wait. Intuition writes in
    its copy as AROS does in any strip, so the program's own structures
    keep what it put there. CHECKED, ITEMENABLED and MENUENABLED pass
    between the two by bit, each way only what one side changed since both
    last agreed, so that a change of the program that has not reached
    Intuition yet stays; the windows that show the copy let it go while the
    program's changes go into it. NextSelect is Intuition's, which the
    program gets, the items of a menu pick in any case.
*/

#include "kaffeine_intern.h"
#include "kaffeine_copy.h"
#include "kaffeine_menus.h"
#include "kaffeine_mirror.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_taglist.h"

#include <aros/debug.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k structures of the NDK */
enum
{
    G_NM_TYPE = 0, G_NM_LABEL = 2, G_NM_COMMKEY = 6, G_NM_FLAGS = 10,
    G_NM_MUTUALEXCLUDE = 12, G_NM_USERDATA = 16, G_NEWMENU_SIZE = 20,

    /* Menu, then the UserData of GadTools */
    G_MU_NEXTMENU = 0, G_MU_LEFTEDGE = 4, G_MU_TOPEDGE = 6, G_MU_WIDTH = 8,
    G_MU_HEIGHT = 10, G_MU_FLAGS = 12, G_MU_MENUNAME = 14, G_MU_FIRSTITEM = 18,
    G_MU_JAZZX = 22, G_MU_JAZZY = 24, G_MU_BEATX = 26, G_MU_BEATY = 28,
    G_MU_USERDATA = 30, G_MENU_SIZE = 34, G_PLAIN_MENU_SIZE = 30,

    /* MenuItem, then the UserData of GadTools */
    G_MI_NEXTITEM = 0, G_MI_LEFTEDGE = 4, G_MI_TOPEDGE = 6, G_MI_WIDTH = 8,
    G_MI_HEIGHT = 10, G_MI_FLAGS = 12, G_MI_MUTUALEXCLUDE = 14, G_MI_ITEMFILL = 18,
    G_MI_SELECTFILL = 22, G_MI_COMMAND = 26, G_MI_SUBITEM = 28, G_MI_NEXTSELECT = 32,
    G_MI_USERDATA = 34, G_MENUITEM_SIZE = 38, G_PLAIN_MENUITEM_SIZE = 34,

    G_VISUALINFO_SIZE = 4
};

#define G_BARLABEL 0xFFFFFFFFu
/* Longer lists than this are no list a program means. */
#define MAX_ENTRIES 4096
#define MAX_CHAIN 1024
#define MAX_TAGS 512

/* The objects of a strip with a 68k copy of their own kind; the shared
   helpers of kaffeine_mirror.c copy texts, fonts, images and strings. */
enum { NODE_MENU, NODE_ITEM };

/* The flags that Intuition keeps for itself, without which the copy of a
   strip that the program built starts: AROS toggles HIGHITEM of items
   and menus, and marks an item with subitems with 0x8000, which the NDK
   reserves as MIF_EXTENDED. */
#define ITEM_INTUITION (ISDRAWN | HIGHITEM | MENUTOGGLED | 0x8000)
#define MENU_INTUITION (MIDRAWN | HIGHITEM)
/* The flags that both the program and Intuition change */
#define ITEM_SHARED (CHECKED | ITEMENABLED)
#define MENU_SHARED MENUENABLED

/* A Menu or a MenuItem of a strip that the program built, its native
   copy, and the shared flags and NextSelect on which both last agreed */
struct KaffeineMenuNode
{
    APTR native;
    ULONG guest;
    UWORD flags, select;
    UBYTE kind;
};

struct KaffeineMenuStrip
{
    struct KaffeineMenuStrip *next;
    APTR root;
    BOOL root_is_item;  /* no NM_TITLE: items, as GTMN_FullMenu FALSE allows */
    ULONG guest_root;
    struct KaffeineMirror mirror;
    /* A strip that the program built: its nodes, and the same sorted by
       their copies, in one block with the strip and the native copies;
       NULL for a strip of CreateMenusA() */
    struct KaffeineMenuNode *nodes, **by_native;
    ULONG count;
    BOOL parked;    /* no window shows it: it waits for ResetMenuStrip() */
};

/* The copies that wait for ResetMenuStrip(), the most recent ones */
#define MAX_PARKED 8

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

static BOOL guest_string_ok(struct KaffeineContext *ctx, ULONG addr)
{
    return guest_range(ctx, addr, 1) && memchr(ctx->mem + addr, 0, ctx->mem_size - addr);
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

/* refuse() with the reason that a check of 68k input gave */
static IPTR refuse_input(struct KaffeineContext *ctx, const char *call, const char *why)
{
    bug("[kaffeine] %s: %s, stopping (20)\n", call, why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* ── VisualInfo ──────────────────────────────────────────────────── */

APTR Kaffeine_VisualInfoNative(struct KaffeineContext *ctx, ULONG guest)
{
    return Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_VISUALINFO);
}

/* Keep the borrowed screen alive independently of the guest's locks. */
static BOOL visual_public_hold(struct Screen *screen)
{
    struct List *list = LockPubScreenList();
    struct Node *node;
    struct Screen *held = NULL;
    ULONG count = 0;
    for (node = list ? list->lh_Head : NULL; node && node->ln_Succ && count++ < 256;
         node = node->ln_Succ)
    {
        struct PubScreenNode *pub = (struct PubScreenNode *)node;
        if (pub->psn_Screen == screen && node->ln_Name)
        {
            held = LockPubScreen((CONST_STRPTR)node->ln_Name);
            break;
        }
    }
    UnlockPubScreenList();
    if (held && held != screen)
        UnlockPubScreen(NULL, held);
    return held == screen;
}

static void visual_release(struct KaffeineContext *ctx, int i)
{
    APTR native = ctx->visualinfos[i].native;
    struct Screen *screen = ctx->visualinfos[i].screen;
    ULONG guest = ctx->visualinfos[i].guest;
    BOOL locked = ctx->visualinfos[i].public_lock;

    FreeVisualInfo(native);
    Kaffeine_ShadowForget(ctx, guest, KAFFEINE_SHADOW_VISUALINFO);
    Kaffeine_MirrorRelease(ctx, guest);
    memset(&ctx->visualinfos[i], 0, sizeof(ctx->visualinfos[i]));
    /* Forget guest snapshots while the native visitor lock still holds. */
    if (ctx->screen_release)
        ctx->screen_release(ctx, screen);
    if (locked)
        UnlockPubScreen(NULL, screen);
}

void Kaffeine_VisualInfosCleanup(struct KaffeineContext *ctx)
{
    int i;
    for (i = 0; i < KAFFEINE_MAX_VISUALINFOS; i++)
        if (ctx->visualinfos[i].native)
            visual_release(ctx, i);
}

/* gadtools -126: GetVisualInfoA(A0=screen, A1=tagList) -> D0: a 68k handle.
   GadTools takes no tags here. */
IPTR Kaffeine_GetVisualInfoA(struct KaffeineContext *ctx, void *cpu)
{
    struct Screen *scr = Kaffeine_ScreenNative(ctx, THUNK_A(0));
    APTR vi;
    ULONG guest;
    int slot;
    BOOL locked;

    if (!THUNK_A(0))
        return 0;   /* as GadTools for a screen that could not be locked */
    if (!scr)
        return refuse(ctx, "GetVisualInfoA(): not a screen of the program");
    for (slot = 0; slot < KAFFEINE_MAX_VISUALINFOS && ctx->visualinfos[slot].native; slot++)
        ;
    if (slot == KAFFEINE_MAX_VISUALINFOS)
        return 0;
    locked = !Kaffeine_ShadowNative(ctx, THUNK_A(0), KAFFEINE_SHADOW_SCREEN);
    if (locked && !visual_public_hold(scr))
        return 0;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi)
    {
        if (locked) UnlockPubScreen(NULL, scr);
        return 0;
    }
    guest = Kaffeine_MirrorAlloc(ctx, G_VISUALINFO_SIZE);
    if (guest && Kaffeine_ShadowAdd(ctx, guest, vi, KAFFEINE_SHADOW_VISUALINFO))
    {
        ctx->visualinfos[slot].native = vi;
        ctx->visualinfos[slot].screen = scr;
        ctx->visualinfos[slot].guest = guest;
        ctx->visualinfos[slot].public_lock = locked;
        return guest;
    }
    bug("[kaffeine] GetVisualInfoA(): no guest memory for the handle\n");
    if (guest)
        Kaffeine_MirrorRelease(ctx, guest);
    FreeVisualInfo(vi);
    if (locked) UnlockPubScreen(NULL, scr);
    return 0;
}

/* gadtools -132: FreeVisualInfo(A0=vi) */
IPTR Kaffeine_FreeVisualInfo(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    int i;
    for (i = 0; i < KAFFEINE_MAX_VISUALINFOS; i++)
        if (ctx->visualinfos[i].native && ctx->visualinfos[i].guest == guest)
        {
            visual_release(ctx, i);
            return 0;
        }
    if (guest)
        bug("[kaffeine] FreeVisualInfo(%08lx): not an owned VisualInfo, ignored\n", (unsigned long)guest);
    return 0;
}

/* gadtools -120: DrawBevelBoxA(A0=rport, D0=left, D1=top, D2=width,
   D3=height, A1=taglist). GadTools reads the first GT_VisualInfo,
   GTBB_Recessed and GTBB_FrameType of the list, ignores any other tag,
   and draws nothing without a VisualInfo; the bridge passes just those
   three, the VisualInfo as the native one behind its 68k handle. The
   geometry is LONG in the NDK and WORD in AROS. */
IPTR Kaffeine_DrawBevelBoxA(struct KaffeineContext *ctx, void *cpu)
{
    struct TagItem tags[4];
    struct KaffeineTagWalk walk;
    struct RastPort *rp;
    ULONG item, tag, data, seen = 0;
    APTR vi = NULL;
    int n = 0, step;

    Kaffeine_TagWalkInit(&walk, THUNK_A(1));
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        tag = m68k_read32(ctx, item);
        data = m68k_read32(ctx, item + 4);
        if (tag == GT_VisualInfo && !(seen & 1))
        {
            seen |= 1;
            if (data && !(vi = Kaffeine_VisualInfoNative(ctx, data)))
                return refuse(ctx, "DrawBevelBoxA(): a GT_VisualInfo that GetVisualInfoA() did not return");
            tags[n].ti_Tag = GT_VisualInfo;
            tags[n++].ti_Data = (IPTR)vi;
        }
        else if (tag == GTBB_Recessed && !(seen & 2))
        {
            seen |= 2;
            tags[n].ti_Tag = GTBB_Recessed;
            tags[n++].ti_Data = data;
        }
        else if (tag == GTBB_FrameType && !(seen & 4))
        {
            seen |= 4;
            tags[n].ti_Tag = GTBB_FrameType;
            tags[n++].ti_Data = data;
        }
    }
    if (step == KAFFEINE_TAG_BAD)
        return refuse(ctx, "DrawBevelBoxA(): a tag list outside guest memory or in a cycle");
    tags[n].ti_Tag = TAG_DONE;
    if (!vi)
        return 0;   /* as GadTools, which looks at nothing else then */
    if (!(rp = Kaffeine_RastPortIn(ctx, THUNK_A(0))))
        return refuse(ctx, "DrawBevelBoxA(): not the RastPort of a window or a screen");
    DrawBevelBoxA(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1), (WORD)THUNK_D(2), (WORD)THUNK_D(3), tags);
    return 0;
}

/* ── Strips ──────────────────────────────────────────────────────── */

static ULONG mirror_fill(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s,
                         struct MenuItem *mi, APTR fill)
{
    if (!fill)
        return 0;
    return (mi->Flags & ITEMTEXT) ? Kaffeine_MirrorTexts(ctx, &s->mirror, fill)
                                  : Kaffeine_MirrorImage(ctx, &s->mirror, fill);
}

static ULONG mirror_items(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s,
                          struct MenuItem *mi, BOOL subitems)
{
    ULONG first = 0, prev = 0, guest, n;
    BOOL fresh;

    for (n = 0; mi && n < MAX_CHAIN; mi = mi->NextItem, n++)
    {
        guest = Kaffeine_MirrorNode(ctx, &s->mirror, mi, NODE_ITEM, G_MENUITEM_SIZE, &fresh);
        if (!guest)
            break;
        m68k_write32(ctx, guest + G_MI_NEXTITEM, 0);
        m68k_write16(ctx, guest + G_MI_LEFTEDGE, (UWORD)mi->LeftEdge);
        m68k_write16(ctx, guest + G_MI_TOPEDGE, (UWORD)mi->TopEdge);
        m68k_write16(ctx, guest + G_MI_WIDTH, (UWORD)mi->Width);
        m68k_write16(ctx, guest + G_MI_HEIGHT, (UWORD)mi->Height);
        m68k_write16(ctx, guest + G_MI_FLAGS, mi->Flags);
        m68k_write32(ctx, guest + G_MI_MUTUALEXCLUDE, (ULONG)mi->MutualExclude);
        m68k_write32(ctx, guest + G_MI_ITEMFILL, mirror_fill(ctx, s, mi, mi->ItemFill));
        m68k_write32(ctx, guest + G_MI_SELECTFILL, mirror_fill(ctx, s, mi, mi->SelectFill));
        ctx->mem[guest + G_MI_COMMAND] = (UBYTE)mi->Command;
        /* Intuition has no submenus of submenus. */
        m68k_write32(ctx, guest + G_MI_SUBITEM,
                     !subitems && mi->SubItem ? mirror_items(ctx, s, mi->SubItem, TRUE) : 0);
        m68k_write16(ctx, guest + G_MI_NEXTSELECT, mi->NextSelect);
        /* The UserData is the program's: GadTools and Intuition never
           change it, the program may. */
        if (fresh)
            m68k_write32(ctx, guest + G_MI_USERDATA, (ULONG)(IPTR)GTMENUITEM_USERDATA(mi));
        if (prev)
            m68k_write32(ctx, prev + G_MI_NEXTITEM, guest);
        else
            first = guest;
        prev = guest;
    }
    return first;
}

static ULONG mirror_menus(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s,
                          struct Menu *m)
{
    ULONG first = 0, prev = 0, guest, n;
    BOOL fresh;

    for (n = 0; m && n < MAX_CHAIN; m = m->NextMenu, n++)
    {
        guest = Kaffeine_MirrorNode(ctx, &s->mirror, m, NODE_MENU, G_MENU_SIZE, &fresh);
        if (!guest)
            break;
        m68k_write32(ctx, guest + G_MU_NEXTMENU, 0);
        m68k_write16(ctx, guest + G_MU_LEFTEDGE, (UWORD)m->LeftEdge);
        m68k_write16(ctx, guest + G_MU_TOPEDGE, (UWORD)m->TopEdge);
        m68k_write16(ctx, guest + G_MU_WIDTH, (UWORD)m->Width);
        m68k_write16(ctx, guest + G_MU_HEIGHT, (UWORD)m->Height);
        m68k_write16(ctx, guest + G_MU_FLAGS, m->Flags);
        m68k_write32(ctx, guest + G_MU_MENUNAME,
                     Kaffeine_MirrorString(ctx, &s->mirror, (const char *)m->MenuName));
        m68k_write32(ctx, guest + G_MU_FIRSTITEM, mirror_items(ctx, s, m->FirstItem, FALSE));
        m68k_write16(ctx, guest + G_MU_JAZZX, (UWORD)m->JazzX);
        m68k_write16(ctx, guest + G_MU_JAZZY, (UWORD)m->JazzY);
        m68k_write16(ctx, guest + G_MU_BEATX, (UWORD)m->BeatX);
        m68k_write16(ctx, guest + G_MU_BEATY, (UWORD)m->BeatY);
        if (fresh)
            m68k_write32(ctx, guest + G_MU_USERDATA, (ULONG)(IPTR)GTMENU_USERDATA(m));
        if (prev)
            m68k_write32(ctx, prev + G_MU_NEXTMENU, guest);
        else
            first = guest;
        prev = guest;
    }
    return first;
}

/* Bring the whole 68k mirror up to date with the native strip. */
static void sync_out(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    s->guest_root = s->root_is_item ? mirror_items(ctx, s, s->root, FALSE)
                                    : mirror_menus(ctx, s, s->root);
    if (s->mirror.failed)
        bug("[kaffeine] menus: no guest memory for a part of the 68k mirror\n");
}

/* The flags that the program may set itself, to the native strip. */
static void sync_in(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    ULONG i;

    for (i = 0; i < s->mirror.count; i++)
    {
        struct KaffeineMirrorNode *n = &s->mirror.nodes[i];

        if (n->kind == NODE_ITEM)
        {
            struct MenuItem *mi = n->native;
            UWORD flags = m68k_read16(ctx, n->guest + G_MI_FLAGS);

            mi->Flags = (mi->Flags & ~(CHECKED | ITEMENABLED)) | (flags & (CHECKED | ITEMENABLED));
        }
        else if (n->kind == NODE_MENU)
        {
            struct Menu *m = n->native;
            UWORD flags = m68k_read16(ctx, n->guest + G_MU_FLAGS);

            m->Flags = (m->Flags & ~MENUENABLED) | (flags & MENUENABLED);
        }
    }
}

static void strip_free(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    struct KaffeineMenuStrip **link;

    for (link = &ctx->menus; *link; link = &(*link)->next)
        if (*link == s)
        {
            *link = s->next;
            break;
        }
    Kaffeine_MirrorFree(ctx, &s->mirror);
    FreeVec(s);
}

/* The strip whose 68k root is guest */
static struct KaffeineMenuStrip *strip_by_root(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineMenuStrip *s;

    for (s = ctx->menus; s && guest; s = s->next)
        if (s->guest_root == guest)
            return s;
    return NULL;
}

/* The strip with a 68k mirror of that kind at guest, and its native object */
static struct KaffeineMenuStrip *strip_by_node(struct KaffeineContext *ctx, ULONG guest,
                                              UBYTE kind, void **native)
{
    struct KaffeineMenuStrip *s;
    struct KaffeineMirrorNode *n;

    for (s = ctx->menus; s && guest; s = s->next)
        if ((n = Kaffeine_MirrorGuest(&s->mirror, guest, kind)))
        {
            *native = n->native;
            return s;
        }
    return NULL;
}

static struct KaffeineMenuStrip *strip_by_native(struct KaffeineContext *ctx, APTR root)
{
    struct KaffeineMenuStrip *s;

    for (s = ctx->menus; s && root; s = s->next)
        if (s->root == root)
            return s;
    return NULL;
}

/* The strip of CreateMenusA() whose 68k root is guest */
static struct KaffeineMenuStrip *gadtools_strip(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineMenuStrip *s = strip_by_root(ctx, guest);

    return s && !s->nodes ? s : NULL;
}

/* The windows of the program that show a native strip; with take, the
   strip comes off each of them. */
static ULONG windows_showing(struct KaffeineContext *ctx, APTR root, BOOL take)
{
    ULONG n = 0;
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].type == KAFFEINE_SHADOW_WINDOW)
        {
            struct Window *w = ctx->shadow_map[i].native_ptr;

            if (!root || w->MenuStrip != root)
                continue;
            n++;
            if (take)
            {
                ClearMenuStrip(w);
                m68k_write32(ctx, ctx->shadow_map[i].m68k_addr + M68K_WIN_MENUSTRIP, 0);
            }
        }
    return n;
}

/* ── Strips that the program built ───────────────────────────────── */

/* Both runs of a copy of a strip (kaffeine_copy.h) */
struct strip_copy
{
    struct KaffeineCopy copy;
    struct KaffeineMenuNode *nodes;  /* the second run */
    ULONG count, room;              /* nodes so far, and the room for them */
};

static APTR copy_node(struct strip_copy *b, ULONG guest, UBYTE kind, ULONG size, UWORD flags,
                      UWORD select)
{
    APTR native = Kaffeine_CopyTake(&b->copy, size);

    if (native)
    {
        struct KaffeineMenuNode *n;

        if (b->count == b->room)
        {
            Kaffeine_CopyRefuse(&b->copy, "a copy larger than its count");
            return NULL;
        }
        n = &b->nodes[b->count];
        n->native = native;
        n->guest = guest;
        n->kind = kind;
        n->flags = flags & (kind == NODE_MENU ? MENU_SHARED : ITEM_SHARED);
        n->select = select;
    }
    b->count++;
    return native;
}

/* The items of a menu, or the subitems of an item: as many as a menu
   number can name, which also ends a chain in a cycle */
static struct MenuItem *copy_items(struct KaffeineContext *ctx, struct strip_copy *b, ULONG guest,
                                   BOOL subitems)
{
    struct MenuItem *first = NULL, *prev = NULL, *mi;
    ULONG n, fill, select, sub;
    APTR itemfill, selectfill;
    struct MenuItem *subitem;
    UWORD flags;

    for (n = 0; guest && !b->copy.why; n++, guest = m68k_read32(ctx, guest + G_MI_NEXTITEM))
    {
        if (n == (subitems ? NOSUB : NOITEM) || !guest_range(ctx, guest, G_PLAIN_MENUITEM_SIZE))
        {
            Kaffeine_CopyRefuse(&b->copy, "a MenuItem outside guest memory, or more items or "
                                         "subitems than a menu number names");
            break;
        }
        flags = m68k_read16(ctx, guest + G_MI_FLAGS) & ~ITEM_INTUITION;
        fill = m68k_read32(ctx, guest + G_MI_ITEMFILL);
        select = m68k_read32(ctx, guest + G_MI_SELECTFILL);
        /* Intuition follows the IntuiText of any text item. */
        if ((flags & ITEMTEXT) && !fill)
        {
            Kaffeine_CopyRefuse(&b->copy, "a text item without an IntuiText");
            break;
        }
        mi = copy_node(b, guest, NODE_ITEM, sizeof(*mi), flags,
                       m68k_read16(ctx, guest + G_MI_NEXTSELECT));
        if (flags & ITEMTEXT)
        {
            itemfill = Kaffeine_CopyTexts(ctx, &b->copy, fill);
            selectfill = Kaffeine_CopyTexts(ctx, &b->copy, select);
        }
        else
        {
            itemfill = Kaffeine_CopyImages(ctx, &b->copy, fill);
            selectfill = Kaffeine_CopyImages(ctx, &b->copy, select);
        }
        /* Intuition has no submenus of submenus. */
        sub = subitems ? 0 : m68k_read32(ctx, guest + G_MI_SUBITEM);
        subitem = sub ? copy_items(ctx, b, sub, TRUE) : NULL;
        if (!mi)
            continue;
        mi->LeftEdge = (WORD)m68k_read16(ctx, guest + G_MI_LEFTEDGE);
        mi->TopEdge = (WORD)m68k_read16(ctx, guest + G_MI_TOPEDGE);
        mi->Width = (WORD)m68k_read16(ctx, guest + G_MI_WIDTH);
        mi->Height = (WORD)m68k_read16(ctx, guest + G_MI_HEIGHT);
        mi->Flags = flags;
        mi->MutualExclude = (LONG)m68k_read32(ctx, guest + G_MI_MUTUALEXCLUDE);
        mi->ItemFill = itemfill;
        mi->SelectFill = selectfill;
        mi->Command = (BYTE)ctx->mem[guest + G_MI_COMMAND];
        mi->SubItem = subitem;
        mi->NextSelect = m68k_read16(ctx, guest + G_MI_NEXTSELECT);
        mi->NextItem = NULL;
        if (prev)
            prev->NextItem = mi;
        else
            first = mi;
        prev = mi;
    }
    return b->copy.why ? NULL : first;
}

static struct Menu *copy_menus(struct KaffeineContext *ctx, struct strip_copy *b, ULONG guest)
{
    struct Menu *first = NULL, *prev = NULL, *m;
    struct MenuItem *items;
    STRPTR name;
    ULONG n, at;
    UWORD flags;

    for (n = 0; guest && !b->copy.why; n++, guest = m68k_read32(ctx, guest + G_MU_NEXTMENU))
    {
        if (n == NOMENU || !guest_range(ctx, guest, G_PLAIN_MENU_SIZE))
        {
            Kaffeine_CopyRefuse(&b->copy, "a Menu outside guest memory, or more menus than a "
                                         "menu number names");
            break;
        }
        flags = m68k_read16(ctx, guest + G_MU_FLAGS) & ~MENU_INTUITION;
        /* Intuition measures every menu name. */
        at = m68k_read32(ctx, guest + G_MU_MENUNAME);
        name = Kaffeine_CopyString(ctx, &b->copy, at);
        if (!at)
            Kaffeine_CopyRefuse(&b->copy, "a menu without a name");
        m = copy_node(b, guest, NODE_MENU, sizeof(*m), flags, 0);
        items = copy_items(ctx, b, m68k_read32(ctx, guest + G_MU_FIRSTITEM), FALSE);
        if (!m)
            continue;
        /* JazzX, JazzY, BeatX and BeatY are Intuition's, which
           SetMenuStrip() works out in the copy. */
        m->LeftEdge = (WORD)m68k_read16(ctx, guest + G_MU_LEFTEDGE);
        m->TopEdge = (WORD)m68k_read16(ctx, guest + G_MU_TOPEDGE);
        m->Width = (WORD)m68k_read16(ctx, guest + G_MU_WIDTH);
        m->Height = (WORD)m68k_read16(ctx, guest + G_MU_HEIGHT);
        m->Flags = flags;
        m->MenuName = name;
        m->FirstItem = items;
        m->NextMenu = NULL;
        if (prev)
            prev->NextMenu = m;
        else
            first = m;
        prev = m;
    }
    return b->copy.why ? NULL : first;
}

typedef BOOL (*node_order)(const struct KaffeineMenuNode *, const struct KaffeineMenuNode *);

static BOOL node_by_guest(const struct KaffeineMenuNode *a, const struct KaffeineMenuNode *b)
{
    return a->guest < b->guest;
}

static BOOL node_by_native(const struct KaffeineMenuNode *a, const struct KaffeineMenuNode *b)
{
    return (IPTR)a->native < (IPTR)b->native;
}

static void sift(struct KaffeineMenuNode **a, ULONG root, ULONG n, node_order before)
{
    struct KaffeineMenuNode *t;
    ULONG child;

    while ((child = 2 * root + 1) < n)
    {
        if (child + 1 < n && before(a[child], a[child + 1]))
            child++;
        if (!before(a[root], a[child]))
            return;
        t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

/* A heap sort, n log n at worst, whatever order the program's strip has */
static void sort_nodes(struct KaffeineMenuNode **a, ULONG n, node_order before)
{
    struct KaffeineMenuNode *t;
    ULONG i;

    for (i = n / 2; i-- > 0;)
        sift(a, i, n, before);
    while (n > 1)
    {
        t = a[0];
        a[0] = a[--n];
        a[n] = t;
        sift(a, 0, n, before);
    }
}

/* A native copy of a strip that the program built, checked all over
   first; NULL with *why for a strip that the bridge refuses, else for no
   memory. */
static struct KaffeineMenuStrip *strip_copy(struct KaffeineContext *ctx, ULONG guest,
                                           const char **why)
{
    struct KaffeineMenuStrip *s;
    struct KaffeineMenuNode **index;
    struct strip_copy b;
    APTR block;
    ULONG nodes, i;

    /* the first run checks the strip and counts the room of its copies */
    memset(&b, 0, sizeof(b));
    Kaffeine_CopyTake(&b.copy, sizeof(*s));
    copy_menus(ctx, &b, guest);
    nodes = b.count;
    Kaffeine_CopyTake(&b.copy, nodes * sizeof(*b.nodes));
    Kaffeine_CopyTake(&b.copy, nodes * sizeof(*index));
    if ((*why = b.copy.why))
        return NULL;
    if (!(block = AllocVec(b.copy.used, MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    Kaffeine_CopyFill(&b.copy, block);
    s = Kaffeine_CopyTake(&b.copy, sizeof(*s));
    b.nodes = Kaffeine_CopyTake(&b.copy, nodes * sizeof(*b.nodes));
    index = Kaffeine_CopyTake(&b.copy, nodes * sizeof(*index));
    b.room = nodes;
    b.count = 0;
    s->root = copy_menus(ctx, &b, guest);
    /* A Menu or a MenuItem in two places of the strip would get two copies
       that Intuition changes apart. */
    for (i = 0; !b.copy.why && i < b.count; i++)
        index[i] = &b.nodes[i];
    if (!b.copy.why)
        sort_nodes(index, b.count, node_by_guest);
    for (i = 1; !b.copy.why && i < b.count; i++)
        if (index[i]->guest == index[i - 1]->guest)
            Kaffeine_CopyRefuse(&b.copy, "a Menu or a MenuItem in two places of the strip");
    if ((*why = b.copy.why))
    {
        FreeVec(block);
        return NULL;
    }
    sort_nodes(index, b.count, node_by_native);
    s->guest_root = guest;
    s->nodes = b.nodes;
    s->by_native = index;
    s->count = b.count;
    return s;
}

static UWORD *node_flags(struct KaffeineMenuNode *n, ULONG *at, UWORD *shared)
{
    if (n->kind == NODE_MENU)
    {
        *at = n->guest + G_MU_FLAGS;
        *shared = MENU_SHARED;
        return &((struct Menu *)n->native)->Flags;
    }
    *at = n->guest + G_MI_FLAGS;
    *shared = ITEM_SHARED;
    return &((struct MenuItem *)n->native)->Flags;
}

/* The node of a native Menu or MenuItem of the copy */
static struct KaffeineMenuNode *node_of(struct KaffeineMenuStrip *s, APTR native)
{
    ULONG lo = 0, hi = s->count, mid;

    while (native && lo < hi)
    {
        mid = lo + (hi - lo) / 2;
        if (s->by_native[mid]->native == native)
            return s->by_native[mid];
        if ((IPTR)s->by_native[mid]->native < (IPTR)native)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* What the program changed in the shared flags since both sides last
   agreed, to the copy. NextSelect stays Intuition's: it follows the
   NextSelect chain of a pick in its own copy. */
static void strip_in(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    ULONG i, at;
    UWORD *flags, shared, changed, guest;

    for (i = 0; i < s->count; i++)
    {
        struct KaffeineMenuNode *n = &s->nodes[i];

        flags = node_flags(n, &at, &shared);
        guest = m68k_read16(ctx, at);
        if ((changed = (guest ^ n->flags) & shared))
        {
            *flags = (*flags & ~changed) | (guest & changed);
            n->flags ^= changed;
        }
    }
}

/* strip_in() into a copy that windows may show: they let it go first, as
   the NDK has a program clear a strip before it changes it, which waits
   for a menu of Intuition that uses the copy, and get it back after. */
static void strip_import(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    UBYTE held[(KAFFEINE_MAX_SHADOWS + 7) / 8];
    UWORD i;

    memset(held, 0, sizeof(held));
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].type == KAFFEINE_SHADOW_WINDOW &&
            ((struct Window *)ctx->shadow_map[i].native_ptr)->MenuStrip == s->root)
        {
            ClearMenuStrip(ctx->shadow_map[i].native_ptr);
            held[i / 8] |= 1 << (i % 8);
        }
    strip_in(ctx, s);
    for (i = 0; i < ctx->num_shadows; i++)
        if (held[i / 8] & (1 << (i % 8)))
            ResetMenuStrip(ctx->shadow_map[i].native_ptr, s->root);
}

/* What Intuition changed in a Menu or MenuItem of the copy since both
   sides last agreed, to the program's */
static void node_out(struct KaffeineContext *ctx, struct KaffeineMenuNode *n)
{
    struct MenuItem *mi;
    ULONG at;
    UWORD *flags, shared, changed;

    flags = node_flags(n, &at, &shared);
    if ((changed = (*flags ^ n->flags) & shared))
    {
        m68k_write16(ctx, at, (m68k_read16(ctx, at) & ~changed) | (*flags & changed));
        n->flags ^= changed;
    }
    if (n->kind == NODE_ITEM && (mi = n->native)->NextSelect != n->select)
        m68k_write16(ctx, n->guest + G_MI_NEXTSELECT, n->select = mi->NextSelect);
}

/* node_out() of the whole strip; the items of a menu pick, from its code,
   get their NextSelect in any case. */
static void strip_out(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s, UWORD pick)
{
    struct KaffeineMenuNode *n;
    struct MenuItem *mi;
    ULONG i;

    for (i = 0; i < s->count; i++)
        node_out(ctx, &s->nodes[i]);
    for (i = 0; pick != MENUNULL && i < s->count; i++, pick = mi->NextSelect)
    {
        if (!(n = node_of(s, mi = ItemAddress(s->root, pick))))
            break;
        m68k_write16(ctx, n->guest + G_MI_NEXTSELECT, n->select = mi->NextSelect);
    }
}

/* A copy that no window shows waits for ResetMenuStrip(), so that the
   Clear, change, Reset of the NDK needs no new copy, as the most recent of
   at most MAX_PARKED; an older one goes. */
static void strip_park(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    struct KaffeineMenuStrip **link, *p, *next;
    ULONG n = 0;

    for (link = &ctx->menus; *link; link = &(*link)->next)
        if (*link == s)
        {
            *link = s->next;
            break;
        }
    s->next = ctx->menus;
    ctx->menus = s;
    s->parked = TRUE;
    for (p = ctx->menus; p; p = next)
    {
        next = p->next;
        if (p->parked && ++n > MAX_PARKED)
            strip_free(ctx, p);
    }
}

/* A strip that a window shows no more: what Intuition set goes to the
   program, and the copy of a strip that the program built waits once no
   window shows it. */
static void strip_left(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s)
{
    if (!s->nodes)
        sync_out(ctx, s);
    else
    {
        strip_out(ctx, s, MENUNULL);
        if (!windows_showing(ctx, s->root, FALSE))
            strip_park(ctx, s);
    }
}

/* Before a menu message reaches the program (kaffeine_window.c) */
static void menu_sync(struct KaffeineContext *ctx, struct IntuiMessage *im)
{
    struct Window *w = im->IDCMPWindow;
    struct KaffeineMenuStrip *s = w ? strip_by_native(ctx, w->MenuStrip) : NULL;

    if (s && s->nodes)
        strip_out(ctx, s, im->Class == IDCMP_MENUPICK ? im->Code : MENUNULL);
    else if (s)
        sync_out(ctx, s);
}

/* Before CloseWindow() and FreeSysRequest() (kaffeine_window.c,
   kaffeine_requester.c): the strip comes off the window first. */
static void menu_closing(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineMenuStrip *s = strip_by_native(ctx, w->MenuStrip);

    if (s)
    {
        ClearMenuStrip(w);
        strip_left(ctx, s);
    }
}

void Kaffeine_MenusCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineMenuStrip *s;
    ULONG left = 0;

    /* No window that the program left open keeps a strip that points
       into guest memory, and the strips go, also those of CreateMenusA()
       that the program did not free. */
    for (s = ctx->menus; s; s = s->next)
        windows_showing(ctx, s->root, TRUE);
    while ((s = ctx->menus))
    {
        if (!s->nodes)
        {
            FreeMenus(s->root);
            left++;
        }
        strip_free(ctx, s);
    }
    if (left)
        bug("[kaffeine] %lu GadTools menu strips that the program left, freed\n",
            (unsigned long)left);
}

/* ── Tags ────────────────────────────────────────────────────────── */

/* The next tag of a 68k tag list, through TAG_MORE and past TAG_IGNORE and
   TAG_SKIP. FALSE at the end; *bad for a list that leaves guest memory or
   goes on longer than any program means. */
static BOOL tag_next(struct KaffeineContext *ctx, ULONG *pos, ULONG *tag, ULONG *data,
                     UWORD *budget, BOOL *bad)
{
    while (*pos)
    {
        if (!*budget || !guest_range(ctx, *pos, 8))
        {
            *bad = TRUE;
            return FALSE;
        }
        (*budget)--;
        *tag = m68k_read32(ctx, *pos);
        *data = m68k_read32(ctx, *pos + 4);
        *pos += 8;
        if (*tag == TAG_DONE)
            break;
        if (*tag == TAG_IGNORE)
            continue;
        if (*tag == TAG_MORE)
            *pos = *data;
        else if (*tag == TAG_SKIP)
        {
            if (*data > MAX_TAGS)
            {
                *bad = TRUE;
                return FALSE;
            }
            *pos += 8 * *data;
        }
        else
            return TRUE;
    }
    *pos = 0;
    return FALSE;
}

/* The tags of LayoutMenusA() and LayoutMenuItemsA() as native ones. The
   images of GTMN_Checkmark and GTMN_AmigaKey are left out, so GadTools
   draws its own. Returns 1, 0 for 68k input that the bridge refuses, or
   -1 without memory. */
#define LAYOUT_TAGS 5
static int layout_tags(struct KaffeineContext *ctx, struct KaffeineMenuStrip *s, ULONG tags,
                       struct TagItem *out, BOOL items)
{
    ULONG pos = tags, tag, data, i;
    UWORD budget = MAX_TAGS, n = 0;
    BOOL bad = FALSE, nomem = FALSE, seen;

    while (tag_next(ctx, &pos, &tag, &data, &budget, &bad))
    {
        void *native = NULL;

        /* the first of each tag counts, as for GetTagData() */
        for (i = 0, seen = FALSE; i < n; i++)
            seen |= out[i].ti_Tag == tag;
        if (seen)
            continue;
        switch (tag)
        {
        case GTMN_TextAttr:
            if (n < LAYOUT_TAGS - 1)
            {
                struct TextAttr *ta = data ? Kaffeine_MirrorNativeFont(ctx, &s->mirror, data, &nomem)
                                           : NULL;

                if (data && !ta)
                    return nomem ? -1 : 0;
                out[n].ti_Tag = tag;
                out[n++].ti_Data = (IPTR)ta;
            }
            break;
        case GTMN_FrontPen:
        case GTMN_NewLookMenus:
            if (n < LAYOUT_TAGS - 1)
            {
                out[n].ti_Tag = tag;
                out[n++].ti_Data = data;
            }
            break;
        case GTMN_Menu:
            if (!items || !strip_by_node(ctx, data, NODE_MENU, &native) || n >= LAYOUT_TAGS - 1)
                return 0;
            out[n].ti_Tag = tag;
            out[n++].ti_Data = (IPTR)native;
            break;
        case GTMN_Checkmark:
        case GTMN_AmigaKey:
            bug("[kaffeine] menus: the 68k image of tag %08lx is left out\n", (unsigned long)tag);
            break;
        default:
            break;
        }
    }
    out[n].ti_Tag = TAG_DONE;
    out[n].ti_Data = 0;
    return bad ? 0 : 1;
}

/* ── GadTools ────────────────────────────────────────────────────── */

/* gadtools -48: CreateMenusA(A0=newmenu, A1=tagList) -> D0: the strip as
   68k Menus, or 68k MenuItems for a NewMenu without NM_TITLE. */
IPTR Kaffeine_CreateMenusA(struct KaffeineContext *ctx, void *cpu)
{
    ULONG nm = THUNK_A(0), pos = THUNK_A(1), tag, data, errptr = 0, err_out = 0;
    ULONG count = 0, kept = 0, i, frontpen = 0;
    UWORD budget = MAX_TAGS;
    BOOL bad = FALSE, full = FALSE, full_set = FALSE, frontpen_set = FALSE, errptr_set = FALSE;
    BOOL titles = FALSE;
    struct NewMenu *entries;
    struct TagItem tags[4];
    struct KaffeineMenuStrip *s;
    APTR root;

    /* the first of each tag counts, as for GetTagData() */
    while (tag_next(ctx, &pos, &tag, &data, &budget, &bad))
    {
        if (tag == GTMN_FrontPen && !frontpen_set)
        {
            frontpen = data;
            frontpen_set = TRUE;
        }
        else if (tag == GTMN_FullMenu && !full_set)
        {
            full = data != 0;
            full_set = TRUE;
        }
        else if (tag == GTMN_SecondaryError && !errptr_set)
        {
            errptr = data;
            errptr_set = TRUE;
        }
    }
    if (bad || (errptr && !guest_range(ctx, errptr, 4)))
        return refuse(ctx, "CreateMenusA(): a tag list or an error ULONG outside guest memory");

    /* the entries up to NM_END, checked before any is used */
    for (pos = nm;; pos += G_NEWMENU_SIZE, count++)
    {
        UBYTE type;
        ULONG label, key;

        if (count == MAX_ENTRIES || !guest_range(ctx, pos, G_NEWMENU_SIZE))
            return refuse(ctx, "CreateMenusA(): a NewMenu array outside guest memory");
        type = ctx->mem[pos + G_NM_TYPE];
        if (type == NM_END)
            break;
        if ((type & NM_IGNORE) || (type != NM_TITLE && type != NM_ITEM && type != NM_SUB &&
                                   type != IM_ITEM && type != IM_SUB))
            continue;   /* GadTools skips these */
        if (type == IM_ITEM || type == IM_SUB)
            return refuse(ctx, "CreateMenusA(): image menu items are not supported yet");
        label = m68k_read32(ctx, pos + G_NM_LABEL);
        key = m68k_read32(ctx, pos + G_NM_COMMKEY);
        if ((label == G_BARLABEL ? type == NM_TITLE : !guest_string_ok(ctx, label)) ||
            (key && !guest_string_ok(ctx, key)))
            return refuse(ctx, "CreateMenusA(): a label or a command key that is not a string "
                               "in guest memory");
        titles |= type == NM_TITLE;
        kept++;
    }

    entries = AllocVec((kept + 1) * sizeof(*entries), MEMF_ANY | MEMF_CLEAR);
    if (!entries)
    {
        if (errptr)
            m68k_write32(ctx, errptr, GTMENU_NOMEM);
        return 0;
    }
    for (pos = nm, i = 0; i < kept; pos += G_NEWMENU_SIZE)
    {
        UBYTE type = ctx->mem[pos + G_NM_TYPE];
        ULONG label = m68k_read32(ctx, pos + G_NM_LABEL), key = m68k_read32(ctx, pos + G_NM_COMMKEY);

        if ((type & NM_IGNORE) || (type != NM_TITLE && type != NM_ITEM && type != NM_SUB))
            continue;
        entries[i].nm_Type = type;
        entries[i].nm_Label = label == G_BARLABEL ? NM_BARLABEL : (CONST_STRPTR)ctx->mem + label;
        entries[i].nm_CommKey = key ? (CONST_STRPTR)ctx->mem + key : NULL;
        entries[i].nm_Flags = m68k_read16(ctx, pos + G_NM_FLAGS);
        entries[i].nm_MutualExclude = (LONG)m68k_read32(ctx, pos + G_NM_MUTUALEXCLUDE);
        entries[i].nm_UserData = (APTR)(IPTR)m68k_read32(ctx, pos + G_NM_USERDATA);
        i++;
    }
    entries[kept].nm_Type = NM_END;

    tags[0].ti_Tag = frontpen_set ? GTMN_FrontPen : TAG_IGNORE;
    tags[0].ti_Data = frontpen;
    tags[1].ti_Tag = GTMN_FullMenu;
    tags[1].ti_Data = full;
    tags[2].ti_Tag = errptr ? GTMN_SecondaryError : TAG_IGNORE;
    tags[2].ti_Data = (IPTR)&err_out;
    tags[3].ti_Tag = TAG_DONE;
    tags[3].ti_Data = 0;
    root = CreateMenusA(entries, tags);
    FreeVec(entries);
    /* AROS writes the error into the tag itself, as an earlier GadTools
       did, except when it runs out of memory at once; the NDK writes it
       through the pointer. Either way the 68k ULONG gets it. */
    if (errptr)
        m68k_write32(ctx, errptr, tags[2].ti_Data != (IPTR)&err_out ? (ULONG)tags[2].ti_Data : err_out);
    if (!root)
        return 0;

    s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (s)
    {
        s->root = root;
        s->root_is_item = !titles;
        sync_out(ctx, s);
        if (s->guest_root && !s->mirror.failed)
        {
            s->next = ctx->menus;
            ctx->menus = s;
            ctx->menu_sync = menu_sync;
            ctx->menu_closing = menu_closing;
            return s->guest_root;
        }
        strip_free(ctx, s);
    }
    bug("[kaffeine] CreateMenusA(): no memory for the 68k menus\n");
    FreeMenus(root);
    if (errptr)
        m68k_write32(ctx, errptr, GTMENU_NOMEM);
    return 0;
}

/* gadtools -54: FreeMenus(A0=menu) */
IPTR Kaffeine_FreeMenus(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    struct KaffeineMenuStrip *s = gadtools_strip(ctx, guest);

    if (!s)
    {
        if (guest)
            bug("[kaffeine] FreeMenus(%08lx): not a strip of CreateMenusA(), ignored\n",
                (unsigned long)guest);
        return 0;
    }
    /* A strip still on a window would leave it a freed strip to show. */
    if (windows_showing(ctx, s->root, TRUE))
        bug("[kaffeine] FreeMenus(%08lx): the strip was still on a window, taken off\n",
            (unsigned long)guest);
    FreeMenus(s->root);
    strip_free(ctx, s);
    return 0;
}

/* gadtools -66: LayoutMenusA(A0=menu, A1=vi, A2=tagList) -> D0=success */
IPTR Kaffeine_LayoutMenusA(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineMenuStrip *s = gadtools_strip(ctx, THUNK_A(0));
    APTR vi = Kaffeine_VisualInfoNative(ctx, THUNK_A(1));
    struct TagItem tags[LAYOUT_TAGS];
    BOOL ok;
    int converted;

    if (!s || s->root_is_item || !vi)
        return refuse(ctx, "LayoutMenusA(): not a menu strip of CreateMenusA() or not a VisualInfo");
    converted = layout_tags(ctx, s, THUNK_A(2), tags, FALSE);
    if (converted == 0)
        return refuse(ctx, "LayoutMenusA(): a tag that the bridge cannot take");
    if (converted < 0)
        return FALSE;
    sync_in(ctx, s);
    ok = LayoutMenusA(s->root, vi, tags);
    sync_out(ctx, s);
    return ok ? TRUE : FALSE;
}

/* gadtools -60: LayoutMenuItemsA(A0=menuitem, A1=vi, A2=tagList) -> D0 */
IPTR Kaffeine_LayoutMenuItemsA(struct KaffeineContext *ctx, void *cpu)
{
    void *item = NULL;
    struct KaffeineMenuStrip *s = strip_by_node(ctx, THUNK_A(0), NODE_ITEM, &item);
    APTR vi = Kaffeine_VisualInfoNative(ctx, THUNK_A(1));
    struct TagItem tags[LAYOUT_TAGS];
    BOOL ok;
    int converted;

    if (!s || !vi)
        return refuse(ctx, "LayoutMenuItemsA(): not an item of CreateMenusA() or not a VisualInfo");
    converted = layout_tags(ctx, s, THUNK_A(2), tags, TRUE);
    if (converted == 0)
        return refuse(ctx, "LayoutMenuItemsA(): a tag that the bridge cannot take");
    if (converted < 0)
        return FALSE;
    sync_in(ctx, s);
    ok = LayoutMenuItemsA(item, vi, tags);
    sync_out(ctx, s);
    return ok ? TRUE : FALSE;
}

/* ── Intuition ───────────────────────────────────────────────────── */

static IPTR set_strip(struct KaffeineContext *ctx, void *cpu, BOOL reset)
{
    ULONG window = THUNK_A(0), menu = THUNK_A(1);
    struct Window *w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW);
    struct KaffeineMenuStrip *s = strip_by_root(ctx, menu), *old, *copy;
    const char *why;
    BOOL ok, fresh = FALSE;

    if (!w)
        return refuse(ctx, "SetMenuStrip(): not a window of the program");
    if (s && s->root_is_item)
        return refuse(ctx, "SetMenuStrip(): GadTools items without a menu, as CreateMenusA() "
                           "makes them without NM_TITLE");
    old = strip_by_native(ctx, w->MenuStrip);
    if (s && !s->nodes)
        sync_in(ctx, s);
    else if (s && (reset || windows_showing(ctx, s->root, FALSE)))
        strip_import(ctx, s);   /* the copy that a window shows, or that waits */
    else if (menu)
    {
        /* SetMenuStrip() of a strip that no window shows takes all of it
           anew, since the program may have changed any of it meanwhile. */
        if (!(copy = strip_copy(ctx, menu, &why)))
            return why ? refuse_input(ctx, reset ? "ResetMenuStrip()" : "SetMenuStrip()", why)
                       : FALSE;
        if (s)
            strip_free(ctx, s);
        s = copy;
        fresh = TRUE;
    }
    /* A new copy has no page sizes yet, which only SetMenuStrip() works
       out. */
    ok = reset && !fresh ? ResetMenuStrip(w, s ? s->root : NULL)
                         : SetMenuStrip(w, s ? s->root : NULL);
    if (s && !s->nodes)
        sync_out(ctx, s);
    if (!ok)
    {
        if (fresh)
            strip_free(ctx, s);
        return FALSE;
    }
    if (fresh)
    {
        s->next = ctx->menus;
        ctx->menus = s;
        ctx->menu_sync = menu_sync;
        ctx->menu_closing = menu_closing;
    }
    if (s)
        s->parked = FALSE;
    m68k_write32(ctx, window + M68K_WIN_MENUSTRIP, menu);
    if (old && old != s)
        strip_left(ctx, old);
    return TRUE;
}

/* intuition -264: SetMenuStrip(A0=window, A1=menu) -> D0=success */
IPTR Kaffeine_SetMenuStrip(struct KaffeineContext *ctx, void *cpu)
{
    return set_strip(ctx, cpu, FALSE);
}

/* intuition -702: ResetMenuStrip(A0=window, A1=menu) -> D0=success */
IPTR Kaffeine_ResetMenuStrip(struct KaffeineContext *ctx, void *cpu)
{
    return set_strip(ctx, cpu, TRUE);
}

/* intuition -54: ClearMenuStrip(A0=window) */
IPTR Kaffeine_ClearMenuStrip(struct KaffeineContext *ctx, void *cpu)
{
    ULONG window = THUNK_A(0);
    struct Window *w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW);
    struct KaffeineMenuStrip *s;

    if (!w)
        return refuse(ctx, "ClearMenuStrip(): not a window of the program");
    s = strip_by_native(ctx, w->MenuStrip);
    ClearMenuStrip(w);
    m68k_write32(ctx, window + M68K_WIN_MENUSTRIP, 0);
    /* What Intuition set last, also for a menu message still on its way,
       which no longer finds the strip on the window. */
    if (s)
        strip_left(ctx, s);
    return 0;
}

/* ItemAddress() in a strip that the program built: its own item or
   subitem, found where it lies as AROS's ItemAddress() finds it in a
   native strip. FALSE for a strip that leaves guest memory on the way. */
static BOOL item_address(struct KaffeineContext *ctx, ULONG menu, UWORD number, ULONG *item)
{
    ULONG at, sub;
    UWORD i;

    *item = 0;
    if (!menu || number == MENUNULL)
        return TRUE;
    for (i = 0; menu && i < MENUNUM(number); i++)
    {
        if (!guest_range(ctx, menu, G_PLAIN_MENU_SIZE))
            return FALSE;
        menu = m68k_read32(ctx, menu + G_MU_NEXTMENU);
    }
    if (!menu)
        return TRUE;
    if (!guest_range(ctx, menu, G_PLAIN_MENU_SIZE))
        return FALSE;
    at = m68k_read32(ctx, menu + G_MU_FIRSTITEM);
    for (i = 0; at && i < ITEMNUM(number); i++)
    {
        if (!guest_range(ctx, at, G_PLAIN_MENUITEM_SIZE))
            return FALSE;
        at = m68k_read32(ctx, at + G_MI_NEXTITEM);
    }
    if (at && SUBNUM(number) != NOSUB)
    {
        if (!guest_range(ctx, at, G_PLAIN_MENUITEM_SIZE))
            return FALSE;
        if ((sub = m68k_read32(ctx, at + G_MI_SUBITEM)))
            for (at = sub, i = 0; at && i < SUBNUM(number); i++)
            {
                if (!guest_range(ctx, at, G_PLAIN_MENUITEM_SIZE))
                    return FALSE;
                at = m68k_read32(ctx, at + G_MI_NEXTITEM);
            }
    }
    if (at && !guest_range(ctx, at, G_PLAIN_MENUITEM_SIZE))
        return FALSE;
    *item = at;
    return TRUE;
}

/* intuition -144: ItemAddress(A0=menustrip, D0=menunumber) -> D0: the 68k
   item or subitem, or 0. */
IPTR Kaffeine_ItemAddress(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), found;
    UWORD number = (UWORD)THUNK_D(0);
    struct KaffeineMenuStrip *s = strip_by_root(ctx, guest);
    struct KaffeineMirrorNode *n;
    struct KaffeineMenuNode *n2;
    struct MenuItem *item;

    if (s && !s->nodes)
    {
        item = ItemAddress(s->root, number);
        n = item ? Kaffeine_MirrorFind(&s->mirror, item, NODE_ITEM) : NULL;
        return n ? n->guest : 0;
    }
    /* A strip that the program built, its item with what Intuition set in
       it for the program to read */
    if (!item_address(ctx, guest, number, &found))
        return refuse(ctx, "ItemAddress(): a menu strip that leaves guest memory");
    if (s && found && (n2 = node_of(s, ItemAddress(s->root, number))) && n2->guest == found)
        node_out(ctx, n2);
    return found;
}

static IPTR on_off(struct KaffeineContext *ctx, void *cpu, BOOL on)
{
    struct Window *w = Kaffeine_ShadowNative(ctx, THUNK_A(0), KAFFEINE_SHADOW_WINDOW);
    struct KaffeineMenuStrip *s;

    if (!w)
        return refuse(ctx, "OnMenu()/OffMenu(): not a window of the program");
    /* Nothing goes to the native strip first: it is in use, and Intuition
       may have checked an item for a message still on its way. */
    s = strip_by_native(ctx, w->MenuStrip);
    if (on)
        OnMenu(w, (UWORD)THUNK_D(0));
    else
        OffMenu(w, (UWORD)THUNK_D(0));
    if (s && s->nodes)
        strip_out(ctx, s, MENUNULL);
    else if (s)
        sync_out(ctx, s);
    return 0;
}

/* intuition -180: OffMenu(A0=window, D0=menunumber) */
IPTR Kaffeine_OffMenu(struct KaffeineContext *ctx, void *cpu)
{
    return on_off(ctx, cpu, FALSE);
}

/* intuition -192: OnMenu(A0=window, D0=menunumber) */
IPTR Kaffeine_OnMenu(struct KaffeineContext *ctx, void *cpu)
{
    return on_off(ctx, cpu, TRUE);
}
