/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Protocol test of the unmodified released Amigent 1.1.1 daemon.
   Loopback-only screen protocol, independent known pixels and buffer guards.
   No SETUP, injected input, EXEC, external network or target hardware. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "socket_fixture_abi.h"
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>

#include <proto/intuition.h>
#include <proto/graphics.h>
/* Independent literal classic CyberGraphX ABI (NDK has no CGX headers). */
#define RECTFMT_RGB 0UL
#define RECTFMT_LUT8 3UL
#define CYBRMATTR_ISCYBERGFX 0x80000008UL
#define GetCyberMapAttr(b,a) LP2(96,ULONG,GetCyberMapAttr,struct BitMap *,b,a0,ULONG,a,d0,,CyberGfxBase)
#define ReadPixelArray(b,dx,dy,s,rp,x,y,w,h,f) LP10(120,ULONG,ReadPixelArray,APTR,b,a0,UWORD,dx,d0,UWORD,dy,d1,UWORD,s,d2,struct RastPort *,rp,a1,UWORD,x,d3,UWORD,y,d4,UWORD,w,d5,UWORD,h,d6,UBYTE,f,d7,,CyberGfxBase)
#include <string.h>
#include "m68ktest.h"

struct Library *SocketBase, *CyberGfxBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static const char daemon_name[] = "amigent-screen-fixture";
static const char key[] = "public-loopback-fixture-key";
static const char proof_path[] = "SYS:EMU68/socket-proof.txt";
static const char proof[] = "exact Amigent channel bytes\n";

static BOOL read_all(LONG fd, void *data, LONG length)
{
    char *p = data;
    while (length)
    {
        LONG n = recv(fd, p, length, 0);
        if (n <= 0) return FALSE;
        p += n; length -= n;
    }
    return TRUE;
}

static BOOL command(LONG fd, UBYTE which, UWORD x)
{
    UBYTE header[10];
    memset(header, 0, sizeof(header)); header[0] = which;
    header[2] = x >> 8; header[3] = x;
    return send(fd, header, sizeof(header), 0) == sizeof(header);
}

static LONG connection(void)
{
    struct KSocketIn address;
    struct KSocketTime tv = {3, 0};
    LONG fd, tries;
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_port = htons(44125); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (tries = 0; tries < 40; ++tries)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (!connect(fd, (struct KSocketAddress *)&address, sizeof(address))) return fd;
        CloseSocket(fd); Delay(2);
    }
    return -1;
}

static BOOL hello(LONG fd)
{
    UBYTE bytes[8];
    return read_all(fd, bytes, sizeof(bytes)) && !memcmp(bytes, "AMGT", 4) &&
           bytes[4] == 0 && bytes[5] == 3 && (bytes[7] & 1);
}

static BOOL authenticate(LONG fd)
{
    UBYTE status = 255;
    return hello(fd) && command(fd, 17, sizeof(key) - 1) &&
           send(fd, key, sizeof(key) - 1, 0) == sizeof(key) - 1 &&
           read_all(fd, &status, 1) && status == 0;
}

static UWORD be16(const UBYTE *p) { return (UWORD)p[0]<<8 | p[1]; }
static BOOL region(LONG fd, UBYTE arg)
{
    UBYTE head[10]={13,0,0,64,0,64,0,32,0,24};
    UBYTE reply[8], pixels[32*24*3], packed[32*24*4]; ULONG i,n,j;
    head[1]=arg;
    if(send(fd,head,10,0)!=10 || !read_all(fd,reply,arg?8:4) || be16(reply)!=32 || be16(reply+2)!=24)return FALSE;
    n=sizeof(pixels);
    if(arg) {
        n=(ULONG)reply[4]<<24 | (ULONG)reply[5]<<16 | (ULONG)reply[6]<<8 | reply[7];
        if(n>sizeof(packed) || !read_all(fd,packed,n))return FALSE;
        if(n==sizeof(pixels))memcpy(pixels,packed,n);
        else {
            if(n%4)return FALSE;
            for(i=0,j=0;i<n;i+=4) { ULONG run=(ULONG)packed[i]+1,k;
                if(j+run*3>sizeof(pixels))return FALSE;
                for(k=0;k<run;++k) {memcpy(pixels+j,packed+i+1,3);j+=3;}
            }
            if(j!=sizeof(pixels))return FALSE;
        }
    } else if(!read_all(fd,pixels,n))return FALSE;
    for(i=0;i<sizeof(pixels);i+=3)
        if(pixels[i]!=42 || pixels[i+1]!=99 || pixels[i+2]!=177)return FALSE;
    return TRUE;
}
static BOOL screen_list(LONG fd, UWORD width, UWORD height)
{
    UBYTE count[2],item[14],title[256]; UWORD i,n; BOOL front=FALSE;
    if(!command(fd,11,0) || !read_all(fd,count,2) || be16(count)>32)return FALSE;
    for(i=0;i<be16(count);++i) {
        if(!read_all(fd,item,14) || (n=be16(item+12))>=sizeof(title) || !read_all(fd,title,n))return FALSE;
        if(i==0)front=be16(item+4)==width && be16(item+6)==height &&
          be16(item+8)==8 && (be16(item+10)&5)==5;
    }
    return be16(count)>0 && front;
}
static BOOL direct_readback(struct Screen *screen)
{
    UBYTE bytes[160], before[160]; ULONG i,j,result,key; struct Screen *front;
    memset(bytes,0x5a,sizeof(bytes));memcpy(before,bytes,sizeof(bytes));
    result=ReadPixelArray(bytes,2,1,32,&screen->RastPort,64,64,5,3,RECTFMT_RGB);
    check(result==15,"typed RGB readback returns every requested pixel");
    for(i=0;i<3;++i)for(j=0;j<5;++j) {
        before[32+i*32+6+j*3]=42;before[32+i*32+7+j*3]=99;before[32+i*32+8+j*3]=177;
    }
    check(!memcmp(bytes,before,sizeof(bytes)),"RGB stride/origin preserve all prefix, padding and suffix bytes");
    check(ReadPixelArray(bytes,2,1,8,&screen->RastPort,64,64,5,3,RECTFMT_RGB)==0 &&
          !memcmp(bytes,before,sizeof(bytes)),"short destination stride refuses before writes");
    check(ReadPixelArray((APTR)0xfffffff0UL,0,0,15,&screen->RastPort,64,64,5,3,RECTFMT_RGB)==0,
          "invalid destination refuses before native writes");
    check(ReadPixelArray(bytes,0,0,15,&screen->RastPort,64,64,5,3,RECTFMT_LUT8)==0,
          "unsupported native read format is explicit and write-free");
    {
        BOOL geometry=FALSE,attributes=FALSE,mode=FALSE,readback=FALSE,nested=FALSE;
        key=LockIBase(0);front=IntuitionBase->FirstScreen;
        geometry=front && front->Width==screen->Width && front->Height==screen->Height;
        if(front) {
            ULONG inner;
            attributes=GetCyberMapAttr(front->RastPort.BitMap,CYBRMATTR_ISCYBERGFX)!=0 && GetBitMapAttr(front->RastPort.BitMap,BMA_DEPTH)==8;
            mode=GetVPModeID(&front->ViewPort)==0x00100001;
            memset(bytes,0x5a,sizeof(bytes));
            result=ReadPixelArray(bytes,0,0,15,&front->RastPort,64,64,5,3,RECTFMT_RGB);
            readback=result==15 && bytes[0]==42 && bytes[1]==99 && bytes[2]==177;
            inner=LockIBase(0);nested=IntuitionBase->FirstScreen==front;UnlockIBase(inner);
        }
        UnlockIBase(key);
        if(!geometry)Printf("snapshot geometry %ld x %ld versus owned %ld x %ld\n",
            front?(LONG)front->Width:-1L,front?(LONG)front->Height:-1L,(LONG)screen->Width,(LONG)screen->Height);
        check(geometry,"LockIBase supplies current native front screen geometry");
        check(attributes,"snapshot bitmap token permits scalar RTG/depth queries while locked");
        check(mode,"snapshot embedded ViewPort resolves its native mode");
        check(readback,"read-only snapshot RastPort captures real RGB pixels");
        check(nested,"nested IBase lock retains snapshot identity");
    }
    return result==15;
}
int main(void)
{
    BPTR segment = 0;
    struct Screen *screen=NULL;
    struct Process *daemon = NULL;
    struct Task *live;
    LONG fd = -1, channel = -1, tries, old_priority;
    UBYTE status = 255, size[4];
    char bytes[sizeof(proof)];
    ULONG length;
    old_priority = SetTaskPri(FindTask(NULL), -5);
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    CyberGfxBase=OpenLibrary("cybergraphics.library",41);
    if(!check(IntuitionBase && GfxBase && CyberGfxBase,"open typed screen/graphics/CGX libraries"))goto end;
    screen=OpenScreenTags(NULL,SA_DisplayID,0x00100001,SA_Width,640,SA_Height,480,
        SA_Depth,8,SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_ShowTitle,FALSE,
        SA_PubName,(ULONG)"KAFFEINE-AMIGENT-FIXTURE",SA_Title,(ULONG)"Amigent known pixels",TAG_DONE);
    if(!check(screen!=NULL,"open isolated owned RTG screen for known pixels"))goto end;
    PubScreenStatus(screen,0);ScreenToFront(screen);
    SetRGB32(&screen->ViewPort,2,0x2a2a2a2a,0x63636363,0xb1b1b1b1);
    SetAPen(&screen->RastPort,2);RectFill(&screen->RastPort,64,64,127,127);
    /* AROS ScreenToFront queues an Intuition action; wait on observed
       geometry outside IBase rather than assuming that it already ran. */
    for(tries=0;tries<50;++tries) {
        ULONG lock=LockIBase(0);struct Screen *front=IntuitionBase->FirstScreen;
        BOOL ready=front && front->Width==screen->Width && front->Height==screen->Height;
        UnlockIBase(lock);if(ready)break;Delay(1);
    }
    check(tries<50,"asynchronous ScreenToFront reached the requested geometry");
    if(!direct_readback(screen))goto end;
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!check(SocketBase != NULL, "client opens the typed socket bridge")) goto end;
    segment = LoadSeg((CONST_STRPTR)"SYS:EMU68/amigentd");
    if (!check(segment != 0, "loads the original released Amigent daemon hunk")) goto end;
    daemon = CreateNewProcTags(NP_Seglist, segment, NP_FreeSeglist, TRUE,
        NP_Name, (ULONG)daemon_name, NP_CommandName, (ULONG)"amigentd",
        NP_Arguments, (ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
        NP_Cli, TRUE, NP_StackSize, 65536, NP_CopyVars, FALSE, TAG_DONE);
    if (!check(daemon != NULL, "starts the unmodified daemon as an independent guest Process")) goto end;
    segment = 0; /* successful NP_FreeSeglist transfers our loaded reference */
    fd = connection();
    if (!check(fd >= 0, "original daemon listens on its dedicated loopback port")) goto end;
    check(hello(fd), "original protocol hello is v3 and requires the fixture key");
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 1,
          "unauthenticated PING is refused");
    CloseSocket(fd); fd = connection();
    if (!check(fd >= 0 && authenticate(fd), "authenticates with the nonempty isolated fixture key")) goto end;
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 0,
          "authenticated PING completes without requesting graphics or input");
    {
        UBYTE info[8];
        check(command(fd,1,0) && read_all(fd,info,8) && be16(info)==screen->Width &&
              be16(info+2)==screen->Height && be16(info+4)==64,
              "original daemon INFO reports current front geometry");
        check(screen_list(fd,screen->Width,screen->Height),"original SCREENS returns front RTG mode/depth and bounded titles");
        check(region(fd,0),"original FRAME_RGB raw pixels match all 768 known RGB triples");
        check(region(fd,1),"original FRAME_RGB packed pixels decode to all known RGB triples");
    }
    channel = connection();
    if (!check(channel >= 0 && authenticate(channel), "authenticates an independent command connection")) goto end;
    status = 255;
    if (!check(command(channel, 20, 0) && read_all(channel, &status, 1) && status == 0,
               "CHANNEL transfers the socket to the daemon's guest child")) goto end;
    status = 255; memset(bytes, 0, sizeof(bytes));
    if (command(channel, 22, sizeof(proof_path) - 1) &&
        send(channel, proof_path, sizeof(proof_path) - 1, 0) == sizeof(proof_path) - 1 &&
        read_all(channel, &status, 1) && status == 0 && read_all(channel, size, 4))
    {
        length = (ULONG)size[0] << 24 | (ULONG)size[1] << 16 | (ULONG)size[2] << 8 | size[3];
        check(length == sizeof(proof) - 1 && read_all(channel, bytes, length) &&
              !memcmp(bytes, proof, sizeof(proof) - 1), "READ returns every expected byte through the transferred socket");
    }
    else check(FALSE, "READ returns every expected byte through the transferred socket");
    status = 255;
    check(command(fd, 6, 0) && read_all(fd, &status, 1) && status == 0,
          "original main connection remains responsive beside the command child");
end:
    if (channel >= 0) CloseSocket(channel);
    if (fd >= 0) CloseSocket(fd);
    if (daemon)
    {
        live = FindTask((CONST_STRPTR)daemon_name);
        if (live) Signal(live, SIGBREAKF_CTRL_C);
        for (tries = 0; tries < 100 && FindTask((CONST_STRPTR)daemon_name); ++tries) Delay(1);
        check(FindTask((CONST_STRPTR)daemon_name) == NULL, "original daemon handles Ctrl-C and its children terminate");
    }
    if (segment && !FindTask((CONST_STRPTR)daemon_name)) UnLoadSeg(segment);
    if (SocketBase) CloseLibrary(SocketBase);
    if(screen)check(CloseScreen(screen),"owned capture screen closes after daemon termination");
    if(CyberGfxBase)CloseLibrary(CyberGfxBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    SetTaskPri(FindTask(NULL), old_priority);
    return test_done("net_amigent_screen");
}
