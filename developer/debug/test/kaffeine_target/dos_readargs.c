/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Command line parsing with ReadArgs(), the way most CLI tools do it,
    from a libnix C program that uses neither stdio nor malloc. Run it as

        dos_readargs alpha beta SIZE=42 NAME=hello QUIET

    ReadArgs() fills the array with a flag for /S and with pointers for
    /K (a string), /N (a LONG) and /M (an array of strings). The program
    then parses a string of its own the same way, with an RDArgs from
    AllocDosObject() whose RDA_Source points at the string.

    With INPUT, the program then reads the first line of its input, which
    ReadArgs() has to leave alone: RunCommand() puts the command line in
    front of the input, and ReadArgs() reads only that line, as for a
    native command. Run it as

        dos_readargs alpha beta SIZE=42 NAME=hello QUIET INPUT <readargs.in

    with the line "first line of the input" in readargs.in.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "m68ktest.h"

#define TEMPLATE "FILES/M,SIZE/K/N,NAME/K,QUIET/S,INPUT/S"
#define INPUT_LINE "first line of the input"
#define SOURCE_LINE "gamma SIZE=7 NAME=source QUIET\n"

enum { ARG_FILES, ARG_SIZE, ARG_NAME, ARG_QUIET, ARG_INPUT, ARG_COUNT };

/* ReadArgs() on a string of the program, as programs parse commands of
   their own. It must not read the program's input either. */
static void source_args(void)
{
    static UBYTE line[] = SOURCE_LINE;
    LONG array[ARG_COUNT] = { 0 };
    struct RDArgs *rda = (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL);
    CONST_STRPTR *files;

    if (!check(rda != NULL, "AllocDosObject(DOS_RDARGS)"))
        return;
    rda->RDA_Source.CS_Buffer = line;
    rda->RDA_Source.CS_Length = sizeof(line) - 1;
    rda->RDA_Source.CS_CurChr = 0;
    rda->RDA_Flags |= RDAF_NOPROMPT;
    if (check(ReadArgs(TEMPLATE, array, rda) == rda,
              "ReadArgs() with RDA_Source returns the RDArgs"))
    {
        files = (CONST_STRPTR *)array[ARG_FILES];
        check_str("RDA_Source FILES/M", files ? files[0] : NULL, "gamma");
        check(files && files[1] == NULL, "RDA_Source FILES/M ends after one name");
        if (check(array[ARG_SIZE] != 0, "RDA_Source SIZE/K/N is set"))
            check_long("RDA_Source SIZE/K/N value", *(LONG *)array[ARG_SIZE], 7);
        check_str("RDA_Source NAME/K", (CONST_STRPTR)array[ARG_NAME], "source");
        check(array[ARG_QUIET] != 0, "RDA_Source QUIET/S is set");
        check(rda->RDA_Source.CS_CurChr > 0, "RDA_Source CS_CurChr moved on");
        FreeArgs(rda);
    }
    else
        PrintFault(IoErr(), "dos_readargs");
    FreeDosObject(DOS_RDARGS, rda);
}

int main(void)
{
    LONG array[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    CONST_STRPTR *files;

    rda = ReadArgs(TEMPLATE, array, NULL);
    if (!check(rda != NULL, "ReadArgs() accepts the arguments"))
    {
        PrintFault(IoErr(), "dos_readargs");
        return test_done("dos_readargs");
    }
    Printf("     array: FILES 0x%08lx SIZE 0x%08lx NAME 0x%08lx QUIET 0x%08lx\n",
           array[ARG_FILES], array[ARG_SIZE], array[ARG_NAME], array[ARG_QUIET]);

    check(array[ARG_QUIET] != 0, "QUIET/S is set");
    check_str("NAME/K", (CONST_STRPTR)array[ARG_NAME], "hello");
    if (check(array[ARG_SIZE] != 0, "SIZE/K/N is set"))
        check_long("SIZE/K/N value", *(LONG *)array[ARG_SIZE], 42);
    files = (CONST_STRPTR *)array[ARG_FILES];
    if (check(files != NULL, "FILES/M is set"))
    {
        check_str("FILES/M first name", files[0], "alpha");
        check_str("FILES/M second name", files[1], "beta");
        check(files[2] == NULL, "FILES/M ends after two names");
    }

    source_args();

    if (array[ARG_INPUT])
    {
        UBYTE line[80];
        LONG len;

        if (check(FGets(Input(), line, sizeof(line)) != NULL,
                  "FGets() reads the input after ReadArgs()"))
        {
            for (len = 0; line[len] && line[len] != '\n'; len++)
                ;
            line[len] = 0;
            check_str("first line of the input", (CONST_STRPTR)line, INPUT_LINE);
        }
    }

    FreeArgs(rda);
    return test_done("dos_readargs");
}
