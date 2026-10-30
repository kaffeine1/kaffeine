/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Windows of 68k code: their screens, their fields, their IDCMP ports and
    the IDCMP messages that come through them.
*/
#ifndef KAFFEINE_WINDOW_H
#define KAFFEINE_WINDOW_H

#include "kaffeine_thunks.h"

struct Gadget;
struct Window;
struct MsgPort;
struct TagItem;

#ifdef __cplusplus
extern "C" {
#endif

/* The gadgets of a new window, FirstGadget or WA_Gadgets: a GadTools list
   of the program as its native head, checked by the gadget bridge, in
   *native; NULL for none, and for gadgets that the program made itself,
   which stay out of the window, as before. FALSE when the program stops. */
BOOL Kaffeine_WindowGadgets(struct KaffeineContext *ctx, ULONG gadgets, const char *call,
                           struct Gadget **native);
/* The tags of OpenWindowTagList() as one flat native list, through
   TAG_MORE, TAG_SKIP and TAG_IGNORE: WA_Title, WA_ScreenTitle and
   WA_PubScreenName in guest memory, WA_CustomScreen and WA_PubScreen as
   the native screen of a screen of the program, NULL as NULL, the last
   WA_Gadgets through Kaffeine_WindowGadgets(), its native list also in
   *gadgets, the others as they are. *size is the size for FreeMem(),
   *screen the 68k screen of the last WA_CustomScreen or WA_PubScreen, or
   0. NULL without memory, or when the program stops: for a screen tag
   with anything but a screen of the program, for a gadget list that the
   bridge refuses, and for a list outside guest memory. */
struct TagItem *Kaffeine_WindowTags(struct KaffeineContext *ctx, ULONG list, ULONG *size,
                                   ULONG *screen, struct Gadget **gadgets);

/* Copy the fields of a native window that change while it is open to its
   68k Window: place, size, mouse, limits, flags, borders, IDCMP flags, pens
   and the GZZ fields. */
void Kaffeine_WindowSync(struct KaffeineContext *ctx, ULONG guest, struct Window *w);

/* A cleared 68k Window for a native one: its fields, its RastPort as a 68k
   RastPort, its IDCMP port as a 68k MsgPort, its title when it lies in guest
   memory, and WScreen for a screen of the program, as Kaffeine_ScreenGuest()
   gives it. Its RastPort shares the typed screen BitMap handle when the
   live owned screen and the native bitmap agree. 0 without memory, also
   for the RastPort or the port. */
ULONG Kaffeine_WindowCreate(struct KaffeineContext *ctx, struct Window *w);
/* Free the 68k Window of a native window, with its RastPort, its IDCMP port
   and its 68k messages; the messages that the program has not replied go
   back to Intuition and GadTools first, and the 68k Screen of a public
   screen that the program no longer holds goes after. Call it while the
   native window is still open. */
void Kaffeine_WindowDelete(struct KaffeineContext *ctx, ULONG guest);
/* CloseWindow and FreeSysRequest release layer locks before requester and
   menu/gadget teardown, then delete without repeating the layer callback. */
void Kaffeine_WindowPrepareClose(struct KaffeineContext *ctx, ULONG guest);
void Kaffeine_WindowDeletePrepared(struct KaffeineContext *ctx, ULONG guest);

/* In the task that ran the guest, before screen and I/O cleanup. */
void Kaffeine_WindowsCleanup(struct KaffeineContext *ctx);

/* The 68k MsgPort that stands for the IDCMP port of a window: signalling,
   with the signal bit of the native port and the current guest task, and an
   empty list. 0 without memory. */
ULONG Kaffeine_UserPortCreate(struct KaffeineContext *ctx, struct MsgPort *port);
void Kaffeine_UserPortDelete(struct KaffeineContext *ctx, ULONG guest);

/* ModifyIDCMP can borrow a complete ordinary PA_SIGNAL guest port, including
   a manually allocated one. The owner must be live and own its signal. A
   private native inbox signals the fixed driver; guest addresses never enter
   Intuition. Several windows may borrow it. Get/Wait/GT_GetIMsg serve the
   bounded ordinary guest FIFO (and Workbench copies) before native IDCMP.
   Closing/disabling a window retires only that window's messages; neither
   its caller-owned port nor its signal is freed. Optional Context hooks
   retire peers before port/signal/owner/storage reuse and final cleanup. */

IPTR Kaffeine_GetMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ReplyMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_WaitPort(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OpenWindow(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OpenWindowTagList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CloseWindow(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ModifyIDCMP(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_GetIMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_ReplyIMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_FilterIMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GT_PostFilterIMsg(struct KaffeineContext *ctx, void *cpu);

/* Requesters and EasyRequests of 68k code (kaffeine_requester.c) */
IPTR Kaffeine_ClearDMRequest(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_InitRequester(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_Request(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_EndRequest(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SetDMRequest(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_EasyRequestArgs(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_BuildEasyRequestArgs(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SysReqHandler(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeSysRequest(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual tables and the host dispatch tests. */
#define KAFFEINE_EXEC_IDCMP_THUNKS \
    { 372, Kaffeine_GetMsg }, \
    { 378, Kaffeine_ReplyMsg }, \
    { 384, Kaffeine_WaitPort }

#define KAFFEINE_INTUITION_WINDOW_THUNKS \
    { 72,  Kaffeine_CloseWindow }, \
    { 150, Kaffeine_ModifyIDCMP }, \
    { 204, Kaffeine_OpenWindow }, \
    { 606, Kaffeine_OpenWindowTagList }

#define KAFFEINE_INTUITION_REQUEST_THUNKS \
    { 48,  Kaffeine_ClearDMRequest }, \
    { 120, Kaffeine_EndRequest }, \
    { 138, Kaffeine_InitRequester }, \
    { 240, Kaffeine_Request }, \
    { 258, Kaffeine_SetDMRequest }, \
    { 372, Kaffeine_FreeSysRequest }, \
    { 588, Kaffeine_EasyRequestArgs }, \
    { 594, Kaffeine_BuildEasyRequestArgs }, \
    { 600, Kaffeine_SysReqHandler }

#define KAFFEINE_GADTOOLS_IDCMP_THUNKS \
    { 72,  Kaffeine_GT_GetIMsg }, \
    { 78,  Kaffeine_GT_ReplyIMsg }, \
    { 102, Kaffeine_GT_FilterIMsg }, \
    { 108, Kaffeine_GT_PostFilterIMsg }

#endif /* KAFFEINE_WINDOW_H */
