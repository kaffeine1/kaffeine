/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    DOS data, buffer, string, formatter, object and process thunks.
*/
#include <aros/debug.h>
#include <dos/dostags.h>
#include <dos/dosasl.h>
#include <dos/exall.h>
#include <dos/var.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <dos/stdio.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
#include <stdint.h>
#include <utility/tagitem.h>

#include "kaffeine_intern.h"
#include "kaffeine_locks.h"
#include "kaffeine_callback.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_messages.h"
#include "kaffeine_cli.h"
#include "kaffeine_format.h"
#include "kaffeine_offsets.h"
#include "kaffeine_readargs.h"
#include "kaffeine_shadow.h"
#include "kaffeine_sched.h"
#include "kaffeine_taglist.h"
#include "kaffeine_segments.h"
#include "kaffeine_process_tags.h"
#include "kaffeine_thunks_dos.h"

#define GUEST_FIB_SIZE 260
#define MAX_SYSTEM_TAG_STEPS 256

/* Validate the whole destination before making any writes. Subtracting the
   address avoids wrapping a guest ULONG when the caller supplies garbage. */
static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    if (!addr || addr >= ctx->mem_size || ctx->mem_size - addr < size ||
        Kaffeine_LockProtected(ctx, addr, size))
        return FALSE;
    return TRUE;
}

static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    const char *s;

    if (!guest_range(ctx, addr, 1))
        return NULL;
    s = (const char *)ctx->mem + addr;
    return memchr(s, 0, ctx->mem_size - addr) ? s : NULL;
}

static IPTR bad_dos_buffer(LONG result)
{
    SetIoErr(ERROR_BAD_NUMBER);
    return (IPTR)result;
}

static BOOL guest_overlap(ULONG a, ULONG asize, ULONG b, ULONG bsize);

/* NDK 3.2 layouts: InfoData is nine longwords; DateStamp three;
   DateTime has byte format/flags at 12/13 and APTRs at 14/18/22;
   CSource has an APTR and two LONGs at 0/4/8. No native casts of these. */
#define GUEST_INFO_SIZE 36
#define GUEST_DATE_SIZE 12
#define GUEST_DATETIME_SIZE 26
#define GUEST_CSOURCE_SIZE 12

static void read_date(struct KaffeineContext *ctx, ULONG addr, struct DateStamp *date)
{
    date->ds_Days = (LONG)m68k_read32(ctx, addr);
    date->ds_Minute = (LONG)m68k_read32(ctx, addr + 4);
    date->ds_Tick = (LONG)m68k_read32(ctx, addr + 8);
}

static void write_date(struct KaffeineContext *ctx, ULONG addr, const struct DateStamp *date)
{
    m68k_write32(ctx, addr, (ULONG)date->ds_Days);
    m68k_write32(ctx, addr + 4, (ULONG)date->ds_Minute);
    m68k_write32(ctx, addr + 8, (ULONG)date->ds_Tick);
}

/* -144: Exit(D1=returnCode). Use the same stop path as semaphore_failure(),
   including when a future guest callback invokes Exit(). Native cleanup
   still runs after Execute(), without relying on library C++ unwinding. */
IPTR kaffeine_dos_Exit(struct KaffeineContext *ctx, void *cpu)
{
    LONG code = (LONG)THUNK_D(1);

    ctx->running = FALSE;
    ctx->exit_code = code;
    return (IPTR)code;
}

/* -114: Info(D1=lock, D2=parameterBlock) -> D0=success */
IPTR kaffeine_dos_Info(struct KaffeineContext *ctx, void *cpu)
{
    ULONG out = THUNK_D(2);
    struct InfoData info = { 0 };
    LONG result;
    BPTR lock;

    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;
    if (!guest_range(ctx, out, GUEST_INFO_SIZE))
        return bad_dos_buffer(0);
    /* Like Examine(), lock zero selects the boot filesystem. */
    result = Info(lock, &info);
    if (result)
    {
        m68k_write32(ctx, out, (ULONG)info.id_NumSoftErrors);
        m68k_write32(ctx, out + 4, (ULONG)info.id_UnitNumber);
        m68k_write32(ctx, out + 8, (ULONG)info.id_DiskState);
        m68k_write32(ctx, out + 12, (ULONG)info.id_NumBlocks);
        m68k_write32(ctx, out + 16, (ULONG)info.id_NumBlocksUsed);
        m68k_write32(ctx, out + 20, (ULONG)info.id_BytesPerBlock);
        m68k_write32(ctx, out + 24, (ULONG)info.id_DiskType);
        /* A native DosList BPTR is neither a lock token nor guest memory.
           There is no DosList bridge yet, so expose no volume node. */
        m68k_write32(ctx, out + 28, 0);
        m68k_write32(ctx, out + 32, (ULONG)info.id_InUse);
    }
    return (IPTR)result;
}

/* -120: CreateDir(D1=name) -> D0=lock */
IPTR kaffeine_dos_CreateDir(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return Kaffeine_LockAdopt(ctx, CreateDir(name));
}

/* -366: SetVBuf(D1=fh, D2=buff, D3=type, D4=size) -> D0=0 or EOF */
IPTR kaffeine_dos_SetVBuf(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh = (BPTR)(IPTR)THUNK_D(1);
    ULONG buffer = THUNK_D(2), needed;
    LONG type = (LONG)THUNK_D(3), size = (LONG)THUNK_D(4);

    if (!fh || size < -1 || (size >= 0 && (ULONG)size > ctx->mem_size) ||
        (type != BUF_LINE && type != BUF_FULL && type != BUF_NONE))
        return bad_dos_buffer(-1);
    if (size > KAFFEINE_FILE_BUFFER_LIMIT) return bad_dos_buffer(-1);
    needed = size < 208 ? 208 : (ULONG)size;
    if (size != -1 && buffer &&
        ((buffer & 3) || !guest_range(ctx, buffer, needed)))
        return bad_dos_buffer(-1);
    /* Native DOS keeps fh_Buf beyond this call, possibly beyond guest Exit.
       Give it an owned buffer even when a valid guest buffer was supplied.
       Size -1 changes only the mode and does not inspect the guest buffer. */
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    IPTR result = (IPTR)SetVBuf(fh, NULL, type, size);
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? result : 20;
}

/* -396: SetFileDate(D1=name, D2=date) -> D0=success */
IPTR kaffeine_dos_SetFileDate(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    ULONG addr = THUNK_D(2);
    struct DateStamp date;

    if (!name || !guest_range(ctx, addr, GUEST_DATE_SIZE))
        return bad_dos_buffer(0);
    read_date(ctx, addr, &date);
    return (IPTR)SetFileDate(name, &date);
}

/* -414: SplitName(D1=name, D2=separator, D3=buf, D4=oldpos, D5=size) */
IPTR kaffeine_dos_SplitName(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_D(1), buffer = THUNK_D(3);
    LONG pos = (LONG)THUNK_D(4), size = (LONG)THUNK_D(5);
    const char *name = guest_string(ctx, addr);

    /* Native code adds oldpos to name and decrements size unconditionally.
       Its terminator and subsequent separator test also forbid overlap. */
    if (!name || pos < 0 || (ULONG)pos > strlen(name) || size <= 0 ||
        !guest_range(ctx, buffer, (ULONG)size) ||
        guest_overlap(addr, (ULONG)strlen(name) + 1, buffer, (ULONG)size))
        return bad_dos_buffer(-1);
    return (IPTR)SplitName(name, THUNK_D(2), (STRPTR)(ctx->mem + buffer), pos, size);
}

static ULONG argument_field(struct KaffeineContext *ctx)
{
    ULONG task;

    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        return 0;
    task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    return guest_range(ctx, task, M68K_PR_ARGUMENTS + 4) ? task + M68K_PR_ARGUMENTS : 0;
}

/* -534: GetArgStr() -> D0=guest pr_Arguments, including guest writes */
IPTR kaffeine_dos_GetArgStr(struct KaffeineContext *ctx, void *cpu)
{
    ULONG field = argument_field(ctx), addr;

    (void)cpu;
    if (!field)
        return bad_dos_buffer(0);
    addr = m68k_read32(ctx, field);
    if (addr && !guest_string(ctx, addr))
        return bad_dos_buffer(0);
    return addr;
}

/* -540: SetArgStr(D1=string) -> D0=old guest string. NULL is legal. */
IPTR kaffeine_dos_SetArgStr(struct KaffeineContext *ctx, void *cpu)
{
    ULONG field = argument_field(ctx), addr = THUNK_D(1), old;

    if (!field || (addr && !guest_string(ctx, addr)))
        return bad_dos_buffer(0);
    old = m68k_read32(ctx, field);
    if (old && !guest_string(ctx, old))
        return bad_dos_buffer(0);
    m68k_write32(ctx, field, addr);
    /* m68k_argptr/argsize remain the immutable startup registers. ReadArgs
       and native process arguments retain their existing separate lifetime. */
    return old;
}

static IPTR set_cli_name(struct KaffeineContext *ctx, ULONG addr, ULONG offset, ULONG slot)
{
    const char *name = guest_string(ctx, addr);
    ULONG cli, dest, len;

    if (!name)
        return bad_dos_buffer(0);
    len = (ULONG)strlen(name);
    if (len > 255)
    {
        SetIoErr(ERROR_LINE_TOO_LONG);
        return 0;
    }
    cli = Kaffeine_GetCLI(ctx);
    if (!cli)
        return 0;
    if (!guest_range(ctx, cli, M68K_SIZEOF_CLI + 3 * M68K_CLI_STRING_SLOT))
        return bad_dos_buffer(0);
    dest = cli + M68K_SIZEOF_CLI + slot * M68K_CLI_STRING_SLOT;
    /* A setter always uses its owned slot, even after a guest repoints the
       field. memmove also allows the name to alias this slot or the CLI. */
    memmove(ctx->mem + dest + 1, name, len);
    ctx->mem[dest] = (UBYTE)len;
    ctx->mem[dest + len + 1] = 0;
    m68k_write32(ctx, cli + offset, dest >> 2);
    if (offset == M68K_CLI_COMMANDNAME)
    {
        memcpy(ctx->program_name, ctx->mem + dest + 1, len);
        ctx->program_name[len] = 0;
    }
    return (IPTR)(LONG)DOSTRUE;
}

static IPTR get_cli_name(struct KaffeineContext *ctx, ULONG buffer, LONG size, ULONG offset)
{
    ULONG cli, bptr, addr, len;
    BOOL fits;

    if (size <= 0 || !guest_range(ctx, buffer, (ULONG)size))
        return bad_dos_buffer(0);
    cli = Kaffeine_GetCLI(ctx);
    if (!cli)
    {
        ctx->mem[buffer] = 0;
        return 0;
    }
    if (!guest_range(ctx, cli, M68K_SIZEOF_CLI))
        return bad_dos_buffer(0);
    bptr = m68k_read32(ctx, cli + offset);
    /* Check before shifting so a wrapped BPTR cannot become a valid APTR. */
    if (!bptr || bptr > 0x3FFFFFFFUL || !guest_range(ctx, bptr << 2, 1))
        return bad_dos_buffer(0);
    addr = bptr << 2;
    len = ctx->mem[addr];
    if (!guest_range(ctx, addr, len + 1))
        return bad_dos_buffer(0);
    /* Match the >= len-1 test in AROS GetPrompt/GetCurrentDirName, including
       the exact-fit failure. Never reproduce their len <= 0 underflow. */
    fits = len < (ULONG)size - 1;
    if (!fits)
        len = (ULONG)size - 1;
    memmove(ctx->mem + buffer, ctx->mem + addr + 1, len);
    ctx->mem[buffer + len] = 0;
    if (!fits)
        SetIoErr(ERROR_LINE_TOO_LONG);
    return fits ? (IPTR)(LONG)DOSTRUE : 0;
}

/* These five calls change/read only the shared guest CLI, not the host Shell
   or the native process's directory lock. The initial prompt/set-name are
   empty, as this partial CLI does not import native mutable strings. */
IPTR kaffeine_dos_SetCurrentDirName(struct KaffeineContext *ctx, void *cpu)
{
    return set_cli_name(ctx, THUNK_D(1), M68K_CLI_SETNAME, 1);
}

IPTR kaffeine_dos_GetCurrentDirName(struct KaffeineContext *ctx, void *cpu)
{
    return get_cli_name(ctx, THUNK_D(1), (LONG)THUNK_D(2), M68K_CLI_SETNAME);
}

IPTR kaffeine_dos_SetProgramName(struct KaffeineContext *ctx, void *cpu)
{
    return set_cli_name(ctx, THUNK_D(1), M68K_CLI_COMMANDNAME, 0);
}

IPTR kaffeine_dos_SetPrompt(struct KaffeineContext *ctx, void *cpu)
{
    return set_cli_name(ctx, THUNK_D(1), M68K_CLI_PROMPT, 2);
}

IPTR kaffeine_dos_GetPrompt(struct KaffeineContext *ctx, void *cpu)
{
    return get_cli_name(ctx, THUNK_D(1), (LONG)THUNK_D(2), M68K_CLI_PROMPT);
}

/* -612: AssignLock(D1=name, D2=lock). Zero removes the assign. */
IPTR kaffeine_dos_AssignLock(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    struct KaffeineLockTransfer lease;
    LONG result;
    if (!Kaffeine_LockTransferBegin(ctx, THUNK_D(2), &lease)) return 20;
    result = AssignLock(name, lease.native);
    Kaffeine_LockTransferEnd(ctx, &lease, result != 0);
    return result;
}

static IPTR assign_path(struct KaffeineContext *ctx, void *cpu, BOOL late)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    const char *path = guest_string(ctx, THUNK_D(2));
    char *copy;
    ULONG len;
    LONG result;
    SIPTR err;

    if (!name || !path)
        return bad_dos_buffer(0);
    len = (ULONG)strlen(path);
    if (len > 0xFFFFFFFFUL - 2)
        return bad_dos_buffer(0);
    /* Both native routines copy strlen(path)+2 bytes, one past the NUL.
       Supply that extra byte even for a string ending at the arena edge.
       DOS copies the path, so it never retains this scratch allocation. */
    copy = (char *)AllocVec(len + 2, MEMF_ANY);
    if (!copy)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memcpy(copy, path, len + 1);
    copy[len + 1] = 0;
    result = late ? AssignLate(name, copy) : AssignPath(name, copy);
    err = IoErr();
    FreeVec(copy);
    SetIoErr(err);
    return (IPTR)result;
}

IPTR kaffeine_dos_AssignLate(struct KaffeineContext *ctx, void *cpu)
{
    return assign_path(ctx, cpu, TRUE);
}

IPTR kaffeine_dos_AssignPath(struct KaffeineContext *ctx, void *cpu)
{
    return assign_path(ctx, cpu, FALSE);
}

IPTR kaffeine_dos_AssignAdd(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    struct KaffeineLockTransfer lease;
    LONG result;
    if (!name || !THUNK_D(2)) return bad_dos_buffer(0);
    if (!Kaffeine_LockTransferBegin(ctx, THUNK_D(2), &lease)) return 20;
    result = AssignAdd(name, lease.native);
    Kaffeine_LockTransferEnd(ctx, &lease, result != 0);
    return result;
}

IPTR kaffeine_dos_RemAssignList(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    struct KaffeineLockTransfer lease;
    LONG result;
    /* The assign releases its own reference; the caller must hold one. */
    if (!Kaffeine_LockTransferBegin(ctx, THUNK_D(2), &lease)) return 20;
    result = RemAssignList(name, lease.native);
    Kaffeine_LockTransferEnd(ctx, &lease, FALSE);
    return result;
}

IPTR kaffeine_dos_IsFileSystem(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)IsFileSystem(name);
}

IPTR kaffeine_dos_AddBuffers(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)AddBuffers(name, (LONG)THUNK_D(2));
}

/* -738: CompareDates(D1=date1, D2=date2). Positive means date2 is later. */
IPTR kaffeine_dos_CompareDates(struct KaffeineContext *ctx, void *cpu)
{
    ULONG a = THUNK_D(1), b = THUNK_D(2);
    struct DateStamp first, second;
    int64_t diff;

    if (!guest_range(ctx, a, GUEST_DATE_SIZE) || !guest_range(ctx, b, GUEST_DATE_SIZE))
        return bad_dos_buffer(0);
    read_date(ctx, a, &first);
    read_date(ctx, b, &second);
    diff = (int64_t)second.ds_Days - first.ds_Days;
    if (!diff)
        diff = (int64_t)second.ds_Minute - first.ds_Minute;
    if (!diff)
        diff = (int64_t)second.ds_Tick - first.ds_Tick;
    /* The native subtraction is signed LONG arithmetic. Preserve its exact
       difference when safe and the documented ordering for extreme inputs. */
    if (diff < INT32_MIN || diff > INT32_MAX)
        return diff < 0 ? (IPTR)-1 : 1;
    return (IPTR)CompareDates(&first, &second);
}

static BOOL ascii_prefix(const char *string, const char *prefix)
{
    while (*prefix)
    {
        unsigned char c = (unsigned char)*string++;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != (unsigned char)*prefix++)
            return FALSE;
    }
    return TRUE;
}

/* StrToLong() accumulates positive signed LONGs even for negative input.
   Check just the numbers DOS will consume, with headroom for its ASCII
   digit addition before subtracting '0'. 0 means syntax failure, which
   DOS handles without arithmetic; -1 means unsafe native arithmetic. */
static int date_number(const char **string, LONG *number)
{
    const char *s = *string;
    ULONG value = 0;
    BOOL negative;

    while (*s == ' ' || *s == '\t')
        s++;
    negative = *s == '-';
    if (*s == '+' || *s == '-')
        s++;
    if (*s < '0' || *s > '9')
        return 0;
    while (*s >= '0' && *s <= '9')
    {
        ULONG digit = (ULONG)(*s++ - '0');

        if (value > ((ULONG)INT32_MAX - '0' - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    *number = negative ? -(LONG)value : (LONG)value;
    *string = s;
    return 1;
}

static BOOL safe_time_input(const char *s)
{
    LONG value;
    int status = date_number(&s, &value);

    if (status <= 0)
        return status == 0;
    if (value < 0 || value > 23 || *s++ != ':')
        return TRUE; /* Native syntax/range failure before the next number. */
    status = date_number(&s, &value);
    if (status <= 0)
        return status == 0;
    if (value < 0 || value > 59 || !*s || *s++ != ':')
        return TRUE;
    return date_number(&s, &value) >= 0;
}

/* No native StrToDate() call may reach Dos_DayTable[month-1] with an invalid
   month or overflow its LONG calendar intermediates. Syntax errors that DOS
   detects before that point remain native failures with native IoErr(). */
static BOOL safe_date_input(const char *s, UBYTE format)
{
    static const char *const words[] = {
        "tomorrow", "today", "yesterday", "sunday", "monday", "tuesday",
        "wednesday", "thursday", "friday", "saturday"
    };
    static const char *const months[] = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
    };
    static const LONG day_table[] = { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 };
    const char *spec = format == FORMAT_INT ? "yMd" :
        format == FORMAT_USA ? "mdy" : format == FORMAT_CDN ? "dmy" : "dMy";
    LONG year = 0, month = 0, day = 0;
    int64_t days, product;
    unsigned i;
    BOOL leap;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (ascii_prefix(s, words[i]))
            return TRUE;
    for (i = 0; i < 3; i++)
    {
        LONG value = 0;

        if (spec[i] == 'm')
        {
            unsigned m;

            for (m = 0; m < 12 && !ascii_prefix(s, months[m]); m++)
                ;
            if (m == 12)
                return TRUE;
            value = (LONG)m + 1;
            s += 3;
        }
        else
        {
            int status = date_number(&s, &value);

            if (status <= 0)
                return status == 0;
        }
        if (spec[i] == 'y') year = value;
        else if (spec[i] == 'd') day = value;
        else month = value;
        if (i < 2 && *s++ != '-')
            return TRUE;
    }
    if (year < 100)
        year += 1900;
    if (year < 1978)
        return TRUE; /* DOS fails before indexing or calendar arithmetic. */
    if (month < 1 || month > 12)
        return FALSE;
    leap = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
    days = (int64_t)day - 1;
    product = ((int64_t)year - 1978) * 365;
    if (product > INT32_MAX || days < INT32_MIN)
        return FALSE;
    days += product;
    if (days < INT32_MIN || days > INT32_MAX)
        return FALSE;
    year--;
    days += year / 4 - year / 100 + year / 400 - (494 - 19 + 4);
    if (days < INT32_MIN || days > INT32_MAX)
        return FALSE;
    days += day_table[month - 1];
    if (days < INT32_MIN || days > INT32_MAX)
        return FALSE;
    if (!leap && month >= 3)
        days--;
    return days >= INT32_MIN;
}

static IPTR date_strings(struct KaffeineContext *ctx, void *cpu, BOOL parse)
{
    ULONG addr = THUNK_D(1), strings[3], i;
    struct DateTime date;
    STRPTR *fields[3] = { &date.dat_StrDay, &date.dat_StrDate, &date.dat_StrTime };
    LONG result;

    if (!guest_range(ctx, addr, GUEST_DATETIME_SIZE))
        return bad_dos_buffer(0);
    read_date(ctx, addr, &date.dat_Stamp);
    date.dat_Format = ctx->mem[addr + 12];
    date.dat_Flags = ctx->mem[addr + 13];
    for (i = 0; i < 3; i++)
    {
        strings[i] = m68k_read32(ctx, addr + 14 + i * 4);
        *fields[i] = NULL;
        if (!strings[i])
            continue;
        if (parse)
        {
            const char *s = guest_string(ctx, strings[i]);
            if (!s)
                return bad_dos_buffer(0);
            /* Native Strnicmp/StrToLong stop at NUL. LEN_DATSTRING is an
               output capacity, not a maximum input string length. These
               synchronous reads cannot outlive the validated guest arena. */
            *fields[i] = (STRPTR)s;
        }
        else
        {
            ULONG j;

            if (!guest_range(ctx, strings[i], LEN_DATSTRING) ||
                guest_overlap(addr, GUEST_DATETIME_SIZE, strings[i], LEN_DATSTRING))
                return bad_dos_buffer(0);
            for (j = 0; j < i; j++)
                if (strings[j] && guest_overlap(strings[j], LEN_DATSTRING, strings[i], LEN_DATSTRING))
                    return bad_dos_buffer(0);
            *fields[i] = (STRPTR)(ctx->mem + strings[i]);
        }
    }
    if (!parse)
        return (IPTR)DateToStr(&date);
    if ((date.dat_StrDate && !safe_date_input((const char *)date.dat_StrDate, date.dat_Format)) ||
        (date.dat_StrTime && !safe_time_input((const char *)date.dat_StrTime)))
        return bad_dos_buffer(0);
    result = StrToDate(&date);
    /* StrToDate can change ds_Days before failing on the time string. Copy
       that partial result too; never write native pointers back to guest. */
    write_date(ctx, addr, &date.dat_Stamp);
    return (IPTR)result;
}

IPTR kaffeine_dos_DateToStr(struct KaffeineContext *ctx, void *cpu)
{
    return date_strings(ctx, cpu, FALSE);
}

IPTR kaffeine_dos_StrToDate(struct KaffeineContext *ctx, void *cpu)
{
    return date_strings(ctx, cpu, TRUE);
}

/* -804: FindArg(D1=template, D2=keyword). NDK 3.2 names these arguments
   keyword/arg_template in both fd and clib, but its inline sends the first
   C argument to D1. AROS findarg.c and ReadArgs() use template first. Names
   in a prototype do not change that ABI. Do not guess from string contents. */
IPTR kaffeine_dos_FindArg(struct KaffeineContext *ctx, void *cpu)
{
    const char *templ = guest_string(ctx, THUNK_D(1));
    const char *keyword = guest_string(ctx, THUNK_D(2));

    if (!templ || !keyword)
        return bad_dos_buffer(-1);
    return (IPTR)FindArg(templ, keyword);
}

/* -810: ReadItem(D1=buffer, D2=maxchars, D3=CSource or NULL for Input()) */
IPTR kaffeine_dos_ReadItem(struct KaffeineContext *ctx, void *cpu)
{
    ULONG buffer = THUNK_D(1), addr = THUNK_D(3), source_addr;
    LONG size = (LONG)THUNK_D(2), result;
    struct CSource source, *input = NULL;

    /* These two early paths are native ReadItem semantics. In particular,
       size zero writes one NUL but does not inspect or consume the source. */
    if (!buffer)
        return ITEM_NOTHING;
    if (size < 0 || !guest_range(ctx, buffer, size ? (ULONG)size : 1))
        return bad_dos_buffer(ITEM_ERROR);
    if (!size)
    {
        ctx->mem[buffer] = 0;
        return ITEM_NOTHING;
    }
    if (addr)
    {
        if (!guest_range(ctx, addr, GUEST_CSOURCE_SIZE) ||
            guest_overlap(addr, GUEST_CSOURCE_SIZE, buffer, (ULONG)size))
            return bad_dos_buffer(ITEM_ERROR);
        source_addr = m68k_read32(ctx, addr);
        source.CS_Length = (LONG)m68k_read32(ctx, addr + 4);
        source.CS_CurChr = (LONG)m68k_read32(ctx, addr + 8);
        if (source.CS_Length < 0 || source.CS_CurChr < 0 ||
            source.CS_CurChr > source.CS_Length ||
            (source.CS_Length &&
             (!guest_range(ctx, source_addr, (ULONG)source.CS_Length) ||
              guest_overlap(source_addr, (ULONG)source.CS_Length, buffer, (ULONG)size))))
            return bad_dos_buffer(ITEM_ERROR);
        source.CS_Buffer = source.CS_Length ? (STRPTR)(ctx->mem + source_addr) : NULL;
        input = &source;
    }
    ULONG stream_token = 0;
    if (!input && !Kaffeine_FileStreamPrepare(ctx, TRUE, &stream_token)) return ctx->exit_code == 20 ? 20 : ITEM_ERROR;
    result = ReadItem((STRPTR)(ctx->mem + buffer), size, input);
    if (!input && !Kaffeine_FileBufferFinish(ctx, stream_token)) return 20;
    if (input)
        m68k_write32(ctx, addr + 8, (ULONG)source.CS_CurChr);
    return (IPTR)result;
}

/* -942: WriteChars(D1=buf, D2=buflen) -> D0=bytes or EOF */
IPTR kaffeine_dos_WriteChars(struct KaffeineContext *ctx, void *cpu)
{
    ULONG buffer = THUNK_D(1), len = THUNK_D(2);

    if (!len)
        return 0;
    if (!guest_range(ctx, buffer, len))
        return bad_dos_buffer(-1);
    ULONG token;
    if (!Kaffeine_FileStreamPrepare(ctx, FALSE, &token)) return ctx->exit_code == 20 ? 20 : -1;
    IPTR result = (IPTR)WriteChars((CONST_STRPTR)(ctx->mem + buffer), len);
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

/* -996: SetOwner(D1=name, D2=(UID << 16) | GID), D2 is a scalar. */
IPTR kaffeine_dos_SetOwner(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)SetOwner((STRPTR)name, THUNK_D(2));
}

/* -324: FRead(D1=fh, D2=block, D3=blocklen, D4=number) -> D0=blocks or -1 */
IPTR kaffeine_dos_FRead(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh = (BPTR)(IPTR)THUNK_D(1);
    ULONG block = THUNK_D(2), blocklen = THUNK_D(3), number = THUNK_D(4);

    /* A zero product is a no-op even with an unusable pointer or handle.
       Do not let the native ULONG multiplication hide a wrapped product. */
    if (!blocklen || !number)
    {
        SetIoErr(0);
        return 0;
    }
    if (number > (ULONG)0xFFFFFFFFUL / blocklen ||
        !guest_range(ctx, block, blocklen * number) || !fh)
        return bad_dos_buffer(-1);
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    IPTR result = (IPTR)FRead(fh, ctx->mem + block, blocklen, number);
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? result : 20;
}

/* -330: FWrite(D1=fh, D2=block, D3=blocklen, D4=number) -> D0=blocks or -1 */
IPTR kaffeine_dos_FWrite(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh = (BPTR)(IPTR)THUNK_D(1);
    ULONG block = THUNK_D(2), blocklen = THUNK_D(3), number = THUNK_D(4);

    if (!blocklen || !number)
    {
        SetIoErr(0);
        return 0;
    }
    if (number > (ULONG)0xFFFFFFFFUL / blocklen ||
        !guest_range(ctx, block, blocklen * number) || !fh)
        return bad_dos_buffer(-1);
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    IPTR result = (IPTR)FWrite(fh, ctx->mem + block, blocklen, number);
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? result : 20;
}

/* File handles resolve only through private associations. Locks and segment
   lists retain their separate opaque BPTR conventions. Native Read/Write
   report NULL handles themselves; no guessed nonzero handle is forwarded. */

/* Unlike FRead(), native Read() and Write() pass a zero length on to the
   handler, after refusing a NULL handle. No byte moves then, so a buffer
   outside guest memory is replaced by the start of the arena. */
static UBYTE *transfer_buffer(struct KaffeineContext *ctx, ULONG buffer, LONG length)
{
    if (!length)
        return guest_range(ctx, buffer, 1) ? ctx->mem + buffer : ctx->mem;
    if (length < 0 || !guest_range(ctx, buffer, (ULONG)length))
        return NULL;
    return ctx->mem + buffer;
}

/* -42: Read(D1=file, D2=buffer, D3=length) -> D0=actual or -1 */
IPTR kaffeine_dos_Read(struct KaffeineContext *ctx, void *cpu)
{
    LONG length = (LONG)THUNK_D(3);
    UBYTE *buffer = transfer_buffer(ctx, THUNK_D(2), length);

    if (!buffer)
        return bad_dos_buffer(-1);
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    return (IPTR)Read(fh, buffer, length);
}

/* -48: Write(D1=file, D2=buffer, D3=length) -> D0=actual or -1 */
IPTR kaffeine_dos_Write(struct KaffeineContext *ctx, void *cpu)
{
    LONG length = (LONG)THUNK_D(3);
    UBYTE *buffer = transfer_buffer(ctx, THUNK_D(2), length);

    if (!buffer)
        return bad_dos_buffer(-1);
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    return (IPTR)Write(fh, buffer, length);
}

/* -30: Open(D1=name, D2=accessMode) -> D0=file */
IPTR kaffeine_dos_Open(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    ULONG token = Kaffeine_FileImport(ctx, Open((CONST_STRPTR)name, (LONG)THUNK_D(2)), TRUE);
    return !ctx->running && ctx->exit_code == 20 ? 20 : token;
}

/* -84: Lock(D1=name, D2=type) -> D0=lock */
IPTR kaffeine_dos_Lock(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return Kaffeine_LockAdopt(ctx, Lock((CONST_STRPTR)name, (LONG)THUNK_D(2)));
}

/* -72: DeleteFile(D1=name) -> D0=success */
IPTR kaffeine_dos_DeleteFile(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)DeleteFile((CONST_STRPTR)name);
}

/* -78: Rename(D1=oldName, D2=newName) -> D0=success */
IPTR kaffeine_dos_Rename(struct KaffeineContext *ctx, void *cpu)
{
    const char *from = guest_string(ctx, THUNK_D(1));
    const char *to = guest_string(ctx, THUNK_D(2));

    if (!from || !to)
        return bad_dos_buffer(0);
    return (IPTR)Rename((CONST_STRPTR)from, (CONST_STRPTR)to);
}

/* -948: PutStr(D1=string) -> D0=0 or -1 */
IPTR kaffeine_dos_PutStr(struct KaffeineContext *ctx, void *cpu)
{
    const char *s = guest_string(ctx, THUNK_D(1));

    if (!s)
        return bad_dos_buffer(-1);
    ULONG token;
    if (!Kaffeine_FileStreamPrepare(ctx, FALSE, &token)) return ctx->exit_code == 20 ? 20 : -1;
    IPTR result = (IPTR)PutStr((CONST_STRPTR)s);
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

/* -342: FPuts(D1=file, D2=string) -> D0=0 or -1 */
IPTR kaffeine_dos_FPuts(struct KaffeineContext *ctx, void *cpu)
{
    const char *s = guest_string(ctx, THUNK_D(2));

    if (!s)
        return bad_dos_buffer(-1);
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    IPTR result = (IPTR)FPuts(fh, (CONST_STRPTR)s);
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? result : 20;
}

/* -222: Execute(D1=command, D2=input, D3=output) -> D0=success */
IPTR kaffeine_dos_Execute(struct KaffeineContext *ctx, void *cpu)
{
    const char *command = guest_string(ctx, THUNK_D(1));

    if (!command)
        return bad_dos_buffer(0);
    BPTR input, output;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(2), &input) ||
        !Kaffeine_FileResolve(ctx, THUNK_D(3), &output)) return 20;
    if (!Kaffeine_FileAllPrepare(ctx)) return ctx->exit_code == 20 ? 20 : 0;
    IPTR result = (IPTR)Execute((CONST_STRPTR)command, input, output);
    return Kaffeine_FileAllFinish(ctx) ? result : 20;
}

/* -504: RunCommand(D1=segList, D2=stackSize, D3=argptr, D4=argsize) -> D0 */
IPTR kaffeine_dos_RunCommand(struct KaffeineContext *ctx, void *cpu)
{
    ULONG args = THUNK_D(3), size = THUNK_D(4);
    const char *text = NULL;

    /* the seglist of a 68k library from disk is guest memory, which native
       DOS would run as native code */
    if (Kaffeine_IsM68KSegList(ctx, THUNK_D(1)))
    {
        SetIoErr(ERROR_OBJECT_WRONG_TYPE);
        return (IPTR)-1;
    }
    /* AROS injects argsize bytes into Input() and keeps argptr as the
       NUL-terminated pr_Arguments: both views must be guest memory. */
    if (args || size)
    {
        text = guest_string(ctx, args);
        if (!text || !guest_range(ctx, args, size))
            return bad_dos_buffer(-1);
    }
    if (size > KAFFEINE_FILE_BUFFER_LIMIT) return bad_dos_buffer(-1);
    struct KaffeineSegmentLoan loan;
    IPTR result;
    SIPTR ioerr;
    BOOL finished;
    if (!Kaffeine_SegmentLoanBegin(ctx, THUNK_D(1), &loan)) return (IPTR)-1;
    if (!Kaffeine_FileAllPrepare(ctx))
    {
        Kaffeine_SegmentLoanEnd(ctx, &loan);
        return ctx->exit_code == 20 ? 20 : -1;
    }
    result = (IPTR)RunCommand(loan.native, THUNK_D(2), (STRPTR)text, size);
    ioerr = IoErr();
    finished = Kaffeine_FileAllFinish(ctx);
    Kaffeine_SegmentLoanEnd(ctx, &loan);
    if (!finished) return 20;
    SetIoErr(ioerr);
    return result;
}

/* -150: LoadSeg(D1=name) -> D0=segList */
IPTR kaffeine_dos_LoadSeg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG file = THUNK_D(1);
    const char *name = guest_string(ctx, file);
    BPTR seg;
    SIPTR ioerr;
    ULONG token;
    struct KaffeineSegmentLoad load;

    if (!name)
        return bad_dos_buffer(0);
    if (!Kaffeine_SegmentLoadBegin(ctx, &load)) return 0;
    seg = LoadSeg((CONST_STRPTR)name);
    token = Kaffeine_SegmentLoadEnd(ctx, &load, seg);
    ioerr = IoErr();
    /* Record the opaque native BPTR, not BADDR(seg) or any segment bytes. */
    bug("[kaffeine] LoadSeg input=%08lx native=%p d0=%08lx\n",
        (unsigned long)file, (APTR)(IPTR)seg, (unsigned long)token);
    SetIoErr(ioerr);
    return token;
}

/* -768: NewLoadSeg(D1=file, D2=tags) -> D0=segList */
IPTR kaffeine_dos_NewLoadSeg(struct KaffeineContext *ctx, void *cpu)
{
    ULONG file = THUNK_D(1);
    const char *name = guest_string(ctx, file);
    struct TagItem *tags;
    BPTR seg;
    SIPTR ioerr;
    ULONG token;
    struct KaffeineSegmentLoad load;

    if (!name)
        return bad_dos_buffer(0);
    if (!Kaffeine_SegmentLoadBegin(ctx, &load)) return 0;
    /* The tags still go through the generic converter; see INTERNALS.md. */
    tags = m68k_to_native_taglist(ctx, THUNK_D(2));
    seg = NewLoadSeg((CONST_STRPTR)name, tags);
    token = Kaffeine_SegmentLoadEnd(ctx, &load, seg);
    ioerr = IoErr();
    if (tags)
        FreeVec(tags);
    bug("[kaffeine] NewLoadSeg input=%08lx native=%p d0=%08lx\n",
        (unsigned long)file, (APTR)(IPTR)seg, (unsigned long)token);
    SetIoErr(ioerr);
    return token;
}

/* -156: only this context's loaded modules may reach native UnLoadSeg. */
IPTR kaffeine_dos_UnLoadSeg(struct KaffeineContext *ctx, void *cpu)
{
    if (!Kaffeine_IsM68KSegList(ctx, THUNK_D(1)))
        Kaffeine_SegmentRelease(ctx, THUNK_D(1));
    return 0;
}

/* Global segment publication and native-width information outputs need
   separate bridges. Do not expose loader handles through generated thunks. */
IPTR kaffeine_dos_SegmentUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx; (void)cpu;
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return 0;
}

/* -930/-936: CliInitNewcli()/CliInitRun(A0=packet). AROS reads a native
   DosPacket, even for NULL, and replaces the CLI of the native process that
   runs the emulator, so a 68k packet is never passed on. */
IPTR kaffeine_dos_CliInitNewcli(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return 0;
}

IPTR kaffeine_dos_CliInitRun(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return 0;
}

/* -402: NameFromLock(D1=lock, D2=buffer, D3=len) -> D0=success */
IPTR kaffeine_dos_NameFromLock(struct KaffeineContext *ctx, void *cpu)
{
    BPTR lock;
    ULONG buffer = THUNK_D(2);
    LONG len = (LONG)THUNK_D(3);

    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;

    if (len <= 0 || !guest_range(ctx, buffer, (ULONG)len))
        return bad_dos_buffer(0);
    /* A zero lock is legal: native DOS supplies the boot volume name. */
    return (IPTR)NameFromLock(lock, (STRPTR)(ctx->mem + buffer), len);
}

/* -408: NameFromFH(D1=fh, D2=buffer, D3=len) -> D0=success */
IPTR kaffeine_dos_NameFromFH(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh = (BPTR)(IPTR)THUNK_D(1);
    ULONG buffer = THUNK_D(2);
    LONG len = (LONG)THUNK_D(3);

    if (!fh || len <= 0 || !guest_range(ctx, buffer, (ULONG)len))
        return bad_dos_buffer(0);
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    return (IPTR)NameFromFH(fh, (STRPTR)(ctx->mem + buffer), len);
}

/* Both intervals have already passed guest_range(). Avoid native strcpy()
   on overlapping arguments, including a source inside the output buffer. */
static BOOL guest_overlap(ULONG a, ULONG asize, ULONG b, ULONG bsize)
{
    return a <= b ? b - a < asize : a - b < bsize;
}

/* -882: AddPart(D1=dirname, D2=filename, D3=size) -> D0=success */
IPTR kaffeine_dos_AddPart(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dirname = THUNK_D(1), filename = THUNK_D(2), size = THUNK_D(3);
    const char *dir, *file;

    if (!size || !guest_range(ctx, dirname, size))
        return bad_dos_buffer(0);
    dir = guest_string(ctx, dirname);
    file = guest_string(ctx, filename);
    if (!dir || !file || !memchr(dir, 0, size) ||
        guest_overlap(dirname, size, filename, (ULONG)strlen(file) + 1))
        return bad_dos_buffer(0);
    /* A valid buffer that cannot hold the result is left to native DOS:
       AddPart() leaves dirname intact and reports ERROR_LINE_TOO_LONG. */
    return (IPTR)AddPart((STRPTR)dir, file, size);
}

/* -900: SetVar(D1=name, D2=buffer, D3=size, D4=flags) -> D0=success */
IPTR kaffeine_dos_SetVar(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    ULONG buffer = THUNK_D(2);
    LONG size = (LONG)THUNK_D(3), flags = (LONG)THUNK_D(4);
    const char *value;

    if (!name || size < -1)
        return bad_dos_buffer(0);
    if (size == -1)
        value = guest_string(ctx, buffer);
    else
        value = guest_range(ctx, buffer, (ULONG)size) ?
            (const char *)ctx->mem + buffer : NULL;
    if (!value)
        return bad_dos_buffer(0);
    /* Explicit sizes are byte counts, also with BINARY_VAR. A zero-sized
       value is legal and needs no terminator or access to the buffer. */
    return (IPTR)SetVar(name, value, size, flags);
}

/* -906: GetVar(D1=name, D2=buffer, D3=size, D4=flags) -> D0=bytes or -1 */
IPTR kaffeine_dos_GetVar(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name_addr = THUNK_D(1), buffer = THUNK_D(2);
    LONG size = (LONG)THUNK_D(3), flags = (LONG)THUNK_D(4);
    const char *name;

    /* Native GetVar() returns 0 with ERROR_BAD_NUMBER for size 0, before
       inspecting either pointer. Negative sizes can reach CopyMem(). */
    if (!size)
        return bad_dos_buffer(0);
    if (size < 0 || !guest_range(ctx, buffer, (ULONG)size))
        return bad_dos_buffer(-1);
    /* Both text paths in rom/dos/getvar.c test buffer[j] before j < i.
       A full buffer therefore needs one readable lookahead byte. Binary
       mode, with or without DONT_NULL_TERM, stays within size. */
    if (!(flags & GVF_BINARY_VAR) &&
        ctx->mem_size - buffer == (ULONG)size)
        return bad_dos_buffer(-1);
    name = guest_string(ctx, name_addr);
    /* A failed global read can retry ENVARC: using name again. Keep its
       terminator safe from writes to the value buffer on the first try. */
    if (!name || guest_overlap(name_addr, (ULONG)strlen(name) + 1, buffer, (ULONG)size))
        return bad_dos_buffer(-1);
    /* Preserve the native truncation result and IoErr() (the full value's
       length), and pass the type and LOCAL/GLOBAL flags through verbatim. */
    return (IPTR)GetVar(name, (STRPTR)(ctx->mem + buffer), size, flags);
}

/* -912: DeleteVar(D1=name, D2=flags) -> D0=success */
IPTR kaffeine_dos_DeleteVar(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    ULONG flags = THUNK_D(2);

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)DeleteVar(name, flags);
}

/* -468: Fault(D1=code, D2=header, D3=buffer, D4=len) -> D0=native result */
IPTR kaffeine_dos_Fault(struct KaffeineContext *ctx, void *cpu)
{
    LONG code = (LONG)THUNK_D(1), len = (LONG)THUNK_D(4);
    ULONG header_addr = THUNK_D(2), buffer = THUNK_D(3), needed;
    const char *header;

    if (len <= 0 || !guest_range(ctx, buffer, (ULONG)len))
        return bad_dos_buffer(0);
    header = header_addr ? guest_string(ctx, header_addr) : NULL;
    if (header_addr && !header)
        return bad_dos_buffer(0);
    needed = (ULONG)len;
    if (code)
    {
        if (header)
        {
            ULONG copied = (ULONG)strlen(header);
            if (copied >= (ULONG)len)
                copied = (ULONG)len - 1;
            /* Native Fault() appends ': ' without testing len, followed
               by a NUL. Check its actual footprint, not just len. */
            if (needed < copied + 3)
                needed = copied + 3;
        }
        /* Its fallback for unknown negative codes also appends '-' without
           a length check and negates code. Refuse LONG_MIN to avoid UB. */
        if (code == (LONG)(-2147483647L - 1))
            return bad_dos_buffer(0);
        if (code < 0)
            needed++;
    }
    /* An overlapping output can overwrite the header's NUL while it is
       being copied, invalidating the footprint computed from its length. */
    if (!guest_range(ctx, buffer, needed) ||
        (header && guest_overlap(header_addr, (ULONG)strlen(header) + 1, buffer, needed)))
        return bad_dos_buffer(0);
    return (IPTR)Fault(code, header, (STRPTR)(ctx->mem + buffer), len);
}

/* -474: PrintFault(D1=code, D2=header) -> D0=success */
IPTR kaffeine_dos_PrintFault(struct KaffeineContext *ctx, void *cpu)
{
    LONG code = (LONG)THUNK_D(1);
    ULONG header_addr = THUNK_D(2);
    const char *header = header_addr ? guest_string(ctx, header_addr) : NULL;

    if ((header_addr && !header) || code == (LONG)(-2147483647L - 1))
        return bad_dos_buffer(0);
    return (IPTR)PrintFault(code, header);
}

/* -180: SetComment(D1=name, D2=comment) -> D0=success */
IPTR kaffeine_dos_SetComment(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    const char *comment = guest_string(ctx, THUNK_D(2));

    if (!name || !comment)
        return bad_dos_buffer(0);
    return (IPTR)SetComment(name, comment);
}

/* -186: SetProtection(D1=name, D2=protect) -> D0=success */
IPTR kaffeine_dos_SetProtection(struct KaffeineContext *ctx, void *cpu)
{
    const char *name = guest_string(ctx, THUNK_D(1));
    ULONG protect = THUNK_D(2);

    if (!name)
        return bad_dos_buffer(0);
    return (IPTR)SetProtection(name, protect);
}

/* Protect the native parser's unbounded stack scan for an ungrouped '|'
   and its read past a final quoted NUL in a character class. Syntax errors
   retain ERROR_BAD_TEMPLATE, rather than being treated as bad addresses. */
static BOOL safe_pattern_source(const char *source)
{
    const char *p = source;
    ULONG groups = 0;

    while (*p)
    {
        char c = *p++;
        if (c == '[')
        {
            if (*p == '~')
                p++;
            if (!*p)
                return FALSE;
            c = *p++;
            do
            {
                if (c == '\'')
                {
                    if (!*p)
                        return FALSE;
                    p++;
                }
                if (!*p)
                    return FALSE;
                c = *p++;
            } while (c != ']');
        }
        else if (c == '\'')
        {
            if (*p && strchr("#*?()|~[]%'", *p))
                p++;
        }
        else if (c == '(')
            groups++;
        else if (c == ')')
        {
            if (!groups)
                return FALSE;
            groups--;
        }
        else if (c == '|' && !groups)
            return FALSE;
    }
    return groups == 0;
}

static IPTR parse_pattern(struct KaffeineContext *ctx, void *cpu, BOOL nocase)
{
    ULONG source_addr = THUNK_D(1), buffer = THUNK_D(2);
    LONG size = (LONG)THUNK_D(3);
    const char *source;
    ULONG length;

    if (size <= 0 || !guest_range(ctx, buffer, (ULONG)size))
        return bad_dos_buffer(-1);
    source = guest_string(ctx, source_addr);
    if (!source)
        return bad_dos_buffer(-1);
    length = (ULONG)strlen(source);
    if (guest_overlap(source_addr, length + 1, buffer, (ULONG)size))
        return bad_dos_buffer(-1);
    /* patternParse() uses the end of Dest as an unchecked token stack.
       Keep space for both tokens per source byte plus the terminators.
       Division avoids overflowing 2 * length + 2 on an arbitrary arena. */
    if (size < 2 || length > ((ULONG)size - 2) / 2)
    {
        SetIoErr(ERROR_BUFFER_OVERFLOW);
        return (IPTR)-1;
    }
    if (!safe_pattern_source(source))
    {
        SetIoErr(ERROR_BAD_TEMPLATE);
        return (IPTR)-1;
    }
    if (nocase)
        return (IPTR)ParsePatternNoCase(source, (STRPTR)(ctx->mem + buffer), size);
    return (IPTR)ParsePattern(source, (STRPTR)(ctx->mem + buffer), size);
}

/* -840: ParsePattern(D1=pat, D2=buf, D3=buflen) -> D0=1, 0 or -1 */
IPTR kaffeine_dos_ParsePattern(struct KaffeineContext *ctx, void *cpu)
{
    return parse_pattern(ctx, cpu, FALSE);
}

/* -966: ParsePatternNoCase(D1=pat, D2=buf, D3=buflen) -> D0=1, 0 or -1 */
IPTR kaffeine_dos_ParsePatternNoCase(struct KaffeineContext *ctx, void *cpu)
{
    return parse_pattern(ctx, cpu, TRUE);
}

/* A NUL alone cannot bound the native matcher: its token scans run until
   a matching end token, including backwards for repetitions. Check nesting
   and class terminators first. The scratch stack has no guest pointers and
   is freed before DOS is called, so native IoErr() is returned unchanged. */
static BOOL safe_pattern_tokens(const char *pattern)
{
    const UBYTE *p = (const UBYTE *)pattern;
    ULONG length = (ULONG)strlen(pattern), depth = 0;
    UBYTE *stack;
    BOOL valid = TRUE;

    if (!length)
        return TRUE;
    stack = (UBYTE *)AllocVec(length, MEMF_ANY);
    if (!stack)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return FALSE;
    }
    while (*p && valid)
    {
        UBYTE token = *p++;
        switch (token)
        {
        case P_REPBEG: stack[depth++] = P_REPEND; break;
        case P_NOT: stack[depth++] = P_NOTEND; break;
        case P_ORSTART: stack[depth++] = P_OREND; break;
        case P_REPEND:
        case P_NOTEND:
        case P_OREND:
            if (!depth || stack[--depth] != token)
                valid = FALSE;
            break;
        case P_ORNEXT:
            if (!depth || stack[depth - 1] != P_OREND)
                valid = FALSE;
            break;
        case P_CLASS:
        case P_NOTCLASS:
            while (*p && *p != P_CLASS)
            {
                /* Native group/repetition scans do not skip classes. A
                   structural token here can defeat their paired scan even
                   though this class and the outer tokens are balanced. */
                if ((*p >= P_ORSTART && *p <= P_NOTEND) ||
                    *p == P_REPBEG || *p == P_REPEND)
                    valid = FALSE;
                p++;
            }
            if (*p)
                p++;
            else
                valid = FALSE;
            break;
        default:
            break;
        }
    }
    FreeVec(stack);
    if (!valid || depth)
    {
        SetIoErr(ERROR_BAD_TEMPLATE);
        return FALSE;
    }
    return TRUE;
}

static IPTR match_pattern(struct KaffeineContext *ctx, void *cpu, BOOL nocase)
{
    const char *pattern = guest_string(ctx, THUNK_D(1));
    const char *string = guest_string(ctx, THUNK_D(2));

    if (!pattern || !string)
        return bad_dos_buffer(0);
    if (!safe_pattern_tokens(pattern))
        return 0;
    if (nocase)
        return (IPTR)MatchPatternNoCase(pattern, string);
    return (IPTR)MatchPattern(pattern, string);
}

/* -846: MatchPattern(D1=pat, D2=str) -> D0=match */
IPTR kaffeine_dos_MatchPattern(struct KaffeineContext *ctx, void *cpu)
{
    return match_pattern(ctx, cpu, FALSE);
}

/* -972: MatchPatternNoCase(D1=pat, D2=str) -> D0=match */
IPTR kaffeine_dos_MatchPatternNoCase(struct KaffeineContext *ctx, void *cpu)
{
    return match_pattern(ctx, cpu, TRUE);
}

/* -816: StrToLong(D1=string, D2=value) -> D0=characters or -1 */
IPTR kaffeine_dos_StrToLong(struct KaffeineContext *ctx, void *cpu)
{
    const char *string = guest_string(ctx, THUNK_D(1));
    ULONG guest_value = THUNK_D(2);
    LONG value, result;

    if (!string || !guest_range(ctx, guest_value, 4))
        return (IPTR)-1;

    result = StrToLong(string, &value);
    /* No digits is a valid conversion attempt: the native call writes zero
       and returns -1, so its output must be copied on that path as well. */
    m68k_write32(ctx, guest_value, (ULONG)value);
    return (IPTR)result;
}

/* -192: DateStamp(D1=date) -> D0=the same guest date pointer */
IPTR kaffeine_dos_DateStamp(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest_date = THUNK_D(1);
    struct DateStamp date;

    if (!guest_range(ctx, guest_date, 12))
        return 0;
    if (!DateStamp(&date))
        return 0;

    m68k_write32(ctx, guest_date, (ULONG)date.ds_Days);
    m68k_write32(ctx, guest_date + 4, (ULONG)date.ds_Minute);
    m68k_write32(ctx, guest_date + 8, (ULONG)date.ds_Tick);
    return (IPTR)guest_date;
}

/* -870: FilePart(D1=path) -> D0=an interior pointer into that guest string */
IPTR kaffeine_dos_FilePart(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest_path = THUNK_D(1);
    const char *path = guest_string(ctx, guest_path);
    const char *part;

    if (!path)
        return 0;
    part = FilePart(path);
    if (!part)
        return 0;

    /* FilePart returns within the original, validated string, including
       its terminating NUL. Preserve that alias instead of making a shadow. */
    return (IPTR)(guest_path + (ULONG)(part - path));
}

/* -876: PathPart(D1=path) -> D0=an interior pointer into that guest string */
IPTR kaffeine_dos_PathPart(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest_path = THUNK_D(1);
    const char *path = guest_string(ctx, guest_path);
    const char *part;

    if (!path)
        return 0;
    part = PathPart(path);
    /* The result is the last separator, the character after the last colon,
       the start of the string or its terminating NUL. Anything else is not
       an address the guest can be given back. */
    if (!part || part < path || part > path + strlen(path))
        return 0;

    return (IPTR)(guest_path + (ULONG)(part - path));
}

/* Bytes the native FGets() may write for buflen, or 0 when unbounded.
   rom/dos/fgets.c decrements the ULONG buflen before a do-while loop:
   buflen 1 still stores one character and the terminator, and buflen 0
   stops only at a newline or EOF. Values of 2^31 and above do not fit in
   guest memory, so the range check refuses them as well. */
static ULONG fgets_write_size(ULONG buflen)
{
    return buflen == 1 ? 2 : buflen;
}

/* -336: FGets(D1=fh, D2=buf, D3=buflen) -> D0=buf or NULL */
IPTR kaffeine_dos_FGets(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh = (BPTR)(IPTR)THUNK_D(1);
    ULONG guest_buf = THUNK_D(2);
    ULONG buflen = THUNK_D(3);
    ULONG size = fgets_write_size(buflen);
    STRPTR buf, result;

    /* Refuse before reading, so that the stream position is unchanged. */
    if (!size || !guest_range(ctx, guest_buf, size))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }

    buf = (STRPTR)m68k_to_host(ctx, guest_buf);
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    result = FGets(fh, buf, buflen);
    if (!Kaffeine_FileBufferFinish(ctx, THUNK_D(1))) return 20;
    /* NULL means EOF or an error before the first character, with IoErr()
       set by the native call. Otherwise FGets() returns its buffer. */
    return result == buf ? (IPTR)guest_buf : 0;
}

BOOL Kaffeine_FIBToGuest(struct KaffeineContext *ctx, ULONG guest,
                      const struct FileInfoBlock *fib)
{
    if (!fib || !guest_range(ctx, guest, GUEST_FIB_SIZE))
        return FALSE;

    m68k_write32(ctx, guest + M68K_FIB_DISKKEY,     (ULONG)fib->fib_DiskKey);
    m68k_write32(ctx, guest + M68K_FIB_DIRENTRYTYPE, (ULONG)fib->fib_DirEntryType);
    memcpy(ctx->mem + guest + M68K_FIB_FILENAME, fib->fib_FileName, M68K_FIB_FILENAME_SIZE);
    m68k_write32(ctx, guest + M68K_FIB_PROTECTION, (ULONG)fib->fib_Protection);
    m68k_write32(ctx, guest + M68K_FIB_ENTRYTYPE,  (ULONG)fib->fib_EntryType);
    m68k_write32(ctx, guest + M68K_FIB_SIZE,       (ULONG)fib->fib_Size);
    m68k_write32(ctx, guest + M68K_FIB_NUMBLOCKS,  (ULONG)fib->fib_NumBlocks);
    m68k_write32(ctx, guest + M68K_FIB_DATE_DAYS,   (ULONG)fib->fib_Date.ds_Days);
    m68k_write32(ctx, guest + M68K_FIB_DATE_MINUTE, (ULONG)fib->fib_Date.ds_Minute);
    m68k_write32(ctx, guest + M68K_FIB_DATE_TICK,   (ULONG)fib->fib_Date.ds_Tick);
    memcpy(ctx->mem + guest + M68K_FIB_COMMENT, fib->fib_Comment, M68K_FIB_COMMENT_SIZE);
    m68k_write16(ctx, guest + M68K_FIB_OWNERUID, fib->fib_OwnerUID);
    m68k_write16(ctx, guest + M68K_FIB_OWNERGID, fib->fib_OwnerGID);
    return TRUE;
}

/* -390: ExamineFH(D1=fh, D2=fib) -> D0=success */
IPTR kaffeine_dos_ExamineFH(struct KaffeineContext *ctx, void *cpu)
{
    BPTR file = (BPTR)(IPTR)THUNK_D(1);
    ULONG guest_fib = THUNK_D(2);
    struct FileInfoBlock *fib;
    BOOL result;
    SIPTR ioerr;

    /* Invalid guest buffers must never reach the native filesystem handler.
       A null filehandle is unsafe in the native ExamineFH implementation. */
    if (!file || !guest_range(ctx, guest_fib, GUEST_FIB_SIZE))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }

    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &file)) return 20;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }

    result = ExamineFH(file, fib);
    ioerr = IoErr();
    if (result)
        Kaffeine_FIBToGuest(ctx, guest_fib, fib);
    FreeDosObject(DOS_FIB, fib);
    SetIoErr(ioerr);
    return (IPTR)result;
}

/* Examine() and ExNext() through a native temporary FIB. The whole guest
   FIB is checked before the native call: ExNext() moves the directory on,
   so an entry that could not be written back would be lost. */
static IPTR examine_lock(struct KaffeineContext *ctx, void *cpu, BOOL next)
{
    BPTR lock;
    ULONG guest_fib = THUNK_D(2);
    struct FileInfoBlock *fib;
    LONG result;
    SIPTR ioerr;

    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;

    if (!guest_range(ctx, guest_fib, GUEST_FIB_SIZE))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return 0;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }

    if (next)
    {
        /* ExNext() continues from the entry that the guest FIB holds */
        fib->fib_DiskKey = m68k_read32(ctx, guest_fib + M68K_FIB_DISKKEY);
        fib->fib_DirEntryType = (LONG)m68k_read32(ctx, guest_fib + M68K_FIB_DIRENTRYTYPE);
        result = ExNext(lock, fib);
    }
    else
        result = Examine(lock, fib);
    ioerr = IoErr();
    if (result)
        Kaffeine_FIBToGuest(ctx, guest_fib, fib);
    FreeDosObject(DOS_FIB, fib);
    SetIoErr(ioerr);
    return (IPTR)(ULONG)result;
}

/* -102: Examine(D1=lock, D2=fib) -> D0=success */
IPTR kaffeine_dos_Examine(struct KaffeineContext *ctx, void *cpu)
{
    return examine_lock(ctx, cpu, FALSE);
}

/* -108: ExNext(D1=lock, D2=fib) -> D0=success */
IPTR kaffeine_dos_ExNext(struct KaffeineContext *ctx, void *cpu)
{
    return examine_lock(ctx, cpu, TRUE);
}

/* MatchFirst(), MatchNext() and MatchEnd() keep their state in a native
   AnchorPath: the 68k one has 32-bit pointers, a 260-byte FIB at 20 and its
   buffer at 280, and native DOS keeps native AChain pointers in it. The
   bridge copies the fields a program may set to the native AnchorPath
   before each call, and the results back afterwards. ap_Base and ap_Last
   point to guest copies of the first and the current AChain, because
   programs pass ap_Current->an_Lock to CurrentDir() and NameFromLock().
   The copies hold the lock with the opaque BPTR convention of the other
   thunks, the FIB and the flags. an_String is empty, and an_Child and
   an_Parent only link the two copies. */
#define GUEST_ACHAIN_SIZE (M68K_AN_STRING + 1)

static int find_anchor(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_ANCHORS; i++)
        if (ctx->anchor_map[i].native && ctx->anchor_map[i].guest == guest)
            return i;
    return -1;
}

/* MatchEnd() unlocks the native chain, also after a failed match. */
static void release_anchor(struct KaffeineContext *ctx, int slot)
{
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_MATCH, ctx->anchor_map[slot].guest);
    MatchEnd((struct AnchorPath *)ctx->anchor_map[slot].native);
    FreeVec(ctx->anchor_map[slot].native);
    Kaffeine_AChainFree(ctx, ctx->anchor_map[slot].base);
    Kaffeine_AChainFree(ctx, ctx->anchor_map[slot].current);
    memset(&ctx->anchor_map[slot], 0, sizeof(ctx->anchor_map[slot]));
}

/* ap_Strlen of a guest AnchorPath whose header and buffer are guest
   memory, or -1. A negative size would make AddPart() write anywhere. */
static LONG guest_anchor_size(struct KaffeineContext *ctx, ULONG guest)
{
    LONG len;

    if (!guest_range(ctx, guest, M68K_AP_BUF))
        return -1;
    len = (WORD)m68k_read16(ctx, guest + M68K_AP_STRLEN);
    if (len < 0 || !guest_range(ctx, guest, M68K_AP_BUF + (ULONG)len))
        return -1;
    return len;
}

static void anchor_from_guest(struct KaffeineContext *ctx, int slot, LONG len)
{
    struct AnchorPath *ap = (struct AnchorPath *)ctx->anchor_map[slot].native;
    ULONG guest = ctx->anchor_map[slot].guest;

    ap->ap_BreakBits = (LONG)m68k_read32(ctx, guest + M68K_AP_BREAKBITS);
    ap->ap_FoundBreak = (LONG)m68k_read32(ctx, guest + M68K_AP_FOUNDBREAK);
    ap->ap_Flags = (BYTE)ctx->mem[guest + M68K_AP_FLAGS];
    ap->ap_Strlen = (WORD)len;
    memcpy(ap->ap_Buf, ctx->mem + guest + M68K_AP_BUF, (size_t)len);
}

static void achain_to_guest(struct KaffeineContext *ctx, ULONG guest,
                            const struct AChain *ac, ULONG child, ULONG parent, ULONG owner)
{
    m68k_write32(ctx, guest + M68K_AN_CHILD, child);
    m68k_write32(ctx, guest + M68K_AN_PARENT, parent);
    ULONG lock = Kaffeine_LockBorrow(ctx, ac->an_Lock, KAFFEINE_LOCK_MATCH, owner);
    if (ac->an_Lock && !lock) Kaffeine_MessageFailure(ctx, "Match lock publication");
    m68k_write32(ctx, guest + M68K_AN_LOCK, lock);
    Kaffeine_FIBToGuest(ctx, guest + M68K_AN_INFO, &ac->an_Info);
    ctx->mem[guest + M68K_AN_FLAGS] = (UBYTE)ac->an_Flags;
    ctx->mem[guest + M68K_AN_STRING] = 0;
}

/* ap_Info goes back only when native DOS wrote it, as AROS leaves it alone
   on the paths that fail before a result. ap_Buf goes back with the size
   checked before the call. */
static void anchor_to_guest(struct KaffeineContext *ctx, int slot,
                            const struct FileInfoBlock *info, LONG len)
{
    struct AnchorPath *ap = (struct AnchorPath *)ctx->anchor_map[slot].native;
    ULONG guest = ctx->anchor_map[slot].guest;
    ULONG base = 0, current = 0;

    /* The base copy follows the first AChain, which stays the same for the
       whole match; the other copy follows the current one below it. */
    if (ap->ap_Base && ap->ap_Current)
    {
        base = ctx->anchor_map[slot].base;
        if (ap->ap_Base == ap->ap_Current)
        {
            current = base;
            achain_to_guest(ctx, base, ap->ap_Base, 0, 0, guest);
        }
        else
        {
            current = ctx->anchor_map[slot].current;
            achain_to_guest(ctx, base, ap->ap_Base,
                            ap->ap_Base->an_Child == ap->ap_Current ? current : 0, 0, guest);
            achain_to_guest(ctx, current, ap->ap_Current, 0,
                            ap->ap_Current->an_Parent == ap->ap_Base ? base : 0, guest);
        }
    }
    m68k_write32(ctx, guest + M68K_AP_BASE, base);
    m68k_write32(ctx, guest + M68K_AP_LAST, current);
    m68k_write32(ctx, guest + M68K_AP_FOUNDBREAK, (ULONG)ap->ap_FoundBreak);
    ctx->mem[guest + M68K_AP_FLAGS] = (UBYTE)ap->ap_Flags;
    if (memcmp(info, &ap->ap_Info, sizeof(*info)))
        Kaffeine_FIBToGuest(ctx, guest + M68K_AP_INFO, &ap->ap_Info);
    memcpy(ctx->mem + guest + M68K_AP_BUF, ap->ap_Buf, (size_t)len);
}

/* -822: MatchFirst(D1=pattern, D2=anchor) -> D0=error */
IPTR kaffeine_dos_MatchFirst(struct KaffeineContext *ctx, void *cpu)
{
    const char *pattern = guest_string(ctx, THUNK_D(1));
    ULONG guest = THUNK_D(2);
    struct AnchorPath *ap;
    struct FileInfoBlock info;
    LONG len = guest_anchor_size(ctx, guest), error;
    SIPTR ioerr;
    int slot, i;

    if (!pattern || len < 0)
        return bad_dos_buffer(ERROR_BAD_NUMBER);
    slot = find_anchor(ctx, guest);
    for (i = 0; i < KAFFEINE_MAX_ANCHORS; i++)
        if (i != slot && ctx->anchor_map[i].native &&
            guest_overlap(guest, M68K_AP_BUF + (ULONG)len, ctx->anchor_map[i].guest,
                          M68K_AP_BUF + ctx->anchor_map[i].capacity))
            return bad_dos_buffer(ERROR_BAD_NUMBER);
    /* A new match on a busy AnchorPath: AROS would lose the earlier chain. */
    if (slot >= 0)
        release_anchor(ctx, slot);
    /* As AROS, before anything can fail. */
    m68k_write32(ctx, guest + M68K_AP_BASE, 0);
    m68k_write32(ctx, guest + M68K_AP_LAST, 0);
    ctx->mem[guest + M68K_AP_FLAGS] = 0;
    for (slot = 0; slot < KAFFEINE_MAX_ANCHORS && ctx->anchor_map[slot].native; slot++)
        ;
    if (slot == KAFFEINE_MAX_ANCHORS)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return ERROR_NO_FREE_STORE;
    }
    ap = AllocVec(sizeof(struct AnchorPath) + (ULONG)len, MEMF_ANY | MEMF_CLEAR);
    ctx->anchor_map[slot].base = Kaffeine_AChainAlloc(ctx, GUEST_ACHAIN_SIZE);
    ctx->anchor_map[slot].current = Kaffeine_AChainAlloc(ctx, GUEST_ACHAIN_SIZE);
    if (!ap || !ctx->anchor_map[slot].base || !ctx->anchor_map[slot].current)
    {
        if (ap)
            FreeVec(ap);
        Kaffeine_AChainFree(ctx, ctx->anchor_map[slot].base);
        Kaffeine_AChainFree(ctx, ctx->anchor_map[slot].current);
        memset(&ctx->anchor_map[slot], 0, sizeof(ctx->anchor_map[slot]));
        SetIoErr(ERROR_NO_FREE_STORE);
        return ERROR_NO_FREE_STORE;
    }
    ctx->anchor_map[slot].guest = guest;
    ctx->anchor_map[slot].native = ap;
    ctx->anchor_map[slot].capacity = (UWORD)len;
    anchor_from_guest(ctx, slot, len);
    info = ap->ap_Info;
    error = MatchFirst((CONST_STRPTR)pattern, ap);
    anchor_to_guest(ctx, slot, &info, len);
    /* Without a chain MatchEnd() has nothing to release later. */
    if (!ap->ap_Base)
    {
        ioerr = IoErr();
        release_anchor(ctx, slot);
        SetIoErr(ioerr);
    }
    return (IPTR)error;
}

/* -828: MatchNext(D1=anchor) -> D0=error */
IPTR kaffeine_dos_MatchNext(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_D(1);
    int slot = find_anchor(ctx, guest);
    struct AnchorPath *ap;
    struct FileInfoBlock info;
    LONG len, error;

    /* AROS would follow whatever ap_Current holds. */
    if (slot < 0)
        return bad_dos_buffer(ERROR_BAD_NUMBER);
    len = guest_anchor_size(ctx, guest);
    if (len < 0 || len > ctx->anchor_map[slot].capacity)
        return bad_dos_buffer(ERROR_BAD_NUMBER);
    ap = (struct AnchorPath *)ctx->anchor_map[slot].native;
    anchor_from_guest(ctx, slot, len);
    info = ap->ap_Info;
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_MATCH, guest);
    error = MatchNext(ap);
    anchor_to_guest(ctx, slot, &info, len);
    return (IPTR)error;
}

/* -834: MatchEnd(D1=anchor) */
IPTR kaffeine_dos_MatchEnd(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_D(1);
    int slot = find_anchor(ctx, guest);

    if (slot >= 0)
        release_anchor(ctx, slot);
    /* AROS clears both pointers even when there was no match. */
    if (guest_range(ctx, guest, M68K_AP_LAST + 4))
    {
        m68k_write32(ctx, guest + M68K_AP_BASE, 0);
        m68k_write32(ctx, guest + M68K_AP_LAST, 0);
    }
    return 0;
}

void Kaffeine_MatchCleanup(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_ANCHORS; i++)
        if (ctx->anchor_map[i].native)
            release_anchor(ctx, i);
}

/* Raw guest code and generic tag conversion cannot start a native process.
   Registered independent modules use the typed process adapter. CreateProc
   makes a guest Process on the guest task scheduler, which
   has no DOS context (kaffeine_sched.c); without a scheduler and a backend
   CPU factory it fails like CreateNewProc(). */

/* -498: CreateNewProc(D1=tags) -> D0=process */
IPTR kaffeine_dos_CreateNewProc(struct KaffeineContext *ctx, void *cpu)
{
    enum { TRACE_TAGS = 16, TRACE_STEPS = 64 };
    struct KaffeineTagWalk walk;
    ULONG list = THUNK_D(1), item = 0, tags = 0, logged = 0;
    int result;

    /* Numeric diagnosis precedes typed validation; never follow arbitrary
       tag payloads here. The typed adapter owns startup and rollback. */
    bug("[kaffeine] CreateNewProc tags=%08lx\n", (unsigned long)list);
    Kaffeine_TagWalkInit(&walk, list);
    /* A preset limit bounds control tags too and disables the walker's
       extra cycle-length scan. Cycles and long lists stop at this budget. */
    walk.limit = TRACE_STEPS;
    while ((result = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        tags++;
        if (logged < TRACE_TAGS)
        {
            bug("[kaffeine] CreateNewProc tag[%lu] at=%08lx id=%08lx data=%08lx\n",
                (unsigned long)logged, (unsigned long)item,
                (unsigned long)m68k_read32(ctx, item),
                (unsigned long)m68k_read32(ctx, item + 4));
            logged++;
        }
    }
    bug("[kaffeine] CreateNewProc trace stop=%s tags=%lu logged=%lu steps=%lu\n",
        result == KAFFEINE_TAG_END ? "end" :
            (walk.steps >= TRACE_STEPS ? "limit" : "invalid"),
        (unsigned long)tags, (unsigned long)logged, (unsigned long)walk.steps);
    {
        ULONG process = Kaffeine_ProcessFromTags(ctx, list);
        SIPTR error = IoErr();
        bug("[kaffeine] process startup result=%08lx error=%ld\n",
            (unsigned long)process, (long)error);
        SetIoErr(error);
        return process;
    }
}

/* -138: CreateProc(D1=name, D2=pri, D3=seglist, D4=stack) -> D0=port */
IPTR kaffeine_dos_CreateProc(struct KaffeineContext *ctx, void *cpu)
{
    LONG ioerr = ERROR_NOT_IMPLEMENTED;
    IPTR port = 0;

    if (Kaffeine_SegmentToken(THUNK_D(3)))
    {
        SetIoErr(ERROR_NOT_IMPLEMENTED);
        return 0;
    }
    /* A refusal stops the program and returns its 20, without IoErr(). */
    if (ctx->task_ops && ctx->task_ops->create_proc && ctx->cpu_factory)
        port = ctx->task_ops->create_proc(ctx, cpu, &ioerr);
    if (!port)
        SetIoErr(ioerr);
    return port;
}

/* ExAll() and ExAllEnd() are emulated on the bridge with native Examine()
   and ExNext(), as AmigaOS dos.library and AROS emulate them for handlers
   without ACTION_EXAMINE_ALL: native DOS would write ExAllData with 64-bit
   pointers and keep native state in the private part of its ExAllControl.
   Each guest ExAllControl has a native FileInfoBlock that holds the scan
   position. The entries are written in the NDK 3.2 layout straight into the
   guest buffer: the fixed part that the type selects, then the name and,
   from ED_COMMENT on, the comment, or NULL without one, as the autodoc
   says; the next entry starts on a longword. An entry that does not fit
   stays in the native FileInfoBlock and comes first in the next call.

   Measured on AmigaOS 3.2.3, ram-handler and FFS agree that the scan ends
   with FALSE and ERROR_NO_MORE_ENTRIES also when that call delivered
   entries, that eac_LastKey 0 starts again, that eac_MatchString matches
   without case, and that eac_MatchFunc gets the hook in A0, the filled
   entry in A1 and a pointer to the type longword in A2. Where they differ,
   the bridge follows the autodoc and AROS: a buffer too small for one
   entry gives FALSE and ERROR_BUFFER_OVERFLOW, never TRUE with no entries
   (FFS repeats that forever), a type outside ED_NAME..ED_OWNER gives
   ERROR_BAD_NUMBER, and a lock on a file ERROR_OBJECT_WRONG_TYPE. */
#define GUEST_EXALLCONTROL_SIZE 16
#define GUEST_EAC_ENTRIES 0
#define GUEST_EAC_LASTKEY 4
#define GUEST_EAC_MATCHSTRING 8
#define GUEST_EAC_MATCHFUNC 12
#define GUEST_ED_NEXT 0
#define GUEST_ED_NAME 4
#define GUEST_ED_TYPE 8
#define GUEST_ED_SIZE 12
#define GUEST_ED_PROT 16
#define GUEST_ED_DATE 20
#define GUEST_ED_COMMENT 32
#define GUEST_ED_OWNERUID 36
#define GUEST_ED_OWNERGID 38
/* Any non-zero eac_LastKey: the guest only tests it against zero */
#define GUEST_EXALL_SCANNING 1

/* Size of the fixed part of an entry for ED_NAME..ED_OWNER */
static const UBYTE exall_fixed[ED_OWNER + 1] = { 0, 8, 12, 16, 20, 32, 36, 40 };

static int find_exall(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_EXALL; i++)
        if (ctx->exall_map[i].guest == guest)
            return i;
    return -1;
}

static int new_exall(struct KaffeineContext *ctx, ULONG guest, BOOL owned)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_EXALL; i++)
        if (!ctx->exall_map[i].guest)
        {
            memset(&ctx->exall_map[i], 0, sizeof(ctx->exall_map[i]));
            ctx->exall_map[i].guest = guest;
            ctx->exall_map[i].owned = owned;
            return i;
        }
    return -1;
}

static void release_exall(struct KaffeineContext *ctx, int slot)
{
    if (ctx->exall_map[slot].fib)
        FreeDosObject(DOS_FIB, ctx->exall_map[slot].fib);
    memset(&ctx->exall_map[slot], 0, sizeof(ctx->exall_map[slot]));
}

/* AllocDosObject(DOS_EXALLCONTROL): a cleared 68k ExAllControl */
ULONG Kaffeine_ExAllAllocControl(struct KaffeineContext *ctx)
{
    ULONG control = Kaffeine_HeapAlloc(ctx, GUEST_EXALLCONTROL_SIZE, 0);

    if (control && new_exall(ctx, control, TRUE) < 0)
    {
        Kaffeine_HeapFree(ctx, control, GUEST_EXALLCONTROL_SIZE);
        control = 0;
    }
    if (!control)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(ctx->mem + control, 0, GUEST_EXALLCONTROL_SIZE);
    return control;
}

/* FreeDosObject(DOS_EXALLCONTROL): the guest memory only if it came from
   Kaffeine_ExAllAllocControl(), and never while its MatchFunc runs */
BOOL Kaffeine_ExAllFreeControl(struct KaffeineContext *ctx, ULONG control)
{
    int slot = control ? find_exall(ctx, control) : -1;
    BOOL owned;

    if (slot < 0)
        return TRUE;
    if (ctx->exall_map[slot].active)
        return FALSE;
    owned = ctx->exall_map[slot].owned;
    release_exall(ctx, slot);
    if (owned)
        Kaffeine_HeapFree(ctx, control, GUEST_EXALLCONTROL_SIZE);
    return TRUE;
}

void Kaffeine_ExAllCleanup(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_EXALL; i++)
        if (ctx->exall_map[i].guest)
            release_exall(ctx, i);
}

/* Write the entry of the native FIB at guest address entry. need is the
   whole size of the entry without the alignment after it. */
static ULONG exall_entry_size(const struct FileInfoBlock *fib, LONG data)
{
    ULONG need = exall_fixed[data] + (ULONG)strlen((const char *)fib->fib_FileName) + 1;

    if (data >= ED_COMMENT && fib->fib_Comment[0])
        need += (ULONG)strlen((const char *)fib->fib_Comment) + 1;
    return need;
}

static void exall_write_entry(struct KaffeineContext *ctx, ULONG entry,
                              const struct FileInfoBlock *fib, LONG data)
{
    ULONG next = entry + exall_fixed[data];
    ULONG length;

    m68k_write32(ctx, entry + GUEST_ED_NEXT, 0);
    switch (data)
    {
        case ED_OWNER:
            m68k_write16(ctx, entry + GUEST_ED_OWNERUID, fib->fib_OwnerUID);
            m68k_write16(ctx, entry + GUEST_ED_OWNERGID, fib->fib_OwnerGID);
            /* Fall through */
        case ED_COMMENT:
            m68k_write32(ctx, entry + GUEST_ED_COMMENT, 0);
            /* Fall through */
        case ED_DATE:
            write_date(ctx, entry + GUEST_ED_DATE, &fib->fib_Date);
            /* Fall through */
        case ED_PROTECTION:
            m68k_write32(ctx, entry + GUEST_ED_PROT, (ULONG)fib->fib_Protection);
            /* Fall through */
        case ED_SIZE:
            m68k_write32(ctx, entry + GUEST_ED_SIZE, (ULONG)fib->fib_Size);
            /* Fall through */
        case ED_TYPE:
            m68k_write32(ctx, entry + GUEST_ED_TYPE, (ULONG)fib->fib_DirEntryType);
            /* Fall through */
        default:
            length = (ULONG)strlen((const char *)fib->fib_FileName) + 1;
            memcpy(ctx->mem + next, fib->fib_FileName, length);
            m68k_write32(ctx, entry + GUEST_ED_NAME, next);
            next += length;
    }
    if (data >= ED_COMMENT && fib->fib_Comment[0])
    {
        length = (ULONG)strlen((const char *)fib->fib_Comment) + 1;
        memcpy(ctx->mem + next, fib->fib_Comment, length);
        m68k_write32(ctx, entry + GUEST_ED_COMMENT, next);
    }
}

/* Call the guest MatchFunc for the filled entry: A0 the hook, A1 the entry,
   A2 the type longword. code is h_Entry, taken once before the scan. TRUE
   in *stopped if the guest stopped. */
static BOOL exall_match_func(struct KaffeineContext *ctx, void *cpu, ULONG hook,
                             ULONG code, ULONG entry, ULONG typeptr, BOOL *stopped)
{
    struct KaffeineGuestCall call;

    memset(&call, 0, sizeof(call));
    call.a[0] = hook;
    call.a[1] = entry;
    call.a[2] = typeptr;
    call.a_mask = (1u << 0) | (1u << 1) | (1u << 2);
    if (!Kaffeine_CallGuest(ctx, cpu, code, &call))
    {
        *stopped = TRUE;
        return FALSE;
    }
    return call.d[0] != 0;
}

static BOOL valid_guest_hook(struct KaffeineContext *ctx, ULONG hook)
{
    ULONG entry;

    if ((hook & 1) || !guest_range(ctx, hook, 20))
        return FALSE;
    entry = m68k_read32(ctx, hook + 8);
    return entry && !(entry & 1) && guest_range(ctx, entry, 2) &&
           !Kaffeine_IsCallbackReturn(entry);
}

/* -432: ExAll(D1=lock, D2=buffer, D3=size, D4=data, D5=control) -> D0=continue */
IPTR kaffeine_dos_ExAll(struct KaffeineContext *ctx, void *cpu)
{
    BPTR lock;
    ULONG buffer = THUNK_D(2);
    LONG size = (LONG)THUNK_D(3);
    LONG data = (LONG)THUNK_D(4);
    ULONG control = THUNK_D(5);
    ULONG pattern_addr, hook, code = 0, typeptr = 0, entry, end, last = 0, entries = 0;
    const char *guest_pattern;
    char *pattern = NULL;
    struct FileInfoBlock *fib;
    BOOL stopped = FALSE;
    SIPTR err = 0;
    int slot;

    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;

    if (!guest_range(ctx, control, GUEST_EXALLCONTROL_SIZE) ||
        size < 0 || (size > 0 && !guest_range(ctx, buffer, (ULONG)size)))
        return bad_dos_buffer(0);
    m68k_write32(ctx, control + GUEST_EAC_ENTRIES, 0);
    if (data < ED_NAME || data > ED_OWNER)
        return bad_dos_buffer(0);
    hook = m68k_read32(ctx, control + GUEST_EAC_MATCHFUNC);
    if (hook && !valid_guest_hook(ctx, hook))
        return bad_dos_buffer(0);
    if (hook)
        code = m68k_read32(ctx, hook + 8);
    /* The MatchFunc may change guest memory: match against a native copy
       of the pattern, checked once */
    pattern_addr = m68k_read32(ctx, control + GUEST_EAC_MATCHSTRING);
    if (pattern_addr)
    {
        guest_pattern = guest_string(ctx, pattern_addr);
        if (!guest_pattern)
            return bad_dos_buffer(0);
        pattern = (char *)AllocVec(strlen(guest_pattern) + 1, MEMF_ANY);
        if (!pattern)
        {
            SetIoErr(ERROR_NO_FREE_STORE);
            return 0;
        }
        strcpy(pattern, guest_pattern);
        if (!safe_pattern_tokens(pattern))
        {
            FreeVec(pattern);
            return 0;
        }
    }

    slot = find_exall(ctx, control);
    if (slot < 0)
        slot = new_exall(ctx, control, FALSE);
    if (slot >= 0 && ctx->exall_map[slot].active)
    {
        bug("[kaffeine] ExAll: control in use by its own MatchFunc, stopping (20)\n");
        FreeVec(pattern);
        Kaffeine_StopGuest(ctx, cpu, 20);
        return 20;
    }
    if (slot >= 0 && !ctx->exall_map[slot].fib)
        ctx->exall_map[slot].fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (slot < 0 || !ctx->exall_map[slot].fib)
    {
        FreeVec(pattern);
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    fib = ctx->exall_map[slot].fib;

    /* eac_LastKey 0 starts a scan, as does a control the bridge has not
       started; after the end the scan stays ended until then */
    if (!m68k_read32(ctx, control + GUEST_EAC_LASTKEY) || !ctx->exall_map[slot].started)
    {
        ctx->exall_map[slot].started = FALSE;
        ctx->exall_map[slot].ended = FALSE;
        ctx->exall_map[slot].pending = FALSE;
        if (!Examine(lock, fib))
        {
            FreeVec(pattern);
            return 0;
        }
        if (fib->fib_DirEntryType <= 0)
        {
            FreeVec(pattern);
            SetIoErr(ERROR_OBJECT_WRONG_TYPE);
            return 0;
        }
        ctx->exall_map[slot].started = TRUE;
        m68k_write32(ctx, control + GUEST_EAC_LASTKEY, GUEST_EXALL_SCANNING);
    }
    else if (ctx->exall_map[slot].ended)
    {
        FreeVec(pattern);
        SetIoErr(ERROR_NO_MORE_ENTRIES);
        return 0;
    }

    if (hook)
    {
        typeptr = Kaffeine_HeapAlloc(ctx, 4, 0);
        if (!typeptr)
        {
            FreeVec(pattern);
            SetIoErr(ERROR_NO_FREE_STORE);
            return 0;
        }
    }
    entry = buffer;
    end = buffer + (ULONG)size;
    for (;;)
    {
        ULONG need;

        if (!ctx->exall_map[slot].pending)
        {
            if (!ExNext(lock, fib))
            {
                err = IoErr();
                if (err == ERROR_NO_MORE_ENTRIES)
                    ctx->exall_map[slot].ended = TRUE;
                break;
            }
            ctx->exall_map[slot].pending = TRUE;
        }
        if (pattern && !MatchPatternNoCase(pattern, (CONST_STRPTR)fib->fib_FileName))
        {
            ctx->exall_map[slot].pending = FALSE;
            continue;
        }
        need = exall_entry_size(fib, data);
        if (entry >= end || end - entry < need)
        {
            if (!entries)
                err = ERROR_BUFFER_OVERFLOW;
            break;
        }
        exall_write_entry(ctx, entry, fib, data);
        if (hook)
        {
            BOOL keep;

            m68k_write32(ctx, typeptr, (ULONG)data);
            ctx->exall_map[slot].active = TRUE;
            keep = exall_match_func(ctx, cpu, hook, code, entry, typeptr, &stopped);
            ctx->exall_map[slot].active = FALSE;
            if (stopped)
                break;
            if (!keep)
            {
                ctx->exall_map[slot].pending = FALSE;
                continue;
            }
        }
        if (last)
            m68k_write32(ctx, last + GUEST_ED_NEXT, entry);
        last = entry;
        entries++;
        ctx->exall_map[slot].pending = FALSE;
        entry = (entry + need + 3) & ~3u;
    }
    if (typeptr)
        Kaffeine_HeapFree(ctx, typeptr, 4);
    FreeVec(pattern);
    if (stopped)
        return (IPTR)ctx->exit_code;
    m68k_write32(ctx, control + GUEST_EAC_ENTRIES, entries);
    SetIoErr(err);
    return err ? 0 : (IPTR)DOSTRUE;
}

/* -990: ExAllEnd(D1=lock, D2=buffer, D3=size, D4=data, D5=control). The
   next ExAll() on the control starts a new scan. */
IPTR kaffeine_dos_ExAllEnd(struct KaffeineContext *ctx, void *cpu)
{
    int slot = find_exall(ctx, THUNK_D(5));
    BPTR lock;
    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;

    if (slot >= 0 && !ctx->exall_map[slot].active)
    {
        ctx->exall_map[slot].started = FALSE;
        ctx->exall_map[slot].ended = FALSE;
        ctx->exall_map[slot].pending = FALSE;
    }
    return 0;
}

/* DOS packets follow AmigaOS dos.library on the guest structures: SendPkt()
   sets dp_Port and mn_ReplyPort to the reply port and puts the message,
   WaitPkt() takes the next message from pr_MsgPort and returns its ln_Name,
   ReplyPkt() writes the results, puts pr_MsgPort in dp_Port and puts the
   message back, and AbortPkt() does nothing, as in V37 to V47. Messages go
   through the same routing as PutMsg(): guest ports, and the handlers of
   the program's FileHandles (kaffeine_filehandle.c). DoPkt() sends a packet
   of its own with a private PA_IGNORE reply port, so it never takes a reply
   that is waiting on pr_MsgPort; AmigaOS 3.2.3 does take it and loses it.
   It returns dp_Res1 and, as the autodoc says, dp_Res2 in D1 and IoErr();
   AmigaOS 3.2.3 leaves D1 alone. A packet whose reply comes later from
   another guest task, and a WaitPkt() with nothing on pr_MsgPort, stop the
   program: only replies that the handlers give at once are supported.

   A NULL port is NIL:, as in AROS: it writes everything and reads nothing,
   and answers every other action with DOSTRUE. AmigaOS 3.2.3 hangs. */
#define GUEST_MSG_SIZE 20
#define GUEST_DP_SIZE 48
#define GUEST_STDPKT_SIZE (GUEST_MSG_SIZE + GUEST_DP_SIZE)
#define GUEST_MN_NAME 10
#define GUEST_MN_REPLYPORT 14
#define GUEST_MN_LENGTH 18
#define GUEST_DP_LINK 0
#define GUEST_DP_PORT 4
#define GUEST_DP_TYPE 8
#define GUEST_DP_RES1 12
#define GUEST_DP_RES2 16
#define GUEST_DP_ARG1 20
#define GUEST_PORT_SIZE 34
/* DoPkt(): a StandardPacket and its reply port in one guest block */
#define GUEST_DOPKT_BLOCK (GUEST_STDPKT_SIZE + GUEST_PORT_SIZE + 2)

/* pr_MsgPort of the program, if the program's process is running */
static ULONG process_port(struct KaffeineContext *ctx)
{
    ULONG task;

    if (!ctx->guest_exec_task || !guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        return 0;
    task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    return task == ctx->guest_exec_task ? task + M68K_PR_MSGPORT : 0;
}

/* The packet and its message. The guest queues size a message by its
   mn_Length, which AmigaOS AllocDosObject() leaves 0: 0 becomes the size of
   the StandardPacket, or of a bare Message when the packet is elsewhere. */
static BOOL guest_packet(struct KaffeineContext *ctx, ULONG dp, ULONG *msg)
{
    if ((dp & 1) || !guest_range(ctx, dp, GUEST_DP_SIZE))
        return FALSE;
    *msg = m68k_read32(ctx, dp + GUEST_DP_LINK);
    if ((*msg & 1) || !guest_range(ctx, *msg, GUEST_MSG_SIZE))
        return FALSE;
    if (m68k_read16(ctx, *msg + GUEST_MN_LENGTH) < GUEST_MSG_SIZE)
        m68k_write16(ctx, *msg + GUEST_MN_LENGTH,
                     dp == *msg + GUEST_MSG_SIZE ? GUEST_STDPKT_SIZE : GUEST_MSG_SIZE);
    return TRUE;
}

static LONG nil_result(LONG action, LONG arg3)
{
    if (action == ACTION_WRITE)
        return arg3;
    return action == ACTION_READ ? 0 : DOSTRUE;
}

/* AllocDosObject(DOS_STDPKT): a cleared StandardPacket in guest memory, with
   ln_Name and dp_Link set; the result is its DosPacket, sp_Pkt */
ULONG Kaffeine_AllocStdPacket(struct KaffeineContext *ctx)
{
    ULONG block;
    int i;

    for (i = 0; i < KAFFEINE_MAX_STDPKTS; i++)
        if (!ctx->stdpkt_map[i])
            break;
    block = i < KAFFEINE_MAX_STDPKTS ? Kaffeine_HeapAlloc(ctx, GUEST_STDPKT_SIZE, 0) : 0;
    if (!block)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(ctx->mem + block, 0, GUEST_STDPKT_SIZE);
    m68k_write32(ctx, block + GUEST_MN_NAME, block + GUEST_MSG_SIZE);
    m68k_write32(ctx, block + GUEST_MSG_SIZE + GUEST_DP_LINK, block);
    ctx->stdpkt_map[i] = block;
    return block + GUEST_MSG_SIZE;
}

/* FreeDosObject(DOS_STDPKT): only packets that came from it */
void Kaffeine_FreeStdPacket(struct KaffeineContext *ctx, ULONG dp)
{
    int i;

    for (i = 0; dp > GUEST_MSG_SIZE && i < KAFFEINE_MAX_STDPKTS; i++)
        if (ctx->stdpkt_map[i] == dp - GUEST_MSG_SIZE)
        {
            Kaffeine_HeapFree(ctx, ctx->stdpkt_map[i], GUEST_STDPKT_SIZE);
            ctx->stdpkt_map[i] = 0;
            return;
        }
}

/* -240: DoPkt(D1=port, D2=action, D3=arg1, D4=arg2, D5=arg3, D6=arg4, D7=arg5) -> D0=res1, D1=res2 */
IPTR kaffeine_dos_DoPkt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = THUNK_D(1), block, msg, dp, reply, task;
    LONG action = (LONG)THUNK_D(2), res1, res2;

    if (!port)
    {
        res1 = nil_result(action, (LONG)THUNK_D(5));
        SetIoErr(0);
        Kaffeine_SetD(cpu, 1, 0);
        return (IPTR)(ULONG)res1;
    }
    if (!guest_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
        return Kaffeine_MessageFailure(ctx, "DoPkt task");
    task = m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    block = Kaffeine_HeapAlloc(ctx, GUEST_DOPKT_BLOCK, 0);
    if (!block)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        Kaffeine_SetD(cpu, 1, ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(ctx->mem + block, 0, GUEST_DOPKT_BLOCK);
    msg = block;
    dp = block + GUEST_MSG_SIZE;
    reply = block + GUEST_STDPKT_SIZE;
    m68k_write32(ctx, msg + GUEST_MN_NAME, dp);
    m68k_write32(ctx, msg + GUEST_MN_REPLYPORT, reply);
    m68k_write16(ctx, msg + GUEST_MN_LENGTH, GUEST_STDPKT_SIZE);
    m68k_write32(ctx, dp + GUEST_DP_LINK, msg);
    m68k_write32(ctx, dp + GUEST_DP_PORT, reply);
    m68k_write32(ctx, dp + GUEST_DP_TYPE, (ULONG)action);
    m68k_write32(ctx, dp + GUEST_DP_ARG1, THUNK_D(3));
    m68k_write32(ctx, dp + GUEST_DP_ARG1 + 4, THUNK_D(4));
    m68k_write32(ctx, dp + GUEST_DP_ARG1 + 8, THUNK_D(5));
    m68k_write32(ctx, dp + GUEST_DP_ARG1 + 12, THUNK_D(6));
    m68k_write32(ctx, dp + GUEST_DP_ARG1 + 16, THUNK_D(7));
    ctx->mem[reply + 8] = NT_MSGPORT;
    ctx->mem[reply + 14] = PA_IGNORE;
    m68k_write32(ctx, reply + 16, task);
    m68k_write32(ctx, reply + 20, reply + 24);
    m68k_write32(ctx, reply + 28, reply + 20);

    Kaffeine_RoutePutMsg(ctx, port, msg, 0);
    if (!ctx->running)
        return (IPTR)ctx->exit_code;
    if ((ULONG)Kaffeine_GuestGetMsg(ctx, reply, FALSE) != msg)
        return ctx->running ? Kaffeine_MessageFailure(ctx, "DoPkt without an immediate reply")
                            : (IPTR)ctx->exit_code;
    res1 = (LONG)m68k_read32(ctx, dp + GUEST_DP_RES1);
    res2 = (LONG)m68k_read32(ctx, dp + GUEST_DP_RES2);
    Kaffeine_HeapFree(ctx, block, GUEST_DOPKT_BLOCK);
    SetIoErr(res2);
    Kaffeine_SetD(cpu, 1, (ULONG)res2);
    return (IPTR)(ULONG)res1;
}

/* -246: SendPkt(D1=dp, D2=port, D3=replyport) */
IPTR kaffeine_dos_SendPkt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dp = THUNK_D(1), port = THUNK_D(2), reply = THUNK_D(3), msg;

    if (!guest_packet(ctx, dp, &msg))
        return Kaffeine_MessageFailure(ctx, "SendPkt packet");
    m68k_write32(ctx, dp + GUEST_DP_PORT, reply);
    m68k_write32(ctx, msg + GUEST_MN_REPLYPORT, reply);
    if (port)
        return Kaffeine_RoutePutMsg(ctx, port, msg, 0);
    /* NIL: replies at once, as a handler would */
    m68k_write32(ctx, dp + GUEST_DP_PORT, 0);
    m68k_write32(ctx, dp + GUEST_DP_RES1, (ULONG)nil_result((LONG)m68k_read32(ctx, dp + GUEST_DP_TYPE),
                                                            (LONG)m68k_read32(ctx, dp + GUEST_DP_ARG1 + 8)));
    m68k_write32(ctx, dp + GUEST_DP_RES2, 0);
    return Kaffeine_GuestPutMsg(ctx, reply, msg, 0);
}

/* -252: WaitPkt() -> D0=packet. It leaves IoErr() alone, as AmigaOS 3.2.3. */
IPTR kaffeine_dos_WaitPkt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG port = process_port(ctx), msg;

    (void)cpu;
    if (!port)
        return Kaffeine_MessageFailure(ctx, "WaitPkt outside the program's process");
    if (m68k_read32(ctx, ctx->guest_exec_task + M68K_PR_PKTWAIT))
        return Kaffeine_MessageFailure(ctx, "WaitPkt with pr_PktWait");
    msg = (ULONG)Kaffeine_GuestGetMsg(ctx, port, FALSE);
    if (!ctx->running)
        return (IPTR)ctx->exit_code;
    if (!msg)
        return Kaffeine_MessageFailure(ctx, "WaitPkt with no packet on pr_MsgPort");
    return m68k_read32(ctx, msg + GUEST_MN_NAME);
}

/* -258: ReplyPkt(D1=dp, D2=res1, D3=res2) */
IPTR kaffeine_dos_ReplyPkt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dp = THUNK_D(1), mine = process_port(ctx), msg, port;

    if (!mine)
        return Kaffeine_MessageFailure(ctx, "ReplyPkt outside the program's process");
    if (!guest_packet(ctx, dp, &msg))
        return Kaffeine_MessageFailure(ctx, "ReplyPkt packet");
    port = m68k_read32(ctx, dp + GUEST_DP_PORT);
    if (!port)
        return Kaffeine_MessageFailure(ctx, "ReplyPkt to no port");
    m68k_write32(ctx, msg + GUEST_MN_NAME, dp);
    m68k_write32(ctx, dp + GUEST_DP_PORT, mine);
    m68k_write32(ctx, dp + GUEST_DP_RES1, THUNK_D(2));
    m68k_write32(ctx, dp + GUEST_DP_RES2, THUNK_D(3));
    return Kaffeine_RoutePutMsg(ctx, port, msg, 0);
}

/* -264: AbortPkt(D1=port, D2=pkt): nothing, as in AmigaOS V37 to V47 */
IPTR kaffeine_dos_AbortPkt(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0;
}

/* Walk a guest TagItem chain (with TAG_MORE, TAG_SKIP and TAG_IGNORE) and
   convert only known SystemTagList tags so scalar values such as
   NP_StackSize are not turned into host pointers. */
static BOOL walk_system_tags(struct KaffeineContext *ctx, ULONG addr,
                             struct TagItem *out, ULONG *out_count, SIPTR *err)
{
    ULONG p = addr;
    ULONG count = 0;

    for (ULONG steps = 0; steps < MAX_SYSTEM_TAG_STEPS; steps++)
    {
        ULONG tag, data;
        IPTR native_data;

        if (!guest_range(ctx, p, 8))
        {
            *err = ERROR_BAD_NUMBER;
            return FALSE;
        }
        tag = m68k_read32(ctx, p);
        data = m68k_read32(ctx, p + 4);
        if (tag == TAG_DONE)
        {
            *out_count = count;
            return TRUE;
        }
        if (tag == TAG_IGNORE)
        {
            p += 8;
            continue;
        }
        if (tag == TAG_MORE)
        {
            if (!data)
            {
                *out_count = count;
                return TRUE;
            }
            p = data;
            continue;
        }
        if (tag == TAG_SKIP)
        {
            if (data > (ctx->mem_size - p) / 8 - 1)
            {
                *err = ERROR_BAD_NUMBER;
                return FALSE;
            }
            p += (data + 1) * 8;
            continue;
        }

        switch (tag)
        {
            case SYS_Input:
            case SYS_Output:
            case SYS_Error:
            {
                BPTR fh;
                if (!Kaffeine_FileResolve(ctx, data, &fh)) { *err = ERROR_BAD_NUMBER; return FALSE; }
                native_data = (IPTR)fh;
                break;
            }
            case NP_CurrentDir:
            case NP_HomeDir:
                *err = ERROR_NOT_IMPLEMENTED;
                return FALSE;
            case SYS_Asynch:
            case SYS_UserShell:
            case NP_StackSize:
            case NP_CopyVars:
            case ADO_DirLen:
            case ADO_CommNameLen:
            case ADO_CommFileLen:
            case ADO_PromptLen:
                native_data = (IPTR)data;
                break;

            case NP_Priority:
                native_data = (IPTR)(LONG)data;
                break;

            case SYS_CustomShell:
            case NP_Name:
            case NP_CommandName:
            {
                const char *s = guest_string(ctx, data);
                if (!s)
                {
                    *err = ERROR_BAD_NUMBER;
                    return FALSE;
                }
                native_data = (IPTR)s;
                break;
            }

            default:
                *err = ERROR_NOT_IMPLEMENTED;
                return FALSE;
        }

        if (out)
        {
            out[count].ti_Tag = tag;
            out[count].ti_Data = native_data;
        }
        count++;
        p += 8;
    }

    *err = ERROR_BAD_NUMBER;
    return FALSE;
}

/* -606: SystemTagList(D1=command, D2=tags) -> D0=returnCode */
IPTR kaffeine_dos_SystemTagList(struct KaffeineContext *ctx, void *cpu)
{
    const char *cmd = guest_string(ctx, THUNK_D(1));
    ULONG guest_tags = THUNK_D(2);
    struct TagItem *tags = NULL;
    ULONG count = 0;
    SIPTR err = 0;
    LONG rc;

    if (!cmd)
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return (IPTR)-1;
    }

    if (guest_tags)
    {
        if (!walk_system_tags(ctx, guest_tags, NULL, &count, &err))
        {
            SetIoErr(err);
            return (IPTR)-1;
        }
        tags = (struct TagItem *)AllocVec((count + 1) * sizeof(struct TagItem), MEMF_CLEAR);
        if (!tags)
        {
            SetIoErr(ERROR_NO_FREE_STORE);
            return (IPTR)-1;
        }
        walk_system_tags(ctx, guest_tags, tags, &count, &err);
        tags[count].ti_Tag = TAG_DONE;
        tags[count].ti_Data = 0;
    }

    if (!Kaffeine_FileAllPrepare(ctx))
    {
        if (tags) FreeVec(tags);
        return ctx->exit_code == 20 ? 20 : -1;
    }
    rc = SystemTagList(cmd, tags);
    err = IoErr();
    if (tags)
        FreeVec(tags);
    SetIoErr(err);
    return Kaffeine_FileAllFinish(ctx) ? (IPTR)rc : 20;
}

/* -234: FreeDosObject(D1=type, D2=object) */
IPTR kaffeine_dos_FreeDosObject(struct KaffeineContext *ctx, void *cpu)
{
    /* File handles are closed through their owned association, never through
       a guest APTR passed to the native object allocator. */
    if (THUNK_D(1) == DOS_FILEHANDLE)
    {
        bug("[kaffeine] FreeDosObject: use Close for an owned FileHandle, stopping (20)\n");
        ctx->running = FALSE;
        ctx->exit_code = 20;
        return 20;
    }
    /* AllocDosObject() gave the program its RDArgs in guest memory */
    if (THUNK_D(1) == DOS_RDARGS)
        Kaffeine_FreeRDArgs(ctx, THUNK_D(2));
    else if (THUNK_D(1) == DOS_STDPKT)
        Kaffeine_FreeStdPacket(ctx, THUNK_D(2));
    /* and its ExAllControl, whose scan state is native */
    else if (THUNK_D(1) == DOS_EXALLCONTROL)
    {
        if (!Kaffeine_ExAllFreeControl(ctx, THUNK_D(2)))
        {
            bug("[kaffeine] FreeDosObject: ExAllControl in use by its MatchFunc, stopping (20)\n");
            ctx->running = FALSE;
            ctx->exit_code = 20;
            return 20;
        }
    }
    else
        FreeDosObject(THUNK_D(1), THUNK_DPTR(2));
    return 0;
}

/* VFPrintf() and VPrintf() use RawDoFmt() rules in AROS, so the checked
   formatter of kaffeine_format.c renders them on the bridge. It checks the
   whole template and data stream first and then hands over one character
   at a time, so a guest field width never sizes a native buffer. As in AROS
   vfprintf.c, each character except a zero goes out with FPutC(), and the
   result is the count, or -1 from the first failed write on. FPutC()
   refuses a NULL handle itself, so an empty text still returns 0. */
struct PrintOutput
{
    BPTR fh;
    LONG count;
};

static BOOL print_put(void *state, UBYTE ch)
{
    struct PrintOutput *out = (struct PrintOutput *)state;

    if (ch && out->count >= 0)
        out->count = FPutC(out->fh, ch) < 0 ? -1 : out->count + 1;
    return TRUE;
}

static IPTR print_formatted(struct KaffeineContext *ctx, BPTR fh, ULONG fmt, ULONG args)
{
    struct PrintOutput out;

    out.fh = fh;
    out.count = 0;
    if (!Kaffeine_FormatToPut(ctx, fmt, args, print_put, &out))
        return bad_dos_buffer(-1);
    return (IPTR)out.count;
}

/* -354: VFPrintf(D1=file, D2=format, D3=argArray) -> D0=count or -1 */
IPTR kaffeine_dos_VFPrintf(struct KaffeineContext *ctx, void *cpu)
{
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    IPTR result = print_formatted(ctx, fh, THUNK_D(2), THUNK_D(3));
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? result : 20;
}

/* -954: VPrintf(D1=format, D2=argArray) -> D0=count or -1 */
IPTR kaffeine_dos_VPrintf(struct KaffeineContext *ctx, void *cpu)
{
    ULONG token;
    if (!Kaffeine_FileStreamPrepare(ctx, FALSE, &token)) return ctx->exit_code == 20 ? 20 : -1;
    IPTR result = print_formatted(ctx, Output(), THUNK_D(1), THUNK_D(2));
    return Kaffeine_FileBufferFinish(ctx, token) ? result : 20;
}

IPTR kaffeine_dos_VFWritef(struct KaffeineContext *ctx, void *cpu)
{
    const char *fmt = guest_string(ctx, THUNK_D(2));
    if (!fmt)
        return bad_dos_buffer(0);
    ULONG size;
    /* The existing converter handles RawDoFmt streams. VFWritef's distinct
       format grammar and native IPTR argument array still need a converter. */
    BPTR fh;
    if (!Kaffeine_FileResolve(ctx, THUNK_D(1), &fh)) return 20;
    if (!Kaffeine_FileBufferPrepare(ctx, THUNK_D(1))) return ctx->running ? -1 : 20;
    APTR args = m68k_to_native_fmt_stream(ctx, fmt, THUNK_D(3), &size);
    VFWritef(fh, fmt, (RAWARG)args);
    if (args) FreeMem(args, size);
    return Kaffeine_FileBufferFinish(ctx, THUNK_D(1)) ? 0 : 20;
}
