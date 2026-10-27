/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    68k mirrors of native Intuition objects that GadTools builds, such as
    menu strips and gadget lists. A mirror keeps its own table of native
    objects and their 68k copies in guest memory, so a menu or a gadget
    list of any size takes no entry of the shadow table, and copies the
    strings, TextAttrs, IntuiTexts and images that the objects point to.
    A string that lies in guest memory, as the program's own labels do,
    stays where it is; others get a guest copy. A TextAttr that the program
    gave is kept as a native copy while the mirror lives, since native
    objects point to it, and its 68k mirror is the program's own.
*/

#include "kaffeine_intern.h"
#include "kaffeine_mirror.h"

#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
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
    G_IMAGE_SIZE = 20
};

/* Longer chains than this are no chain a program means. */
#define MAX_CHAIN 1024

struct KaffeineMirrorFont
{
    struct KaffeineMirrorFont *next;
    ULONG guest;
    struct TextAttr attr;
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* A guest address for a native pointer into guest memory, else 0. */
static ULONG guest_address(struct KaffeineContext *ctx, const void *ptr)
{
    const UBYTE *p = (const UBYTE *)ptr;

    return p && p > ctx->mem && p < ctx->mem + ctx->mem_size ? (ULONG)(p - ctx->mem) : 0;
}

struct KaffeineMirrorNode *Kaffeine_MirrorFind(struct KaffeineMirror *m, const void *native,
                                             UBYTE kind)
{
    ULONG i;

    for (i = 0; i < m->count; i++)
        if (m->nodes[i].native == native && m->nodes[i].kind == kind)
            return &m->nodes[i];
    return NULL;
}

struct KaffeineMirrorNode *Kaffeine_MirrorGuest(struct KaffeineMirror *m, ULONG guest, UBYTE kind)
{
    ULONG i;

    for (i = 0; guest && i < m->count; i++)
        if (m->nodes[i].guest == guest && m->nodes[i].kind == kind)
            return &m->nodes[i];
    return NULL;
}

ULONG Kaffeine_MirrorNode(struct KaffeineContext *ctx, struct KaffeineMirror *m, void *native,
                         UBYTE kind, ULONG size, BOOL *fresh)
{
    struct KaffeineMirrorNode *n = Kaffeine_MirrorFind(m, native, kind);
    ULONG guest;

    if (fresh)
        *fresh = !n;
    if (n)
        return n->guest;
    if (m->count == m->capacity)
    {
        ULONG capacity = m->capacity ? 2 * m->capacity : 32;
        struct KaffeineMirrorNode *nodes = AllocVec(capacity * sizeof(*nodes), MEMF_ANY);

        if (!nodes)
        {
            m->failed = TRUE;
            return 0;
        }
        if (m->count)
            memcpy(nodes, m->nodes, m->count * sizeof(*nodes));
        FreeVec(m->nodes);
        m->nodes = nodes;
        m->capacity = capacity;
    }
    guest = Kaffeine_MirrorAlloc(ctx, size);
    if (!guest)
    {
        m->failed = TRUE;
        return 0;
    }
    n = &m->nodes[m->count++];
    n->native = native;
    n->guest = guest;
    n->size = size;
    n->kind = kind;
    return guest;
}

ULONG Kaffeine_MirrorString(struct KaffeineContext *ctx, struct KaffeineMirror *m, const char *str)
{
    struct KaffeineMirrorNode *n;
    ULONG guest, size;

    if (!str)
        return 0;
    if ((guest = guest_address(ctx, str)))
        return guest;
    if ((n = Kaffeine_MirrorFind(m, str, KAFFEINE_MIRROR_STRING)))
        return n->guest;
    size = strlen(str) + 1;
    guest = Kaffeine_MirrorNode(ctx, m, (void *)str, KAFFEINE_MIRROR_STRING, size, NULL);
    if (guest)
        memcpy(ctx->mem + guest, str, size);
    return guest;
}

ULONG Kaffeine_MirrorFont(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct TextAttr *ta)
{
    struct KaffeineMirrorFont *f;
    ULONG guest, name;

    if (!ta)
        return 0;
    for (f = m->fonts; f; f = f->next)
        if (&f->attr == ta)
            return f->guest;
    /* the name first: no TextAttr without its name */
    name = Kaffeine_MirrorString(ctx, m, (const char *)ta->ta_Name);
    if (ta->ta_Name && !name)
        return 0;
    guest = Kaffeine_MirrorNode(ctx, m, ta, KAFFEINE_MIRROR_FONT, G_TEXTATTR_SIZE, NULL);
    if (!guest)
        return 0;
    m68k_write32(ctx, guest + G_TA_NAME, name);
    m68k_write16(ctx, guest + G_TA_YSIZE, ta->ta_YSize);
    ctx->mem[guest + G_TA_STYLE] = ta->ta_Style;
    ctx->mem[guest + G_TA_FLAGS] = ta->ta_Flags;
    return guest;
}

/* The copies that a TextAttr needs, its name and itself, made without
   writing a copy that the program may already see */
static BOOL reserve_font(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct TextAttr *ta)
{
    struct KaffeineMirrorFont *f;

    for (f = m->fonts; f; f = f->next)
        if (&f->attr == ta)
            return TRUE;
    if (ta->ta_Name && !Kaffeine_MirrorString(ctx, m, (const char *)ta->ta_Name))
        return FALSE;
    return Kaffeine_MirrorNode(ctx, m, ta, KAFFEINE_MIRROR_FONT, G_TEXTATTR_SIZE, NULL) != 0;
}

ULONG Kaffeine_MirrorTexts(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct IntuiText *it)
{
    ULONG first = 0, prev = 0, guest, n;
    struct IntuiText *t;

    /* Every copy that the chain needs first, and only new ones written: a
       chain without guest memory for all of them is not changed at all,
       since its old copy may be in use, and 0 says so. */
    for (n = 0, t = it; t && n < MAX_CHAIN; t = t->NextText, n++)
        if (!Kaffeine_MirrorNode(ctx, m, t, KAFFEINE_MIRROR_TEXT, G_INTUITEXT_SIZE, NULL) ||
            (t->ITextFont && !reserve_font(ctx, m, t->ITextFont)) ||
            (t->IText && !Kaffeine_MirrorString(ctx, m, (const char *)t->IText)))
            return 0;
    for (n = 0; it && n < MAX_CHAIN; it = it->NextText, n++)
    {
        guest = Kaffeine_MirrorNode(ctx, m, it, KAFFEINE_MIRROR_TEXT, G_INTUITEXT_SIZE, NULL);
        if (!guest)
            break;
        ctx->mem[guest + G_IT_FRONTPEN] = it->FrontPen;
        ctx->mem[guest + G_IT_BACKPEN] = it->BackPen;
        ctx->mem[guest + G_IT_DRAWMODE] = it->DrawMode;
        m68k_write16(ctx, guest + G_IT_LEFTEDGE, (UWORD)it->LeftEdge);
        m68k_write16(ctx, guest + G_IT_TOPEDGE, (UWORD)it->TopEdge);
        m68k_write32(ctx, guest + G_IT_ITEXTFONT, Kaffeine_MirrorFont(ctx, m, it->ITextFont));
        m68k_write32(ctx, guest + G_IT_ITEXT, Kaffeine_MirrorString(ctx, m, (const char *)it->IText));
        m68k_write32(ctx, guest + G_IT_NEXTTEXT, 0);
        if (prev)
            m68k_write32(ctx, prev + G_IT_NEXTTEXT, guest);
        else
            first = guest;
        prev = guest;
    }
    return first;
}

ULONG Kaffeine_MirrorImage(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct Image *im)
{
    ULONG guest = Kaffeine_MirrorNode(ctx, m, im, KAFFEINE_MIRROR_IMAGE, G_IMAGE_SIZE, NULL);

    if (!guest)
        return 0;
    m68k_write16(ctx, guest + G_IM_LEFTEDGE, (UWORD)im->LeftEdge);
    m68k_write16(ctx, guest + G_IM_TOPEDGE, (UWORD)im->TopEdge);
    m68k_write16(ctx, guest + G_IM_WIDTH, (UWORD)im->Width);
    m68k_write16(ctx, guest + G_IM_HEIGHT, (UWORD)im->Height);
    m68k_write16(ctx, guest + G_IM_DEPTH, (UWORD)im->Depth);
    m68k_write32(ctx, guest + G_IM_IMAGEDATA, 0);
    ctx->mem[guest + G_IM_PLANEPICK] = im->PlanePick;
    ctx->mem[guest + G_IM_PLANEONOFF] = im->PlaneOnOff;
    m68k_write32(ctx, guest + G_IM_NEXTIMAGE, 0);
    return guest;
}

struct TextAttr *Kaffeine_MirrorNativeFont(struct KaffeineContext *ctx, struct KaffeineMirror *m,
                                          ULONG guest, BOOL *nomem)
{
    struct KaffeineMirrorFont *f;
    ULONG name;

    if (!guest_range(ctx, guest, G_TEXTATTR_SIZE))
        return NULL;
    name = m68k_read32(ctx, guest + G_TA_NAME);
    if (name && (!guest_range(ctx, name, 1) || !memchr(ctx->mem + name, 0, ctx->mem_size - name)))
        return NULL;
    for (f = m->fonts; f; f = f->next)
        if (f->guest == guest)
            break;
    if (!f)
    {
        f = AllocVec(sizeof(*f), MEMF_ANY | MEMF_CLEAR);
        if (!f)
        {
            *nomem = TRUE;
            return NULL;
        }
        f->guest = guest;
        f->next = m->fonts;
        m->fonts = f;
    }
    /* Again each time: the program may have changed its TextAttr. */
    f->attr.ta_Name = name ? (STRPTR)ctx->mem + name : NULL;
    f->attr.ta_YSize = m68k_read16(ctx, guest + G_TA_YSIZE);
    f->attr.ta_Style = ctx->mem[guest + G_TA_STYLE];
    f->attr.ta_Flags = ctx->mem[guest + G_TA_FLAGS];
    return &f->attr;
}

void Kaffeine_MirrorFree(struct KaffeineContext *ctx, struct KaffeineMirror *m)
{
    struct KaffeineMirrorFont *f;
    ULONG i;

    for (i = 0; i < m->count; i++)
        Kaffeine_MirrorRelease(ctx, m->nodes[i].guest);
    FreeVec(m->nodes);
    m->nodes = NULL;
    m->count = m->capacity = 0;
    while ((f = m->fonts))
    {
        m->fonts = f->next;
        FreeVec(f);
    }
}
