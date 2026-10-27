/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Host build of the DOS HUNK loader, rom/dos/internalloadseg_aos.c, for
    the kaffeine tests. The loader source is compiled unchanged; the DOS
    internals it uses are replaced by the declarations below. register_hunk()
    is provided by the test program.
*/
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/bptr.h>
#include <dos/dosextens.h>
#include <dos/doshunks.h>
#include <proto/dos.h>
#include <exec/execbase.h>

/* Keep the real DOS internal headers out of the host build. */
#define DOS_INTERN_H
#define INTERNALLOADSEG_H
#define LOADSEG_LOADSEG_H

#define ASSERT_VALID_PROCESS(p) ((void)(p))
#ifndef BNULL
#define BNULL ((BPTR)0)
#endif

struct Process host_process;
static struct ExecBase *SysBase;

typedef LONG (*host_read_func)(BPTR, void *, LONG, struct DosLibrary *);
typedef void *(*host_alloc_func)(ULONG, ULONG, struct ExecBase *);
typedef void (*host_free_func)(void *, ULONG, struct ExecBase *);

#define ilsRead(file, buf, size) ((host_read_func)funcarray[0])(file, buf, size, DOSBase)
#define ilsAllocMem(size, flags) ((host_alloc_func)funcarray[1])(size, flags, SysBase)
#define ilsFreeMem(addr, size)   ((host_free_func)funcarray[2])(addr, size, SysBase)

APTR _ilsAllocVec(SIPTR *funcarray, ULONG size, ULONG flags);
void _ilsFreeVec(SIPTR *funcarray, void *buf);
#define ilsAllocVec(size, flags) _ilsAllocVec(funcarray, size, flags)
#define ilsFreeVec(ptr)          _ilsFreeVec(funcarray, ptr)

BPTR InternalLoadSeg_AOS(BPTR fh, BPTR table, SIPTR *funcarray,
                         LONG *stacksize, struct DosLibrary *DOSBase);
void register_hunk(BPTR file, BPTR hunks, APTR header,
                   const struct HunkRelocTable *relocs, struct DosLibrary *DOSBase);

#include "../../../../rom/dos/internalloadseg_aos.c"

/* As in rom/dos/internalloadseg.c: the allocation size is stored in the
   ULONG before the returned block, where kaffeine reads it. */
APTR _ilsAllocVec(SIPTR *funcarray, ULONG size, ULONG req)
{
    UBYTE *p = ilsAllocMem(size, req);

    if (!p)
        return NULL;
    *((ULONG *)p) = (ULONG)size;
    return p + sizeof(ULONG);
}

void _ilsFreeVec(SIPTR *funcarray, void *buf)
{
    UBYTE *p = (UBYTE *)buf;
    ULONG size;

    if (!buf)
        return;
    p -= sizeof(ULONG);
    size = ((ULONG *)p)[0];
    if (size)
        ilsFreeMem(p, size);
}

struct host_file
{
    const UBYTE *data;
    ULONG        size;
    ULONG        pos;
};

static LONG host_read(BPTR file, void *buffer, LONG length, struct DosLibrary *DOSBase)
{
    struct host_file *f = (struct host_file *)file;
    ULONG avail = f->size - f->pos;
    ULONG n = (ULONG)length < avail ? (ULONG)length : avail;

    memcpy(buffer, f->data + f->pos, n);
    f->pos += n;
    return (LONG)n;
}

static void *host_alloc(ULONG size, ULONG flags, struct ExecBase *base)
{
    return calloc(1, size);
}

static void host_free(void *mem, ULONG size, struct ExecBase *base)
{
    free(mem);
}

static SIPTR host_funcarray[] = {
    (SIPTR)host_read, (SIPTR)host_alloc, (SIPTR)host_free
};

/* Load a HUNK image as LoadSeg() does: the caller of InternalLoadSeg_AOS()
   has already consumed the HUNK_HEADER longword. */
BPTR host_load_hunks(const UBYTE *image, ULONG size, struct DosLibrary *DOSBase, LONG *error)
{
    struct host_file file = { image, size, 4 };
    BPTR segs;

    host_process.pr_Result2 = 0;
    segs = InternalLoadSeg_AOS((BPTR)&file, BNULL, host_funcarray, NULL, DOSBase);
    *error = (LONG)host_process.pr_Result2;
    return segs;
}

void host_unload_hunks(BPTR seglist)
{
    SIPTR *funcarray = host_funcarray;

    while (seglist)
    {
        BPTR next = *(BPTR *)BADDR(seglist);
        ilsFreeVec(BADDR(seglist));
        seglist = next;
    }
}
