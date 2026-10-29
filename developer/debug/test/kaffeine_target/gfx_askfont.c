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
struct CheckedExtent { struct TextExtent e; UBYTE guard[8]; };
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
    struct RastPort copy,kept;
    struct CheckedExtent ea,eb;
    struct TextExtent limit;
    static const UBYTE text[]="abcde";
    WORD spacing;
    ULONG fa,fb;
    unsigned j;
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
    spacing=w->RPort->TxSpacing;
    for(i=0;i<2;i++)
    {
        copy.TxSpacing=w->RPort->TxSpacing=i?2:-1;kept=copy;
        memset(&ea,0xA5,sizeof(ea));memset(&eb,0xA5,sizeof(eb));
        TextExtent(&copy,text,5,&ea.e);TextExtent(w->RPort,text,5,&eb.e);
        for(j=0;j<8;j++)if(ea.guard[j]!=0xA5||eb.guard[j]!=0xA5)break;
        check(!memcmp(&ea.e,&eb.e,sizeof(ea.e))&&j==8,
              "caller-owned TextExtent matches the window with signed spacing and guards");
        fa=TextFit(&copy,text+4,5,&ea.e,NULL,-1,font->tf_XSize*3,font->tf_YSize);
        fb=TextFit(w->RPort,text+4,5,&eb.e,NULL,-1,font->tf_XSize*3,font->tf_YSize);
        check(fa==fb&&!memcmp(&ea.e,&eb.e,sizeof(ea.e)),
              "reverse TextFit of a caller-owned RastPort matches the window");
        limit=ea.e;
        fa=TextFit(&copy,text,5,&ea.e,&limit,1,32767,font->tf_YSize);
        fb=TextFit(w->RPort,text,5,&eb.e,&limit,1,32767,font->tf_YSize);
        check(fa==fb&&!memcmp(&ea.e,&eb.e,sizeof(ea.e))&&!memcmp(&copy,&kept,sizeof(copy)),
              "constrained TextFit matches and preserves the complete caller RastPort");
    }
    w->RPort->TxSpacing=spacing;
done:
    if(w)CloseWindow(w);
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_askfont");
}
