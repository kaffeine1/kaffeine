/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Deliberate guest faults. Build without FPU startup so the instruction
    below reaches Moira even while AttnFlags correctly advertises no FPU.
    With the argument "mmu" it only runs PFLUSHA, a 68040 MMU instruction.
*/
#include "m68ktest.h"

#include <string.h>

static void step(const char *message)
{
    PutStr((CONST_STRPTR)message);
    PutStr((CONST_STRPTR)"\n");
    test_flush();
}

/* A C access to this odd constant is split into bytes by the 68000
   compiler. Keep the actual word instruction that reaches one callback. */
static UWORD read_odd_word(void)
{
    register volatile UWORD *addr __asm("a0") = (volatile UWORD *)0xFFFFFFFFUL;
    register ULONG value __asm("d0") = 0;

    __asm volatile ("move.w (%1),%0" : "+d"(value) : "a"(addr) : "cc", "memory");
    return (UWORD)value;
}

static void write_odd_word(UWORD value)
{
    register volatile UWORD *addr __asm("a0") = (volatile UWORD *)0xFFFFFFFFUL;
    register ULONG data __asm("d0") = value;

    __asm volatile ("move.w %0,(%1)" : : "d"(data), "a"(addr) : "cc", "memory");
}

static UWORD read_vector_word(void)
{
    ULONG value = 0;

    /* Guest address zero is valid. Express it in assembler to avoid a C
       null-pointer dereference and its compiler transformations. */
    __asm volatile ("move.w 0.w,%0" : "+d"(value) : : "cc", "memory");
    return (UWORD)value;
}

int main(int argc, char **argv)
{
    volatile ULONG *longword = (volatile ULONG *)0xFFFFFFFCUL;
    /* An aligned word: the 68000 compiler splits a word at an odd address
       into two byte accesses, and the second one wraps to address 0. */
    volatile UWORD *word = (volatile UWORD *)0xFFFFFFFEUL;
    volatile UBYTE *byte = (volatile UBYTE *)0xFFFFFFFFUL;

    if (argc == 2 && strcmp(argv[1], "mmu") == 0)
    {
        step("STEP cpu_faults: PFLUSHA, expect RETURN_FAIL 20 and caller resumes");
        __asm volatile (".word 0xf518" : : : "memory");
        step("FAIL cpu_faults: PFLUSHA returned");
        return RETURN_ERROR;
    }

    step("STEP cpu_faults: read long at $FFFFFFFC, expect zero");
    check_long("long read outside arena", *longword, 0);
    step("STEP cpu_faults: write long at $FFFFFFFC, expect ignored");
    *longword = 0x12345678UL;
    check_long("long write outside arena was ignored", *longword, 0);
    step("STEP cpu_faults: read word at $FFFFFFFE, expect zero");
    check_long("word read outside arena", *word, 0);
    step("STEP cpu_faults: write word at $FFFFFFFE, expect ignored");
    *word = 0x5678;
    check_long("word write outside arena was ignored", *word, 0);
    step("STEP cpu_faults: MOVE.W (A0),D0 at $FFFFFFFF, expect zero");
    check_long("odd word read outside arena", read_odd_word(), 0);
    step("STEP cpu_faults: MOVE.W D0,(A0) at $FFFFFFFF, expect ignored");
    {
        UWORD vector_word = read_vector_word();

        write_odd_word(0x5678);
        check_long("odd word write outside arena was ignored", read_odd_word(), 0);
        check_long("odd word write kept guest vector zero", read_vector_word(), vector_word);
    }
    step("STEP cpu_faults: read byte at $FFFFFFFF, expect zero");
    check_long("byte read outside arena", *byte, 0);
    step("STEP cpu_faults: write byte at $FFFFFFFF, expect ignored");
    *byte = 0x12;
    check_long("byte write outside arena was ignored", *byte, 0);
    if (test_done("cpu_faults bounds") != RETURN_OK)
        return RETURN_ERROR;

    step("STEP cpu_faults: FMOVE.L FPCR,D0, expect RETURN_FAIL 20 and caller resumes");
    __asm volatile (".word 0xf200,0xb000" : : : "d0", "memory");
    step("FAIL cpu_faults: unsupported FPU instruction returned");
    return RETURN_ERROR;
}
