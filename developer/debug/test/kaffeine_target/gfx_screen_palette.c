/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original palette probe, shared by native ARM and classic 68k builds. */
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
static unsigned checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
static ULONG indexed_mode(void)
{
    ULONG id = INVALID_ID; unsigned i;
    for (i = 0; i < 256; ++i)
    {
        struct DimensionInfo d;
        id = NextDisplayInfo(id); if (id == (ULONG)INVALID_ID) break;
        memset(&d, 0, sizeof(d));
        if (GetDisplayInfoData(NULL, (UBYTE *)&d, sizeof(d), DTAG_DIMS, id) && d.MaxDepth == 8)
            return id;
    }
    return INVALID_ID;
}
int main(void)
{
    struct Screen *s = NULL;
    ULONG id, table[] = {0x00020004, 0x12121212, 0x34343434, 0x56565656,
                       0xABABABAB, 0xCDCDCDCD, 0xEFEFEFEF,
                       0x0001000A, 0x98989898, 0x76767676, 0x54545454, 0};
    ULONG saved[sizeof(table) / sizeof(table[0])];
    struct { ULONG before, rgb[9], after; } output;
    UWORD old[] = {0x123, 0x456, 0xABC};
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    CHECK(GfxBase != NULL); CHECK(IntuitionBase != NULL);
    if (!GfxBase || !IntuitionBase) goto done;
    id = indexed_mode(); CHECK(id != (ULONG)INVALID_ID); if (id == (ULONG)INVALID_ID) goto done;
    s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, 640, SA_Height, 480,
                       SA_Depth, 8, SA_Type, CUSTOMSCREEN, SA_Behind, TRUE, TAG_DONE);
    CHECK(s != NULL); if (!s) goto done;
    CHECK(s->ViewPort.ColorMap != NULL); if (!s->ViewPort.ColorMap) goto done;
    CHECK(s->ViewPort.ColorMap->Count == 256);
    memcpy(saved, table, sizeof(table)); LoadRGB32(&s->ViewPort, table);
    CHECK(!memcmp(saved, table, sizeof(table)));
    output.before = 0x12345678; output.after = 0xDEADBEEF;
    GetRGB32(s->ViewPort.ColorMap, 4, 2, output.rgb);
    CHECK(output.rgb[0] == 0x12121212); CHECK(output.rgb[1] == 0x34343434);
    CHECK(output.rgb[2] == 0x56565656); CHECK(output.rgb[3] == 0xABABABAB);
    CHECK(output.rgb[4] == 0xCDCDCDCD); CHECK(output.rgb[5] == 0xEFEFEFEF);
    CHECK(output.before == 0x12345678 && output.after == 0xDEADBEEF);
    GetRGB32(s->ViewPort.ColorMap, 10, 1, output.rgb);
    CHECK(output.rgb[0] == 0x98989898 && output.rgb[1] == 0x76767676 && output.rgb[2] == 0x54545454);
    LoadRGB32(&s->ViewPort, NULL);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 4) == 0x135);
    LoadRGB4(&s->ViewPort, old, 3);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 0) == 0x123);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 1) == 0x456);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 2) == 0xABC);
    SetRGB4(&s->ViewPort, 7, 7, 2, 14);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 7) == 0x72E);
    SetRGB4CM(s->ViewPort.ColorMap, 7, 3, 9, 5);
    CHECK(GetRGB4(s->ViewPort.ColorMap, 7) == 0x395);
    SetRGB32(&s->ViewPort, 8, 0xDEDEDEDE, 0xADADADAD, 0xBEBEBEBE);
    GetRGB32(s->ViewPort.ColorMap, 8, 1, output.rgb);
    CHECK(output.rgb[0] == 0xDEDEDEDE && output.rgb[1] == 0xADADADAD && output.rgb[2] == 0xBEBEBEBE);
    SetRGB32CM(s->ViewPort.ColorMap, 9, 0x21212121, 0x43434343, 0x65656565);
    GetRGB32(s->ViewPort.ColorMap, 9, 1, output.rgb);
    CHECK(output.rgb[0] == 0x21212121 && output.rgb[1] == 0x43434343 && output.rgb[2] == 0x65656565);
done:
    if (s) CHECK(CloseScreen(s));
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    printf("gfx_screen_palette: %u checks, %u failed\n", checks, failed);
    return failed ? 10 : 0;
}
