/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    utility.library for 68k callers.

    Integer math: on the m68k these functions return a second result in
    D1. SDivMod32() and UDivMod32() return the quotient in D0 and the
    remainder in D1, and SMult64() and UMult64() return the high 32 bits of
    the product in D0 and the low 32 bits in D1. The native AROS functions
    return one C value instead, so these thunks compute both results
    themselves.

    Tag lists and strings stay in checked guest memory. Tag items are
    always two big-endian longwords, never native pointers or structures.
*/

#include <stdint.h>
#include <string.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <utility/date.h>
#include <proto/utility.h>

#include "kaffeine_intern.h"
#include "kaffeine_taglist.h"
#include "kaffeine_thunks_utility.h"

static ULONG utility_find_tag(struct KaffeineContext *ctx, ULONG tag, ULONG list)
{
    struct KaffeineTagWalk walk;
    ULONG item;

    Kaffeine_TagWalkInit(&walk, list);
    while (Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM)
        if (m68k_read32(ctx, item) == tag)
            return item;
    return 0;
}

/* -30: FindTagItem(D0=tagVal, A0=tagList) -> D0=the guest item with that tag */
IPTR kaffeine_utility_FindTagItem(struct KaffeineContext *ctx, void *cpu)
{
    /* AROS FindTagItem() returns the first item that NextTagItem() yields
       with that tag, so the caller gets a pointer into its own list, also
       behind TAG_MORE. Search the guest list in place: nothing is copied,
       converted or freed, and the result stays valid as long as the
       caller's list. A list that leaves guest memory ends the search
       with NULL, and so does a list that loops, once every item of the
       loop has been compared. */
    return (IPTR)utility_find_tag(ctx, THUNK_D(0), THUNK_A(0));
}

/* -36: GetTagData(D0=tagValue, D1=defaultVal, A0=tagList) -> D0=ti_Data or defaultVal */
IPTR kaffeine_utility_GetTagData(struct KaffeineContext *ctx, void *cpu)
{
    ULONG default_val = THUNK_D(1);
    ULONG item = (ULONG)kaffeine_utility_FindTagItem(ctx, cpu);

    /* Like AROS rom/utility/gettagdata.c and compiler/arossupport/libgettagdata.c,
       look the item up with FindTagItem() in the guest list and return its
       32-bit ti_Data unchanged, or defaultVal when no item matches (also
       when tagList is NULL, despite the note in the gettagdata.c autodoc). */
    return item ? (IPTR)m68k_read32(ctx, item + 4) : (IPTR)default_val;
}

/* -48: NextTagItem(A0=tagListPtr) -> D0=next guest user item, *A0 updated */
IPTR kaffeine_utility_NextTagItem(struct KaffeineContext *ctx, void *cpu)
{
    ULONG ptr = THUNK_A(0);
    struct KaffeineTagWalk walk;
    ULONG item;

    /* Compared with LibNextTagItem() in compiler/arossupport/libnexttagitem.c:
       - Lines 7-10: LibNextTagItem() dereferences tagListPtr without checking
         it, and returns NULL when *tagListPtr is NULL. Here A0 is the guest
         address of the caller's 4-byte big-endian struct TagItem * variable;
         if its 4 bytes do not lie in guest memory, return 0 and write nothing.
         When *A0 is 0, Kaffeine_TagWalkNext() returns KAFFEINE_TAG_END with
         walk.next == 0, leaving *A0 at 0 and returning 0.
       - Lines 12-38: LibNextTagItem() updates *tagListPtr on every step of
         its while (1) loop (TAG_MORE at lines 16-19, TAG_IGNORE at lines
         21-22 and 37, TAG_END/TAG_DONE at lines 24-26, TAG_SKIP at lines
         28-30, and default at lines 32-33 returning (*tagListPtr)++). Here
         tag_step() in kaffeine_taglist.c performs the same switch in 8-byte
         guest TagItem steps inside walk, and *A0 is written once at the end
         of the call: walk.next (item + 8) when a user item is returned
         (matching line 33), or 0 at TAG_DONE (line 25) and at a NULL
         TAG_MORE (line 17).
       - Bounds and cycles: LibNextTagItem() has no bounds or cycle checks,
         so an item or TAG_SKIP past guest memory reads out of bounds, and a
         loop of control tags runs for ever inside one call. When the walker
         stops with KAFFEINE_TAG_BAD (an item or TAG_SKIP outside guest memory,
         or a cycle of control tags with no user item), write 0 to *A0 and
         return 0, just as at the end of a list. A ring that contains user
         items returns its first user item and writes item + 8 to *A0;
         because *A0 holds only a 4-byte item pointer and no walker state
         across calls, successive calls walk round a ring of user items as
         LibNextTagItem() does, while every single call terminates. */
    if (!ptr || ptr >= ctx->mem_size || ctx->mem_size - ptr < 4)
        return 0;
    Kaffeine_TagWalkInit(&walk, m68k_read32(ctx, ptr));
    if (Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM)
    {
        m68k_write32(ctx, ptr, walk.next);
        return (IPTR)item;
    }
    m68k_write32(ctx, ptr, 0);
    return 0;
}

/* -42: PackBoolTags(D0=initialFlags, A0=tagList, A1=boolMap) */
IPTR kaffeine_utility_PackBoolTags(struct KaffeineContext *ctx, void *cpu)
{
    ULONG flags = THUNK_D(0), map = THUNK_A(1), item, found;
    struct KaffeineTagWalk walk;

    Kaffeine_TagWalkInit(&walk, THUNK_A(0));
    while (Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM)
    {
        found = utility_find_tag(ctx, m68k_read32(ctx, item), map);
        if (found)
        {
            ULONG mask = m68k_read32(ctx, found + 4);

            if (m68k_read32(ctx, item + 4)) flags |= mask;
            else flags &= ~mask;
        }
    }
    return (IPTR)flags;
}

struct UtilityTagCopy
{
    ULONG addr, tag, data;
};

/* Writes can change control links in a ring or in overlapping guest items.
   Snapshot the finite walk before any mutation. The same snapshot lets
   Clone/Refresh reject a bad source before writing, and keeps their copies
   safe even if source and destination overlap. */
static BOOL utility_copy_tags(struct KaffeineContext *ctx, ULONG list, BOOL allow_bad,
                              struct UtilityTagCopy **copy, ULONG *count)
{
    struct KaffeineTagWalk walk;
    struct UtilityTagCopy *tags;
    ULONG item, i = 0;
    int end;

    *copy = NULL;
    *count = 0;
    Kaffeine_TagWalkInit(&walk, list);
    while ((end = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
        ++*count;
    if (end == KAFFEINE_TAG_BAD && !allow_bad) return FALSE;
    if (!*count) return TRUE;
    if (*count > (ULONG)~0UL / sizeof(*tags)) return FALSE;
    tags = (struct UtilityTagCopy *)AllocMem(*count * sizeof(*tags), MEMF_PRIVATE);
    if (!tags) return FALSE;
    Kaffeine_TagWalkInit(&walk, list);
    while (Kaffeine_TagWalkNext(ctx, &walk, &item) == KAFFEINE_TAG_ITEM)
    {
        tags[i].addr = item;
        tags[i].tag = m68k_read32(ctx, item);
        tags[i].data = m68k_read32(ctx, item + 4);
        i++;
    }
    *copy = tags;
    return TRUE;
}

static void utility_free_copy(struct UtilityTagCopy *copy, ULONG count)
{
    if (copy) FreeMem(copy, count * sizeof(*copy));
}

/* -54: FilterTagChanges(A0=changeList, A1=originalList, D0=apply), void */
IPTR kaffeine_utility_FilterTagChanges(struct KaffeineContext *ctx, void *cpu)
{
    ULONG changes = THUNK_A(0), original = THUNK_A(1), apply = THUNK_D(0);
    ULONG count, i;
    struct UtilityTagCopy *tags;

    if (!changes || !original ||
        !utility_copy_tags(ctx, changes, TRUE, &tags, &count)) return THUNK_D(0);
    for (i = 0; i < count; i++)
    {
        ULONG item = tags[i].addr;
        ULONG found = utility_find_tag(ctx, m68k_read32(ctx, item), original);

        if (found)
        {
            ULONG data = m68k_read32(ctx, item + 4);

            if (data == m68k_read32(ctx, found + 4))
                m68k_write32(ctx, item, TAG_IGNORE);
            else if (apply)
                m68k_write32(ctx, found + 4, data);
        }
    }
    utility_free_copy(tags, count);
    return THUNK_D(0);
}

/* -60: MapTags(A0=tagList, A1=mapList, D0=mapType), void */
IPTR kaffeine_utility_MapTags(struct KaffeineContext *ctx, void *cpu)
{
    struct UtilityTagCopy *tags;
    ULONG count, i, map = THUNK_A(1), type = THUNK_D(0);

    if (!utility_copy_tags(ctx, THUNK_A(0), TRUE, &tags, &count)) return THUNK_D(0);
    for (i = 0; i < count; i++)
    {
        ULONG found = utility_find_tag(ctx, m68k_read32(ctx, tags[i].addr), map);

        if (found)
        {
            ULONG tag = m68k_read32(ctx, found + 4);

            m68k_write32(ctx, tags[i].addr, tag == TAG_DONE ? TAG_IGNORE : tag);
        }
        else if (type == MAP_REMOVE_NOT_FOUND)
            m68k_write32(ctx, tags[i].addr, TAG_IGNORE);
    }
    utility_free_copy(tags, count);
    return THUNK_D(0);
}

/* -66: AllocateTagItems(D0=numTags), including the caller's terminator */
IPTR kaffeine_utility_AllocateTagItems(struct KaffeineContext *ctx, void *cpu)
{
    return (IPTR)Kaffeine_TagItemsAlloc(ctx, THUNK_D(0));
}

/* -72: CloneTagItems(A0=tagList) -> flat guest user items and TAG_DONE */
IPTR kaffeine_utility_CloneTagItems(struct KaffeineContext *ctx, void *cpu)
{
    struct UtilityTagCopy *tags;
    ULONG count, i, clone;

    if (!utility_copy_tags(ctx, THUNK_A(0), FALSE, &tags, &count)) return 0;
    clone = Kaffeine_TagItemsAlloc(ctx, count + 1);
    if (clone)
    {
        for (i = 0; i < count; i++)
        {
            m68k_write32(ctx, clone + i * 8, tags[i].tag);
            m68k_write32(ctx, clone + i * 8 + 4, tags[i].data);
        }
    }
    utility_free_copy(tags, count);
    return (IPTR)clone;
}

/* -78: FreeTagItems(A0=tagList), void */
IPTR kaffeine_utility_FreeTagItems(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_TagItemsFree(ctx, THUNK_A(0));
    return THUNK_D(0);
}

/* -84: RefreshTagItemClones(A0=clone, A1=original), void */
IPTR kaffeine_utility_RefreshTagItemClones(struct KaffeineContext *ctx, void *cpu)
{
    ULONG clone = THUNK_A(0), original = THUNK_A(1);
    ULONG capacity = Kaffeine_TagItemsCapacity(ctx, clone), count, i;
    struct UtilityTagCopy *tags;

    if (!capacity) return THUNK_D(0);
    if (!original)
    {
        /* AROS changes only ti_Tag for a NULL original. */
        m68k_write32(ctx, clone, TAG_DONE);
        return THUNK_D(0);
    }
    if (!utility_copy_tags(ctx, original, FALSE, &tags, &count)) return THUNK_D(0);
    /* Reserve the terminator slot, even though AROS does not copy it.
       Restore both longwords of each user item, leaving the tail alone. */
    if (count < capacity)
    {
        for (i = 0; i < count; i++)
        {
            m68k_write32(ctx, clone + i * 8, tags[i].tag);
            m68k_write32(ctx, clone + i * 8 + 4, tags[i].data);
        }
    }
    utility_free_copy(tags, count);
    return THUNK_D(0);
}

/* A Tag array contains BE32 tag values, with no ti_Data or control links.
   Require the terminator even when the wanted value occurs before it. */
static BOOL utility_tag_array(struct KaffeineContext *ctx, ULONG addr, ULONG *count)
{
    *count = 0;
    if (!addr) return FALSE;
    for (;;)
    {
        if (addr >= ctx->mem_size || ctx->mem_size - addr < 4) return FALSE;
        if (m68k_read32(ctx, addr) == TAG_DONE) return TRUE;
        ++*count;
        addr += 4;
    }
}

static BOOL utility_array_contains(struct KaffeineContext *ctx, ULONG addr,
                                   ULONG count, ULONG tag)
{
    ULONG i;

    for (i = 0; i < count; i++)
        if (m68k_read32(ctx, addr + i * 4) == tag) return TRUE;
    return FALSE;
}

/* -90: TagInArray(D0=tagValue, A0=tagArray) */
IPTR kaffeine_utility_TagInArray(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_A(0), tag = THUNK_D(0), count;

    if (!utility_tag_array(ctx, addr, &count)) return FALSE;
    return (IPTR)utility_array_contains(ctx, addr, count, tag);
}

/* -96: FilterTagItems(A0=tagList, A1=filterArray, D0=logic) */
IPTR kaffeine_utility_FilterTagItems(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), array = THUNK_A(1), logic = THUNK_D(0);
    ULONG count, num_tags, i, valid = 0;
    struct UtilityTagCopy *tags;

    if (!list || (logic != TAGFILTER_AND && logic != TAGFILTER_NOT) ||
        !utility_tag_array(ctx, array, &count)) return 0;
    if (!utility_copy_tags(ctx, list, TRUE, &tags, &num_tags)) return 0;
    for (i = 0; i < num_tags; i++)
    {
        ULONG item = tags[i].addr;
        BOOL hit = utility_array_contains(ctx, array, count, m68k_read32(ctx, item));

        if (hit == (logic == TAGFILTER_AND)) valid++;
        else m68k_write32(ctx, item, TAG_IGNORE);
    }
    utility_free_copy(tags, num_tags);
    return (IPTR)valid;
}

/* Match rom/utility/tolower.c, using unsigned bytes, without the host's
   locale or ctype rules. Locale.library patches are not modeled here. */
static UBYTE utility_lower(UBYTE c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (UBYTE)(c + 0x20);
    return c;
}

static BOOL utility_string_valid(struct KaffeineContext *ctx, ULONG addr)
{
    if (!addr || addr >= ctx->mem_size || ctx->mem_size - addr < 1) return FALSE;
    return memchr(ctx->mem + addr, 0, ctx->mem_size - addr) != NULL;
}

/* Bytes from addr to the end of guest memory, 0 for NULL or outside it. */
static ULONG utility_guest_tail(struct KaffeineContext *ctx, ULONG addr)
{
    if (!addr || addr >= ctx->mem_size) return 0;
    return ctx->mem_size - addr;
}

/* Compare like rom/utility/strnicmp.c, reading only the bytes it compares:
   up to length bytes, the first difference or NUL. A byte outside guest
   memory ends the call with the neutral failure result 0. */
static IPTR utility_compare(struct KaffeineContext *ctx, ULONG a, ULONG b, ULONG length)
{
    ULONG tail_a = utility_guest_tail(ctx, a), tail_b = utility_guest_tail(ctx, b), i = 0;
    UBYTE c1, c2;

    if (!length) return 0;
    do
    {
        if (i >= tail_a || i >= tail_b) return 0;
        c1 = utility_lower(ctx->mem[a + i]);
        c2 = utility_lower(ctx->mem[b + i]);
        i++;
    } while (c1 == c2 && c1 && --length);
    return (IPTR)(ULONG)((LONG)c1 - (LONG)c2);
}

/* -162: Stricmp(A0=string1, A1=string2), signed byte difference in D0 */
IPTR kaffeine_utility_Stricmp(struct KaffeineContext *ctx, void *cpu)
{
    ULONG a = THUNK_A(0), b = THUNK_A(1);

    /* Both complete strings must end inside guest memory, even if a
       mismatch comes first. Zero is the neutral failure result. */
    if (!utility_string_valid(ctx, a) || !utility_string_valid(ctx, b)) return 0;
    return utility_compare(ctx, a, b, (ULONG)~0UL);
}

/* -168: Strnicmp(A0=string1, A1=string2, D0=length) */
IPTR kaffeine_utility_Strnicmp(struct KaffeineContext *ctx, void *cpu)
{
    /* No terminator is needed beyond the compared bytes, so fixed-width
       fields work. An unsigned countdown avoids AROS's signed overflow on
       negative lengths; in this arena the end of guest memory, a NUL or a
       difference comes before that limit. */
    return utility_compare(ctx, THUNK_A(0), THUNK_A(1), THUNK_D(0));
}

#define M68K_SIZEOF_CLOCKDATA 14

/* Guest offset 0 holds the 68k exception vectors and is NULL in C, and all
   seven big-endian UWORD fields must fit without wrapping a 32-bit address. */
static BOOL clockdata_in_guest(struct KaffeineContext *ctx, ULONG addr)
{
    return addr && addr < ctx->mem_size &&
           ctx->mem_size - addr >= M68K_SIZEOF_CLOCKDATA;
}

static void read_guest_clockdata(struct KaffeineContext *ctx, ULONG addr,
                                 struct ClockData *date)
{
    date->sec   = m68k_read16(ctx, addr);
    date->min   = m68k_read16(ctx, addr + 2);
    date->hour  = m68k_read16(ctx, addr + 4);
    date->mday  = m68k_read16(ctx, addr + 6);
    date->month = m68k_read16(ctx, addr + 8);
    date->year  = m68k_read16(ctx, addr + 10);
    date->wday  = m68k_read16(ctx, addr + 12);
}

/* -120: Amiga2Date(D0=seconds, A0=result) */
IPTR kaffeine_utility_Amiga2Date(struct KaffeineContext *ctx, void *cpu)
{
    ULONG seconds = THUNK_D(0);
    ULONG addr = THUNK_A(0);
    struct ClockData result;

    if (!clockdata_in_guest(ctx, addr))
        return 0;

    Amiga2Date(seconds, &result);
    m68k_write16(ctx, addr,      result.sec);
    m68k_write16(ctx, addr + 2,  result.min);
    m68k_write16(ctx, addr + 4,  result.hour);
    m68k_write16(ctx, addr + 6,  result.mday);
    m68k_write16(ctx, addr + 8,  result.month);
    m68k_write16(ctx, addr + 10, result.year);
    m68k_write16(ctx, addr + 12, result.wday);
    return 0;
}

/* -126: Date2Amiga(A0=date) -> D0=seconds */
IPTR kaffeine_utility_Date2Amiga(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_A(0);
    struct ClockData date;

    if (!clockdata_in_guest(ctx, addr))
        return 0;

    read_guest_clockdata(ctx, addr, &date);
    /* rom/utility/date2amiga.c indexes dayspermonth[date->month - 1] without
       checking month, so refuse an out-of-range month before the native call. */
    if (date.month < 1 || date.month > 12)
        return 0;
    return (IPTR)Date2Amiga(&date);
}

/* -132: CheckDate(A0=date) -> D0=seconds or 0 */
IPTR kaffeine_utility_CheckDate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_A(0);
    struct ClockData date;

    if (!clockdata_in_guest(ctx, addr))
        return 0;

    read_guest_clockdata(ctx, addr, &date);
    return (IPTR)CheckDate(&date);
}

/* -150: SDivMod32(D0=dividend, D1=divisor) -> D0=quotient, D1=remainder */
IPTR kaffeine_utility_SDivMod32(struct KaffeineContext *ctx, void *cpu)
{
    int32_t dividend = (int32_t)THUNK_D(0), divisor = (int32_t)THUNK_D(1);
    int32_t quotient, remainder;

    (void)ctx;
    if (divisor == 0)
    {
        /* A 68k CPU raises a divide-by-zero exception here. Like the
           native AArch64 division, return a zero quotient, and leave the
           dividend as the remainder. */
        quotient = 0;
        remainder = dividend;
    }
    else if (divisor == -1)
    {
        /* -2^31 / -1 overflows in C; the quotient wraps to -2^31. */
        quotient = (int32_t)(0U - (uint32_t)dividend);
        remainder = 0;
    }
    else
    {
        /* C division truncates toward zero and gives the remainder the
           sign of the dividend, as DIVS.L does. */
        quotient = dividend / divisor;
        remainder = dividend % divisor;
    }
    THUNK_SET_D(1, (ULONG)remainder);
    return (IPTR)(ULONG)quotient;
}

/* -156: UDivMod32(D0=dividend, D1=divisor) -> D0=quotient, D1=remainder */
IPTR kaffeine_utility_UDivMod32(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t dividend = THUNK_D(0), divisor = THUNK_D(1);
    uint32_t quotient = 0, remainder = dividend;

    (void)ctx;
    /* A zero divisor gives the same results as in SDivMod32(). */
    if (divisor)
    {
        quotient = dividend / divisor;
        remainder = dividend % divisor;
    }
    THUNK_SET_D(1, remainder);
    return (IPTR)quotient;
}

/* -198: SMult64(D0=arg1, D1=arg2) -> D0=high 32 bits, D1=low 32 bits */
IPTR kaffeine_utility_SMult64(struct KaffeineContext *ctx, void *cpu)
{
    int64_t product = (int64_t)(int32_t)THUNK_D(0) * (int32_t)THUNK_D(1);

    (void)ctx;
    THUNK_SET_D(1, (ULONG)(uint32_t)product);
    return (IPTR)(uint32_t)((uint64_t)product >> 32);
}

/* -204: UMult64(D0=arg1, D1=arg2) -> D0=high 32 bits, D1=low 32 bits */
IPTR kaffeine_utility_UMult64(struct KaffeineContext *ctx, void *cpu)
{
    uint64_t product = (uint64_t)THUNK_D(0) * THUNK_D(1);

    (void)ctx;
    THUNK_SET_D(1, (ULONG)(uint32_t)product);
    return (IPTR)(uint32_t)(product >> 32);
}

/* -102: CallHookPkt(A0=hook, A2=object, A1=paramPacket) -> D0=hook result */
IPTR kaffeine_utility_CallHookPkt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG hook = THUNK_A(0), entry;
    struct KaffeineGuestCall call = { {0}, {0}, 0, 7u };

    /* The classic Hook is a MinNode followed by h_Entry, h_SubEntry and
       h_Data (20 bytes). Only the guest entry is invoked; it implements
       the HLL SubEntry convention if its owner needs that convention. */
    if (!hook || (hook & 1) || hook >= ctx->mem_size || ctx->mem_size - hook < 20)
        return 0;
    entry = m68k_read32(ctx, hook + 8);
    if (!entry || (entry & 1) || entry >= ctx->mem_size || ctx->mem_size - entry < 2 ||
        Kaffeine_IsCallbackReturn(entry)) return 0;
    call.a[0] = hook;
    call.a[1] = THUNK_A(1);
    call.a[2] = THUNK_A(2);
    if (!Kaffeine_CallGuest(ctx, cpu, entry, &call)) return (IPTR)ctx->exit_code;
    return (IPTR)call.d[0];
}

const struct M68KThunkEntry kaffeine_thunks_utility_manual[] = {
    { 30, kaffeine_utility_FindTagItem },
    { 36, kaffeine_utility_GetTagData },
    { 42, kaffeine_utility_PackBoolTags },
    { 48, kaffeine_utility_NextTagItem },
    { 54, kaffeine_utility_FilterTagChanges },
    { 60, kaffeine_utility_MapTags },
    { 66, kaffeine_utility_AllocateTagItems },
    { 72, kaffeine_utility_CloneTagItems },
    { 78, kaffeine_utility_FreeTagItems },
    { 84, kaffeine_utility_RefreshTagItemClones },
    { 90, kaffeine_utility_TagInArray },
    { 96, kaffeine_utility_FilterTagItems },
    { 102, kaffeine_utility_CallHookPkt },
    { 120, kaffeine_utility_Amiga2Date },
    { 126, kaffeine_utility_Date2Amiga },
    { 132, kaffeine_utility_CheckDate },
    { 150, kaffeine_utility_SDivMod32 },
    { 156, kaffeine_utility_UDivMod32 },
    { 162, kaffeine_utility_Stricmp },
    { 168, kaffeine_utility_Strnicmp },
    { 198, kaffeine_utility_SMult64 },
    { 204, kaffeine_utility_UMult64 },
    { 312, kaffeine_utility_VSNPrintf },
    { 0, NULL }
};
const ULONG kaffeine_thunks_utility_manual_count =
    sizeof(kaffeine_thunks_utility_manual) / sizeof(kaffeine_thunks_utility_manual[0]) - 1;
