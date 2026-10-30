#pragma once
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxnodes.h>

struct ColorMap
{
    UBYTE Flags, Type;
    UWORD Count;
    UWORD *ColorTable;
    struct ViewPortExtra *cm_vpe;
    UWORD *LowColorBits;
    UBYTE TransparencyPlane, SpriteResolution, SpriteResDefault, AuxFlags;
    struct ViewPort *cm_vp;
    void *NormalDisplayInfo, *CoerceDisplayInfo;
    struct TagItem *cm_batch_items;
    ULONG VPModeID;
    struct PaletteExtra *PalExtra;
    UWORD SpriteBase_Even, SpriteBase_Odd, Bp_0_base, Bp_1_base;
};

/* ObtainBestPenA() tags and precisions, from compiler/include/graphics/view.h */
#define PRECISION_EXACT -1
#define PRECISION_IMAGE 0
#define PRECISION_ICON  16
#define PRECISION_GUI   32
#define OBP_Precision 0x84000000
#define OBP_FailIfBad 0x84000001

/* Field names, order and types from compiler/include/graphics/view.h. */
struct ViewPort
{
    struct ViewPort *Next;
    struct ColorMap *ColorMap;
    struct CopList *DspIns, *SprIns, *ClrIns;
    struct UCopList *UCopIns;
    WORD DWidth, DHeight, DxOffset, DyOffset;
    UWORD Modes;
    UBYTE SpritePriorities, ExtendedModes;
    struct RasInfo *RasInfo;
};

/* Native public layout from compiler/include/graphics/view.h. */
struct ViewPortExtra
{
    struct ExtendedNode n;
    struct ViewPort *ViewPort;
    struct Rectangle DisplayClip;
    APTR VecTable, DriverData[2];
    UWORD Flags;
    Point Origin[2];
    ULONG cop1ptr, cop2ptr;
};
