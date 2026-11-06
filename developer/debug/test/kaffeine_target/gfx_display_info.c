/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Original display metadata regression. Values come from the native mode
    database; only classic layouts and measured transfer boundaries are fixed.
    Every prefix length 0..104 is checked, including partial rectangles and
    preservation of bytes that GetDisplayInfoData does not transfer.

    The same four queries also run inside LockIBase(), where classic programs
    ask for the metadata of a screen's mode. Each one asks for its classic
    struct size (DISP 56 bytes, as the NDK's sizeof). Only GetDisplayInfoData
    runs between LockIBase() and UnlockIBase(): results are collected there
    and compared with a baseline taken outside only after the unlock, with no
    output, check, DOS or library call while IBase is held. The counts are
    the classic ones; a native 64-bit build would need its own.
*/
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/monitor.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
struct Block { UBYTE pre[8], data[160], post[8]; };
static ULONG transferred(ULONG tag, ULONG size)
{
    ULONG n=tag==DTAG_DIMS?66:tag==DTAG_MNTR?88:48;
    if(size<n) n=size;
    if(tag==DTAG_DIMS && n>26) n=26+((n-26)/8)*8;
    return n;
}
static void query(ULONG id,ULONG tag)
{
    struct Block full,b,saved;
    struct QueryHeader header;
    ULONG n,size,i;
    BOOL counts=TRUE,prefixes=TRUE,guards=TRUE;
    memset(&full,0xA5,sizeof(full));
    n=GetDisplayInfoData(NULL,full.data,sizeof(full.data),tag,id);
    if(!check_long("full classic metadata transfer",n,transferred(tag,160))) return;
    memcpy(&header,full.data,sizeof(header));
    check_long("header identifies the requested chunk",header.StructID,tag);
    check_long("header uses TAG_SKIP",header.SkipID,3);
    check_long("header length describes classic local data",header.Length,tag==DTAG_DIMS?8:tag==DTAG_MNTR?9:4);
    if(tag==DTAG_MNTR)
    {
        struct MonitorInfo m,again;
        struct MonitorSpec saved;
        memcpy(&m,full.data,sizeof(m));
        if(check(m.Mspc!=NULL,"monitor metadata supplies a MonitorSpec"))
        {
            memcpy(&saved,m.Mspc,sizeof(saved));
            check_long("monitor and spec row counts agree",m.TotalRows,saved.total_rows);
            check_long("monitor and spec color clocks agree",m.TotalColorClocks,saved.total_colorclocks);
            if(saved.ms_Node.xln_Name)
            {
                ULONG len=0; while(len<512 && saved.ms_Node.xln_Name[len]) len++;
                check(len<512,"monitor name has a bounded terminator");
            }
            memset(&again,0,sizeof(again));
            check_long("repeat monitor query transfer",GetDisplayInfoData(NULL,(UBYTE *)&again,sizeof(again),tag,id),88);
            check(again.Mspc==m.Mspc,"repeated queries keep the same monitor identity");
            check(!memcmp(&saved,m.Mspc,sizeof(saved)),"repeated query preserves every MonitorSpec byte");
        }
    }
    for(size=0;size<=(tag==DTAG_MNTR?120:104);size++)
    {
        memset(&b,0xA5,sizeof(b));
        n=GetDisplayInfoData(NULL,b.data,size,tag,id);
        if(n!=transferred(tag,size)) counts=FALSE;
        if(n>sizeof(b.data) || memcmp(b.data,full.data,n)) prefixes=FALSE;
        for(i=0;i<sizeof(b);i++)
            if((i<8 || i>=8+n) && ((UBYTE *)&b)[i]!=0xA5) guards=FALSE;
    }
    check(counts,"all requested lengths have classic transfer counts");
    check(prefixes,"every copied prefix agrees with the full native query");
    check(guards,"all guards and untransferred reserved bytes remain intact");
    memset(&b,0xA5,sizeof(b)); memcpy(&saved,&b,sizeof(b));
    check_long("invalid mode has no metadata",GetDisplayInfoData(NULL,b.data,sizeof(b.data),tag,0xffffffffUL),0);
    check(!memcmp(&b,&saved,sizeof(b)),"invalid mode preserves the complete buffer and guards");
}
#define HELD_TAGS 4
static struct Block outside[HELD_TAGS], inside[HELD_TAGS];
static void held(ULONG id)
{
    static const ULONG tags[HELD_TAGS] = { DTAG_DISP, DTAG_DIMS, DTAG_NAME, DTAG_MNTR };
    ULONG size[HELD_TAGS], out_n[HELD_TAGS], in_n[HELD_TAGS], lock, i, j;
    BOOL counts = TRUE, same = TRUE, guards = TRUE;

    size[0] = sizeof(struct DisplayInfo); size[1] = sizeof(struct DimensionInfo);
    size[2] = sizeof(struct NameInfo); size[3] = sizeof(struct MonitorInfo);
    for (i = 0; i < HELD_TAGS; i++)
    {
        memset(&outside[i], 0xA5, sizeof(outside[i]));
        memset(&inside[i], 0xA5, sizeof(inside[i]));
        out_n[i] = GetDisplayInfoData(NULL, outside[i].data, size[i], tags[i], id);
    }
    lock = LockIBase(0);
    for (i = 0; i < HELD_TAGS; i++)
        in_n[i] = GetDisplayInfoData(NULL, inside[i].data, size[i], tags[i], id);
    UnlockIBase(lock);

    for (i = 0; i < HELD_TAGS; i++)
    {
        if (out_n[i] != transferred(tags[i], size[i]) || in_n[i] != out_n[i])
            counts = FALSE;
        if (in_n[i] > sizeof(inside[i].data) || memcmp(inside[i].data, outside[i].data, in_n[i]))
            same = FALSE;
        for (j = 0; j < sizeof(struct Block); j++)
            if ((j < 8 || j >= 8 + in_n[i]) && ((UBYTE *)&inside[i])[j] != 0xA5)
                guards = FALSE;
    }
    check(counts, "inside LockIBase(): classic counts for the classic struct sizes, as outside");
    check(same, "inside LockIBase(): the same metadata as outside");
    check(guards, "inside LockIBase(): guards and the reserved tails of the structs stay intact");
}
int main(void)
{
    ULONG id=NextDisplayInfo(0xffffffffUL);
    check_long("classic DisplayInfo size",sizeof(struct DisplayInfo),56);
    check_long("classic DimensionInfo size",sizeof(struct DimensionInfo),88);
    check_long("classic NameInfo size",sizeof(struct NameInfo),56);
    check_long("classic MonitorInfo size",sizeof(struct MonitorInfo),96);
    check_long("classic MonitorSpec size",sizeof(struct MonitorSpec),160);
    if(check(id!=0xffffffffUL,"native database provides a display mode"))
    {
        query(id,DTAG_DISP); query(id,DTAG_DIMS); query(id,DTAG_NAME); query(id,DTAG_MNTR);
        IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
        if (check(IntuitionBase != NULL, "intuition.library 37 opens"))
        {
            held(id);
            CloseLibrary((struct Library *)IntuitionBase);
            IntuitionBase = NULL;
        }
    }
    return test_done("gfx_display_info");
}
