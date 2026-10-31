/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original 68020 full-format, non-indirect indexed bytecode. */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"
int main(void)
{
    UBYTE *memory=AllocMem(8192,MEMF_PUBLIC);
    ULONG base_after,index_after;
    UWORD flags;
    unsigned i;
    BOOL bytes=TRUE;
    if(!check(memory!=NULL,"allocate guarded full-index operand storage"))goto done;
    memset(memory,0xA5,8192);
    {
        register UBYTE *base __asm("a2")=memory;
        register ULONG index __asm("a0")=3;
        register UBYTE *address __asm("a3");
        __asm volatile(
            "move.w #31,%%ccr\n\t"
            ".word 0x4232,0x8d20,0x0e70\n\t" /* CLR.B (3696,A2,A0.L*4) */
            ".word 0x47f2,0x8d20,0x0e70\n\t" /* LEA same address,A3 */
            "move.w %%ccr,%0\n\t"
            "move.l %%a2,%1\n\t"
            "move.l %%a0,%2"
            : "=d"(flags),"=d"(base_after),"=d"(index_after),"=a"(address)
            : "a"(base),"a"(index)
            : "cc","memory");
        check(address==memory+3696+12,"LEA resolves a scaled address index and word base displacement");
    }
    for(i=0;i<8192;i++)if(memory[i]!=(i==3708?0:0xA5))bytes=FALSE;
    check(bytes,"CLR.B writes only the byte at the full-format indexed address");
    check((flags&31)==20,"CLR sets Z, clears NVC and retains X across LEA");
    check(base_after==(ULONG)memory&&index_after==3,"indexed addressing preserves base and index registers");
done:
    if(memory)FreeMem(memory,8192);
    return test_done("cpu_full_index");
}
