/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    68k mirrors of native Intuition objects that GadTools builds, such as
    menu strips and gadget lists: a table of native objects and their 68k
    copies in guest memory, and the copies of the strings, TextAttrs,
    IntuiTexts and images that they point to.
*/
#ifndef KAFFEINE_MIRROR_H
#define KAFFEINE_MIRROR_H

#include "kaffeine_intern.h"

struct TextAttr;
struct IntuiText;
struct Image;

/* The kinds of the shared helpers; a user of the table numbers its own
   kinds below these. */
enum
{
    KAFFEINE_MIRROR_STRING = 0x80,
    KAFFEINE_MIRROR_FONT,
    KAFFEINE_MIRROR_TEXT,
    KAFFEINE_MIRROR_IMAGE
};

struct KaffeineMirrorNode
{
    void *native;
    ULONG guest, size;
    UBYTE kind;
};

struct KaffeineMirrorFont;

struct KaffeineMirror
{
    struct KaffeineMirrorNode *nodes;
    ULONG count, capacity;
    BOOL failed;                        /* a copy without guest memory */
    struct KaffeineMirrorFont *fonts;    /* native copies of 68k TextAttrs */
};

#ifdef __cplusplus
extern "C" {
#endif

/* The node of a native object, or NULL. */
struct KaffeineMirrorNode *Kaffeine_MirrorFind(struct KaffeineMirror *m, const void *native,
                                             UBYTE kind);
/* The node of a guest address, or NULL. */
struct KaffeineMirrorNode *Kaffeine_MirrorGuest(struct KaffeineMirror *m, ULONG guest, UBYTE kind);
/* The 68k copy of a native object: the one it has, or a new cleared one,
   for which *fresh (if given) is TRUE; 0 without memory, which marks the
   mirror as failed. */
ULONG Kaffeine_MirrorNode(struct KaffeineContext *ctx, struct KaffeineMirror *m, void *native,
                         UBYTE kind, ULONG size, BOOL *fresh);
/* A string as the program sees it: where it is when it lies in guest
   memory, else a guest copy. */
ULONG Kaffeine_MirrorString(struct KaffeineContext *ctx, struct KaffeineMirror *m, const char *str);
/* A TextAttr: the program's own for a native copy of it, else a 68k copy. */
ULONG Kaffeine_MirrorFont(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct TextAttr *ta);
/* A chain of IntuiTexts, at most 1024 long; 0 without guest memory for any
   of its copies, and then an old copy of the chain stays as it was. */
ULONG Kaffeine_MirrorTexts(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct IntuiText *it);
/* An image: its geometry, without image data or a next image */
ULONG Kaffeine_MirrorImage(struct KaffeineContext *ctx, struct KaffeineMirror *m, struct Image *im);
/* A native copy of a 68k TextAttr, kept with the mirror and filled in again
   each time; NULL for one that is not in guest memory, or without memory
   (*nomem TRUE). */
struct TextAttr *Kaffeine_MirrorNativeFont(struct KaffeineContext *ctx, struct KaffeineMirror *m,
                                          ULONG guest, BOOL *nomem);
/* Free every 68k copy and native font copy of the mirror. */
void Kaffeine_MirrorFree(struct KaffeineContext *ctx, struct KaffeineMirror *m);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_MIRROR_H */
