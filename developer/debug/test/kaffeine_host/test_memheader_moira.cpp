/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    A guest program allocates and frees from a private MemHeader, using the
    production Exec thunks and Moira. This does not execute libnix startup.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>

#include "kaffeine_intern.h"
#include "kaffeine_memheader.h"

const M68KThunkEntry kaffeine_thunks_exec[] = { KAFFEINE_EXEC_MEMHEADER_THUNKS };
const ULONG kaffeine_thunks_exec_count =
    sizeof(kaffeine_thunks_exec) / sizeof(kaffeine_thunks_exec[0]);
const M68KThunkEntry kaffeine_thunks_dos[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_dos_count = 0;

extern "C" {
ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { return 0; }
ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { return 0; }
BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }
}

/* CPU, dispatcher and MemHeader thunks are separate C objects. */
#include "kaffeine_memory.c"
#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

int main()
{
    KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    assert(ctx);

    /* libnix places MemHeader and its pool immediately after AllocMem's
       result. Do not realign the pointer in this fixture. A preceding odd
       request exposes a heap that rounds allocations to only four bytes. */
    assert(Kaffeine_HeapAlloc(ctx, 1, 0));
    constexpr ULONG pool_size = 256;
    const ULONG header = Kaffeine_HeapAlloc(ctx, 32 + pool_size, 0);
    assert(header && (header & 7) == 0);
    const ULONG pool = header + 32;
    m68k_write32(ctx, header + 16, pool);
    m68k_write32(ctx, header + 20, pool);
    m68k_write32(ctx, header + 24, pool + pool_size);
    m68k_write32(ctx, header + 28, pool_size);
    m68k_write32(ctx, pool, 0);
    m68k_write32(ctx, pool + 4, pool_size);

    const ULONG execbase = Kaffeine_SetupLibBase(ctx, 0, "exec.library", 512,
        kaffeine_thunks_exec, kaffeine_thunks_exec_count, nullptr, 0);
    assert(execbase);
    const ULONG code = Kaffeine_HeapAlloc(ctx, 44, 0);
    assert(code);
    ULONG pc = code;
    auto word = [&](UWORD value) { m68k_write16(ctx, pc, value); pc += 2; };
    auto longword = [&](ULONG value) { m68k_write32(ctx, pc, value); pc += 4; };

    word(0x2C7C); longword(execbase); /* MOVEA.L #execbase,A6 */
    word(0x207C); longword(header);   /* MOVEA.L #header,A0 */
    word(0x700D);                    /* MOVEQ #13,D0 */
    word(0x4EAE); word(0xFF46);      /* JSR -186(A6): Allocate */
    word(0x2240);                    /* MOVEA.L D0,A1 */
    word(0x2600);                    /* MOVE.L D0,D3: save guest address */
    word(0x243C); longword(0x11223344); /* MOVE.L #marker,D2 */
    word(0x2342); word(0x0008);      /* MOVE.L D2,8(A1) */
    word(0x700D);                    /* MOVEQ #13,D0: original size */
    word(0x4EAE); word(0xFF40);      /* JSR -192(A6): Deallocate */
    word(0x2003);                    /* MOVE.L D3,D0 */
    word(0x4E75);                    /* RTS */
    assert(pc - code == 42);

    ctx->entry_point = code;
    ctx->m68k_sysbase = execbase;
    ctx->m68k_argptr = ctx->m68k_argsize = 0;
    const LONG result = Kaffeine_Execute(ctx);
    assert(static_cast<ULONG>(result) == pool);
    assert(m68k_read32(ctx, header + 16) == pool);
    assert(m68k_read32(ctx, header + 28) == pool_size);
    assert(m68k_read32(ctx, pool) == 0);
    assert(m68k_read32(ctx, pool + 4) == pool_size);
    assert(m68k_read32(ctx, pool + 8) == 0x11223344);
    const UBYTE marker[] = { 0x11, 0x22, 0x33, 0x44 };
    assert(std::memcmp(ctx->mem + pool + 8, marker, sizeof(marker)) == 0);

    Kaffeine_DestroyContext(ctx);
    std::puts("[PASS] Moira: private MemHeader allocation, guest write and deallocation via Exec LVOs");
    return 0;
}
