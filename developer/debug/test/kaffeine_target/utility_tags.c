/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    utility.library tag operations and string comparisons on data in 68k
    memory, called from a libnix C program that uses neither
    stdio nor malloc. The items must come from the program's own list, also
    behind TAG_MORE, with their ti_Data unchanged: a value such as 8192 is a
    size here even though it lies in the range of guest addresses. A list
    that loops must be searched up to the last item of the loop.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "m68ktest.h"

struct Library *UtilityBase;

static struct TagItem more[] =
{
    { TAG_USER | 4, 44 },
    { TAG_USER | 5, 8192 },
    { TAG_DONE, 0 }
};

static struct TagItem list[] =
{
    { TAG_USER | 1, 11 },
    { TAG_IGNORE, TAG_USER | 4 },   /* its ti_Data is not a tag */
    { TAG_SKIP, 1 },
    { TAG_USER | 4, 99 },           /* passed over by TAG_SKIP */
    { TAG_USER | 2, 8192 },
    { TAG_MORE, (ULONG)more }
};

/* Two items before a ring of four items and a TAG_MORE back to its start */
static struct TagItem ring[] =
{
    { TAG_USER | 10, 0 },
    { TAG_USER | 11, 0 },
    { TAG_USER | 12, 0 },
    { TAG_USER | 13, 0 },
    { TAG_USER | 14, 0 },
    { TAG_USER | 15, 0 },
    { TAG_MORE, (ULONG)&ring[2] }
};

static BOOL same(CONST_STRPTR what, struct TagItem *got, struct TagItem *expected)
{
    return check_long(what, (LONG)got, (LONG)expected);
}

static void tag_operations(void)
{
    struct TagItem tail[] = {
        { TAG_USER | 2, 0 }, { TAG_USER | 1, 0 },
        { TAG_USER | 3, 8192 }, { TAG_DONE, 0 }
    };
    struct TagItem source[] = {
        { TAG_USER | 1, 1 }, { TAG_IGNORE, 99 }, { TAG_SKIP, 1 },
        { TAG_USER | 3, 999 }, { TAG_MORE, (ULONG)tail }
    };
    struct TagItem bool_map[] = {
        { TAG_USER | 1, 3 }, { TAG_USER | 2, 0x80000004 },
        { TAG_USER | 3, 16 }, { TAG_DONE, 0 }
    };
    struct TagItem map_tail[] = {
        { TAG_USER | 2, TAG_DONE }, { TAG_USER | 3, TAG_USER | 4 }, { TAG_DONE, 0 }
    };
    struct TagItem map[] = {
        { TAG_USER | 1, TAG_USER | 5 }, { TAG_MORE, (ULONG)map_tail }
    };
    struct TagItem original[] = {
        { TAG_USER | 1, 1 }, { TAG_USER | 2, 55 }, { TAG_USER | 3, 8192 }, { TAG_DONE, 0 }
    };
    struct TagItem changes[] = {
        { TAG_USER | 1, 1 }, { TAG_USER | 2, 0 }, { TAG_USER | 4, 44 }, { TAG_DONE, 0 }
    };
    Tag filter[] = { TAG_USER | 1, TAG_USER | 3, TAG_DONE };
    Tag values[] = { TAG_USER | 1, TAG_IGNORE, TAG_MORE, TAG_SKIP, TAG_DONE, TAG_USER | 4 };
    struct TagItem *tags, *clone, *empty;
    ULONG i;

    check_long("PackBoolTags() follows TAG_MORE and the last duplicate wins",
               PackBoolTags(0x80000000, source, bool_map), 16);
    check_long("PackBoolTags() with no list preserves initial flags",
               PackBoolTags(123, NULL, bool_map), 123);
    check_long("TagInArray() finds a contiguous ULONG value", TagInArray(TAG_USER | 1, values), TRUE);
    check_long("TagInArray() treats TAG_MORE as an array value", TagInArray(TAG_MORE, values), TRUE);
    check_long("TagInArray() does not match TAG_DONE", TagInArray(TAG_DONE, values), FALSE);
    check_long("TagInArray() stops at TAG_DONE", TagInArray(TAG_USER | 4, values), FALSE);

    FilterTagChanges(changes, original, FALSE);
    check_long("FilterTagChanges() removes equal tags", changes[0].ti_Tag, TAG_IGNORE);
    check_long("FilterTagChanges(FALSE) preserves different original data", original[1].ti_Data, 55);
    check_long("FilterTagChanges() leaves unmatched changes", changes[2].ti_Tag, TAG_USER | 4);
    FilterTagChanges(changes, original, TRUE);
    check_long("FilterTagChanges(TRUE) applies different data", original[1].ti_Data, 0);
    FilterTagChanges(changes, original, TRUE);
    check_long("FilterTagChanges() removes a change after it was applied", changes[1].ti_Tag, TAG_IGNORE);

    same("AllocateTagItems(0) returns NULL", AllocateTagItems(0), NULL);
    tags = AllocateTagItems(4);
    if (check(tags != NULL, "AllocateTagItems() returns guest memory"))
    {
        BOOL cleared = TRUE;
        for (i = 0; i < 4; i++)
            if (tags[i].ti_Tag || tags[i].ti_Data) cleared = FALSE;
        check(cleared, "AllocateTagItems() clears exactly four tag/data pairs");
        tags[0].ti_Tag = TAG_USER | 1;
        tags[0].ti_Data = 8192;
        tags[1].ti_Tag = TAG_DONE;
        check_long("allocated tag memory can be used by GetTagData()",
                   GetTagData(TAG_USER | 1, 0, tags), 8192);
        FreeTagItems(tags);
        FreeTagItems(tags);
    }
    empty = CloneTagItems(NULL);
    if (check(empty != NULL, "CloneTagItems(NULL) allocates an empty list"))
    {
        check_long("the empty clone has TAG_DONE", empty[0].ti_Tag, TAG_DONE);
        check_long("the empty clone has cleared ti_Data", empty[0].ti_Data, 0);
        FreeTagItems(empty);
    }
    clone = CloneTagItems(source);
    if (check(clone != NULL, "CloneTagItems() copies a guest chain"))
    {
        check_long("clone first tag", clone[0].ti_Tag, TAG_USER | 1);
        check_long("clone passes over TAG_IGNORE and TAG_SKIP", clone[1].ti_Tag, TAG_USER | 2);
        check_long("clone keeps duplicate tags", clone[2].ti_Tag, TAG_USER | 1);
        check_long("clone keeps scalar data 8192", clone[3].ti_Data, 8192);
        check_long("clone appends a cleared terminator", clone[4].ti_Tag | clone[4].ti_Data, 0);

        check_long("FilterTagItems(AND) counts surviving tags", FilterTagItems(clone, filter, TAGFILTER_AND), 3);
        check_long("AND removes tags outside the array", clone[1].ti_Tag, TAG_IGNORE);
        clone[3].ti_Data = 9;
        clone[4].ti_Data = 0x12345678;
        RefreshTagItemClones(clone, source);
        check_long("RefreshTagItemClones() restores a removed tag", clone[1].ti_Tag, TAG_USER | 2);
        check_long("RefreshTagItemClones() restores data", clone[3].ti_Data, 8192);
        check_long("RefreshTagItemClones() leaves the terminator data alone", clone[4].ti_Data, 0x12345678);
        check_long("FilterTagItems(NOT) counts surviving tags", FilterTagItems(clone, filter, TAGFILTER_NOT), 1);
        check_long("NOT removes tags in the array", clone[0].ti_Tag, TAG_IGNORE);
        RefreshTagItemClones(clone, source);
        check_long("unknown filter logic returns zero", FilterTagItems(clone, filter, 2), 0);
        check_long("unknown filter logic preserves the list", clone[0].ti_Tag, TAG_USER | 1);

        MapTags(clone, map, MAP_KEEP_NOT_FOUND);
        check_long("MapTags() replaces matching tags", clone[0].ti_Tag, TAG_USER | 5);
        check_long("MapTags() converts a TAG_DONE mapping to TAG_IGNORE", clone[1].ti_Tag, TAG_IGNORE);
        check_long("MapTags() follows the map chain", clone[3].ti_Tag, TAG_USER | 4);
        check_long("MapTags() preserves data", clone[3].ti_Data, 8192);
        MapTags(clone, NULL, MAP_KEEP_NOT_FOUND);
        check_long("MAP_KEEP_NOT_FOUND keeps an unmapped tag", clone[0].ti_Tag, TAG_USER | 5);
        MapTags(clone, NULL, MAP_REMOVE_NOT_FOUND);
        check_long("MAP_REMOVE_NOT_FOUND removes an unmapped tag", clone[0].ti_Tag, TAG_IGNORE);
        RefreshTagItemClones(clone, source);
        RefreshTagItemClones(clone, NULL);
        check_long("RefreshTagItemClones(NULL original) writes TAG_DONE", clone[0].ti_Tag, TAG_DONE);
        check_long("NULL original refresh preserves data", clone[0].ti_Data, 1);
        FreeTagItems(clone);
    }
    FreeTagItems(NULL);
    FreeTagItems(source);
    check_long("FreeTagItems() ignores a caller-owned list", source[0].ti_Tag, TAG_USER | 1);
    tags = (struct TagItem *)AllocVec(16, MEMF_PUBLIC | MEMF_CLEAR);
    if (check(tags != NULL, "allocate a foreign guest block"))
    {
        tags[0].ti_Tag = TAG_USER | 7;
        FreeTagItems(tags);
        check_long("FreeTagItems() ignores ordinary AllocVec memory", tags[0].ti_Tag, TAG_USER | 7);
        FreeVec(tags);
    }
    same("CloneTagItems() rejects a ring", CloneTagItems(ring), NULL);
}

static void string_comparisons(void)
{
    static const char upper[] = { (char)0xC0, (char)0xD6, (char)0xD8, (char)0xDE, 0 };
    static const char lower[] = { (char)0xE0, (char)0xF6, (char)0xF8, (char)0xFE, 0 };
    static const char multiply[] = { (char)0xD7, 0 };
    static const char divide[] = { (char)0xF7, 0 };

    check_long("Stricmp() folds ASCII case", Stricmp("AmIgA", "aMiGa"), 0);
    check_long("Stricmp() returns the lowercase byte difference", Stricmp("A", "D"), -3);
    check_long("Stricmp() compares an empty string", Stricmp("", "Z"), -122);
    check_long("Stricmp() uses the AROS extended byte case rules", Stricmp(upper, lower), 0);
    check_long("Stricmp() does not fold D7 into F7", Stricmp(multiply, divide), -32);
    check_long("Strnicmp() stops at the length", Strnicmp("Abx", "aBy", 2), 0);
    check_long("Strnicmp() returns the byte difference", Strnicmp("Abx", "aBy", 3), -1);
    check_long("Strnicmp() stops at NUL", Strnicmp("Ab", "aB", 200), 0);
    check_long("Strnicmp() with a negative LONG reaches NUL", Strnicmp("Ab", "aB", -1), 0);
    check_long("Strnicmp() with zero length reads no pointers", Strnicmp(NULL, NULL, 0), 0);
    check_long("Stricmp() returns zero for an invalid guest pointer",
               Stricmp((CONST_STRPTR)0xFFFFFFFF, "abc"), 0);
    check_long("Strnicmp() returns zero for an invalid guest pointer",
               Strnicmp("abc", (CONST_STRPTR)0xFFFFFFFF, 1), 0);
}

int main(void)
{
    struct TagItem *item, *state;

    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37);
    if (!check(UtilityBase != NULL, "OpenLibrary() of utility.library"))
        return test_done("utility_tags");

    same("FindTagItem() returns the first item of the list",
         FindTagItem(TAG_USER | 1, list), &list[0]);
    item = FindTagItem(TAG_USER | 2, list);
    if (same("FindTagItem() passes over TAG_IGNORE and TAG_SKIP", item, &list[4]))
        check_long("the item keeps ti_Data 8192", (LONG)item->ti_Data, 8192);
    item = FindTagItem(TAG_USER | 4, list);
    if (same("FindTagItem() follows TAG_MORE, not into the skipped item", item, &more[0]))
        check_long("the item behind TAG_MORE keeps its ti_Data", (LONG)item->ti_Data, 44);
    same("FindTagItem() finds the second item behind TAG_MORE",
         FindTagItem(TAG_USER | 5, list), &more[1]);
    same("FindTagItem() returns NULL for a tag that is not there",
         FindTagItem(TAG_USER | 6, list), NULL);
    same("FindTagItem() never returns TAG_IGNORE", FindTagItem(TAG_IGNORE, list), NULL);
    same("FindTagItem() of a NULL list", FindTagItem(TAG_USER | 1, NULL), NULL);

    /* AROS NextTagItem() reaches every item of the ring before it repeats
       one. Without the tag, AROS would loop for ever; the bridge returns
       NULL once it has compared every item. */
    same("FindTagItem() finds the last item of a ring behind two items",
         FindTagItem(TAG_USER | 15, ring), &ring[5]);
    same("FindTagItem() of a tag that is not in the ring returns NULL",
         FindTagItem(TAG_USER | 16, ring), NULL);

    check_long("GetTagData() returns ti_Data of the first item",
               (LONG)GetTagData(TAG_USER | 1, 777, list), 11);
    check_long("GetTagData() passes over TAG_IGNORE and TAG_SKIP and keeps 8192",
               (LONG)GetTagData(TAG_USER | 2, 777, list), 8192);
    check_long("GetTagData() follows TAG_MORE, not into the skipped item",
               (LONG)GetTagData(TAG_USER | 4, 777, list), 44);
    check_long("GetTagData() keeps 8192 behind TAG_MORE",
               (LONG)GetTagData(TAG_USER | 5, 777, list), 8192);
    check_long("GetTagData() returns defaultVal for a missing tag",
               (LONG)GetTagData(TAG_USER | 6, 777, list), 777);
    check_long("GetTagData() never matches TAG_IGNORE",
               (LONG)GetTagData(TAG_IGNORE, 777, list), 777);
    check_long("GetTagData() of a NULL list returns defaultVal",
               (LONG)GetTagData(TAG_USER | 1, 777, NULL), 777);
    check_long("GetTagData() returns a zero ti_Data instead of defaultVal",
               (LONG)GetTagData(TAG_USER | 15, 777, ring), 0);
    check_long("GetTagData() of a tag that is not in the ring returns defaultVal",
               (LONG)GetTagData(TAG_USER | 16, 777, ring), 777);

    state = list;
    same("NextTagItem() returns the first item of the list",
         NextTagItem(&state), &list[0]);
    same("NextTagItem() advances the variable after the first item",
         state, &list[1]);
    item = NextTagItem(&state);
    if (same("NextTagItem() passes over TAG_IGNORE and TAG_SKIP", item, &list[4]))
        check_long("NextTagItem() keeps ti_Data 8192", (LONG)item->ti_Data, 8192);
    same("NextTagItem() advances the variable to TAG_MORE", state, &list[5]);
    same("NextTagItem() follows TAG_MORE into the next list",
         NextTagItem(&state), &more[0]);
    same("NextTagItem() advances the variable inside the next list",
         state, &more[1]);
    item = NextTagItem(&state);
    if (same("NextTagItem() returns the second item behind TAG_MORE", item, &more[1]))
        check_long("NextTagItem() keeps ti_Data 8192 behind TAG_MORE",
                   (LONG)item->ti_Data, 8192);
    same("NextTagItem() advances the variable to TAG_DONE", state, &more[2]);
    same("NextTagItem() returns NULL at TAG_DONE", NextTagItem(&state), NULL);
    same("NextTagItem() clears the variable to NULL at TAG_DONE", state, NULL);
    same("NextTagItem() on a NULL variable stays NULL", NextTagItem(&state), NULL);

    item = FindTagItem(TAG_USER | 1, list);
    if (item)
    {
        item->ti_Data = 123;
        check_long("writing through the result changes the program's list",
                   (LONG)list[0].ti_Data, 123);
        check_long("GetTagData() sees the updated ti_Data in the program's list",
                   (LONG)GetTagData(TAG_USER | 1, 777, list), 123);
    }

    tag_operations();
    string_comparisons();
    CloseLibrary(UtilityBase);
    return test_done("utility_tags");
}
