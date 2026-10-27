/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    Host-side test for kaffeine core — no AROS required.
    Uses stub headers in stubs/ directory.
*/
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

/* Stub thunk tables */
const struct M68KThunkEntry kaffeine_thunks_exec[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_exec_count = 0;
const struct M68KThunkEntry kaffeine_thunks_dos[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_dos_count = 0;

extern "C" {
ULONG Kaffeine_GetD(void *, int) { return 0; }
ULONG Kaffeine_GetA(void *, int) { return 0; }
void  Kaffeine_SetD(void *, int, ULONG) {}
void  Kaffeine_SetA(void *, int, ULONG) {}
ULONG Kaffeine_OpenLibrary(struct KaffeineContext *, const char *) { return 0; }
ULONG Kaffeine_OpenM68KLibrary(struct KaffeineContext *, void *, const char *, ULONG) { return 0; }
BOOL Kaffeine_CloseM68KLibrary(struct KaffeineContext *, void *, ULONG) { return 0; }
}

/* Include memory manager source */
#include "kaffeine_memory.c"

#include <cstdio>
#include <vector>
static int pass = 0, fail = 0;
#define TEST(n) printf("  %-40s ", n)
#define OK() do { printf("PASS\n"); pass++; } while(0)
#define NG(m) do { printf("FAIL: %s\n", m); fail++; return; } while(0)
#define CHK(c,m) do { if(!(c)) NG(m); } while(0)

void t_ctx() {
    TEST("Context create/destroy");
    struct KaffeineLibBase b = {};
    auto *c = Kaffeine_CreateContext(&b);
    CHK(c && c->mem, "null"); CHK(c->mem_size == KAFFEINE_MEM_SIZE, "size");
    Kaffeine_DestroyContext(c); OK();
}
void t_heap() {
    TEST("Heap alloc/free/reuse");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    ULONG a = Kaffeine_HeapAlloc(c, 100, 0); CHK(a, "a1");
    ULONG a2 = Kaffeine_HeapAlloc(c, 200, 0); CHK(a2 && a2!=a, "a2");
    Kaffeine_HeapFree(c, a, 100);
    ULONG a3 = Kaffeine_HeapAlloc(c, 64, 0); CHK(a3, "a3");
    Kaffeine_DestroyContext(c); OK();
}
void t_stress() {
    TEST("Heap stress 1000 alloc/free");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    ULONG addrs[1000];
    for(int i=0;i<1000;i++) { addrs[i]=Kaffeine_HeapAlloc(c,64,0); CHK(addrs[i],"oom"); }
    for(int i=0;i<1000;i++) Kaffeine_HeapFree(c,addrs[i],64);
    CHK(Kaffeine_HeapAlloc(c,100000,0), "post-free"); Kaffeine_DestroyContext(c); OK();
}
void t_heap_alignment() {
    TEST("Heap 8-byte alignment and mixed-size reuse");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    CHK(c, "context");
    ULONG addrs[64];
    for (unsigned i = 0; i < 64; i++) {
        addrs[i] = Kaffeine_HeapAlloc(c, i + 1, 0);
        CHK(addrs[i] && (addrs[i] & 7) == 0, "unaligned allocation");
        memset(c->mem + addrs[i], i + 1, i + 1);
    }
    for (unsigned i = 0; i < 64; i += 2)
        Kaffeine_HeapFree(c, addrs[i], i + 1);
    for (unsigned i = 0; i < 64; i += 2) {
        addrs[i] = Kaffeine_HeapAlloc(c, i + 1, 0);
        CHK(addrs[i] && (addrs[i] & 7) == 0, "unaligned reused block");
    }
    for (unsigned i = 1; i < 64; i += 2)
        for (unsigned j = 0; j <= i; j++)
            CHK(c->mem[addrs[i] + j] == i + 1, "live payload overwritten");
    for (unsigned i = 64; i-- > 0;)
        Kaffeine_HeapFree(c, addrs[i], i + 1);
    /* Fully coalesced: one free interval on each side of the custom chips. */
    const ULONG above = CUSTOM_BASE + CUSTOM_SIZE;
    CHK(c->heap_free == c->heap_start, "heap head not restored");
    CHK(m68k_read32(c, c->heap_start) == CUSTOM_BASE - c->heap_start,
        "heap below the custom chips not fully coalesced");
    CHK(m68k_read32(c, c->heap_start + 4) == above, "extra free chunks below the custom chips");
    CHK(m68k_read32(c, above) == c->heap_end - above,
        "heap above the custom chips not fully coalesced");
    CHK(m68k_read32(c, above + 4) == 0, "extra free chunks above the custom chips");
    Kaffeine_DestroyContext(c); OK();
}
void t_heap_overflow() {
    TEST("Heap rejects size/header overflow");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    CHK(c, "context");
    const ULONG free_head = c->heap_free;
    const ULONG free_size = m68k_read32(c, free_head);
    const ULONG free_next = m68k_read32(c, free_head + 4);
    const ULONG sizes[] = { 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFF9u,
                            0xFFFFFFF8u, 0xFFFFFFF1u, 0xFFFFFFF0u };
    for (ULONG size : sizes) {
        CHK(Kaffeine_HeapAlloc(c, size, 0) == 0, "oversized allocation accepted");
        CHK(c->heap_free == free_head && m68k_read32(c, free_head) == free_size &&
            m68k_read32(c, free_head + 4) == free_next, "failed allocation changed heap");
    }
    Kaffeine_DestroyContext(c); OK();
}
void t_custom_window() {
    TEST("Heap keeps out of the custom chips");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    CHK(c, "context");
    const ULONG above = CUSTOM_BASE + CUSTOM_SIZE, start = c->heap_start, end = c->heap_end;
    const ULONG pristine = Kaffeine_HeapAvail(c, 0);
    CHK(pristine == CUSTOM_BASE - start - KAFFEINE_HEAP_HDR + end - above - KAFFEINE_HEAP_HDR,
        "not one free interval on each side");
    CHK(Kaffeine_HeapAvail(c, MEMF_TOTAL) == pristine, "MEMF_TOTAL counts the window");
    CHK(Kaffeine_HeapAvail(c, MEMF_LARGEST) == end - above - KAFFEINE_HEAP_HDR, "largest");
    /* No absolute allocation may touch the window. */
    const ULONG refused[][2] = {
        { CUSTOM_BASE, 8 }, { CUSTOM_BASE + 0x16, 2 }, { above - 8, 8 }, { CUSTOM_BASE - 8, 16 },
        { above - 8, 16 }, { CUSTOM_BASE - CUSTOM_SIZE, 3 * CUSTOM_SIZE } };
    for (const auto &r : refused)
        CHK(Kaffeine_HeapAllocAbs(c, r[1], r[0]) == 0, "absolute allocation in the window");
    CHK(Kaffeine_HeapAvail(c, 0) == pristine, "a refusal changed the heap");
    ULONG below = Kaffeine_HeapAllocAbs(c, 8, CUSTOM_BASE - 8), after = Kaffeine_HeapAllocAbs(c, 8, above);
    CHK(below == CUSTOM_BASE - 8 && after == above, "next to the window");
    Kaffeine_HeapFree(c, below, 8);
    Kaffeine_HeapFree(c, after, 8);
    /* FreeMem() cannot release the reserved interval. */
    Kaffeine_HeapFree(c, CUSTOM_BASE, CUSTOM_SIZE);
    Kaffeine_HeapFree(c, CUSTOM_BASE + KAFFEINE_HEAP_HDR, CUSTOM_SIZE);
    CHK(Kaffeine_HeapAvail(c, 0) == pristine, "heap not restored");
    /* Exhaust the heap: no block may overlap the window. */
    std::vector<ULONG> blocks;
    for (ULONG size = 1 << 20; size >= 8; size >>= 1)
        for (ULONG a; (a = Kaffeine_HeapAlloc(c, size, 0)) != 0; blocks.push_back(a)) {
            auto *node = heap_find(c, a);
            CHK(node && (node->block + node->size <= CUSTOM_BASE || node->block >= above),
                "block in the window");
        }
    CHK(blocks.size() > 30 && Kaffeine_HeapAvail(c, 0) < 8, "heap not exhausted");
    for (ULONG a : blocks) Kaffeine_HeapFree(c, a, 0);
    CHK(Kaffeine_HeapAvail(c, 0) == pristine, "heap not restored after exhaustion");
    /* Resizing keeps the window out exactly when the bounds include it. */
    c->heap_end = CUSTOM_BASE;
    CHK(Kaffeine_HeapInit(c) && Kaffeine_HeapAvail(c, MEMF_TOTAL) == CUSTOM_BASE - start - KAFFEINE_HEAP_HDR &&
        m68k_read32(c, start + 4) == 0, "heap ending at the window");
    c->heap_start = CUSTOM_BASE + 0x800; c->heap_end = end;
    CHK(Kaffeine_HeapInit(c) && c->heap_free == above &&
        Kaffeine_HeapAvail(c, MEMF_TOTAL) == end - above - KAFFEINE_HEAP_HDR &&
        Kaffeine_HeapAllocAbs(c, 8, CUSTOM_BASE + 0x800) == 0, "heap starting in the window");
    c->heap_start = CUSTOM_BASE; c->heap_end = above;
    CHK(Kaffeine_HeapInit(c) && c->heap_free == 0 && Kaffeine_HeapAvail(c, MEMF_TOTAL) == 0 &&
        Kaffeine_HeapAlloc(c, 8, 0) == 0, "heap inside the window");
    c->heap_start = start;
    CHK(Kaffeine_HeapInit(c) && Kaffeine_HeapAvail(c, 0) == CUSTOM_BASE - start - KAFFEINE_HEAP_HDR,
        "heap ending inside the window");
    c->heap_end = end;
    CHK(Kaffeine_HeapInit(c) && Kaffeine_HeapAvail(c, 0) == pristine, "original bounds");
    ULONG live = Kaffeine_HeapAlloc(c, 64, 0);
    CHK(live && !Kaffeine_HeapInit(c), "a live allocation allows a reset");
    Kaffeine_HeapFree(c, live, 64);
    Kaffeine_DestroyContext(c); OK();
}
void t_endian() {
    TEST("Big-endian read/write");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    m68k_write16(c,0x1000,0xCAFE); CHK(m68k_read16(c,0x1000)==0xCAFE,"16");
    CHK(c->mem[0x1000]==0xCA,"hi");
    m68k_write32(c,0x2000,0xDEADBEEF); CHK(m68k_read32(c,0x2000)==0xDEADBEEF,"32");
    Kaffeine_DestroyContext(c); OK();
}
void t_xlat() {
    TEST("Pointer translation");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    CHK(m68k_to_host(c,0)==NULL,"0→null"); CHK(host_to_m68k(c,NULL)==0,"null→0");
    CHK(m68k_to_host(c,0x1000)==c->mem+0x1000,"fwd");
    CHK(host_to_m68k(c,c->mem+0x2000)==0x2000,"rev");
    Kaffeine_DestroyContext(c); OK();
}
void t_libbase() {
    TEST("Fake lib base + A-line traps");
    struct KaffeineLibBase b = {}; auto *c = Kaffeine_CreateContext(&b);
    ULONG a = Kaffeine_SetupLibBase(c,0,"test.library",10,kaffeine_thunks_exec,0,NULL,0);
    CHK(a,"addr"); CHK(c->num_libs==1,"nlibs");
    UWORD op = m68k_read16(c, a-6);
    CHK((op&0xF000)==0xA000,"aline");
    Kaffeine_DestroyContext(c); OK();
}

int main() {
    printf("kaffeine host-side tests\n=======================\n");
    t_ctx(); t_heap(); t_stress(); t_heap_alignment(); t_heap_overflow(); t_custom_window();
    t_endian(); t_xlat(); t_libbase();
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
