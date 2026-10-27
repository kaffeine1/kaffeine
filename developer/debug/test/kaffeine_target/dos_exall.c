/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    dos.library ExAll() and ExAllEnd() checks in RAM:, written for both
    AmigaOS 3.2.3 and AROS through kaffeine.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/exall.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>
#include <string.h>

#include "m68ktest.h"

#define LAYOUT(n, e) typedef char layout_##n[(e) ? 1 : -1]
#define OFFSET(t, f, o) LAYOUT(t##_##f, offsetof(struct t, f) == (o))

OFFSET(ExAllControl, eac_Entries, 0);
OFFSET(ExAllControl, eac_LastKey, 4);
OFFSET(ExAllControl, eac_MatchString, 8);
OFFSET(ExAllControl, eac_MatchFunc, 12);

OFFSET(ExAllData, ed_Next, 0);
OFFSET(ExAllData, ed_Name, 4);
OFFSET(ExAllData, ed_Type, 8);
OFFSET(ExAllData, ed_Size, 12);
OFFSET(ExAllData, ed_Prot, 16);
OFFSET(ExAllData, ed_Days, 20);
OFFSET(ExAllData, ed_Mins, 24);
OFFSET(ExAllData, ed_Ticks, 28);
OFFSET(ExAllData, ed_Comment, 32);
OFFSET(ExAllData, ed_OwnerUID, 36);
OFFSET(ExAllData, ed_OwnerGID, 38);
LAYOUT(exalldata_size, sizeof(struct ExAllData) == 40);

#define DIR_NAME  "RAM:kaffeine-exall"
#define FILE_A    DIR_NAME "/a"
#define FILE_BB   DIR_NAME "/bb.txt"
#define FILE_CCC  DIR_NAME "/ccc_long_name_for_alignment.txt"
#define FILE_D    DIR_NAME "/d.TXT"
#define DIR_SUB   DIR_NAME "/sub"

#define SCAN_BUF_SIZE  4096
#define MAX_SCAN_CALLS 64

enum
{
    IDX_A,
    IDX_BB,
    IDX_CCC,
    IDX_D,
    IDX_SUB,
    ENTRY_COUNT
};

#define MASK_ALL  ((1UL << IDX_A) | (1UL << IDX_BB) | (1UL << IDX_CCC) | (1UL << IDX_D) | (1UL << IDX_SUB))
#define MASK_TXT  ((1UL << IDX_BB) | (1UL << IDX_CCC) | (1UL << IDX_D))
#define MASK_NO_A ((1UL << IDX_BB) | (1UL << IDX_CCC) | (1UL << IDX_D) | (1UL << IDX_SUB))

struct ExpectedItem
{
    const char *name;
    const char *path;
    BOOL is_dir;
    LONG size;
    const char *comment;
};

static const struct ExpectedItem expected_items[ENTRY_COUNT] =
{
    { "a",                               FILE_A,   FALSE,   0, NULL    },
    { "bb.txt",                          FILE_BB,  FALSE,  10, "hello" },
    { "ccc_long_name_for_alignment.txt", FILE_CCC, FALSE, 100, NULL    },
    { "d.TXT",                           FILE_D,   FALSE,   1, NULL    },
    { "sub",                             DIR_SUB,  TRUE,    0, NULL    }
};

/* Fixed part sizes in bytes for ED_NAME (1) .. ED_OWNER (7). */
static const ULONG fixed_sizes[ED_OWNER + 1] =
{
    0, 8, 12, 16, 20, 32, 36, 40
};

static const char *const type_names[ED_OWNER + 1] =
{
    "0",
    "ED_NAME",
    "ED_TYPE",
    "ED_SIZE",
    "ED_PROTECTION",
    "ED_DATE",
    "ED_COMMENT",
    "ED_OWNER"
};

static UBYTE scan_buf[SCAN_BUF_SIZE] __attribute__((aligned(4)));
static UBYTE filler[100];
static char pat_tokens[64];

struct HookState
{
    const UBYTE *buf;
    ULONG buf_size;
    LONG expected_type;
    LONG calls;
    BOOL hook_ok;
    BOOL ed_ok;
    BOOL type_ok;
    LONG seen[ENTRY_COUNT];
    LONG unknown;
};

static struct Hook match_hook;
static struct HookState hook_state;

static LONG find_expected(const char *name)
{
    LONG i;

    if (!name)
        return -1;
    for (i = 0; i < ENTRY_COUNT; i++)
        if (strcmp(name, expected_items[i].name) == 0)
            return i;
    return -1;
}

static BOOL names_match_once(const LONG *seen, LONG unknown, ULONG mask)
{
    LONG i;

    if (unknown != 0)
        return FALSE;
    for (i = 0; i < ENTRY_COUNT; i++)
    {
        LONG want = (mask & (1UL << i)) ? 1 : 0;
        if (seen[i] != want)
            return FALSE;
    }
    return TRUE;
}

static BOOL name_in_buffer(const struct ExAllData *ed, const UBYTE *buf,
                           ULONG buf_size, ULONG fixed_size)
{
    const UBYTE *entry = (const UBYTE *)ed;
    const UBYTE *end = buf + buf_size;
    const UBYTE *name;
    const UBYTE *p;

    if (entry < buf || entry + fixed_size > end)
        return FALSE;
    name = (const UBYTE *)ed->ed_Name;
    if (!name || name < entry + fixed_size || name >= end)
        return FALSE;
    for (p = name; p < end; p++)
        if (*p == '\0')
            return TRUE;
    return FALSE;
}

static BOOL write_file(CONST_STRPTR path, LONG size)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    LONG n = 0;

    if (!fh)
        return FALSE;
    if (size > 0)
        n = Write(fh, filler, size);
    return Close(fh) && n == size;
}

static BOOL create_fixture_entries(void)
{
    LONG i;
    BPTR sub;

    for (i = 0; i < (LONG)sizeof(filler); i++)
        filler[i] = (UBYTE)('A' + (i % 26));

    for (i = 0; i < ENTRY_COUNT; i++)
    {
        if (expected_items[i].is_dir)
        {
            sub = CreateDir((CONST_STRPTR)expected_items[i].path);
            if (!sub)
                return FALSE;
            UnLock(sub);
        }
        else
        {
            if (!write_file((CONST_STRPTR)expected_items[i].path, expected_items[i].size))
                return FALSE;
            if (expected_items[i].comment &&
                !SetComment((CONST_STRPTR)expected_items[i].path,
                            (CONST_STRPTR)expected_items[i].comment))
                return FALSE;
        }
    }
    return TRUE;
}

static BOOL cleanup_fixture(void)
{
    BOOL ok = TRUE;
    LONG i;

    for (i = 0; i < ENTRY_COUNT; i++)
        if (!DeleteFile((CONST_STRPTR)expected_items[i].path))
            ok = FALSE;
    if (!DeleteFile((CONST_STRPTR)DIR_NAME))
        ok = FALSE;
    return ok;
}

/* Hook entry using GCC m68k register parameters; free of library calls. */
static ULONG match_hook_func(struct Hook *h __asm("a0"),
                             struct ExAllData *ed __asm("a1"),
                             LONG *type __asm("a2"))
{
    LONG idx;

    hook_state.calls++;
    if (h != &match_hook)
        hook_state.hook_ok = FALSE;
    if (!type || *type != hook_state.expected_type)
        hook_state.type_ok = FALSE;
    if (!ed || !name_in_buffer(ed, hook_state.buf, hook_state.buf_size,
                               fixed_sizes[hook_state.expected_type]))
    {
        hook_state.ed_ok = FALSE;
        return FALSE;
    }
    idx = find_expected((const char *)ed->ed_Name);
    if (idx < 0)
    {
        hook_state.ed_ok = FALSE;
        hook_state.unknown++;
    }
    else
        hook_state.seen[idx]++;
    if (strcmp((const char *)ed->ed_Name, "a") == 0)
        return FALSE;
    return TRUE;
}

static void reset_hook_state(LONG expected_type, ULONG buf_size)
{
    memset(&hook_state, 0, sizeof(hook_state));
    hook_state.buf = scan_buf;
    hook_state.buf_size = buf_size;
    hook_state.expected_type = expected_type;
    hook_state.hook_ok = TRUE;
    hook_state.ed_ok = TRUE;
    hook_state.type_ok = TRUE;
}

/* Walk a standard scan (for buffer sizes large enough to hold entries) and
   record per-call chain and entry properties. */
struct ScanStats
{
    LONG calls;
    LONG total_entries;
    BOOL last_more;
    LONG last_err;
    BOOL all_prev_true;
    BOOL last_next_null;
    BOOL entries_match_chain;
    BOOL names_valid;
    BOOL next_even;
    LONG seen[ENTRY_COUNT];
    LONG unknown;
};

static void run_scan(BPTR lock, ULONG buf_size, LONG type,
                     struct ExAllControl *eac, const char *label,
                     struct ScanStats *st)
{
    ULONG fixed = (type >= ED_NAME && type <= ED_OWNER) ? fixed_sizes[type] : 8;

    memset(st, 0, sizeof(*st));
    st->all_prev_true = TRUE;
    st->last_next_null = TRUE;
    st->entries_match_chain = TRUE;
    st->names_valid = TRUE;
    st->next_even = TRUE;

    eac->eac_LastKey = 0;
    do
    {
        struct ExAllData *ed;
        LONG chain_len = 0;

        memset(scan_buf, 0xA5, buf_size);
        st->last_more = ExAll(lock, (struct ExAllData *)scan_buf, (LONG)buf_size, type, eac);
        st->last_err = IoErr();
        st->calls++;

        if (!st->last_more && st->last_err != ERROR_NO_MORE_ENTRIES)
            break;

        if (eac->eac_Entries > 0)
        {
            ed = (struct ExAllData *)scan_buf;
            while (ed && chain_len < MAX_SCAN_CALLS)
            {
                LONG idx;

                chain_len++;
                if (((ULONG)ed->ed_Next & 1) != 0)
                    st->next_even = FALSE;
                if (!name_in_buffer(ed, scan_buf, buf_size, fixed))
                {
                    st->names_valid = FALSE;
                    st->unknown++;
                }
                else
                {
                    idx = find_expected((const char *)ed->ed_Name);
                    if (idx < 0)
                        st->unknown++;
                    else
                        st->seen[idx]++;
                }
                if (ed->ed_Next &&
                    ((UBYTE *)ed->ed_Next < scan_buf ||
                     (UBYTE *)ed->ed_Next >= scan_buf + buf_size))
                {
                    st->last_next_null = FALSE;
                    break;
                }
                ed = ed->ed_Next;
            }
            if (ed != NULL)
                st->last_next_null = FALSE;
        }
        if ((LONG)eac->eac_Entries != chain_len)
            st->entries_match_chain = FALSE;
        st->total_entries += chain_len;
    } while (st->last_more && st->calls < MAX_SCAN_CALLS);

    if (st->last_more)
        ExAllEnd(lock, (struct ExAllData *)scan_buf, (LONG)buf_size, type, eac);

    Printf("info %s: %ld calls, %ld entries\n",
           (LONG)label, st->calls, st->total_entries);
    test_flush();
}

static void check_owner_scan(BPTR lock, struct ExAllControl *eac)
{
    LONG calls = 0, total_entries = 0, err = 0, i;
    BOOL more = FALSE;
    BOOL last_next_null = TRUE, entries_match_chain = TRUE;
    BOOL files_type_neg = TRUE, other_comments_empty = TRUE;
    LONG sub_type = 0;
    LONG got_size[ENTRY_COUNT];
    LONG seen[ENTRY_COUNT];
    LONG unknown = 0;
    char bb_comment[32];
    BOOL bb_has_comment = FALSE;

    for (i = 0; i < ENTRY_COUNT; i++)
    {
        got_size[i] = -1;
        seen[i] = 0;
    }
    bb_comment[0] = '\0';

    eac->eac_LastKey = 0;
    do
    {
        struct ExAllData *ed;
        LONG chain_len = 0;

        memset(scan_buf, 0xA5, SCAN_BUF_SIZE);
        more = ExAll(lock, (struct ExAllData *)scan_buf, SCAN_BUF_SIZE, ED_OWNER, eac);
        err = IoErr();
        calls++;

        if (!more && err != ERROR_NO_MORE_ENTRIES)
            break;

        if (eac->eac_Entries > 0)
        {
            ed = (struct ExAllData *)scan_buf;
            while (ed && chain_len < MAX_SCAN_CALLS)
            {
                LONG idx;

                chain_len++;
                if (!name_in_buffer(ed, scan_buf, SCAN_BUF_SIZE, fixed_sizes[ED_OWNER]))
                {
                    unknown++;
                }
                else
                {
                    idx = find_expected((const char *)ed->ed_Name);
                    if (idx < 0)
                    {
                        unknown++;
                    }
                    else
                    {
                        seen[idx]++;
                        got_size[idx] = (LONG)ed->ed_Size;
                        if (idx == IDX_SUB)
                        {
                            sub_type = ed->ed_Type;
                        }
                        else if (ed->ed_Type >= 0)
                        {
                            files_type_neg = FALSE;
                        }

                        if (idx == IDX_BB)
                        {
                            if (ed->ed_Comment &&
                                (UBYTE *)ed->ed_Comment >= scan_buf &&
                                (UBYTE *)ed->ed_Comment < scan_buf + SCAN_BUF_SIZE)
                            {
                                strncpy(bb_comment, (const char *)ed->ed_Comment,
                                        sizeof(bb_comment) - 1);
                                bb_comment[sizeof(bb_comment) - 1] = '\0';
                                bb_has_comment = TRUE;
                            }
                        }
                        else
                        {
                            if (ed->ed_Comment != NULL && ed->ed_Comment[0] != '\0')
                                other_comments_empty = FALSE;
                        }
                    }
                }
                if (ed->ed_Next &&
                    ((UBYTE *)ed->ed_Next < scan_buf ||
                     (UBYTE *)ed->ed_Next >= scan_buf + SCAN_BUF_SIZE))
                {
                    last_next_null = FALSE;
                    break;
                }
                ed = ed->ed_Next;
            }
            if (ed != NULL)
                last_next_null = FALSE;
        }
        if ((LONG)eac->eac_Entries != chain_len)
            entries_match_chain = FALSE;
        total_entries += chain_len;
    } while (more && calls < MAX_SCAN_CALLS);

    if (more)
        ExAllEnd(lock, (struct ExAllData *)scan_buf, SCAN_BUF_SIZE, ED_OWNER, eac);

    Printf("info ED_OWNER 4096-byte scan: %ld calls, %ld entries\n",
           calls, total_entries);
    test_flush();

    check(!more, "ED_OWNER scan returns FALSE");
    check_long("ED_OWNER scan IoErr()", err, ERROR_NO_MORE_ENTRIES);
    check(names_match_once(seen, unknown, MASK_ALL),
          "ED_OWNER scan gives the five names once");
    check(sub_type > 0, "ED_OWNER sub has ed_Type > 0");
    check(files_type_neg, "ED_OWNER files have ed_Type < 0");
    check_long("ED_OWNER size of a", got_size[IDX_A], 0);
    check_long("ED_OWNER size of bb.txt", got_size[IDX_BB], 10);
    check_long("ED_OWNER size of ccc_long_name_for_alignment.txt", got_size[IDX_CCC], 100);
    check_long("ED_OWNER size of d.TXT", got_size[IDX_D], 1);
    check_str("ED_OWNER comment of bb.txt", bb_has_comment ? bb_comment : NULL, "hello");
    check(other_comments_empty,
          "ED_OWNER other entries have NULL or empty comment");
    check(last_next_null, "ED_OWNER ed_Next of the last entry is NULL");
    check(entries_match_chain,
          "ED_OWNER eac_Entries equals chain length of each call");
}

int main(void)
{
    BPTR dir = 0, dir_lock = 0, file_lock = 0;
    struct ExAllControl *eac = NULL;
    struct ScanStats st;
    LONG type, calls, total_entries, err, first_entries;
    BOOL more, setup_ok = FALSE;
    char desc[96];

    /* Ensure no stale test directory remains before starting. */
    cleanup_fixture();

    dir = CreateDir((CONST_STRPTR)DIR_NAME);
    if (!check(dir != 0, "CreateDir(RAM:kaffeine-exall)"))
        goto cleanup;
    UnLock(dir);
    dir = 0;

    if (!check(create_fixture_entries(),
               "create test files, comment and sub drawer"))
        goto cleanup;

    dir_lock = Lock((CONST_STRPTR)DIR_NAME, SHARED_LOCK);
    if (!check(dir_lock != 0, "Lock(RAM:kaffeine-exall, SHARED_LOCK)"))
        goto cleanup;

    eac = (struct ExAllControl *)AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (!check(eac != NULL, "AllocDosObject(DOS_EXALLCONTROL, NULL)"))
        goto cleanup;

    setup_ok = TRUE;

    /* 1. Fresh ExAllControl has all four fields 0. */
    check_long("fresh eac_Entries", (LONG)eac->eac_Entries, 0);
    check_long("fresh eac_LastKey", (LONG)eac->eac_LastKey, 0);
    check_long("fresh eac_MatchString", (LONG)eac->eac_MatchString, 0);
    check_long("fresh eac_MatchFunc", (LONG)eac->eac_MatchFunc, 0);

    /* 2. Full scan, ED_OWNER, 4096-byte buffer. */
    check_owner_scan(dir_lock, eac);

    /* 3. Each type ED_NAME..ED_COMMENT. */
    for (type = ED_NAME; type <= ED_COMMENT; type++)
    {
        run_scan(dir_lock, SCAN_BUF_SIZE, type, eac, type_names[type], &st);

        strcpy(desc, type_names[type]);
        strcat(desc, ": ends with FALSE and ERROR_NO_MORE_ENTRIES");
        check(!st.last_more && st.last_err == ERROR_NO_MORE_ENTRIES, desc);

        strcpy(desc, type_names[type]);
        strcat(desc, ": gives the five names once");
        check(names_match_once(st.seen, st.unknown, MASK_ALL), desc);

        strcpy(desc, type_names[type]);
        strcat(desc, ": ed_Name inside buffer at or after fixed part");
        check(st.names_valid, desc);

        strcpy(desc, type_names[type]);
        strcat(desc, ": every ed_Next is even and chain matches eac_Entries");
        check(st.next_even && st.last_next_null && st.entries_match_chain, desc);
    }

    /* 4. ED_NAME with a 64-byte buffer. */
    run_scan(dir_lock, 64, ED_NAME, eac, "ED_NAME 64-byte scan", &st);
    check(st.calls > 1 && st.all_prev_true,
          "64-byte ED_NAME scan: several calls, TRUE while more remain");
    check(!st.last_more, "64-byte ED_NAME scan: last call returns FALSE");
    check_long("64-byte ED_NAME scan: last IoErr()",
               st.last_err, ERROR_NO_MORE_ENTRIES);
    check(names_match_once(st.seen, st.unknown, MASK_ALL),
          "64-byte ED_NAME scan: five names once overall");

    /* 5. A 4-byte buffer: FALSE, eac_Entries 0, IoErr() != 0; stop after at most 8 calls.
          AmigaOS 3.2.3 ram-handler leaves eac_Entries alone when it fails,
          so the count is cleared before each call that should fail. */
    eac->eac_LastKey = 0;
    calls = 0;
    total_entries = 0;
    do
    {
        eac->eac_Entries = 0;
        more = ExAll(dir_lock, (struct ExAllData *)scan_buf, 4, ED_NAME, eac);
        err = IoErr();
        calls++;
        total_entries += (LONG)eac->eac_Entries;
    } while (more && calls < 8);
    if (more)
        ExAllEnd(dir_lock, (struct ExAllData *)scan_buf, 4, ED_NAME, eac);
    Printf("info 4-byte buffer scan: %ld calls, %ld entries\n",
           calls, total_entries);
    test_flush();
    check(!more, "4-byte buffer: ExAll() returns FALSE");
    check_long("4-byte buffer: eac_Entries", total_entries, 0);
    check(err != 0, "4-byte buffer: IoErr() is non-zero");

    /* 6. Types 0 and 8: FALSE with IoErr() 115 (ERROR_BAD_NUMBER), eac_Entries 0. */
    eac->eac_LastKey = 0;
    eac->eac_Entries = 0;
    more = ExAll(dir_lock, (struct ExAllData *)scan_buf, SCAN_BUF_SIZE, 0, eac);
    err = IoErr();
    Printf("info type 0 scan: 1 calls, %ld entries\n", (LONG)eac->eac_Entries);
    test_flush();
    check(!more, "type 0: ExAll() returns FALSE");
    check_long("type 0: IoErr()", err, ERROR_BAD_NUMBER);
    check_long("type 0: eac_Entries", (LONG)eac->eac_Entries, 0);

    eac->eac_LastKey = 0;
    eac->eac_Entries = 0;
    more = ExAll(dir_lock, (struct ExAllData *)scan_buf, SCAN_BUF_SIZE, 8, eac);
    err = IoErr();
    Printf("info type 8 scan: 1 calls, %ld entries\n", (LONG)eac->eac_Entries);
    test_flush();
    check(!more, "type 8: ExAll() returns FALSE");
    check_long("type 8: IoErr()", err, ERROR_BAD_NUMBER);
    check_long("type 8: eac_Entries", (LONG)eac->eac_Entries, 0);

    /* 7. A lock on the file bb.txt: FALSE, IoErr() 212 (ERROR_OBJECT_WRONG_TYPE). */
    file_lock = Lock((CONST_STRPTR)FILE_BB, SHARED_LOCK);
    if (check(file_lock != 0, "Lock(bb.txt, SHARED_LOCK)"))
    {
        eac->eac_LastKey = 0;
        eac->eac_Entries = 0;
        more = ExAll(file_lock, (struct ExAllData *)scan_buf, SCAN_BUF_SIZE, ED_NAME, eac);
        err = IoErr();
        Printf("info file lock scan: 1 calls, %ld entries\n", (LONG)eac->eac_Entries);
        test_flush();
        check(!more, "file lock on bb.txt: ExAll() returns FALSE");
        check_long("file lock on bb.txt: IoErr()", err, ERROR_OBJECT_WRONG_TYPE);
        UnLock(file_lock);
        file_lock = 0;
    }

    /* 8. eac_MatchString from ParsePatternNoCase("#?.txt", ...). */
    check_long("ParsePatternNoCase(\"#?.txt\")",
               ParsePatternNoCase("#?.txt", (STRPTR)pat_tokens, sizeof(pat_tokens)), 1);
    eac->eac_MatchString = (STRPTR)pat_tokens;
    eac->eac_MatchFunc = NULL;
    run_scan(dir_lock, SCAN_BUF_SIZE, ED_NAME, eac, "eac_MatchString #?.txt scan", &st);
    check(!st.last_more && st.last_err == ERROR_NO_MORE_ENTRIES,
          "eac_MatchString: scan ends with FALSE and ERROR_NO_MORE_ENTRIES");
    check(names_match_once(st.seen, st.unknown, MASK_TXT),
          "eac_MatchString: gives only bb.txt, ccc_long_name_for_alignment.txt and d.TXT");
    eac->eac_MatchString = NULL;

    /* 9. eac_MatchFunc with ED_TYPE, rejecting "a". */
    memset(&match_hook, 0, sizeof(match_hook));
    match_hook.h_Entry = (ULONG (*)())match_hook_func;
    match_hook.h_Data = &hook_state;
    reset_hook_state(ED_TYPE, SCAN_BUF_SIZE);
    eac->eac_MatchString = NULL;
    eac->eac_MatchFunc = &match_hook;
    run_scan(dir_lock, SCAN_BUF_SIZE, ED_TYPE, eac, "eac_MatchFunc scan", &st);
    check(!st.last_more && st.last_err == ERROR_NO_MORE_ENTRIES,
          "eac_MatchFunc: scan ends with FALSE and ERROR_NO_MORE_ENTRIES");
    check(hook_state.hook_ok, "eac_MatchFunc: h equals the hook in every call");
    check(hook_state.ed_ok,
          "eac_MatchFunc: ed is inside the buffer with its name filled in");
    check(hook_state.type_ok,
          "eac_MatchFunc: *type equals ED_TYPE in every call");
    check_long("eac_MatchFunc: hook ran five times", hook_state.calls, 5);
    check(names_match_once(hook_state.seen, hook_state.unknown, MASK_ALL),
          "eac_MatchFunc: hook saw all five names once");
    check(names_match_once(st.seen, st.unknown, MASK_NO_A),
          "eac_MatchFunc: scan gives the four other names and never a");

    /* 10. Pattern and hook together: the hook only sees the three .txt names. */
    reset_hook_state(ED_TYPE, SCAN_BUF_SIZE);
    eac->eac_MatchString = (STRPTR)pat_tokens;
    eac->eac_MatchFunc = &match_hook;
    run_scan(dir_lock, SCAN_BUF_SIZE, ED_TYPE, eac, "pattern and hook scan", &st);
    check(!st.last_more && st.last_err == ERROR_NO_MORE_ENTRIES,
          "pattern + hook: scan ends with FALSE and ERROR_NO_MORE_ENTRIES");
    check_long("pattern + hook: hook ran three times", hook_state.calls, 3);
    check(names_match_once(hook_state.seen, hook_state.unknown, MASK_TXT),
          "pattern + hook: hook only sees the three .txt names");
    check(names_match_once(st.seen, st.unknown, MASK_TXT),
          "pattern + hook: scan gives the three .txt names");
    eac->eac_MatchString = NULL;
    eac->eac_MatchFunc = NULL;

    /* 11. ExAllEnd() after the first call of a 40-byte ED_NAME scan, then
           eac_LastKey = 0 and a full scan with 4096 bytes. */
    eac->eac_LastKey = 0;
    memset(scan_buf, 0xA5, 40);
    more = ExAll(dir_lock, (struct ExAllData *)scan_buf, 40, ED_NAME, eac);
    err = IoErr();
    first_entries = (LONG)eac->eac_Entries;
    if (more)
        ExAllEnd(dir_lock, (struct ExAllData *)scan_buf, 40, ED_NAME, eac);
    Printf("info ExAllEnd() 40-byte partial scan: 1 calls, %ld entries\n",
           first_entries);
    test_flush();
    check(more != 0, "ExAllEnd() 40-byte ED_NAME first call returns TRUE");
    check(first_entries > 0 && first_entries < ENTRY_COUNT,
          "ExAllEnd() 40-byte ED_NAME first call returns a partial batch");

    run_scan(dir_lock, SCAN_BUF_SIZE, ED_NAME, eac,
             "full scan after ExAllEnd()", &st);
    check(!st.last_more && st.last_err == ERROR_NO_MORE_ENTRIES,
          "full scan after ExAllEnd() ends with FALSE and ERROR_NO_MORE_ENTRIES");
    check(names_match_once(st.seen, st.unknown, MASK_ALL),
          "full scan after ExAllEnd() gives all five names once");

cleanup:
    if (file_lock)
        UnLock(file_lock);
    if (eac)
        FreeDosObject(DOS_EXALLCONTROL, eac);
    if (dir_lock)
        UnLock(dir_lock);
    if (setup_ok)
        check(cleanup_fixture(), "remove RAM:kaffeine-exall and its contents");
    else
        cleanup_fixture();

    return test_done("dos_exall");
}
