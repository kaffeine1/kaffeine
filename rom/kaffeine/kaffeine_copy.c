/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Native copies of 68k structures that native code reads itself, made in
    two runs over the same input: one that checks it and counts, one that
    fills one block in the same order. Every copy is checked against guest
    memory before native code may follow it, and chains end within a
    limit, which also ends a chain in a cycle. Strings are copied too, so
    that what the program writes later cannot take their end away, and
    texts that use the same TextAttr one after another share its copy.
    Border chains get their XY pairs as native WORDs, and a gadget gets
    the renders and the label that Intuition reads of it, a string gadget
    its StringInfo, whose text buffers stay in guest memory, where
    Intuition edits the program's text itself, and a proportional gadget
    its PropInfo and the Image of its knob, into which Intuition writes
    where the knob is.
*/

#include "kaffeine_intern.h"
#include "kaffeine_copy.h"
#include "kaffeine_rastport.h"

#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <string.h>

/* The 68k structures of the NDK */
enum
{
    G_IT_FRONTPEN = 0, G_IT_BACKPEN = 1, G_IT_DRAWMODE = 2, G_IT_LEFTEDGE = 4,
    G_IT_TOPEDGE = 6, G_IT_ITEXTFONT = 8, G_IT_ITEXT = 12, G_IT_NEXTTEXT = 16,
    G_INTUITEXT_SIZE = 20,

    G_TA_NAME = 0, G_TA_YSIZE = 4, G_TA_STYLE = 6, G_TA_FLAGS = 7, G_TEXTATTR_SIZE = 8,

    G_IM_LEFTEDGE = 0, G_IM_TOPEDGE = 2, G_IM_WIDTH = 4, G_IM_HEIGHT = 6, G_IM_DEPTH = 8,
    G_IM_IMAGEDATA = 10, G_IM_PLANEPICK = 14, G_IM_PLANEONOFF = 15, G_IM_NEXTIMAGE = 16,
    G_IMAGE_SIZE = 20,

    G_BD_LEFTEDGE = 0, G_BD_TOPEDGE = 2, G_BD_FRONTPEN = 4, G_BD_BACKPEN = 5,
    G_BD_DRAWMODE = 6, G_BD_COUNT = 7, G_BD_XY = 8, G_BD_NEXTBORDER = 12, G_BORDER_SIZE = 16,

    /* ExtGadget: a Gadget, then the fields that GFLG_EXTENDED announces */
    G_GD_LEFTEDGE = 4, G_GD_TOPEDGE = 6, G_GD_WIDTH = 8, G_GD_HEIGHT = 10, G_GD_FLAGS = 12,
    G_GD_ACTIVATION = 14, G_GD_GADGETTYPE = 16, G_GD_GADGETRENDER = 18,
    G_GD_SELECTRENDER = 22, G_GD_GADGETTEXT = 26, G_GD_MUTUALEXCLUDE = 30,
    G_GD_GADGETID = 38, G_GD_USERDATA = 40, G_GADGET_SIZE = 44, G_GD_MOREFLAGS = 44,
    G_GD_BOUNDSLEFTEDGE = 48, G_GD_BOUNDSTOPEDGE = 50, G_GD_BOUNDSWIDTH = 52,
    G_GD_BOUNDSHEIGHT = 54, G_EXTGADGET_SIZE = 56,
    /* GMORE_BOUNDS and GMORE_SCROLLRASTER, the same bits in AROS, whose
       GMORE_BOOPSIGADGET is another bit than the NDK's. GMORE_GADGETHELP
       stays out until a test with the pointer checks it: before #71 the
       input handler kept the gadget under the pointer for its help past
       RemoveGList(), after which a copy may go. */
    G_GMORE_COPIED = 0x5,

    G_CUSTOMIMAGEDEPTH = -1,

    /* The SpecialInfo of a string gadget: a StringInfo, and the
       StringExtend of GFLG_STRINGEXTEND or GACT_STRINGEXTEND */
    G_GD_SPECIALINFO = 34,
    G_SI_BUFFER = 0, G_SI_UNDOBUFFER = 4, G_SI_BUFFERPOS = 8, G_SI_MAXCHARS = 10,
    G_SI_DISPPOS = 12, G_SI_UNDOPOS = 14, G_SI_NUMCHARS = 16, G_SI_DISPCOUNT = 18,
    G_SI_CLEFT = 20, G_SI_CTOP = 22, G_SI_EXTENSION = 24, G_SI_LONGINT = 28,
    G_STRINGINFO_SIZE = 36,
    G_SE_FONT = 0, G_SE_PENS = 4, G_SE_ACTIVEPENS = 6, G_SE_INITIALMODES = 8,
    G_SE_EDITHOOK = 12, G_SE_WORKBUFFER = 16, G_STRINGEXTEND_SIZE = 36,
    /* The SpecialInfo of a proportional gadget */
    G_PI_FLAGS = 0, G_PI_HORIZPOT = 2, G_PI_VERTPOT = 4, G_PI_HORIZBODY = 6, G_PI_VERTBODY = 8,
    G_PI_CWIDTH = 10, G_PI_CHEIGHT = 12, G_PI_HPOTRES = 14, G_PI_VPOTRES = 16,
    G_PI_LEFTBORDER = 18, G_PI_TOPBORDER = 20, G_PROPINFO_SIZE = 22
};

/* Longer chains than this are no chain a program means, and more copies
   than this no menu strip or gadget list a program means. */
#define MAX_CHAIN 1024
#define MAX_COPIES 32768
/* TextExtent() and TextLength() give the width of a text in a WORD, which
   the layouts of Intuition add up in WORDs too: the longest string here
   stays below 32768 pixels with glyphs up to 128 pixels wide. */
#define MAX_STRING 256
/* Every copy starts on this boundary, which no native structure here
   needs more than. */
#define COPY_ALIGN 8

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

void Kaffeine_CopyRefuse(struct KaffeineCopy *c, const char *why)
{
    if (!c->why)
        c->why = why;
}

APTR Kaffeine_CopyTake(struct KaffeineCopy *c, ULONG size)
{
    ULONG rounded = (size + COPY_ALIGN - 1) & ~(ULONG)(COPY_ALIGN - 1);
    APTR at;

    if (c->why)
        return NULL;
    if (++c->objects > MAX_COPIES)
    {
        Kaffeine_CopyRefuse(c, "more structures than the bridge copies");
        return NULL;
    }
    if (!c->block)
    {
        c->used += rounded;
        return NULL;
    }
    /* The second run takes what the first counted; more would mean that
       the runs went apart, which the same input does not do. */
    if (c->room - c->used < rounded)
    {
        Kaffeine_CopyRefuse(c, "a copy larger than its count");
        return NULL;
    }
    at = c->block + c->used;
    c->used += rounded;
    return at;
}

void Kaffeine_CopyFill(struct KaffeineCopy *c, APTR block)
{
    c->block = block;
    c->room = c->used;
    c->used = 0;
    c->objects = 0;
    c->font_guest = 0;
    c->font = NULL;
}

STRPTR Kaffeine_CopyString(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    const UBYTE *end = NULL;
    STRPTR copy;
    ULONG size;

    if (!guest)
        return NULL;
    if (guest_range(ctx, guest, 1))
        end = memchr(ctx->mem + guest, 0, ctx->mem_size - guest < MAX_STRING ? ctx->mem_size - guest
                                                                              : MAX_STRING);
    if (!end)
    {
        Kaffeine_CopyRefuse(c, "a string that does not end in guest memory, or longer than "
                              "255 characters");
        return NULL;
    }
    size = end - (ctx->mem + guest) + 1;
    if ((copy = Kaffeine_CopyTake(c, size)))
        memcpy(copy, ctx->mem + guest, size);
    return copy;
}

static struct TextAttr *copy_font(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct TextAttr *ta;
    STRPTR name;

    if (!guest_range(ctx, guest, G_TEXTATTR_SIZE))
    {
        Kaffeine_CopyRefuse(c, "a TextAttr outside guest memory");
        return NULL;
    }
    /* both runs, so that the first counts what the second takes */
    if (c->font_guest == guest)
        return c->font;
    name = Kaffeine_CopyString(ctx, c, m68k_read32(ctx, guest + G_TA_NAME));
    ta = Kaffeine_CopyTake(c, sizeof(*ta));
    c->font_guest = guest;
    c->font = ta;
    if (!ta)
        return NULL;
    ta->ta_Name = name;
    ta->ta_YSize = m68k_read16(ctx, guest + G_TA_YSIZE);
    /* The tags of a TTextAttr stay behind: OpenFont() does not read them,
       and the copy has none to show. */
    ta->ta_Style = ctx->mem[guest + G_TA_STYLE] & ~FSF_TAGGED;
    ta->ta_Flags = ctx->mem[guest + G_TA_FLAGS];
    return ta;
}

struct IntuiText *Kaffeine_CopyTexts(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct IntuiText *first = NULL, *prev = NULL, *it;
    struct TextAttr *font;
    STRPTR text;
    ULONG n, attr;

    for (n = 0; guest && !c->why; n++, guest = m68k_read32(ctx, guest + G_IT_NEXTTEXT))
    {
        if (n == MAX_CHAIN || !guest_range(ctx, guest, G_INTUITEXT_SIZE))
        {
            Kaffeine_CopyRefuse(c, "an IntuiText outside guest memory, or a chain of them "
                                  "in a cycle");
            break;
        }
        attr = m68k_read32(ctx, guest + G_IT_ITEXTFONT);
        font = attr ? copy_font(ctx, c, attr) : NULL;
        text = Kaffeine_CopyString(ctx, c, m68k_read32(ctx, guest + G_IT_ITEXT));
        if (!(it = Kaffeine_CopyTake(c, sizeof(*it))))
            continue;
        it->FrontPen = ctx->mem[guest + G_IT_FRONTPEN];
        it->BackPen = ctx->mem[guest + G_IT_BACKPEN];
        it->DrawMode = ctx->mem[guest + G_IT_DRAWMODE];
        it->LeftEdge = (WORD)m68k_read16(ctx, guest + G_IT_LEFTEDGE);
        it->TopEdge = (WORD)m68k_read16(ctx, guest + G_IT_TOPEDGE);
        it->ITextFont = font;
        it->IText = (UBYTE *)text;
        it->NextText = NULL;
        if (prev)
            prev->NextText = it;
        else
            first = it;
        prev = it;
    }
    return c->why ? NULL : first;
}

struct Image *Kaffeine_CopyImages(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct Image *first = NULL, *prev = NULL, *im;
    LONG width, height, depth;
    ULONG n, data, planes, bytes, bit;
    UBYTE pick;

    for (n = 0; guest && !c->why; n++, guest = m68k_read32(ctx, guest + G_IM_NEXTIMAGE))
    {
        if (n == MAX_CHAIN || !guest_range(ctx, guest, G_IMAGE_SIZE))
        {
            Kaffeine_CopyRefuse(c, "an Image outside guest memory, or a chain of them in a cycle");
            break;
        }
        width = (WORD)m68k_read16(ctx, guest + G_IM_WIDTH);
        height = (WORD)m68k_read16(ctx, guest + G_IM_HEIGHT);
        depth = (WORD)m68k_read16(ctx, guest + G_IM_DEPTH);
        data = m68k_read32(ctx, guest + G_IM_IMAGEDATA);
        pick = ctx->mem[guest + G_IM_PLANEPICK];
        if (depth == G_CUSTOMIMAGEDEPTH)
        {
            Kaffeine_CopyRefuse(c, "an image of a BOOPSI class is not supported yet");
            break;
        }
        if (width < 0 || height < 0 || depth < 0 || depth > 8)
        {
            Kaffeine_CopyRefuse(c, "an Image with a negative size or more than 8 planes");
            break;
        }
        /* DrawImage() reads a plane for each bit of PlanePick, up to Depth
           planes, and none for a PlanePick of 0; it counts the words of a
           plane in a WORD. */
        for (planes = 0, bit = 0; bit < 8; bit++)
            planes += (pick >> bit) & 1;
        if (planes > (ULONG)depth)
            planes = depth;
        if (planes && ((width + 15) >> 4) * height > 0x7FFF)
        {
            Kaffeine_CopyRefuse(c, "an Image with more than 32767 words in a plane");
            break;
        }
        bytes = planes * (ULONG)(((width + 15) >> 4) * height) * 2;
        if (bytes && ((data & 1) || !guest_range(ctx, data, bytes)))
        {
            Kaffeine_CopyRefuse(c, "image data outside guest memory or at an odd address");
            break;
        }
        if (!(im = Kaffeine_CopyTake(c, sizeof(*im))))
            continue;
        im->LeftEdge = (WORD)m68k_read16(ctx, guest + G_IM_LEFTEDGE);
        im->TopEdge = (WORD)m68k_read16(ctx, guest + G_IM_TOPEDGE);
        im->Width = width;
        im->Height = height;
        im->Depth = depth;
        im->ImageData = bytes ? (UWORD *)(ctx->mem + data) : NULL;
        im->PlanePick = pick;
        im->PlaneOnOff = ctx->mem[guest + G_IM_PLANEONOFF];
        im->NextImage = NULL;
        if (prev)
            prev->NextImage = im;
        else
            first = im;
        prev = im;
    }
    return c->why ? NULL : first;
}

struct Border *Kaffeine_CopyBorders(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct Border *first = NULL, *prev = NULL, *bd;
    WORD *pairs;
    ULONG n, xy, i;
    BYTE count;

    for (n = 0; guest && !c->why; n++, guest = m68k_read32(ctx, guest + G_BD_NEXTBORDER))
    {
        if (n == MAX_CHAIN || !guest_range(ctx, guest, G_BORDER_SIZE))
        {
            Kaffeine_CopyRefuse(c, "a Border outside guest memory, or a chain of them in a cycle");
            break;
        }
        /* DrawBorder() draws Count pairs, and none for a Count below 1 */
        count = (BYTE)ctx->mem[guest + G_BD_COUNT];
        xy = m68k_read32(ctx, guest + G_BD_XY);
        if (count > 0 && !guest_range(ctx, xy, count * 4UL))
        {
            Kaffeine_CopyRefuse(c, "the XY pairs of a Border outside guest memory");
            break;
        }
        pairs = count > 0 ? Kaffeine_CopyTake(c, count * 2 * sizeof(WORD)) : NULL;
        if (!(bd = Kaffeine_CopyTake(c, sizeof(*bd))))
            continue;
        for (i = 0; pairs && i < count * 2UL; i++)
            pairs[i] = (WORD)m68k_read16(ctx, xy + i * 2);
        bd->LeftEdge = (WORD)m68k_read16(ctx, guest + G_BD_LEFTEDGE);
        bd->TopEdge = (WORD)m68k_read16(ctx, guest + G_BD_TOPEDGE);
        bd->FrontPen = ctx->mem[guest + G_BD_FRONTPEN];
        bd->BackPen = ctx->mem[guest + G_BD_BACKPEN];
        bd->DrawMode = ctx->mem[guest + G_BD_DRAWMODE];
        bd->Count = count;
        bd->XY = pairs;
        bd->NextBorder = NULL;
        if (prev)
            prev->NextBorder = bd;
        else
            first = bd;
        prev = bd;
    }
    return c->why ? NULL : first;
}

/* GadgetRender and SelectRender: Image chains for GFLG_GADGIMAGE, Border
   chains otherwise */
static APTR copy_render(struct KaffeineContext *ctx, struct KaffeineCopy *c, UWORD flags, ULONG guest)
{
    if (flags & GFLG_GADGIMAGE)
        return Kaffeine_CopyImages(ctx, c, guest);
    return Kaffeine_CopyBorders(ctx, c, guest);
}

/* A buffer of size bytes in guest memory */
static BOOL guest_buffer(struct KaffeineContext *ctx, ULONG addr, WORD size)
{
    return size > 0 && guest_range(ctx, addr, (ULONG)size);
}

/* The StringInfo of a string gadget, with a StringExtend when extended */
static struct StringInfo *copy_stringinfo(struct KaffeineContext *ctx, struct KaffeineCopy *c,
                                          ULONG guest, BOOL extended)
{
    struct StringInfo *si;
    struct StringExtend *se = NULL;
    struct TextFont *font = NULL;
    ULONG buffer, undo, ext = 0, work = 0;
    WORD maxchars;

    if (!guest_range(ctx, guest, G_STRINGINFO_SIZE))
    {
        Kaffeine_CopyRefuse(c, "a StringInfo outside guest memory");
        return NULL;
    }
    maxchars = (WORD)m68k_read16(ctx, guest + G_SI_MAXCHARS);
    buffer = m68k_read32(ctx, guest + G_SI_BUFFER);
    undo = m68k_read32(ctx, guest + G_SI_UNDOBUFFER);
    /* Intuition counts the text as far as its NUL, which may lie past
       MaxChars, as on AmigaOS, and stops in the guard after guest memory
       at the latest (KAFFEINE_MEM_GUARD) */
    if (!guest_buffer(ctx, buffer, maxchars))
        Kaffeine_CopyRefuse(c, "a string Buffer of less than MaxChars bytes in guest memory");
    else if (undo && !guest_buffer(ctx, undo, maxchars))
        Kaffeine_CopyRefuse(c, "an UndoBuffer of less than MaxChars bytes in guest memory");
    else if (extended)
    {
        ext = m68k_read32(ctx, guest + G_SI_EXTENSION);
        if (!guest_range(ctx, ext, G_STRINGEXTEND_SIZE))
            Kaffeine_CopyRefuse(c, "a StringExtend outside guest memory");
        /* Intuition calls it from the input handler's task */
        else if (m68k_read32(ctx, ext + G_SE_EDITHOOK))
            Kaffeine_CopyRefuse(c, "an EditHook of 68k code");
        else if (m68k_read32(ctx, ext + G_SE_FONT) &&
                 !(font = Kaffeine_FontNative(ctx, m68k_read32(ctx, ext + G_SE_FONT))))
            Kaffeine_CopyRefuse(c, "a string gadget font that the program did not open");
        else if ((work = m68k_read32(ctx, ext + G_SE_WORKBUFFER)) &&
                 !guest_buffer(ctx, work, maxchars))
            Kaffeine_CopyRefuse(c, "a WorkBuffer of less than MaxChars bytes in guest memory");
    }
    if (extended)
        se = Kaffeine_CopyTake(c, sizeof(*se));
    if (!(si = Kaffeine_CopyTake(c, sizeof(*si))) || (extended && !se))
        return NULL;
    /* the text stays in the program's buffers, where Intuition edits it */
    si->Buffer = ctx->mem + buffer;
    si->UndoBuffer = undo ? ctx->mem + undo : NULL;
    si->BufferPos = (WORD)m68k_read16(ctx, guest + G_SI_BUFFERPOS);
    si->MaxChars = maxchars;
    si->DispPos = (WORD)m68k_read16(ctx, guest + G_SI_DISPPOS);
    si->UndoPos = (WORD)m68k_read16(ctx, guest + G_SI_UNDOPOS);
    si->NumChars = (WORD)m68k_read16(ctx, guest + G_SI_NUMCHARS);
    si->DispCount = (WORD)m68k_read16(ctx, guest + G_SI_DISPCOUNT);
    si->CLeft = (WORD)m68k_read16(ctx, guest + G_SI_CLEFT);
    si->CTop = (WORD)m68k_read16(ctx, guest + G_SI_CTOP);
    si->Extension = se;
    si->LongInt = (LONG)m68k_read32(ctx, guest + G_SI_LONGINT);
    /* a 68k KeyMap is not a native one: a gadget with a keymap of its own
       stays off windows */
    si->AltKeyMap = NULL;
    if (se)
    {
        se->Font = font;
        se->Pens[0] = ctx->mem[ext + G_SE_PENS];
        se->Pens[1] = ctx->mem[ext + G_SE_PENS + 1];
        se->ActivePens[0] = ctx->mem[ext + G_SE_ACTIVEPENS];
        se->ActivePens[1] = ctx->mem[ext + G_SE_ACTIVEPENS + 1];
        se->InitialModes = m68k_read32(ctx, ext + G_SE_INITIALMODES);
        se->EditHook = NULL;
        se->WorkBuffer = work ? (STRPTR)(ctx->mem + work) : NULL;
    }
    return si;
}

/* The PropInfo of a proportional gadget, all of it */
static struct PropInfo *copy_propinfo(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct PropInfo *pi;

    if (!guest_range(ctx, guest, G_PROPINFO_SIZE))
    {
        Kaffeine_CopyRefuse(c, "a PropInfo outside guest memory");
        return NULL;
    }
    if (!(pi = Kaffeine_CopyTake(c, sizeof(*pi))))
        return NULL;
    pi->Flags = m68k_read16(ctx, guest + G_PI_FLAGS);
    pi->HorizPot = m68k_read16(ctx, guest + G_PI_HORIZPOT);
    pi->VertPot = m68k_read16(ctx, guest + G_PI_VERTPOT);
    pi->HorizBody = m68k_read16(ctx, guest + G_PI_HORIZBODY);
    pi->VertBody = m68k_read16(ctx, guest + G_PI_VERTBODY);
    pi->CWidth = m68k_read16(ctx, guest + G_PI_CWIDTH);
    pi->CHeight = m68k_read16(ctx, guest + G_PI_CHEIGHT);
    pi->HPotRes = m68k_read16(ctx, guest + G_PI_HPOTRES);
    pi->VPotRes = m68k_read16(ctx, guest + G_PI_VPOTRES);
    pi->LeftBorder = m68k_read16(ctx, guest + G_PI_LEFTBORDER);
    pi->TopBorder = m68k_read16(ctx, guest + G_PI_TOPBORDER);
    return pi;
}

/* The knob of a proportional gadget, an Image that Intuition writes where
   the knob is into, and for AUTOKNOB how large: for AUTOKNOB the Image
   alone, without its data, which programs may leave unset; else the
   program's own Image chain, which Intuition draws; and for a gadget
   without one an empty Image, which the program does not see, so that
   Intuition never writes through NULL */
static struct Image *copy_knob(struct KaffeineContext *ctx, struct KaffeineCopy *c, BOOL autoknob,
                               ULONG guest)
{
    struct Image *im;

    if (guest && !autoknob)
        return Kaffeine_CopyImages(ctx, c, guest);
    if (guest && !guest_range(ctx, guest, G_IMAGE_SIZE))
    {
        Kaffeine_CopyRefuse(c, "a knob Image outside guest memory");
        return NULL;
    }
    if (!(im = Kaffeine_CopyTake(c, sizeof(*im))))
        return NULL;
    im->LeftEdge = guest ? (WORD)m68k_read16(ctx, guest + G_IM_LEFTEDGE) : 0;
    im->TopEdge = guest ? (WORD)m68k_read16(ctx, guest + G_IM_TOPEDGE) : 0;
    im->Width = guest ? (WORD)m68k_read16(ctx, guest + G_IM_WIDTH) : 0;
    im->Height = guest ? (WORD)m68k_read16(ctx, guest + G_IM_HEIGHT) : 0;
    im->Depth = 0;
    im->ImageData = NULL;
    im->PlanePick = im->PlaneOnOff = 0;
    im->NextImage = NULL;
    return im;
}

struct Gadget *Kaffeine_CopyGadget(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest)
{
    struct ExtGadget *eg;
    struct Gadget *g;
    APTR render, select = NULL, text = NULL, info = NULL;
    ULONG label;
    UWORD flags, activation, type;

    if (!guest_range(ctx, guest, G_GADGET_SIZE))
    {
        Kaffeine_CopyRefuse(c, "a Gadget outside guest memory");
        return NULL;
    }
    flags = m68k_read16(ctx, guest + G_GD_FLAGS);
    if ((flags & GFLG_EXTENDED) && !guest_range(ctx, guest, G_EXTGADGET_SIZE))
    {
        Kaffeine_CopyRefuse(c, "an ExtGadget outside guest memory");
        return NULL;
    }
    type = m68k_read16(ctx, guest + G_GD_GADGETTYPE);
    if ((type & GTYP_GTYPEMASK) == GTYP_PROPGADGET)
    {
        /* the knob, an Image whatever the flags and never NULL; Intuition
           reads no SelectRender of a proportional gadget */
        ULONG pi = m68k_read32(ctx, guest + G_GD_SPECIALINFO);

        info = copy_propinfo(ctx, c, pi);
        render = c->why ? NULL :
                 copy_knob(ctx, c, (m68k_read16(ctx, pi + G_PI_FLAGS) & AUTOKNOB) != 0,
                           m68k_read32(ctx, guest + G_GD_GADGETRENDER));
    }
    else
    {
        render = copy_render(ctx, c, flags, m68k_read32(ctx, guest + G_GD_GADGETRENDER));
        /* Intuition draws SelectRender only for GFLG_GADGHIMAGE; otherwise
           the program may leave anything in it. */
        if ((flags & GFLG_GADGHIGHBITS) == GFLG_GADGHIMAGE)
            select = copy_render(ctx, c, flags, m68k_read32(ctx, guest + G_GD_SELECTRENDER));
    }
    /* GadgetText as Intuition reads it, and nothing for both label bits */
    label = m68k_read32(ctx, guest + G_GD_GADGETTEXT);
    switch (flags & GFLG_LABELMASK)
    {
    case GFLG_LABELITEXT:
        text = Kaffeine_CopyTexts(ctx, c, label);
        break;
    case GFLG_LABELSTRING:
        text = Kaffeine_CopyString(ctx, c, label);
        break;
    case GFLG_LABELIMAGE:
        text = Kaffeine_CopyImages(ctx, c, label);
        break;
    }
    activation = m68k_read16(ctx, guest + G_GD_ACTIVATION);
    if ((type & GTYP_GTYPEMASK) == GTYP_STRGADGET)
    {
        /* GACT_STRINGEXTEND means GFLG_STRINGEXTEND since V37; the copy
           carries the Flags bit */
        if (activation & GACT_STRINGEXTEND)
            flags |= GFLG_STRINGEXTEND;
        info = copy_stringinfo(ctx, c, m68k_read32(ctx, guest + G_GD_SPECIALINFO),
                               (flags & GFLG_STRINGEXTEND) != 0);
    }
    if (!(g = Kaffeine_CopyTake(c, (flags & GFLG_EXTENDED) ? sizeof(*eg) : sizeof(*g))))
        return NULL;
    g->NextGadget = NULL;
    g->LeftEdge = (WORD)m68k_read16(ctx, guest + G_GD_LEFTEDGE);
    g->TopEdge = (WORD)m68k_read16(ctx, guest + G_GD_TOPEDGE);
    g->Width = (WORD)m68k_read16(ctx, guest + G_GD_WIDTH);
    g->Height = (WORD)m68k_read16(ctx, guest + G_GD_HEIGHT);
    g->Flags = flags;
    g->Activation = activation;
    g->GadgetType = type;
    g->GadgetRender = render;
    g->SelectRender = select;
    g->GadgetText = text;
    g->MutualExclude = m68k_read32(ctx, guest + G_GD_MUTUALEXCLUDE);
    g->SpecialInfo = info;
    g->GadgetID = m68k_read16(ctx, guest + G_GD_GADGETID);
    g->UserData = (APTR)(IPTR)m68k_read32(ctx, guest + G_GD_USERDATA);
    if (flags & GFLG_EXTENDED)
    {
        eg = (struct ExtGadget *)g;
        eg->MoreFlags = m68k_read32(ctx, guest + G_GD_MOREFLAGS) & G_GMORE_COPIED;
        eg->BoundsLeftEdge = (WORD)m68k_read16(ctx, guest + G_GD_BOUNDSLEFTEDGE);
        eg->BoundsTopEdge = (WORD)m68k_read16(ctx, guest + G_GD_BOUNDSTOPEDGE);
        eg->BoundsWidth = (WORD)m68k_read16(ctx, guest + G_GD_BOUNDSWIDTH);
        eg->BoundsHeight = (WORD)m68k_read16(ctx, guest + G_GD_BOUNDSHEIGHT);
    }
    return g;
}
