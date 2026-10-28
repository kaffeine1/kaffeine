#pragma once
/* The native icon.library of the Workbench tests, defined by the test. The
   plain calls go through the global base, as kaffeine_icon.c makes them;
   the __..._WB forms take the base as their first argument, as the SDK
   defines them, and record which base each call used. */
#include <workbench/workbench.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Library;
struct TagItem;
struct DiskObject *GetDiskObject(CONST_STRPTR);
struct DiskObject *GetDiskObjectNew(CONST_STRPTR);
struct DiskObject *GetDefDiskObject(LONG);
struct DiskObject *NewDiskObject(ULONG);
void FreeDiskObject(struct DiskObject *);
UBYTE *FindToolType(const STRPTR *, const STRPTR);
BOOL MatchToolValue(UBYTE *, UBYTE *);
struct DiskObject *Test_DupDiskObjectA_WB(struct Library *base, struct DiskObject *icon, struct TagItem *tags);
void Test_FreeDiskObject_WB(struct Library *base, struct DiskObject *icon);
#ifdef __cplusplus
}
#endif
#define __DupDiskObjectA_WB(base, icon, tags) Test_DupDiskObjectA_WB((base), (icon), (tags))
#define __FreeDiskObject_WB(base, icon) Test_FreeDiskObject_WB((base), (icon))
