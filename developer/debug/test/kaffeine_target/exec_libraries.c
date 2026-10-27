/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    OpenLibrary() and CloseLibrary() of 68k disk libraries built from
    emu68test_lib.c: emu68test.library with a WORD offset function table
    and emu68test_l.library with a LONG pointer function table. Each goes
    through the same life, as on AmigaOS 3.2.3: opened, opened again from
    the cache, refused for a newer version, which asks the open library
    to go, expunged at its last close, loaded again by the next open,
    closed and kept, refused again for a newer version, which leaves the
    closed library as it is, and expunged at the end.
*/

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>

#include "m68ktest.h"

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

/* Four standard vectors (Open, Close, Expunge, Null) and six library
   vectors (TestAdd, TestInits, TestOpens, TestSegList, TestSysBase,
   TestExpunge), six bytes per jump table slot, rounded up to a multiple
   of four. */
#define TEST_FUNC_COUNT  10
#define EXPECTED_NEGSIZE (((TEST_FUNC_COUNT * LIB_VECTSIZE) + 3) & ~3)

static struct TestBase *TestBase;

#define TestAdd(a, b) \
    LP2(0x1e, LONG, TestAdd, LONG, a, d0, LONG, b, d1, , TestBase)
#define TestInits() \
    LP0(0x24, ULONG, TestInits, , TestBase)
#define TestOpens() \
    LP0(0x2a, ULONG, TestOpens, , TestBase)
#define TestSegList() \
    LP0(0x30, BPTR, TestSegList, , TestBase)
#define TestSysBase() \
    LP0(0x36, struct ExecBase *, TestSysBase, , TestBase)
#define TestExpunge() \
    LP0(0x3c, ULONG, TestExpunge, , TestBase)

/* The checks of the LONG table library say so */
static BOOL long_table;
#define MSG(text) (long_table ? "long table " text : text)

/* The first life of a library: the fields that MakeLibrary() and
   InitStruct() set, its functions and a second open of the cached
   library, then a version that it is too old for, which asks the open
   library to go through its Expunge(), as AmigaOS 3.2.3 does; the last
   close then expunges it, and the name alone finds none, since the
   library is not in LIBS: */
static void first_life(const char *path, const char *name, const char *id)
{
    struct TestBase *base, *second, *third;
    BPTR seglist;
    ULONG opens;

    TestBase = base = (struct TestBase *)OpenLibrary((CONST_STRPTR)path, 2);
    if (!check(base != NULL, MSG("OpenLibrary() from PROGDIR:, version 2")))
        return;

    check_long(MSG("lib_Version"), base->lib.lib_Version, 2);
    check_long(MSG("lib_Revision"), base->lib.lib_Revision, 5);
    check_long(MSG("ln_Type"), base->lib.lib_Node.ln_Type, NT_LIBRARY);
    check_str(MSG("ln_Name"), base->lib.lib_Node.ln_Name, name);
    check_str(MSG("lib_IdString"), (const char *)base->lib.lib_IdString, id);
    check_long(MSG("test_word"), base->test_word, 0x1234);
    check_long(MSG("test_long"), (LONG)base->test_long, (LONG)0x89ABCDEFUL);
    check_long(MSG("lib_NegSize"), base->lib.lib_NegSize, EXPECTED_NEGSIZE);
    check_long(MSG("lib_PosSize"), base->lib.lib_PosSize, sizeof(struct TestBase));
    check_long(MSG("lib_OpenCnt"), base->lib.lib_OpenCnt, 1);

    check_long(MSG("TestAdd(2, 3)"), TestAdd(2, 3), 5);
    check_long(MSG("TestAdd(-7, 7)"), TestAdd(-7, 7), 0);
    check_long(MSG("TestInits()"), TestInits(), 1);
    /* each run ends in the expunges, but AmigaOS would keep the library
       of an earlier run that stopped before them, with its opens */
    opens = TestOpens();
    check(opens >= 1, MSG("TestOpens() counts this open"));
    check(TestSysBase() == SysBase, MSG("TestSysBase() equals SysBase"));

    seglist = TestSegList();
    check(seglist != 0, MSG("TestSegList() is non-zero"));
    check((seglist ? *((ULONG *)BADDR(seglist) - 1) : 0) >= 8,
          MSG("first hunk size before BADDR(seglist) is at least 8"));

    second = (struct TestBase *)OpenLibrary((CONST_STRPTR)name, 2);
    check(second == base, MSG("OpenLibrary() of the name alone returns the same base"));
    check_long(MSG("lib_OpenCnt after second open"), base->lib.lib_OpenCnt, 2);
    check_long(MSG("TestInits() after second open"), TestInits(), 1);
    check_long(MSG("TestOpens() after second open"), TestOpens(), opens + 1);

    /* Exec takes the low word of D0 as the version */
    third = (struct TestBase *)OpenLibrary((CONST_STRPTR)name, 0x12340002);
    check(third == base, MSG("OpenLibrary() takes the low word of D0 as the version"));
    if (third)
        CloseLibrary((struct Library *)third);

    check(OpenLibrary((CONST_STRPTR)name, 3) == NULL, MSG("OpenLibrary() of version 3 returns NULL"));
    check_long(MSG("lib_OpenCnt after version 3 open"), base->lib.lib_OpenCnt, 2);
    check((base->lib.lib_Flags & LIBF_DELEXP) != 0,
          MSG("version 3 asks the open library to go: LIBF_DELEXP"));

    CloseLibrary((struct Library *)base);
    check_long(MSG("lib_OpenCnt after first CloseLibrary()"), base->lib.lib_OpenCnt, 1);
    /* the last close expunges it: base is gone */
    CloseLibrary((struct Library *)base);
    check(OpenLibrary((CONST_STRPTR)name, 2) == NULL,
          MSG("after the last close the name alone finds none"));
}

/* Loaded afresh after that expunge */
static struct TestBase *reload(const char *path)
{
    struct TestBase *base;

    TestBase = base = (struct TestBase *)OpenLibrary((CONST_STRPTR)path, 2);
    if (!check(base != NULL, MSG("OpenLibrary() after the expunge loads the library again")))
        return NULL;
    check_long(MSG("TestInits() after the expunge"), TestInits(), 1);
    check_long(MSG("TestOpens() after the expunge"), TestOpens(), 1);
    check_long(MSG("lib_OpenCnt after the expunge"), base->lib.lib_OpenCnt, 1);
    check((base->lib.lib_Flags & LIBF_DELEXP) == 0, MSG("no LIBF_DELEXP after the expunge"));
    check(TestSegList() != 0, MSG("TestSegList() after the expunge is non-zero"));
    return base;
}

/* Closed, which leaves the library cached, and asked for a newer version,
   which leaves a closed library as it is; then opened again from the
   cache and expunged at its last close, which TestExpunge() asks for, so
   that a later run finds none */
static void last_life(struct TestBase *base, const char *name)
{
    struct TestBase *again;

    TestBase = base;
    CloseLibrary((struct Library *)base);
    check_long(MSG("lib_OpenCnt after the close"), base->lib.lib_OpenCnt, 0);
    check(OpenLibrary((CONST_STRPTR)name, 3) == NULL,
          MSG("OpenLibrary() of version 3 of the closed library returns NULL"));
    check((base->lib.lib_Flags & LIBF_DELEXP) == 0,
          MSG("version 3 leaves the closed library as it is"));

    again = (struct TestBase *)OpenLibrary((CONST_STRPTR)name, 2);
    if (!check(again == base, MSG("OpenLibrary() of the name alone returns the cached base")))
    {
        if (again)
            CloseLibrary((struct Library *)again);
        return;
    }
    check_long(MSG("TestOpens() of the cached library"), TestOpens(), 2);
    check((TestExpunge() & LIBF_DELEXP) != 0, MSG("TestExpunge() sets LIBF_DELEXP"));
    /* the expunge: base is gone */
    CloseLibrary((struct Library *)base);
    check(OpenLibrary((CONST_STRPTR)name, 2) == NULL,
          MSG("after the expunge the name alone finds none"));
}

int main(void)
{
    struct TestBase *word, *other;

    first_life("PROGDIR:emu68test.library", "emu68test.library", "emu68test 2.5 (24.10.2026)");
    long_table = TRUE;
    first_life("PROGDIR:emu68test_l.library", "emu68test_l.library",
               "emu68test_l 2.5 (24.10.2026)");

    long_table = FALSE;
    word = reload("PROGDIR:emu68test.library");
    long_table = TRUE;
    other = reload("PROGDIR:emu68test_l.library");
    if (word && other)
        check(word != other, "the two libraries have bases of their own");

    long_table = FALSE;
    if (word)
        last_life(word, "emu68test.library");
    long_table = TRUE;
    if (other)
        last_life(other, "emu68test_l.library");
    return test_done("exec_libraries");
}
