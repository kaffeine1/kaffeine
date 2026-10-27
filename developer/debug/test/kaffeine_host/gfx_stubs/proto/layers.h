#pragma once
#include <exec/types.h>
/* The layers.library calls of the layer bridge, as layers.conf declares
   them; each test that links the bridge defines them. */
struct Layer;
struct Layer_Info;
struct Hook;
struct BitMap;
struct SignalSemaphore;
struct Region;
#ifdef __cplusplus
extern "C" {
#endif
void LockLayer(LONG dummy, struct Layer *layer);
void UnlockLayer(struct Layer *layer);
void LockLayerInfo(struct Layer_Info *li);
void UnlockLayerInfo(struct Layer_Info *li);
/* Exec collaborators used to take the LayerInfo lock without waiting. */
ULONG AttemptSemaphore(struct SignalSemaphore *lock);
void ReleaseSemaphore(struct SignalSemaphore *lock);
struct Hook *InstallLayerHook(struct Layer *layer, struct Hook *hook);
struct Layer_Info *NewLayerInfo(void);
void DisposeLayerInfo(struct Layer_Info *li);
struct Layer *CreateUpfrontHookLayer(struct Layer_Info *li, struct BitMap *bm, LONG x0, LONG y0,
                                     LONG x1, LONG y1, LONG flags, struct Hook *hook,
                                     struct BitMap *bm2);
LONG DeleteLayer(LONG dummy, struct Layer *layer);
struct Region *InstallClipRegion(struct Layer *layer, struct Region *region);
#ifdef __cplusplus
}
#endif
