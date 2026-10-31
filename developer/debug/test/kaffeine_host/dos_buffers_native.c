/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Native byte algorithms used as collaborators by test_dos_thunks.cpp.
    Compile as C with unsigned char, like AROS C STRPTR (UBYTE *), so the
    token bytes in patternMatch() are not sign extended by the host stub's
    char * STRPTR. The algorithms themselves are included unchanged.
*/
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dosasl.h>

#define DOS_INTERN_H
#define AROS_LHA(type, name, reg) type name
#define AROS_LH3(type, fn, a1, a2, a3, bt, bn, o, lib) type fn(a1, a2, a3)
#define AROS_LH4(type, fn, a1, a2, a3, a4, bt, bn, o, lib) type fn(a1, a2, a3, a4)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define AddPart dos_native_AddPart
#define Fault dos_native_Fault
/* This comparison belongs to the unchanged native source. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../../../rom/dos/addpart.c"
#pragma GCC diagnostic pop
#include "../../../../rom/dos/fault.c"

/* The internal marker storage/macros of rom/dos/dos_intern.h, with the
   dependencies reduced to AllocMem(), FreeMem() and ERROR(). */
struct marker
{
    UBYTE type;
    CONST_STRPTR pat;
    CONST_STRPTR str;
};
struct markerarray
{
    struct markerarray *next, *prev;
    struct marker marker[128];
};
#define PUSH(t,p,s)                                                     \
{                                                                      \
    if (macnt == 128)                                                  \
    {                                                                  \
        if (macur->next == NULL)                                       \
        {                                                              \
            macur->next = AllocMem(sizeof(struct markerarray), MEMF_ANY); \
            if (macur->next == NULL)                                   \
                ERROR(ERROR_NO_FREE_STORE);                            \
            macur->next->prev = macur;                                  \
        }                                                              \
        macur = macur->next;                                           \
        macnt = 0;                                                     \
    }                                                                  \
    macur->marker[macnt].type = (t);                                    \
    macur->marker[macnt].pat = (p);                                     \
    macur->marker[macnt].str = (s);                                     \
    macnt++;                                                           \
}
#define POP(t,p,s)                                                      \
{                                                                      \
    macnt--;                                                           \
    if (macnt < 0)                                                     \
    {                                                                  \
        macnt = 127;                                                   \
        macur = macur->prev;                                           \
        if (macur == NULL)                                             \
            ERROR(0);                                                  \
    }                                                                  \
    (t) = macur->marker[macnt].type;                                    \
    (p) = macur->marker[macnt].pat;                                     \
    (s) = macur->marker[macnt].str;                                     \
}
#define RNF_WILDSTAR 1
#define patternParse dos_native_patternParse
#define patternMatch dos_native_patternMatch
#include "../../../../rom/dos/patternmatching.c"

LONG dos_native_ParsePattern(CONST_STRPTR source, STRPTR dest, LONG size, BOOL use_case)
{
    struct RootNode root = { RNF_WILDSTAR };
    struct DosLibrary base = { .dl_Root = &root };
    return dos_native_patternParse(source, dest, size, use_case, &base);
}

BOOL dos_native_MatchPattern(CONST_STRPTR pattern, CONST_STRPTR string, BOOL use_case)
{
    return dos_native_patternMatch(pattern, string, use_case, NULL);
}

/* Date and command parsing use the unchanged AROS algorithms as the oracle.
   Their historical pointer-sign conversions model UBYTE * STRPTR on AROS. */
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <stdio.h>
LONG ToLower(LONG c);
LONG Strnicmp(CONST_STRPTR a, CONST_STRPTR b, LONG n);
#define AROS_LH1(type, fn, a1, bt, bn, o, lib) type fn(a1)
#define AROS_LH2(type, fn, a1, a2, bt, bn, o, lib) type fn(a1, a2)
#define AROS_LH5(type, fn, a1, a2, a3, a4, a5, bt, bn, o, lib) type fn(a1, a2, a3, a4, a5)
#define CompareDates dos_native_CompareDates
#define DateToStr dos_native_DateToStr
#define StrToDate dos_native_StrToDate
#define FindArg dos_native_FindArg
#define ReadItem dos_native_ReadItem
#define SplitName dos_native_SplitName
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../../../rom/dos/comparedates.c"
#include "../../../../rom/dos/datetostr.c"
#include "../../../../rom/dos/strtodate.c"
#include "../../../../rom/dos/findarg.c"
#include "../../../../rom/dos/readitem.c"
#include "../../../../rom/dos/splitname.c"
#pragma GCC diagnostic pop
