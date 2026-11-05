/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The struct Library header of each library base a program opens first
    thing, read right after its own OpenLibrary(), before any call of that
    library: ln_Type NT_LIBRARY, a version above 0, a lib_NegSize that
    holds the standard vectors and a lib_PosSize that holds a struct
    Library. Classic programs choose their code paths by lib_Version (a
    base of version 0 is taken for Kickstart 1.x), so the header must be
    there at the first open and stay as it is: after all the later opens,
    for a second open from the cache and an open that asks for exactly its
    version, after LockIBase()/UnlockIBase() and after a close and a new
    open. The extents [base - lib_NegSize, base + lib_PosSize) of all bases
    overlap none of the others.

    Each base prints "info <name> <version>.<revision>". Under Kaffeine
    these are compared with what library_versions (kaffeine_native) prints
    for the native libraries of the same system, which they must equal.
    Whether a base is opened for a version above its own is printed as
    info too: Kaffeine's native-backed bases refuse no version yet.
*/
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;

struct opened
{
    const char *name;
    struct Library *base;
    UBYTE type;
    UWORD neg, pos, version, revision;
};

/* intuition.library first, read at once, as PPaint does; then one
   library for each way Kaffeine makes a native-backed base: graphics,
   asl, workbench, mathffp and the three mathieee libraries have their own,
   utility, gadtools, layers and icon the generated tables. With ExecBase
   and DOSBase that is 14 of the 16 bases the library cache of a Kaffeine
   program holds (KAFFEINE_MAX_LIBMAP), so 2 are left for what the
   startup may open; a base beyond the cache is not found again by a
   second open, which this program does not test. library_versions
   (kaffeine_native) prints all native libraries. */
static struct opened libs[] =
{
    { "intuition.library" }, { "graphics.library" }, { "utility.library" },
    { "gadtools.library" }, { "layers.library" }, { "icon.library" },
    { "asl.library" }, { "workbench.library" }, { "mathffp.library" },
    { "mathieeedoubbas.library" }, { "mathieeedoubtrans.library" },
    { "mathieeesingbas.library" }
};

#define LIB_COUNT ((LONG)(sizeof(libs) / sizeof(libs[0])))

/* The check text: the library name, then what is checked. */
static char text[128];

static const char *what(const char *name, const char *checked)
{
    char *out = text, *end = text + sizeof(text) - 1;

    while (*name && out < end)
        *out++ = *name++;
    while (*checked && out < end)
        *out++ = *checked++;
    *out = 0;
    return text;
}

static void record(struct opened *o)
{
    o->type = o->base->lib_Node.ln_Type;
    o->neg = o->base->lib_NegSize;
    o->pos = o->base->lib_PosSize;
    o->version = o->base->lib_Version;
    o->revision = o->base->lib_Revision;
}

static void check_header(const struct opened *o)
{
    check(o->type == NT_LIBRARY, what(o->name, " ln_Type is NT_LIBRARY"));
    check(o->version > 0, what(o->name, " lib_Version is above 0"));
    check(o->neg >= 4 * LIB_VECTSIZE && !(o->neg & 1),
          what(o->name, " lib_NegSize holds the standard vectors"));
    check(o->pos >= (UWORD)sizeof(struct Library),
          what(o->name, " lib_PosSize holds a struct Library"));
}

/* The header of the base is still the one recorded. */
static BOOL same_header(const struct opened *o, const struct Library *base)
{
    return base->lib_Node.ln_Type == o->type && base->lib_NegSize == o->neg &&
           base->lib_PosSize == o->pos && base->lib_Version == o->version &&
           base->lib_Revision == o->revision;
}

static void info_version(const char *name, UWORD version, UWORD revision)
{
    Printf("info %s %ld.%ld\n", (LONG)name, (LONG)version, (LONG)revision);
    test_flush();
}

int main(void)
{
    struct opened special[2];
    struct opened *all[LIB_COUNT + 2];
    LONG i, j, count = 0;

    /* Each base is read right after its own first open. */
    for (i = 0; i < LIB_COUNT; i++)
    {
        struct opened *o = &libs[i];

        o->base = OpenLibrary((CONST_STRPTR)o->name, 0);
        if (!check(o->base != NULL, what(o->name, " opened")))
            continue;
        record(o);
        check_header(o);
        info_version(o->name, o->version, o->revision);
        all[count++] = o;
    }

    /* ExecBase and DOSBase have headers of their own owners: only their
       versions are checked, the rest is info. */
    special[0].name = "exec.library";
    special[0].base = &SysBase->LibNode;
    special[1].name = "dos.library";
    special[1].base = &DOSBase->dl_lib;
    for (i = 0; i < 2; i++)
    {
        record(&special[i]);
        check(special[i].version > 0, what(special[i].name, " lib_Version is above 0"));
        info_version(special[i].name, special[i].version, special[i].revision);
        Printf("info %s ln_Type=%ld lib_NegSize=%ld lib_PosSize=%ld\n",
               (LONG)special[i].name, (LONG)special[i].type,
               (LONG)special[i].neg, (LONG)special[i].pos);
        test_flush();
        all[count++] = &special[i];
    }

    /* No base made later wrote into an earlier one, and no extent overlaps
       another: the jump table of each base starts where the positive area
       of the one before it ends, if not further. */
    for (i = 0; i < count; i++)
    {
        const struct opened *o = all[i];
        ULONG low = (ULONG)o->base - o->neg, high = (ULONG)o->base + o->pos;
        BOOL apart = TRUE;

        check(same_header(o, o->base), what(o->name, " header kept after the later opens"));
        for (j = 0; j < count; j++)
        {
            const struct opened *p = all[j];
            ULONG other_low = (ULONG)p->base - p->neg, other_high = (ULONG)p->base + p->pos;

            if (j != i && low < other_high && other_low < high)
                apart = FALSE;
        }
        check(apart, what(o->name, " extent overlaps no other base"));
    }

    /* A second open finds the same base with the same header, and so does
       an open of exactly its version. */
    for (i = 0; i < count - 2; i++)
    {
        const struct opened *o = all[i];
        struct Library *again = OpenLibrary((CONST_STRPTR)o->name, 0);

        check(again == o->base, what(o->name, " second open finds the same base"));
        if (again)
        {
            check(same_header(o, again), what(o->name, " second open keeps the header"));
            CloseLibrary(again);
        }
        again = OpenLibrary((CONST_STRPTR)o->name, o->version);
        check(again == o->base, what(o->name, " open of its own version finds the same base"));
        if (again)
            CloseLibrary(again);
        again = OpenLibrary((CONST_STRPTR)o->name, (ULONG)o->version + 1);
        Printf("info %s version %ld: %s\n", (LONG)o->name, (LONG)o->version + 1,
               (LONG)(again ? "opened" : "refused"));
        test_flush();
        if (again)
            CloseLibrary(again);
    }

    /* LockIBase() of Kaffeine writes the version of IntuitionBase again:
       it must be the same. */
    if (libs[0].base)
    {
        ULONG lock;

        IntuitionBase = (struct IntuitionBase *)libs[0].base;
        lock = LockIBase(0);
        UnlockIBase(lock);
        check(same_header(&libs[0], libs[0].base),
              "intuition.library LockIBase()/UnlockIBase() keep the header");
        IntuitionBase = NULL;
    }

    /* Closed and opened again, each library has the same header; whether
       it is the same base is info, as a library may be expunged. */
    for (i = 0; i < count - 2; i++)
    {
        struct opened *o = all[i];
        struct Library *closed = o->base, *again;

        CloseLibrary(closed);
        again = OpenLibrary((CONST_STRPTR)o->name, 0);
        o->base = NULL;
        if (!check(again != NULL, what(o->name, " opened again after its close")))
            continue;
        check(same_header(o, again), what(o->name, " header the same after a close"));
        Printf("info %s same base after a close: %ld\n", (LONG)o->name,
               (LONG)(again == closed));
        test_flush();
        o->base = again;
    }

    for (i = 0; i < LIB_COUNT; i++)
        if (libs[i].base)
            CloseLibrary(libs[i].base);
    return test_done("exec_library_metadata");
}
