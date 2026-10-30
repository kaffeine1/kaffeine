/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Intuition and graphics manual thunks and RastPort resolution helper.
*/
#ifndef KAFFEINE_THUNKS_GFX_H
#define KAFFEINE_THUNKS_GFX_H

#include "kaffeine_thunks.h"

struct RastPort;

#ifdef __cplusplus
extern "C" {
#endif

struct RastPort *resolve_rp(struct KaffeineContext *ctx, ULONG m68k_rp);

IPTR thunk_intuition_SetWindowPointerA(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_intuition_DrawImage(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_intuition_DrawBorder(struct KaffeineContext *ctx, void *cpu);

IPTR thunk_gfx_OpenFont(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_SetABPenDrMd(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_ScrollRaster(struct KaffeineContext *ctx, void *cpu);

IPTR thunk_gfx_CloseFont(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_SetFont(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_InitGels(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_GelsUnsupported(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_InitRastPort(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_InitTmpRas(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_AskFont(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_TextLength(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_Text(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_TextExtent(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_TextFit(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_Move(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_Draw(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_RectFill(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_ReadPixel(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_WritePixel(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_BltTemplate(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_SetAPen(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_SetBPen(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_SetDrMd(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_GetAPen(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_GetBPen(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_GetDrMd(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_GetOutlinePen(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_AllocRaster(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_gfx_FreeRaster(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

#define KAFFEINE_INTUITION_COMPAT_THUNKS \
    { 108, thunk_intuition_DrawBorder }, \
    { 114, thunk_intuition_DrawImage }, \
    { 816, thunk_intuition_SetWindowPointerA }

#define KAFFEINE_GRAPHICS_COMPAT_THUNKS \
    { 72,  thunk_gfx_OpenFont }, \
    { 894, thunk_gfx_SetABPenDrMd }, \
    { 396, thunk_gfx_ScrollRaster }

/* Calls on the 68k RastPort of a window or a screen, the state of and
   bounded drawing through a caller-owned classic one, fonts, and guest
   heap rasters */
#define KAFFEINE_GRAPHICS_RASTPORT_THUNKS \
    { 36,  thunk_gfx_BltTemplate }, \
    { 54,  thunk_gfx_TextLength }, \
    { 60,  thunk_gfx_Text }, \
    { 66,  thunk_gfx_SetFont }, \
    { 78,  thunk_gfx_CloseFont }, \
    { 96, thunk_gfx_GelsUnsupported }, \
    { 102, thunk_gfx_GelsUnsupported }, \
    { 108, thunk_gfx_GelsUnsupported }, \
    { 114, thunk_gfx_GelsUnsupported }, \
    { 120, thunk_gfx_InitGels }, \
    { 126, thunk_gfx_GelsUnsupported }, \
    { 132, thunk_gfx_GelsUnsupported }, \
    { 138, thunk_gfx_GelsUnsupported }, \
    { 144, thunk_gfx_GelsUnsupported }, \
    { 150, thunk_gfx_GelsUnsupported }, \
    { 156, thunk_gfx_GelsUnsupported }, \
    { 162, thunk_gfx_GelsUnsupported }, \
    { 168, thunk_gfx_GelsUnsupported }, \
    { 174, thunk_gfx_GelsUnsupported }, \
    { 600, thunk_gfx_GelsUnsupported }, \
    { 198, thunk_gfx_InitRastPort }, \
    { 240, thunk_gfx_Move }, \
    { 246, thunk_gfx_Draw }, \
    { 306, thunk_gfx_RectFill }, \
    { 318, thunk_gfx_ReadPixel }, \
    { 324, thunk_gfx_WritePixel }, \
    { 342, thunk_gfx_SetAPen }, \
    { 348, thunk_gfx_SetBPen }, \
    { 354, thunk_gfx_SetDrMd }, \
    { 468, thunk_gfx_InitTmpRas }, \
    { 474, thunk_gfx_AskFont }, \
    { 492, thunk_gfx_AllocRaster }, \
    { 498, thunk_gfx_FreeRaster }, \
    { 690, thunk_gfx_TextExtent }, \
    { 696, thunk_gfx_TextFit }, \
    { 858, thunk_gfx_GetAPen }, \
    { 864, thunk_gfx_GetBPen }, \
    { 870, thunk_gfx_GetDrMd }, \
    { 876, thunk_gfx_GetOutlinePen }

#endif /* KAFFEINE_THUNKS_GFX_H */
