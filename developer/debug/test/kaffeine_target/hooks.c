/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Guest hooks and RawDoFmt callbacks. The stubs translate the NDK 3.2
    register conventions to the ordinary libnix C stack convention.
*/
#include <exec/types.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include "m68ktest.h"

struct Library *UtilityBase;

extern ULONG hook_entry(void);
extern void put_entry(void), copy_entry(void), carried_entry(void);

__asm__(
    ".text\n"
    ".even\n"
    ".globl _hook_entry\n"
    "_hook_entry:\n"
    "move.l %a1,-(%sp)\n"
    "move.l %a2,-(%sp)\n"
    "move.l %a0,-(%sp)\n"
    "move.l 12(%a0),%a0\n"
    "jsr (%a0)\n"
    "lea 12(%sp),%sp\n"
    "rts\n"
    ".globl _put_entry\n"
    "_put_entry:\n"
    "move.l %a3,-(%sp)\n"
    "move.l %d0,-(%sp)\n"
    "jsr _put_character\n"
    "lea 8(%sp),%sp\n"
    "rts\n"
    ".globl _copy_entry\n"
    "_copy_entry:\n"
    "move.b %d0,(%a3)+\n"
    "rts\n"
    ".globl _carried_entry\n"
    "_carried_entry:\n"
    "nop\n"
    "move.b %d0,(%a3)+\n"
    "rts\n"
);

struct Output
{
    ULONG count;
    UBYTE *next;
};

/* Referenced by the assembler stub, so keep external linkage. */
void put_character(ULONG ch, struct Output *output)
{
    output->count++;
    *output->next++ = (UBYTE)ch;
}

static ULONG value_hook(struct Hook *hook, ULONG *object, ULONG *message)
{
    return (ULONG)hook->h_Data + *object + *message;
}

static ULONG nested_hook(struct Hook *hook, APTR object, APTR message)
{
    return CallHookPkt((struct Hook *)hook->h_Data, object, message) + 1;
}

static void check_hooks(void)
{
    ULONG object = 23, message = 31;
    struct Hook inner = { {0}, hook_entry, (ULONG (*)())value_hook, (APTR)17 };
    struct Hook outer = { {0}, hook_entry, (ULONG (*)())nested_hook, &inner };

    check_long("CallHookPkt() passes A0, A2 and A1 to a C hook",
               CallHookPkt(&inner, &object, &message), 71);
    check_long("a C hook calls utility.library and a nested C hook",
               CallHookPkt(&outer, &object, &message), 72);
}

static void check_formatting(void)
{
    static UBYTE bstr[8] __attribute__((aligned(4))) = { 3, 'B', 'C', 'P' };
    struct Arguments
    {
        WORD small;
        LONG large;
        STRPTR string;
        ULONG bstring;
        UWORD character, hex, upper;
    } __attribute__((packed, aligned(2))) args;
    static const char format[] = "%05d|%ld|%s|%b|%c|%x|%X|%%";
    static const char expected[] = "-0012|-123456|guest|BCP|Q|BEEF|ABCD|%";
    char buffer[128], short_buffer[8];
    struct Output output;
    APTR next;
    LONG required;
    ULONG count = 5;

    args.small = -12; args.large = -123456; args.string = "guest";
    args.bstring = (ULONG)bstr >> 2;
    args.character = 'Q'; args.hex = 0xBEEF; args.upper = 0xABCD;

    next = RawDoFmt(format, &args, copy_entry, buffer);
    check_str("RawDoFmt() standard copy routine and packed WORD/LONG arguments", buffer, expected);
    check_long("RawDoFmt() returns the advanced argument pointer",
               (LONG)next, (LONG)((UBYTE *)&args + sizeof(args)));

    output.count = 0; output.next = (UBYTE *)buffer;
    next = RawDoFmt(format, &args, put_entry, &output);
    check_str("RawDoFmt() calls a custom C PutChProc", buffer, expected);
    check_long("the custom PutChProc counts the terminating zero", output.count, sizeof(expected));
    check_long("the custom PutChProc gets every character", output.next - (UBYTE *)buffer, sizeof(expected));
    check_long("the custom callback keeps RawDoFmt()'s return value",
               (LONG)next, (LONG)((UBYTE *)&args + sizeof(args)));

    RawDoFmt("carry A3", NULL, carried_entry, buffer);
    check_str("A3 advances across interpreted PutChProc calls", buffer, "carry A3");
    RawDoFmt(format, &args, (void (*)())0, buffer);
    check_str("RAWFMTFUNC_STRING (0)", buffer, expected);
    RawDoFmt(format, &args, (void (*)())2, &count);
    check_long("RAWFMTFUNC_COUNT (2) includes zero and keeps the initial count",
               count, 5 + sizeof(expected));

    required = VSNPrintf(buffer, sizeof(buffer), format, &args);
    check_str("VSNPrintf() uses the same packed guest formatter", buffer, expected);
    check_long("VSNPrintf() includes the terminating zero in the required size",
               required, sizeof(expected));
    required = VSNPrintf(short_buffer, sizeof(short_buffer), format, &args);
    check_str("VSNPrintf() truncates and terminates the output", short_buffer, "-0012|-");
    check_long("VSNPrintf() reports the full size after truncation", required, sizeof(expected));
    check_long("VSNPrintf(NULL) only counts", VSNPrintf(NULL, 0, format, &args), sizeof(expected));
}

int main(void)
{
    UtilityBase = OpenLibrary("utility.library", 0);
    if (!check(UtilityBase != NULL, "open utility.library")) return test_done("hooks");
    check_hooks();
    check_formatting();
    CloseLibrary(UtilityBase);
    return test_done("hooks");
}
