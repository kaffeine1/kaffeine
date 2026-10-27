/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "m68ktest.h"

__typeof__(UtilityBase) UtilityBase = NULL;

#define TARGET_NONE     0
#define TARGET_UTILITY  1
#define TARGET_DISKFONT 2

struct lib_case {
    const char *name;
    ULONG version;
    LONG target;
    const char *desc;
};

static const struct lib_case reg_cases[] = {
    {
        "utility.library",
        37UL,
        TARGET_UTILITY,
        "bare utility.library v37 while canonical base held"
    },
    {
        "LIBS:utility.library",
        37UL,
        TARGET_UTILITY,
        "LIBS:utility.library v37 alias matches canonical base"
    },
    {
        "LIBS:utility.library",
        37UL,
        TARGET_UTILITY,
        "repeated LIBS:utility.library v37 alias matches canonical base"
    },
    {
        "SYS:Libs/utility.library",
        37UL,
        TARGET_UTILITY,
        "SYS:Libs/utility.library v37 alias matches canonical base"
    },
    {
        "RAM:kaffeine-no-such-directory/utility.library",
        37UL,
        TARGET_UTILITY,
        "RAM: nonexistent dir utility.library v37 uses loaded list"
    },
    {
        "diskfont.library",
        34UL,
        TARGET_DISKFONT,
        "bare diskfont.library v34 while canonical base held"
    },
    {
        "LIBS:diskfont.library",
        34UL,
        TARGET_DISKFONT,
        "LIBS:diskfont.library v34 alias matches canonical base"
    },
    {
        "LIBS:diskfont.library",
        34UL,
        TARGET_DISKFONT,
        "repeated LIBS:diskfont.library v34 alias matches canonical base"
    },
    {
        "SYS:Libs/diskfont.library",
        34UL,
        TARGET_DISKFONT,
        "SYS:Libs/diskfont.library v34 alias matches canonical base"
    },
    {
        "RAM:kaffeine-no-such-directory/diskfont.library",
        34UL,
        TARGET_DISKFONT,
        "RAM: nonexistent dir diskfont.library v34 uses loaded list"
    },
    {
        "RAM:kaffeine-no-such-directory/kaffeine-unknown.library",
        0UL,
        TARGET_NONE,
        "unknown library in RAM: nonexistent dir fails cleanly"
    },
    {
        "RAM:kaffeine-no-such-directory/UTILITY.LIBRARY",
        37UL,
        TARGET_NONE,
        "case mismatch UTILITY.LIBRARY in RAM: nonexistent dir fails"
    }
};

#define REG_CASE_COUNT ((LONG)(sizeof(reg_cases) / sizeof(reg_cases[0])))

static const struct lib_case probe_cases[] = {
    { "utility.library", 37UL, TARGET_UTILITY, "bare_utility" },
    { "LIBS:utility.library", 37UL, TARGET_UTILITY, "libs_utility" },
    { "SYS:Libs/utility.library", 37UL, TARGET_UTILITY, "sys_libs_utility" },
    { "RAM:kaffeine-no-such-directory/utility.library", 37UL, TARGET_UTILITY, "ram_nosuch_utility" },
    { "diskfont.library", 34UL, TARGET_DISKFONT, "bare_diskfont" },
    { "LIBS:diskfont.library", 34UL, TARGET_DISKFONT, "libs_diskfont" },
    { "SYS:Libs/diskfont.library", 34UL, TARGET_DISKFONT, "sys_libs_diskfont" },
    { "RAM:kaffeine-no-such-directory/diskfont.library", 34UL, TARGET_DISKFONT, "ram_nosuch_diskfont" },
    { "Utility.library", 37UL, TARGET_UTILITY, "case_bare_Utility" },
    { "LIBS:Utility.library", 37UL, TARGET_UTILITY, "case_libs_Utility" },
    { "RAM:kaffeine-no-such-directory/Utility.library", 37UL, TARGET_UTILITY, "case_ram_Utility" },
    { "DiskFont.library", 34UL, TARGET_DISKFONT, "case_bare_DiskFont" },
    { "LIBS:DiskFont.library", 34UL, TARGET_DISKFONT, "case_libs_DiskFont" },
    { "LIBS:DISKFONT.LIBRARY", 34UL, TARGET_DISKFONT, "case_libs_DISKFONT" },
    { "RAM:kaffeine-no-such-directory/DiskFont.library", 34UL, TARGET_DISKFONT, "case_ram_DiskFont" },
    { "RAM:kaffeine-no-such-directory/kaffeine-unknown.library", 0UL, TARGET_NONE, "unknown_ram_subdir" },
    { "RAM:kaffeine-unknown.library", 0UL, TARGET_NONE, "unknown_ram_root" },
    { "", 0UL, TARGET_NONE, "empty_name" },
    { "LIBS:utility.library/", 37UL, TARGET_UTILITY, "trailing_slash_utility" },
    { "RAM:utility.library:", 37UL, TARGET_UTILITY, "trailing_colon_utility" }
};

#define PROBE_CASE_COUNT ((LONG)(sizeof(probe_cases) / sizeof(probe_cases[0])))

static struct Library *resolve_target(LONG target,
                                      struct Library *u_ref,
                                      struct Library *df_ref)
{
    if (target == TARGET_UTILITY) {
        return u_ref;
    }
    if (target == TARGET_DISKFONT) {
        return df_ref;
    }
    return NULL;
}

static void print_probe_record(const char *stage,
                               const char *name,
                               ULONG req_ver,
                               struct Library *got,
                               struct Library *ref_base,
                               LONG target,
                               LONG ver,
                               LONG rev,
                               LONG closed)
{
    LONG ok = (got != NULL) ? 1L : 0L;
    LONG same = -1L;

    if (target != TARGET_NONE) {
        same = (got != NULL && ref_base != NULL && got == ref_base) ? 1L : 0L;
    }

    Printf((STRPTR)"PROBE stage=%s name=\"%s\" req=%ld ok=%ld base=%lx same=%ld ver=%ld rev=%ld closed=%ld\n",
           (LONG)stage,
           (LONG)name,
           (LONG)req_ver,
           (LONG)ok,
           (LONG)got,
           (LONG)same,
           (LONG)ver,
           (LONG)rev,
           (LONG)closed);
    Flush(Output());
}

static LONG run_probe_mode(void)
{
    struct Library *u_first = NULL;
    struct Library *u_ref = NULL;
    struct Library *df_first = NULL;
    struct Library *df_ref = NULL;
    LONG u_first_ver = 0L;
    LONG u_first_rev = 0L;
    LONG u_ref_ver = 0L;
    LONG u_ref_rev = 0L;
    LONG df_first_ver = 0L;
    LONG df_first_rev = 0L;
    LONG df_ref_ver = 0L;
    LONG df_ref_rev = 0L;
    LONG i;

    PutStr((STRPTR)"PROBE_BEGIN exec_library_paths\n");
    Flush(Output());

    /*
     * Distinguish the first path open from reuse of an already-held canonical
     * reference by opening the LIBS: path before the bare reference name.
     */
    u_first = OpenLibrary((STRPTR)"LIBS:utility.library", 37UL);
    if (u_first != NULL) {
        u_first_ver = (LONG)u_first->lib_Version;
        u_first_rev = (LONG)u_first->lib_Revision;
    }
    u_ref = OpenLibrary((STRPTR)"utility.library", 37UL);
    if (u_ref != NULL) {
        u_ref_ver = (LONG)u_ref->lib_Version;
        u_ref_rev = (LONG)u_ref->lib_Revision;
    }
    if (u_first != NULL) {
        CloseLibrary(u_first);
    }
    print_probe_record("FIRST_PATH", "LIBS:utility.library", 37UL,
                       u_first, u_ref, TARGET_UTILITY,
                       u_first_ver, u_first_rev, (u_first != NULL) ? 1L : 0L);
    print_probe_record("REF_HOLD", "utility.library", 37UL,
                       u_ref, u_ref, TARGET_UTILITY,
                       u_ref_ver, u_ref_rev, 0L);

    df_first = OpenLibrary((STRPTR)"LIBS:diskfont.library", 34UL);
    if (df_first != NULL) {
        df_first_ver = (LONG)df_first->lib_Version;
        df_first_rev = (LONG)df_first->lib_Revision;
    }
    df_ref = OpenLibrary((STRPTR)"diskfont.library", 34UL);
    if (df_ref != NULL) {
        df_ref_ver = (LONG)df_ref->lib_Version;
        df_ref_rev = (LONG)df_ref->lib_Revision;
    }
    if (df_first != NULL) {
        CloseLibrary(df_first);
    }
    print_probe_record("FIRST_PATH", "LIBS:diskfont.library", 34UL,
                       df_first, df_ref, TARGET_DISKFONT,
                       df_first_ver, df_first_rev, (df_first != NULL) ? 1L : 0L);
    print_probe_record("REF_HOLD", "diskfont.library", 34UL,
                       df_ref, df_ref, TARGET_DISKFONT,
                       df_ref_ver, df_ref_rev, 0L);

    for (i = 0L; i < PROBE_CASE_COUNT; i++) {
        const struct lib_case *c = &probe_cases[i];
        struct Library *ref = resolve_target(c->target, u_ref, df_ref);
        struct Library *got = OpenLibrary((STRPTR)c->name, c->version);
        LONG ver = 0L;
        LONG rev = 0L;
        LONG closed = 0L;

        if (got != NULL) {
            ver = (LONG)got->lib_Version;
            rev = (LONG)got->lib_Revision;
            CloseLibrary(got);
            closed = 1L;
        }

        print_probe_record("REUSE", c->name, c->version,
                           got, ref, c->target, ver, rev, closed);
    }

    if (df_ref != NULL) {
        CloseLibrary(df_ref);
        print_probe_record("REF_CLOSE", "diskfont.library", 34UL,
                           df_ref, df_ref, TARGET_DISKFONT,
                           df_ref_ver, df_ref_rev, 1L);
    }
    if (u_ref != NULL) {
        CloseLibrary(u_ref);
        print_probe_record("REF_CLOSE", "utility.library", 37UL,
                           u_ref, u_ref, TARGET_UTILITY,
                           u_ref_ver, u_ref_rev, 1L);
    }

    Printf((STRPTR)"PROBE_DONE table_cases=%ld\n", (LONG)PROBE_CASE_COUNT);
    Flush(Output());
    return 0L;
}

static LONG run_regression_mode(void)
{
    struct Library *u_first = NULL;
    struct Library *u_ref = NULL;
    struct Library *df_first = NULL;
    struct Library *df_ref = NULL;
    struct Library *usable_u = NULL;
    LONG cmp_eq = -1L;
    LONG cmp_order = 0L;
    LONG i;

    /*
     * 1. Path-first opens followed by bare reference opens while path-first
     *    handles are still alive.
     */
    u_first = OpenLibrary((STRPTR)"LIBS:utility.library", 37UL);
    check(u_first != NULL, "path-first OpenLibrary LIBS:utility.library v37 succeeds");

    u_ref = OpenLibrary((STRPTR)"utility.library", 37UL);
    check(u_ref != NULL, "bare OpenLibrary utility.library v37 after path open succeeds");
    check(u_first != NULL && u_ref != NULL && u_first == u_ref,
          "path-first and bare utility.library return identical base");

    usable_u = (u_first != NULL) ? u_first : u_ref;
    if (usable_u != NULL) {
        UtilityBase = (__typeof__(UtilityBase))usable_u;
        cmp_eq = (LONG)Stricmp((STRPTR)"DiskFont.library", (STRPTR)"diskfont.library");
        cmp_order = (LONG)Stricmp((STRPTR)"amiga", (STRPTR)"zeta");
    }
    check_long("Stricmp equal strings via UtilityBase", cmp_eq, 0L);
    check(usable_u != NULL && cmp_order < 0L,
          "Stricmp ordering via UtilityBase is negative");

    if (u_first != NULL) {
        UtilityBase = (__typeof__(UtilityBase))u_ref;
        CloseLibrary(u_first);
        u_first = NULL;
    }

    df_first = OpenLibrary((STRPTR)"LIBS:diskfont.library", 34UL);
    check(df_first != NULL, "path-first OpenLibrary LIBS:diskfont.library v34 succeeds");

    df_ref = OpenLibrary((STRPTR)"diskfont.library", 34UL);
    check(df_ref != NULL, "bare OpenLibrary diskfont.library v34 after path open succeeds");
    check(df_first != NULL && df_ref != NULL && df_first == df_ref,
          "path-first and bare diskfont.library return identical base");

    if (df_first != NULL) {
        CloseLibrary(df_first);
        df_first = NULL;
    }

    /*
     * 2. Alias table checks while canonical reference bases u_ref and df_ref
     *    remain open.
     */
    for (i = 0L; i < REG_CASE_COUNT; i++) {
        const struct lib_case *c = &reg_cases[i];
        struct Library *expected = resolve_target(c->target, u_ref, df_ref);
        struct Library *got = OpenLibrary((STRPTR)c->name, c->version);

        if (c->target == TARGET_NONE) {
            check(got == NULL, c->desc);
        } else {
            check(expected != NULL && got == expected, c->desc);
        }

        if (got != NULL) {
            CloseLibrary(got);
        }
    }

    UtilityBase = NULL;
    if (df_ref != NULL) {
        CloseLibrary(df_ref);
    }
    if (u_ref != NULL) {
        CloseLibrary(u_ref);
    }

    return test_done("exec_library_paths");
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != NULL && argv[1] != NULL && argv[1][0] != '\0') {
        return (int)run_probe_mode();
    }
    return (int)run_regression_mode();
}
