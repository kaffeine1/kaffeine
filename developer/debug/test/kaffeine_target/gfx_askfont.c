/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original AskFont output-layout and font-identity probe. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct CheckedAttr { struct TextAttr attr; UBYTE guard[16]; };
struct CheckedRP { UBYTE before[16]; struct RastPort rp; UBYTE after[16]; };
static BOOL guarded(const struct CheckedAttr *b)
{
    unsigned i;for(i=0;i<sizeof(b->guard);i++)if(b->guard[i]!=0xA5)return FALSE;
    return TRUE;
}
int main(void)
{
    struct Window *w=NULL;
    struct CheckedAttr a;
    struct CheckedRP raw;
    struct RastPort copy;
    struct TextFont *font;
    STRPTR name;
    unsigned i;
    GfxBase=(struct GfxBase*)OpenLibrary("graphics.library",33);
    IntuitionBase=(struct IntuitionBase*)OpenLibrary("intuition.library",33);
    if(!check(GfxBase&&IntuitionBase,"graphics and Intuition libraries"))goto done;
    w=OpenWindowTags(NULL,WA_Width,220,WA_Height,100,TAG_DONE);
    if(!check(w&&w->RPort&&w->RPort->Font,"owned window has a typed RastPort and font"))goto done;
    font=w->RPort->Font;memset(&a,0xA5,sizeof(a));
    AskFont(w->RPort,&a.attr);
    check(guarded(&a),"AskFont preserves bytes after TextAttr");
    check(a.attr.ta_Name&&font->tf_Message.mn_Node.ln_Name&&
          !strcmp((const char*)a.attr.ta_Name,(const char*)font->tf_Message.mn_Node.ln_Name),"font name is readable and matches the font");
    check(a.attr.ta_YSize==font->tf_YSize,"font height matches TextFont");
    check(a.attr.ta_Style==font->tf_Style&&a.attr.ta_Flags==font->tf_Flags,"style and flags match TextFont");
    name=a.attr.ta_Name;
    for(i=0;i<16;i++)AskFont(w->RPort,&a.attr);
    check(a.attr.ta_Name==name&&guarded(&a),"repeated calls preserve name identity and guards");
    check(w->RPort->Font==font,"query leaves RastPort font identity intact");
    memset(&raw,0xA5,sizeof(raw));InitRastPort(&raw.rp);
    for(i=0;i<16;i++)if(raw.before[i]!=0xA5||raw.after[i]!=0xA5)break;
    check(i==16,"InitRastPort preserves surrounding storage");
    check(raw.rp.Mask==0xFF&&(UBYTE)raw.rp.FgPen==0xFF&&
          (UBYTE)raw.rp.AOlPen==0xFF&&raw.rp.DrawMode==JAM2&&raw.rp.LinePtrn==0xFFFF&&
          !raw.rp.Layer&&!raw.rp.BitMap&&raw.rp.Font,"caller-owned RastPort has system defaults");
    SetFont(&raw.rp,font);
    check(raw.rp.Font==font&&raw.rp.TxHeight==font->tf_YSize&&
          raw.rp.TxWidth==font->tf_XSize&&raw.rp.TxBaseline==font->tf_Baseline,
          "SetFont fills caller-owned RastPort metrics");
    memcpy(&copy,&raw.rp,sizeof(copy));memset(&a,0xA5,sizeof(a));
    AskFont(&copy,&a.attr);
    check(a.attr.ta_Name==name&&guarded(&a)&&a.attr.ta_YSize==font->tf_YSize,
          "AskFont accepts a copied caller-owned RastPort");
    copy.TxSpacing=-1;
    check(TextLength(&copy,(CONST_STRPTR)"abc",3)==TextLength(w->RPort,(CONST_STRPTR)"abc",3)-3,
          "TextLength measures a copied RastPort with signed spacing");
done:
    if(w)CloseWindow(w);
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_askfont");
}
