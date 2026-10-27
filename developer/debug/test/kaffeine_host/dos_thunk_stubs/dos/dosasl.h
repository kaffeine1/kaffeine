#pragma once
#include <dos/dos.h>

/* Token ABI from compiler/include/dos/dosasl.h. */
#define P_ANY      0x80
#define P_SINGLE   0x81
#define P_ORSTART  0x82
#define P_ORNEXT   0x83
#define P_OREND    0x84
#define P_NOT      0x85
#define P_NOTEND   0x86
#define P_NOTCLASS 0x87
#define P_CLASS    0x88
#define P_REPBEG   0x89
#define P_REPEND   0x8a
#define P_STOP     0x8b
#define ERROR_BUFFER_OVERFLOW 303
#define ERROR_BREAK 304

/* The native layouts of compiler/include/dos/dosasl.h, with host pointers
   and the native FIB, so they differ from the 68k ones as on AArch64. */
struct AChain
{
    struct AChain *an_Child;
    struct AChain *an_Parent;
    BPTR an_Lock;
    struct FileInfoBlock an_Info;
    BYTE an_Flags;
    UBYTE an_String[1];
};

struct AnchorPath
{
    struct AChain *ap_Base;
    struct AChain *ap_Last;
    LONG ap_BreakBits;
    LONG ap_FoundBreak;
    BYTE ap_Flags;
    BYTE ap_Reserved;
    WORD ap_Strlen;
    struct FileInfoBlock ap_Info;
    UBYTE ap_Buf[1];
};
#define ap_Current ap_Last

#define DDF_PatternBit  (1<<0)
#define DDF_ExaminedBit (1<<1)
#define APF_ITSWILD     (1<<1)
#define APF_DODIR       (1<<2)
#define APF_DIDDIR      (1<<3)
