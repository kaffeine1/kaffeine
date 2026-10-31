/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    exec.library memory, signal and task functions, called from a libnix
    C program that uses neither stdio nor malloc. kaffeine serves AllocMem(),
    AllocVec() and the pool functions from its own heap inside the 68k
    address space, so the checks cover what programs expect from exec:
    blocks that do not overlap, MEMF_CLEAR also for reused memory, the
    8 byte alignment of exec allocations and AvailMem() following the
    allocations. The heap must also leave the custom chip registers at
    $DFF000 alone and keep the memory 16 MB higher.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/alib.h>
#include <string.h>

#include "m68ktest.h"

#define SLOTS 16

static struct SignalSemaphore semaphore;
static struct SignalSemaphore semaphore_other;
static struct List semaphore_list;
static struct SemaphoreMessage semaphore_message;

static void check_semaphore(CONST_STRPTR what, struct SignalSemaphore *sem,
                            WORD nest, struct Task *owner)
{
    check(sem->ss_NestCount == nest && sem->ss_QueueCount == nest - 1 &&
          sem->ss_Owner == owner, what);
}

static void semaphore_checks(void)
{
    struct Task *me = FindTask(NULL);
    BOOL obtained;

    InitSemaphore(&semaphore);
    check(semaphore.ss_Link.ln_Type == NT_SIGNALSEM &&
          semaphore.ss_WaitQueue.mlh_Head == (struct MinNode *)&semaphore.ss_WaitQueue.mlh_Tail &&
          semaphore.ss_WaitQueue.mlh_Tail == NULL &&
          semaphore.ss_WaitQueue.mlh_TailPred == (struct MinNode *)&semaphore.ss_WaitQueue.mlh_Head,
          "InitSemaphore() initializes the 68k wait queue and type");
    check_semaphore("InitSemaphore() has zero nest count, NULL owner and queue count -1",
                    &semaphore, 0, NULL);
    obtained = AttemptSemaphoreShared(&semaphore) != 0;
    check_long("AttemptSemaphoreShared() of a free semaphore", obtained, 1);
    if (obtained)
    {
        check_semaphore("shared attempt has nest count 1 and NULL owner", &semaphore, 1, NULL);
        ReleaseSemaphore(&semaphore);
    }
    ObtainSemaphore(&semaphore);
    check_semaphore("exclusive obtain uses FindTask(NULL) as its owner", &semaphore, 1, me);
    obtained = AttemptSemaphore(&semaphore) != 0;
    check(obtained, "AttemptSemaphore() nests an exclusive lock");
    if (obtained)
    {
        check_semaphore("nested exclusive attempt has nest count 2", &semaphore, 2, me);
        ReleaseSemaphore(&semaphore);
    }
    ObtainSemaphoreShared(&semaphore);
    check_semaphore("shared obtain nests our exclusive lock without changing its owner",
                    &semaphore, 2, me);
    obtained = AttemptSemaphoreShared(&semaphore) != 0;
    check(obtained, "shared attempt nests our exclusive lock");
    if (obtained)
        ReleaseSemaphore(&semaphore);
    ReleaseSemaphore(&semaphore);
    ReleaseSemaphore(&semaphore);
    check_semaphore("exclusive releases restore the free semaphore", &semaphore, 0, NULL);

    ObtainSemaphoreShared(&semaphore);
    ObtainSemaphoreShared(&semaphore);
    check_semaphore("two shared obtains have nest count 2 and NULL owner", &semaphore, 2, NULL);
    check(AttemptSemaphore(&semaphore) == 0, "exclusive attempt of a shared lock fails");
    check_semaphore("failed exclusive attempt preserves shared counts", &semaphore, 2, NULL);
    ReleaseSemaphore(&semaphore);
    ReleaseSemaphore(&semaphore);
    check_semaphore("shared releases restore the free semaphore", &semaphore, 0, NULL);

    InitSemaphore(&semaphore_other);
    NewList(&semaphore_list);
    AddTail(&semaphore_list, &semaphore.ss_Link);
    AddTail(&semaphore_list, &semaphore_other.ss_Link);
    ObtainSemaphoreList(&semaphore_list);
    ObtainSemaphoreList(&semaphore_list);
    check_semaphore("ObtainSemaphoreList() nests the first member", &semaphore, 2, me);
    check_semaphore("ObtainSemaphoreList() nests the second member", &semaphore_other, 2, me);
    ReleaseSemaphoreList(&semaphore_list);
    ReleaseSemaphoreList(&semaphore_list);
    check_semaphore("ReleaseSemaphoreList() frees the first member", &semaphore, 0, NULL);
    check_semaphore("ReleaseSemaphoreList() frees the second member", &semaphore_other, 0, NULL);
    RemHead(&semaphore_list);
    RemHead(&semaphore_list);
    ObtainSemaphoreList(&semaphore_list);
    ReleaseSemaphoreList(&semaphore_list);
    check(IsListEmpty(&semaphore_list), "semaphore list calls accept an empty list");

    semaphore.ss_Link.ln_Name = "kaffeine.exec_memory.low";
    semaphore.ss_Link.ln_Pri = -100;
    semaphore_other.ss_Link.ln_Name = "kaffeine.exec_memory.high";
    semaphore_other.ss_Link.ln_Pri = 100;
    AddSemaphore(&semaphore);
    AddSemaphore(&semaphore_other);
    check(FindSemaphore("kaffeine.exec_memory.low") == &semaphore &&
          FindSemaphore("kaffeine.exec_memory.low") == &semaphore,
          "FindSemaphore() returns the published guest object every time");
    check(FindSemaphore("kaffeine.exec_memory.high") == &semaphore_other,
          "FindSemaphore() finds a second public semaphore");
    check(SysBase->SemaphoreList.lh_Head == &semaphore_other.ss_Link,
          "AddSemaphore() uses signed priority in the guest ExecBase list");
    RemSemaphore(&semaphore);
    RemSemaphore(&semaphore_other);
    check(FindSemaphore("kaffeine.exec_memory.low") == NULL &&
          FindSemaphore("kaffeine.exec_memory.high") == NULL,
          "RemSemaphore() removes both names from the guest public list");
}

/* The custom chips answer at $DFF000 only. The heap gives no memory there,
   and the same 4 KB at $1DFF000 are ordinary memory: before the fix, 68k
   code read POTGOR and the beam position there and lost its writes. Any
   read of the old return sentinel at $DEAD00 stopped the program. That
   address may now belong to an I/O mirror: read it without modifying its
   owner, and exercise absolute writes in a separately freed block. */
static void custom_window_checks(ULONG before)
{
    volatile UWORD *vhposr = (volatile UWORD *)0x00DFF006;
    volatile ULONG *alias, *old_sentinel;
    UWORD first, second;
    APTR block, scratch;
    ULONG sentinel_value;
    BOOL ok = TRUE;
    LONG i;

    block = AllocAbs(0x1000, (APTR)0x00DFF000);
    check(block == NULL, "AllocAbs() refuses the custom chip registers at $DFF000");
    if (block)
        FreeMem(block, 0x1000);
    first = *vhposr;
    second = *vhposr;
    check(first != second, "VHPOSR at $DFF006 still advances between two reads");
    block = AllocAbs(0x1000, (APTR)0x01DFF000);
    if (check(block == (APTR)0x01DFF000, "AllocAbs() of the 4 KB at $1DFF000"))
    {
        alias = (volatile ULONG *)block;
        for (i = 0; i < 1024; i++)
            alias[i] = 0x5A000000UL | i;
        for (i = 0; i < 1024 && ok; i++)
            ok = alias[i] == (0x5A000000UL | i);
        check(ok, "the 4 KB at $1DFF000 keep what 68k code writes there");
        FreeMem(block, 0x1000);
        check(AvailMem(MEMF_ANY) == before, "FreeMem() of the AllocAbs() block restores AvailMem()");
    }
    old_sentinel = (volatile ULONG *)0x00DEAD00;
    sentinel_value = old_sentinel[0];
    check(old_sentinel[0] == sentinel_value,
          "the old sentinel at $DEAD00 is ordinary readable guest memory");
    scratch = AllocMem(16, MEMF_PUBLIC);
    block = NULL;
    if (scratch)
    {
        FreeMem(scratch, 16);
        block = AllocAbs(16, scratch);
    }
    if (check(scratch && block == scratch, "AllocAbs() of 16 bytes in a known freed block"))
    {
        old_sentinel = (volatile ULONG *)block;
        old_sentinel[0] = 0x4E714E71UL;
        old_sentinel[1] = 0x12345678UL;
        check(old_sentinel[0] == 0x4E714E71UL && old_sentinel[1] == 0x12345678UL,
              "the absolute block keeps what 68k code writes there");
        FreeMem(block, 16);
    }
}

static BOOL all_bytes(const UBYTE *p, ULONG size, UBYTE value)
{
    ULONG i;

    for (i = 0; i < size; i++)
        if (p[i] != value)
            return FALSE;
    return TRUE;
}

static ULONG slot_size(LONG i)
{
    return 1 + (ULONG)(i * 97) % 2000;
}

int main(int argc, char **argv)
{
    UBYTE *slot[SLOTS] = { NULL };
    ULONG size[SLOTS] = { 0 };
    UBYTE *p, *q;
    APTR pool;
    ULONG before, after, total, misaligned;
    BOOL ok;
    LONG i, j, sig;
    struct Task *me, *again;

    /* Optional negative probes, separate from the normal passing battery.
       The bridge must stop at either call with rc 20, without a reply or wait. */
    if (argc == 2 && (!strcmp(argv[1], "procure") || !strcmp(argv[1], "vacate")))
    {
        InitSemaphore(&semaphore);
        if (!strcmp(argv[1], "procure"))
            Procure(&semaphore, &semaphore_message);
        else
            Vacate(&semaphore, &semaphore_message);
        check(FALSE, "unsupported asynchronous semaphore call unexpectedly returned");
        return test_done("exec_memory");
    }

    /* MEMF_CLEAR of memory that nothing has used yet */
    p = AllocMem(4096, MEMF_PUBLIC | MEMF_CLEAR);
    check(p && all_bytes(p, 4096, 0), "AllocMem(MEMF_CLEAR) of memory not used before is zero");
    if (p)
        FreeMem(p, 4096);

    /* Blocks of changing sizes stay intact while others come and go */
    ok = TRUE;
    for (i = 0; i < 200 && ok; i++)
    {
        j = i % SLOTS;
        if (slot[j])
        {
            ok = all_bytes(slot[j], size[j], (UBYTE)j);
            FreeMem(slot[j], size[j]);
        }
        size[j] = slot_size(i);
        slot[j] = AllocMem(size[j], MEMF_PUBLIC);
        if (!slot[j])
            ok = FALSE;
        else
            memset(slot[j], j, size[j]);
    }
    check(ok, "200 AllocMem()/FreeMem() rounds keep the live blocks intact");
    for (j = 0; j < SLOTS; j++)
    {
        if (slot[j])
            FreeMem(slot[j], size[j]);
        slot[j] = NULL;
    }

    ok = TRUE;
    for (i = 0; i < 200 && ok; i++)
    {
        j = i % SLOTS;
        if (slot[j])
        {
            ok = all_bytes(slot[j], size[j], (UBYTE)(j + 1));
            FreeVec(slot[j]);
        }
        size[j] = slot_size(i + 7);
        slot[j] = AllocVec(size[j], MEMF_ANY);
        if (!slot[j])
            ok = FALSE;
        else
            memset(slot[j], j + 1, size[j]);
    }
    check(ok, "200 AllocVec()/FreeVec() rounds keep the live blocks intact");
    for (j = 0; j < SLOTS; j++)
    {
        FreeVec(slot[j]);
        slot[j] = NULL;
    }

    /* exec hands out memory in multiples of 8 bytes, aligned to 8 */
    misaligned = 0;
    for (i = 1; i <= 64; i++)
    {
        p = AllocMem(i, MEMF_ANY);
        if (p && ((ULONG)p & 7))
            misaligned++;
        if (p)
            FreeMem(p, i);
    }
    check_long("AllocMem() results of 1..64 bytes not aligned to 8", misaligned, 0);

    /* MEMF_CLEAR of memory that was used and freed */
    p = AllocMem(4096, MEMF_PUBLIC);
    if (p)
    {
        memset(p, 0xa5, 4096);
        FreeMem(p, 4096);
    }
    q = AllocMem(4096, MEMF_PUBLIC | MEMF_CLEAR);
    check(q && all_bytes(q, 4096, 0), "AllocMem(MEMF_CLEAR) of reused memory is zero");
    Printf("     first block 0x%08lx, second block 0x%08lx\n", (LONG)p, (LONG)q);
    if (q)
        FreeMem(q, 4096);

    p = AllocVec(2048, MEMF_ANY);
    if (p)
    {
        memset(p, 0x5a, 2048);
        FreeVec(p);
    }
    q = AllocVec(2048, MEMF_ANY | MEMF_CLEAR);
    check(q && all_bytes(q, 2048, 0), "AllocVec(MEMF_CLEAR) of reused memory is zero");
    FreeVec(q);

    /* AvailMem() follows the allocations */
    before = AvailMem(MEMF_ANY);
    total = AvailMem(MEMF_TOTAL);
    p = AllocMem(1024 * 1024, MEMF_ANY);
    after = AvailMem(MEMF_ANY);
    Printf("     AvailMem(MEMF_ANY): %ld before, %ld after allocating 1 MB\n", before, after);
    check(before > 0, "AvailMem(MEMF_ANY) is not zero");
    check(p != NULL && before - after >= 1024 * 1024, "AvailMem(MEMF_ANY) drops by 1 MB after AllocMem() of 1 MB");
    check(total >= before && AvailMem(MEMF_TOTAL) == total,
          "AvailMem(MEMF_TOTAL) includes allocated memory");
    if (p)
        FreeMem(p, 1024 * 1024);
    check(AvailMem(MEMF_ANY) == before, "FreeMem() restores AvailMem(MEMF_ANY)");
    check(AvailMem(MEMF_LARGEST) > 0, "AvailMem(MEMF_LARGEST) is not zero");
    check(AvailMem(MEMF_LARGEST | MEMF_TOTAL) == AvailMem(MEMF_LARGEST),
          "MEMF_LARGEST takes precedence over MEMF_TOTAL");

    /* CopyMem() and CopyMemQuick() */
    p = AllocVec(256, MEMF_ANY);
    q = AllocVec(256, MEMF_ANY | MEMF_CLEAR);
    if (check(p && q, "AllocVec() of two 256 byte buffers"))
    {
        for (i = 0; i < 256; i++)
            p[i] = (UBYTE)i;
        CopyMem(p + 1, q + 3, 200);
        check(memcmp(p + 1, q + 3, 200) == 0, "CopyMem() of 200 bytes at odd addresses");
        memset(q, 0, 256);
        CopyMemQuick(p, q, 128);
        check(memcmp(p, q, 128) == 0, "CopyMemQuick() of 128 bytes");
        check(TypeOfMem(p) != 0, "TypeOfMem() of an AllocVec() block is not zero");
    }
    FreeVec(p);
    FreeVec(q);

    /* Memory pools */
    before = AvailMem(MEMF_ANY);
    DeletePool(NULL);
    check(AvailMem(MEMF_ANY) == before, "DeletePool(NULL) leaves the heap unchanged");
    pool = CreatePool(MEMF_ANY | MEMF_CLEAR, 4096, 1024);
    if (check(pool != NULL, "CreatePool(MEMF_CLEAR)"))
    {
        ok = TRUE;
        for (j = 0; j < SLOTS; j++)
        {
            slot[j] = AllocPooled(pool, 100 + j * 10);
            if (!slot[j])
                ok = FALSE;
            else
                memset(slot[j], j + 2, 100 + j * 10);
        }
        for (j = 0; j < SLOTS && ok; j++)
            ok = all_bytes(slot[j], 100 + j * 10, (UBYTE)(j + 2));
        check(ok, "AllocPooled() blocks do not overlap");
        FreePooled(pool, slot[0], 100);
        p = AllocPooled(pool, 100);
        check(p && all_bytes(p, 100, 0), "AllocPooled() from a MEMF_CLEAR pool is zero after FreePooled()");
        q = AllocPooled(pool, 8192);
        check(q && all_bytes(q, 8192, 0), "AllocPooled(MEMF_CLEAR) above puddle and threshold sizes is zero");
        DeletePool(pool);
        check(AvailMem(MEMF_ANY) == before,
              "DeletePool() restores AvailMem() with allocations still outstanding");
    }

    pool = CreatePool(MEMF_ANY, 4096, 1024);
    if (check(pool != NULL, "CreatePool() without MEMF_CLEAR"))
    {
        p = AllocPooled(pool, 37);
        q = AllocPooled(pool, 73);
        if (p)
            memset(p, 0x6a, 37);
        if (q)
            memset(q, 0x6b, 73);
        check(p && q && all_bytes(p, 37, 0x6a) && all_bytes(q, 73, 0x6b),
              "AllocPooled() without MEMF_CLEAR keeps live blocks intact");
        FreePooled(pool, p, 37);
        DeletePool(pool);
        check(AvailMem(MEMF_ANY) == before,
              "DeletePool() without MEMF_CLEAR returns its header and all blocks");
    }

    custom_window_checks(before);
    semaphore_checks();

    /* Signals of the running task */
    sig = AllocSignal(-1);
    check(sig >= 16 && sig < 32, "AllocSignal(-1) returns a user signal");
    if (sig >= 0)
    {
        SetSignal(1L << sig, 1L << sig);
        check((SetSignal(0, 1L << sig) & (1L << sig)) != 0, "SetSignal() sets and clears the signal");
        FreeSignal(sig);
    }

    /* FindTask(NULL) names the same task every time */
    me = FindTask(NULL);
    check(me != NULL, "FindTask(NULL) is not NULL");
    if (me)
        check_long("FindTask(NULL) ln_Type", me->tc_Node.ln_Type, NT_PROCESS);
    again = FindTask(NULL);
    check_long("a second FindTask(NULL) returns the same task", (LONG)again, (LONG)me);
    check_long("SysBase->ThisTask is FindTask(NULL)", (LONG)SysBase->ThisTask, (LONG)me);

    return test_done("exec_memory");
}
