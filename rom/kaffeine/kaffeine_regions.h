/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Regions of 68k code: real native graphics Regions of the program, each
    with a stable classic 68k Region and bounded snapshots of its
    RegionRectangles, and the clip Regions of private layers. See
    kaffeine_regions.c.
*/
#ifndef KAFFEINE_REGIONS_H
#define KAFFEINE_REGIONS_H

#include "kaffeine_thunks.h"

struct Region;

/* Classic big-endian layouts of graphics/regions.h and graphics/gfx.h */
#define M68K_REGION_BOUNDS        0    /* struct Rectangle: MinX, MinY, MaxX, MaxY */
#define M68K_REGION_RECTANGLE     8    /* struct RegionRectangle * */
#define M68K_SIZEOF_REGION        12
#define M68K_RR_NEXT              0
#define M68K_RR_PREV              4
#define M68K_RR_BOUNDS            8
#define M68K_SIZEOF_RR            16
#define M68K_SIZEOF_RECTANGLE     8

/* Bounds: Regions of the program alive at once, and RegionRectangles that
   the snapshot of one Region takes. A Region whose native rectangles would
   exceed the snapshot is never published: the change that would make it
   fails as when memory runs out. */
#define KAFFEINE_REGION_MAX        16
#define KAFFEINE_REGION_MAX_RECTS  64

#ifdef __cplusplus
extern "C" {
#endif

/* graphics -510, -516, -534 */
IPTR Kaffeine_OrRectRegion(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_NewRegion(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DisposeRegion(struct KaffeineContext *ctx, void *cpu);
/* Every other graphics call that takes a Region: stops the program (20)
   before any native call */
IPTR kaffeine_regions_Unsupported(struct KaffeineContext *ctx, void *cpu);

/* For the layer bridge (kaffeine_layers.c), which installs Regions of the
   program as clip Regions of its private layers. Lease: the native Region
   of a live 68k Region of NewRegion(), which from now on can neither be
   changed nor disposed of until it is unleased as often; NULL after
   stopping the program for anything else. Unlease: one lease goes, after
   the native layer no longer uses it. Never frees anything. */
struct Region *Kaffeine_RegionLease(struct KaffeineContext *ctx, ULONG guest);
void Kaffeine_RegionUnlease(struct KaffeineContext *ctx, ULONG guest);
/* At the end of the program, after Kaffeine_LayersCleanup(): every Region
   that the program kept goes, its 68k Region and RegionRectangles cleared.
   One that a layer still uses is left to native memory, never freed under
   it. Safe to call again. */
void Kaffeine_RegionsCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

/* The Region calls of graphics.library (graphics.conf). Only NewRegion(),
   OrRectRegion() and DisposeRegion() are bridged; every other vector that
   takes a Region is refused here, so that no generated thunk ever reads a
   68k Region as a native one. */
#define KAFFEINE_GRAPHICS_REGION_THUNKS \
    {  504, kaffeine_regions_Unsupported },   /* AndRectRegion */ \
    {  510, Kaffeine_OrRectRegion }, \
    {  516, Kaffeine_NewRegion }, \
    {  522, kaffeine_regions_Unsupported },   /* ClearRectRegion */ \
    {  528, kaffeine_regions_Unsupported },   /* ClearRegion */ \
    {  534, Kaffeine_DisposeRegion }, \
    {  558, kaffeine_regions_Unsupported },   /* XorRectRegion */ \
    {  612, kaffeine_regions_Unsupported },   /* OrRegionRegion */ \
    {  618, kaffeine_regions_Unsupported },   /* XorRegionRegion */ \
    {  624, kaffeine_regions_Unsupported },   /* AndRegionRegion */ \
    { 1086, kaffeine_regions_Unsupported },   /* SetRegion */ \
    { 1092, kaffeine_regions_Unsupported },   /* ClearRegionRegion */ \
    { 1098, kaffeine_regions_Unsupported },   /* AreRegionsEqual */ \
    { 1104, kaffeine_regions_Unsupported },   /* IsPointInRegion */ \
    { 1110, kaffeine_regions_Unsupported },   /* ScrollRegion */ \
    { 1116, kaffeine_regions_Unsupported },   /* AndRectRegionND */ \
    { 1122, kaffeine_regions_Unsupported },   /* AndRegionRegionND */ \
    { 1128, kaffeine_regions_Unsupported },   /* OrRectRegionND */ \
    { 1134, kaffeine_regions_Unsupported },   /* OrRegionRegionND */ \
    { 1140, kaffeine_regions_Unsupported },   /* XorRectRegionND */ \
    { 1146, kaffeine_regions_Unsupported },   /* XorRegionRegionND */ \
    { 1152, kaffeine_regions_Unsupported },   /* ClearRectRegionND */ \
    { 1158, kaffeine_regions_Unsupported }    /* ClearRegionRegionND */

#endif /* KAFFEINE_REGIONS_H */
