#pragma once
#include <exec/types.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/displayinfo.h>

#ifdef __cplusplus
extern "C" {
#endif

ULONG GetDisplayInfoData(DisplayInfoHandle handle, UBYTE *buf, ULONG size, ULONG tag, ULONG id);
struct ViewPort;
struct ColorMap;
void LoadRGB4(struct ViewPort *, UWORD *, WORD);
void LoadRGB32(struct ViewPort *, const ULONG *);
void GetRGB32(struct ColorMap *, ULONG, ULONG, ULONG *);
ULONG GetRGB4(struct ColorMap *, LONG);
void SetRGB4(struct ViewPort *, WORD, UBYTE, UBYTE, UBYTE);
void SetRGB32(struct ViewPort *, ULONG, ULONG, ULONG, ULONG);
void SetRGB4CM(struct ColorMap *, WORD, UBYTE, UBYTE, UBYTE);
void SetRGB32CM(struct ColorMap *, ULONG, ULONG, ULONG, ULONG);
struct TextFont *OpenFont(const struct TextAttr *textAttr);
void CloseFont(struct TextFont *textFont);
void SetFont(struct RastPort *rp, struct TextFont *textFont);
void AskFont(struct RastPort *rp, struct TextAttr *textAttr);
void InitRastPort(struct RastPort *rp);
WORD TextLength(struct RastPort *rp, CONST_STRPTR string, ULONG count);
void Text(struct RastPort *rp, CONST_STRPTR string, ULONG count);
void TextExtent(struct RastPort *rp, CONST_STRPTR string, ULONG count,
                struct TextExtent *textExtent);
ULONG TextFit(struct RastPort *rp, CONST_STRPTR string, ULONG strLen,
              struct TextExtent *textExtent, struct TextExtent *constrainingExtent,
              LONG strDirection, ULONG constrainingBitWidth, ULONG constrainingBitHeight);
void Move(struct RastPort *rp, WORD x, WORD y);
void Draw(struct RastPort *rp, WORD x, WORD y);
void RectFill(struct RastPort *rp, WORD xMin, WORD yMin, WORD xMax, WORD yMax);
LONG ReadPixel(struct RastPort *rp, WORD x, WORD y);
LONG WritePixel(struct RastPort *rp, WORD x, WORD y);
void SetAPen(struct RastPort *rp, ULONG pen);
void SetBPen(struct RastPort *rp, ULONG pen);
void SetDrMd(struct RastPort *rp, ULONG drawMode);
void SetABPenDrMd(struct RastPort *rp, ULONG apen, ULONG bpen, ULONG drawMode);
ULONG GetAPen(struct RastPort *rp);
ULONG GetBPen(struct RastPort *rp);
ULONG GetDrMd(struct RastPort *rp);
ULONG GetOutlinePen(struct RastPort *rp);
void ScrollRaster(struct RastPort *rp, WORD dx, WORD dy,
                  WORD xMin, WORD yMin, WORD xMax, WORD yMax);
struct Layer;
BOOL AttemptLockLayerRom(struct Layer *layer);
struct BitMap;
void InitBitMap(struct BitMap *bm, BYTE depth, UWORD width, UWORD height);
/* The native Region calls of the Region bridge (graphics.conf) */
struct Region;
struct Rectangle;
struct Region *NewRegion(void);
void DisposeRegion(struct Region *region);
BOOL OrRectRegion(struct Region *region, struct Rectangle *rect);
BOOL OrRegionRegion(struct Region *r1, struct Region *r2);

LONG BltBitMap(struct BitMap *srcBitMap, WORD xSrc, WORD ySrc, struct BitMap *destBitMap,
               WORD xDest, WORD yDest, WORD xSize, WORD ySize, UBYTE minterm, UBYTE mask,
               UBYTE *tempA);
void WaitBlit(void);
IPTR GetBitMapAttr(struct BitMap *bitmap, ULONG attribute);

#ifdef __cplusplus
}
#endif
