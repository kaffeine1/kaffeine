/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Picasso96API.library of 68k code: p96BestModeIDTagList() and
    p96WritePixelArray(), the two calls of version 2 that RTG programs such
    as AmiPixelShop make to find a true colour mode and to copy a chunky
    buffer of their own into a window, on the cybergraphics.library of AROS,
    which has no Picasso96API.library of its own. Every other known LVO, -30
    to -192, stops the program with 20 and says which call it was: no
    result of those calls could be faked without the program trusting it,
    and the bridge claims no more of Picasso96 than these two.

    The numbers of the 68k side are those of the Picasso96 SDK, as
    workbench/devs/diskimage/include/amigaos/libraries/Picasso96.h has them
    (RGBFTYPE, the P96BIDTAG tags, struct RenderInfo), with the offsets of
    clib/Picasso96API_protos.h and inline/Picasso96API_protos.h.
*/

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_p96.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks.h"
#include "kaffeine_thunks_gfx.h"

#include <aros/debug.h>
#include <exec/nodes.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#include <proto/graphics.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* struct RenderInfo: APTR Memory, WORD BytesPerRow, WORD pad, RGBFTYPE */
#define M68K_RI_MEMORY       0
#define M68K_RI_BYTESPERROW  4
#define M68K_RI_RGBFORMAT    8
#define M68K_SIZEOF_RI       12

/* RGBFTYPE */
#define RGBFB_CLUT       1
#define RGBFB_R8G8B8     2
#define RGBFB_B8G8R8     3
#define RGBFB_R5G6B5PC   4
#define RGBFB_R5G5B5PC   5
#define RGBFB_A8R8G8B8   6
#define RGBFB_A8B8G8R8   7
#define RGBFB_R8G8B8A8   8
#define RGBFB_B8G8R8A8   9
#define RGBFB_R5G6B5     10
#define RGBFB_R5G5B5     11
#define RGBFB_B5G6R5PC   12
#define RGBFB_B5G5R5PC   13

/* p96BestModeIDTagList() tags, P96BIDTAG_Dummy = TAG_USER + 96 */
#define P96BIDTAG_FormatsAllowed    (TAG_USER + 97)
#define P96BIDTAG_FormatsForbidden  (TAG_USER + 98)
#define P96BIDTAG_NominalWidth      (TAG_USER + 99)
#define P96BIDTAG_NominalHeight     (TAG_USER + 100)
#define P96BIDTAG_Depth             (TAG_USER + 101)
#define P96BIDTAG_VideoCompatible   (TAG_USER + 102)
#define P96BIDTAG_PabloIVCompatible (TAG_USER + 103)
#define P96BIDTAG_PalomaIVCompatible (TAG_USER + 104)

/* The size and depth that a list without them asks for: the defaults of
   BestCModeIDTagList() in workbench/libs/cgfx/bestcmodeidtaglist.c, which
   the Picasso96 autodoc leaves unsaid. */
#define P96_DEFAULT_WIDTH  800
#define P96_DEFAULT_HEIGHT 600
#define P96_DEFAULT_DEPTH  8

/* Items of a guest tag list that p96BestModeIDTagList() reads at most,
   the TAG_DONE included, before it takes the list for a cycle. */
#define P96_TAG_STEPS 64

/* Display IDs that the search for an eligible mode looks at, at most: far
   more than the drivers of AROS list, one per sync and pixel format. */
#define P96_MODE_STEPS 512

static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* The RGBFTYPE of a cybergraphics pixel format, -1 for one that Picasso96
   has no name for (BGR15 and BGR16). The 32-bit formats with an unused
   byte of AROS are those of Picasso96, whose alpha byte is unused too. */
static LONG rgbformat_of_pixfmt(ULONG pixfmt)
{
    switch (pixfmt)
    {
        case PIXFMT_LUT8:    return RGBFB_CLUT;
        case PIXFMT_RGB15:   return RGBFB_R5G5B5;
        case PIXFMT_RGB15PC: return RGBFB_R5G5B5PC;
        case PIXFMT_BGR15PC: return RGBFB_B5G5R5PC;
        case PIXFMT_RGB16:   return RGBFB_R5G6B5;
        case PIXFMT_RGB16PC: return RGBFB_R5G6B5PC;
        case PIXFMT_BGR16PC: return RGBFB_B5G6R5PC;
        case PIXFMT_RGB24:   return RGBFB_R8G8B8;
        case PIXFMT_BGR24:   return RGBFB_B8G8R8;
        case PIXFMT_ARGB32:  return RGBFB_A8R8G8B8;
        case PIXFMT_BGRA32:  return RGBFB_B8G8R8A8;
        case PIXFMT_RGBA32:  return RGBFB_R8G8B8A8;
        case PIXFMT_ABGR32:  return RGBFB_A8B8G8R8;
        case PIXFMT_0RGB32:  return RGBFB_A8R8G8B8;
        case PIXFMT_BGR032:  return RGBFB_B8G8R8A8;
        case PIXFMT_RGB032:  return RGBFB_R8G8B8A8;
        case PIXFMT_0BGR32:  return RGBFB_A8B8G8R8;
        default:             return -1;
    }
}

/* The RECTFMT and the bytes per pixel of a source buffer in rgbformat, for
   the 24 and 32-bit formats; FALSE for the others, CLUT and the 15 and
   16-bit ones among them, which this version does not copy. Both name the
   bytes in memory order, so the big-endian bytes of the guest buffer go to
   WritePixelArray() as they are. */
static BOOL rectfmt_of_rgbformat(ULONG rgbformat, UBYTE *rectfmt, ULONG *bpp)
{
    switch (rgbformat)
    {
        case RGBFB_R8G8B8:   *rectfmt = RECTFMT_RGB24;  *bpp = 3; return TRUE;
        case RGBFB_B8G8R8:   *rectfmt = RECTFMT_BGR24;  *bpp = 3; return TRUE;
        case RGBFB_A8R8G8B8: *rectfmt = RECTFMT_ARGB32; *bpp = 4; return TRUE;
        case RGBFB_A8B8G8R8: *rectfmt = RECTFMT_ABGR32; *bpp = 4; return TRUE;
        case RGBFB_R8G8B8A8: *rectfmt = RECTFMT_RGBA32; *bpp = 4; return TRUE;
        case RGBFB_B8G8R8A8: *rectfmt = RECTFMT_BGRA32; *bpp = 4; return TRUE;
        default:             return FALSE;
    }
}

/* TRUE when the cybergraphics mode id has a format of mask and, for a
   request of 32-bit storage, four bytes a pixel. */
static BOOL eligible(ULONG id, ULONG mask, BOOL four_bytes)
{
    LONG format;

    if (!IsCyberModeID(id))
        return FALSE;
    format = rgbformat_of_pixfmt(GetCyberIDAttr(CYBRIDATTR_PIXFMT, id));
    return format >= 0 && (mask & (1UL << format)) &&
           (!four_bytes || GetCyberIDAttr(CYBRIDATTR_BPPIX, id) == 4);
}

/* TRUE when BestModeIDA() itself would take id: with a MonitorID, its
   first pass can match that exact ID only, so its own filters decide
   (NotAvailable, DIPF_IS_HAM and the other special flags, 4 bits a gun,
   a monitor that is enabled or the default one). A later pass may give
   another ID of the same monitor, which does not count. */
static BOOL considered(ULONG id)
{
    struct TagItem probe[] = {
        { BIDTAG_MonitorID, id },
        { BIDTAG_NominalWidth, 1 },
        { BIDTAG_NominalHeight, 1 },
        { BIDTAG_Depth, 1 },
        { TAG_DONE, 0 }
    };

    return BestModeIDA(probe) == id;
}

/* The mode that BestModeIDA() would choose among the eligible ones: every
   display ID from NextDisplayInfo() with a size and depth of at least
   those asked for, ranked as bestmodeida.c ranks, a mode replacing the
   best so far only when its depth, width and height are all no larger
   and one is smaller, so the first of equal modes stays. The default
   monitor comes first in BestModeIDA(), not here, where the order of the
   display database decides between monitors. A database of more than
   P96_MODE_STEPS IDs gives INVALID_ID rather than part of a ranking. */
static ULONG eligible_mode(ULONG width, ULONG height, ULONG depth, ULONG mask, BOOL four_bytes)
{
    struct DimensionInfo dims;
    ULONG id = (ULONG)INVALID_ID, best = (ULONG)INVALID_ID, steps;
    UWORD best_width = 0, best_height = 0, best_depth = 0;

    for (steps = 0;; steps++)
    {
        UWORD w, h;

        id = NextDisplayInfo(id);
        if (id == (ULONG)INVALID_ID)
            return best;
        if (steps == P96_MODE_STEPS)
        {
            bug("[kaffeine] p96BestModeIDTagList(): more than %d display IDs\n",
                P96_MODE_STEPS);
            return (ULONG)INVALID_ID;
        }
        if (!IsCyberModeID(id) ||
            GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) <
                offsetof(struct DimensionInfo, MaxOScan))
            continue;
        w = dims.Nominal.MaxX - dims.Nominal.MinX + 1;
        h = dims.Nominal.MaxY - dims.Nominal.MinY + 1;
        if (w < width || h < height || dims.MaxDepth < depth || !eligible(id, mask, four_bytes))
            continue;
        if (best != (ULONG)INVALID_ID &&
            !(dims.MaxDepth <= best_depth && w <= best_width && h <= best_height &&
              (dims.MaxDepth < best_depth || w < best_width || h < best_height)))
            continue;
        if (!considered(id))
            continue;
        best = id;
        best_width = w;
        best_height = h;
        best_depth = dims.MaxDepth;
    }
}

/* -60: p96BestModeIDTagList(A0=tags) -> D0=ModeID or INVALID_ID. The tag
   list is read item by item as plain numbers; the generic tag list copy
   would take a NominalWidth of 640 for a guest pointer. Each item that is
   read must be word aligned and in the memory of the program, and a list
   that does not end within P96_TAG_STEPS items, or a TAG_SKIP whose count
   would wrap the address, stops the program; TAG_SKIP passes over its
   items without reading them, so they need not be readable. Unknown tags
   are ignored.

   The request is checked before cybergraphics.library sees it, which keeps
   sizes in UWORDs and depths in UBYTEs, and one that no mode can meet gives
   INVALID_ID while the program goes on: a width or height above 0xffff, a
   depth above 32 (no RGBFTYPE has more bits), no format left once
   FormatsForbidden is taken from FormatsAllowed (every format and none by
   default), or VideoCompatible, PabloIVCompatible or PalomaIVCompatible set
   (no mode of AROS is one). A size or depth not given is that of
   P96_DEFAULT_WIDTH, P96_DEFAULT_HEIGHT and P96_DEFAULT_DEPTH, and 0 counts
   as 1, since all of them are minimums. Depths 25 to 32 ask for 32-bit
   storage: 24 bits on four bytes a pixel, the MaxDepth that the drivers of
   AROS give their 4-byte modes.

   BestCModeIDTagList() chooses first, with the default monitor first, and
   its mode is the answer when its format is allowed. When it finds none,
   no mode has the size, depth and flags, so no eligible one has either.
   Otherwise eligible_mode() looks among the modes of the formats allowed.
   For depths 25 to 32 the mode may differ from the one of the first
   bridge, which asked for depth 32 before 24, when modes of both kinds
   have the same size; both have four bytes a pixel. */
static IPTR thunk_p96_BestModeIDTagList(struct KaffeineContext *ctx, void *cpu)
{
    struct TagItem tags[4];
    ULONG at = THUNK_A(0), allowed = 0xffffffffUL, forbidden = 0, steps, id;
    ULONG width = P96_DEFAULT_WIDTH, height = P96_DEFAULT_HEIGHT, depth = P96_DEFAULT_DEPTH;
    ULONG card[3] = { 0, 0, 0 };
    BOOL four_bytes;

    for (steps = 0; at; steps++)
    {
        ULONG tag, data;

        if (steps == P96_TAG_STEPS || (at & 1) || !Kaffeine_ProgramMemory(ctx, at, 8))
            return refuse(ctx, "p96BestModeIDTagList(): a tag list that is not in the "
                               "memory of the program or does not end");
        tag = m68k_read32(ctx, at);
        data = m68k_read32(ctx, at + 4);
        at += 8;
        switch (tag)
        {
            case TAG_DONE:
                at = 0;
                break;
            case TAG_MORE:
                at = data;
                break;
            case TAG_SKIP:
                if (data > (ctx->mem_size - at) / 8)
                    return refuse(ctx, "p96BestModeIDTagList(): a tag list that is not in the "
                                       "memory of the program or does not end");
                at += data * 8;
                break;
            case P96BIDTAG_NominalWidth:
                width = data;
                break;
            case P96BIDTAG_NominalHeight:
                height = data;
                break;
            case P96BIDTAG_Depth:
                depth = data;
                break;
            case P96BIDTAG_FormatsAllowed:
                allowed = data;
                break;
            case P96BIDTAG_FormatsForbidden:
                forbidden = data;
                break;
            case P96BIDTAG_VideoCompatible:
            case P96BIDTAG_PabloIVCompatible:
            case P96BIDTAG_PalomaIVCompatible:
                card[tag - P96BIDTAG_VideoCompatible] = data;
                break;
            default:    /* TAG_IGNORE and unknown tags */
                break;
        }
    }
    allowed &= ~forbidden;
    if (width > 0xffff || height > 0xffff || depth > 32 || !allowed ||
        card[0] || card[1] || card[2])
        return (IPTR)(ULONG)INVALID_ID;
    four_bytes = depth > 24;
    tags[0].ti_Tag = CYBRBIDTG_NominalWidth;
    tags[0].ti_Data = width ? width : 1;
    tags[1].ti_Tag = CYBRBIDTG_NominalHeight;
    tags[1].ti_Data = height ? height : 1;
    tags[2].ti_Tag = CYBRBIDTG_Depth;
    tags[2].ti_Data = four_bytes ? 24 : depth ? depth : 1;
    tags[3].ti_Tag = TAG_DONE;
    tags[3].ti_Data = 0;
    id = BestCModeIDTagList(tags);
    if (id == (ULONG)INVALID_ID || eligible(id, allowed, four_bytes))
        return (IPTR)id;
    return (IPTR)eligible_mode(tags[0].ti_Data, tags[1].ti_Data, tags[2].ti_Data, allowed,
                               four_bytes);
}

/* -102: p96WritePixelArray(A0=RenderInfo, D0=SrcX, D1=SrcY, A1=RastPort,
   D2=DestX, D3=DestY, D4=SizeX, D5=SizeY), the six numbers UWORDs whose
   upper words the caller may leave dirty. The RenderInfo and the RastPort
   of a window or a screen are checked first, and the format, even for an
   empty rectangle, which then reads no pixel. The source rows are checked
   whole in the memory of the program, and WritePixelArray() of
   cybergraphics.library gets the first source pixel, so that SrcX and
   SrcY are applied, and checked, here. Anything else stops the program. */
static IPTR thunk_p96_WritePixelArray(struct KaffeineContext *ctx, void *cpu)
{
    ULONG ri = THUNK_A(0), memory, rgbformat, bpp;
    ULONG srcx = (UWORD)THUNK_D(0), srcy = (UWORD)THUNK_D(1);
    ULONG destx = (UWORD)THUNK_D(2), desty = (UWORD)THUNK_D(3);
    ULONG width = (UWORD)THUNK_D(4), height = (UWORD)THUNK_D(5);
    struct RastPort *rp;
    uint64_t first, span;
    WORD bpr;
    UBYTE rectfmt;

    if (!ri || (ri & 1) || !Kaffeine_ProgramMemory(ctx, ri, M68K_SIZEOF_RI))
        return refuse(ctx, "p96WritePixelArray(): no RenderInfo in the memory of the program");
    rp = Kaffeine_RastPortIn(ctx, THUNK_A(1));
    if (!rp || !rp->BitMap)
        return refuse(ctx, "p96WritePixelArray(): not the RastPort of a window or a screen");
    memory = m68k_read32(ctx, ri + M68K_RI_MEMORY);
    bpr = (WORD)m68k_read16(ctx, ri + M68K_RI_BYTESPERROW);
    rgbformat = m68k_read32(ctx, ri + M68K_RI_RGBFORMAT);
    if (!rectfmt_of_rgbformat(rgbformat, &rectfmt, &bpp))
        return refuse(ctx, "p96WritePixelArray(): an RGBFormat that is not copied");
    if (!width || !height)
        return 0;
    if (destx + width > 32768 || desty + height > 32768)
        return refuse(ctx, "p96WritePixelArray(): a destination past the coordinates of a RastPort");
    if (bpr <= 0 || (uint64_t)bpr < ((uint64_t)srcx + width) * bpp)
        return refuse(ctx, "p96WritePixelArray(): BytesPerRow shorter than the source rows");
    first = (uint64_t)memory + (uint64_t)srcy * (uint64_t)bpr + (uint64_t)srcx * bpp;
    span = (uint64_t)(height - 1) * (uint64_t)bpr + (uint64_t)width * bpp;
    if (!memory || first + span > ctx->mem_size ||
        !Kaffeine_ProgramMemory(ctx, (ULONG)first, (ULONG)span))
        return refuse(ctx, "p96WritePixelArray(): source pixels outside the memory of the program");
    WritePixelArray(ctx->mem + first, 0, 0, (UWORD)bpr, rp, destx, desty, width, height, rectfmt);
    return 0;
}

/* The other LVOs of version 2: a halt that names the call. */
#define P96_UNSUPPORTED(name) \
    static IPTR thunk_p96_##name(struct KaffeineContext *ctx, void *cpu) \
    { \
        (void)cpu; \
        return refuse(ctx, "p96" #name "(): an unsupported Picasso96API.library call"); \
    }

P96_UNSUPPORTED(AllocBitMap)
P96_UNSUPPORTED(FreeBitMap)
P96_UNSUPPORTED(GetBitMapAttr)
P96_UNSUPPORTED(LockBitMap)
P96_UNSUPPORTED(UnlockBitMap)
P96_UNSUPPORTED(RequestModeIDTagList)
P96_UNSUPPORTED(AllocModeListTagList)
P96_UNSUPPORTED(FreeModeList)
P96_UNSUPPORTED(GetModeIDAttr)
P96_UNSUPPORTED(OpenScreenTagList)
P96_UNSUPPORTED(CloseScreen)
P96_UNSUPPORTED(ReadPixelArray)
P96_UNSUPPORTED(WritePixel)
P96_UNSUPPORTED(ReadPixel)
P96_UNSUPPORTED(RectFill)
P96_UNSUPPORTED(WriteTrueColorData)
P96_UNSUPPORTED(ReadTrueColorData)
P96_UNSUPPORTED(PIP_OpenTagList)
P96_UNSUPPORTED(PIP_Close)
P96_UNSUPPORTED(PIP_SetTagList)
P96_UNSUPPORTED(PIP_GetTagList)
P96_UNSUPPORTED(PIP_GetIMsg)
P96_UNSUPPORTED(PIP_ReplyIMsg)
P96_UNSUPPORTED(GetRTGDataTagList)
P96_UNSUPPORTED(GetBoardDataTagList)
P96_UNSUPPORTED(EncodeColor)

const struct M68KThunkEntry kaffeine_thunks_p96[] = {
    {  30, thunk_p96_AllocBitMap },
    {  36, thunk_p96_FreeBitMap },
    {  42, thunk_p96_GetBitMapAttr },
    {  48, thunk_p96_LockBitMap },
    {  54, thunk_p96_UnlockBitMap },
    {  60, thunk_p96_BestModeIDTagList },
    {  66, thunk_p96_RequestModeIDTagList },
    {  72, thunk_p96_AllocModeListTagList },
    {  78, thunk_p96_FreeModeList },
    {  84, thunk_p96_GetModeIDAttr },
    {  90, thunk_p96_OpenScreenTagList },
    {  96, thunk_p96_CloseScreen },
    { 102, thunk_p96_WritePixelArray },
    { 108, thunk_p96_ReadPixelArray },
    { 114, thunk_p96_WritePixel },
    { 120, thunk_p96_ReadPixel },
    { 126, thunk_p96_RectFill },
    { 132, thunk_p96_WriteTrueColorData },
    { 138, thunk_p96_ReadTrueColorData },
    { 144, thunk_p96_PIP_OpenTagList },
    { 150, thunk_p96_PIP_Close },
    { 156, thunk_p96_PIP_SetTagList },
    { 162, thunk_p96_PIP_GetTagList },
    { 168, thunk_p96_PIP_GetIMsg },
    { 174, thunk_p96_PIP_ReplyIMsg },
    { 180, thunk_p96_GetRTGDataTagList },
    { 186, thunk_p96_GetBoardDataTagList },
    { 192, thunk_p96_EncodeColor },
    { 0, NULL }
};
const ULONG kaffeine_thunks_p96_count =
    sizeof(kaffeine_thunks_p96) / sizeof(kaffeine_thunks_p96[0]) - 1;

ULONG Kaffeine_P96Open(struct KaffeineContext *ctx, ULONG version)
{
    ULONG base;

    if (version > KAFFEINE_P96_VERSION)
    {
        bug("[kaffeine] Picasso96API.library %lu: only version %d\n",
            (unsigned long)version, KAFFEINE_P96_VERSION);
        return 0;
    }
    if ((base = Kaffeine_FindLibBase(ctx, "Picasso96API.library")))
        return base;
    /* setup_base() makes a base that no OpenLibrary() finds when the
       library map is full, so the room is checked before anything is made */
    if (ctx->num_libs >= KAFFEINE_MAX_LIBS || ctx->num_libmap >= KAFFEINE_MAX_LIBMAP)
    {
        bug("[kaffeine] Picasso96API.library: no room for one more base\n");
        return 0;
    }
    base = Kaffeine_SetupLibBaseSized(ctx, ctx->num_libs, "Picasso96API.library",
                                      KAFFEINE_MAX_LVO, M68K_SIZEOF_LIBRARY,
                                      kaffeine_thunks_p96, kaffeine_thunks_p96_count,
                                      NULL, 0);
    if (!base)
    {
        bug("[kaffeine] Picasso96API.library: no room for the base\n");
        return 0;
    }
    memset(ctx->mem + base, 0, M68K_SIZEOF_LIBRARY);
    ctx->mem[base + M68K_LIB_LNTYPE] = NT_LIBRARY;
    m68k_write16(ctx, base + M68K_LIB_NEGSIZE, KAFFEINE_MAX_LVO * M68K_JT_SLOT_SIZE);
    m68k_write16(ctx, base + M68K_LIB_POSSIZE, M68K_SIZEOF_LIBRARY);
    m68k_write16(ctx, base + M68K_LIB_VERSION, KAFFEINE_P96_VERSION);
    m68k_write16(ctx, base + M68K_LIB_REVISION, 0);
    return base;
}
