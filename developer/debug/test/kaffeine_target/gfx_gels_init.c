/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original GELS sentinel initialization, also built for native ARM. */
#include <exec/types.h>
#include <graphics/gels.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
#if defined(__mc68000__) || defined(__m68k__)
#define PATH_FIELD DrawPath
#else
#define PATH_FIELD IntVSprite
#endif
int main(void)
{
    struct Sprite { ULONG pre; struct VSprite value; ULONG post; } head,tail,wh,wt;
    struct Info { ULONG pre; struct GelsInfo value; ULONG post; } info,wi;
    struct collTable coll,wc;
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    if(!check(GfxBase!=NULL,"open graphics"))goto done;
    memset(&head,0xA5,sizeof(head));memset(&tail,0x5A,sizeof(tail));
    memset(&info,0x31,sizeof(info));memset(&coll,0x73,sizeof(coll));
    info.value.collHandler=&coll;
    wh=head;wt=tail;wi=info;wc=coll;
    wh.value.NextVSprite=&tail.value;wh.value.PrevVSprite=NULL;
    wh.value.PATH_FIELD=NULL;wh.value.ClearPath=&tail.value;
    wh.value.OldY=wh.value.OldX=wh.value.Y=wh.value.X=(WORD)0x8000;
    wt.value.NextVSprite=NULL;wt.value.PrevVSprite=&head.value;wt.value.PATH_FIELD=NULL;
    wt.value.OldY=wt.value.OldX=wt.value.Y=wt.value.X=0x7FFF;
    wi.value.gelHead=&head.value;wi.value.gelTail=&tail.value;wc.collPtrs[0]=NULL;
    InitGels(&head.value,&tail.value,&info.value);
    check(!memcmp(&head,&wh,sizeof(head)),"head links, minimum coordinates and untouched storage");
    check(!memcmp(&tail,&wt,sizeof(tail)),"tail links, maximum coordinates and untouched storage");
    check(!memcmp(&info,&wi,sizeof(info)),"GelsInfo links and all other fields preserved");
    check(!memcmp(&coll,&wc,sizeof(coll)),"only collision handler zero is cleared");
    check(head.value.NextVSprite==&tail.value&&tail.value.PrevVSprite==&head.value&&
          info.value.gelHead==&head.value&&info.value.gelTail==&tail.value,
          "readable list identities contain caller addresses");
    InitGels(&head.value,&tail.value,&info.value);
    check(!memcmp(&head,&wh,sizeof(head))&&!memcmp(&tail,&wt,sizeof(tail))&&
          !memcmp(&info,&wi,sizeof(info))&&!memcmp(&coll,&wc,sizeof(coll)),"repeated initialization is stable");
    info.value.collHandler=NULL;wi.value.collHandler=NULL;
    InitGels(&head.value,&tail.value,&info.value);
    check(!memcmp(&head,&wh,sizeof(head))&&!memcmp(&tail,&wt,sizeof(tail))&&
          !memcmp(&info,&wi,sizeof(info)),"initialization without a collision table");
done:
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_gels_init");
}
