/* Native-width recording collaborator; no native pointer is guest data. */
#pragma once
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <graphics/gfx.h>
struct ExtendedNode {
    struct Node *xln_Succ, *xln_Pred;
    UBYTE xln_Type; BYTE xln_Pri; char *xln_Name;
    UBYTE xln_Subsystem, xln_Subtype; APTR xln_Library;
    LONG (*xln_Init)();
};
struct MonitorSpec {
    struct ExtendedNode ms_Node; UWORD ms_Flags;
    LONG ratioh, ratiov;
    UWORD total_rows,total_colorclocks,DeniseMaxDisplayColumn,BeamCon0,min_row;
    APTR ms_Special; UWORD ms_OpenCount;
    LONG (*ms_transform)(),(*ms_translate)(),(*ms_scale)();
    UWORD ms_xoffset,ms_yoffset; struct Rectangle ms_LegalView;
    LONG (*ms_maxoscan)(),(*ms_videoscan)();
    UWORD DeniseMinDisplayColumn; ULONG DisplayCompatible;
    struct List DisplayInfoDataBase;
    UBYTE DisplayInfoDataBaseSemaphore[100];
    LONG (*ms_MrgCop)(),(*ms_LoadView)(),(*ms_KillView)();
};
