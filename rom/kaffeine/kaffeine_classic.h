/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Gadgets that a 68k program builds itself, without GadTools, on its
    windows: a native copy of each for Intuition, the Flags and the
    StringInfo fields that pass between the two, and the program's links
    and messages.
*/
#ifndef KAFFEINE_CLASSIC_H
#define KAFFEINE_CLASSIC_H

#include "kaffeine_intern.h"

struct Gadget;
struct Window;

#ifdef __cplusplus
extern "C" {
#endif

/* The program's gadget whose copy is the native gadget g, on a window or
   come off one, or 0 */
ULONG Kaffeine_ClassicGuest(struct KaffeineContext *ctx, const struct Gadget *g);
/* Whether the program's gadget at guest is on a window, as a copy */
BOOL Kaffeine_ClassicOn(struct KaffeineContext *ctx, ULONG guest);

/* For OpenWindow() and WA_Gadgets: the native head of the copies of the
   program's own list from guest, ready to go on the window, NULL for a
   list that stays off it, as one with gadgets that the bridge does not
   copy yet, and NULL with the program stopped for a list that the bridge
   refuses. Then Kaffeine_ClassicOpened() once the window opened with them,
   or Kaffeine_ClassicDropped() when it did not open. */
struct Gadget *Kaffeine_ClassicPrepare(struct KaffeineContext *ctx, ULONG guest, const char *call);
void Kaffeine_ClassicOpened(struct KaffeineContext *ctx, struct Window *w);
void Kaffeine_ClassicDropped(struct KaffeineContext *ctx);

/* AddGList() and AddGadget() of the program's own gadgets on the native
   window w, num of them from guest, or for a negative num as far as its
   68k list goes; one for AddGadget(). The UWORD position, (UWORD)~0 for a
   run that stays off the window, or the exit code of a program stopped. */
IPTR Kaffeine_ClassicAdd(struct KaffeineContext *ctx, struct Window *w, ULONG guest, UWORD position,
                        WORD num, BOOL one, const char *call);
/* RemoveGList() and RemoveGadget(): the program's own gadgets from guest
   come off w, num of them or the rest of the window's list. */
IPTR Kaffeine_ClassicRemove(struct KaffeineContext *ctx, struct Window *w, ULONG guest, WORD num,
                           const char *call);
/* Before RefreshGList() of num gadgets of w from the native g, and before
   ActivateGadget() of g with 1: what the program changed of the shared
   state of its gadgets there reaches them. */
void Kaffeine_ClassicRefreshing(struct KaffeineContext *ctx, struct Window *w, struct Gadget *g,
                               WORD num);
/* The native gadget of the program's gadget at guest on w, or NULL */
struct Gadget *Kaffeine_ClassicNative(struct KaffeineContext *ctx, struct Window *w, ULONG guest);
/* OnGadget() and OffGadget() of the program's own gadget at guest, with
   the native window w or NULL; FALSE for a gadget that is not one. On its
   window, as at RefreshGList(), with the state of every gadget of the
   program's own there, which Intuition draws after it. */
BOOL Kaffeine_ClassicEnable(struct KaffeineContext *ctx, ULONG guest, struct Window *w, BOOL off);
/* NewModifyProp() of num gadgets, or for all ModifyProp(), of the
   program's own proportional gadget at guest with values Flags,
   HorizPot, VertPot, HorizBody and VertBody, with w, a window of the
   program: natively on its copy, which Intuition draws into w, its own
   window or another, after which the program follows; for a gadget on
   no window into its PropInfo alone. FALSE for a gadget that is not
   one. */
BOOL Kaffeine_ClassicModifyProp(struct KaffeineContext *ctx, ULONG guest, struct Window *w,
                               const UWORD *values, WORD num, BOOL all);

/* The 68k NextGadget of the program's gadgets on w, as the native list
   has them now */
void Kaffeine_ClassicRelink(struct KaffeineContext *ctx, struct Window *w);
/* Before a message of w reaches the program: what Intuition changed of
   the gadgets on w and of the one at address goes to their 68k gadgets */
void Kaffeine_ClassicSync(struct KaffeineContext *ctx, struct Window *w, const void *address);
/* w is about to close: the program's gadgets come off it, and their
   copies go */
void Kaffeine_ClassicClosing(struct KaffeineContext *ctx, struct Window *w);
/* At the end of the program: the gadgets on windows that it left open
   come off them, and every copy goes */
void Kaffeine_ClassicCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_CLASSIC_H */
