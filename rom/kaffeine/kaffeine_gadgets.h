/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    GadTools gadgets of 68k code: gadget lists made by CreateContext() and
    CreateGadgetA() with a 68k mirror of every gadget, FreeGadgets(), the
    attributes of GT_GetGadgetAttrsA() and GT_SetGadgetAttrsA(), the lists
    on windows, and the Intuition calls that would reach a GadTools gadget
    past them.
*/
#ifndef KAFFEINE_GADGETS_H
#define KAFFEINE_GADGETS_H

#include "kaffeine_thunks.h"

struct Gadget;
struct Window;
struct IntuiMessage;

#ifdef __cplusplus
extern "C" {
#endif

/* What windows do with the gadget lists of the program, GadTools ones and
   those that it built itself. The bridge sets ctx->window_gadgets as the
   program starts, and with the first GadTools batch for the host tests,
   so that windows need none of it without these. */
struct KaffeineWindowGadgets
{
    /* The native head of the 68k list of a window about to open, checked;
       NULL for a list that stays off the window, and NULL with the program
       stopped for a list that the bridge refuses */
    struct Gadget *(*prepare)(struct KaffeineContext *ctx, ULONG guest, const char *call);
    /* The window opened with that list, which is on it now */
    void (*opened)(struct KaffeineContext *ctx, struct Window *w, struct Gadget *head);
    /* The window did not open: what prepare made for it goes */
    void (*dropped)(struct KaffeineContext *ctx);
    /* The window is about to close: the program's gadgets come off it */
    void (*closing)(struct KaffeineContext *ctx, struct Window *w);
    /* A message of the window is about to reach the program: the mirrors
       of the gadgets on it follow what the input changed, and those of the
       gadget at address, the message's, also when it came off the window
       before GadTools filtered the message */
    void (*sync)(struct KaffeineContext *ctx, struct Window *w, APTR address);
    /* GadTools is about to filter that message of the window for the
       program */
    void (*filtering)(struct KaffeineContext *ctx, struct IntuiMessage *im);
    /* The program's gadget for the native one that a message names, or 0 */
    ULONG (*gadget)(struct KaffeineContext *ctx, const void *native);
};

IPTR Kaffeine_CreateGadgetA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeGadgets(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_SetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CreateGadgetContext(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_GetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DisposeObject(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ActivateGadget(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SetEditHook(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SetAttrsA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GetAttr(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SetGadgetAttrsA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DoGadgetMethodA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OnGadget(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ModifyProp(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_NewModifyProp(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OffGadget(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RefreshGadgets(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddGadget(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemoveGadget(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RefreshGList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddGList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemoveGList(struct KaffeineContext *ctx, void *cpu);
/* As the program starts: windows take its gadget lists through the bridge. */
void Kaffeine_GadgetsSetup(struct KaffeineContext *ctx);
/* Free the gadgets that the program left, at the end of the program. */
void Kaffeine_GadgetsCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host dispatch tests. */
#define KAFFEINE_GADTOOLS_GADGET_THUNKS \
    { 30,  Kaffeine_CreateGadgetA }, \
    { 36,  Kaffeine_FreeGadgets }, \
    { 42,  Kaffeine_GT_SetGadgetAttrsA }, \
    { 114, Kaffeine_CreateGadgetContext }, \
    { 174, Kaffeine_GT_GetGadgetAttrsA }

#define KAFFEINE_INTUITION_GADGET_THUNKS \
    { 42,  Kaffeine_AddGadget }, \
    { 156, Kaffeine_ModifyProp }, \
    { 174, Kaffeine_OffGadget }, \
    { 186, Kaffeine_OnGadget }, \
    { 222, Kaffeine_RefreshGadgets }, \
    { 228, Kaffeine_RemoveGadget }, \
    { 432, Kaffeine_RefreshGList }, \
    { 438, Kaffeine_AddGList }, \
    { 444, Kaffeine_RemoveGList }, \
    { 462, Kaffeine_ActivateGadget }, \
    { 468, Kaffeine_NewModifyProp }, \
    { 492, Kaffeine_SetEditHook }, \
    { 642, Kaffeine_DisposeObject }, \
    { 648, Kaffeine_SetAttrsA }, \
    { 654, Kaffeine_GetAttr }, \
    { 660, Kaffeine_SetGadgetAttrsA }, \
    { 810, Kaffeine_DoGadgetMethodA }

#endif /* KAFFEINE_GADGETS_H */
