/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Layers of 68k code: bounded classic snapshots of the native layer
    topology of the screens that the program uses, with stable guest
    identities, and the few layer calls that the bridge can take safely.
    See kaffeine_layers.c.
*/
#ifndef KAFFEINE_LAYERS_H
#define KAFFEINE_LAYERS_H

#include "kaffeine_thunks.h"

struct Window;
struct Screen;

/* Classic big-endian offsets of the NDK structures (graphics/clip.h,
   graphics/layers.h, utility/hooks.h, intuition/intuition.h and
   intuition/screens.h). kaffeine_shadow_layouts.h lists the same offsets;
   test_layers checks them against it. */
#define M68K_LAYER_FRONT          0
#define M68K_LAYER_BACK           4
#define M68K_LAYER_CLIPRECT       8
#define M68K_LAYER_RP             12
#define M68K_LAYER_MINX           16
#define M68K_LAYER_MINY           18
#define M68K_LAYER_MAXX           20
#define M68K_LAYER_MAXY           22
#define M68K_LAYER_RESERVED       24
#define M68K_LAYER_PRIORITY       28
#define M68K_LAYER_FLAGS          30
#define M68K_LAYER_SUPERBITMAP    32
#define M68K_LAYER_SUPERCLIPRECT  36
#define M68K_LAYER_WINDOW         40
#define M68K_LAYER_SCROLL_X       44
#define M68K_LAYER_SCROLL_Y       46
#define M68K_LAYER_CR             48   /* cr, cr2, crnew, SuperSaveClipRects, _cliprects */
#define M68K_LAYER_LAYERINFO      68
#define M68K_LAYER_LOCK           72   /* struct SignalSemaphore, 46 bytes */
#define M68K_LAYER_BACKFILL       118
#define M68K_LAYER_RESERVED1      122
#define M68K_LAYER_CLIPREGION     126
#define M68K_LAYER_SAVECLIPRECTS  130
#define M68K_LAYER_WIDTH          134
#define M68K_LAYER_HEIGHT         136
#define M68K_LAYER_DAMAGELIST     156
#define M68K_SIZEOF_LAYER         160

#define M68K_LI_TOP_LAYER         0
#define M68K_LI_LOCK              24
#define M68K_LI_FLAGS             88
#define M68K_SIZEOF_LAYER_INFO    102

#define M68K_SIZEOF_HOOK          20

/* SignalSemaphore and MinList inside it */
#define M68K_SS_LN_TYPE           8
#define M68K_SS_NESTCOUNT         14
#define M68K_SS_WAITQUEUE         16
#define M68K_SS_OWNER             40
#define M68K_SS_QUEUECOUNT        44

#define M68K_WIN_WLAYER           124
#define M68K_SCR_LAYERINFO        224
#define M68K_SCR_BARLAYER         334
#define M68K_RP_LAYER             0

/* Bounds: 68k Layers alive at once, layers of one screen that a snapshot
   takes, screens with snapshots, opaque handles of native backfill hooks,
   nested program locks of one layer, and LayerInfos of NewLayerInfo()
   alive at once. */
#define KAFFEINE_LAYER_MAX_RECORDS 64
#define KAFFEINE_LAYER_MAX_DEPTH   48
#define KAFFEINE_LAYER_MAX_GRAPHS  16
#define KAFFEINE_LAYER_MAX_HOOKS   32
#define KAFFEINE_LAYER_MAX_LOCKS   255
#define KAFFEINE_LAYER_MAX_INFOS   16

/* Canonical classic semaphore, initialized exactly once by the owner of
   its containing guest object, independently of native snapshot timing. */
static inline void Kaffeine_LayerSemaphoreInit(struct KaffeineContext *ctx, ULONG ss)
{
    ULONG queue = ss + M68K_SS_WAITQUEUE;

    ctx->mem[ss + M68K_SS_LN_TYPE] = 15; /* NT_SIGNALSEM */
    m68k_write16(ctx, ss + M68K_SS_NESTCOUNT, 0);
    m68k_write32(ctx, queue, queue + 4);
    m68k_write32(ctx, queue + 4, 0);
    m68k_write32(ctx, queue + 8, queue);
    m68k_write32(ctx, ss + M68K_SS_OWNER, 0);
    m68k_write16(ctx, ss + M68K_SS_QUEUECOUNT, 0xFFFF);
}

/* What the window and screen bridges tell the layer bridge, through
   ctx->layer_ops, so that they link without it. All are optional for the
   callers: ctx->layer_ops is NULL without the module. */
struct KaffeineLayerOps
{
    /* A 68k Window has just been made for w (Kaffeine_WindowCreate()):
       its WLayer and the Layer of its RastPort get the 68k Layer of the
       native one. FALSE without memory or room, and then nothing stays. */
    BOOL  (*window_opened)(struct KaffeineContext *ctx, ULONG guest, struct Window *w);
    /* Before the native window closes (Kaffeine_WindowDelete()): the
       program's locks of its layer go back and its 68k Layer goes. */
    void  (*window_closing)(struct KaffeineContext *ctx, ULONG guest, struct Window *w);
    /* The Flags that Kaffeine_WindowSync() writes: the native ones, with
       a WFLG_BACKDROP that the program changed and no depth call has used
       yet left as the program wrote it. */
    ULONG (*window_flags)(struct KaffeineContext *ctx, ULONG guest, ULONG native_flags);
    /* After Kaffeine_WindowSync() for a message of the window that is no
       VERIFY message: the snapshot of its screen follows the native layers. */
    void  (*window_synced)(struct KaffeineContext *ctx, ULONG guest, struct Window *w);
    /* A 68k Screen of the program now stands for scr (OpenScreen(),
       LockPubScreen()): its snapshot is made or brought up to date. */
    void  (*screen_mirrored)(struct KaffeineContext *ctx, struct Screen *scr);
    /* Before scr may close or its 68k Screen goes; NULL for every screen,
       at the end of the program. Touches nothing native. */
    void  (*screen_closing)(struct KaffeineContext *ctx, struct Screen *scr);
};

#ifdef __cplusplus
extern "C" {
#endif

/* Install the layer bridge on a context: ctx->layers, ctx->layer_ops and
   ctx->gui_poll. FALSE without memory, and then programs see no layers. */
BOOL Kaffeine_LayersSetup(struct KaffeineContext *ctx);
/* Bring every snapshot up to date, as the periodic poll does. */
void Kaffeine_LayersRefresh(struct KaffeineContext *ctx);
/* At the end of the program, after the windows and screens: the program's
   layer locks go back, every 68k Layer, LayerInfo and hook handle goes,
   each LayerInfo of NewLayerInfo() that the program kept is disposed of
   once, and the bridge leaves the context. Safe to call again. */
void Kaffeine_LayersCleanup(struct KaffeineContext *ctx);

/* intuition -306, -312 */
IPTR Kaffeine_WindowToBack(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_WindowToFront(struct KaffeineContext *ctx, void *cpu);
/* layers -96, -102, -144, -150, -198 */
IPTR Kaffeine_LockLayer(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_UnlockLayer(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_NewLayerInfo(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DisposeLayerInfo(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_InstallLayerHook(struct KaffeineContext *ctx, void *cpu);
/* graphics -432, -438, -654 */
IPTR Kaffeine_LockLayerRom(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_UnlockLayerRom(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AttemptLockLayerRom(struct KaffeineContext *ctx, void *cpu);
/* Every other layer call: stops the program (20) before any native call */
IPTR kaffeine_layers_Unsupported(struct KaffeineContext *ctx, void *cpu);

extern const struct M68KThunkEntry kaffeine_thunks_layers_manual[];
extern const ULONG kaffeine_thunks_layers_manual_count;

#ifdef __cplusplus
}
#endif

/* Shared by the production manual tables and the host tests. */
#define KAFFEINE_INTUITION_LAYER_THUNKS \
    { 306, Kaffeine_WindowToBack }, \
    { 312, Kaffeine_WindowToFront }

/* Graphics calls that take a Layer: the generated thunks would hand guest
   memory to native code as a native Layer. */
#define KAFFEINE_GRAPHICS_LAYER_THUNKS \
    { 432, Kaffeine_LockLayerRom }, \
    { 438, Kaffeine_UnlockLayerRom }, \
    { 444, kaffeine_layers_Unsupported },     /* SyncSBitMap */ \
    { 450, kaffeine_layers_Unsupported },     /* CopySBitMap */ \
    { 654, Kaffeine_AttemptLockLayerRom }

/* Every layers.library LVO of the generated table, so that no generated
   layer thunk is ever reached: none of them can tell a 68k Layer, Region,
   BitMap or Hook from a native one, and several would run guest Hooks. */
#define KAFFEINE_LAYERS_THUNKS \
    {  30, kaffeine_layers_Unsupported },     /* InitLayers */ \
    {  36, kaffeine_layers_Unsupported },     /* CreateUpfrontLayer */ \
    {  42, kaffeine_layers_Unsupported },     /* CreateBehindLayer */ \
    {  48, kaffeine_layers_Unsupported },     /* UpfrontLayer */ \
    {  54, kaffeine_layers_Unsupported },     /* BehindLayer */ \
    {  60, kaffeine_layers_Unsupported },     /* MoveLayer */ \
    {  66, kaffeine_layers_Unsupported },     /* SizeLayer */ \
    {  72, kaffeine_layers_Unsupported },     /* ScrollLayer */ \
    {  78, kaffeine_layers_Unsupported },     /* BeginUpdate */ \
    {  84, kaffeine_layers_Unsupported },     /* EndUpdate */ \
    {  90, kaffeine_layers_Unsupported },     /* DeleteLayer */ \
    {  96, Kaffeine_LockLayer }, \
    { 102, Kaffeine_UnlockLayer }, \
    { 108, kaffeine_layers_Unsupported },     /* LockLayers */ \
    { 114, kaffeine_layers_Unsupported },     /* UnlockLayers */ \
    { 120, kaffeine_layers_Unsupported },     /* LockLayerInfo */ \
    { 126, kaffeine_layers_Unsupported },     /* SwapBitsRastPortClipRect */ \
    { 132, kaffeine_layers_Unsupported },     /* WhichLayer */ \
    { 138, kaffeine_layers_Unsupported },     /* UnlockLayerInfo */ \
    { 144, Kaffeine_NewLayerInfo }, \
    { 150, Kaffeine_DisposeLayerInfo }, \
    { 156, kaffeine_layers_Unsupported },     /* FattenLayerInfo */ \
    { 162, kaffeine_layers_Unsupported },     /* ThinLayerInfo */ \
    { 168, kaffeine_layers_Unsupported },     /* MoveLayerInFrontOf */ \
    { 174, kaffeine_layers_Unsupported },     /* InstallClipRegion */ \
    { 180, kaffeine_layers_Unsupported },     /* MoveSizeLayer */ \
    { 186, kaffeine_layers_Unsupported },     /* CreateUpfrontHookLayer */ \
    { 192, kaffeine_layers_Unsupported },     /* CreateBehindHookLayer */ \
    { 198, Kaffeine_InstallLayerHook }, \
    { 204, kaffeine_layers_Unsupported },     /* InstallLayerInfoHook */ \
    { 210, kaffeine_layers_Unsupported },     /* SortLayerCR */ \
    { 216, kaffeine_layers_Unsupported },     /* DoHookClipRects */ \
    { 222, kaffeine_layers_Unsupported },     /* ChangeLayerShape */ \
    { 228, kaffeine_layers_Unsupported },     /* ScaleLayer */ \
    { 234, kaffeine_layers_Unsupported },     /* CreateUpfrontLayerTagList */ \
    { 240, kaffeine_layers_Unsupported },     /* CreateBehindLayerTagList */ \
    { 246, kaffeine_layers_Unsupported },     /* ChangeLayerVisibility */ \
    { 258, kaffeine_layers_Unsupported },     /* IsLayerVisible */ \
    { 264, kaffeine_layers_Unsupported },     /* IsLayerHiddenBySibling */ \
    { 270, kaffeine_layers_Unsupported }      /* CollectPixelsLayer */

#endif /* KAFFEINE_LAYERS_H */
