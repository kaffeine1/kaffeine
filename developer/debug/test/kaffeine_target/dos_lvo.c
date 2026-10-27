/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Isolated regressions for dos.library calls whose kaffeine thunks used
    to sit at the wrong LVO or read the wrong register. The first argument
    selects a call so a blocking regression does not hide the others:

        dos_lvo fprintf        FPrintf(), which calls VFPrintf() (LVO -354)
        dos_lvo cli            Cli() (LVO -492)
        dos_lvo freedosobject  FreeDosObject() (LVO -234, object in D2)
        dos_lvo launch         CreateNewProc() (LVO -498) and CreateProc()
                               (LVO -138) with 68k code, which must fail
                               until kaffeine can start an emulator for
                               the new process
        dos_lvo system         SystemTagList() (LVO -606, command in D1 and
                               tags in D2) without tags and with NP_StackSize

    The argument is read from the libnix argv.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "m68ktest.h"

/* pr_Arguments is the argument string of the command; compare only its
   start, since the copy in guest memory need not end in NUL. */
static BOOL arguments_are(struct Process *process, const char *expected)
{
    const char *arguments = (const char *)process->pr_Arguments;

    while (arguments && *arguments == ' ')
        arguments++;
    return arguments && strncmp(arguments, expected, strlen(expected)) == 0;
}

/* Never runs: no process is created for it */
static void child_entry(void)
{
}

static void check_file_line(CONST_STRPTR path, CONST_STRPTR expected,
                            CONST_STRPTR label)
{
    char buf[64];
    BPTR fh;
    LONG len = strlen(expected);
    LONG n = -1;

    memset(buf, 0, sizeof(buf));
    fh = Open(path, MODE_OLDFILE);
    if (check(fh != 0, label))
    {
        n = Read(fh, buf, sizeof(buf) - 1);
        Close(fh);
        DeleteFile(path);
        if (n > 0 && buf[n - 1] == '\n')
            buf[--n] = '\0';
        check_long(label, n, len);
        check_str(label, buf, expected);
    }
}

int main(int argc, char **argv)
{
    const char *call = argc > 1 ? argv[1] : "";
    struct Process *process;
    struct CommandLineInterface *cli;
    struct FileInfoBlock *fib;
    LONG n;

    if (strcmp(call, "fprintf") == 0)
    {
        n = FPrintf(Output(), "     FPrintf(): %ld and %s\n", 42, (LONG)"text");
        test_flush();
        check_long("FPrintf() returns the number of characters", n, 28);
    }
    else if (strcmp(call, "cli") == 0)
    {
        process = (struct Process *)SysBase->ThisTask;
        cli = Cli();
        check(cli != NULL, "Cli() returns a CLI for a Shell command");
        if (check(process != NULL, "SysBase->ThisTask returns a process"))
        {
            check_long("Cli() is the CLI of SysBase->ThisTask",
                       (LONG)cli, (LONG)BADDR(process->pr_CLI));
            check(arguments_are(process, "cli"),
                  "SysBase->ThisTask->pr_Arguments holds the command line");
        }
        check_long("Cli() keeps the same guest address", (LONG)Cli(), (LONG)cli);
        process = (struct Process *)FindTask(NULL);
        if (check(process != NULL, "FindTask(NULL) returns a process"))
        {
            check_long("Cli() is also the CLI of FindTask(NULL)",
                       (LONG)cli, (LONG)BADDR(process->pr_CLI));
            check(arguments_are(process, "cli"),
                  "FindTask(NULL)->pr_Arguments holds the command line");
        }
    }
    else if (strcmp(call, "freedosobject") == 0)
    {
        fib = AllocDosObject(DOS_FIB, NULL);
        if (check(fib != NULL, "AllocDosObject(DOS_FIB)"))
        {
            FreeDosObject(DOS_FIB, fib);
            check(TRUE, "FreeDosObject(DOS_FIB) returns");
        }
    }
    else if (strcmp(call, "launch") == 0)
    {
        struct TagItem tags[] =
        {
            { NP_Entry, (ULONG)child_entry },
            { NP_Name, (ULONG)"dos_lvo child" },
            { NP_StackSize, 4096 },
            { TAG_DONE, 0 }
        };

        struct Process *child;
        struct MsgPort *port;
        LONG error;

        /* IoErr() is read at once: printing a check line changes it */
        cli = Cli();
        child = CreateNewProc(tags);
        error = IoErr();
        check(child == NULL, "CreateNewProc() of 68k code fails");
        check_long("CreateNewProc() IoErr", error, ERROR_NOT_IMPLEMENTED);
        port = CreateProc("dos_lvo child", 0, cli ? cli->cli_Module : 0, 4096);
        error = IoErr();
        check(port == NULL, "CreateProc() of the program's own seglist fails");
        check_long("CreateProc() IoErr", error, ERROR_NOT_IMPLEMENTED);
    }
    else if (strcmp(call, "system") == 0)
    {
        struct TagItem stack_tags[] =
        {
            { NP_StackSize, 8192 },
            { TAG_DONE, 0 }
        };

        DeleteFile("RAM:kaffeine-system.txt");
        n = SystemTagList("Echo >RAM:kaffeine-system.txt system-ok", NULL);
        check_long("SystemTagList() without tags rc", n, 0);
        check_file_line("RAM:kaffeine-system.txt", "system-ok",
                        "SystemTagList() without tags output");

        DeleteFile("RAM:kaffeine-system-stack.txt");
        n = SystemTagList("Echo >RAM:kaffeine-system-stack.txt stack-ok", stack_tags);
        check_long("SystemTagList() with NP_StackSize rc", n, 0);
        check_file_line("RAM:kaffeine-system-stack.txt", "stack-ok",
                        "SystemTagList() with NP_StackSize output");
    }
    else
    {
        check(FALSE, "argument fprintf, cli, freedosobject, launch or system");
    }

    return test_done("dos_lvo");
}
