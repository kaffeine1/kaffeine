#pragma once
#include <exec/types.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TextFont *OpenFont(const struct TextAttr *textAttr);
void CloseFont(struct TextFont *textFont);
void SetFont(struct RastPort *rp, struct TextFont *textFont);
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

#ifdef __cplusplus
}
#endif
