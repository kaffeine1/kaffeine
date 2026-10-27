/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Second DOS string/structure batch. All filesystem objects are in RAM:.
    The separate "exit" mode flushes a line and must terminate with code 7.
*/
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <dos/stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>
#include <string.h>

#include "m68ktest.h"

/* These compile-time checks use the actual 68k NDK headers, not host
   approximations. DateTime pointers start at 14, with 68k word alignment. */
#define LAYOUT(n, e) typedef char layout_##n[(e) ? 1 : -1]
#define OFFSET(t, f, o) LAYOUT(t##_##f, offsetof(struct t, f) == (o))
LAYOUT(info_size, sizeof(struct InfoData) == 36);
OFFSET(InfoData, id_NumSoftErrors, 0);
OFFSET(InfoData, id_UnitNumber, 4);
OFFSET(InfoData, id_DiskState, 8);
OFFSET(InfoData, id_NumBlocks, 12);
OFFSET(InfoData, id_NumBlocksUsed, 16);
OFFSET(InfoData, id_BytesPerBlock, 20);
OFFSET(InfoData, id_DiskType, 24);
OFFSET(InfoData, id_VolumeNode, 28);
OFFSET(InfoData, id_InUse, 32);
LAYOUT(stamp_size, sizeof(struct DateStamp) == 12);
OFFSET(DateStamp, ds_Days, 0);
OFFSET(DateStamp, ds_Minute, 4);
OFFSET(DateStamp, ds_Tick, 8);
LAYOUT(datetime_size, sizeof(struct DateTime) == 26);
OFFSET(DateTime, dat_Stamp, 0);
OFFSET(DateTime, dat_Format, 12);
OFFSET(DateTime, dat_Flags, 13);
OFFSET(DateTime, dat_StrDay, 14);
OFFSET(DateTime, dat_StrDate, 18);
OFFSET(DateTime, dat_StrTime, 22);
LAYOUT(date_string_size, LEN_DATSTRING == 16);
LAYOUT(source_size, sizeof(struct CSource) == 12);
OFFSET(CSource, CS_Buffer, 0);
OFFSET(CSource, CS_Length, 4);
OFFSET(CSource, CS_CurChr, 8);
LAYOUT(cli_size, sizeof(struct CommandLineInterface) == 64);
OFFSET(CommandLineInterface, cli_SetName, 4);
OFFSET(CommandLineInterface, cli_CommandName, 16);
OFFSET(CommandLineInterface, cli_Prompt, 24);
OFFSET(Process, pr_Arguments, 204);

#define ROOT "RAM:kaffeine-dos2"
#define ONE ROOT "/one"
#define TWO ROOT "/two"
#define FILE_NAME ROOT "/buffered"
#define ASSIGN_NAME "KAFFEINE_DOS2_TEST"
#define ASSIGN_PATH ASSIGN_NAME ":"

static struct InfoData info __attribute__((aligned(4)));
static struct FileInfoBlock fib __attribute__((aligned(4)));
static char buffer[256];
static UBYTE replacement[] = "guest replacement\n";

static void check_bstr(BSTR bstr, const char *expected, const char *label)
{
    UBYTE *s = BADDR(bstr);
    check(s && s[0] == strlen(expected) && memcmp(s + 1, expected, strlen(expected)) == 0, label);
}

static void dates(void)
{
    struct DateStamp stamp = { 380, 754, 1150 }, later;
    struct DateTime dt, parsed;
    char day[LEN_DATSTRING], date[LEN_DATSTRING], time[LEN_DATSTRING];
    LONG format;
    BPTR lock;

    check(SetFileDate(ONE, &stamp) != 0, "SetFileDate() in RAM:");
    lock = Lock(ONE, SHARED_LOCK);
    if (check(lock != 0, "Lock() to read the changed date"))
    {
        memset(&fib, 0, sizeof(fib));
        if (check(Examine(lock, &fib) != 0, "Examine() reads changed date"))
            check_long("SetFileDate() date read back", CompareDates(&stamp, &fib.fib_Date), 0);
        UnLock(lock);
    }
    later = stamp;
    later.ds_Days++;
    check(CompareDates(&stamp, &later) > 0, "CompareDates() positive means second date is later");
    check(CompareDates(&later, &stamp) < 0, "CompareDates() negative means first date is later");
    check_long("CompareDates() equality", CompareDates(&stamp, &stamp), 0);
    for (format = FORMAT_DOS; format <= FORMAT_CDN; format++)
    {
        memset(&dt, 0, sizeof(dt));
        dt.dat_Stamp = stamp;
        dt.dat_Format = format;
        dt.dat_StrDay = day;
        dt.dat_StrDate = date;
        dt.dat_StrTime = time;
        if (check(DateToStr(&dt) != 0, "DateToStr() with three LEN_DATSTRING buffers"))
        {
            check(day[0] && date[0] && time[0], "DateToStr() filled day, date and time");
            memset(&parsed, 0, sizeof(parsed));
            parsed.dat_Format = format;
            parsed.dat_StrDate = date;
            parsed.dat_StrTime = time;
            if (check(StrToDate(&parsed) != 0, "StrToDate() with NULL day pointer"))
                check_long("DateToStr()/StrToDate() round trip", CompareDates(&stamp, &parsed.dat_Stamp), 0);
        }
    }
    dt.dat_StrDay = NULL;
    dt.dat_StrDate = NULL;
    dt.dat_StrTime = NULL;
    check(DateToStr(&dt) != 0, "DateToStr() accepts three NULL buffers");
    check(StrToDate(&dt) != 0 && CompareDates(&stamp, &dt.dat_Stamp) == 0,
          "StrToDate() NULL strings preserve the stamp");
}

static void names_and_arguments(void)
{
    char old_prompt[256], old_dir[256], old_program[256];
    struct Process *process = (struct Process *)SysBase->ThisTask;
    struct CommandLineInterface *cli = Cli();
    STRPTR args, previous;
    LONG position;
    struct CSource source;
    char text[] = "one \"two words\"";
    static const char line[] = "dos_strings: WriteChars reached guest output\n";

    position = SplitName("RAM:dir/file", '/', buffer, 0, sizeof(buffer));
    check_long("SplitName() next position", position, 8);
    check_str("SplitName() first part", buffer, "RAM:dir");
    check_long("SplitName() final part", SplitName("RAM:dir/file", '/', buffer, position, sizeof(buffer)), -1);
    check_str("SplitName() final text", buffer, "file");
    /* NDK 3.2 labels these keyword/arg_template, but the first argument
       goes to D1 and must contain the template, as in AROS ReadArgs(). */
    check_long("FindArg() template D1, keyword D2", FindArg("FROM/A,TO/A,QUIET=Q/S", "q"), 2);
    check_long("FindArg() missing keyword", FindArg("FROM/A,TO/A", "missing"), -1);
    source.CS_Buffer = text;
    source.CS_Length = strlen(text);
    source.CS_CurChr = 0;
    check_long("ReadItem() first item", ReadItem(buffer, sizeof(buffer), &source), ITEM_UNQUOTED);
    check_str("ReadItem() first text", buffer, "one");
    check_long("ReadItem() second item", ReadItem(buffer, sizeof(buffer), &source), ITEM_QUOTED);
    check_str("ReadItem() quoted text", buffer, "two words");
    check(source.CS_CurChr > 4, "ReadItem() writes back CS_CurChr");
    check_long("WriteChars() byte count", WriteChars(line, sizeof(line) - 1), sizeof(line) - 1);

    args = GetArgStr();
    check(args == process->pr_Arguments, "GetArgStr() equals guest ThisTask pr_Arguments");
    previous = SetArgStr(replacement);
    check(previous == args && GetArgStr() == replacement && process->pr_Arguments == replacement,
          "SetArgStr() changes guest arguments and returns the previous guest pointer");
    check(SetArgStr(args) == replacement && GetArgStr() == args, "SetArgStr() restores original arguments");
    if (!check(cli != NULL, "Cli() for names and prompt"))
        return;
    check(GetPrompt(old_prompt, sizeof(old_prompt)) != 0, "GetPrompt() before changes");
    check(GetCurrentDirName(old_dir, sizeof(old_dir)) != 0, "GetCurrentDirName() before changes");
    check(GetProgramName(old_program, sizeof(old_program)) != 0, "GetProgramName() before changes");
    check(SetPrompt("dos2> ") != 0, "SetPrompt()");
    check(GetPrompt(buffer, sizeof(buffer)) != 0, "GetPrompt() after SetPrompt()");
    check_str("guest prompt", buffer, "dos2> ");
    check_bstr(cli->cli_Prompt, "dos2> ", "prompt agrees with guest CLI BSTR");
    check(SetCurrentDirName(ROOT) != 0, "SetCurrentDirName()");
    check(GetCurrentDirName(buffer, sizeof(buffer)) != 0, "GetCurrentDirName() after setter");
    check_str("guest directory name", buffer, ROOT);
    check_bstr(cli->cli_SetName, ROOT, "directory name agrees with guest CLI BSTR");
    check(SetProgramName("dos_strings guest") != 0, "SetProgramName()");
    check(GetProgramName(buffer, sizeof(buffer)) != 0, "GetProgramName() after setter");
    check_str("guest program name", buffer, "dos_strings guest");
    check_bstr(cli->cli_CommandName, "dos_strings guest", "program name agrees with guest CLI BSTR");
    check(SetPrompt(old_prompt) != 0, "restore prompt");
    check(SetCurrentDirName(old_dir) != 0, "restore current directory name");
    check(SetProgramName(old_program) != 0, "restore program name");
}

static void assigns(void)
{
    BPTR one, two, compare, resolved;
    BOOL assigned = FALSE;

    one = Lock(ONE, SHARED_LOCK);
    if (check(one != 0, "Lock() for AssignLock()"))
    {
        assigned = AssignLock(ASSIGN_NAME, one) != 0;
        check(assigned, "AssignLock() transfers directory lock");
        if (!assigned)
            UnLock(one);
    }
    if (assigned)
    {
        two = Lock(TWO, SHARED_LOCK);
        if (check(two != 0, "Lock() for AssignAdd()"))
        {
            if (check(AssignAdd(ASSIGN_NAME, two) != 0, "AssignAdd() transfers second directory lock"))
            {
                compare = Lock(TWO, SHARED_LOCK);
                if (check(compare != 0, "Lock() for RemAssignList() comparison"))
                {
                    check(RemAssignList(ASSIGN_NAME, compare) != 0, "RemAssignList() removes second directory");
                    UnLock(compare);
                }
            }
            else
                UnLock(two);
        }
        check(AssignLock(ASSIGN_NAME, 0) != 0, "AssignLock(NULL) removes assign and frees owned locks");
    }
    if (check(AssignLate(ASSIGN_NAME, ONE) != 0, "AssignLate() to RAM: directory"))
    {
        resolved = Lock(ASSIGN_PATH, SHARED_LOCK);
        check(resolved != 0, "resolve deferred assign");
        if (resolved) UnLock(resolved);
        check(AssignLock(ASSIGN_NAME, 0) != 0, "remove deferred assign");
    }
    if (check(AssignPath(ASSIGN_NAME, TWO) != 0, "AssignPath() to RAM: directory"))
    {
        resolved = Lock(ASSIGN_PATH, SHARED_LOCK);
        check(resolved != 0, "resolve path assign");
        if (resolved) UnLock(resolved);
        check(AssignLock(ASSIGN_NAME, 0) != 0, "remove path assign");
    }
}

int main(int argc, char **argv)
{
    BPTR root, one, two, ram, file;

    if (argc > 1 && strcmp(argv[1], "exit") == 0)
    {
        PutStr("dos_strings exit: stopping with 7\n");
        test_flush();
        Exit(7);
        PutStr("FAIL Exit() returned to guest code\n");
        return RETURN_FAIL;
    }
    root = CreateDir(ROOT);
    if (!check(root != 0, "CreateDir() test root in RAM:"))
        return test_done("dos_strings");
    UnLock(root);
    one = CreateDir(ONE);
    two = CreateDir(TWO);
    check(one != 0 && two != 0, "CreateDir() two assign directories");
    if (one) UnLock(one);
    if (two) UnLock(two);
    ram = Lock("RAM:", SHARED_LOCK);
    if (check(ram != 0, "Lock(RAM:)"))
    {
        memset(&info, 0, sizeof(info));
        if (check(Info(ram, &info) != 0, "Info(RAM:) converts InfoData"))
        {
            check(info.id_BytesPerBlock > 0 && info.id_NumBlocks >= info.id_NumBlocksUsed,
                  "Info() block counts and size");
            check(info.id_VolumeNode == 0, "Info() exposes no native DosList pointer");
        }
        UnLock(ram);
    }
    check(IsFileSystem("RAM:") != 0, "IsFileSystem(RAM:)");
    file = Open(FILE_NAME, MODE_NEWFILE);
    if (check(file != 0, "Open() for SetVBuf()"))
    {
        check_long("SetVBuf() with DOS-owned buffer", SetVBuf(file, NULL, BUF_FULL, 208), 0);
        check_long("FWrite() after SetVBuf()", FWrite(file, "test", 1, 4), 4);
        check(Close(file) != 0, "Close() frees the native SetVBuf() buffer");
    }
    dates();
    names_and_arguments();
    assigns();
    check(DeleteFile(FILE_NAME) != 0, "remove buffered file");
    check(DeleteFile(TWO) != 0, "remove second directory");
    check(DeleteFile(ONE) != 0, "remove first directory");
    check(DeleteFile(ROOT) != 0, "remove test root");
    return test_done("dos_strings");
}
