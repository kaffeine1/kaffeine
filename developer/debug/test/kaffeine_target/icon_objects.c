/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    icon_objects.c - Native m68k icon.library object and tooltype regression.
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <string.h>

#include "m68ktest.h"

#ifndef ICONNAME
#define ICONNAME "icon.library"
#endif

#define CYCLE_COUNT 96

struct Library *IconBase;

static void observe_diskobj(const char *tag, const struct DiskObject *dobj)
{
    Printf("info %s: magic=0x%04lx ver=%ld type=%ld x=0x%08lx y=0x%08lx stack=%ld\n",
           (LONG)tag,
           (LONG)dobj->do_Magic,
           (LONG)dobj->do_Version,
           (LONG)dobj->do_Type,
           (LONG)dobj->do_CurrentX,
           (LONG)dobj->do_CurrentY,
           (LONG)dobj->do_StackSize);
    test_flush();
}

int main(int argc, char **argv)
{
    BOOL leave_owned = argc > 1 && !strcmp(argv[1], "leave");
    struct DiskObject *dobj_new = NULL;
    struct DiskObject *dobj_empty = NULL;
    struct DiskObject *dobj_def = NULL;
    char tt_mode[] = "MODE=one|TWO|three";
    char tt_flag[] = "FLAG";
    char tt_count[] = "COUNT=17";
    CONST_STRPTR tooltypes[4];
    STRPTR val_mode;
    STRPTR val_mode_ci;
    STRPTR val_flag;
    STRPTR val_count;
    STRPTR val_missing;
    BOOL cycles_ok = TRUE;
    LONG i;

    IconBase = OpenLibrary((CONST_STRPTR)ICONNAME, 44);
    if (!check(IconBase != NULL, "OpenLibrary icon.library v44"))
        return test_done("icon_objects");

    dobj_new = NewDiskObject(WBPROJECT);
    if (check(dobj_new != NULL, "NewDiskObject(WBPROJECT) non-NULL"))
    {
        observe_diskobj("NewDiskObject(WBPROJECT)", dobj_new);
        check_long("NewDiskObject(WBPROJECT) do_Type",
                   (LONG)dobj_new->do_Type, WBPROJECT);
    }

    /* GetDiskObject(NULL) is documented safe; GetDiskObjectNew(NULL) is not. */
    dobj_empty = GetDiskObject(NULL);
    if (check(dobj_empty != NULL, "GetDiskObject(NULL) non-NULL"))
        observe_diskobj("GetDiskObject(NULL)", dobj_empty);

    dobj_def = GetDefDiskObject(WBPROJECT);
    if (check(dobj_def != NULL, "GetDefDiskObject(WBPROJECT) non-NULL"))
        observe_diskobj("GetDefDiskObject(WBPROJECT)", dobj_def);

    /* Optional bridge-teardown probe. On native AmigaOS use the default
       mode, which explicitly frees every acquired DiskObject. */
    if (dobj_def && !leave_owned)
        FreeDiskObject(dobj_def);
    if (dobj_empty && !leave_owned)
        FreeDiskObject(dobj_empty);
    if (dobj_new && !leave_owned)
        FreeDiskObject(dobj_new);

    FreeDiskObject(NULL);
    check(TRUE, "FreeDiskObject(NULL) completed");

    for (i = 0; i < CYCLE_COUNT; i++)
    {
        struct DiskObject *iter = NewDiskObject(WBPROJECT);
        if (!iter || iter->do_Type != WBPROJECT)
        {
            if (iter)
                FreeDiskObject(iter);
            cycles_ok = FALSE;
            break;
        }
        FreeDiskObject(iter);
    }
    check(cycles_ok && i == CYCLE_COUNT,
          "96 sequential NewDiskObject/FreeDiskObject cycles");

    tooltypes[0] = (STRPTR)tt_mode;
    tooltypes[1] = (STRPTR)tt_flag;
    tooltypes[2] = (STRPTR)tt_count;
    tooltypes[3] = NULL;

    val_mode = (STRPTR)FindToolType(tooltypes, (CONST_STRPTR)"MODE");
    check(val_mode == (STRPTR)(tt_mode + 5),
          "FindToolType MODE exact guest pointer");
    check_str("FindToolType MODE value", (const char *)val_mode, "one|TWO|three");

    val_mode_ci = (STRPTR)FindToolType(tooltypes, (CONST_STRPTR)"mOdE");
    check(val_mode_ci == (STRPTR)(tt_mode + 5),
          "FindToolType mOdE case-insensitive pointer");

    val_flag = (STRPTR)FindToolType(tooltypes, (CONST_STRPTR)"flag");
    check(val_flag == (STRPTR)(tt_flag + 4),
          "FindToolType flag exact terminator pointer");
    check_str("FindToolType flag empty value", (const char *)val_flag, "");

    val_count = (STRPTR)FindToolType(tooltypes, (CONST_STRPTR)"cOuNt");
    check(val_count == (STRPTR)(tt_count + 6),
          "FindToolType cOuNt exact guest pointer");
    check_str("FindToolType cOuNt value", (const char *)val_count, "17");

    val_missing = (STRPTR)FindToolType(tooltypes, (CONST_STRPTR)" ABSENT ");
    check(val_missing == NULL, "FindToolType missing key returns NULL");

    if (val_mode && val_count)
    {
        check(MatchToolValue(val_mode, (STRPTR)"one") != FALSE,
              "MatchToolValue exact first token");
        check(MatchToolValue(val_mode, (STRPTR)"TWO") != FALSE,
              "MatchToolValue exact middle token");
        check(MatchToolValue(val_mode, (STRPTR)"three") != FALSE,
              "MatchToolValue exact last token");
        check(MatchToolValue(val_mode, (STRPTR)"two") != FALSE,
              "MatchToolValue case-insensitive middle token");
        check(MatchToolValue(val_mode, (STRPTR)"THREE") != FALSE,
              "MatchToolValue case-insensitive last token");
        check(MatchToolValue(val_mode, (STRPTR)"on") == FALSE,
              "MatchToolValue rejects prefix on");
        check(MatchToolValue(val_mode, (STRPTR)"tw") == FALSE,
              "MatchToolValue rejects prefix tw");
        check(MatchToolValue(val_mode, (STRPTR)"thre") == FALSE,
              "MatchToolValue rejects prefix thre");
        check(MatchToolValue(val_mode, (STRPTR)"four") == FALSE,
              "MatchToolValue rejects missing token");
        check(MatchToolValue(val_count, (STRPTR)"17") != FALSE,
              "MatchToolValue exact COUNT token");
        check(MatchToolValue(val_count, (STRPTR)"1") == FALSE,
              "MatchToolValue rejects COUNT prefix");
    }

    check(tooltypes[0] == (STRPTR)tt_mode &&
          tooltypes[1] == (STRPTR)tt_flag &&
          tooltypes[2] == (STRPTR)tt_count &&
          tooltypes[3] == NULL,
          "tooltype pointer array preserved");
    check_str("tt_mode string preserved", tt_mode, "MODE=one|TWO|three");
    check_str("tt_flag string preserved", tt_flag, "FLAG");
    check_str("tt_count string preserved", tt_count, "COUNT=17");

    if (leave_owned)
        check(dobj_new && dobj_empty && dobj_def,
              "three DiskObjects intentionally left for bridge teardown");
    CloseLibrary(IconBase);
    IconBase = NULL;
    return test_done("icon_objects");
}
