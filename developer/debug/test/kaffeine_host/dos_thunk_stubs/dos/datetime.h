#pragma once
#include <dos/dos.h>
#define LEN_DATSTRING 16
#define FORMAT_DOS 0
#define FORMAT_INT 1
#define FORMAT_USA 2
#define FORMAT_CDN 3
#define FORMAT_DEF 4
#define DTF_SUBST 1
#define DTF_FUTURE 2
struct DateTime
{
    struct DateStamp dat_Stamp;
    UBYTE dat_Format, dat_Flags;
    STRPTR dat_StrDay, dat_StrDate, dat_StrTime;
};
