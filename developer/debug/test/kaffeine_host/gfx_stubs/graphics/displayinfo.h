/* Minimal native-width display metadata collaborator. */
#pragma once
#include <exec/types.h>
#include <graphics/gfx.h>
typedef APTR DisplayInfoHandle;
typedef struct { WORD x, y; } Point;
struct QueryHeader { ULONG StructID, DisplayID, SkipID, Length; };
struct DisplayInfo {
    struct QueryHeader Header;
    UWORD NotAvailable; ULONG PropertyFlags; Point Resolution;
    UWORD PixelSpeed, NumStdSprites, PaletteRange; Point SpriteResolution;
    UBYTE pad[4], RedBits, GreenBits, BlueBits, pad2[5]; IPTR reserved[2];
};
struct DimensionInfo {
    struct QueryHeader Header;
    UWORD MaxDepth, MinRasterWidth, MinRasterHeight, MaxRasterWidth, MaxRasterHeight;
    struct Rectangle Nominal, MaxOScan, VideoOScan, TxtOScan, StdOScan;
    UBYTE pad[14]; IPTR reserved[2];
};
struct NameInfo { struct QueryHeader Header; UBYTE Name[32]; IPTR reserved[2]; };
#define DTAG_DISP 0x80000000U
#define DTAG_DIMS 0x80001000U
#define DTAG_MNTR 0x80002000U
#define DTAG_NAME 0x80003000U
#define DTAG_VEC 0x80004000U
