/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Gadgets that a 68k program builds itself, without GadTools, on its
    windows: Boolean, string, integer and proportional gadgets. Intuition
    gets a native copy of each gadget, made by kaffeine_copy.c in one block
    with its renders, its label, the StringInfo of a string gadget, whose
    text buffers stay in guest memory, and the PropInfo and the knob Image
    of a proportional gadget, and the program keeps its own Gadget, which
    the bridge reads as the gadget goes on a window and to which it writes
    what Intuition changes.

    A run of the program's gadgets goes on a window whole or stays off
    it: OpenWindow() with FirstGadget, WA_Gadgets, AddGList() and
    AddGadget() take it when all of its gadgets are Boolean, string or
    proportional gadgets of a window, and leave it off, as they left
    every gadget of the program's own before, for a custom gadget, a
    gadget of a requester or a screen, a system gadget, a masked Boolean
    gadget, a string gadget with a keymap of its own or a gadget object,
    and without memory or a free shadow for the copies. A run that goes on
    with GadTools gadgets, holds a gadget twice or one on a window already,
    lies outside guest memory, falls between the gadgets of one GadTools
    call on the window or holds a string or proportional gadget that the
    copy refuses stops the program.

    GFLG_SELECTED and GFLG_DISABLED pass between a copy and the program's
    gadget by bit, each way only what one side changed since both last
    agreed, and so do BufferPos, DispPos and LongInt of a string gadget,
    each as a whole, and the Flags of a PropInfo by bit, AUTOKNOB aside, and
    its pots and bodies as wholes; NumChars, DispCount and UndoPos, which
    Intuition keeps, CWidth to TopBorder of a PropInfo, which it works out,
    where it put the knob, into the program's knob Image, and
    GACT_ACTIVEGADGET only go to the program. They go to the program before
    each message of the window reaches it, after OpenWindow(), AddGList(),
    OnGadget(), OffGadget(), RefreshGList() and ActivateGadget(), and as the
    gadget comes off the window; to the copy before RefreshGList(),
    OnGadget(), OffGadget() and ActivateGadget(), those of a string gadget
    only while Intuition does not edit it, and BufferPos and DispPos within
    its text, those of a proportional gadget only while the button does not
    hold it. NewModifyProp() and ModifyProp() change the copy through
    Intuition, which draws it into the window given, and the PropInfo alone
    of a gadget on no window, and nothing without a window. A program that
    reads the pots of a knob that the user drags without messages sees them
    at the next of these passes. What the program changes in the renders,
    the label, the geometry, the other bits, the rest of a StringInfo or
    AUTOKNOB reaches Intuition as the gadget goes on a window again after
    RemoveGList(), the way the NDK has a program change a gadget.

    The 68k NextGadget of the program's gadgets on a window follows the
    native list as AddGList() and RemoveGList() splice it: the program's
    next gadget there, or 0 before a system gadget and at the end; the
    last gadget that RemoveGList() takes off keeps its link, as AmigaOS
    3.2.3 and AROS leave it, where the NDK says that V36 cuts it.
    IAddress of a message names the program's gadget. A copy that came
    off a window stays while a message of the window names it, so that
    no new copy at its address takes that message over, and at the latest
    until the window closes. CloseWindow() takes the program's gadgets off
    its window first, and the end of the program frees what is left.
*/

#include "kaffeine_intern.h"
#include "kaffeine_classic.h"
#include "kaffeine_copy.h"
#include "kaffeine_rastport.h"

#include <aros/atomic.h>
#include <aros/debug.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k Gadget and StringInfo of the NDK */
enum
{
    G_GD_NEXTGADGET = 0, G_GD_FLAGS = 12, G_GD_ACTIVATION = 14, G_GD_GADGETTYPE = 16,
    G_GD_SPECIALINFO = 34, G_GADGET_SIZE = 44, G_GD_MOREFLAGS = 44, G_EXTGADGET_SIZE = 56,
    G_GMORE_BOOPSIGADGET = 0x400,

    G_SI_BUFFERPOS = 8, G_SI_DISPPOS = 12, G_SI_UNDOPOS = 14, G_SI_NUMCHARS = 16,
    G_SI_DISPCOUNT = 18, G_SI_LONGINT = 28, G_SI_ALTKEYMAP = 32, G_STRINGINFO_SIZE = 36,

    G_GD_GADGETRENDER = 18,
    G_PI_FLAGS = 0, G_PI_POTS = 2, G_PI_CWIDTH = 10, G_PROPINFO_SIZE = 22,
    G_IM_LEFTEDGE = 0
};

/* Longer runs than this are no gadget list a program means. */
#define MAX_RUN 4096
/* AROS counts the text of a string gadget up to this many bytes. */
#define MAX_TEXT 0x7FFE
/* The Flags bits that pass between a copy and the program's gadget */
#define SHARED_FLAGS (GFLG_SELECTED | GFLG_DISABLED)
/* And those of a PropInfo: all but AUTOKNOB, which chooses the render of
   the knob, its Image alone or the program's chain, and so reaches
   Intuition as the gadget goes on a window again, as the renders do;
   else Intuition would draw an Image that it sized for an AUTOKNOB with
   data checked for its own size. */
#define PROP_FLAGS ((UWORD)~AUTOKNOB)

struct KaffeineClassic
{
    struct KaffeineClassic *next;
    ULONG guest;                /* the program's Gadget */
    struct Gadget *native;      /* its copy, in block */
    APTR block;
    struct Window *window;      /* the window that it is on, or came off */
    BOOL on;                    /* on that window's list */
    BOOL marked;                /* in the run of the call */
    BOOL shadowed;              /* the shadow of the copy names the gadget */
    UWORD flags;                /* the shared Flags bits as both sides agreed */
    UWORD activation;           /* GACT_ACTIVEGADGET, likewise */
    ULONG info;                 /* the program's StringInfo of a string gadget, or 0 */
    WORD pos, disp, undo;       /* its BufferPos, DispPos and UndoPos, likewise */
    WORD chars, count;          /* NumChars and DispCount, likewise */
    LONG number;                /* LongInt, likewise */
    ULONG prop;                 /* the program's PropInfo of a proportional gadget, or 0 */
    ULONG knob;                 /* and the Image of its knob, or 0 */
    UWORD pflags;               /* the PropInfo's Flags as both sides agreed */
    UWORD pots[4];              /* HorizPot, VertPot, HorizBody and VertBody, likewise */
    UWORD calc[6];              /* CWidth to TopBorder, which Intuition works out, likewise */
    WORD place[4];              /* the knob's LeftEdge, TopEdge, Width and Height, likewise */
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* Stop the program with return code 20, which the thunk returns to D0. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

static struct KaffeineClassic *of_native(struct KaffeineContext *ctx, const struct Gadget *g)
{
    struct KaffeineClassic *r;

    for (r = ctx->classic; r; r = r->next)
        if (r->native == g)
            return r;
    return NULL;
}

/* The copy of the program's gadget at guest that is on a window */
static struct KaffeineClassic *on_window(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineClassic *r;

    for (r = ctx->classic; r; r = r->next)
        if (r->on && r->guest == guest)
            return r;
    return NULL;
}

ULONG Kaffeine_ClassicGuest(struct KaffeineContext *ctx, const struct Gadget *g)
{
    struct KaffeineClassic *r = g ? of_native(ctx, g) : NULL;

    return r ? r->guest : 0;
}

BOOL Kaffeine_ClassicOn(struct KaffeineContext *ctx, ULONG guest)
{
    return on_window(ctx, guest) != NULL;
}

struct Gadget *Kaffeine_ClassicNative(struct KaffeineContext *ctx, struct Window *w, ULONG guest)
{
    struct KaffeineClassic *r = on_window(ctx, guest);

    return r && r->window == w ? r->native : NULL;
}

/* ── Copies ──────────────────────────────────────────────────────── */

/* The shadow of this copy: the program's gadget may have another copy by
   now, under the same guest address */
static void shadow_drop(struct KaffeineContext *ctx, const struct Gadget *native)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].native_ptr == native &&
            ctx->shadow_map[i].type == KAFFEINE_SHADOW_CLASSIC)
        {
            ctx->shadow_map[i] = ctx->shadow_map[--ctx->num_shadows];
            return;
        }
}

static void record_free(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    struct KaffeineClassic **p;

    for (p = &ctx->classic; *p; p = &(*p)->next)
        if (*p == r)
        {
            *p = r->next;
            break;
        }
    if (r->shadowed)
        shadow_drop(ctx, r->native);
    FreeVec(r->block);
    FreeVec(r);
}

/* The copy came off its window: only messages of the window name it
   still, through its record */
static void retire(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    shadow_drop(ctx, r->native);
    r->shadowed = FALSE;
    r->on = FALSE;
}

/* Why the program's gadget at guest stays off windows for now, or NULL */
static const char *unsupported(struct KaffeineContext *ctx, ULONG guest)
{
    UWORD type = m68k_read16(ctx, guest + G_GD_GADGETTYPE);
    UWORD activation = m68k_read16(ctx, guest + G_GD_ACTIVATION);
    UWORD kind = type & GTYP_GTYPEMASK;
    ULONG info;

    if (kind != GTYP_BOOLGADGET && kind != GTYP_STRGADGET && kind != GTYP_PROPGADGET)
        return "custom gadgets of the program's own";
    /* the input handler tells system gadgets by their subtype alone too */
    if (type & (GTYP_REQGADGET | GTYP_SCRGADGET | GTYP_SYSGADGET | GTYP_SYSTYPEMASK))
        return "gadgets of requesters or screens, and system gadgets, of the program's own";
    /* the bit is GACT_STRINGEXTEND for a string gadget */
    if (kind == GTYP_BOOLGADGET && (activation & GACT_BOOLEXTEND))
        return "masked Boolean gadgets";
    /* a 68k KeyMap, of tables that a native copy would need; the copy
       refuses a StringInfo outside guest memory */
    if (kind == GTYP_STRGADGET && (activation & GACT_ALTKEYMAP) &&
        guest_range(ctx, info = m68k_read32(ctx, guest + G_GD_SPECIALINFO), G_STRINGINFO_SIZE) &&
        m68k_read32(ctx, info + G_SI_ALTKEYMAP))
        return "string gadgets with a keymap of their own";
    if ((m68k_read16(ctx, guest + G_GD_FLAGS) & GFLG_EXTENDED) &&
        (m68k_read32(ctx, guest + G_GD_MOREFLAGS) & G_GMORE_BOOPSIGADGET))
        return "gadget objects of the program's own";
    return NULL;
}

/* A marked copy of the program's gadget at guest, which the caller checked;
   NULL without memory, or with *why for a gadget that the copy refuses */
static struct KaffeineClassic *copy_one(struct KaffeineContext *ctx, ULONG guest, const char **why)
{
    struct KaffeineClassic *r;
    struct KaffeineCopy c;

    memset(&c, 0, sizeof(c));
    Kaffeine_CopyGadget(ctx, &c, guest);
    if ((*why = c.why))
        return NULL;
    if (!(r = AllocVec(sizeof(*r), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    if (!(r->block = AllocVec(c.used, MEMF_ANY | MEMF_CLEAR)))
    {
        FreeVec(r);
        return NULL;
    }
    Kaffeine_CopyFill(&c, r->block);
    r->native = Kaffeine_CopyGadget(ctx, &c, guest);
    if ((*why = c.why) || !r->native || !Kaffeine_ShadowAdd(ctx, guest, r->native,
                                                          KAFFEINE_SHADOW_CLASSIC))
    {
        FreeVec(r->block);
        FreeVec(r);
        return NULL;
    }
    /* Intuition sets GACT_ACTIVEGADGET itself, and the program's word may
       have it from another time */
    r->native->Activation &= ~GACT_ACTIVEGADGET;
    r->guest = guest;
    r->shadowed = TRUE;
    r->marked = TRUE;
    r->flags = r->native->Flags & SHARED_FLAGS;
    /* a string gadget: what its StringInfo holds, both sides agree on */
    if ((r->native->GadgetType & GTYP_GTYPEMASK) == GTYP_STRGADGET)
    {
        struct StringInfo *si = r->native->SpecialInfo;

        r->info = m68k_read32(ctx, guest + G_GD_SPECIALINFO);
        /* Intuition selects a string gadget while it edits it: a copy
           starts without, and the program's gadget loses it at the next
           pass, as AmigaOS 3.2.3 clears it as the gadget comes off */
        r->native->Flags &= ~GFLG_SELECTED;
        r->pos = si->BufferPos;
        r->disp = si->DispPos;
        r->undo = si->UndoPos;
        r->chars = si->NumChars;
        r->count = si->DispCount;
        r->number = si->LongInt;
    }
    /* a proportional gadget: its PropInfo and its knob likewise */
    if ((r->native->GadgetType & GTYP_GTYPEMASK) == GTYP_PROPGADGET)
    {
        struct PropInfo *pi = r->native->SpecialInfo;
        struct Image *im = r->native->GadgetRender;

        r->prop = m68k_read32(ctx, guest + G_GD_SPECIALINFO);
        /* Intuition selects it while the button holds it */
        r->native->Flags &= ~GFLG_SELECTED;
        r->pflags = pi->Flags;
        r->pots[0] = pi->HorizPot;
        r->pots[1] = pi->VertPot;
        r->pots[2] = pi->HorizBody;
        r->pots[3] = pi->VertBody;
        r->calc[0] = pi->CWidth;
        r->calc[1] = pi->CHeight;
        r->calc[2] = pi->HPotRes;
        r->calc[3] = pi->VPotRes;
        r->calc[4] = pi->LeftBorder;
        r->calc[5] = pi->TopBorder;
        if (im)
        {
            r->knob = m68k_read32(ctx, guest + G_GD_GADGETRENDER);
            r->place[0] = im->LeftEdge;
            r->place[1] = im->TopEdge;
            r->place[2] = im->Width;
            r->place[3] = im->Height;
        }
    }
    r->next = ctx->classic;
    ctx->classic = r;
    return r;
}

/* The marked copies of a run that does not go on a window go. */
static void drop_marked(struct KaffeineContext *ctx)
{
    struct KaffeineClassic *r, *next;

    for (r = ctx->classic; r; r = next)
    {
        next = r->next;
        if (r->marked)
            record_free(ctx, r);
    }
}

/* The program's gadgets of a run from guest, num of them or, for a
   negative num, as far as its 68k list goes, copied and marked, their
   copies linked in that order: the native first one and the count. NULL
   with nothing marked for a run that stays off the window, or the program
   stopped. */
static struct Gadget *run_copy(struct KaffeineContext *ctx, ULONG guest, LONG num, ULONG *count,
                               const char *call)
{
    struct KaffeineClassic *r, *prev = NULL, *first = NULL;
    const char *why = NULL, *out = NULL;
    ULONG at, before, n, i;
    UWORD flags;

    /* the whole run first, so that a refusal comes before any copy */
    for (n = 0, at = guest; !why && at && (num < 0 || n < (ULONG)num);
         n++, at = m68k_read32(ctx, at + G_GD_NEXTGADGET))
    {
        if (n == MAX_RUN)
            why = "a gadget list longer than a window takes, or in a cycle";
        else if (!guest_range(ctx, at, G_GADGET_SIZE) ||
                 (((flags = m68k_read16(ctx, at + G_GD_FLAGS)) & GFLG_EXTENDED) &&
                  !guest_range(ctx, at, G_EXTGADGET_SIZE)))
            why = "a gadget outside guest memory";
        else if (Kaffeine_ShadowNative(ctx, at, KAFFEINE_SHADOW_GADGET))
            why = "a gadget list that mixes the program's own gadgets with GadTools ones";
        else if (on_window(ctx, at))
            why = "a gadget that is on a window already";
        else
        {
            /* a gadget twice in the run: its 68k list runs in a cycle */
            for (i = 0, before = guest; i < n && before != at;
                 i++, before = m68k_read32(ctx, before + G_GD_NEXTGADGET))
                ;
            if (i < n)
                why = "a gadget list that holds a gadget twice, or in a cycle";
            else if (!out)
                out = unsupported(ctx, at);
        }
    }
    if (why)
    {
        bug("[kaffeine] %s: %s\n", call, why);
        refuse(ctx, "a gadget list that a window cannot take");
        return NULL;
    }
    if (out)
    {
        bug("[kaffeine] %s: %s stay off windows for now, and the list with them\n", call, out);
        return NULL;
    }
    for (n = 0, at = guest; at && (num < 0 || n < (ULONG)num);
         n++, at = m68k_read32(ctx, at + G_GD_NEXTGADGET))
    {
        if (!(r = copy_one(ctx, at, &why)) && !why)
        {
            drop_marked(ctx);
            bug("[kaffeine] %s: no memory for the copies, the list stays off the window\n", call);
            return NULL;
        }
        if (why)
        {
            drop_marked(ctx);
            bug("[kaffeine] %s: %s\n", call, why);
            refuse(ctx, "a gadget list that a window cannot take");
            return NULL;
        }
        if (prev)
            prev->native->NextGadget = r->native;
        else
            first = r;
        prev = r;
    }
    *count = n;
    return first ? first->native : NULL;
}

/* ── Shared state ────────────────────────────────────────────────── */

/* A position in a text of chars characters */
static WORD within(WORD value, WORD chars)
{
    return value < 0 ? 0 : value > chars ? chars : value;
}

/* What the program changed of a PropInfo since both sides agreed goes to
   the copy: its Flags bit by bit, AUTOKNOB aside, its pots and bodies as
   wholes. Only while the button does not hold the gadget: the input
   handler selects it as the button goes down on it, sets KNOBHIT and
   moves the pots itself from then on, so not between the check and the
   writes either. */
static void prop_in(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    struct PropInfo *pi = r->native->SpecialInfo;
    UWORD *pots[4] = { &pi->HorizPot, &pi->VertPot, &pi->HorizBody, &pi->VertBody };
    UWORD guest = m68k_read16(ctx, r->prop + G_PI_FLAGS), value[4];
    UWORD changed = (guest ^ r->pflags) & PROP_FLAGS;
    ULONG i;

    for (i = 0; i < 4; i++)
        value[i] = m68k_read16(ctx, r->prop + G_PI_POTS + 2 * i);
    Forbid();
    if (!(r->native->Flags & GFLG_SELECTED))
    {
        pi->Flags = (pi->Flags & ~changed) | (guest & changed);
        r->pflags ^= changed;
        for (i = 0; i < 4; i++)
            if (value[i] != r->pots[i])
                *pots[i] = r->pots[i] = value[i];
    }
    Permit();
}

/* What Intuition changed of a PropInfo goes to the program: its Flags bit
   by bit, KNOBHIT among them and AUTOKNOB aside, its pots and bodies, what
   it works out from CWidth to TopBorder, and where the knob is, into the
   program's Image. */
static void prop_out(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    struct PropInfo *pi = r->native->SpecialInfo, now;
    struct Image *im = r->native->GadgetRender, knob = { 0 };
    UWORD pots[4], calc[6], changed;
    WORD place[4];
    ULONG i;

    /* one sample of both, which the input handler moves together and
       changes neither of while it is taken; an update of its that waits
       halfway, as on a lock, can still show in it */
    Forbid();
    now = *pi;
    if (im)
        knob = *im;
    Permit();
    pots[0] = now.HorizPot;
    pots[1] = now.VertPot;
    pots[2] = now.HorizBody;
    pots[3] = now.VertBody;
    calc[0] = now.CWidth;
    calc[1] = now.CHeight;
    calc[2] = now.HPotRes;
    calc[3] = now.VPotRes;
    calc[4] = now.LeftBorder;
    calc[5] = now.TopBorder;
    place[0] = knob.LeftEdge;
    place[1] = knob.TopEdge;
    place[2] = knob.Width;
    place[3] = knob.Height;
    if ((changed = (now.Flags ^ r->pflags) & PROP_FLAGS))
    {
        m68k_write16(ctx, r->prop + G_PI_FLAGS,
                     (m68k_read16(ctx, r->prop + G_PI_FLAGS) & ~changed) | (now.Flags & changed));
        r->pflags ^= changed;
    }
    for (i = 0; i < 4; i++)
        if (pots[i] != r->pots[i])
            m68k_write16(ctx, r->prop + G_PI_POTS + 2 * i, r->pots[i] = pots[i]);
    for (i = 0; i < 6; i++)
        if (calc[i] != r->calc[i])
            m68k_write16(ctx, r->prop + G_PI_CWIDTH + 2 * i, r->calc[i] = calc[i]);
    if (r->knob && im)
        for (i = 0; i < 4; i++)
            if (place[i] != r->place[i])
                m68k_write16(ctx, r->knob + G_IM_LEFTEDGE + 2 * i, (UWORD)(r->place[i] = place[i]));
}

/* What the program changed of the shared bits since both sides agreed
   goes to the copy, and of BufferPos, DispPos and LongInt of a string
   gadget that Intuition does not edit: the input handler reads them
   between its own checks while it does. The positions go within the text
   as it is now, which Intuition counts first at a refresh too, and the
   program gets them so at the next pass the other way. The guard after
   guest memory ends the count. */
static void state_in(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    UWORD guest = m68k_read16(ctx, r->guest + G_GD_FLAGS);
    UWORD changed = (guest ^ r->flags) & SHARED_FLAGS;
    struct StringInfo *si = r->native->SpecialInfo;
    WORD value, chars;
    LONG number;
    UBYTE *end;

    /* bit by bit, as the input handler sets and clears others meanwhile */
    if (changed & guest)
        AROS_ATOMIC_OR(r->native->Flags, changed & guest);
    if (changed & ~guest)
        AROS_ATOMIC_AND(r->native->Flags, ~(changed & ~guest));
    r->flags ^= changed;
    if (r->prop)
        prop_in(ctx, r);
    if (!r->info || (r->native->Flags & GFLG_SELECTED))
        return;
    end = memchr(si->Buffer, 0, MAX_TEXT);
    chars = end ? (WORD)(end - si->Buffer) : MAX_TEXT;
    if ((value = (WORD)m68k_read16(ctx, r->info + G_SI_BUFFERPOS)) != r->pos)
        si->BufferPos = within(r->pos = value, chars);
    if ((value = (WORD)m68k_read16(ctx, r->info + G_SI_DISPPOS)) != r->disp)
        si->DispPos = within(r->disp = value, chars);
    if ((number = (LONG)m68k_read32(ctx, r->info + G_SI_LONGINT)) != r->number)
        si->LongInt = r->number = number;
}

/* What Intuition changed of them goes to the program's gadget, and of
   NumChars, DispCount and UndoPos, which it keeps. */
static void state_out(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    /* one sample of each, which the input handler may change meanwhile */
    UWORD native = r->native->Flags, changed = (native ^ r->flags) & SHARED_FLAGS;
    UWORD active = r->native->Activation & GACT_ACTIVEGADGET;
    struct StringInfo *si = r->native->SpecialInfo;
    WORD value;
    LONG number;

    if (changed)
    {
        m68k_write16(ctx, r->guest + G_GD_FLAGS,
                     (m68k_read16(ctx, r->guest + G_GD_FLAGS) & ~changed) | (native & changed));
        r->flags ^= changed;
    }
    if (active != r->activation)
    {
        m68k_write16(ctx, r->guest + G_GD_ACTIVATION,
                     (m68k_read16(ctx, r->guest + G_GD_ACTIVATION) & ~GACT_ACTIVEGADGET) |
                     active);
        r->activation = active;
    }
    if (r->prop)
        prop_out(ctx, r);
    if (!r->info)
        return;
    if ((value = si->BufferPos) != r->pos)
        m68k_write16(ctx, r->info + G_SI_BUFFERPOS, (UWORD)(r->pos = value));
    if ((value = si->DispPos) != r->disp)
        m68k_write16(ctx, r->info + G_SI_DISPPOS, (UWORD)(r->disp = value));
    if ((value = si->UndoPos) != r->undo)
        m68k_write16(ctx, r->info + G_SI_UNDOPOS, (UWORD)(r->undo = value));
    if ((value = si->NumChars) != r->chars)
        m68k_write16(ctx, r->info + G_SI_NUMCHARS, (UWORD)(r->chars = value));
    if ((value = si->DispCount) != r->count)
        m68k_write16(ctx, r->info + G_SI_DISPCOUNT, (UWORD)(r->count = value));
    if ((number = si->LongInt) != r->number)
        m68k_write32(ctx, r->info + G_SI_LONGINT, (ULONG)(r->number = number));
}


/* The copy came off its window, natively: Intuition no longer edits a
   string gadget there, which loses its selection, as on AmigaOS 3.2.3.
   The program's gadget follows, and the copy retires. */
static void take_off(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    if (r->info)
        AROS_ATOMIC_AND(r->native->Flags, ~GFLG_SELECTED);
    state_out(ctx, r);
    retire(ctx, r);
}

/* ── Windows ─────────────────────────────────────────────────────── */

void Kaffeine_ClassicRelink(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineClassic *r, *next;
    struct Gadget *g;
    ULONG guest;

    for (r = ctx->classic; r; r = r->next)
        if (r->on && r->window == w)
        {
            /* the program's gadget that follows on the native list, a
               GadTools one or its own; 0 for a system gadget */
            g = r->native->NextGadget;
            guest = g ? Kaffeine_ShadowGuest(ctx, g, KAFFEINE_SHADOW_GADGET) : 0;
            if (g && !guest && (next = of_native(ctx, g)) && next->on)
                guest = next->guest;
            m68k_write32(ctx, r->guest + G_GD_NEXTGADGET, guest);
        }
}

/* Whether a message of the window of r, waiting in its port or held by
   the program, names the copy of r */
static BOOL named(struct KaffeineContext *ctx, struct KaffeineClassic *r)
{
    struct IntuiMessage *im;
    struct Node *n;
    BOOL found = FALSE;
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if ((ctx->shadow_map[i].type == KAFFEINE_SHADOW_INTUIMESSAGE ||
             ctx->shadow_map[i].type == KAFFEINE_SHADOW_GTMESSAGE) &&
            ((struct IntuiMessage *)ctx->shadow_map[i].native_ptr)->IAddress == r->native)
            return TRUE;
    if (r->window->UserPort)
    {
        Forbid();
        ForeachNode(&r->window->UserPort->mp_MsgList, n)
        {
            im = (struct IntuiMessage *)n;
            if (im->IAddress == r->native)
            {
                found = TRUE;
                break;
            }
        }
        Permit();
    }
    return found;
}

/* The copies that came off a window and that no message names go. */
static void sweep(struct KaffeineContext *ctx)
{
    struct KaffeineClassic *r, *next;

    for (r = ctx->classic; r; r = next)
    {
        next = r->next;
        if (!r->on && !r->marked && !named(ctx, r))
            record_free(ctx, r);
    }
}

/* The marked copies are on w now. */
static void attach_marked(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineClassic *r;

    for (r = ctx->classic; r; r = r->next)
        if (r->marked)
        {
            r->marked = FALSE;
            r->on = TRUE;
            r->window = w;
        }
    Kaffeine_ClassicRelink(ctx, w);
}

struct Gadget *Kaffeine_ClassicPrepare(struct KaffeineContext *ctx, ULONG guest, const char *call)
{
    ULONG count;

    return run_copy(ctx, guest, -1, &count, call);
}

void Kaffeine_ClassicOpened(struct KaffeineContext *ctx, struct Window *w)
{
    attach_marked(ctx, w);
    /* the window drew its gadgets, which counts the text of a string */
    Kaffeine_ClassicSync(ctx, w, NULL);
}

void Kaffeine_ClassicDropped(struct KaffeineContext *ctx)
{
    drop_marked(ctx);
}

IPTR Kaffeine_ClassicAdd(struct KaffeineContext *ctx, struct Window *w, ULONG guest, UWORD position,
                        WORD num, BOOL one, const char *call)
{
    struct Gadget *head;
    ULONG count;

    if (!(head = run_copy(ctx, guest, one ? 1 : num, &count, call)))
        return ctx->running ? (IPTR)(UWORD)~0 : (IPTR)ctx->exit_code;
    position = one ? AddGadget(w, head, position) : AddGList(w, head, position, (WORD)count, NULL);
    attach_marked(ctx, w);
    /* AROS's AddGList() draws the gadgets, which counts the text of a string */
    Kaffeine_ClassicSync(ctx, w, NULL);
    sweep(ctx);
    return (IPTR)position;
}

IPTR Kaffeine_ClassicRemove(struct KaffeineContext *ctx, struct Window *w, ULONG guest, WORD num,
                           const char *call)
{
    struct KaffeineClassic *r = on_window(ctx, guest), *last = NULL;
    struct Gadget *g;
    ULONG n = 0;
    UWORD pos;

    if (!r || r->window != w)
        return (IPTR)(UWORD)~0;   /* as Intuition, which does not find it there */
    /* the run on the native list: the program's own gadgets only */
    for (g = r->native; g && (num < 0 || n < (ULONG)num); g = g->NextGadget, n++)
    {
        if (!(last = of_native(ctx, g)) || !last->on || last->window != w)
        {
            for (r = ctx->classic; r; r = r->next)
                r->marked = FALSE;
            bug("[kaffeine] %s: a run that goes on with gadgets that are not the program's own\n",
                call);
            return refuse(ctx, "gadgets that cannot come off the window so");
        }
        last->marked = TRUE;
    }
    pos = RemoveGList(w, r->native, (WORD)n);
    if (pos == (UWORD)~0)
        bug("[kaffeine] %s: the gadgets were not on the window\n", call);
    for (r = ctx->classic; r; r = r->next)
        if (r->marked)
        {
            r->marked = FALSE;
            if (pos == (UWORD)~0)
                continue;
            take_off(ctx, r);
        }
    if (pos != (UWORD)~0)
    {
        /* The NextGadget of the last one stays, on the gadget that
           followed it, as AmigaOS 3.2.3 and AROS leave it: the NDK says
           that V36 cuts it, but DOpus relies on it. */
        Kaffeine_ClassicRelink(ctx, w);
        sweep(ctx);
    }
    return (IPTR)pos;
}

void Kaffeine_ClassicRefreshing(struct KaffeineContext *ctx, struct Window *w, struct Gadget *g,
                               WORD num)
{
    struct KaffeineClassic *r;
    ULONG n;

    for (n = 0; g && (num < 0 || n < (ULONG)num) && n < MAX_RUN; g = g->NextGadget, n++)
        if ((r = of_native(ctx, g)) && r->on && r->window == w)
            state_in(ctx, r);
}

BOOL Kaffeine_ClassicEnable(struct KaffeineContext *ctx, ULONG guest, struct Window *w, BOOL off)
{
    struct KaffeineClassic *r = on_window(ctx, guest), *each;
    UWORD flags;

    if (!r)
    {
        /* a gadget on no window: its own Flags, as Intuition sets them,
           with nothing to draw */
        if (!guest_range(ctx, guest, G_GADGET_SIZE) ||
            Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_GADGET))
            return FALSE;
        flags = m68k_read16(ctx, guest + G_GD_FLAGS);
        m68k_write16(ctx, guest + G_GD_FLAGS, off ? flags | GFLG_DISABLED : flags & ~GFLG_DISABLED);
        return TRUE;
    }
    if (w == r->window)
    {
        /* Intuition draws the gadgets after it too */
        for (each = ctx->classic; each; each = each->next)
            if (each->on && each->window == w)
                state_in(ctx, each);
        if (off)
            OffGadget(r->native, w, NULL);
        else
            OnGadget(r->native, w, NULL);
        Kaffeine_ClassicSync(ctx, w, NULL);
        return TRUE;
    }
    state_in(ctx, r);
    if (off)
        AROS_ATOMIC_OR(r->native->Flags, GFLG_DISABLED);
    else
        AROS_ATOMIC_AND(r->native->Flags, ~GFLG_DISABLED);
    state_out(ctx, r);
    return TRUE;
}

BOOL Kaffeine_ClassicModifyProp(struct KaffeineContext *ctx, ULONG guest, struct Window *w,
                               const UWORD *values, WORD num, BOOL all)
{
    struct KaffeineClassic *r = on_window(ctx, guest), *each;
    ULONG pi, i;

    /* its copy natively, which Intuition draws into the window given, as
       AmigaOS 3.2.3 draws a gadget into another window than its own */
    if (r && r->prop)
    {
        /* with AUTOKNOB as the copy has it, see PROP_FLAGS */
        UWORD flags = (values[0] & PROP_FLAGS) |
                      (((struct PropInfo *)r->native->SpecialInfo)->Flags & AUTOKNOB);

        /* Intuition draws the gadgets after it too */
        for (each = ctx->classic; each; each = each->next)
            if (each->on && each->window == r->window)
                state_in(ctx, each);
        if (all)
            ModifyProp(r->native, w, NULL, flags, values[1], values[2], values[3], values[4]);
        else
            NewModifyProp(r->native, w, NULL, flags, values[1], values[2], values[3], values[4],
                          num);
        Kaffeine_ClassicSync(ctx, r->window, NULL);
        /* the program's PropInfo and knob as Intuition left the copy's,
           also where the program had changed what the copy did not take
           yet, or what Intuition worked out the same again, with the
           AUTOKNOB given; the knob's size only where both have AUTOKNOB,
           as Intuition writes it only for one */
        m68k_write16(ctx, r->prop + G_PI_FLAGS, (r->pflags & PROP_FLAGS) | (values[0] & AUTOKNOB));
        for (i = 0; i < 4; i++)
            m68k_write16(ctx, r->prop + G_PI_POTS + 2 * i, r->pots[i]);
        for (i = 0; i < 6; i++)
            m68k_write16(ctx, r->prop + G_PI_CWIDTH + 2 * i, r->calc[i]);
        if (r->knob)
            for (i = 0; i < ((values[0] & flags & AUTOKNOB) ? 4 : 2); i++)
                m68k_write16(ctx, r->knob + G_IM_LEFTEDGE + 2 * i, (UWORD)r->place[i]);
        return TRUE;
    }
    /* a gadget on no window: its PropInfo takes the values, where AmigaOS
       3.2.3 also works out the rest of it and draws the gadget into the
       window given */
    if (!guest_range(ctx, guest, G_GADGET_SIZE) ||
        Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_GADGET) ||
        (m68k_read16(ctx, guest + G_GD_GADGETTYPE) & GTYP_GTYPEMASK) != GTYP_PROPGADGET ||
        !guest_range(ctx, pi = m68k_read32(ctx, guest + G_GD_SPECIALINFO), G_PROPINFO_SIZE))
        return FALSE;
    for (i = 0; i < 5; i++)
        m68k_write16(ctx, pi + G_PI_FLAGS + 2 * i, values[i]);
    return TRUE;
}

void Kaffeine_ClassicSync(struct KaffeineContext *ctx, struct Window *w, const void *address)
{
    struct KaffeineClassic *r;

    for (r = ctx->classic; r; r = r->next)
        if (!r->marked && (r->window == w || r->native == address))
            state_out(ctx, r);
}

void Kaffeine_ClassicClosing(struct KaffeineContext *ctx, struct Window *w)
{
    struct KaffeineClassic *r, *next;
    struct Gadget *g, *head;
    ULONG count;

    /* run by run, as the native list holds them */
    for (;;)
    {
        for (head = w->FirstGadget; head; head = head->NextGadget)
            if ((r = of_native(ctx, head)) && r->on && r->window == w)
                break;
        if (!head)
            break;
        for (count = 0, g = head; g && (r = of_native(ctx, g)) && r->on && r->window == w;
             g = g->NextGadget)
            count++;
        if (RemoveGList(w, head, (WORD)count) == (UWORD)~0)
        {
            bug("[kaffeine] the program's gadgets were not on the list of their window\n");
            break;
        }
        for (; count--; head = g)
        {
            g = head->NextGadget;
            if ((r = of_native(ctx, head)))
                take_off(ctx, r);
        }
    }
    /* the window and its messages go: so do the copies of its gadgets */
    for (r = ctx->classic; r; r = next)
    {
        next = r->next;
        if (r->window == w)
        {
            if (r->on)
                bug("[kaffeine] a gadget of the program's own that was not on its window\n");
            record_free(ctx, r);
        }
    }
}

void Kaffeine_ClassicCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineClassic *r;

    while ((r = ctx->classic))
    {
        if (r->window && !r->marked)
            Kaffeine_ClassicClosing(ctx, r->window);
        else
            record_free(ctx, r);
    }
}
