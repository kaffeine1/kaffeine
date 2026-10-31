/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    68k test library for the kaffeine disk-library loader and MakeLibrary()
    path. Built twice: as emu68test.library with a WORD offset function
    table, and with -DLONG_TABLE as emu68test_l.library with a table of
    LONG function pointers. Its Expunge() is that of a disk library, which
    TestExpunge() asks for at the last close. The WORD build takes its
    lib_Version, ln_Name and lib_IdString from its resident tag alone, as
    InitResident() sets them before the init; the LONG build sets them
    itself too.
*/

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/execbase.h>
#include <exec/initializers.h>
#include <dos/dos.h>
#include <stddef.h>

/* Exec through the SysBase that init stored in the base */
#define __NOLIBBASE__
#define EXEC_BASE_NAME (base->sysbase)
#include <proto/exec.h>

#ifdef LONG_TABLE
#define LIB_NAME "emu68test_l.library"
#define LIB_ID   "emu68test_l 2.5 (24.10.2026)"
#else
#define LIB_NAME "emu68test.library"
#define LIB_ID   "emu68test 2.5 (24.10.2026)"
#endif

struct TestBase
{
    struct Library lib;
    UWORD test_word;
    ULONG test_long;
    BPTR seglist;
    struct ExecBase *sysbase;
    ULONG inits;
    ULONG opens;
};

LONG _start(void)
{
    return -1;
}

struct TestBase *LibOpen(ULONG version __asm("d0"),
                         struct TestBase *base __asm("a6"))
{
    (void)version;
    base->lib.lib_OpenCnt++;
    base->opens++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

/* The library leaves the list of Exec and gives its seglist back, unless
   it is still open */
BPTR LibExpunge(struct TestBase *base __asm("a6"))
{
    BPTR seglist = base->seglist;

    if (base->lib.lib_OpenCnt)
    {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    Remove(&base->lib.lib_Node);
    FreeMem((UBYTE *)base - base->lib.lib_NegSize,
            base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seglist;
}

BPTR LibClose(struct TestBase *base __asm("a6"))
{
    if (--base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

ULONG LibNull(void)
{
    return 0;
}

LONG TestAdd(LONG a __asm("d0"), LONG b __asm("d1"),
             struct TestBase *base __asm("a6"))
{
    (void)base;
    return a + b;
}

ULONG TestInits(struct TestBase *base __asm("a6"))
{
    return base->inits;
}

ULONG TestOpens(struct TestBase *base __asm("a6"))
{
    return base->opens;
}

BPTR TestSegList(struct TestBase *base __asm("a6"))
{
    return base->seglist;
}

struct ExecBase *TestSysBase(struct TestBase *base __asm("a6"))
{
    return base->sysbase;
}

/* The expunge at the last close: the flags with LIBF_DELEXP */
ULONG TestExpunge(struct TestBase *base __asm("a6"))
{
    base->lib.lib_Flags |= LIBF_DELEXP;
    return base->lib.lib_Flags;
}

#ifndef LONG_TABLE
extern const UWORD functable[];
__asm__(
    ".text\n"
    ".even\n"
    ".globl _functable\n"
    "_functable:\n"
    ".word -1\n"
    ".word _LibOpen-_functable\n"
    ".word _LibClose-_functable\n"
    ".word _LibExpunge-_functable\n"
    ".word _LibNull-_functable\n"
    ".word _TestAdd-_functable\n"
    ".word _TestInits-_functable\n"
    ".word _TestOpens-_functable\n"
    ".word _TestSegList-_functable\n"
    ".word _TestSysBase-_functable\n"
    ".word _TestExpunge-_functable\n"
    ".word -1\n"
);
#endif

static const char lib_name[] = LIB_NAME;
static const char lib_id[] = LIB_ID;

struct TestBase *LibInit(struct TestBase *base __asm("d0"),
                         BPTR seglist __asm("a0"),
                         struct ExecBase *sysbase __asm("a6"))
{
#ifdef LONG_TABLE
    base->lib.lib_Node.ln_Name = (char *)lib_name;
    base->lib.lib_IdString = (char *)lib_id;
#endif
    base->seglist = seglist;
    base->sysbase = sysbase;
    base->inits++;
    return base;
}

#ifdef LONG_TABLE
static const APTR functable[] =
{
    LibOpen,
    LibClose,
    LibExpunge,
    LibNull,
    TestAdd,
    TestInits,
    TestOpens,
    TestSegList,
    TestSysBase,
    TestExpunge,
    (APTR)-1
};
#endif

static const UWORD data_init[] =
{
    INITBYTE(offsetof(struct TestBase, lib.lib_Node.ln_Type), NT_LIBRARY),
    INITBYTE(offsetof(struct TestBase, lib.lib_Flags), LIBF_SUMUSED | LIBF_CHANGED),
#ifdef LONG_TABLE
    INITWORD(offsetof(struct TestBase, lib.lib_Version), 2),
#endif
    INITWORD(offsetof(struct TestBase, lib.lib_Revision), 5),
    INITWORD(offsetof(struct TestBase, test_word), 0x1234),
    INITLONG(offsetof(struct TestBase, test_long), 0x89ABCDEFUL),
    0
};

static const ULONG init_table[4] =
{
    sizeof(struct TestBase),
    (ULONG)functable,
    (ULONG)data_init,
    (ULONG)LibInit
};

static const struct Resident romtag __attribute__((used)) =
{
    RTC_MATCHWORD,
    (struct Resident *)&romtag,
    (APTR)(&romtag + 1),
    RTF_AUTOINIT,
    2,
    NT_LIBRARY,
    0,
    (char *)lib_name,
    (char *)lib_id,
    (APTR)init_table
};

/* A word after the resident tag: the ramlib of AmigaOS 3.2.3 does not find
   a resident tag that ends where its hunk ends. */
static const UWORD romtag_end __attribute__((used)) = 0;
