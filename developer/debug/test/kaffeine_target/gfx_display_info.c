/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Original display metadata regression. Values come from the native mode
    database; only classic layouts and measured transfer boundaries are fixed.
    Every prefix length 0..104 is checked, including partial rectangles and
    preservation of bytes that GetDisplayInfoData does not transfer.
*/
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/monitor.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
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
    }
    return test_done("gfx_display_info");
}
