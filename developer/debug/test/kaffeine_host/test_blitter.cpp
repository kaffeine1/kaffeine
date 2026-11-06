/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The area blitter of kaffeine_blitter.h through its register interface,
    over a plain 16 MB arena that also holds the custom chip window. Every
    expected word is computed by hand from the Hardware Reference Manual
    rules, or by an oracle independent of the code under test: for the
    minterms, A = $F0F0, B = $CCCC and C = $AAAA give LF * $0101.
*/

#include <cstdio>
#include <cstring>
#include <vector>

#include "kaffeine_blitter.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const ULONG ARENA = 16u << 20;

struct Arena
{
    std::vector<UBYTE> mem;
    KaffeineContext *ctx;
    KaffeineCPU cpu;
    std::vector<std::pair<ULONG, ULONG>> flushes;

    Arena() : mem(ARENA + KAFFEINE_MEM_GUARD, 0xA5), ctx(new KaffeineContext())
    {
        ctx->mem = mem.data();
        ctx->mem_size = ARENA;
        Kaffeine_BlitterInit(&ctx->blitter);
        cpu.ops = &ops;
        cpu.backend_state = this;
        ctx->cpu = &cpu;
    }
    ~Arena() { delete ctx; }
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    static void flush(void *state, ULONG addr, ULONG size)
    { static_cast<Arena *>(state)->flushes.push_back({ addr, size }); }
    static const KaffeineCPUOps ops;

    int w(UWORD reg, UWORD val) { return Kaffeine_BlitterWrite(ctx, reg, val); }
    void ptr(UWORD reg, ULONG addr) { w(reg, (UWORD)(addr >> 16)); w((UWORD)(reg + 2), (UWORD)addr); }
    void words(ULONG at, std::initializer_list<UWORD> list)
    { for (UWORD v : list) { m68k_write16(ctx, at, v); at += 2; } }
    void fill(ULONG at, ULONG count, UWORD v) { while (count--) { m68k_write16(ctx, at, v); at += 2; } }
    bool equal(ULONG at, std::initializer_list<UWORD> list)
    {
        for (UWORD v : list) { if (m68k_read16(ctx, at) != v) return false; at += 2; }
        return true;
    }
    UWORD dmaconr() { return Kaffeine_BlitterDmaconr(ctx); }
    /* the usual set-up: masks all ones, no shifts, every modulo 0 */
    void reset(UWORD con0, UWORD con1 = 0)
    {
        w(0x040, con0); w(0x042, con1); w(0x044, 0xFFFF); w(0x046, 0xFFFF);
        w(0x060, 0); w(0x062, 0); w(0x064, 0); w(0x066, 0);
    }
};

const KaffeineCPUOps Arena::ops = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, Arena::flush };

static UWORD size(ULONG w, ULONG h) { return (UWORD)((h << 6) | (w & 63)); }

/* All 256 minterms, with constant channels and with fetched ones */
static void minterms()
{
    Arena t;
    for (unsigned lf = 0; lf < 256; lf++)
    {
        t.reset((UWORD)(KAFFEINE_BLT_USED | lf));
        t.w(0x074, 0xF0F0); t.w(0x072, 0xCCCC); t.w(0x070, 0xAAAA);
        t.ptr(0x054, 0x1000);
        CHECK(t.w(0x058, size(1, 1)) == 0);
        CHECK(m68k_read16(t.ctx, 0x1000) == (UWORD)(lf * 0x0101));
    }
    t.words(0x2000, { 0xF0F0 }); t.words(0x2002, { 0xCCCC }); t.words(0x2004, { 0xAAAA });
    for (unsigned lf = 0; lf < 256; lf += 17)
    {
        t.reset((UWORD)(0x0F00 | lf));
        t.ptr(0x050, 0x2000); t.ptr(0x04C, 0x2002); t.ptr(0x048, 0x2004); t.ptr(0x054, 0x1100);
        CHECK(t.w(0x058, size(1, 1)) == 0);
        CHECK(m68k_read16(t.ctx, 0x1100) == (UWORD)(lf * 0x0101));
    }
}

/* HRM chapter 6, the 3 by 2 copy with a shift of 4: zeros only into the
   first word of the blit, the carry across rows, masks before the shift */
static void a_shift_ascending()
{
    Arena t;
    t.words(0x2000, { 0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x1357, 0x2468 });
    t.reset(0x49F0);                    /* ASH 4, USEA, USED, D = A */
    t.w(0x044, 0x0FFF); t.w(0x046, 0xFF00);
    t.ptr(0x050, 0x2000); t.ptr(0x054, 0x3000);
    t.flushes.clear();
    CHECK(t.w(0x058, size(3, 2)) == 0);
    CHECK(t.equal(0x3000, { 0x0023, 0x4567, 0x89A0, 0x00EF, 0x0135, 0x7240 }));
    CHECK(t.ctx->blitter.apt == 0x200C && t.ctx->blitter.dpt == 0x300C);
    CHECK(t.ctx->blitter.adat == 0x2468);
    CHECK(t.flushes.size() == 1 && t.flushes[0].first == 0x3000 && t.flushes[0].second == 12);
    CHECK(!(t.dmaconr() & KAFFEINE_DMACON_BZERO));
}

/* Descending: pointers from the last word, left shifts, FWM on the first
   word fetched; the carry runs from the lower row into the upper one */
static void a_shift_descending()
{
    Arena t;
    t.words(0x2000, { 0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x1357, 0x2468 });
    t.reset(0x49F0, KAFFEINE_BLT_DESC);
    t.ptr(0x050, 0x200A); t.ptr(0x054, 0x300A);
    CHECK(t.w(0x058, size(3, 2)) == 0);
    CHECK(t.equal(0x3000, { 0x2345, 0x6789, 0xABCD, 0xEF01, 0x3572, 0x4680 }));
    CHECK(t.ctx->blitter.apt == 0x1FFE && t.ctx->blitter.dpt == 0x2FFE);
    /* masks: FWM masks the rightmost (first fetched) word */
    t.words(0x2100, { 0xFFFF, 0xFFFF });
    t.reset(0x09F0, KAFFEINE_BLT_DESC);
    t.w(0x044, 0x00FF); t.w(0x046, 0xF000);
    t.ptr(0x050, 0x2102); t.ptr(0x054, 0x3102);
    CHECK(t.w(0x058, size(2, 1)) == 0);
    CHECK(t.equal(0x3100, { 0xF000, 0x00FF }));
}

/* B shifts like A but without masks */
static void b_shift()
{
    Arena t;
    t.words(0x2000, { 0x8001, 0x0000 });
    t.reset(0x05CC, 0x1000);            /* USEB, USED, D = B, BSH 1 */
    t.w(0x044, 0); t.w(0x046, 0);
    t.ptr(0x04C, 0x2000); t.ptr(0x054, 0x3000);
    CHECK(t.w(0x058, size(2, 1)) == 0);
    CHECK(t.equal(0x3000, { 0x4000, 0x8000 }));
    CHECK(t.ctx->blitter.bdat == 0x0000 && t.ctx->blitter.bhold == 0x8000);
}

/* HRM: a BLTBDAT write shifts the data once with the BSH of that moment;
   with B off that held value is used for every word, even after BSH changes */
static void b_held()
{
    Arena t;
    t.reset(0x01CC, 0x4000);            /* USED only, D = B, BSH 4 */
    t.w(0x072, 0x1234);
    t.w(0x042, 0x8000);                 /* BSH 8 after the write */
    t.ptr(0x054, 0x3000);
    CHECK(t.w(0x058, size(3, 1)) == 0);
    CHECK(t.equal(0x3000, { 0x0123, 0x0123, 0x0123 }));
    t.w(0x042, 0x4000 | KAFFEINE_BLT_DESC);
    t.w(0x072, 0x1234);                 /* descending: shifted left */
    CHECK(t.ctx->blitter.bhold == 0x2340);
}

/* A off: the constant is masked per word; cookie-cut LF $CA (HRM, as PPaint) */
static void cookie_cut()
{
    Arena t;
    t.words(0x4000, { 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF });
    t.fill(0x5000, 6, 0x1111);
    t.reset(0x07CA);                    /* USEB, USEC, USED */
    t.w(0x074, 0xFFFF); t.w(0x044, 0x0FFF); t.w(0x046, 0xFF00);
    t.ptr(0x04C, 0x4000); t.ptr(0x048, 0x5000); t.ptr(0x054, 0x5000);
    t.ptr(0x050, 0x00ABCDEE);           /* A is off: never read, not moved */
    CHECK(t.w(0x058, size(3, 2)) == 0);
    CHECK(t.equal(0x5000, { 0x1AAA, 0xBBBB, 0xCC11, 0x1DDD, 0xEEEE, 0xFF11 }));
    CHECK(t.ctx->blitter.apt == 0x00ABCDEE);
    /* C is fetched before the late D write lands, so every C word read was 0x1111 */
    CHECK(t.ctx->blitter.cdat == 0x1111 && t.ctx->blitter.bdat == 0xFFFF);
}

/* PPaint's canvas clear: C and D only, A = $FFFF, B = 0, so D = 0 */
static void clear_and_bzero()
{
    Arena t;
    t.fill(0x6000, 64 * 3, 0x5A5A);
    t.reset(0x03CA);
    t.w(0x074, 0xFFFF); t.w(0x072, 0x0000);
    t.ptr(0x048, 0x6000); t.ptr(0x054, 0x6000);
    t.ptr(0x04C, 0x00C682C8); t.w(0x062, (UWORD)-70);
    CHECK(t.w(0x05C, 3) == 0);
    CHECK(t.w(0x05E, 64) == 0);         /* BLTSIZH starts it */
    bool cleared = true;
    for (ULONG i = 0; i < 64 * 3; i++)
        cleared = cleared && m68k_read16(t.ctx, 0x6000 + 2 * i) == 0;
    CHECK(cleared && m68k_read16(t.ctx, 0x6000 + 2 * 64 * 3) == 0xA5A5);
    CHECK(t.dmaconr() & KAFFEINE_DMACON_BZERO);
    CHECK(t.ctx->blitter.cpt == 0x6180 && t.ctx->blitter.dpt == 0x6180);
    CHECK(t.ctx->blitter.bpt == 0x00C682C8);
    /* BZERO with D off: A AND B */
    t.words(0x7000, { 0xF000 }); t.words(0x7002, { 0x0F00 });
    t.reset(0x0C80);
    t.ptr(0x050, 0x7000); t.ptr(0x04C, 0x7002);
    t.flushes.clear();
    CHECK(t.w(0x058, size(1, 1)) == 0);
    CHECK((t.dmaconr() & KAFFEINE_DMACON_BZERO) && t.flushes.empty());
    t.words(0x7002, { 0x1F00 });
    t.ptr(0x050, 0x7000); t.ptr(0x04C, 0x7002);
    CHECK(t.w(0x058, size(1, 1)) == 0);
    CHECK(!(t.dmaconr() & KAFFEINE_DMACON_BZERO));
}

/* A negative modulo: rows walk backwards */
static void negative_modulo()
{
    Arena t;
    t.words(0x7000, { 1, 2, 3, 4, 5, 6 });
    t.reset(0x09F0);
    t.w(0x066, (UWORD)-8);
    t.ptr(0x050, 0x7000); t.ptr(0x054, 0x7110);
    CHECK(t.w(0x058, size(2, 3)) == 0);
    CHECK(t.equal(0x7106, { 0xA5A5, 5, 6, 3, 4, 1, 2, 0xA5A5 }));
}

/* Overlap: the one-word D delay of the hardware, and descending memmove */
static void overlap()
{
    Arena t;
    t.words(0x8000, { 1, 2, 3, 4, 5, 6, 7, 8 });
    t.reset(0x09F0);
    t.ptr(0x050, 0x8000); t.ptr(0x054, 0x8004);
    CHECK(t.w(0x058, size(6, 1)) == 0);
    CHECK(t.equal(0x8000, { 1, 2, 1, 2, 1, 2, 1, 2 }));
    t.words(0x8000, { 1, 2, 3, 4, 5, 6, 7, 8 });
    t.reset(0x09F0, KAFFEINE_BLT_DESC);
    t.ptr(0x050, 0x800A); t.ptr(0x054, 0x800E);
    CHECK(t.w(0x058, size(6, 1)) == 0);
    CHECK(t.equal(0x8000, { 1, 2, 1, 2, 3, 4, 5, 6 }));
}

/* DMACON: initial enables, a blit waiting with BBUSY, release on enable */
static void dma_enable()
{
    Arena t;
    CHECK(t.dmaconr() == (KAFFEINE_DMACON_DMAEN | KAFFEINE_DMACON_BLTEN));
    t.w(0x096, KAFFEINE_DMACON_BLTEN);  /* clear BLTEN */
    CHECK(t.dmaconr() == KAFFEINE_DMACON_DMAEN);
    t.reset(0x01F0);                    /* D = A, A off */
    t.w(0x074, 0x7777);
    t.ptr(0x054, 0x9000);
    CHECK(t.w(0x058, size(1, 1)) == 0);
    CHECK(m68k_read16(t.ctx, 0x9000) == 0xA5A5 && (t.dmaconr() & KAFFEINE_DMACON_BBUSY));
    CHECK(t.w(0x096, 0x8400) == 0);     /* BLTPRI only */
    CHECK(m68k_read16(t.ctx, 0x9000) == 0xA5A5 && (t.dmaconr() & KAFFEINE_DMACON_BBUSY));
    CHECK(t.w(0x096, 0x8040) == 0);     /* BLTEN: the waiting blit runs */
    CHECK(m68k_read16(t.ctx, 0x9000) == 0x7777);
    CHECK(t.dmaconr() == 0x0640);
    /* a context without Kaffeine_BlitterInit() has DMA off */
    KaffeineContext *bare = new KaffeineContext();
    bare->mem = t.mem.data(); bare->mem_size = ARENA;
    Kaffeine_BlitterWrite(bare, 0x040, 0x01F0);
    Kaffeine_BlitterWrite(bare, 0x074, 0x1111);
    Kaffeine_BlitterWrite(bare, 0x054, 0); Kaffeine_BlitterWrite(bare, 0x056, 0x9002);
    CHECK(Kaffeine_BlitterWrite(bare, 0x058, size(1, 1)) == 0);
    CHECK(m68k_read16(bare, 0x9002) == 0xA5A5 && (Kaffeine_BlitterDmaconr(bare) & KAFFEINE_DMACON_BBUSY));
    delete bare;
}

/* Refusals: nothing read or written, no flush, pointers and BZERO kept */
static void refusals()
{
    Arena t;
    std::vector<UBYTE> before;
    auto snapshot = [&]() { before.assign(t.mem.begin(), t.mem.begin() + ARENA); t.flushes.clear(); };
    auto untouched = [&]() { return std::memcmp(before.data(), t.mem.data(), ARENA) == 0 && t.flushes.empty(); };

    t.reset(0x01F0); t.w(0x074, 0x1234);
    t.ptr(0x054, ARENA - 2);
    snapshot();
    CHECK(t.w(0x058, size(2, 1)) == 20 && untouched());
    CHECK(t.ctx->blitter.dpt == ARENA - 2);
    t.ptr(0x054, CUSTOM_BASE + 0x100);
    CHECK(t.w(0x058, size(1, 1)) == 20 && untouched());
    t.ptr(0x054, CUSTOM_BASE - 2);      /* the last word below the window is fine */
    CHECK(t.w(0x058, size(1, 1)) == 0 && m68k_read16(t.ctx, CUSTOM_BASE - 2) == 0x1234);
    snapshot();
    t.reset(0x09F0); t.ptr(0x050, 0xFFFFFFFE); t.ptr(0x054, 0x3000);
    CHECK(t.w(0x058, size(1, 1)) == 20 && untouched());
    /* work bound: 2048 by 32768 words in one row through a negative modulo */
    t.reset(0x01F0); t.w(0x066, (UWORD)-4096); t.ptr(0x054, 0x100000);
    t.w(0x05C, 0);
    CHECK(t.w(0x05E, 0) == 20 && untouched());
    /* line mode and area fill are not implemented */
    t.reset(0x0BCA, KAFFEINE_BLT_LINE); t.ptr(0x054, 0x3000);
    CHECK(t.w(0x058, size(2, 4)) == 20 && untouched());
    t.reset(0x09F0, KAFFEINE_BLT_DESC | 0x0008);
    CHECK(t.w(0x058, size(1, 1)) == 20 && untouched());
    t.reset(0x09F0, KAFFEINE_BLT_DESC | 0x0010);
    CHECK(t.w(0x058, size(1, 1)) == 20 && untouched());
    /* a refused waiting blit is not kept */
    t.w(0x096, KAFFEINE_DMACON_BLTEN);
    t.reset(0x01F0); t.ptr(0x054, ARENA);
    CHECK(t.w(0x058, size(1, 1)) == 0);
    CHECK(t.w(0x096, 0x8040) == 20 && untouched() && !(t.dmaconr() & KAFFEINE_DMACON_BBUSY));
}

/* Size encodings, BLTSIZV/H and BLTCON0L */
static void sizes()
{
    Arena t;
    t.reset(0x01F0); t.w(0x074, 0xFFFF); t.ptr(0x054, 0x100000);
    CHECK(t.w(0x058, 0) == 0);          /* 64 words by 1024 rows */
    CHECK(m68k_read16(t.ctx, 0x100000) == 0xFFFF &&
          m68k_read16(t.ctx, 0x100000 + 64 * 1024 * 2 - 2) == 0xFFFF &&
          m68k_read16(t.ctx, 0x100000 + 64 * 1024 * 2) == 0xA5A5);
    t.ptr(0x054, 0x200000);
    t.w(0x05C, 2);
    CHECK(t.w(0x05E, 3) == 0);
    CHECK(m68k_read16(t.ctx, 0x200000 + 10) == 0xFFFF && m68k_read16(t.ctx, 0x200000 + 12) == 0xA5A5);
    t.w(0x05A, 0x0F);                   /* LF $0F: D = NOT A */
    CHECK(t.ctx->blitter.con0 == 0x010F);
    t.w(0x074, 0x00FF); t.ptr(0x054, 0x3000);
    CHECK(t.w(0x058, size(1, 1)) == 0 && m68k_read16(t.ctx, 0x3000) == 0xFF00);
    /* other custom registers are not the blitter's */
    UWORD con0 = t.ctx->blitter.con0;
    CHECK(t.w(0x02C, 0x1234) == 0 && t.w(0x100, 0xFFFF) == 0 && t.ctx->blitter.con0 == con0);
}

int main()
{
    minterms();
    a_shift_ascending();
    a_shift_descending();
    b_shift();
    b_held();
    cookie_cut();
    clear_and_bzero();
    negative_modulo();
    overlap();
    dma_enable();
    refusals();
    sizes();
    std::printf("test_blitter: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
