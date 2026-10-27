#pragma once
#include <exec/types.h>

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
