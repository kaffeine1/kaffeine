#pragma once
#include <workbench/workbench.h>
#ifdef __cplusplus
extern "C" {
#endif
struct DiskObject *GetDiskObject(CONST_STRPTR);
struct DiskObject *GetDiskObjectNew(CONST_STRPTR);
struct DiskObject *GetDefDiskObject(LONG);
struct DiskObject *NewDiskObject(ULONG);
void FreeDiskObject(struct DiskObject *);
struct TagItem;
struct DiskObject *DupDiskObjectA(struct DiskObject *, struct TagItem *);
BOOL PutDiskObject(CONST_STRPTR, struct DiskObject *);
UBYTE *FindToolType(const STRPTR *, const STRPTR);
BOOL MatchToolValue(UBYTE *, UBYTE *);
#ifdef __cplusplus
}
#endif
