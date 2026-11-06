/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   A program that stops while it owns the blitter. It owns the blitter,
   starts a line mode blit, which Kaffeine does not implement and refuses,
   and so stops with 20 before the second line: run it with
   --expect-rc hw_blitter_fault 20. Its output must be the first line
   only. Kaffeine then gives back the native hold this context took, so
   hw_blitter, run after it in the same boot, owns the blitter and passes
   instead of waiting for ever. */
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <hardware/custom.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>

#define CUSTOM ((volatile struct Custom *)0xdff000)

struct GfxBase *GfxBase;

int main(void)
{
    static UWORD line[4];

    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase)
        return 10;
    PutStr((CONST_STRPTR)"hw_blitter_fault: owning the blitter, starting a line blit\n");
    Flush(Output());
    OwnBlitter();
    WaitBlit();
    CUSTOM->bltcon0 = 0x0BCA;
    CUSTOM->bltcon1 = 0x0001;           /* LINE */
    CUSTOM->bltcpt = line;
    CUSTOM->bltdpt = line;
    CUSTOM->bltsize = (4 << 6) | 2;
    PutStr((CONST_STRPTR)"hw_blitter_fault: FAIL the refused blit did not stop the program\n");
    Flush(Output());
    WaitBlit();
    DisownBlitter();
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}
