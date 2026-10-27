/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GadTools gadgets of 68k code. CreateContext() and CreateGadgetA() let
    native GadTools build the gadgets from a native NewGadget and native
    tags, and mirror every gadget of the list in guest memory as a 68k
    ExtGadget: the context, each gadget that CreateGadgetA() returns and
    the gadgets that GadTools puts before it, such as the arrows of a
    scroller, the level display of a slider and the scroller of a
    listview. A mirror has the geometry,
    the flags, the type, the ID and the UserData of the program, the label
    as 68k IntuiTexts, a string or an image, a 68k StringInfo with its
    buffer in guest memory for STRING_KIND and INTEGER_KIND, and a 68k
    PropInfo for SLIDER_KIND, SCROLLER_KIND and the prop of a listview's
    scroller. Native pointers that the
    program has no use for, such as the render images, the dispatcher of a
    BOOPSI gadget in MutualExclude and the extension of a StringInfo, stay
    out of the mirror. Each 68k gadget is also a shadow, so that the
    generated thunks of Intuition find the native gadget.

    Each call of CreateContext() or CreateGadgetA() makes a batch: the
    gadgets of the call, a table of their native objects and 68k mirrors,
    and the native copies that they point to, such as label arrays and
    formats. A batch goes once GadTools frees its gadgets. The bridge takes
    as the previous gadget only one that CreateContext() or CreateGadgetA()
    returned, and FreeGadgets() of a context or of the first gadget of a
    batch only, so that what GadTools cuts off a list, or frees, is always
    whole batches: a chain cut off stays with its batches until the
    program frees it.

    Only the tags that GadTools reads for a kind reach it, and of a
    repeated tag only the one that GadTools uses, the first or the last,
    converted: strings of the program stay in guest memory, label arrays
    become native arrays, a colour table is checked for what the palette
    class reads of it, a format is checked for one integer and copied, and
    GTST_String is cut to the buffer of the string class, which copied it
    without a limit before it cut texts to MaxChars itself.

    GT_GetGadgetAttrsA() fills in the attributes that the autodoc gives
    for the kind of a gadget, as 68k LONGs: numbers as the classes keep
    them, the program's own label array and colour table, the buffer of
    the 68k StringInfo. AROS's text class has no getter, so the bridge
    answers GTTX_Text and GTNM_Number from what it converted, and it counts
    the cycle and palette attributes, which AROS's classes fill in without
    counting them. Before a get the bridge brings the mirrors of the
    gadget's batch up to date with the native gadgets.

    A listview holds a native copy of the program's List, made again at
    every attach after the whole List is checked as the class walks it,
    and freed when the class holds another; the bridge gives a new listview
    the GA_Disabled, GTLV_Top and GTLV_MakeVisible that AROS's builder
    passes before the scroller and the List exist. A string gadget of
    GTLV_ShowSelected stays in its own batch, whose mirrors follow the
    listview's, and cannot be freed while the listview stays.

    GT_SetGadgetAttrsA() changes the attributes that its autodoc gives for
    the kind of a gadget, in one native call, each converted and checked
    for what the classes do with it, and the mirrors follow, as they do
    after OnGadget() and OffGadget(). Both pass the native window when the
    batch of the gadget is on that window, where GadTools then draws the
    change; on no window, or with another one, nothing draws.

    A gadget list goes on a window as a whole: FirstGadget of OpenWindow(),
    WA_Gadgets, AddGList() and AddGadget() take whole batches, from a
    context or the first gadget of a batch, whose 68k links within a batch
    must be the native ones, never between the gadgets of a batch on the
    window, and a palette only when its class can lay its colours out
    there; each batch knows the native window it is on.
    RemoveGList() and RemoveGadget() take whole batches off, and
    CloseWindow() takes all of them off before the window closes. The 68k
    NextGadget of the last one stays on the gadget that followed it, as
    AmigaOS 3.2.3 leaves it, while its native one goes; from the last
    gadget of a batch on, AddGList() and FreeGadgets() follow the program's
    68k list, as GadTools and Intuition do. Before a message of a window
    reaches the program, the mirrors of the gadgets on it follow the
    input, and IAddress names the 68k mirror of a gadget.

    The mirrors lie in guest memory of the bridge's own kind, which
    FreeMem() of the program cannot release. Intuition's SetAttrsA(),
    SetGadgetAttrsA(), GetAttr() and DoGadgetMethodA() would reach the
    native class of a GadTools gadget with 68k data: for a GadTools gadget
    they stop the program, as do hooks and functions of 68k code, until they
    have a bridge. ActivateGadget() reaches Intuition only for a GadTools
    gadget on the window that its batch is on and for the copy of a gadget
    that the program built itself on the window given, and gives FALSE for
    anything else, as AROS activates whatever gadget it gets.
*/

#include "kaffeine_intern.h"
#include "kaffeine_classic.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_menus.h"
#include "kaffeine_mirror.h"
#include "kaffeine_rastport.h"
#include "kaffeine_shadow.h"
#include "kaffeine_taglist.h"

#include <aros/debug.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k structures of the NDK */
enum
{
    G_NG_LEFTEDGE = 0, G_NG_TOPEDGE = 2, G_NG_WIDTH = 4, G_NG_HEIGHT = 6,
    G_NG_GADGETTEXT = 8, G_NG_TEXTATTR = 12, G_NG_GADGETID = 16, G_NG_FLAGS = 18,
    G_NG_VISUALINFO = 22, G_NG_USERDATA = 26, G_NEWGADGET_SIZE = 30,

    /* ExtGadget: a Gadget, then the fields that GFLG_EXTENDED announces */
    G_GD_NEXTGADGET = 0, G_GD_LEFTEDGE = 4, G_GD_TOPEDGE = 6, G_GD_WIDTH = 8,
    G_GD_HEIGHT = 10, G_GD_FLAGS = 12, G_GD_ACTIVATION = 14, G_GD_GADGETTYPE = 16,
    G_GD_GADGETRENDER = 18, G_GD_SELECTRENDER = 22, G_GD_GADGETTEXT = 26,
    G_GD_MUTUALEXCLUDE = 30, G_GD_SPECIALINFO = 34, G_GD_GADGETID = 38,
    G_GD_USERDATA = 40, G_GD_MOREFLAGS = 44, G_GD_BOUNDSLEFTEDGE = 48,
    G_GD_BOUNDSTOPEDGE = 50, G_GD_BOUNDSWIDTH = 52, G_GD_BOUNDSHEIGHT = 54,
    G_EXTGADGET_SIZE = 56,

    G_SI_BUFFER = 0, G_SI_UNDOBUFFER = 4, G_SI_BUFFERPOS = 8, G_SI_MAXCHARS = 10,
    G_SI_DISPPOS = 12, G_SI_UNDOPOS = 14, G_SI_NUMCHARS = 16, G_SI_DISPCOUNT = 18,
    G_SI_CLEFT = 20, G_SI_CTOP = 22, G_SI_EXTENSION = 24, G_SI_LONGINT = 28,
    G_SI_ALTKEYMAP = 32, G_STRINGINFO_SIZE = 36,

    G_PI_FLAGS = 0, G_PI_HORIZPOT = 2, G_PI_VERTPOT = 4, G_PI_HORIZBODY = 6,
    G_PI_VERTBODY = 8, G_PI_CWIDTH = 10, G_PI_CHEIGHT = 12, G_PI_HPOTRES = 14,
    G_PI_VPOTRES = 16, G_PI_LEFTBORDER = 18, G_PI_TOPBORDER = 20, G_PROPINFO_SIZE = 22,

    G_TA_NAME = 0, G_TEXTATTR_SIZE = 8,

    G_LH_HEAD = 0, G_LIST_SIZE = 14,
    G_LN_SUCC = 0, G_LN_TYPE = 8, G_LN_PRI = 9, G_LN_NAME = 10, G_NODE_SIZE = 14
};

/* Longer lists than these are no list a program means. */
#define MAX_TAGS 512
#define MAX_LABELS 1024
#define MAX_CHAIN 65536
/* AddGList() and RemoveGList() count gadgets in a WORD. */
#define MAX_WINDOW_GADGETS 32767
/* The text class of GadTools formats numbers into a buffer of this size. */
#define FORMAT_OUTPUT 256
/* makeitext() of GadTools counts the label and the name of its font in
   WORDs, and their sum with its own structures too. */
#define MAX_LABEL 16000
/* paletteclass.c looks for the pen of GTPA_Color with a WORD index. */
#define MAX_COLORS 32767

/* The guest memory that a colour table of the palette class needs: the
   class draws its colours from its offset on, and reads the table at its
   choice and at its old choice, plus the offset, which a pen that is not
   in the table, or one of an earlier call, can make any UBYTE */
static ULONG palette_room(ULONG colors, UBYTE offset)
{
    return (colors > 256 ? colors : 256) + offset;
}

/* listviewclass.c counts the Nodes of its List in a WORD. */
#define MAX_ENTRIES 32767
/* GTLV_Total and GTLV_Visible as the NDK numbers them; AROS's are
   GT_TagBase + 150 and + 151, and has none at these. */
#define NDK_GTLV_Total (GT_TagBase + 92)
#define NDK_GTLV_Visible (GT_TagBase + 93)

/* The objects of a batch with a 68k copy of their own kind; the shared
   helpers of kaffeine_mirror.c copy texts, fonts, images and strings.
   NODE_TEXT is the bridge's own copy of a text that GTTX_CopyText copies,
   keyed by its batch. */
enum { NODE_GADGET, NODE_STRINGINFO, NODE_BUFFER, NODE_PROPINFO, NODE_TEXT };

/* What else of a gadget has a 68k mirror */
enum { SPECIAL_NONE, SPECIAL_STRING, SPECIAL_PROP };

/* The kind of the batch of CreateContext() */
#define KIND_CONTEXT (~(ULONG)0)

/* A native copy that native gadgets point to, or that only a call uses */
struct KaffeineGadgetKept
{
    struct KaffeineGadgetKept *next;
    ULONG guest;                        /* the 68k original, or 0 */
    /* the copy follows */
};

/* A native copy of a 68k List of the program that a listview holds: the
   header and its Nodes, linked natively, the names in guest memory */
struct KaffeineEntries
{
    struct KaffeineEntries *next;        /* others of the same setter call */
    ULONG guest;                        /* the 68k List */
    struct List list;
    struct Node nodes[];
};

struct KaffeineGadgetBatch
{
    struct KaffeineGadgetBatch *next;
    struct Gadget *first, *last;        /* in the order of the native list */
    struct KaffeineMirror mirror;
    struct KaffeineGadgetKept *kept;
    ULONG kind;                         /* of last, the gadget of the call */
    /* What GT_GetGadgetAttrsA() gives for GTTX_Text, the guest address of
       the text, and for GTNM_Number, the number: AROS's text class has no
       getter for them. */
    ULONG shown;
    /* A palette: its colour table (a guest address, 0 without), the
       colours that it shows, the offset of its first colour and its choice,
       the index that its class keeps, as GadTools works them out, against
       which the setter checks a new table */
    ULONG table, colors;
    UBYTE offset, color;
    UWORD indw, indh;                   /* its GTPA_IndicatorWidth and Height */
    /* A listview: the copy of the List that its class holds, or NULL for
       NULL and ~0, and the batch of the string gadget of its
       GTLV_ShowSelected, which its class writes the selection into */
    struct KaffeineEntries *entries;
    struct KaffeineGadgetBatch *showsel;
    struct Window *window;              /* the native window it is on, or NULL */
    /* A context: the gadget of the last GADGETDOWN that GadTools filtered
       with it, which it may still keep active */
    struct Gadget *held;
    BOOL doomed;                        /* in a chain that FreeGadgets() frees */
    BOOL marked;                        /* in a run that a window takes or gives */
};

static const struct KaffeineWindowGadgets window_hooks;

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* The length of a string in guest memory, or -1 for one that is not */
static LONG guest_strlen(struct KaffeineContext *ctx, ULONG addr)
{
    const UBYTE *end;

    if (!guest_range(ctx, addr, 1) || !(end = memchr(ctx->mem + addr, 0, ctx->mem_size - addr)))
        return -1;
    return (LONG)(end - (ctx->mem + addr));
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

/* A native copy of size bytes of the 68k data at guest */
static APTR keep(struct KaffeineGadgetKept **list, ULONG size, ULONG guest)
{
    struct KaffeineGadgetKept *k = AllocVec(sizeof(*k) + size, MEMF_ANY | MEMF_CLEAR);

    if (!k)
        return NULL;
    k->next = *list;
    k->guest = guest;
    *list = k;
    return k + 1;
}

/* The 68k original of a native copy of the batch, or 0 */
static ULONG kept_guest(struct KaffeineGadgetBatch *b, APTR copy)
{
    struct KaffeineGadgetKept *k;

    for (k = b->kept; k && copy; k = k->next)
        if ((APTR)(k + 1) == copy)
            return k->guest;
    return 0;
}

static void kept_free(struct KaffeineGadgetKept **list)
{
    struct KaffeineGadgetKept *k;

    while ((k = *list))
    {
        *list = k->next;
        FreeVec(k);
    }
}

/* ── Lists of listviews ──────────────────────────────────────────── */

/* A native copy of the 68k List at guest for a listview, made after the
   whole chain is checked as listviewclass.c walks it: from lh_Head along
   ln_Succ to the Node whose ln_Succ is 0, every Node with a name that is a
   string in guest memory, at most MAX_ENTRIES of them, which also ends a
   ring. A Node without a name gets an empty one, which the class can draw.
   NULL with *res 0 for a List that the bridge refuses, -1 without memory. */
static struct KaffeineEntries *entries_new(struct KaffeineContext *ctx, ULONG guest, int *res)
{
    struct KaffeineEntries *e;
    struct Node *n;
    ULONG node, name, count = 0, i;

    *res = 0;
    if (!guest_range(ctx, guest, G_LIST_SIZE))
        return NULL;
    for (node = m68k_read32(ctx, guest + G_LH_HEAD);; node = m68k_read32(ctx, node + G_LN_SUCC))
    {
        if (!guest_range(ctx, node, 4))
            return NULL;
        if (!m68k_read32(ctx, node + G_LN_SUCC))
            break;
        if (count == MAX_ENTRIES || !guest_range(ctx, node, G_NODE_SIZE))
            return NULL;
        if ((name = m68k_read32(ctx, node + G_LN_NAME)) && guest_strlen(ctx, name) < 0)
            return NULL;
        count++;
    }
    if (!(e = AllocVec(sizeof(*e) + count * sizeof(e->nodes[0]), MEMF_ANY | MEMF_CLEAR)))
    {
        *res = -1;
        return NULL;
    }
    e->guest = guest;
    e->list.lh_Head = (struct Node *)&e->list.lh_Tail;
    e->list.lh_Tail = NULL;
    e->list.lh_TailPred = (struct Node *)&e->list.lh_Head;
    for (i = 0, node = m68k_read32(ctx, guest + G_LH_HEAD); i < count;
         i++, node = m68k_read32(ctx, node + G_LN_SUCC))
    {
        n = &e->nodes[i];
        name = m68k_read32(ctx, node + G_LN_NAME);
        n->ln_Type = ctx->mem[node + G_LN_TYPE];
        n->ln_Pri = (BYTE)ctx->mem[node + G_LN_PRI];
        n->ln_Name = name ? (char *)ctx->mem + name : (char *)"";
        n->ln_Succ = (struct Node *)&e->list.lh_Tail;
        n->ln_Pred = e->list.lh_TailPred;
        e->list.lh_TailPred->ln_Succ = n;
        e->list.lh_TailPred = n;
    }
    *res = 1;
    return e;
}

static void entries_free(struct KaffeineEntries *e)
{
    struct KaffeineEntries *next;

    for (; e; e = next)
    {
        next = e->next;
        FreeVec(e);
    }
}

/* ── Batches ─────────────────────────────────────────────────────── */

static struct KaffeineGadgetBatch *batch_new(struct KaffeineContext *ctx)
{
    struct KaffeineGadgetBatch *b = AllocVec(sizeof(*b), MEMF_ANY | MEMF_CLEAR);

    if (b)
    {
        b->next = ctx->gadgets;
        ctx->gadgets = b;
        ctx->window_gadgets = &window_hooks;
    }
    return b;
}

static void batch_free(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b)
{
    struct KaffeineGadgetBatch **link;
    ULONG i;

    for (link = &ctx->gadgets; *link; link = &(*link)->next)
        if (*link == b)
        {
            *link = b->next;
            break;
        }
    for (i = 0; i < b->mirror.count; i++)
        if (b->mirror.nodes[i].kind == NODE_GADGET)
            Kaffeine_ShadowForget(ctx, b->mirror.nodes[i].guest, KAFFEINE_SHADOW_GADGET);
    Kaffeine_MirrorFree(ctx, &b->mirror);
    kept_free(&b->kept);
    entries_free(b->entries);
    FreeVec(b);
}

/* The batch with a 68k gadget at guest, and its native gadget */
static struct KaffeineGadgetBatch *batch_of(struct KaffeineContext *ctx, ULONG guest,
                                           struct Gadget **native)
{
    struct KaffeineGadgetBatch *b;
    struct KaffeineMirrorNode *n;

    for (b = ctx->gadgets; b && guest; b = b->next)
        if ((n = Kaffeine_MirrorGuest(&b->mirror, guest, NODE_GADGET)))
        {
            *native = n->native;
            return b;
        }
    return NULL;
}

static struct KaffeineGadgetBatch *batch_of_native(struct KaffeineContext *ctx, struct Gadget *g)
{
    struct KaffeineGadgetBatch *b;

    for (b = ctx->gadgets; b && g; b = b->next)
        if (Kaffeine_MirrorFind(&b->mirror, g, NODE_GADGET))
            return b;
    return NULL;
}

/* The 68k gadget of a native one: the mirror of a GadTools gadget, the
   program's own gadget behind its copy, or 0 */
static ULONG guest_gadget(struct KaffeineContext *ctx, struct Gadget *g)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, g, KAFFEINE_SHADOW_GADGET);

    if (!guest && g)
        guest = Kaffeine_ShadowGuest(ctx, g, KAFFEINE_SHADOW_CLASSIC);
    return guest;
}

/* The 68k NextGadget of a mirrored gadget, as the native list has it: a
   gadget of the program, a GadTools one or one that it built, or 0 */
static void link(struct KaffeineContext *ctx, struct Gadget *g)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, g, KAFFEINE_SHADOW_GADGET);

    if (guest)
        m68k_write32(ctx, guest + G_GD_NEXTGADGET, guest_gadget(ctx, g->NextGadget));
}

/* ── Mirrors ─────────────────────────────────────────────────────── */

static ULONG mirror_label(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b, struct Gadget *g)
{
    if (!g->GadgetText)
        return 0;
    switch (g->Flags & GFLG_LABELMASK)
    {
    case GFLG_LABELITEXT:
        return Kaffeine_MirrorTexts(ctx, &b->mirror, g->GadgetText);
    case GFLG_LABELSTRING:
        return Kaffeine_MirrorString(ctx, &b->mirror, (const char *)g->GadgetText);
    case GFLG_LABELIMAGE:
        return Kaffeine_MirrorImage(ctx, &b->mirror, (struct Image *)g->GadgetText);
    default:
        return 0;
    }
}

/* A 68k StringInfo whose buffer, in guest memory, has the native text. The
   guest buffer keeps the size that it was made with, the MaxChars of the
   native buffer then: native code may change MaxChars without another
   buffer, so the smaller of the two counts, for the text and for the
   MaxChars that the program sees. Without a copy of a new native buffer
   (no guest memory, or no MaxChars to make it with) nothing of the
   StringInfo changes, and 0 says so. */
static ULONG mirror_stringinfo(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b,
                               struct StringInfo *si)
{
    ULONG guest = Kaffeine_MirrorNode(ctx, &b->mirror, si, NODE_STRINGINFO, G_STRINGINFO_SIZE, NULL);
    ULONG buffer = 0, len = 0, room = 0;
    struct KaffeineMirrorNode *n = NULL;
    const UBYTE *end;

    if (!guest)
        return 0;
    if (si->Buffer && !(n = Kaffeine_MirrorFind(&b->mirror, si->Buffer, NODE_BUFFER)))
    {
        if (si->MaxChars <= 0 ||
            !Kaffeine_MirrorNode(ctx, &b->mirror, si->Buffer, NODE_BUFFER, (ULONG)si->MaxChars, NULL))
        {
            b->mirror.failed = TRUE;
            return 0;
        }
        n = Kaffeine_MirrorFind(&b->mirror, si->Buffer, NODE_BUFFER);
    }
    if (n)
    {
        buffer = n->guest;
        room = si->MaxChars > 0 ? (ULONG)si->MaxChars : 0;
        if (room > n->size)
            room = n->size;
        if (room > 1)
        {
            end = memchr(si->Buffer, 0, room - 1);
            len = end ? (ULONG)(end - si->Buffer) : room - 1;
            memcpy(ctx->mem + buffer, si->Buffer, len);
        }
        ctx->mem[buffer + len] = 0;
    }
    m68k_write32(ctx, guest + G_SI_BUFFER, buffer);
    m68k_write32(ctx, guest + G_SI_UNDOBUFFER, 0);
    m68k_write16(ctx, guest + G_SI_BUFFERPOS, (UWORD)si->BufferPos);
    m68k_write16(ctx, guest + G_SI_MAXCHARS, (UWORD)room);
    m68k_write16(ctx, guest + G_SI_DISPPOS, (UWORD)si->DispPos);
    m68k_write16(ctx, guest + G_SI_UNDOPOS, (UWORD)si->UndoPos);
    m68k_write16(ctx, guest + G_SI_NUMCHARS, (UWORD)si->NumChars);
    m68k_write16(ctx, guest + G_SI_DISPCOUNT, (UWORD)si->DispCount);
    m68k_write16(ctx, guest + G_SI_CLEFT, (UWORD)si->CLeft);
    m68k_write16(ctx, guest + G_SI_CTOP, (UWORD)si->CTop);
    m68k_write32(ctx, guest + G_SI_EXTENSION, 0);
    m68k_write32(ctx, guest + G_SI_LONGINT, (ULONG)si->LongInt);
    m68k_write32(ctx, guest + G_SI_ALTKEYMAP, 0);
    return guest;
}

static ULONG mirror_propinfo(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b,
                             struct PropInfo *pi)
{
    ULONG guest = Kaffeine_MirrorNode(ctx, &b->mirror, pi, NODE_PROPINFO, G_PROPINFO_SIZE, NULL);

    if (!guest)
        return 0;
    m68k_write16(ctx, guest + G_PI_FLAGS, pi->Flags);
    m68k_write16(ctx, guest + G_PI_HORIZPOT, pi->HorizPot);
    m68k_write16(ctx, guest + G_PI_VERTPOT, pi->VertPot);
    m68k_write16(ctx, guest + G_PI_HORIZBODY, pi->HorizBody);
    m68k_write16(ctx, guest + G_PI_VERTBODY, pi->VertBody);
    m68k_write16(ctx, guest + G_PI_CWIDTH, pi->CWidth);
    m68k_write16(ctx, guest + G_PI_CHEIGHT, pi->CHeight);
    m68k_write16(ctx, guest + G_PI_HPOTRES, pi->HPotRes);
    m68k_write16(ctx, guest + G_PI_VPOTRES, pi->VPotRes);
    m68k_write16(ctx, guest + G_PI_LEFTBORDER, pi->LeftBorder);
    m68k_write16(ctx, guest + G_PI_TOPBORDER, pi->TopBorder);
    return guest;
}

/* The 68k ExtGadget of a native gadget, a shadow too. NextGadget is left
   to link(). UserData is the program's: the native gadget has the value of
   the NewGadget, which the program may change in the mirror. */
static ULONG mirror_gadget(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b,
                           struct Gadget *g, UBYTE special, APTR userdata)
{
    struct ExtGadget *eg = (g->Flags & GFLG_EXTENDED) ? (struct ExtGadget *)g : NULL;
    ULONG guest, info = 0, text;
    BOOL fresh, failed = b->mirror.failed, info_failed, text_failed;
    UWORD flags;

    guest = Kaffeine_MirrorNode(ctx, &b->mirror, g, NODE_GADGET, G_EXTGADGET_SIZE, &fresh);
    if (!guest)
        return 0;
    if (fresh && !Kaffeine_ShadowAdd(ctx, guest, g, KAFFEINE_SHADOW_GADGET))
    {
        b->mirror.failed = TRUE;
        return 0;
    }
    /* A refresh needs new copies only for native pointers that changed
       behind the bridge. Without guest memory for all that a StringInfo or
       a label needs, the old ones stay in the mirror, whole. */
    b->mirror.failed = FALSE;
    if (special == SPECIAL_STRING && g->SpecialInfo)
        info = mirror_stringinfo(ctx, b, g->SpecialInfo);
    else if (special == SPECIAL_PROP && g->SpecialInfo)
        info = mirror_propinfo(ctx, b, g->SpecialInfo);
    info_failed = b->mirror.failed;
    b->mirror.failed = FALSE;
    text = mirror_label(ctx, b, g);
    text_failed = b->mirror.failed;
    b->mirror.failed = failed || info_failed || text_failed;
    flags = g->Flags & ~GFLG_STRINGEXTEND;
    if (!fresh && info_failed)
        info = m68k_read32(ctx, guest + G_GD_SPECIALINFO);
    if (!fresh && text_failed)
    {
        /* the old label with its own kind, IntuiText, string or image */
        text = m68k_read32(ctx, guest + G_GD_GADGETTEXT);
        flags = (flags & ~GFLG_LABELMASK) | (m68k_read16(ctx, guest + G_GD_FLAGS) & GFLG_LABELMASK);
    }
    m68k_write16(ctx, guest + G_GD_LEFTEDGE, (UWORD)g->LeftEdge);
    m68k_write16(ctx, guest + G_GD_TOPEDGE, (UWORD)g->TopEdge);
    m68k_write16(ctx, guest + G_GD_WIDTH, (UWORD)g->Width);
    m68k_write16(ctx, guest + G_GD_HEIGHT, (UWORD)g->Height);
    /* The 68k StringInfo has no extension, so the flag that announces one
       stays out. */
    m68k_write16(ctx, guest + G_GD_FLAGS, flags);
    m68k_write16(ctx, guest + G_GD_ACTIVATION, g->Activation);
    m68k_write16(ctx, guest + G_GD_GADGETTYPE, g->GadgetType);
    m68k_write32(ctx, guest + G_GD_GADGETRENDER, 0);
    m68k_write32(ctx, guest + G_GD_SELECTRENDER, 0);
    m68k_write32(ctx, guest + G_GD_GADGETTEXT, text);
    /* gadgetclass keeps the dispatcher of the class there */
    m68k_write32(ctx, guest + G_GD_MUTUALEXCLUDE, 0);
    m68k_write32(ctx, guest + G_GD_SPECIALINFO, info);
    m68k_write16(ctx, guest + G_GD_GADGETID, g->GadgetID);
    if (fresh)
        m68k_write32(ctx, guest + G_GD_USERDATA, g->UserData == userdata ? (ULONG)(IPTR)userdata : 0);
    /* The bits that mean the same in the NDK. AROS's GMORE_BOOPSIGADGET,
       which gadgetclass sets on every gadget, is another bit there, and a
       mirror is no 68k BOOPSI object for the NDK's own. */
    m68k_write32(ctx, guest + G_GD_MOREFLAGS,
                 eg ? eg->MoreFlags & (GMORE_BOUNDS | GMORE_GADGETHELP | GMORE_SCROLLRASTER) : 0);
    m68k_write16(ctx, guest + G_GD_BOUNDSLEFTEDGE, eg ? (UWORD)eg->BoundsLeftEdge : 0);
    m68k_write16(ctx, guest + G_GD_BOUNDSTOPEDGE, eg ? (UWORD)eg->BoundsTopEdge : 0);
    m68k_write16(ctx, guest + G_GD_BOUNDSWIDTH, eg ? (UWORD)eg->BoundsWidth : 0);
    m68k_write16(ctx, guest + G_GD_BOUNDSHEIGHT, eg ? (UWORD)eg->BoundsHeight : 0);
    return guest;
}

/* What else of the gadget of a kind has a mirror; the gadgets that
   GadTools puts before it have nothing else. */
static UBYTE special_of(ULONG kind)
{
    if (kind == STRING_KIND || kind == INTEGER_KIND)
        return SPECIAL_STRING;
    if (kind == SLIDER_KIND || kind == SCROLLER_KIND)
        return SPECIAL_PROP;
    return SPECIAL_NONE;
}

/* What else of the gadget g of the batch b has a mirror: of the gadget of
   its kind what the kind gives, and of the prop that GadTools puts just
   before a listview, for its scroller, a PropInfo */
static UBYTE special_role(struct KaffeineGadgetBatch *b, struct Gadget *g)
{
    if (b->kind == KIND_CONTEXT)
        return SPECIAL_NONE;
    if (g == b->last)
        return special_of(b->kind);
    return b->kind == LISTVIEW_KIND && g->NextGadget == b->last ? SPECIAL_PROP : SPECIAL_NONE;
}

/* Mirror the gadgets that GadTools put after previous, up to gad, the one
   it returned, as the batch b. FALSE without guest memory or room for the
   shadows. */
static BOOL mirror_new(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b,
                       struct Gadget *previous, struct Gadget *gad, APTR userdata)
{
    struct Gadget *g;
    ULONG n;

    b->first = previous->NextGadget;
    b->last = gad;
    for (n = 0, g = b->first; g && n < MAX_CHAIN; g = g->NextGadget, n++)
    {
        if (!mirror_gadget(ctx, b, g, special_role(b, g), userdata))
            return FALSE;
        if (g == gad)
            break;
    }
    return g == gad && !b->mirror.failed;
}

/* Bring the 68k mirrors of a batch up to date with its native gadgets,
   which native code may have changed since. It goes by the table of the
   batch, not by native links, and leaves the links to link() and UserData
   to the program. Without guest memory for a new copy, which only a native
   pointer that changed behind the bridge needs, the old fields stay. */
static void settle(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b)
{
    ULONG i, count = b->mirror.count;
    struct Gadget *g;

    for (i = 0; i < count; i++)
        if (b->mirror.nodes[i].kind == NODE_GADGET)
        {
            g = b->mirror.nodes[i].native;
            mirror_gadget(ctx, b, g, special_role(b, g), NULL);
        }
    if (b->mirror.failed)
    {
        bug("[kaffeine] no guest memory to bring the 68k gadgets up to date\n");
        b->mirror.failed = FALSE;
    }
}

/* ── Tags ────────────────────────────────────────────────────────── */

enum
{
    T_SCALAR,   /* a number or a flag, sign-extended as a LONG */
    T_PEN,      /* a pen of the text class, which indexes the DrawInfo pens */
    T_KEY,      /* GT_Underscore: a character, never NUL */
    T_STRING,   /* a string of the program, which GadTools may keep */
    T_COPY,     /* GTST_String: copied by the string class, cut to MaxChars */
    T_FORMAT,   /* a RawDoFmt() format of one integer, copied */
    T_LABELS,   /* a NULL-terminated array of strings, as a native array */
    T_COLORS,   /* GTPA_ColorTable: a UBYTE pen for each colour */
    T_HOOK,     /* a hook or a function of 68k code: only NULL */
    T_ENTRIES,  /* GTLV_Labels: a 68k List, NULL or ~0 */
    T_SHOWN,    /* GTLV_ShowSelected: NULL, ~0 or a string gadget */
    T_WIDTH     /* GTLV_ScrollWidth: the width of a scroller with arrows */
};

#define K(kind) (1u << (kind))
#define STRING_KINDS (K(STRING_KIND) | K(INTEGER_KIND))
#define PROP_KINDS (K(SLIDER_KIND) | K(SCROLLER_KIND))
#define TEXT_KINDS (K(TEXT_KIND) | K(NUMBER_KIND))
/* The make functions of these kinds take the first of a repeated tag,
   through GetTagData() and FindTagItem(); the others loop over the list
   with NextTagItem() and keep the last. */
#define FIRST_KINDS (K(BUTTON_KIND) | K(CHECKBOX_KIND) | K(CYCLE_KIND) | K(MX_KIND))

/* The creation tags that GadTools reads for each kind, from the program's
   list: GT_Underscore for the label, through GetTagData(), the others in
   the make functions of gadgets.c. GadTools reads no other tag of the
   program. */
static const struct { ULONG tag; UBYTE type; UWORD kinds; } creation_tags[] =
{
    { GT_Underscore,         T_KEY,    0xFFFF },
    { GA_Disabled,           T_SCALAR, K(BUTTON_KIND) | K(CHECKBOX_KIND) | K(CYCLE_KIND) |
                                       K(LISTVIEW_KIND) | K(MX_KIND) | K(PALETTE_KIND) |
                                       PROP_KINDS | STRING_KINDS },
    { GA_Immediate,          T_SCALAR, K(BUTTON_KIND) | PROP_KINDS | STRING_KINDS },
    { GA_RelVerify,          T_SCALAR, PROP_KINDS | STRING_KINDS },
    { GA_TabCycle,           T_SCALAR, STRING_KINDS },
    { GTCB_Checked,          T_SCALAR, K(CHECKBOX_KIND) },
    { GTCB_Scaled,           T_SCALAR, K(CHECKBOX_KIND) },
    { GTCY_Labels,           T_LABELS, K(CYCLE_KIND) },
    { GTCY_Active,           T_SCALAR, K(CYCLE_KIND) },
    { GTLV_Top,              T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_MakeVisible,      T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_Labels,           T_ENTRIES, K(LISTVIEW_KIND) },
    { GTLV_Selected,         T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_ItemHeight,       T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_CallBack,         T_HOOK,   K(LISTVIEW_KIND) },
    { GTLV_MaxPen,           T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_ReadOnly,         T_SCALAR, K(LISTVIEW_KIND) },
    { LAYOUTA_Spacing,       T_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_ShowSelected,     T_SHOWN,  K(LISTVIEW_KIND) },
    { GTLV_ScrollWidth,      T_WIDTH,  K(LISTVIEW_KIND) },
    { GTMX_Labels,           T_LABELS, K(MX_KIND) },
    { GTMX_Active,           T_SCALAR, K(MX_KIND) },
    { GTMX_Spacing,          T_SCALAR, K(MX_KIND) },
    { GTMX_Scaled,           T_SCALAR, K(MX_KIND) },
    { GTMX_TitlePlace,       T_SCALAR, K(MX_KIND) },
    { GTPA_Depth,            T_SCALAR, K(PALETTE_KIND) },
    { GTPA_Color,            T_SCALAR, K(PALETTE_KIND) },
    { GTPA_ColorOffset,      T_SCALAR, K(PALETTE_KIND) },
    { GTPA_IndicatorWidth,   T_SCALAR, K(PALETTE_KIND) },
    { GTPA_IndicatorHeight,  T_SCALAR, K(PALETTE_KIND) },
    { GTPA_NumColors,        T_SCALAR, K(PALETTE_KIND) },
    { GTPA_ColorTable,       T_COLORS, K(PALETTE_KIND) },
    { GTTX_Text,             T_STRING, K(TEXT_KIND) },
    { GTTX_CopyText,         T_SCALAR, K(TEXT_KIND) },
    { GTTX_Border,           T_SCALAR, K(TEXT_KIND) },
    { GTNM_Number,           T_SCALAR, K(NUMBER_KIND) },
    { GTNM_Format,           T_FORMAT, K(NUMBER_KIND) },
    { GTNM_Border,           T_SCALAR, K(NUMBER_KIND) },
    { GTNM_MaxNumberLen,     T_SCALAR, K(NUMBER_KIND) },
    /* GTTX_Clipped, GTTX_FrontPen, GTTX_BackPen and GTTX_Justification
       are these */
    { GTNM_Clipped,          T_SCALAR, TEXT_KINDS },
    { GTNM_FrontPen,         T_PEN,    TEXT_KINDS },
    { GTNM_BackPen,          T_PEN,    TEXT_KINDS },
    { GTNM_Justification,    T_SCALAR, TEXT_KINDS },
    { GTSL_Min,              T_SCALAR, K(SLIDER_KIND) },
    { GTSL_Max,              T_SCALAR, K(SLIDER_KIND) },
    { GTSL_Level,            T_SCALAR, K(SLIDER_KIND) },
    { GTSL_LevelFormat,      T_FORMAT, K(SLIDER_KIND) },
    { GTSL_MaxLevelLen,      T_SCALAR, K(SLIDER_KIND) },
    { GTSL_MaxPixelLen,      T_SCALAR, K(SLIDER_KIND) },
    { GTSL_LevelPlace,       T_SCALAR, K(SLIDER_KIND) },
    { GTSL_Justification,    T_SCALAR, K(SLIDER_KIND) },
    { GTSL_DispFunc,         T_HOOK,   K(SLIDER_KIND) },
    { PGA_Freedom,           T_SCALAR, PROP_KINDS },
    { GTSC_Top,              T_SCALAR, K(SCROLLER_KIND) },
    { GTSC_Total,            T_SCALAR, K(SCROLLER_KIND) },
    { GTSC_Visible,          T_SCALAR, K(SCROLLER_KIND) },
    { GTSC_Arrows,           T_SCALAR, K(SCROLLER_KIND) },
    { GTST_String,           T_COPY,   K(STRING_KIND) },
    { GTST_MaxChars,         T_SCALAR, K(STRING_KIND) },
    { GTIN_Number,           T_SCALAR, K(INTEGER_KIND) },
    { GTIN_MaxChars,         T_SCALAR, K(INTEGER_KIND) },
    /* GTIN_EditHook is this one */
    { GTST_EditHook,         T_HOOK,   STRING_KINDS },
    { STRINGA_ExitHelp,      T_SCALAR, STRING_KINDS },
    { STRINGA_Justification, T_SCALAR, STRING_KINDS },
    { STRINGA_ReplaceMode,   T_SCALAR, STRING_KINDS }
};

/* The conversion of a creation tag for a kind, or -1 for a tag that
   GadTools does not read for it */
static int tag_type(ULONG tag, ULONG kind)
{
    ULONG i;

    for (i = 0; kind < 16 && i < sizeof(creation_tags) / sizeof(creation_tags[0]); i++)
        if (creation_tags[i].tag == tag)
            return (creation_tags[i].kinds & K(kind)) ? creation_tags[i].type : -1;
    return -1;
}

/* A format that the text class of GadTools can take, as AROS's RawDoFmt()
   reads it: no pointer, no more data than the single ULONG that the class
   passes, and no more output than its buffer holds, for any number. The
   widths are ULONGs, as in RawDoFmt(). */
static BOOL format_ok(const char *f)
{
    ULONG out = 1, data = 0;            /* 1 for the final NUL */

    while (*f)
    {
        ULONG minwidth = 0, maxwidth = ~(ULONG)0, width, minus = 0, size;

        if (*f++ != '%')
        {
            out++;
            if (out > FORMAT_OUTPUT)
                return FALSE;
            continue;
        }
        if (*f == '-')
            f++;
        if (*f == '0')
            f++;
        while (*f >= '0' && *f <= '9')
            minwidth = minwidth * 10 + (ULONG)(*f++ - '0');
        if (*f == '.')
        {
            f++;
            if (*f >= '0' && *f <= '9')
                for (maxwidth = 0; *f >= '0' && *f <= '9'; f++)
                    maxwidth = maxwidth * 10 + (ULONG)(*f - '0');
        }
        /* the bytes of a number: a WORD, a LONG or an IPTR */
        size = 2;
        if (*f == 'l')
            size = 4, f++;
        else if (*f == 'i')
            size = 8, f++;
        switch (*f)
        {
        case 'd': case 'D':
            minus = 1;
            /* fall through */
        case 'u': case 'U':
            width = size == 2 ? 5 : 10;
            break;
        case 'x': case 'X':
            width = size == 2 ? 4 : 8;
            break;
        case 'c':
            width = 1;
            break;
        case 's': case 'b': case 'p': case 'P':
            return FALSE;       /* a pointer */
        case '\0':
            /* the padding alone, at the end */
            return minwidth < FORMAT_OUTPUT && out + minwidth <= FORMAT_OUTPUT;
        default:
            /* "%%" and the like: the character itself, and no data */
            width = 1;
            size = 0;
            break;
        }
        if ((data += size) > 4)
            return FALSE;
        f++;
        if (width > maxwidth)
            width = maxwidth;
        if (minwidth >= FORMAT_OUTPUT)
            return FALSE;
        out += width + minus > minwidth ? width + minus : minwidth;
        if (out > FORMAT_OUTPUT)
            return FALSE;
    }
    return TRUE;
}

/* An occurrence of a tag that GadTools uses */
struct Use
{
    ULONG tag, data;
    int type;
};

struct Conversion
{
    struct TagItem *tags;               /* TAG_DONE at the end */
    struct KaffeineGadgetKept *temp;     /* copies for the call alone */
    const char *why;                    /* what the bridge refuses */
    /* the GTTX_Text, GTTX_CopyText and GTNM_Number that GadTools uses */
    ULONG text, number;
    BOOL copytext;
    /* the colour table, its colours, the offset and the pen of GTPA_Color
       that GadTools uses */
    ULONG table, colors;
    UBYTE offset, pen;
    /* a listview: the copy of its List, the item height and spacing, and
       the batch of the string gadget of GTLV_ShowSelected */
    struct KaffeineEntries *entries;
    ULONG itemheight, spacing;
    struct KaffeineGadgetBatch *showsel;
    /* a palette: the size of its indicator */
    UWORD indw, indh;
};

/* A native array of the program's strings, kept on the list */
static STRPTR *labels(struct KaffeineContext *ctx, struct KaffeineGadgetKept **list, ULONG array, int *res)
{
    STRPTR *out;
    ULONG n, i, s;

    for (n = 0;; n++)
    {
        if (n == MAX_LABELS || !guest_range(ctx, array + 4 * n, 4))
        {
            *res = 0;
            return NULL;
        }
        if (!(s = m68k_read32(ctx, array + 4 * n)))
            break;
        if (guest_strlen(ctx, s) < 0)
        {
            *res = 0;
            return NULL;
        }
    }
    if (!(out = keep(list, (n + 1) * sizeof(*out), array)))
    {
        *res = -1;
        return NULL;
    }
    for (i = 0; i < n; i++)
        out[i] = (STRPTR)ctx->mem + m68k_read32(ctx, array + 4 * i);
    out[n] = NULL;
    return out;
}

/* Creation leaves GTCY_Active as it is, and cycleclass.c draws
   labels[active] past the end of the array: an active choice beyond the
   labels becomes the first, as the setter of the class makes it. */
static void clamp_active(struct TagItem *tags)
{
    struct TagItem *t, *active = NULL;
    STRPTR *labels = NULL;
    IPTR n = 0;

    for (t = tags; t->ti_Tag != TAG_DONE; t++)
        if (t->ti_Tag == GTCY_Active)
            active = t;
        else if (t->ti_Tag == GTCY_Labels)
            labels = (STRPTR *)t->ti_Data;
    while (labels && labels[n])
        n++;
    /* the class keeps the choice as a UWORD */
    if (active && labels && (UWORD)active->ti_Data >= n)
        active->ti_Data = 0;
}

/* The tags of the program that GadTools uses for a kind, as native ones:
   through TAG_MORE, TAG_SKIP and TAG_IGNORE, of a repeated tag only the
   one that GadTools uses, in their order, which matters for GTPA_Depth
   and GTPA_NumColors. Returns 1, 0 for 68k input that the bridge refuses
   (cv->why says why), or -1 without memory. */
static int convert_tags(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b, ULONG kind,
                        ULONG list, struct Conversion *cv)
{
    struct KaffeineTagWalk walk;
    struct Use *uses;
    ULONG item, count = 0, used = 0, i, j, len;
    /* the colours of a palette and the MaxChars of the string class, as
       GadTools works them out */
    IPTR colors = 2;
    BOOL odd_depth = FALSE, last = kind < 16 && !(FIRST_KINDS & K(kind));
    WORD maxchars = kind == INTEGER_KIND ? 10 + 1 : 64 + 1;
    int step, type, res = 1;
    char *copy;

    Kaffeine_TagWalkInit(&walk, list);
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
        if (tag_type(m68k_read32(ctx, item), kind) >= 0 && ++count > MAX_TAGS)
            break;
    if (step == KAFFEINE_TAG_BAD || count > MAX_TAGS)
    {
        cv->why = "CreateGadgetA(): a tag list outside guest memory or without an end";
        return 0;
    }
    if (!(uses = AllocVec((count + 1) * sizeof(*uses), MEMF_ANY | MEMF_CLEAR)))
        return -1;
    Kaffeine_TagWalkInit(&walk, list);
    while (used < count && Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM)
        if ((type = tag_type(m68k_read32(ctx, item), kind)) >= 0)
        {
            uses[used].tag = m68k_read32(ctx, item);
            uses[used].data = m68k_read32(ctx, item + 4);
            uses[used++].type = type;
        }
    /* Of a repeated tag GadTools uses the first or the last: GetTagData()
       for GT_Underscore and the first kinds, a loop for the others. */
    for (i = 0; i < used; i++)
        for (j = 0; j < used; j++)
            if (j != i && uses[j].tag == uses[i].tag &&
                (last && uses[i].tag != GT_Underscore ? j > i : j < i))
            {
                uses[i].type = -1;
                break;
            }
    for (i = 0; i < used; i++)
        if (uses[i].type < 0)
            continue;
        else if (uses[i].tag == GTST_MaxChars || uses[i].tag == GTIN_MaxChars)
            maxchars = (WORD)((WORD)uses[i].data + 1);
        else if (uses[i].tag == GTPA_Depth)
        {
            odd_depth = uses[i].data >= 64;
            colors = odd_depth ? 0 : (IPTR)1 << uses[i].data;
        }
        else if (uses[i].tag == GTPA_NumColors)
        {
            odd_depth = FALSE;
            colors = (IPTR)(SIPTR)(LONG)uses[i].data;
        }
        else if (uses[i].tag == GTTX_Text)
            cv->text = uses[i].data;
        else if (uses[i].tag == GTTX_CopyText)
            cv->copytext = uses[i].data != 0;
        else if (uses[i].tag == GTNM_Number)
            cv->number = uses[i].data;
        else if (uses[i].tag == GTPA_ColorOffset)
            cv->offset = (UBYTE)uses[i].data;
        else if (uses[i].tag == GTPA_Color)
            cv->pen = (UBYTE)uses[i].data;
        else if (uses[i].tag == GTPA_IndicatorWidth)
            cv->indw = (UWORD)uses[i].data;
        else if (uses[i].tag == GTPA_IndicatorHeight)
            cv->indh = (UWORD)uses[i].data;
        else if (uses[i].tag == GTLV_ItemHeight)
            cv->itemheight = uses[i].data;
        else if (uses[i].tag == LAYOUTA_Spacing)
            cv->spacing = uses[i].data;
    /* the class keeps a UWORD; of an odd depth the shift of the CPU makes
       a count that the bridge cannot know, more than it takes a table for */
    cv->colors = odd_depth ? MAX_COLORS + 1 : (UWORD)colors;

    /* stringclass.c makes GTST_MaxChars and GTIN_MaxChars one more as a
       WORD, the size of the buffers that strgclass.c allocates */
    if ((kind == STRING_KIND || kind == INTEGER_KIND) && maxchars <= 0)
    {
        cv->why = "CreateGadgetA(): a MaxChars that GadTools cannot take";
        FreeVec(uses);
        return 0;
    }
    if (!(cv->tags = AllocVec((used + 1) * sizeof(*cv->tags), MEMF_ANY | MEMF_CLEAR)))
        res = -1;
    for (i = j = 0; res > 0 && i < used; i++)
    {
        ULONG data = uses[i].data;
        IPTR value = 0;
        LONG l;

        switch (uses[i].type)
        {
        case -1:
            continue;
        case T_SCALAR:
            value = (IPTR)(SIPTR)(LONG)data;
            break;
        case T_PEN:
            /* textclass.c reads dri_Pens[pen], an array of NUMDRIPENS: a
               pen number of AmigaOS beyond it stays out, and the class
               keeps its own pen */
            if (data >= NUMDRIPENS)
            {
                bug("[kaffeine] CreateGadgetA(): pen %lu beyond the DrawInfo pens, left out\n",
                    (unsigned long)data);
                continue;
            }
            value = data;
            break;
        case T_KEY:
            /* makeitext() would find a NUL at the end of every label */
            value = (UBYTE)data ? (IPTR)(SIPTR)(LONG)data : 1;
            break;
        case T_STRING:
            if (data && guest_strlen(ctx, data) < 0)
            {
                cv->why = "CreateGadgetA(): a text that is not a string in guest memory";
                res = 0;
            }
            value = data ? (IPTR)(ctx->mem + data) : 0;
            break;
        case T_COPY:
            if (!data)
                break;
            if ((l = guest_strlen(ctx, data)) < 0)
            {
                cv->why = "CreateGadgetA(): a GTST_String that is not a string in guest memory";
                res = 0;
                break;
            }
            /* the string class copies it into a buffer of MaxChars */
            len = (ULONG)l;
            if (len > (ULONG)maxchars - 1)
                len = (ULONG)maxchars - 1;
            if (!(copy = keep(&cv->temp, len + 1, data)))
            {
                res = -1;
                break;
            }
            memcpy(copy, ctx->mem + data, len);
            value = (IPTR)copy;
            break;
        case T_FORMAT:
            if (!data)
            {
                /* the text class would format with NULL; a slider without
                   a format has no level display */
                if (uses[i].tag == GTNM_Format)
                {
                    cv->why = "CreateGadgetA(): a GTNM_Format of NULL";
                    res = 0;
                }
                break;
            }
            if ((l = guest_strlen(ctx, data)) < 0 || !format_ok((const char *)ctx->mem + data))
            {
                cv->why = "CreateGadgetA(): a format that is not one integer or longer than "
                          "GadTools takes";
                res = 0;
                break;
            }
            if (!(copy = keep(&b->kept, (ULONG)l + 1, data)))
            {
                res = -1;
                break;
            }
            memcpy(copy, ctx->mem + data, (ULONG)l);
            value = (IPTR)copy;
            break;
        case T_LABELS:
            if (data && !(value = (IPTR)labels(ctx, &b->kept, data, &res)))
                cv->why = "CreateGadgetA(): labels that are not strings in guest memory";
            /* cycleclass.c draws labels[active] of an array, even of an
               empty one; NULL it leaves alone */
            else if (uses[i].tag == GTCY_Labels && value && !((STRPTR *)value)[0])
            {
                cv->why = "CreateGadgetA(): an empty label array of a cycle";
                res = 0;
            }
            break;
        case T_COLORS:
            if (!data)
                break;
            /* the class looks for the pen of GTPA_Color in its colours, and
               reads the table as palette_room() says */
            if (cv->colors > MAX_COLORS || !guest_range(ctx, data, palette_room(cv->colors, cv->offset)))
            {
                cv->why = "CreateGadgetA(): a colour table outside guest memory, or for more "
                          "colours than GadTools takes";
                res = 0;
            }
            cv->table = data;
            value = (IPTR)(ctx->mem + data);
            break;
        case T_HOOK:
            if (data)
            {
                cv->why = "CreateGadgetA(): hooks and functions of 68k code are not supported yet";
                res = 0;
            }
            break;
        case T_ENTRIES:
            /* NULL and ~0 as they are, else a native copy that the batch
               keeps while the class holds it */
            if (data == 0xFFFFFFFF)
                value = ~(IPTR)0;
            else if (data && !(cv->entries = entries_new(ctx, data, &res)))
                cv->why = "CreateGadgetA(): a List that is not one in guest memory, with a name "
                          "that is not a string, or with more Nodes than a listview counts";
            else if (data)
                value = (IPTR)&cv->entries->list;
            break;
        case T_SHOWN:
            /* NULL for a selection that the listview shows itself, ~0 for
               none, else the string gadget of a CreateGadgetA(STRING_KIND)
               that the class writes the selection into */
            if (data == 0xFFFFFFFF)
                value = ~(IPTR)0;
            else if (data)
            {
                struct Gadget *sg = NULL;
                struct KaffeineGadgetBatch *sb = batch_of(ctx, data, &sg);

                if (!sb || sg != sb->last || sb->kind != STRING_KIND)
                {
                    cv->why = "CreateGadgetA(): a GTLV_ShowSelected that is not a string gadget of "
                              "GadTools";
                    res = 0;
                }
                /* gadgets.c would move it on the window */
                else if (sb->window)
                {
                    cv->why = "CreateGadgetA(): a GTLV_ShowSelected string gadget on a window";
                    res = 0;
                }
                else
                {
                    value = (IPTR)sg;
                    cv->showsel = sb;
                }
            }
            break;
        case T_WIDTH:
            /* gadgets.c takes the second gadget after the first arrow as
               the prop of the scroller, which a width of 0 gives no arrows;
               a negative width, no width either, stops the program too */
            if ((WORD)(UWORD)data <= 0)
            {
                cv->why = "CreateGadgetA(): a GTLV_ScrollWidth below 1, which at 0 leaves a "
                          "listview's scroller without arrows";
                res = 0;
            }
            value = (IPTR)(SIPTR)(LONG)data;
            break;
        }
        cv->tags[j].ti_Tag = uses[i].tag;
        cv->tags[j++].ti_Data = value;
    }
    if (cv->tags)
    {
        cv->tags[j].ti_Tag = TAG_DONE;
        if (res > 0 && kind == CYCLE_KIND)
            clamp_active(cv->tags);
    }
    FreeVec(uses);
    return res;
}

/* The choice of a palette as its class keeps it after a GTPA_Color of pen:
   with a table where the pen is in it, as far as its colours go, else the
   pen itself; without one the pen less the offset. The caller checked the
   room of the table. */
static UBYTE palette_index(struct KaffeineContext *ctx, ULONG table, ULONG colors, UBYTE offset,
                           UBYTE pen)
{
    ULONG i;

    if (!table)
        return (UBYTE)(pen - offset);
    for (i = 0; i < colors; i++)
        if (ctx->mem[table + i] == pen)
            return (UBYTE)i;
    return pen;
}

/* gadgets.c gives a new listview GA_Disabled before its scroller exists,
   and GTLV_Top and GTLV_MakeVisible before its List, which resets the top:
   they reach the class again, in that order, as AmigaOS takes them at
   creation. No window: nothing draws. */
static void listview_again(struct Gadget *gad, const struct TagItem *tags)
{
    static const ULONG order[] = { GA_Disabled, GTLV_Top, GTLV_MakeVisible };
    struct TagItem again[4];
    const struct TagItem *t;
    ULONG i, n = 0;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        for (t = tags; t->ti_Tag != TAG_DONE; t++)
            if (t->ti_Tag == order[i])
            {
                again[n++] = *t;
                break;
            }
    again[n].ti_Tag = TAG_DONE;
    again[n].ti_Data = 0;
    if (n)
        GT_SetGadgetAttrsA(gad, NULL, NULL, again);
}

/* What the bridge keeps of a new batch: what GT_GetGadgetAttrsA() gives for
   the gadget of a TEXT_KIND or NUMBER_KIND batch, its GTNM_Number, its
   GTTX_Text, or with GTTX_CopyText a copy of the text in guest memory, as
   AmigaOS gives its own copy: the program may reuse its buffer; and of a
   palette what the setter checks a new table against. FALSE without guest
   memory for the copy. */
static BOOL remember(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b, const struct Conversion *cv)
{
    ULONG copy, size;

    b->table = cv->table;
    b->colors = cv->colors;
    b->offset = cv->offset;
    b->indw = cv->indw;
    b->indh = cv->indh;
    /* the class always gets a GTPA_Color from GadTools, and lays itself
       out at creation, which takes back a choice beyond its colours */
    b->color = palette_index(ctx, cv->table, cv->colors, cv->offset, cv->pen);
    if ((LONG)b->color > (LONG)cv->colors - 1)
        b->color = 0;
    if (b->kind == NUMBER_KIND)
        b->shown = cv->number;
    else if (b->kind == TEXT_KIND)
    {
        b->shown = cv->text;
        if (cv->copytext && cv->text)
        {
            /* convert_tags() found it a string in guest memory */
            size = (ULONG)guest_strlen(ctx, cv->text) + 1;
            if (!(copy = Kaffeine_MirrorNode(ctx, &b->mirror, b, NODE_TEXT, size, NULL)))
                return FALSE;
            memcpy(ctx->mem + copy, ctx->mem + cv->text, size);
            b->shown = copy;
        }
    }
    return TRUE;
}

/* ── Attributes ──────────────────────────────────────────────────── */

enum
{
    G_SCALAR,   /* a number or a flag, as the class gives it */
    G_LABELS,   /* GTCY_Labels: the program's own array */
    G_TABLE,    /* GTPA_ColorTable: the program's own table, in guest memory */
    G_BUFFER,   /* GTST_String: the buffer of the 68k StringInfo */
    G_SHOWN,    /* GTTX_Text and GTNM_Number: what the bridge converted */
    G_ENTRIES   /* GTLV_Labels: the program's List, NULL or ~0 */
};

/* The attributes that GT_GetGadgetAttrsA() fills in for each kind, as its
   autodoc gives them. AROS's cycle and palette classes fill theirs in
   without counting them (quiet), and its text class has none. */
static const struct { ULONG tag; UBYTE conv; UBYTE quiet; UWORD kinds; } getters[] =
{
    { GA_Disabled,      G_SCALAR, FALSE, K(BUTTON_KIND) | K(CHECKBOX_KIND) | K(CYCLE_KIND) |
                                         K(LISTVIEW_KIND) | K(MX_KIND) | K(PALETTE_KIND) |
                                         PROP_KINDS | STRING_KINDS },
    { GTCB_Checked,     G_SCALAR, FALSE, K(CHECKBOX_KIND) },
    { GTCY_Active,      G_SCALAR, TRUE,  K(CYCLE_KIND) },
    { GTCY_Labels,      G_LABELS, TRUE,  K(CYCLE_KIND) },
    { GTLV_Top,         G_SCALAR, FALSE, K(LISTVIEW_KIND) },
    { GTLV_Labels,      G_ENTRIES, FALSE, K(LISTVIEW_KIND) },
    { GTLV_Selected,    G_SCALAR, FALSE, K(LISTVIEW_KIND) },
    { NDK_GTLV_Total,   G_SCALAR, FALSE, K(LISTVIEW_KIND) },
    { NDK_GTLV_Visible, G_SCALAR, FALSE, K(LISTVIEW_KIND) },
    { GTMX_Active,      G_SCALAR, FALSE, K(MX_KIND) },
    { GTPA_Color,       G_SCALAR, TRUE,  K(PALETTE_KIND) },
    { GTPA_ColorOffset, G_SCALAR, TRUE,  K(PALETTE_KIND) },
    { GTPA_ColorTable,  G_TABLE,  TRUE,  K(PALETTE_KIND) },
    { GTTX_Text,        G_SHOWN,  FALSE, K(TEXT_KIND) },
    { GTNM_Number,      G_SHOWN,  FALSE, K(NUMBER_KIND) },
    { GTSL_Min,         G_SCALAR, FALSE, K(SLIDER_KIND) },
    { GTSL_Max,         G_SCALAR, FALSE, K(SLIDER_KIND) },
    { GTSL_Level,       G_SCALAR, FALSE, K(SLIDER_KIND) },
    { GTSC_Top,         G_SCALAR, FALSE, K(SCROLLER_KIND) },
    { GTSC_Total,       G_SCALAR, FALSE, K(SCROLLER_KIND) },
    { GTSC_Visible,     G_SCALAR, FALSE, K(SCROLLER_KIND) },
    { GTST_String,      G_BUFFER, FALSE, K(STRING_KIND) },
    { GTIN_Number,      G_SCALAR, FALSE, K(INTEGER_KIND) }
};

/* The getter of a tag for the gadget g of the batch b, or -1: only the
   gadget of a CreateGadgetA() call has attributes, not the context nor
   what GadTools puts before it. */
static int getter(struct KaffeineGadgetBatch *b, struct Gadget *g, ULONG tag)
{
    ULONG i;

    if (g != b->last || b->kind >= 16)
        return -1;
    for (i = 0; i < sizeof(getters) / sizeof(getters[0]); i++)
        if (getters[i].tag == tag)
            return (getters[i].kinds & K(b->kind)) ? (int)i : -1;
    return -1;
}

/* A native pointer into guest memory as a guest address, else 0 */
static ULONG guest_pointer(struct KaffeineContext *ctx, IPTR p)
{
    const UBYTE *q = (const UBYTE *)p;

    return q > ctx->mem && q < ctx->mem + ctx->mem_size ? (ULONG)(q - ctx->mem) : 0;
}

/* Fill in an attribute at storage, a guest LONG: TRUE for one filled in,
   which the count includes. A pointer that the bridge cannot give the
   program as one of its own is not filled in. */
static BOOL get(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b, struct Gadget *g, int i,
                ULONG storage)
{
    struct KaffeineMirrorNode *n;
    struct TagItem tags[2];
    IPTR value = 0;
    ULONG out;

    if (getters[i].conv == G_SHOWN)
        out = b->shown;
    else
    {
        /* AROS asks for these under its own numbers */
        tags[0].ti_Tag = getters[i].tag == NDK_GTLV_Total ? GTLV_Total
                       : getters[i].tag == NDK_GTLV_Visible ? GTLV_Visible : getters[i].tag;
        tags[0].ti_Data = (IPTR)&value;
        tags[1].ti_Tag = TAG_DONE;
        tags[1].ti_Data = 0;
        if (!GT_GetGadgetAttrsA(g, NULL, NULL, tags) && !getters[i].quiet)
            return FALSE;
        switch (getters[i].conv)
        {
        case G_LABELS:
            out = kept_guest(b, (APTR)value);
            break;
        case G_TABLE:
            out = guest_pointer(ctx, value);
            break;
        case G_BUFFER:
            n = value ? Kaffeine_MirrorFind(&b->mirror, (APTR)value, NODE_BUFFER) : NULL;
            out = n ? n->guest : 0;
            break;
        case G_ENTRIES:
            out = value == ~(IPTR)0 ? 0xFFFFFFFF
                : b->entries && value == (IPTR)&b->entries->list ? b->entries->guest : 0;
            break;
        default:
            out = (ULONG)value;
            break;
        }
        if (value && getters[i].conv != G_SCALAR && !out)
        {
            bug("[kaffeine] GT_GetGadgetAttrsA(): a native pointer for tag %08lx, not filled in\n",
                (unsigned long)getters[i].tag);
            return FALSE;
        }
    }
    m68k_write32(ctx, storage, out);
    return TRUE;
}

/* ── Changing attributes ─────────────────────────────────────────── */

enum
{
    S_SCALAR,   /* a number or a flag, sign-extended as a LONG */
    S_PEN,      /* a pen of the text class, which indexes the DrawInfo pens */
    S_LABELS,   /* GTCY_Labels: a new native array of the program's strings */
    S_TABLE,    /* GTPA_ColorTable: the program's table, in guest memory */
    S_OFFSET,   /* GTPA_ColorOffset: where the class starts in the table */
    S_FORMAT,   /* a RawDoFmt() format of one integer, copied */
    S_TEXT,     /* GTTX_Text: a string of the program, which the class keeps */
    S_STRING,   /* GTST_String: copied by the string class into its buffer */
    S_INTEGER,  /* GTIN_Number: written by the string class into its buffer */
    S_HOOK,     /* GTSL_DispFunc: a function of 68k code, only NULL */
    S_ENTRIES   /* GTLV_Labels: a 68k List, NULL or ~0 */
};

/* The attributes that GT_SetGadgetAttrsA() changes for each kind, as its
   autodoc gives them */
static const struct { ULONG tag; UBYTE conv; UWORD kinds; } setters[] =
{
    { GA_Disabled,        S_SCALAR, K(BUTTON_KIND) | K(CHECKBOX_KIND) | K(CYCLE_KIND) |
                                    K(LISTVIEW_KIND) | K(MX_KIND) | K(PALETTE_KIND) |
                                    PROP_KINDS | STRING_KINDS },
    { GTCB_Checked,       S_SCALAR, K(CHECKBOX_KIND) },
    { GTCY_Labels,        S_LABELS, K(CYCLE_KIND) },
    { GTCY_Active,        S_SCALAR, K(CYCLE_KIND) },
    { GTLV_Top,           S_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_MakeVisible,   S_SCALAR, K(LISTVIEW_KIND) },
    { GTLV_Labels,        S_ENTRIES, K(LISTVIEW_KIND) },
    { GTLV_Selected,      S_SCALAR, K(LISTVIEW_KIND) },
    { GTMX_Active,        S_SCALAR, K(MX_KIND) },
    { GTIN_Number,        S_INTEGER, K(INTEGER_KIND) },
    { GTNM_Number,        S_SCALAR, K(NUMBER_KIND) },
    { GTNM_Format,        S_FORMAT, K(NUMBER_KIND) },
    /* GTTX_FrontPen, GTTX_BackPen and GTTX_Justification are these */
    { GTNM_FrontPen,      S_PEN,    TEXT_KINDS },
    { GTNM_BackPen,       S_PEN,    TEXT_KINDS },
    { GTNM_Justification, S_SCALAR, TEXT_KINDS },
    { GTPA_Color,         S_SCALAR, K(PALETTE_KIND) },
    { GTPA_ColorOffset,   S_OFFSET, K(PALETTE_KIND) },
    { GTPA_ColorTable,    S_TABLE,  K(PALETTE_KIND) },
    { GTSC_Top,           S_SCALAR, K(SCROLLER_KIND) },
    { GTSC_Total,         S_SCALAR, K(SCROLLER_KIND) },
    { GTSC_Visible,       S_SCALAR, K(SCROLLER_KIND) },
    { GTSL_Min,           S_SCALAR, K(SLIDER_KIND) },
    { GTSL_Max,           S_SCALAR, K(SLIDER_KIND) },
    { GTSL_Level,         S_SCALAR, K(SLIDER_KIND) },
    { GTSL_LevelFormat,   S_FORMAT, K(SLIDER_KIND) },
    { GTSL_DispFunc,      S_HOOK,   K(SLIDER_KIND) },
    { GTSL_Justification, S_SCALAR, K(SLIDER_KIND) },
    { GTST_String,        S_STRING, K(STRING_KIND) },
    { GTTX_Text,          S_TEXT,   K(TEXT_KIND) }
};

/* The setter of a tag for the gadget g of the batch b, or -1, as getter() */
static int setter(struct KaffeineGadgetBatch *b, struct Gadget *g, ULONG tag)
{
    ULONG i;

    if (g != b->last || b->kind >= 16)
        return -1;
    for (i = 0; i < sizeof(setters) / sizeof(setters[0]); i++)
        if (setters[i].tag == tag)
            return (setters[i].kinds & K(b->kind)) ? (int)i : -1;
    return -1;
}

/* The bytes that the buffer of the string gadget g takes: its MaxChars, but
   no more than the buffer that its class made with the MaxChars of then,
   which the guest copy of the buffer has the size of (mirror_stringinfo());
   0 for none */
static ULONG string_room(struct KaffeineGadgetBatch *b, struct Gadget *g)
{
    struct StringInfo *si = g->SpecialInfo;
    struct KaffeineMirrorNode *n;

    if (!si || !si->Buffer || si->MaxChars <= 0 ||
        !(n = Kaffeine_MirrorFind(&b->mirror, si->Buffer, NODE_BUFFER)))
        return 0;
    return (ULONG)si->MaxChars < n->size ? (ULONG)si->MaxChars : n->size;
}

/* The native tag of one attribute for the setter: 1, 0 with *why for one
   that the bridge refuses, -1 without memory, 2 for one that it leaves out.
   Copies that the class keeps go on stage, for the batch if the call goes
   through; copies for the call alone on temp; copies of Lists on lists. */
static int set_tag(struct KaffeineContext *ctx, struct KaffeineGadgetBatch *b, struct Gadget *g,
                   int at, ULONG data, struct KaffeineGadgetKept **stage,
                   struct KaffeineGadgetKept **temp, struct KaffeineEntries **lists, IPTR *value,
                   const char **why)
{
    struct KaffeineEntries *e;
    ULONG room;
    STRPTR *array;
    char *copy;
    LONG l;
    int res = 1;

    switch (setters[at].conv)
    {
    case S_SCALAR:
    case S_OFFSET:
        *value = (IPTR)(SIPTR)(LONG)data;
        return 1;
    case S_PEN:
        /* as at creation */
        if (data >= NUMDRIPENS)
        {
            bug("[kaffeine] GT_SetGadgetAttrsA(): pen %lu beyond the DrawInfo pens, left out\n",
                (unsigned long)data);
            return 2;
        }
        *value = data;
        return 1;
    case S_LABELS:
        if (!data)
        {
            *value = 0;
            return 1;
        }
        if (!(array = labels(ctx, stage, data, &res)))
        {
            *why = "GT_SetGadgetAttrsA(): labels that are not strings in guest memory";
            return res;
        }
        if (!array[0])
        {
            *why = "GT_SetGadgetAttrsA(): an empty label array of a cycle";
            return 0;
        }
        *value = (IPTR)array;
        return 1;
    case S_TABLE:
        *value = data ? (IPTR)(ctx->mem + data) : 0;
        return 1;
    case S_FORMAT:
        if (!data)
        {
            /* the text class would format with NULL; the slider leaves its
               level display alone */
            if (setters[at].tag == GTNM_Format)
            {
                *why = "GT_SetGadgetAttrsA(): a GTNM_Format of NULL";
                return 0;
            }
            *value = 0;
            return 1;
        }
        if ((l = guest_strlen(ctx, data)) < 0 || !format_ok((const char *)ctx->mem + data))
        {
            *why = "GT_SetGadgetAttrsA(): a format that is not one integer or longer than GadTools takes";
            return 0;
        }
        if (!(copy = keep(stage, (ULONG)l + 1, data)))
            return -1;
        memcpy(copy, ctx->mem + data, (ULONG)l);
        *value = (IPTR)copy;
        return 1;
    case S_TEXT:
        if (data && guest_strlen(ctx, data) < 0)
        {
            *why = "GT_SetGadgetAttrsA(): a text that is not a string in guest memory";
            return 0;
        }
        *value = data ? (IPTR)(ctx->mem + data) : 0;
        return 1;
    case S_STRING:
        /* strgclass.c copied the text into its buffer with strcpy() before
           it cut texts itself, and an empty one for NULL: cut to its room */
        if (!data || !(room = string_room(b, g)))
        {
            *value = 0;
            return 1;
        }
        if ((l = guest_strlen(ctx, data)) < 0)
        {
            *why = "GT_SetGadgetAttrsA(): a GTST_String that is not a string in guest memory";
            return 0;
        }
        if ((ULONG)l > room - 1)
            l = (LONG)(room - 1);
        if (!(copy = keep(temp, (ULONG)l + 1, data)))
            return -1;
        memcpy(copy, ctx->mem + data, (ULONG)l);
        *value = (IPTR)copy;
        return 1;
    case S_INTEGER:
        /* strgclass.c writes the number into its buffer as far as its
           MaxChars goes, and on older systems before the buffer for a
           MaxChars below 1 */
        room = string_room(b, g);
        if (!room || room != (ULONG)((struct StringInfo *)g->SpecialInfo)->MaxChars)
        {
            *why = "GT_SetGadgetAttrsA(): an integer gadget with a MaxChars beyond its buffer";
            return 0;
        }
        *value = (IPTR)(SIPTR)(LONG)data;
        return 1;
    case S_HOOK:
        if (data)
        {
            *why = "GT_SetGadgetAttrsA(): hooks and functions of 68k code are not supported yet";
            return 0;
        }
        *value = 0;
        return 1;
    case S_ENTRIES:
        /* NULL and ~0 as they are, else a new native copy, also of the
           List that the listview holds: the program detached it to change
           it, or changed it while attached, which AmigaOS draws as it is */
        if (data == 0xFFFFFFFF || !data)
        {
            *value = data ? ~(IPTR)0 : 0;
            return 1;
        }
        if (!(e = entries_new(ctx, data, &res)))
        {
            *why = "GT_SetGadgetAttrsA(): a List that is not one in guest memory, with a name that "
                   "is not a string, or with more Nodes than a listview counts";
            return res;
        }
        e->next = *lists;
        *lists = e;
        *value = (IPTR)&e->list;
        return 1;
    }
    return 2;
}

/* ── GadTools ────────────────────────────────────────────────────── */

/* gadtools -114: CreateContext(A0=glistpointer) -> D0: the 68k context
   gadget, which *glistpointer gets too */
IPTR Kaffeine_CreateGadgetContext(struct KaffeineContext *ctx, void *cpu)
{
    ULONG glistptr = THUNK_A(0), guest;
    struct Gadget *native = NULL, *context;
    struct KaffeineGadgetBatch *b;

    if (!guest_range(ctx, glistptr, 4))
        return refuse(ctx, "CreateContext(): a gadget pointer outside guest memory");
    context = CreateContext(&native);
    b = context ? batch_new(ctx) : NULL;
    if (b)
    {
        b->first = b->last = context;
        b->kind = KIND_CONTEXT;
        guest = mirror_gadget(ctx, b, context, SPECIAL_NONE, NULL);
        if (guest && !b->mirror.failed)
        {
            m68k_write32(ctx, guest + G_GD_NEXTGADGET, 0);
            m68k_write32(ctx, glistptr, guest);
            return guest;
        }
        batch_free(ctx, b);
    }
    if (context)
    {
        bug("[kaffeine] CreateContext(): no memory for the 68k context\n");
        FreeGadgets(context);
    }
    m68k_write32(ctx, glistptr, 0);
    return 0;
}

/* gadtools -30: CreateGadgetA(D0=kind, A0=previous, A1=newgad, A2=taglist)
   -> D0: the 68k gadget, the last one that GadTools made */
IPTR Kaffeine_CreateGadgetA(struct KaffeineContext *ctx, void *cpu)
{
    ULONG kind = THUNK_D(0), prev = THUNK_A(0), ng = THUNK_A(1), text, ta;
    struct Conversion cv = { NULL, NULL, NULL, 0, 0, FALSE, 0, 0, 0, 0, NULL, 0, 0, NULL, 0, 0 };
    struct KaffeineGadgetBatch *pb, *b;
    struct Gadget *previous = NULL, *gad, *g;
    struct NewGadget nng;
    BOOL nomem = FALSE;
    LONG label = 0;
    int converted;

    /* GadTools returns NULL for these before anything else, so that a
       program may create its gadgets without a check after each. */
    if (!prev || !ng)
        return 0;
    if (!guest_range(ctx, ng, G_NEWGADGET_SIZE))
        return refuse(ctx, "CreateGadgetA(): a NewGadget outside guest memory");
    if (!m68k_read32(ctx, ng + G_NG_VISUALINFO))
        return 0;
    /* What GadTools cuts off the list at previous is whole batches. */
    if (!(pb = batch_of(ctx, prev, &previous)) || previous != pb->last)
        return refuse(ctx, "CreateGadgetA(): a previous gadget that CreateContext() or "
                           "CreateGadgetA() did not return");
    /* GadTools would link the new gadget into the window's list. */
    if (pb->window)
        return refuse(ctx, "CreateGadgetA(): a previous gadget on a window");

    memset(&nng, 0, sizeof(nng));
    if (!(nng.ng_VisualInfo = Kaffeine_VisualInfoNative(ctx, m68k_read32(ctx, ng + G_NG_VISUALINFO))))
        return refuse(ctx, "CreateGadgetA(): not a VisualInfo of GetVisualInfoA()");
    text = m68k_read32(ctx, ng + G_NG_GADGETTEXT);
    if (text && ((label = guest_strlen(ctx, text)) < 0 || label >= MAX_LABEL))
        return refuse(ctx, "CreateGadgetA(): a label that is not a string in guest memory, or "
                           "longer than GadTools takes");
    nng.ng_LeftEdge = (WORD)m68k_read16(ctx, ng + G_NG_LEFTEDGE);
    nng.ng_TopEdge = (WORD)m68k_read16(ctx, ng + G_NG_TOPEDGE);
    nng.ng_Width = (WORD)m68k_read16(ctx, ng + G_NG_WIDTH);
    nng.ng_Height = (WORD)m68k_read16(ctx, ng + G_NG_HEIGHT);
    nng.ng_GadgetText = text ? (CONST_STRPTR)ctx->mem + text : NULL;
    nng.ng_GadgetID = m68k_read16(ctx, ng + G_NG_GADGETID);
    nng.ng_Flags = m68k_read32(ctx, ng + G_NG_FLAGS);
    nng.ng_UserData = (APTR)(IPTR)m68k_read32(ctx, ng + G_NG_USERDATA);

    if (!(b = batch_new(ctx)))
        return 0;
    b->kind = kind;
    /* GadTools copies the font of the label, and the classes may keep the
       TextAttr: a native copy stays with the batch. */
    if ((ta = m68k_read32(ctx, ng + G_NG_TEXTATTR)) &&
        (!(nng.ng_TextAttr = Kaffeine_MirrorNativeFont(ctx, &b->mirror, ta, &nomem)) ||
         (nng.ng_TextAttr->ta_Name && strlen((const char *)nng.ng_TextAttr->ta_Name) >= MAX_LABEL)))
    {
        batch_free(ctx, b);
        return nomem ? 0 : refuse(ctx, "CreateGadgetA(): a TextAttr outside guest memory, or "
                                       "with a longer name than GadTools takes");
    }

    converted = convert_tags(ctx, b, kind, THUNK_A(2), &cv);
    /* the batch keeps the copy of a listview's List, and frees it with
       itself if anything below fails */
    b->entries = cv.entries;
    /* listviewclass.c divides by the height of an item plus the spacing,
       WORDs of the tags, or for no item height of the font: a TextAttr of
       no height, or a negative spacing, could make that 0 */
    if (converted > 0 && kind == LISTVIEW_KIND &&
        ((cv.itemheight ? (WORD)(UWORD)cv.itemheight
                        : nng.ng_TextAttr ? (WORD)nng.ng_TextAttr->ta_YSize : 1) <= 0 ||
         (WORD)(UWORD)cv.spacing < 0))
    {
        cv.why = "CreateGadgetA(): listview items of no height, or with a negative spacing";
        converted = 0;
    }
    if (converted <= 0)
    {
        FreeVec(cv.tags);
        kept_free(&cv.temp);
        batch_free(ctx, b);
        return converted ? 0 : refuse(ctx, cv.why);
    }

    gad = CreateGadgetA(kind, previous, &nng, cv.tags);
    if (gad && kind == LISTVIEW_KIND)
        listview_again(gad, cv.tags);
    /* gadgets.c moves the string gadget of GTLV_ShowSelected below the
       listview, also when it fails afterwards, and the class writes the
       selection into it */
    if (cv.showsel)
        settle(ctx, cv.showsel);
    FreeVec(cv.tags);
    kept_free(&cv.temp);
    if (gad && (!mirror_new(ctx, b, previous, gad, nng.ng_UserData) || !remember(ctx, b, &cv)))
    {
        /* the gadgets of this call go again, as when GadTools fails */
        struct Gadget *first = previous->NextGadget;

        bug("[kaffeine] CreateGadgetA(): no memory for the 68k gadgets\n");
        previous->NextGadget = NULL;
        FreeGadgets(first);
        gad = NULL;
    }
    if (!gad)
        batch_free(ctx, b);
    else
        b->showsel = cv.showsel;
    link(ctx, previous);
    for (g = gad ? b->first : NULL; g; g = g == gad ? NULL : g->NextGadget)
        link(ctx, g);
    return gad ? Kaffeine_ShadowGuest(ctx, gad, KAFFEINE_SHADOW_GADGET) : 0;
}

/* gadtools -36: FreeGadgets(A0=glist) */
/* GadTools keeps the gadget of a GADGETDOWN active in the first context on
   the window, as gt_filterimsg.c finds it, until a GADGETUP, which does not
   come when the button goes up outside an arrow. It forgets it at a
   GADGETUP without a gadget, which its filter takes here as from a window
   that holds only context c; FALSE when the filter had no memory for its
   copy of that message, while the program holds the one of the context,
   and GadTools still keeps the gadget. */
static BOOL forget_held(struct KaffeineGadgetBatch *c)
{
    struct Window w;
    struct ExtIntuiMessage m;
    struct IntuiMessage *copy;

    memset(&w, 0, sizeof(w));
    memset(&m, 0, sizeof(m));
    w.FirstGadget = c->first;
    m.eim_IntuiMessage.Class = IDCMP_GADGETUP;
    m.eim_IntuiMessage.IDCMPWindow = &w;
    if (!(copy = GT_FilterIMsg(&m.eim_IntuiMessage)))
        return FALSE;
    GT_PostFilterIMsg(copy);
    c->held = NULL;
    return TRUE;
}

IPTR Kaffeine_FreeGadgets(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), n, at, node;
    struct Gadget *g = NULL, *h, *s;
    struct KaffeineGadgetBatch *b = batch_of(ctx, guest, &g), *hb, *kb, *next;
    const char *why = NULL;

    if (!b)
    {
        if (guest)
            bug("[kaffeine] FreeGadgets(%08lx): not a gadget of GadTools, ignored\n",
                (unsigned long)guest);
        return 0;
    }
    if (g != b->first)
        return refuse(ctx, "FreeGadgets(): a gadget that GadTools made after another one in "
                           "the same call");
    /* GadTools frees the GadTools gadgets of the program's list from there
       on, as AmigaOS 3.2.3 does: those of each call as GadTools linked
       them, and from the last of a call on where its 68k NextGadget leads,
       to its end where the program cut it, to the gadget that followed on
       a window, where RemoveGList() leaves it, or through gadgets of the
       program's own, which GadTools passes. */
    for (n = 0, hb = b; hb && !why;)
    {
        hb->doomed = TRUE;
        at = m68k_read32(ctx, Kaffeine_ShadowGuest(ctx, hb->last, KAFFEINE_SHADOW_GADGET) +
                                  G_GD_NEXTGADGET);
        for (h = NULL; at && n < MAX_CHAIN && !(h = Kaffeine_ShadowNative(ctx, at, KAFFEINE_SHADOW_GADGET));
             n++)
            at = guest_range(ctx, at, G_GD_NEXTGADGET + 4) ? m68k_read32(ctx, at + G_GD_NEXTGADGET) : 0;
        if (!(hb = h ? batch_of_native(ctx, h) : NULL))
            break;
        if (h != hb->first)
            why = "FreeGadgets(): a gadget list that leads into the gadgets of one CreateGadgetA()";
        else if (hb->doomed)
            why = "FreeGadgets(): a gadget list in a cycle";
    }
    /* The class of a listview that stays would write its selection into a
       string gadget that goes, and a window would keep gadgets that go. */
    for (hb = ctx->gadgets; hb && !why; hb = hb->next)
        if (!hb->doomed && hb->showsel && hb->showsel->doomed)
            why = "FreeGadgets(): the GTLV_ShowSelected string gadget of a listview that stays";
        else if (hb->doomed && hb->window)
            why = "FreeGadgets(): gadgets on a window, before RemoveGList() or CloseWindow()";
    /* A context that stays forgets a gadget that goes, which GadTools
       would read again at the next tick or mouse move; without the memory
       for that, GadTools keeps it, and nothing goes. */
    for (hb = ctx->gadgets; hb && !why; hb = hb->next)
        if (hb->kind == KIND_CONTEXT && !hb->doomed && hb->held &&
            (kb = batch_of_native(ctx, hb->held)) && kb->doomed && !forget_held(hb))
            why = "FreeGadgets(): a gadget that GadTools keeps active, without memory to make it "
                  "forget it";
    if (why)
    {
        for (hb = ctx->gadgets; hb; hb = hb->next)
            hb->doomed = FALSE;
        return refuse(ctx, why);
    }
    /* Gadgets of the program that stay and lead into gadgets that go would
       lead into freed memory: those links go, native and 68k, where the
       program should have cut them. */
    for (hb = ctx->gadgets; hb; hb = hb->next)
        for (n = 0; !hb->doomed && n < hb->mirror.count; n++)
            if (hb->mirror.nodes[n].kind == NODE_GADGET)
            {
                s = hb->mirror.nodes[n].native;
                node = hb->mirror.nodes[n].guest;
                if ((kb = batch_of_native(ctx, s->NextGadget)) && kb->doomed)
                    s->NextGadget = NULL;
                if ((kb = batch_of(ctx, m68k_read32(ctx, node + G_GD_NEXTGADGET), &h)) && kb->doomed)
                    m68k_write32(ctx, node + G_GD_NEXTGADGET, 0);
            }
    /* each batch by itself, as the native links between them need not be
       the program's */
    for (hb = ctx->gadgets; hb; hb = hb->next)
        if (hb->doomed)
        {
            hb->last->NextGadget = NULL;
            FreeGadgets(hb->first);
        }
    for (hb = ctx->gadgets; hb; hb = next)
    {
        next = hb->next;
        if (hb->doomed)
            batch_free(ctx, hb);
    }
    return 0;
}

/* gadtools -42: GT_SetGadgetAttrsA(A0=gad, A1=win, A2=req, A3=tagList). The
   attributes that the autodoc gives for the kind of the gadget go to native
   GadTools in one call, as a class collapses some of them before its
   superclass sees them; then the mirrors of the batch follow. GadTools
   draws the change when it gets the window of the gadget: the native one
   goes when the batch is on that window, which also laid a palette out;
   on no window, or with another one, the gadget changes without drawing.
   req, which the autodoc wants NULL, is not passed. */
IPTR Kaffeine_GT_SetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), win = THUNK_A(1), list = THUNK_A(3), item, data, n = 0;
    ULONG table, shown;
    struct KaffeineGadgetKept *stage = NULL, *temp = NULL, *k;
    struct KaffeineEntries *lists = NULL, *e, *next;
    struct KaffeineTagWalk walk;
    struct KaffeineGadgetBatch *b;
    struct Gadget *g = NULL;
    struct Window *w = NULL;
    struct TagItem *tags;
    const char *why = NULL;
    BOOL has_shown = FALSE, has_pen = FALSE, has_entries = FALSE;
    UBYTE offset, color, pen = 0;
    IPTR value = 0, held = 0;
    int step, at, res = 1;

    if (!guest)
        return 0;   /* as GadTools */
    if (!(b = batch_of(ctx, guest, &g)))
        return refuse(ctx, "GT_SetGadgetAttrsA(): not a gadget of CreateContext() or CreateGadgetA()");
    if (win && !(w = Kaffeine_ShadowNative(ctx, win, KAFFEINE_SHADOW_WINDOW)))
        return refuse(ctx, "GT_SetGadgetAttrsA(): not a window of the program");
    if (!list)
        return 0;
    if (!(tags = AllocVec((MAX_TAGS + 1) * sizeof(*tags), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    table = b->table;
    offset = b->offset;
    color = b->color;
    shown = b->shown;
    /* every attribute converted, and checked, before native GadTools gets
       any of them */
    Kaffeine_TagWalkInit(&walk, list);
    while (res > 0 && (step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        if ((at = setter(b, g, m68k_read32(ctx, item))) < 0)
            continue;
        if (n == MAX_TAGS)
        {
            step = KAFFEINE_TAG_BAD;
            break;
        }
        data = m68k_read32(ctx, item + 4);
        if ((res = set_tag(ctx, b, g, at, data, &stage, &temp, &lists, &value, &why)) <= 0)
            break;
        if (res == 2)
        {
            res = 1;
            continue;
        }
        if (setters[at].conv == S_TABLE)
            table = data;
        else if (setters[at].conv == S_OFFSET)
            offset = (UBYTE)data;
        else if (setters[at].tag == GTPA_Color)
        {
            pen = (UBYTE)data;
            has_pen = TRUE;
        }
        else if (setters[at].tag == GTTX_Text || setters[at].tag == GTNM_Number)
        {
            shown = data;
            has_shown = TRUE;
        }
        else if (setters[at].conv == S_ENTRIES)
        {
            held = value;
            has_entries = TRUE;
        }
        tags[n].ti_Tag = setters[at].tag;
        tags[n++].ti_Data = value;
    }
    if (res > 0 && step == KAFFEINE_TAG_BAD)
    {
        why = "GT_SetGadgetAttrsA(): a tag list outside guest memory, without an end or with more "
              "attributes than a program means";
        res = 0;
    }
    /* The palette class looks for the pen of GTPA_Color in as many colours
       of its table as it has, with a WORD index, and reads the table as
       palette_room() says, with a window also as it draws the old choice
       again; GT_GetGadgetAttrsA() reads the pen of the choice that it
       keeps. */
    if (res > 0 && b->kind == PALETTE_KIND)
    {
        if (table && (b->colors > MAX_COLORS || !guest_range(ctx, table, palette_room(b->colors, offset))))
            res = 0;
        else if (has_pen)
            color = palette_index(ctx, table, b->colors, offset, pen);
        if (res == 0)
            why = "GT_SetGadgetAttrsA(): a colour table with less room in guest memory than the "
                  "palette class reads, or for more colours than GadTools takes";
    }
    if (res > 0 && n)
    {
        tags[n].ti_Tag = TAG_DONE;
        GT_SetGadgetAttrsA(g, b->window == w ? w : NULL, NULL, tags);
        b->table = table;
        b->offset = offset;
        b->color = color;
        if (has_shown)
            b->shown = shown;
        /* the copies that the classes keep now */
        while ((k = stage))
        {
            stage = k->next;
            k->next = b->kept;
            b->kept = k;
        }
        /* The listview holds the List of the last GTLV_Labels: the copy
           that it held before, and those of any earlier GTLV_Labels of the
           call, go. The class drew within the call, if at all; no input
           comes in between. */
        if (has_entries)
        {
            entries_free(b->entries);
            b->entries = NULL;
            for (e = lists; e; e = next)
            {
                next = e->next;
                if (held == (IPTR)&e->list)
                {
                    e->next = NULL;
                    b->entries = e;
                }
                else
                    FreeVec(e);
            }
            lists = NULL;
        }
    }
    FreeVec(tags);
    kept_free(&stage);
    kept_free(&temp);
    entries_free(lists);
    if (res < 0)
        bug("[kaffeine] GT_SetGadgetAttrsA(): no memory for the attributes\n");
    if (res == 0)
        return refuse(ctx, why);
    settle(ctx, b);
    if (b->showsel)
        settle(ctx, b->showsel);
    return 0;
}

/* gadtools -174: GT_GetGadgetAttrsA(A0=gad, A1=win, A2=req, A3=taglist) -> D0:
   the attributes filled in. GadTools reads neither win nor req. */
IPTR Kaffeine_GT_GetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), list = THUNK_A(3), item, storage, n = 0, i, count = 0;
    struct { ULONG storage; int getter; } *wanted;
    struct KaffeineTagWalk walk;
    struct KaffeineGadgetBatch *b;
    struct Gadget *g = NULL;
    int step, at;

    if (!guest || !list)
        return 0;   /* as GadTools */
    /* native GadTools would take anything else for a BOOPSI object */
    if (!(b = batch_of(ctx, guest, &g)))
        return refuse(ctx, "GT_GetGadgetAttrsA(): not a gadget of CreateContext() or CreateGadgetA()");
    if (!(wanted = AllocVec(MAX_TAGS * sizeof(*wanted), MEMF_ANY)))
        return 0;
    /* The attributes and their storage, all checked before anything is
       written: what the writes or the refresh of the mirrors change in
       guest memory afterwards, the list too, changes nothing here. */
    Kaffeine_TagWalkInit(&walk, list);
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
        if ((at = getter(b, g, m68k_read32(ctx, item))) >= 0)
        {
            if (n == MAX_TAGS)
            {
                step = KAFFEINE_TAG_BAD;
                break;
            }
            if (!guest_range(ctx, storage = m68k_read32(ctx, item + 4), 4))
            {
                FreeVec(wanted);
                return refuse(ctx, "GT_GetGadgetAttrsA(): storage outside guest memory");
            }
            wanted[n].storage = storage;
            wanted[n++].getter = at;
        }
    if (step == KAFFEINE_TAG_BAD)
    {
        FreeVec(wanted);
        return refuse(ctx, "GT_GetGadgetAttrsA(): a tag list outside guest memory, without an end "
                           "or with more attributes than a program means");
    }
    settle(ctx, b);
    for (i = 0; i < n; i++)
        if (get(ctx, b, g, wanted[i].getter, wanted[i].storage))
            count++;
    FreeVec(wanted);
    return count;
}

/* intuition -642: DisposeObject(A0=object). GadTools frees its gadgets with
   FreeGadgets(), and the bridge its batches with them. Other objects as
   the generated thunk passes them. */
IPTR Kaffeine_DisposeObject(struct KaffeineContext *ctx, void *cpu)
{
    if (Kaffeine_ShadowNative(ctx, THUNK_A(0), KAFFEINE_SHADOW_GADGET))
        return refuse(ctx, "DisposeObject(): a gadget of GadTools, which FreeGadgets() frees");
    DisposeObject(m68k_to_host_or_shadow(ctx, THUNK_A(0)));
    return 0;
}

/* A 68k gadget of GadTools, which the calls below would reach as the native
   one through the shadow table */
static BOOL gadtools_gadget(struct KaffeineContext *ctx, ULONG guest)
{
    return Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_GADGET) != NULL;
}

/* intuition -492: SetEditHook(A0=hook) -> D0 stops the program with rc 20:
   the hook would become the edit hook of every string gadget in the
   system, which the input handler would call as native code, and the old
   one would reach the program as a native pointer. */
IPTR Kaffeine_SetEditHook(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] SetEditHook(%08lx)\n", (unsigned long)THUNK_A(0));
    return refuse(ctx, "SetEditHook(): a global edit hook of 68k code");
}

/* intuition -462: ActivateGadget(A0=gadget, A1=window, A2=requester) -> D0:
   the BOOL of Intuition, for a GadTools gadget of the program on the window
   that its batch is on, which then stays active until input ends it or
   RemoveGList() or CloseWindow() takes it off, before FreeGadgets() can
   free it, and for the copy of a gadget that the program built itself on
   that window, whose 68k gadget then follows; Intuition activates string
   and custom gadgets, and gives TRUE for a Boolean one while the window is
   active and no gadget is in use, without activating it, as AmigaOS 3.2.3.
   AROS activates such a gadget on the window's list or not: any other
   gadget, of a requester or not on that window, gives FALSE without
   Intuition. */
IPTR Kaffeine_ActivateGadget(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g = NULL;
    struct Window *w;
    BOOL active;

    if (THUNK_A(2) || !(w = Kaffeine_ShadowNative(ctx, THUNK_A(1), KAFFEINE_SHADOW_WINDOW)))
        return FALSE;
    if (!(b = batch_of(ctx, THUNK_A(0), &g)))
    {
        if (!(g = Kaffeine_ClassicNative(ctx, w, THUNK_A(0))))
            return FALSE;
        Kaffeine_ClassicRefreshing(ctx, w, g, 1);
        active = ActivateGadget(g, w, NULL);
        Kaffeine_ClassicSync(ctx, w, g);
        return active ? TRUE : FALSE;
    }
    if (b->window != w)
        return FALSE;
    active = ActivateGadget(g, w, NULL);
    settle(ctx, b);
    return active ? TRUE : FALSE;
}

/* intuition -648: SetAttrsA(A0=object, A1=tagList). The attributes of a
   GadTools gadget go through GT_SetGadgetAttrsA(): this path would pass
   68k tags to its native class unconverted. Other objects as the generated
   thunk passes them. */
IPTR Kaffeine_SetAttrsA(struct KaffeineContext *ctx, void *cpu)
{
    struct TagItem *tags;

    if (gadtools_gadget(ctx, THUNK_A(0)))
        return refuse(ctx, "SetAttrsA(): a gadget of GadTools, whose attributes "
                           "GT_SetGadgetAttrsA() sets");
    tags = m68k_to_native_taglist(ctx, THUNK_A(1));
    SetAttrsA(m68k_to_host_or_shadow(ctx, THUNK_A(0)), tags);
    if (tags)
        FreeVec(tags);
    return 0;
}

/* intuition -654: GetAttr(D0=attrID, A0=object, A1=storagePtr) -> D0. The
   native class would write a native IPTR into the 68k LONG; the attributes
   of a GadTools gadget come from GT_GetGadgetAttrsA(). Other objects as
   the generated thunk passes them. */
IPTR Kaffeine_GetAttr(struct KaffeineContext *ctx, void *cpu)
{
    if (gadtools_gadget(ctx, THUNK_A(0)))
        return refuse(ctx, "GetAttr(): a gadget of GadTools, whose attributes "
                           "GT_GetGadgetAttrsA() gives");
    return (IPTR)GetAttr(THUNK_D(0), m68k_to_host_or_shadow(ctx, THUNK_A(0)),
                         m68k_to_host_or_shadow(ctx, THUNK_A(1)));
}

/* intuition -660: SetGadgetAttrsA(A0=gadget, A1=window, A2=requester,
   A3=tagList) -> D0, as SetAttrsA() */
IPTR Kaffeine_SetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu)
{
    struct TagItem *tags;
    IPTR ret;

    if (gadtools_gadget(ctx, THUNK_A(0)))
        return refuse(ctx, "SetGadgetAttrsA(): a gadget of GadTools, whose attributes "
                           "GT_SetGadgetAttrsA() sets");
    tags = m68k_to_native_taglist(ctx, THUNK_A(3));
    ret = (IPTR)SetGadgetAttrsA((struct Gadget *)m68k_to_host_or_shadow(ctx, THUNK_A(0)),
                                (struct Window *)m68k_to_host_or_shadow(ctx, THUNK_A(1)),
                                (struct Requester *)m68k_to_host_or_shadow(ctx, THUNK_A(2)), tags);
    if (tags)
        FreeVec(tags);
    return ret;
}

/* intuition -810: DoGadgetMethodA(A0=gad, A1=win, A2=req, A3=msg) -> D0. A
   method of 68k code would reach the native class of a GadTools gadget
   unconverted. Other gadgets as the generated thunk passes them. */
IPTR Kaffeine_DoGadgetMethodA(struct KaffeineContext *ctx, void *cpu)
{
    if (gadtools_gadget(ctx, THUNK_A(0)))
        return refuse(ctx, "DoGadgetMethodA(): a gadget of GadTools");
    return (IPTR)DoGadgetMethodA((struct Gadget *)m68k_to_host_or_shadow(ctx, THUNK_A(0)),
                                 (struct Window *)m68k_to_host_or_shadow(ctx, THUNK_A(1)),
                                 (struct Requester *)m68k_to_host_or_shadow(ctx, THUNK_A(2)),
                                 (Msg)m68k_to_host_or_shadow(ctx, THUNK_A(3)));
}

/* intuition -186: OnGadget(A0=gadget, A1=window, A2=requester) and -174
   OffGadget(...). Without a window AROS leaves a gadget alone. With one, a
   GadTools gadget changes as AROS changes it, through GA_Disabled for a
   BOOPSI gadget and its GFLG_DISABLED for another, which GT_SetGadgetAttrsA()
   does with GA_Disabled, and it is drawn when its batch is on that window.
   Then its 68k mirrors follow. A gadget that the program built itself
   changes through kaffeine_classic.c, its copy on a window and its own
   Flags; a native object behind a shadow as the generated thunks pass
   it. */
static IPTR enable(struct KaffeineContext *ctx, void *cpu, BOOL off)
{
    struct TagItem tags[] = { { GA_Disabled, off }, { TAG_DONE, 0 } };
    struct Gadget *g = NULL;
    struct KaffeineGadgetBatch *b = batch_of(ctx, THUNK_A(0), &g);
    void *object = m68k_to_host_or_shadow(ctx, THUNK_A(0));
    struct Window *w;

    /* a native object behind a shadow, such as a gadget object, as the
       shadows of the window and the requester name them */
    if (!b && !Kaffeine_ClassicOn(ctx, THUNK_A(0)) && object && object != m68k_to_host(ctx, THUNK_A(0)))
    {
        if (off)
            OffGadget(object, (struct Window *)m68k_to_host_or_shadow(ctx, THUNK_A(1)),
                      (struct Requester *)m68k_to_host_or_shadow(ctx, THUNK_A(2)));
        else
            OnGadget(object, (struct Window *)m68k_to_host_or_shadow(ctx, THUNK_A(1)),
                     (struct Requester *)m68k_to_host_or_shadow(ctx, THUNK_A(2)));
        return 0;
    }
    w = THUNK_A(1) ? Kaffeine_ShadowNative(ctx, THUNK_A(1), KAFFEINE_SHADOW_WINDOW) : NULL;
    if (THUNK_A(1) && !w)
        return refuse(ctx, off ? "OffGadget(): not a window of the program"
                               : "OnGadget(): not a window of the program");
    if (!b)
    {
        if (!Kaffeine_ClassicEnable(ctx, THUNK_A(0), w, off))
            bug("[kaffeine] %s(%08lx): not a gadget of the program\n", off ? "OffGadget" : "OnGadget",
                (unsigned long)THUNK_A(0));
        return 0;
    }
    if (!w)
        return 0;
    GT_SetGadgetAttrsA(g, b->window == w ? w : NULL, NULL, tags);
    settle(ctx, b);
    return 0;
}

IPTR Kaffeine_OnGadget(struct KaffeineContext *ctx, void *cpu)
{
    return enable(ctx, cpu, FALSE);
}

IPTR Kaffeine_OffGadget(struct KaffeineContext *ctx, void *cpu)
{
    return enable(ctx, cpu, TRUE);
}

/* NewModifyProp() and ModifyProp(): a GadTools gadget natively, as the
   generated thunks passed it, which AROS changes only while its class
   poses as a proportional gadget of Intuition, as it does for the input
   handler; the program's own proportional gadget through
   kaffeine_classic.c. A window of the program, and no requester, as the
   bridge puts no gadget of the program on one, so that no 68k Window or
   Requester reaches Intuition. */
static IPTR modify_prop(struct KaffeineContext *ctx, void *cpu, WORD num, BOOL all)
{
    UWORD values[5] = { (UWORD)THUNK_D(0), (UWORD)THUNK_D(1), (UWORD)THUNK_D(2),
                        (UWORD)THUNK_D(3), (UWORD)THUNK_D(4) };
    struct Window *w;
    struct Gadget *g = NULL;

    /* without a window AROS's Intuition changes nothing, where AmigaOS
       3.2.3 hangs */
    if (!THUNK_A(1))
        return 0;
    if (!(w = Kaffeine_ShadowNative(ctx, THUNK_A(1), KAFFEINE_SHADOW_WINDOW)))
        return refuse(ctx, all ? "ModifyProp(): not a window of the program"
                               : "NewModifyProp(): not a window of the program");
    if (batch_of(ctx, THUNK_A(0), &g))
    {
        if (all)
            ModifyProp(g, w, NULL, values[0], values[1], values[2], values[3], values[4]);
        else
            NewModifyProp(g, w, NULL, values[0], values[1], values[2], values[3], values[4], num);
        return 0;
    }
    if (!Kaffeine_ClassicModifyProp(ctx, THUNK_A(0), w, values, num, all))
        bug("[kaffeine] %s(%08lx): not a proportional gadget of the program\n",
            all ? "ModifyProp" : "NewModifyProp", (unsigned long)THUNK_A(0));
    return 0;
}

/* intuition -156: ModifyProp(A0=gadget, A1=window, A2=requester, D0=flags,
   D1=horizPot, D2=vertPot, D3=horizBody, D4=vertBody) */
IPTR Kaffeine_ModifyProp(struct KaffeineContext *ctx, void *cpu)
{
    return modify_prop(ctx, cpu, -1, TRUE);
}

/* intuition -468: NewModifyProp(A0=gadget, A1=window, A2=requester,
   D0=flags, D1=horizPot, D2=vertPot, D3=horizBody, D4=vertBody, D5=numGad) */
IPTR Kaffeine_NewModifyProp(struct KaffeineContext *ctx, void *cpu)
{
    return modify_prop(ctx, cpu, (WORD)THUNK_D(5), FALSE);
}

/* ── Gadget lists on windows ─────────────────────────────────────── */

static void unmark(struct KaffeineContext *ctx)
{
    struct KaffeineGadgetBatch *b;

    for (b = ctx->gadgets; b; b = b->next)
        b->marked = FALSE;
}

/* The gadgets of a list that a window is to take, from the 68k gadget at
   guest: whole batches, from a context or the first gadget of a batch,
   num gadgets or, for a negative num, as far as the list goes. Within a
   batch the 68k links of the program must be the native ones; from the
   last gadget of one on, the list goes where the program's 68k list goes,
   as AmigaOS 3.2.3 takes it: to its end where the program cut it, to the
   gadget that followed on a window, where RemoveGList() leaves it, or to
   another batch that the program linked on; the native links follow, as a
   window takes the native list to its end. Each batch of the run is
   marked. The native first gadget and the count, or NULL with nothing
   marked and the program stopped. */
static struct Gadget *run_mark(struct KaffeineContext *ctx, ULONG guest, LONG num, ULONG *count,
                               const char *call)
{
    struct Gadget *g = NULL, *head, *next = NULL;
    struct KaffeineGadgetBatch *b = batch_of(ctx, guest, &g), *in = NULL, *nb;
    ULONG n = 0, at = guest;
    const char *why = NULL;

    if (!b || g != b->first)
        why = "a gadget list that starts neither at a context nor at the first gadget of a "
              "CreateGadgetA()";
    for (head = g; !why && g; g = next)
    {
        if (n == MAX_WINDOW_GADGETS)
            why = "a gadget list longer than a window takes, or in a cycle";
        /* within a batch the 68k list goes along with the native one */
        else if (at != Kaffeine_ShadowGuest(ctx, g, KAFFEINE_SHADOW_GADGET))
            why = "a 68k gadget list that the program linked otherwise than GadTools did";
        else if (!in)
        {
            in = batch_of_native(ctx, g);
            if (!in || g != in->first)
                why = "a gadget list with gadgets of one CreateGadgetA() apart";
            else if (in->window)
                why = "a gadget that is on a window already";
            else if (in->marked)
                why = "a gadget list in a cycle";
            else
                in->marked = TRUE;
        }
        else if (batch_of_native(ctx, g) != in)
            why = "a gadget list with gadgets of one CreateGadgetA() apart";
        if (why)
            break;
        n++;
        at = m68k_read32(ctx, at + G_GD_NEXTGADGET);
        next = g->NextGadget;
        if (g == in->last)
        {
            in = NULL;
            if (num >= 0 && n >= (ULONG)num)
                break;
            next = at ? Kaffeine_ShadowNative(ctx, at, KAFFEINE_SHADOW_GADGET) : NULL;
            if (at && !next)
                why = "a 68k gadget list that goes on with gadgets that GadTools did not make";
            /* the native link follows to a batch that the window can take,
               and the checks above stop at any other */
            else if (!next || ((nb = batch_of_native(ctx, next)) && next == nb->first &&
                               !nb->window && !nb->marked))
                g->NextGadget = next;
        }
        else if (num >= 0 && n >= (ULONG)num)
            why = "a count of gadgets that ends inside the gadgets of one CreateGadgetA()";
    }
    /* The native list ends inside the gadgets of a call only where GadTools
       cut it. */
    if (!why && in)
        why = "a gadget list that ends inside the gadgets of one CreateGadgetA()";
    if (why)
    {
        unmark(ctx);
        bug("[kaffeine] %s: %s\n", call, why);
        refuse(ctx, "a gadget list that a window cannot take");
        return NULL;
    }
    *count = n;
    return head;
}

/* The class of a listview draws the string gadget of its GTLV_ShowSelected
   into the listview's window: they stay on one window, the marked batches
   with w, NULL for a window about to open. */
static const char *showsel_apart(struct KaffeineContext *ctx, struct Window *w, BOOL taking)
{
    struct KaffeineGadgetBatch *b;

    for (b = ctx->gadgets; b; b = b->next)
        if (b->showsel)
        {
            if (taking && b->marked && !b->showsel->marked && (!w || b->showsel->window != w))
                return "a listview without the string gadget of its GTLV_ShowSelected on the window";
            if (!b->marked && b->showsel->marked && b->window && (!taking || b->window != w))
                return "the GTLV_ShowSelected string gadget of a listview on a window";
        }
    return NULL;
}

/* paletteclass.c lays a palette out when it goes on a window, and divides
   by what it works out there and on a click: the colours need at least one
   column and one row of a pixel each in the box within its borders, less
   the indicator. The arithmetic of its GM_LAYOUT, in its types. */
#define PALETTE_BORDERS 8
static BOOL palette_fits(struct KaffeineGadgetBatch *b)
{
    struct Gadget *g = b->last;
    WORD width = (WORD)(g->Width - PALETTE_BORDERS), height = (WORD)(g->Height - PALETTE_BORDERS);
    UWORD colors = (UWORD)b->colors, cols = 1, cols_p2, rows;
    UBYTE ncols, nrows;

    /* an odd depth, which the class shifts by */
    if (b->colors > MAX_COLORS)
        return FALSE;
    if (b->indh)
        height = (WORD)(height - (b->indh + 4));
    else if (b->indw)
        width = (WORD)(width - (b->indw + 4));
    if (!height)
        return FALSE;
    cols_p2 = (UWORD)(colors * width / height);
    while (cols * cols <= cols_p2)
        cols++;
    if (cols * cols > cols_p2)
        cols--;
    if (!cols)
        return FALSE;
    rows = (UWORD)((colors + cols - 1) / cols);
    ncols = (UBYTE)cols;
    nrows = (UBYTE)rows;
    return ncols && nrows && (UWORD)(width / ncols) && (UWORD)(height / nrows);
}

/* The marked batches as a window would take them: a listview with the
   string of its GTLV_ShowSelected, and palettes that can be laid out */
static const char *unfit(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineGadgetBatch *b;
    const char *why = showsel_apart(ctx, w, TRUE);

    for (b = ctx->gadgets; b && !why; b = b->next)
        if (b->marked && b->kind == PALETTE_KIND && !palette_fits(b))
            why = "a palette too small for its colours, whose layout would divide by zero";
    return why;
}

/* Where Intuition puts a run at position on the list of w, as addglist.c
   walks it: after the gadget there, or at the front. It must not fall
   between the gadgets of one batch on the window, whose links and roles,
   such as the prop before its listview, the bridge keeps. */
static const char *splits(struct KaffeineContext *ctx, struct Window *w, UWORD position)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *pred = NULL, *next = w->FirstGadget;

    while (position && next)
    {
        position--;
        pred = next;
        next = next->NextGadget;
    }
    if (pred && (b = batch_of_native(ctx, pred)) && b->window == w && pred != b->last)
        return "a position between the gadgets of one CreateGadgetA() on the window";
    return NULL;
}

/* The 68k links of the gadgets of the bridge on w and of the marked ones,
   as Intuition left the native links: AddGList() and RemoveGList() also
   change the NextGadget of the gadget before the run, which may be one of
   the program's on the window. */
static void relink(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineGadgetBatch *b;
    ULONG i;

    for (b = ctx->gadgets; b; b = b->next)
        if (b->window == w || b->marked)
            for (i = 0; i < b->mirror.count; i++)
                if (b->mirror.nodes[i].kind == NODE_GADGET)
                    link(ctx, b->mirror.nodes[i].native);
    Kaffeine_ClassicRelink(ctx, w);
}

/* The marked batches are on w: their mirrors follow what Intuition's
   layout changed, and the 68k links what it spliced. */
static void attach_marked(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineGadgetBatch *b;

    relink(ctx, w);
    for (b = ctx->gadgets; b; b = b->next)
        if (b->marked)
        {
            b->marked = FALSE;
            b->window = w;
            settle(ctx, b);
        }
}

/* The marked batches, count gadgets from head, come off w. The 68k
   NextGadget of the last one stays on the gadget that followed it, as
   AmigaOS 3.2.3 leaves it, where the NDK says that V36 cuts it; the
   native one goes, so that no native link leads from gadgets off a window
   into its list, whose gadgets may go first. FreeGadgets() and AddGList()
   follow the 68k one. As the window closes, every 68k link stays as it
   was, also of the program's gadget before the run, as CloseWindow()
   leaves the program's gadgets on AmigaOS 3.2.3. The position that
   RemoveGList() gives, (UWORD)~0 when not found, and then they are on no
   window either. */
static UWORD detach_marked(struct KaffeineContext *ctx, struct Window *w, struct Gadget *head,
                           ULONG count, BOOL closing)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *last = head;
    ULONG n, after, guest;
    UWORD pos;

    for (n = 1; n < count; n++)
        last = last->NextGadget;
    pos = RemoveGList(w, head, (WORD)count);
    if (pos == (UWORD)~0)
        bug("[kaffeine] RemoveGList(): the gadgets were not on the window\n");
    else
    {
        after = guest_gadget(ctx, last->NextGadget);
        last->NextGadget = NULL;
        if (!closing)
        {
            relink(ctx, w);
            if ((guest = Kaffeine_ShadowGuest(ctx, last, KAFFEINE_SHADOW_GADGET)))
                m68k_write32(ctx, guest + G_GD_NEXTGADGET, after);
        }
    }
    for (b = ctx->gadgets; b; b = b->next)
        if (b->marked)
        {
            b->marked = FALSE;
            b->window = NULL;
            settle(ctx, b);
        }
    return pos;
}

/* The gadgets of the bridge on w from g, num of them on the window's list
   or, for a negative num, the rest of it, whole batches: NULL with each
   batch marked and the count, or the reason with nothing marked. */
static const char *run_on(struct KaffeineContext *ctx, struct Window *w, struct Gadget *g, LONG num,
                          ULONG *count)
{
    struct KaffeineGadgetBatch *in = NULL;
    ULONG n = 0;

    for (; g && (num < 0 || n < (ULONG)num) && n < MAX_WINDOW_GADGETS; g = g->NextGadget)
    {
        if (!in)
        {
            in = batch_of_native(ctx, g);
            if (!in || in->window != w || g != in->first)
            {
                unmark(ctx);
                return "gadgets of the window that the bridge did not put there";
            }
            in->marked = TRUE;
        }
        else if (batch_of_native(ctx, g) != in)
        {
            unmark(ctx);
            return "gadgets of the window that the bridge did not put there";
        }
        n++;
        if (g == in->last)
            in = NULL;
    }
    if (in)
    {
        unmark(ctx);
        return "a count of gadgets that ends inside the gadgets of one CreateGadgetA()";
    }
    *count = n;
    return NULL;
}

/* Every gadget of the bridge on w comes off it, run by run, as the window
   closes or the program ends. */
static void window_closing(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g, *head;
    ULONG count;

    for (;;)
    {
        for (head = w->FirstGadget; head; head = head->NextGadget)
            if ((b = batch_of_native(ctx, head)) && b->window == w)
                break;
        if (!head)
            break;
        for (count = 0, g = head; g && (b = batch_of_native(ctx, g)) && b->window == w;
             g = g->NextGadget)
        {
            b->marked = TRUE;
            count++;
        }
        if (detach_marked(ctx, w, head, count, TRUE) == (UWORD)~0)
            break;
    }
    /* none is left on w, also where the bridge lost track */
    for (b = ctx->gadgets; b; b = b->next)
        if (b->window == w)
        {
            bug("[kaffeine] GadTools gadgets that were not on the list of their window\n");
            b->window = NULL;
        }
    Kaffeine_ClassicClosing(ctx, w);
}

/* For OpenWindow() and OpenWindowTagList(): the native head of the list
   of the window about to open, checked as a whole as AddGList() with -1
   takes it */
static struct Gadget *window_prepare(struct KaffeineContext *ctx, ULONG guest, const char *call)
{
    struct Gadget *head;
    const char *why;
    ULONG count;

    if (!gadtools_gadget(ctx, guest))
        return Kaffeine_ClassicPrepare(ctx, guest, call);
    if (!(head = run_mark(ctx, guest, -1, &count, call)))
        return NULL;
    why = unfit(ctx, NULL);
    unmark(ctx);
    if (why)
    {
        refuse(ctx, why);
        return NULL;
    }
    return head;
}

/* Intuition put the list at the end of the window's list */
static void window_opened(struct KaffeineContext *ctx, struct Window *w, struct Gadget *head)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g;

    Kaffeine_ClassicOpened(ctx, w);
    for (g = head; g && (b = batch_of_native(ctx, g)) && !b->window; g = g->NextGadget)
        b->marked = TRUE;
    attach_marked(ctx, w);
}

/* The window did not open: the copies of the program's own list go */
static void window_dropped(struct KaffeineContext *ctx)
{
    Kaffeine_ClassicDropped(ctx);
}

/* The batches on w and the one of the gadget at address, which the
   program may have taken off w after the message came: GadTools changes
   it as it filters the message all the same */
/* The context with which GadTools filters a message of w: the first one
   on the window's list */
static struct KaffeineGadgetBatch *window_context(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g;

    for (g = w ? w->FirstGadget : NULL; g; g = g->NextGadget)
        if ((b = batch_of_native(ctx, g)) && b->kind == KIND_CONTEXT)
            return b;
    return NULL;
}

/* The gadget of a GADGETDOWN, which GadTools may keep active in that
   context until FreeGadgets() makes it forget it */
static void window_filtering(struct KaffeineContext *ctx, struct IntuiMessage *im)
{
    struct KaffeineGadgetBatch *c;

    if (im->Class == IDCMP_GADGETDOWN && (c = window_context(ctx, im->IDCMPWindow)))
        c->held = (struct Gadget *)im->IAddress;
}

static void window_sync(struct KaffeineContext *ctx, struct Window *w, APTR address)
{
    struct KaffeineGadgetBatch *b, *named = batch_of_native(ctx, (struct Gadget *)address);

    for (b = ctx->gadgets; b; b = b->next)
        if (b->window == w || b == named)
            settle(ctx, b);
    Kaffeine_ClassicSync(ctx, w, address);
}

/* The 68k gadget of a native one that a message names: the mirror of a
   GadTools gadget, or the program's own gadget of a copy */
static ULONG window_gadget(struct KaffeineContext *ctx, const void *native)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, native, KAFFEINE_SHADOW_GADGET);

    return guest ? guest : Kaffeine_ClassicGuest(ctx, native);
}

static const struct KaffeineWindowGadgets window_hooks =
{
    window_prepare, window_opened, window_dropped, window_closing, window_sync, window_filtering,
    window_gadget
};

void Kaffeine_GadgetsSetup(struct KaffeineContext *ctx)
{
    ctx->window_gadgets = &window_hooks;
}

/* AddGList() and AddGadget(): a list of GadTools gadgets of the program on
   a window of the program, whole batches, or of gadgets that it built
   itself through kaffeine_classic.c; neither goes between the gadgets of
   one GadTools call on the window. */
static IPTR add_gadgets(struct KaffeineContext *ctx, ULONG window, ULONG gadget, UWORD position,
                        WORD num, ULONG requester, BOOL one, const char *call)
{
    struct Window *w;
    struct Gadget *head;
    const char *why;
    ULONG count;
    IPTR result;

    if (!gadtools_gadget(ctx, gadget))
    {
        /* Intuition ignores the requester for gadgets without
           GTYP_REQGADGET, and requester gadgets stay off for now. */
        if (!(w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW)))
            return refuse(ctx, "gadgets for what is not a window of the program");
        /* the gadgets of one GadTools call stay together */
        if ((why = splits(ctx, w, position)))
            return refuse(ctx, why);
        result = Kaffeine_ClassicAdd(ctx, w, gadget, position, num, one, call);
        /* the GadTools gadget before the run links to it now */
        if (ctx->running)
            relink(ctx, w);
        return result;
    }
    if (requester)
        return refuse(ctx, "a GadTools gadget for a requester");
    if (!(w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW)))
        return refuse(ctx, "GadTools gadgets for what is not a window of the program");
    if (!(head = run_mark(ctx, gadget, num, &count, call)))
        return ctx->exit_code;
    if ((why = unfit(ctx, w)) || (why = splits(ctx, w, position)))
    {
        unmark(ctx);
        return refuse(ctx, why);
    }
    position = one ? AddGadget(w, head, position) : AddGList(w, head, position, (WORD)count, NULL);
    attach_marked(ctx, w);
    return (IPTR)position;
}

/* intuition -438: AddGList(A0=window, A1=gadget, D0=position, D1=numGad,
   A2=requester) -> D0: the UWORD position where the list went */
IPTR Kaffeine_AddGList(struct KaffeineContext *ctx, void *cpu)
{
    /* Intuition adds nothing for these */
    if (!THUNK_A(0) || !THUNK_A(1) || !(WORD)THUNK_D(1))
        return (IPTR)(UWORD)~0;
    return add_gadgets(ctx, THUNK_A(0), THUNK_A(1), (UWORD)THUNK_D(0), (WORD)THUNK_D(1), THUNK_A(2),
                       FALSE, "AddGList()");
}

/* intuition -42: AddGadget(A0=window, A1=gadget, D0=position) -> D0: the
   UWORD position; a batch of one gadget, such as a context or a button */
IPTR Kaffeine_AddGadget(struct KaffeineContext *ctx, void *cpu)
{
    if (!THUNK_A(0) || !THUNK_A(1))
        return 0;   /* as Intuition */
    return add_gadgets(ctx, THUNK_A(0), THUNK_A(1), (UWORD)THUNK_D(0), 1, 0, TRUE, "AddGadget()");
}

/* RemoveGList() and RemoveGadget(): GadTools gadgets of the program come
   off its window, whole batches, num of them from gadget or, for a
   negative num, the rest of the window's list, which must hold only them. */
static IPTR remove_gadgets(struct KaffeineContext *ctx, ULONG window, ULONG gadget, WORD num,
                           const char *call)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g = NULL;
    struct Window *w;
    const char *why;
    ULONG count;
    IPTR result;

    if (!(b = batch_of(ctx, gadget, &g)))
    {
        if (!Kaffeine_ClassicOn(ctx, gadget))
            return (IPTR)(UWORD)~0;   /* never on a window */
        if (!(w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW)))
            return refuse(ctx, "gadgets off what is not a window of the program");
        result = Kaffeine_ClassicRemove(ctx, w, gadget, num, call);
        if (ctx->running)
            relink(ctx, w);
        return result;
    }
    if (!(w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW)))
        return refuse(ctx, "GadTools gadgets off what is not a window of the program");
    if (b->window != w)
        return (IPTR)(UWORD)~0;   /* as Intuition, which does not find it there */
    if (g != b->first)
        return refuse(ctx, "a gadget that GadTools made after another one in the same call, off "
                           "a window");
    if ((why = run_on(ctx, w, g, num, &count)) || (why = showsel_apart(ctx, w, FALSE)))
    {
        unmark(ctx);
        bug("[kaffeine] %s: %s\n", call, why);
        return refuse(ctx, "GadTools gadgets that cannot come off the window so");
    }
    return (IPTR)detach_marked(ctx, w, g, count, FALSE);
}

/* intuition -444: RemoveGList(A0=window, A1=gadget, D0=numGad) -> D0: the
   UWORD position, (UWORD)~0 when not found */
IPTR Kaffeine_RemoveGList(struct KaffeineContext *ctx, void *cpu)
{
    if (!THUNK_A(0) || !THUNK_A(1) || !(WORD)THUNK_D(0))
        return (IPTR)(UWORD)~0;   /* as Intuition */
    return remove_gadgets(ctx, THUNK_A(0), THUNK_A(1), (WORD)THUNK_D(0), "RemoveGList()");
}

/* intuition -228: RemoveGadget(A0=window, A1=gadget) -> D0, as
   RemoveGList() of one gadget */
IPTR Kaffeine_RemoveGadget(struct KaffeineContext *ctx, void *cpu)
{
    if (!THUNK_A(0) || !THUNK_A(1))
        return (IPTR)(UWORD)~0;
    return remove_gadgets(ctx, THUNK_A(0), THUNK_A(1), 1, "RemoveGadget()");
}

/* RefreshGList() and RefreshGadgets(): native for gadgets of the program
   on that window, GadTools ones or its own, whose mirrors and own gadgets
   then follow what rendering set; what the program changed of the shared
   bits of its own gadgets reaches their copies first. Nothing for other
   gadgets. Requesters hold no gadget of the program. */
static IPTR refresh(struct KaffeineContext *ctx, ULONG gadgets, ULONG window, WORD num)
{
    struct KaffeineGadgetBatch *b;
    struct Gadget *g = NULL;
    struct Window *w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW);

    if (!num || !w)
        return 0;
    if (!(b = batch_of(ctx, gadgets, &g)) || b->window != w)
        g = Kaffeine_ClassicNative(ctx, w, gadgets);
    if (!g)
        return 0;
    Kaffeine_ClassicRefreshing(ctx, w, g, num);
    RefreshGList(g, w, NULL, num);
    /* rendering brings fields such as NumChars of a string up to date */
    window_sync(ctx, w, NULL);
    return 0;
}

/* intuition -432: RefreshGList(A0=gadgets, A1=window, A2=requester,
   D0=numGad) */
IPTR Kaffeine_RefreshGList(struct KaffeineContext *ctx, void *cpu)
{
    return refresh(ctx, THUNK_A(0), THUNK_A(1), (WORD)THUNK_D(0));
}

/* intuition -222: RefreshGadgets(A0=gadgets, A1=window, A2=requester), as
   RefreshGList() with -1 */
IPTR Kaffeine_RefreshGadgets(struct KaffeineContext *ctx, void *cpu)
{
    return refresh(ctx, THUNK_A(0), THUNK_A(1), -1);
}

void Kaffeine_GadgetsCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineGadgetBatch *b;
    ULONG i, left = 0;

    /* Gadgets on windows that the program left open come off them, then
       all of them go, each by itself, wherever the lists were cut. */
    for (b = ctx->gadgets; b; b = b ? b->next : ctx->gadgets)
        if (b->window)
        {
            window_closing(ctx, b->window);
            b = NULL;
        }
    for (b = ctx->gadgets; b; b = b->next)
        for (i = 0; i < b->mirror.count; i++)
            if (b->mirror.nodes[i].kind == NODE_GADGET)
                ((struct Gadget *)b->mirror.nodes[i].native)->NextGadget = NULL;
    for (b = ctx->gadgets; b; b = b->next)
        for (i = 0; i < b->mirror.count; i++)
            if (b->mirror.nodes[i].kind == NODE_GADGET)
            {
                FreeGadgets(b->mirror.nodes[i].native);
                left++;
            }
    if (left)
        bug("[kaffeine] %lu GadTools gadgets that the program left, freed\n", (unsigned long)left);
    while (ctx->gadgets)
        batch_free(ctx, ctx->gadgets);
    Kaffeine_ClassicCleanup(ctx);
}
