/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Expected containment: an unconverted cybergraphics bitmap call stops20. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    struct Library *cyber=OpenLibrary("cybergraphics.library",0);
    if(!cyber) { PutStr("FAIL cybergraphics.library could not be opened\n");return 10; }
    PutStr("STEP unsupported cybergraphics bitmap call must stop with20\n");
    Flush(Output());
    {
        register struct Library *base __asm("a6")=cyber;
        register APTR bitmap __asm("a0")=NULL;
        register ULONG attribute __asm("d0")=0;
        __asm__ volatile("jsr -96(%%a6)" : "+r"(attribute) : "r"(base),"r"(bitmap)
                         : "d1","a1","cc","memory");
    }
    PutStr("FAIL unconverted bitmap call returned to guest code\n");
    CloseLibrary(cyber);return 10;
}
