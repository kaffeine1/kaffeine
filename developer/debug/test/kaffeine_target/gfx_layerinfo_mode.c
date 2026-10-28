/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original native ARM / classic 68k LayerInfo backfill mode probe. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/layers.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
struct GfxBase *GfxBase;
struct Library *LayersBase;
struct IntuitionBase *IntuitionBase;
static unsigned checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
int main(void)
{
    struct Layer_Info *li = NULL; struct Layer *layer = NULL;
    struct BitMap bm; UBYTE *plane = NULL;
    struct Screen *screen = NULL; ULONG id = (ULONG)INVALID_ID; unsigned i;
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CHECK(LayersBase != NULL); CHECK(GfxBase != NULL);
    if (!LayersBase || !GfxBase) goto done;
    li = NewLayerInfo(); CHECK(li != NULL); if (!li) goto done;
    CHECK(li->BlankHook == LAYERS_BACKFILL);
    CHECK(InstallLayerInfoHook(li, LAYERS_NOBACKFILL) == LAYERS_BACKFILL);
    CHECK(li->BlankHook == LAYERS_NOBACKFILL);
    CHECK(InstallLayerInfoHook(li, LAYERS_NOBACKFILL) == LAYERS_NOBACKFILL);
    CHECK(InstallLayerInfoHook(li, LAYERS_BACKFILL) == LAYERS_NOBACKFILL);
    CHECK(li->BlankHook == LAYERS_BACKFILL);
    plane = AllocMem(64, MEMF_CHIP | MEMF_CLEAR); CHECK(plane != NULL); if (!plane) goto done;
    InitBitMap(&bm, 1, 32, 16); bm.Planes[0] = plane;
    layer = CreateUpfrontHookLayer(li, &bm, 0, 0, 31, 15, LAYERSIMPLE, LAYERS_NOBACKFILL, NULL);
    CHECK(layer != NULL); if (!layer) goto done;
    CHECK(InstallLayerInfoHook(li, LAYERS_NOBACKFILL) == LAYERS_BACKFILL);
    CHECK(li->BlankHook == LAYERS_NOBACKFILL);
    { LONG deleted = DeleteLayer(0, layer); CHECK(deleted); if (!deleted) goto done; layer = NULL; }
    CHECK(InstallLayerInfoHook(li, LAYERS_BACKFILL) == LAYERS_NOBACKFILL);
    CHECK(li->BlankHook == LAYERS_BACKFILL);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    CHECK(IntuitionBase != NULL); if (!IntuitionBase) goto done;
    for (i = 0; i < 256; ++i)
    {
        struct DimensionInfo d;
        id = NextDisplayInfo(id); if (id == (ULONG)INVALID_ID) break;
        memset(&d, 0, sizeof(d));
        if (GetDisplayInfoData(NULL, (UBYTE *)&d, sizeof(d), DTAG_DIMS, id) && d.MaxDepth == 8) break;
    }
    CHECK(id != (ULONG)INVALID_ID); if (id == (ULONG)INVALID_ID) goto done;
    screen = OpenScreenTags(NULL, SA_DisplayID, id, SA_Depth, 8, SA_Width, 320,
                            SA_Height, 200, SA_Type, CUSTOMSCREEN, SA_Behind, TRUE, TAG_DONE);
    CHECK(screen != NULL); if (!screen) goto done;
    {
        struct Hook *previous = (struct Hook *)screen->LayerInfo.BlankHook;
        CHECK((uintptr_t)previous <= 1);
        CHECK(InstallLayerInfoHook(&screen->LayerInfo, LAYERS_NOBACKFILL) == previous);
        CHECK(screen->LayerInfo.BlankHook == LAYERS_NOBACKFILL);
        CHECK(InstallLayerInfoHook(&screen->LayerInfo, previous) == LAYERS_NOBACKFILL);
        CHECK(screen->LayerInfo.BlankHook == previous);
    }
done:
    if (screen) CHECK(CloseScreen(screen));
    if (layer && DeleteLayer(0, layer)) layer = NULL;
    if (!layer && li) DisposeLayerInfo(li);
    if (!layer && plane) FreeMem(plane, 64);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (LayersBase) CloseLibrary(LayersBase);
    printf("gfx_layerinfo_mode: %u checks, %u failed\n", checks, failed);
    return failed ? 10 : 0;
}
