/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Native copies of 68k structures that native code reads itself, as
    Intuition reads a menu strip or a gadget that a program built:
    IntuiText chains with their TextAttrs, strings included, Image and
    Border chains and gadgets, checked against guest memory. Image data
    stays where it lies in guest memory,
    planar and big-endian as graphics takes it. A copy is made in two runs
    over the same 68k input. The first checks the input and counts the room
    that its copies take; the second takes that room in the same order from
    one block that the caller allocated with the count, and fills it in, so
    that a copy is made whole or not at all.
*/
#ifndef KAFFEINE_COPY_H
#define KAFFEINE_COPY_H

#include "kaffeine_intern.h"

struct Border;
struct Gadget;
struct IntuiText;
struct Image;
struct TextAttr;

struct KaffeineCopy
{
    UBYTE *block;       /* the block of the second run; NULL in the first */
    ULONG room, used;   /* the bytes of the block, and those taken or counted */
    ULONG objects;      /* the copies so far, against a limit */
    const char *why;    /* why the 68k input is refused, once it is */
    ULONG font_guest;   /* the TextAttr copied last, and its copy */
    struct TextAttr *font;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Room for a native object: counted in the first run, which gets NULL,
   and taken from the block in the second. NULL once why is set. */
APTR Kaffeine_CopyTake(struct KaffeineCopy *c, ULONG size);
/* The second run, on a block of the room that the first one counted */
void Kaffeine_CopyFill(struct KaffeineCopy *c, APTR block);
/* Refuse the 68k input, unless it already is. */
void Kaffeine_CopyRefuse(struct KaffeineCopy *c, const char *why);
/* A copy of a string; NULL for 0, or for one that does not end in guest
   memory or is 256 characters long or longer, which refuses the input. */
STRPTR Kaffeine_CopyString(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest);
/* An IntuiText chain, at most 1024 long, with its strings and TextAttrs;
   NULL in the first run, for 0 and once the input is refused. */
struct IntuiText *Kaffeine_CopyTexts(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest);
/* An Image chain, at most 1024 long, as Kaffeine_CopyTexts(). An image of
   a BOOPSI class, of a negative size or more than 8 planes, or whose
   planes that DrawImage() reads have more than 32767 words each or lie
   outside guest memory or at an odd address, refuses the input. */
struct Image *Kaffeine_CopyImages(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest);
/* A Border chain, at most 1024 long, as Kaffeine_CopyTexts(), with the XY
   pairs that DrawBorder() reads, Count of them, as native WORDs; pairs
   outside guest memory refuse the input. */
struct Border *Kaffeine_CopyBorders(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest);
/* One gadget, a Gadget or for GFLG_EXTENDED an ExtGadget, without its
   NextGadget: its GadgetRender and, for GFLG_GADGHIMAGE, which alone draws
   it, its SelectRender, as Image chains for GFLG_GADGIMAGE and as Border
   chains otherwise, its GadgetText as an IntuiText chain, a string or an
   Image chain as its GFLG_LABELMASK bits say, MutualExclude and UserData
   as the numbers they are, and of MoreFlags GMORE_BOUNDS and
   GMORE_SCROLLRASTER. A string gadget gets a StringInfo whose Buffer,
   UndoBuffer and, with a StringExtend, WorkBuffer stay in guest memory,
   MaxChars bytes each, with AltKeyMap NULL, and for GFLG_STRINGEXTEND or
   GACT_STRINGEXTEND a StringExtend with the native font and
   GFLG_STRINGEXTEND. A proportional gadget gets its PropInfo, and as its
   GadgetRender the Image of its knob whatever its flags: for AUTOKNOB the
   Image alone, whose place and size Intuition writes, without its data,
   else the program's Image chain, and without a knob an empty Image; no
   SelectRender. Other gadgets get no SpecialInfo. NULL in the first run
   and once the input is refused; a gadget outside guest memory, 0
   included, refuses it, and so does a string gadget whose StringInfo or
   StringExtend lies outside guest memory, whose MaxChars is below 1, whose
   buffers hold less than MaxChars bytes, whose StringExtend has an
   EditHook, or whose font is not a 68k TextFont of the bridge, and a
   proportional gadget whose PropInfo or knob lies outside guest memory.
   The text may hold its NUL past MaxChars, as on AmigaOS. */
struct Gadget *Kaffeine_CopyGadget(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_COPY_H */
