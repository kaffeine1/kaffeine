/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Kaffeine-specific unsupported font-extension boundary regression. */
#include <exec/types.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"

struct GfxBase *GfxBase;
int main(void)
{
    struct TextAttr attr={(STRPTR)"topaz.font",8,0,0};
    struct TextFont *font=NULL,copy;
    struct TagItem tags[]={{TAG_USER|0x8001,0xDEADBEEF},{TAG_DONE,0}};
    GfxBase=(struct GfxBase*)OpenLibrary("graphics.library",37);
    if(!check(GfxBase!=NULL,"graphics library opens"))goto done;
    font=OpenFont(&attr);
    if(!check(font!=NULL,"typed monochrome font opens"))goto done;
    memcpy(&copy,font,sizeof(copy));
    check(!ExtendFont(font,NULL),"unsupported guest extension reports failure");
    check(!memcmp(font,&copy,sizeof(copy)),"failed extension preserves complete classic TextFont");
    check(!ExtendFont(font,tags),"unconverted pointer-valued tags are refused");
    StripFont(font);
    check(!memcmp(font,&copy,sizeof(copy)),"strip leaves unextended guest and shared native font intact");
    check(!ExtendFont(NULL,NULL),"NULL font reports failure");
    StripFont(NULL);
    check(!ExtendFont((struct TextFont*)0xFFFFFFFFUL,(struct TagItem*)0xFFFFFFFFUL),
          "unowned addresses never reach native font extension calls");
    StripFont((struct TextFont*)0xFFFFFFFFUL);
    check(!memcmp(font,&copy,sizeof(copy)),"invalid-address calls preserve live font");
done:
    if(font)CloseFont(font);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_font_extension");
}
