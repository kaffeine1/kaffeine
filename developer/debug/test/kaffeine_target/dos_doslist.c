/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Read-only DosList access, the way classic programs use it: the names
   walk of amigentd 1.1.1's c_volumes() and its Lock()/Info() pass, a
   cross-check of Info("SYS:") against the volume list, the classic layout
   of every entry and of the list handle, the type filters of
   NextDosEntry() and FindDosEntry(), nested and refused locks, and names
   read after the final unlock. It never adds, removes or changes an
   entry: write locks are only asked for where Kaffeine refuses them, and
   inside a read lock only through AttemptLockDosList(), which cannot wait.
   Only mounted volumes are locked, so no requester can open. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"

#ifndef DLT_PRIVATE
#define DLT_PRIVATE -1
#endif

#define RECORD_LIMIT 256
#define WALK_LIMIT 4096
#define NAMES_BYTES 8192
#define SENTINEL 4242
#define REPEATS 64

typedef char dos_list_is_44_bytes[sizeof(struct DosList) == 44 ? 1 : -1];

struct Entry
{
    struct DosList *dl;
    LONG type;
    ULONG task;
    UBYTE length;
    char name[256];
};

static struct Entry entries[RECORD_LIMIT];
static struct DosList *walked[RECORD_LIMIT];
static UBYTE names[NAMES_BYTES], again[NAMES_BYTES];
static struct InfoData sys_info __attribute__((aligned(4)));
static struct InfoData volume_info __attribute__((aligned(4)));
static char long_name[300];

static void info_long(const char *what, LONG value)
{
    Printf("info %s: %ld (0x%08lx)\n", (LONG)what, value, value);
    test_flush();
}

static ULONG at(const void *p, ULONG offset)
{
    return *(const ULONG *)((const UBYTE *)p + offset);
}

static BOOL aligned(const void *p)
{
    return ((ULONG)p & 3) == 0;
}

static UBYTE fold(UBYTE c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (UBYTE)(c + 32);
    return c;
}

static BOOL same_name(const char *a, const char *b)
{
    while (*a && fold((UBYTE)*a) == fold((UBYTE)*b))
        a++, b++;
    return !*a && !*b;
}

/* A classic BSTR of 1..255 bytes, longword aligned, with a NUL after its
   last byte as AROS and MakeDosEntry() keep it, and no NUL or colon in
   the name. Copies it as a C string. */
static BOOL bstr_name(BPTR b, char *out, UBYTE *length)
{
    const UBYTE *s = (const UBYTE *)BADDR(b);
    ULONG i;

    out[0] = 0;
    *length = 0;
    if (!b || !aligned(s) || s[0] == 0 || s[s[0] + 1] != 0)
        return FALSE;
    for (i = 1; i <= s[0]; i++)
        if (s[i] == 0 || s[i] == ':')
            return FALSE;
    memcpy(out, s + 1, s[0]);
    out[s[0]] = 0;
    *length = s[0];
    return TRUE;
}

/* Any NUL-terminated classic BSTR, such as a handler name with a colon */
static BOOL bstr_ok(BPTR b)
{
    const UBYTE *s = (const UBYTE *)BADDR(b);
    return b && aligned(s) && s[0] && s[s[0] + 1] == 0;
}

/* A C string of at most 1023 bytes */
static BOOL c_string(ULONG p)
{
    const UBYTE *s = (const UBYTE *)p;
    ULONG i;

    for (i = 0; i < 1024; i++)
        if (!s[i])
            return TRUE;
    return FALSE;
}

/* A port that guest code may read: what dol_Task and fl_Task must be */
static BOOL guest_port(ULONG p)
{
    return p && aligned((void *)p) && ((struct Node *)p)->ln_Type == NT_MSGPORT;
}

static LONG walk(struct DosList *start, ULONG flags, struct DosList **out)
{
    struct DosList *dl = start;
    LONG n = 0;

    while ((dl = NextDosEntry(dl, flags)) != NULL)
    {
        if (n < RECORD_LIMIT)
            out[n] = dl;
        if (++n > WALK_LIMIT)
            break;
    }
    return n;
}

static BOOL matches(LONG type, ULONG flags)
{
    switch (type)
    {
    case DLT_DEVICE:
        return (flags & LDF_DEVICES) != 0;
    case DLT_VOLUME:
        return (flags & LDF_VOLUMES) != 0;
    case DLT_DIRECTORY:
    case DLT_LATE:
    case DLT_NONBINDING:
        return (flags & LDF_ASSIGNS) != 0;
    }
    return FALSE;
}

static LONG find_recorded(struct DosList *dl, LONG count)
{
    LONG i;

    for (i = 0; i < count && i < RECORD_LIMIT; i++)
        if (entries[i].dl == dl)
            return i;
    return -1;
}

/* amigentd 1.1.1 c_volumes(), names part: mounted volumes only, each name
   with a colon and a NUL, copied before the list is unlocked */
static LONG volume_names(UBYTE *buffer, BOOL *overflow)
{
    struct DosList *dl;
    const UBYTE *name;
    LONG used = 0;

    *overflow = FALSE;
    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_VOLUMES)))
        if (dl->dol_Task)
        {
            name = BADDR(dl->dol_Name);
            if (used + name[0] + 2 > NAMES_BYTES)
            {
                *overflow = TRUE;
                continue;
            }
            memcpy(buffer + used, name + 1, name[0]);
            used += name[0];
            buffer[used++] = ':';
            buffer[used++] = 0;
        }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return used;
}

static BOOL listed(const UBYTE *buffer, LONG used, const char *volume)
{
    LONG at_ = 0;

    while (at_ < used)
    {
        const char *name = (const char *)buffer + at_;
        LONG n = strlen(name);
        if (n > 1 && !strncmp(name, volume, n - 1) && volume[n - 1] == 0)
            return TRUE;
        at_ += n + 1;
    }
    return FALSE;
}

int main(void)
{
    struct DosList *list, *dl, *found, *nested, *view, *bad[3];
    struct FileLock *fl;
    struct DevProc *dp;
    BPTR sys = 0, lock;
    LONG used, count, mounted = 0, lines = 0, i, j, m, n, by_type[5], volumes = 0;
    ULONG sys_task = 0, avail;
    LONG error;
    UBYTE length, saved_length = 0;
    const UBYTE *saved_bstr = NULL;
    char volume[256], saved_name[256], buffer[512];
    BOOL overflow, ok, ports_ok;

    /* 1. Refusals before any lock is held. AROS returns NULL for flags
       without exactly one of LDF_READ and LDF_WRITE or with unknown bits,
       and leaves IoErr alone. Kaffeine refuses a write lock with
       ERROR_NOT_IMPLEMENTED. */
    SetIoErr(SENTINEL);
    bad[0] = LockDosList(LDF_VOLUMES);
    bad[1] = LockDosList(LDF_READ | LDF_WRITE | LDF_VOLUMES);
    bad[2] = LockDosList(LDF_READ | 0x80);
    error = IoErr();
    check(bad[0] == NULL, "flags without LDF_READ or LDF_WRITE are refused");
    check(bad[1] == NULL, "LDF_READ with LDF_WRITE is refused");
    check(bad[2] == NULL, "an unknown flag bit is refused");
    check_long("malformed flags leave IoErr alone", error, SENTINEL);
    SetIoErr(0);
    list = LockDosList(LDF_WRITE | LDF_VOLUMES);
    error = IoErr();
    check(list == NULL, "a write lock is refused");
    check_long("refused write lock IoErr", error, ERROR_NOT_IMPLEMENTED);
    /* c_volumes() never checks the handle: the refused path must return */
    dl = NextDosEntry(list, LDF_VOLUMES);
    UnLockDosList(LDF_WRITE | LDF_VOLUMES);
    check(dl == NULL, "NextDosEntry(NULL) and the unlock after a refused lock return to the program");
    check(AttemptLockDosList(LDF_WRITE | LDF_VOLUMES) == NULL, "an attempted write lock is refused");
    UnLockDosList(LDF_WRITE | LDF_VOLUMES);
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    check(TRUE, "an unlock without a lock returns to the program");

    /* 2. The original c_volumes() sequence */
    SetIoErr(SENTINEL);
    used = volume_names(names, &overflow);
    check_long("the c_volumes walk leaves IoErr alone", IoErr(), SENTINEL);
    check(!overflow, "the mounted volume names fit amigentd's text");
    for (i = 0; i < used; i += strlen((char *)names + i) + 1)
    {
        const char *name = (const char *)names + i;
        mounted++;
        if (!(lock = Lock((STRPTR)name, SHARED_LOCK)))
        {
            Printf("info volume %s: no lock, IoErr %ld\n", (LONG)name, IoErr());
            continue;
        }
        if (Info(lock, &volume_info))
        {
            Printf("info volume %s used %ld free %ld total %ld blocks of %ld\n", (LONG)name,
                   volume_info.id_NumBlocksUsed,
                   volume_info.id_NumBlocks - volume_info.id_NumBlocksUsed,
                   volume_info.id_NumBlocks, volume_info.id_BytesPerBlock);
            lines++;
        }
        UnLock(lock);
    }
    test_flush();
    info_long("mounted volumes", mounted);
    check(mounted > 0, "c_volumes finds a mounted volume");
    check_long("every mounted volume gives an Info line", lines, mounted);

    /* 3. Info("SYS:") against the volume list */
    sys = Lock((STRPTR)"SYS:", SHARED_LOCK);
    if (!check(sys != 0, "lock SYS:"))
        goto done;
    check(Info(sys, &sys_info), "Info(SYS:)");
    ok = NameFromLock(sys, (STRPTR)buffer, sizeof(buffer));
    check(ok, "NameFromLock(SYS:)");
    for (i = 0; ok && buffer[i] && buffer[i] != ':' && i < 255; i++)
        volume[i] = buffer[i];
    volume[i] = 0;
    if (!ok || buffer[i] != ':')
        volume[0] = 0;
    Printf("info SYS: is on volume \"%s\"\n", (LONG)volume);
    check(volume[0] && listed(names, used, volume), "c_volumes lists the volume of SYS:");
    fl = (struct FileLock *)BADDR(sys);
    sys_task = (ULONG)fl->fl_Task;
    check(guest_port(sys_task), "the SYS: lock names a readable handler port");
    view = (struct DosList *)BADDR(fl->fl_Volume);
    check(view && view->dol_Type == DLT_VOLUME && bstr_name(view->dol_Name, buffer, &length) &&
          !strcmp(buffer, volume) && (ULONG)view->dol_Task == sys_task,
          "the fl_Volume view of SYS: has the volume's name and handler");
    info_long("Info(SYS:) id_VolumeNode", (LONG)sys_info.id_VolumeNode);
    if (sys_info.id_VolumeNode)
    {
        struct DosList *node = (struct DosList *)BADDR(sys_info.id_VolumeNode);
        check(node->dol_Type == DLT_VOLUME && bstr_name(node->dol_Name, buffer, &length) &&
              !strcmp(buffer, volume), "a nonzero id_VolumeNode is the volume of SYS:");
    }
    strcpy(buffer, volume);
    strcat(buffer, ":");
    lock = volume[0] ? Lock((STRPTR)buffer, SHARED_LOCK) : 0;
    if (check(lock != 0, "lock the volume of SYS: by name"))
    {
        check(Info(lock, &volume_info) &&
              volume_info.id_NumBlocks == sys_info.id_NumBlocks &&
              volume_info.id_BytesPerBlock == sys_info.id_BytesPerBlock &&
              volume_info.id_DiskType == sys_info.id_DiskType,
              "Info of the volume by name matches Info(SYS:)");
        check((ULONG)((struct FileLock *)BADDR(lock))->fl_Task == sys_task,
              "both locks name the same handler port");
        UnLock(lock);
    }
    dp = GetDeviceProc((STRPTR)"SYS:", NULL);
    check(dp && (ULONG)dp->dvp_Port == sys_task, "GetDeviceProc(SYS:) gives the same handler port");
    if (dp)
        FreeDeviceProc(dp);

    /* 4. One read lock on every list: handle, layout and filters */
    SetIoErr(SENTINEL);
    list = LockDosList(LDF_ALL | LDF_READ);
    error = IoErr();
    if (!check(list != NULL && aligned(list), "LockDosList(LDF_ALL|LDF_READ) gives an aligned handle"))
        goto done;
    check_long("a read lock leaves IoErr alone", error, SENTINEL);
    count = walk(list, LDF_ALL, walked);
    info_long("entries", count);
    check(count > 0 && count <= WALK_LIMIT, "the walk of every entry ends");
    if (count > RECORD_LIMIT)
        info_long("entries beyond the fixture's record", count - RECORD_LIMIT);
    for (i = 0; i < 5; i++)
        by_type[i] = 0;
    ok = TRUE;
    for (i = 0; i < count && i < RECORD_LIMIT; i++)
    {
        struct Entry *e = &entries[i];
        e->dl = walked[i];
        e->type = e->dl->dol_Type;
        e->task = (ULONG)e->dl->dol_Task;
        if (!bstr_name(e->dl->dol_Name, e->name, &e->length))
            ok = FALSE;
        if (e->type >= DLT_DEVICE && e->type <= DLT_NONBINDING)
            by_type[e->type]++;
        Printf("info entry %ld type %ld %s \"%s\"\n", i, e->type,
               (LONG)(e->task ? "task" : "no task"), (LONG)e->name);
    }
    test_flush();
    check(ok, "every name is a NUL-terminated classic BSTR of 1 to 255 bytes");
    for (i = 0; i < 5; i++)
    {
        static const char *const label[5] =
        { "devices", "directory assigns", "volumes", "late assigns", "nonbinding assigns" };
        info_long(label[i], by_type[i]);
    }
    n = count < RECORD_LIMIT ? count : RECORD_LIMIT;

    /* The handle is no entry; its first longword is the classic BPTR of
       the first entry, and dol_Next links the entries in walk order */
    dl = (struct DosList *)BADDR(at(list, 0));
    for (i = 0; dl && i < n && dl == entries[i].dl; i++)
        dl = (struct DosList *)BADDR(dl->dol_Next);
    check(i == n && (count > RECORD_LIMIT || dl == NULL),
          "a dol_Next walk from the handle gives the NextDosEntry order");
    check(find_recorded(list, count) < 0, "the handle is not an entry");

    ok = TRUE;
    ports_ok = TRUE;
    for (i = 0; i < n; i++)
    {
        struct Entry *e = &entries[i];
        if (!aligned(e->dl) || e->type < DLT_DEVICE || e->type > DLT_NONBINDING)
            ok = FALSE;
        if (e->task && !guest_port(e->task))
            ports_ok = FALSE;
    }
    check(ok, "every entry is aligned and of a classic type 0 to 4");
    check(ports_ok, "every nonzero dol_Task is a readable message port");

    ok = TRUE;
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_DEVICE &&
            (at(entries[i].dl, 12) || at(entries[i].dl, 28) || at(entries[i].dl, 32) ||
             at(entries[i].dl, 36) ||
             (at(entries[i].dl, 16) && !bstr_ok(at(entries[i].dl, 16)))))
            ok = FALSE;
    check(ok, "devices expose no lock, startup, seglist or global vector, and a handler name only as a BSTR");

    ok = TRUE;
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_VOLUME)
        {
            volumes++;
            if (at(entries[i].dl, 12) || at(entries[i].dl, 28) ||
                (LONG)at(entries[i].dl, 16) < 0 || at(entries[i].dl, 20) >= 1440 ||
                at(entries[i].dl, 24) >= 50 * 60)
                ok = FALSE;
        }
    check(ok, "volumes have a valid date and no lock or lock list");

    ok = TRUE;
    for (i = 0; i < n; i++)
    {
        if (entries[i].type == DLT_DIRECTORY && at(entries[i].dl, 20))
            ok = FALSE;
        if ((entries[i].type == DLT_LATE || entries[i].type == DLT_NONBINDING) &&
            (entries[i].task || (at(entries[i].dl, 16) && !c_string(at(entries[i].dl, 16)))))
            ok = FALSE;
        if (entries[i].type == DLT_DIRECTORY)
            Printf("info assign \"%s\" dol_Lock 0x%08lx\n", (LONG)entries[i].name, at(entries[i].dl, 12));
    }
    test_flush();
    check(ok, "assigns expose no AssignList, and late and nonbinding ones no task and a bounded path");

    j = 0;
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_VOLUME && entries[i].task)
            j++;
    check_long("mounted volumes in the full list", j, mounted);

    {
        static const ULONG filter[3] = { LDF_DEVICES, LDF_ASSIGNS, LDF_VOLUMES };
        static const char *const what[3] =
        { "LDF_DEVICES gives the devices in list order",
          "LDF_ASSIGNS gives the three assign types in list order",
          "LDF_VOLUMES gives the volumes in list order" };
        for (j = 0; j < 3; j++)
        {
            LONG k = 0, got = walk(list, filter[j], walked);
            ok = TRUE;
            for (i = 0; i < n; i++)
                if (matches(entries[i].type, filter[j]))
                {
                    if (k >= got || k >= RECORD_LIMIT || walked[k] != entries[i].dl)
                        ok = FALSE;
                    k++;
                }
            check(ok && (count > RECORD_LIMIT || k == got), what[j]);
        }
    }
    check(NextDosEntry(list, 0) == NULL, "NextDosEntry with no type flag gives nothing");
    check(NextDosEntry(list, LDF_READ | LDF_ENTRY | LDF_DELETE) == NULL,
          "lock mode bits are no type flags for NextDosEntry");
    if (n > 0 && count <= RECORD_LIMIT)
        check(NextDosEntry(entries[n - 1].dl, LDF_ALL) == NULL, "the last entry has no next");

    /* 5. FindDosEntry: starts after its argument, ignores case and one
       trailing colon, matches the whole name, and filters types */
    found = FindDosEntry(list, (STRPTR)"SYS", LDF_ASSIGNS);
    check(found && matches(found->dol_Type, LDF_ASSIGNS) && find_recorded(found, count) >= 0 &&
          bstr_name(found->dol_Name, buffer, &length) && same_name(buffer, "SYS"),
          "FindDosEntry finds the SYS assign");
    if (found)
    {
        check((ULONG)found->dol_Task == sys_task, "the SYS assign has the handler port of SYS:");
        check(FindDosEntry(list, (STRPTR)"sys", LDF_ASSIGNS) == found, "FindDosEntry ignores case");
        check(FindDosEntry(list, (STRPTR)"SYS:", LDF_ASSIGNS) == found, "FindDosEntry ignores one trailing colon");
        check(FindDosEntry(found, (STRPTR)"SYS", LDF_ASSIGNS) == NULL, "FindDosEntry continues after its argument");
    }
    dl = FindDosEntry(list, (STRPTR)"SYS", LDF_VOLUMES);
    check(!dl || dl->dol_Type == DLT_VOLUME, "FindDosEntry keeps to the type flags");
    dl = FindDosEntry(list, (STRPTR)"SY", LDF_ALL);
    check(!dl || (bstr_name(dl->dol_Name, buffer, &length) && same_name(buffer, "SY")),
          "a prefix does not match a longer name");
    check(FindDosEntry(list, (STRPTR)"KaffeineNoSuchDosEntry", LDF_ALL) == NULL, "an unknown name gives nothing");
    check(FindDosEntry(NULL, (STRPTR)"SYS", LDF_ASSIGNS) == NULL, "FindDosEntry(NULL) gives nothing");
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    check(FindDosEntry(list, (STRPTR)long_name, LDF_ALL) == NULL, "a name of 299 bytes gives nothing");
    ok = TRUE;
    for (i = 0; i < n; i++)
    {
        ULONG flag = entries[i].type == DLT_DEVICE ? LDF_DEVICES :
                     entries[i].type == DLT_VOLUME ? LDF_VOLUMES : LDF_ASSIGNS;
        found = FindDosEntry(list, (STRPTR)entries[i].name, flag);
        for (j = 0; j < i; j++)
            if (matches(entries[j].type, flag) && same_name(entries[j].name, entries[i].name))
                break;
        if (found != entries[j].dl)
            ok = FALSE;
    }
    check(ok, "FindDosEntry finds every entry, or the first one of its type with that name");
    ok = FALSE;
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_VOLUME && entries[i].task == sys_task &&
            !strcmp(entries[i].name, volume))
        {
            ok = TRUE;
            info_long("the fl_Volume view of SYS: is this very entry", entries[i].dl == view);
        }
    check(ok, "the volume of SYS: is in the list with its handler port");
    j = 0;
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_DEVICE && entries[i].task == sys_task)
            j++;
    info_long("devices with the handler port of SYS:", j);
    SetIoErr(SENTINEL);
    walk(list, LDF_ALL, walked);
    FindDosEntry(list, (STRPTR)"SYS", LDF_ASSIGNS);
    FindDosEntry(list, (STRPTR)"KaffeineNoSuchDosEntry", LDF_ALL);
    error = IoErr();
    check_long("walks to the end and searches leave IoErr alone", error, SENTINEL);

    /* 6. DOS calls inside the read lock, nested locks and a refused write */
    lock = Lock((STRPTR)"SYS:", SHARED_LOCK);
    check(lock && Info(lock, &volume_info), "Lock and Info of SYS: inside the read lock");
    if (lock)
        UnLock(lock);
    nested = LockDosList(LDF_DEVICES | LDF_READ);
    check(nested == list, "a nested read lock gives the same handle");
    check(AttemptLockDosList(LDF_VOLUMES | LDF_READ) == list, "a nested attempted read lock gives the same handle");
    check(LockDosList(LDF_ENTRY | LDF_READ) == list, "a nested entry read lock gives the same handle");
    /* On AROS an unlock with LDF_WRITE releases the semaphore of the read
       locks. Kaffeine refused the write lock, so its unlock must leave
       the read locks held. */
    check(AttemptLockDosList(LDF_WRITE | LDF_VOLUMES) == NULL, "a write lock inside a read lock is refused");
    UnLockDosList(LDF_WRITE | LDF_VOLUMES);
    UnLockDosList(LDF_ENTRY | LDF_READ);
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    UnLockDosList(LDF_DEVICES | LDF_READ);
    m = walk(list, LDF_ALL, walked);
    ok = m == count;
    for (i = 0; ok && i < n; i++)
        if (walked[i] != entries[i].dl || !bstr_name(walked[i]->dol_Name, buffer, &length) ||
            strcmp(buffer, entries[i].name))
            ok = FALSE;
    check(ok, "the outer snapshot is intact after nested locks and a refused write");
    for (i = 0; i < n; i++)
        if (entries[i].type == DLT_VOLUME && entries[i].task)
        {
            saved_bstr = (const UBYTE *)BADDR(entries[i].dl->dol_Name);
            saved_length = entries[i].length;
            strcpy(saved_name, entries[i].name);
            break;
        }
    UnLockDosList(LDF_ALL | LDF_READ);
    check(saved_bstr && saved_bstr[0] == saved_length && !memcmp(saved_bstr + 1, saved_name, saved_length) &&
          saved_bstr[saved_length + 1] == 0,
          "a name stays readable after the final unlock, until the next lock");
    UnLockDosList(LDF_ALL | LDF_READ);
    check(TRUE, "a surplus unlock returns to the program");

    /* 7. Fresh locks after the final unlock */
    list = LockDosList(LDF_READ);
    check(list != NULL, "LDF_READ without a list flag gives a handle, as on AROS");
    if (list)
        check_long("the handle of LDF_READ walks the volumes", walk(list, LDF_VOLUMES, walked), volumes);
    UnLockDosList(LDF_READ);
    list = AttemptLockDosList(LDF_ALL | LDF_READ);
    check(list != NULL, "a fresh attempted read lock succeeds");
    if (list)
    {
        check_long("the fresh snapshot has as many entries", walk(list, LDF_ALL, walked), count);
        UnLockDosList(LDF_ALL | LDF_READ);
    }

    /* 8. The Viewer asks for volumes every 30 seconds: repeated snapshots
       must give the same names and give their memory back */
    avail = AvailMem(MEMF_ANY);
    ok = TRUE;
    for (i = 0; i < REPEATS; i++)
    {
        LONG again_used = volume_names(again, &overflow);
        if (overflow || again_used != used || memcmp(again, names, used))
            ok = FALSE;
    }
    check(ok, "64 repeated c_volumes walks give the same names");
    info_long("free memory change over the repeated walks", (LONG)(AvailMem(MEMF_ANY) - avail));
    check(AvailMem(MEMF_ANY) + 16384 >= avail, "repeated snapshots give their memory back");

done:
    if (sys)
        UnLock(sys);
    return test_done("dos_doslist");
}
