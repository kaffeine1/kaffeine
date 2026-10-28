/* Original IntuiText measurement probe, shared by ARM and 68k builds. */
#include <exec/types.h>
#include <graphics/text.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
static unsigned checks,failed;
#define CHECK(x) do {checks++;if(!(x)){failed++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
int main(void)
{
    struct IntuiText text,saved;
    struct TextAttr attr;
    struct TextFont *font;
    LONG one,four;
    GfxBase=(struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",0);
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",0);
    CHECK(GfxBase!=NULL);CHECK(IntuitionBase!=NULL);
    if(!GfxBase||!IntuitionBase)goto done;
    CHECK(IntuiTextLength(NULL)==0);
    memset(&text,0,sizeof(text));
    text.IText=(UBYTE *)"M";saved=text;
    one=IntuiTextLength(&text);CHECK(one>0);CHECK(!memcmp(&text,&saved,sizeof(text)));
    text.IText=(UBYTE *)"MMMM";four=IntuiTextLength(&text);CHECK(four==one*4);
    text.IText=(UBYTE *)"";CHECK(IntuiTextLength(&text)==0);
    text.IText=(UBYTE *)"MMMM";text.NextText=(struct IntuiText *)(uintptr_t)0x12345678;
    CHECK(IntuiTextLength(&text)==four);
    memset(&attr,0,sizeof(attr));attr.ta_Name=(STRPTR)"topaz.font";attr.ta_YSize=8;
    attr.ta_Flags=FPF_ROMFONT;
    font=OpenFont(&attr);CHECK(font!=NULL);
    if(font){
        text.ITextFont=&attr;text.IText=(UBYTE *)"Hello";saved=text;
        CHECK(!(font->tf_Flags&FPF_PROPORTIONAL));
        CHECK(IntuiTextLength(&text)==5*font->tf_XSize);
        CHECK(!memcmp(&text,&saved,sizeof(text)));
        CloseFont(font);
    }
    attr.ta_Name=(STRPTR)"kaffeine-missing-test-font.font";
    text.ITextFont=&attr;text.IText=(UBYTE *)"MMMM";
    CHECK(IntuiTextLength(&text)==four);
    text.ITextFont=NULL;text.NextText=&text;CHECK(IntuiTextLength(&text)==four);
done:
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    printf("gfx_intuitext_width: %u checks, %u failed\n",checks,failed);return failed?10:0;
}
