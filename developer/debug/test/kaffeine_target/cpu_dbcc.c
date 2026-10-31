/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Original 68k DBcc sequences, with observable branch targets and CCR.
*/
#include "m68ktest.h"

static ULONG run_code(const UWORD *code, ULONG *counter)
{
    register ULONG result __asm("d0");
    register ULONG remaining __asm("d1");
    register const UWORD *entry __asm("a0") = code;

    __asm volatile("jsr (%2)"
                   : "=d" (result), "=d" (remaining)
                   : "a" (entry)
                   : "a1", "cc", "memory");
    *counter = remaining;
    return result;
}

int main(void)
{
    /* MOVEQ #0,D0; MOVEQ #1,D1; ADDQ.W #1,D0; DBF D1,-4; RTS. */
    static const UWORD backward[] = {0x7000, 0x7201, 0x5240, 0x51C9, 0xFFFC, 0x4E75};
    /* Dirty high word: DBF changes only D1.W, then copies D1 to D0. */
    static const UWORD upper_word[] = {0x223C, 0xC0DE, 0x0001, 0x51C9, 0xFFFE,
                                      0x2001, 0x4E75};
    /* Taken DBF skips MOVEQ #-1,D0 / RTS and lands on MOVEQ #7,D0. */
    static const UWORD forward[] = {0x7000, 0x7201, 0x51C9, 0x0006,
                                   0x70FF, 0x4E75, 0x7007, 0x4E75};
    /* Z is set: DBEQ falls through and does not decrement D1.W. */
    static const UWORD condition_true[] = {0x7201, 0x7000, 0x57C9, 0x0006,
                                          0x7007, 0x4E75, 0x70FF, 0x4E75};
    /* MOVE #$1F,CCR; taken DBF skips NOP and lands exactly on MOVE CCR,D0. */
    static const UWORD flags[] = {0x7201, 0x44FC, 0x001F, 0x51C9, 0x0004,
                                 0x4E71, 0x42C0, 0x4E75};
    ULONG value, counter;

    value = run_code(backward, &counter);
    check_long("backward DBF executes both loop bodies", value, 2);
    check_long("backward DBF exhausts the low counter word", counter, 0xFFFF);
    value = run_code(upper_word, &counter);
    check_long("DBF preserves the upper counter word", value, 0xC0DEFFFF);
    check_long("self-loop DBF terminates with the original upper word", counter, 0xC0DEFFFF);
    value = run_code(forward, &counter);
    check_long("forward DBF lands on the target instruction", value, 7);
    check_long("taken forward DBF decrements once", counter, 0);
    value = run_code(condition_true, &counter);
    check_long("true DBEQ condition falls through", value, 7);
    check_long("true DBEQ condition preserves the counter", counter, 1);
    value = run_code(flags, &counter);
    check_long("taken DBF preserves every CCR bit", value & 31, 31);
    check_long("CCR test takes its forward DBF once", counter, 0);
    return test_done("cpu_dbcc");
}
