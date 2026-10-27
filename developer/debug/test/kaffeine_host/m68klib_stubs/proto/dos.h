#pragma once
#include <exec/types.h>
#include <dos/bptr.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <exec/libraries.h>
struct DosLibrary { struct Library lib; };
#define BNULL ((BPTR)0)

#ifdef __cplusplus
extern "C" {
#endif
BPTR LoadSeg(CONST_STRPTR path);
void UnLoadSeg(BPTR seglist);
LONG GetSegListInfo(BPTR seglist, const struct TagItem *tags);
LONG RunCommand(BPTR seglist, ULONG stacksize, STRPTR argptr, ULONG argsize);
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR code);
#ifdef __cplusplus
}
#endif
