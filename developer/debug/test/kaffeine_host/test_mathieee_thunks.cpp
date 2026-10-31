/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 *
 * Call the production mathieeedoubbas.library, mathieeedoubtrans.library
 * and mathieeesingbas.library thunks through the real dispatcher with
 * literal LVOs, and check the result registers, condition codes (Z, N
 * and V with X, C and the system byte preserved), untouched registers
 * and the guest memory output of IEEEDPSincos().
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks_mathieee.h"

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *what, uint64_t a = 0, uint64_t b = 0)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::printf("FAIL: %s (0x%016llx, 0x%016llx)\n",
                    what, (unsigned long long)a, (unsigned long long)b);
    }
}

static uint64_t to_bits(double value)
{
    uint64_t bits;

    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double from_bits(uint64_t bits)
{
    double value;

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_to_bits(float value)
{
    uint32_t bits;

    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float bits_to_float(uint32_t bits)
{
    float value;

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0;
    ULONG pc = 0;
};

static ULONG get_d(void *state, int reg) { return static_cast<TestCPU *>(state)->d[reg]; }
static ULONG get_a(void *state, int reg) { return static_cast<TestCPU *>(state)->a[reg]; }
static void set_d(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->d[reg] = value; }
static void set_a(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->a[reg] = value; }
static UWORD get_sr(void *state) { return static_cast<TestCPU *>(state)->sr; }
static void set_sr(void *state, UWORD value) { static_cast<TestCPU *>(state)->sr = value; }
static ULONG get_pc(void *state) { return static_cast<TestCPU *>(state)->pc; }
static void set_pc(void *state, ULONG value) { static_cast<TestCPU *>(state)->pc = value; }
static void execute(void *) { std::abort(); }

static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

static unsigned poison_calls;

static IPTR poison_thunk(struct KaffeineContext *, void *)
{
    ++poison_calls;
    return 0xBAD0BEEFUL;
}

static const struct M68KThunkEntry poison_gen_table[] =
{
    {  30, poison_thunk },
    {  36, poison_thunk },
    {  42, poison_thunk },
    {  48, poison_thunk },
    {  54, poison_thunk },
    {  60, poison_thunk },
    {  66, poison_thunk },
    {  72, poison_thunk },
    {  78, poison_thunk },
    {  84, poison_thunk },
    {  90, poison_thunk },
    {  96, poison_thunk },
    { 102, poison_thunk },
    { 108, poison_thunk },
    { 114, poison_thunk },
    { 120, poison_thunk },
    { 126, poison_thunk },
    {   0, nullptr }
};

enum
{
    BAS_BASE   = 0x1000,
    TRANS_BASE = 0x2000,
    SING_BASE  = 0x3000,

    /* 68k CCR bits: N=0x08, Z=0x04, V=0x02 (X=0x10 and C=0x01 are preserved) */
    CCR_POS      = 0x0000u,
    CCR_V        = 0x0002u,
    CCR_Z        = 0x0004u,
    CCR_N        = 0x0008u,
    CCR_NZV_MASK = 0x000Eu,

    /* mathieeedoubbas.library LVOs */
    LVO_IEEEDPFIX   = 30,
    LVO_IEEEDPFLT   = 36,
    LVO_IEEEDPCMP   = 42,
    LVO_IEEEDPTST   = 48,
    LVO_IEEEDPABS   = 54,
    LVO_IEEEDPNEG   = 60,
    LVO_IEEEDPADD   = 66,
    LVO_IEEEDPSUB   = 72,
    LVO_IEEEDPMUL   = 78,
    LVO_IEEEDPDIV   = 84,
    LVO_IEEEDPFLOOR = 90,
    LVO_IEEEDPCEIL  = 96,

    /* mathieeedoubtrans.library LVOs */
    LVO_IEEEDPATAN   = 30,
    LVO_IEEEDPSIN    = 36,
    LVO_IEEEDPCOS    = 42,
    LVO_IEEEDPTAN    = 48,
    LVO_IEEEDPSINCOS = 54,
    LVO_IEEEDPSINH   = 60,
    LVO_IEEEDPCOSH   = 66,
    LVO_IEEEDPTANH   = 72,
    LVO_IEEEDPEXP    = 78,
    LVO_IEEEDPLOG    = 84,
    LVO_IEEEDPPOW    = 90,
    LVO_IEEEDPSQRT   = 96,
    LVO_IEEEDPTIEEE  = 102,
    LVO_IEEEDPFIEEE  = 108,
    LVO_IEEEDPASIN   = 114,
    LVO_IEEEDPACOS   = 120,
    LVO_IEEEDPLOG10  = 126,

    /* mathieeesingbas.library LVOs */
    LVO_IEEESPFIX   = 30,
    LVO_IEEESPFLT   = 36,
    LVO_IEEESPCMP   = 42,
    LVO_IEEESPTST   = 48,
    LVO_IEEESPABS   = 54,
    LVO_IEEESPNEG   = 60,
    LVO_IEEESPADD   = 66,
    LVO_IEEESPSUB   = 72,
    LVO_IEEESPMUL   = 78,
    LVO_IEEESPDIV   = 84,
    LVO_IEEESPFLOOR = 90,
    LVO_IEEESPCEIL  = 96
};

struct Fixture
{
    static constexpr ULONG return_pc = 0x76543210;
    static constexpr UWORD default_sr = 0xA715;
    KaffeineContext ctx = {};
    UBYTE memory[4096] = {};
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        ctx.mem = memory;
        ctx.mem_size = sizeof(memory);
        ctx.num_libs = 3;

        auto &bas = ctx.libs[0];
        bas.m68k_addr = BAS_BASE;
        bas.jt_start = BAS_BASE - 0x400;
        std::strcpy(bas.name, "mathieeedoubbas.library");
        bas.thunks = kaffeine_thunks_mathieeedoubbas;
        bas.num_thunks = kaffeine_thunks_mathieeedoubbas_count;
        bas.gen_thunks = poison_gen_table;
        bas.num_gen_thunks = sizeof(poison_gen_table) / sizeof(poison_gen_table[0]) - 1;

        auto &trans = ctx.libs[1];
        trans.m68k_addr = TRANS_BASE;
        trans.jt_start = TRANS_BASE - 0x400;
        std::strcpy(trans.name, "mathieeedoubtrans.library");
        trans.thunks = kaffeine_thunks_mathieeedoubtrans;
        trans.num_thunks = kaffeine_thunks_mathieeedoubtrans_count;
        trans.gen_thunks = poison_gen_table;
        trans.num_gen_thunks = sizeof(poison_gen_table) / sizeof(poison_gen_table[0]) - 1;

        auto &sing = ctx.libs[2];
        sing.m68k_addr = SING_BASE;
        sing.jt_start = SING_BASE - 0x400;
        std::strcpy(sing.name, "mathieeesingbas.library");
        sing.thunks = kaffeine_thunks_mathieeesingbas;
        sing.num_thunks = kaffeine_thunks_mathieeesingbas_count;
        sing.gen_thunks = poison_gen_table;
        sing.num_gen_thunks = sizeof(poison_gen_table) / sizeof(poison_gen_table[0]) - 1;
    }

    void dispatch_raw(ULONG base, ULONG lvo, uint32_t d0, uint32_t d1,
                      uint32_t d2, uint32_t d3, uint32_t a0,
                      bool writes_d1, UWORD init_sr = default_sr)
    {
        for (int i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0D0D000u + (ULONG)i;
            state.a[i] = 0xA0A0A000u + (ULONG)i;
        }
        state.d[0] = d0;
        state.d[1] = d1;
        state.d[2] = d2;
        state.d[3] = d3;
        state.a[0] = a0;
        state.a[7] = 0x80;
        state.sr = init_sr;
        m68k_write32(&ctx, state.a[7], return_pc);
        poison_calls = 0;

        KaffeineDispatchResult result = {};
        bool dispatched = Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result);
        check(dispatched && poison_calls == 0, "mathieee LVO is dispatched to manual thunk", base, lvo);
        check(state.a[7] == 0x84 && result.return_pc == return_pc && !result.supervisor,
              "dispatch consumes exactly one guest return address", base, lvo);

        bool regs_ok = state.d[2] == d2 && state.d[3] == d3 && state.a[0] == a0;
        if (!writes_d1)
            regs_ok = regs_ok && state.d[1] == d1;
        for (int i = 4; i < 8; i++)
            regs_ok = regs_ok && state.d[i] == 0xD0D0D000u + (ULONG)i;
        for (int i = 1; i < 7; i++)
            regs_ok = regs_ok && state.a[i] == 0xA0A0A000u + (ULONG)i;
        check(regs_ok, "non-output registers are left untouched", base, lvo);

        check((state.sr & ~CCR_NZV_MASK) == (init_sr & ~CCR_NZV_MASK),
              "system byte, X and C in SR are left untouched", state.sr, init_sr);
    }

    uint64_t call_d1(ULONG base, ULONG lvo, double parm)
    {
        uint64_t bits = to_bits(parm);
        dispatch_raw(base, lvo, (uint32_t)(bits >> 32), (uint32_t)bits,
                     0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, true);
        return ((uint64_t)state.d[0] << 32) | (uint64_t)state.d[1];
    }

    /* A single goes in D0, a second one in D1, and the result in D0 only:
       D1 must come back unchanged. */
    uint32_t call_s(ULONG lvo, uint32_t d0, uint32_t d1 = 0xD1D1D101u,
                    UWORD init_sr = default_sr)
    {
        dispatch_raw(SING_BASE, lvo, d0, d1, 0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u,
                     false, init_sr);
        return state.d[0];
    }

    uint64_t call_d2(ULONG base, ULONG lvo, double left_or_arg, double right_or_exp)
    {
        uint64_t b01 = to_bits(left_or_arg);
        uint64_t b23 = to_bits(right_or_exp);
        dispatch_raw(base, lvo, (uint32_t)(b01 >> 32), (uint32_t)b01,
                     (uint32_t)(b23 >> 32), (uint32_t)b23, 0xA0A0A000u, true);
        return ((uint64_t)state.d[0] << 32) | (uint64_t)state.d[1];
    }
};

static Fixture fixture;

static bool same_double_bits(uint64_t got_bits, double want)
{
    if (std::isnan(want))
        return std::isnan(from_bits(got_bits));
    return got_bits == to_bits(want);
}

static uint64_t ulp_distance(uint64_t got_bits, double want)
{
    if (std::isnan(want))
        return std::isnan(from_bits(got_bits)) ? 0 : UINT64_MAX;
    uint64_t want_bits = to_bits(want);
    if (got_bits == want_bits)
        return 0;
    if ((got_bits >> 63) != (want_bits >> 63))
        return UINT64_MAX;
    return got_bits > want_bits ? got_bits - want_bits : want_bits - got_bits;
}

/* The order of two singles by sign and magnitude, both zeros equal. */
static int32_t sign_magnitude_order(uint32_t a, uint32_t b)
{
    int64_t ka = a & 0x7FFFFFFFu, kb = b & 0x7FFFFFFFu;
    if (a & 0x80000000u) ka = -ka;
    if (b & 0x80000000u) kb = -kb;
    return ka > kb ? 1 : ka < kb ? -1 : 0;
}

static void test_tables()
{
    bool bas_ok = kaffeine_thunks_mathieeedoubbas_count == 12;
    for (ULONG i = 0; bas_ok && i < 12; i++)
        bas_ok = kaffeine_thunks_mathieeedoubbas[i].lvo == 30 + i * 6 &&
                 kaffeine_thunks_mathieeedoubbas[i].thunk != nullptr;
    bas_ok = bas_ok && kaffeine_thunks_mathieeedoubbas[12].lvo == 0 &&
             kaffeine_thunks_mathieeedoubbas[12].thunk == nullptr;
    check(bas_ok, "mathieeedoubbas table registers LVOs -30 through -96");

    bool trans_ok = kaffeine_thunks_mathieeedoubtrans_count == 17;
    for (ULONG i = 0; trans_ok && i < 17; i++)
        trans_ok = kaffeine_thunks_mathieeedoubtrans[i].lvo == 30 + i * 6 &&
                   kaffeine_thunks_mathieeedoubtrans[i].thunk != nullptr;
    trans_ok = trans_ok && kaffeine_thunks_mathieeedoubtrans[17].lvo == 0 &&
               kaffeine_thunks_mathieeedoubtrans[17].thunk == nullptr;
    check(trans_ok, "mathieeedoubtrans table registers LVOs -30 through -126");

    bool sing_ok = kaffeine_thunks_mathieeesingbas_count == 12;
    for (ULONG i = 0; sing_ok && i < 12; i++)
        sing_ok = kaffeine_thunks_mathieeesingbas[i].lvo == 30 + i * 6 &&
                  kaffeine_thunks_mathieeesingbas[i].thunk != nullptr;
    sing_ok = sing_ok && kaffeine_thunks_mathieeesingbas[12].lvo == 0 &&
              kaffeine_thunks_mathieeesingbas[12].thunk == nullptr;
    check(sing_ok, "mathieeesingbas table registers LVOs -30 through -96");
}

static void expect_fix(uint64_t input_bits, int32_t want, UWORD want_ccr, const char *what)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPFIX,
                             (uint32_t)(input_bits >> 32), (uint32_t)input_bits,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, false, init_sr);
        check(fixture.state.d[0] == (uint32_t)want, what, fixture.state.d[0], (uint32_t)want);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void test_dpfix()
{
    expect_fix(to_bits(0.0), 0, CCR_Z, "IEEEDPFix(+0.0)");
    expect_fix(to_bits(-0.0), 0, CCR_Z, "IEEEDPFix(-0.0)");
    expect_fix(to_bits(1.0), 1, CCR_POS, "IEEEDPFix(1.0)");
    expect_fix(to_bits(-1.0), -1, CCR_N, "IEEEDPFix(-1.0)");
    expect_fix(to_bits(1.9), 1, CCR_POS, "IEEEDPFix(1.9) truncates toward zero");
    expect_fix(to_bits(-1.9), -1, CCR_N, "IEEEDPFix(-1.9) truncates toward zero");
    expect_fix(to_bits(42.75), 42, CCR_POS, "IEEEDPFix(42.75) leaves D1 untouched");
    expect_fix(to_bits(-42.75), -42, CCR_N, "IEEEDPFix(-42.75) leaves D1 untouched");
    expect_fix(to_bits(2147483647.0), INT32_MAX, CCR_POS, "IEEEDPFix(2^31 - 1)");
    expect_fix(to_bits(2147483647.9), INT32_MAX, CCR_POS, "IEEEDPFix(2^31 - 0.1) truncates to INT32_MAX");
    expect_fix(to_bits(-2147483647.9), -2147483647, CCR_N, "IEEEDPFix(-2^31 + 0.1) truncates to -2147483647");
    expect_fix(to_bits(-2147483648.0), INT32_MIN, CCR_N, "IEEEDPFix(-2^31)");
    expect_fix(to_bits(2147483648.0), INT32_MAX, CCR_V, "IEEEDPFix(2^31) clamps to INT32_MAX with V");
    expect_fix(to_bits(1e30), INT32_MAX, CCR_V, "IEEEDPFix(1e30) clamps to INT32_MAX with V");
    expect_fix(to_bits(-2147483648.5), INT32_MIN, CCR_N | CCR_V, "IEEEDPFix(-2^31 - 0.5) clamps to INT32_MIN with N|V");
    expect_fix(to_bits(-1e30), INT32_MIN, CCR_N | CCR_V, "IEEEDPFix(-1e30) clamps to INT32_MIN with N|V");
    expect_fix(0x7FF0000000000000ULL, INT32_MAX, CCR_V, "IEEEDPFix(+Inf) clamps to INT32_MAX with V");
    expect_fix(0xFFF0000000000000ULL, INT32_MIN, CCR_N | CCR_V, "IEEEDPFix(-Inf) clamps to INT32_MIN with N|V");
    expect_fix(0x7FF8000000000001ULL, INT32_MAX, CCR_V, "IEEEDPFix(+NaN) clamps to INT32_MAX with V");
    expect_fix(0xFFF8000000000001ULL, INT32_MIN, CCR_N | CCR_V, "IEEEDPFix(-NaN) clamps to INT32_MIN with N|V");
    expect_fix(0x0000000000000001ULL, 0, CCR_Z, "IEEEDPFix(+denormal) truncates to 0");
    expect_fix(0x8000000000000001ULL, 0, CCR_Z, "IEEEDPFix(-denormal) truncates to 0");
}

static void expect_flt(int32_t input, double want, UWORD want_ccr, const char *what)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPFLT, (uint32_t)input, 0xDEADBEEFu,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, true, init_sr);
        uint64_t got = ((uint64_t)fixture.state.d[0] << 32) | (uint64_t)fixture.state.d[1];
        check(got == to_bits(want), what, got, to_bits(want));
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void test_dpflt()
{
    expect_flt(0, 0.0, CCR_Z, "IEEEDPFlt(0)");
    expect_flt(1, 1.0, CCR_POS, "IEEEDPFlt(1)");
    expect_flt(-1, -1.0, CCR_N, "IEEEDPFlt(-1)");
    expect_flt(42, 42.0, CCR_POS, "IEEEDPFlt(42)");
    expect_flt(-123456789, -123456789.0, CCR_N, "IEEEDPFlt(-123456789)");
    expect_flt(INT32_MAX, 2147483647.0, CCR_POS, "IEEEDPFlt(INT32_MAX)");
    expect_flt(INT32_MIN, -2147483648.0, CCR_N, "IEEEDPFlt(INT32_MIN)");
}

static void expect_cmp(uint64_t left_bits, uint64_t right_bits, int32_t want_d0,
                       UWORD want_ccr, const char *what)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPCMP,
                             (uint32_t)(left_bits >> 32), (uint32_t)left_bits,
                             (uint32_t)(right_bits >> 32), (uint32_t)right_bits,
                             0xA0A0A000u, false, init_sr);
        check(fixture.state.d[0] == (uint32_t)want_d0, what, fixture.state.d[0], (uint32_t)want_d0);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void expect_tst(uint64_t parm_bits, int32_t want_d0, UWORD want_ccr, const char *what)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPTST,
                             (uint32_t)(parm_bits >> 32), (uint32_t)parm_bits,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, false, init_sr);
        check(fixture.state.d[0] == (uint32_t)want_d0, what, fixture.state.d[0], (uint32_t)want_d0);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void test_dpcmp_dptst()
{
    expect_cmp(to_bits(3.5), to_bits(1.25), 1, CCR_POS, "IEEEDPCmp(3.5, 1.25)");
    expect_cmp(to_bits(1.25), to_bits(3.5), -1, CCR_N, "IEEEDPCmp(1.25, 3.5)");
    expect_cmp(to_bits(-2.5), to_bits(-2.5), 0, CCR_Z, "IEEEDPCmp(-2.5, -2.5)");
    expect_cmp(to_bits(0.0), to_bits(-0.0), 0, CCR_Z, "IEEEDPCmp(+0.0, -0.0)");
    expect_cmp(0xFFF0000000000000ULL, 0x7FF0000000000000ULL, -1, CCR_N, "IEEEDPCmp(-Inf, +Inf)");
    expect_cmp(0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 1, CCR_POS, "IEEEDPCmp(+Inf, -Inf)");
    expect_cmp(0x0000000000000001ULL, to_bits(0.0), 1, CCR_POS, "IEEEDPCmp(+denormal, 0.0)");
    expect_cmp(0x8000000000000001ULL, to_bits(0.0), -1, CCR_N, "IEEEDPCmp(-denormal, 0.0)");
    /* NaNs order by sign and magnitude, as AmigaOS 3.2.3 measures them */
    expect_cmp(0x7FF8000000000001ULL, to_bits(1.0), 1, CCR_POS, "IEEEDPCmp(NaN, 1.0): a positive NaN is above");
    expect_cmp(to_bits(1.0), 0x7FF8000000000000ULL, -1, CCR_N, "IEEEDPCmp(1.0, +qNaN)");
    expect_cmp(to_bits(1.0), 0xFFF8000000000000ULL, 1, CCR_POS, "IEEEDPCmp(1.0, -qNaN)");
    expect_cmp(0x7FF0000000000000ULL, 0x7FF8000000000000ULL, -1, CCR_N, "IEEEDPCmp(+Inf, +qNaN)");
    expect_cmp(0xFFF0000000000000ULL, 0xFFF8000000000000ULL, 1, CCR_POS, "IEEEDPCmp(-Inf, -qNaN)");
    expect_cmp(0x7FF8000000000000ULL, 0x7FF8000000000000ULL, 0, CCR_Z, "IEEEDPCmp(qNaN, qNaN)");
    expect_cmp(0x7FF8000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, -1, CCR_N, "IEEEDPCmp(qNaN, all-ones NaN)");
    expect_cmp(to_bits(-2.0), to_bits(-0.5), -1, CCR_N, "IEEEDPCmp(-2.0, -0.5)");
    expect_cmp(to_bits(-0.0), to_bits(0.0), 0, CCR_Z, "IEEEDPCmp(-0.0, +0.0)");

    expect_tst(to_bits(3.5), 1, CCR_POS, "IEEEDPTst(3.5)");
    expect_tst(to_bits(-3.5), -1, CCR_N, "IEEEDPTst(-3.5)");
    expect_tst(to_bits(0.0), 0, CCR_Z, "IEEEDPTst(+0.0)");
    expect_tst(to_bits(-0.0), 0, CCR_Z, "IEEEDPTst(-0.0)");
    expect_tst(0x7FF0000000000000ULL, 1, CCR_POS, "IEEEDPTst(+Inf)");
    expect_tst(0xFFF0000000000000ULL, -1, CCR_N, "IEEEDPTst(-Inf)");
    expect_tst(0x0000000000000001ULL, 1, CCR_POS, "IEEEDPTst(+denormal)");
    expect_tst(0x8000000000000001ULL, -1, CCR_N, "IEEEDPTst(-denormal)");
    expect_tst(0x7FF8000000000001ULL, 1, CCR_POS, "IEEEDPTst(NaN): the sign of the NaN");
    expect_tst(0xFFF8000000000000ULL, -1, CCR_N, "IEEEDPTst(-qNaN)");
    expect_tst(0x7FF0000000000001ULL, 1, CCR_POS, "IEEEDPTst(sNaN)");
}

static void test_dpbasic_arith()
{
    double pos_inf = from_bits(0x7FF0000000000000ULL);
    double neg_inf = from_bits(0xFFF0000000000000ULL);
    double nan_val = from_bits(0x7FF8000000000001ULL);
    double pos_denorm = from_bits(0x0000000000000001ULL);
    double neg_denorm = from_bits(0x8000000000000001ULL);

    /* IEEEDPAbs (-54) and IEEEDPNeg (-60) */
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, -0.0) == to_bits(0.0), "IEEEDPAbs(-0.0) clears sign");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, -3.5) == to_bits(3.5), "IEEEDPAbs(-3.5)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, 3.5) == to_bits(3.5), "IEEEDPAbs(3.5)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, neg_inf) == to_bits(pos_inf), "IEEEDPAbs(-Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, neg_denorm) == to_bits(pos_denorm), "IEEEDPAbs(-denormal)");
    check(same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, nan_val), nan_val), "IEEEDPAbs(NaN)");

    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, 0.0) == to_bits(-0.0), "IEEEDPNeg(+0.0) is -0.0");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, -0.0) == to_bits(0.0), "IEEEDPNeg(-0.0) is +0.0");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, 3.5) == to_bits(-3.5), "IEEEDPNeg(3.5)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, -3.5) == to_bits(3.5), "IEEEDPNeg(-3.5)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, pos_inf) == to_bits(neg_inf), "IEEEDPNeg(+Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, neg_inf) == to_bits(pos_inf), "IEEEDPNeg(-Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, pos_denorm) == to_bits(neg_denorm), "IEEEDPNeg(+denormal)");

    /* IEEEDPAdd (-66), IEEEDPSub (-72), IEEEDPMul (-78), IEEEDPDiv (-84) */
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPADD, 1.5, 2.25) == to_bits(3.75), "IEEEDPAdd(1.5, 2.25)");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPADD, -0.0, -0.0) == to_bits(-0.0), "IEEEDPAdd(-0.0, -0.0)");
    check(same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPADD, pos_inf, neg_inf), nan_val),
          "IEEEDPAdd(+Inf, -Inf) is NaN");

    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPSUB, 5.5, 2.25) == to_bits(3.25), "IEEEDPSub(5.5, 2.25)");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPSUB, 2.25, 5.5) == to_bits(-3.25),
          "IEEEDPSub(2.25, 5.5) preserves operand order");
    check(same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPSUB, pos_inf, pos_inf), nan_val),
          "IEEEDPSub(+Inf, +Inf) is NaN");

    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPMUL, 3.5, -2.0) == to_bits(-7.0), "IEEEDPMul(3.5, -2.0)");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPMUL, -3.5, 0.0) == to_bits(-0.0), "IEEEDPMul(-3.5, +0.0) is -0.0");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPMUL, pos_denorm, 2.0) == to_bits(pos_denorm * 2.0),
          "IEEEDPMul(+denormal, 2.0)");
    check(same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPMUL, pos_inf, 0.0), nan_val),
          "IEEEDPMul(+Inf, 0.0) is NaN");

    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, 7.0, 2.0) == to_bits(3.5), "IEEEDPDIV(7.0, 2.0)");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, 2.0, 8.0) == to_bits(0.25),
          "IEEEDPDiv(2.0, 8.0) preserves operand order");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, 1.0, 0.0) == to_bits(pos_inf), "IEEEDPDiv(1.0, +0.0) is +Inf");
    check(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, 1.0, -0.0) == to_bits(neg_inf), "IEEEDPDiv(1.0, -0.0) is -Inf");
    check(same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, 0.0, 0.0), nan_val),
          "IEEEDPDiv(0.0, 0.0) is NaN");

    /* IEEEDPFloor (-90) and IEEEDPCeil (-96) */
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, 3.7) == to_bits(3.0), "IEEEDPFloor(3.7)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, -3.7) == to_bits(-4.0), "IEEEDPFloor(-3.7)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, -0.0) == to_bits(-0.0), "IEEEDPFloor(-0.0)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, pos_inf) == to_bits(pos_inf), "IEEEDPFloor(+Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, neg_inf) == to_bits(neg_inf), "IEEEDPFloor(-Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, neg_denorm) == to_bits(-1.0), "IEEEDPFloor(-denormal)");
    check(same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, nan_val), nan_val), "IEEEDPFloor(NaN)");

    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, 3.2) == to_bits(4.0), "IEEEDPCeil(3.2)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, -3.2) == to_bits(-3.0), "IEEEDPCeil(-3.2)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, -0.0) == to_bits(-0.0), "IEEEDPCeil(-0.0)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, pos_inf) == to_bits(pos_inf), "IEEEDPCeil(+Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, neg_inf) == to_bits(neg_inf), "IEEEDPCeil(-Inf)");
    check(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, pos_denorm) == to_bits(1.0), "IEEEDPCeil(+denormal)");
    check(same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, nan_val), nan_val), "IEEEDPCeil(NaN)");
}

static void test_dpbasic_reference()
{
    uint32_t seed = 0x89ABCDEFu;
    auto next32 = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
    auto next_double = [&next32](int i) {
        int32_t num = (int32_t)next32();
        int32_t den = (int32_t)(next32() & 0x7FFFu) + 1;
        double val = (double)num / (double)den;
        if (i % 16 == 0)
            val = std::ldexp(val, -1040); /* include subnormals and tiny magnitudes */
        return val;
    };

    bool all_ok = true;
    for (int i = 0; i < 5000 && all_ok; i++)
    {
        double a = next_double(i);
        double b = next_double(i + 1);
        if (b == 0.0)
            b = 1.0;

        int32_t iarg = (int32_t)next32();
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPFLT, (uint32_t)iarg, 0x11111111u,
                             0x22222222u, 0x33333333u, 0xA0A0A000u, true);
        uint64_t flt_bits = ((uint64_t)fixture.state.d[0] << 32) | fixture.state.d[1];
        all_ok = all_ok && flt_bits == to_bits((double)iarg);

        uint64_t abits = to_bits(a);
        fixture.dispatch_raw(BAS_BASE, LVO_IEEEDPFIX, (uint32_t)(abits >> 32), (uint32_t)abits,
                             0x22222222u, 0x33333333u, 0xA0A0A000u, false);
        int32_t want_fix = (a >= -2147483648.0 && a < 2147483648.0)
            ? (int32_t)a
            : ((abits >> 63) ? INT32_MIN : INT32_MAX);
        all_ok = all_ok && (int32_t)fixture.state.d[0] == want_fix;

        all_ok = all_ok && same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPABS, a), std::fabs(a));
        all_ok = all_ok && same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPNEG, a), -a);
        all_ok = all_ok && same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPADD, a, b), a + b);
        all_ok = all_ok && same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPSUB, a, b), a - b);
        all_ok = all_ok && same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPMUL, a, b), a * b);
        all_ok = all_ok && same_double_bits(fixture.call_d2(BAS_BASE, LVO_IEEEDPDIV, a, b), a / b);
        all_ok = all_ok && same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPFLOOR, a), std::floor(a));
        all_ok = all_ok && same_double_bits(fixture.call_d1(BAS_BASE, LVO_IEEEDPCEIL, a), std::ceil(a));
    }
    check(all_ok, "mathieeedoubbas functions match host IEEE 754 arithmetic across 5000 pairs");
}

/* mathieeesingbas.library */

static bool same_float_bits(uint32_t got_bits, float want)
{
    if (std::isnan(want))
        return std::isnan(bits_to_float(got_bits));
    return got_bits == float_to_bits(want);
}

/* Both start values of SR: the thunk must replace N, Z and V only. */
static void expect_s(ULONG lvo, uint32_t d0, uint32_t d1, uint32_t want_d0,
                     UWORD want_ccr, const char *what, bool nan_result = false)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        uint32_t got = fixture.call_s(lvo, d0, d1, init_sr);
        bool value_ok = nan_result ? std::isnan(bits_to_float(got)) : got == want_d0;
        check(value_ok, what, got, want_d0);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static uint32_t f(float value)
{
    return float_to_bits(value);
}

static void test_spfix_spflt()
{
    expect_s(LVO_IEEESPFIX, f(0.0f), 0, 0, CCR_Z, "IEEESPFix(+0.0)");
    expect_s(LVO_IEEESPFIX, f(-0.0f), 0, 0, CCR_Z, "IEEESPFix(-0.0)");
    expect_s(LVO_IEEESPFIX, f(1.9f), 0, 1, CCR_POS, "IEEESPFix(1.9) truncates toward zero");
    expect_s(LVO_IEEESPFIX, f(-1.9f), 0, (uint32_t)-1, CCR_N, "IEEESPFix(-1.9) truncates toward zero");
    expect_s(LVO_IEEESPFIX, f(2147483520.0f), 0, 2147483520u, CCR_POS,
             "IEEESPFix() of the largest single below 2^31");
    expect_s(LVO_IEEESPFIX, f(-2147483648.0f), 0, 0x80000000u, CCR_N, "IEEESPFix(-2^31)");
    expect_s(LVO_IEEESPFIX, f(2147483648.0f), 0, 0x7FFFFFFFu, CCR_V,
             "IEEESPFix(2^31) clamps to INT32_MAX with V");
    expect_s(LVO_IEEESPFIX, f(-2147483904.0f), 0, 0x80000000u, CCR_N | CCR_V,
             "IEEESPFix() below -2^31 clamps to INT32_MIN with N|V");
    expect_s(LVO_IEEESPFIX, 0x7F800000u, 0, 0x7FFFFFFFu, CCR_V, "IEEESPFix(+Inf)");
    expect_s(LVO_IEEESPFIX, 0xFF800000u, 0, 0x80000000u, CCR_N | CCR_V, "IEEESPFix(-Inf)");
    expect_s(LVO_IEEESPFIX, 0x7FC00001u, 0, 0x7FFFFFFFu, CCR_V, "IEEESPFix(+NaN)");
    expect_s(LVO_IEEESPFIX, 0xFFC00001u, 0, 0x80000000u, CCR_N | CCR_V, "IEEESPFix(-NaN)");
    expect_s(LVO_IEEESPFIX, 0x00000001u, 0, 0, CCR_Z, "IEEESPFix(+denormal)");

    expect_s(LVO_IEEESPFLT, 0, 0, f(0.0f), CCR_Z, "IEEESPFlt(0)");
    expect_s(LVO_IEEESPFLT, (uint32_t)-1, 0, f(-1.0f), CCR_N, "IEEESPFlt(-1)");
    expect_s(LVO_IEEESPFLT, 16777216u, 0, f(16777216.0f), CCR_POS, "IEEESPFlt(2^24) is exact");
    expect_s(LVO_IEEESPFLT, 16777217u, 0, f(16777216.0f), CCR_V,
             "IEEESPFlt(2^24 + 1) rounds to even and sets V");
    expect_s(LVO_IEEESPFLT, (uint32_t)-16777217, 0, f(-16777216.0f), CCR_N | CCR_V,
             "IEEESPFlt(-2^24 - 1) rounds to even and sets N|V");
    expect_s(LVO_IEEESPFLT, 0x7FFFFFFFu, 0, f(2147483648.0f), CCR_V, "IEEESPFlt(INT32_MAX) sets V");
    expect_s(LVO_IEEESPFLT, 0x80000000u, 0, f(-2147483648.0f), CCR_N, "IEEESPFlt(INT32_MIN) is exact");
}

static void test_spcmp_sptst()
{
    expect_s(LVO_IEEESPCMP, f(3.5f), f(1.25f), 1, CCR_POS, "IEEESPCmp(3.5, 1.25)");
    expect_s(LVO_IEEESPCMP, f(1.25f), f(3.5f), (uint32_t)-1, CCR_N, "IEEESPCmp(1.25, 3.5)");
    expect_s(LVO_IEEESPCMP, f(-2.5f), f(-2.5f), 0, CCR_Z, "IEEESPCmp(-2.5, -2.5)");
    expect_s(LVO_IEEESPCMP, f(0.0f), f(-0.0f), 0, CCR_Z, "IEEESPCmp(+0.0, -0.0)");
    expect_s(LVO_IEEESPCMP, 0xFF800000u, 0x7F800000u, (uint32_t)-1, CCR_N, "IEEESPCmp(-Inf, +Inf)");
    expect_s(LVO_IEEESPCMP, 0x80000001u, f(0.0f), (uint32_t)-1, CCR_N, "IEEESPCmp(-denormal, 0.0)");
    expect_s(LVO_IEEESPCMP, 0x7FC00001u, f(1.0f), 1, CCR_POS, "IEEESPCmp(NaN, 1.0): a positive NaN is above");
    expect_s(LVO_IEEESPCMP, f(1.0f), 0xFFC00000u, 1, CCR_POS, "IEEESPCmp(1.0, -qNaN)");
    expect_s(LVO_IEEESPCMP, 0x7F800000u, 0x7FC00000u, (uint32_t)-1, CCR_N, "IEEESPCmp(+Inf, +qNaN)");
    expect_s(LVO_IEEESPCMP, f(-2.0f), f(-0.5f), (uint32_t)-1, CCR_N, "IEEESPCmp(-2.0, -0.5)");

    expect_s(LVO_IEEESPTST, f(3.5f), 0, 1, CCR_POS, "IEEESPTst(3.5)");
    expect_s(LVO_IEEESPTST, f(-3.5f), 0, (uint32_t)-1, CCR_N, "IEEESPTst(-3.5)");
    expect_s(LVO_IEEESPTST, f(-0.0f), 0, 0, CCR_Z, "IEEESPTst(-0.0)");
    expect_s(LVO_IEEESPTST, 0x00000001u, 0, 1, CCR_POS, "IEEESPTst(+denormal)");
    expect_s(LVO_IEEESPTST, 0x7FC00001u, 0, 1, CCR_POS, "IEEESPTst(NaN): the sign of the NaN");
    expect_s(LVO_IEEESPTST, 0xFFC00000u, 0, (uint32_t)-1, CCR_N, "IEEESPTst(-qNaN)");
}

static void test_spbasic_arith()
{
    const uint32_t inf = 0x7F800000u, ninf = 0xFF800000u, nan = 0x7FC00001u;
    const float big = 3.0e38f;

    expect_s(LVO_IEEESPABS, f(-3.5f), 0, f(3.5f), CCR_POS, "IEEESPAbs(-3.5)");
    expect_s(LVO_IEEESPABS, f(-0.0f), 0, f(0.0f), CCR_Z, "IEEESPAbs(-0.0) clears the sign");
    expect_s(LVO_IEEESPABS, ninf, 0, inf, CCR_POS, "IEEESPAbs(-Inf)");
    expect_s(LVO_IEEESPNEG, f(3.5f), 0, f(-3.5f), CCR_N, "IEEESPNeg(3.5)");
    expect_s(LVO_IEEESPNEG, f(0.0f), 0, f(-0.0f), CCR_Z, "IEEESPNeg(+0.0) is -0.0");

    expect_s(LVO_IEEESPADD, f(1.5f), f(2.25f), f(3.75f), CCR_POS, "IEEESPAdd(1.5, 2.25)");
    expect_s(LVO_IEEESPADD, f(-0.0f), f(-0.0f), f(-0.0f), CCR_Z, "IEEESPAdd(-0.0, -0.0)");
    expect_s(LVO_IEEESPADD, f(big), f(big), inf, CCR_V, "IEEESPAdd() overflowing to +Inf sets V");
    expect_s(LVO_IEEESPADD, inf, f(1.0f), inf, CCR_V, "IEEESPAdd(+Inf, 1.0) sets V as AROS does");
    expect_s(LVO_IEEESPADD, inf, ninf, nan, CCR_POS, "IEEESPAdd(+Inf, -Inf) is NaN", true);

    expect_s(LVO_IEEESPSUB, f(2.25f), f(5.5f), f(-3.25f), CCR_N, "IEEESPSub(2.25, 5.5) keeps the order");
    expect_s(LVO_IEEESPSUB, f(-big), f(big), ninf, CCR_N | CCR_V, "IEEESPSub() overflowing to -Inf");
    expect_s(LVO_IEEESPSUB, inf, f(1.0f), inf, CCR_POS, "IEEESPSub(+Inf, 1.0) leaves V clear");

    expect_s(LVO_IEEESPMUL, f(3.5f), f(-2.0f), f(-7.0f), CCR_N, "IEEESPMul(3.5, -2.0)");
    expect_s(LVO_IEEESPMUL, f(-3.5f), f(0.0f), f(-0.0f), CCR_Z, "IEEESPMul(-3.5, +0.0) is -0.0");
    expect_s(LVO_IEEESPMUL, f(big), f(2.0f), inf, CCR_V, "IEEESPMul() overflowing to +Inf");
    expect_s(LVO_IEEESPMUL, inf, f(0.0f), nan, CCR_POS, "IEEESPMul(+Inf, 0.0) is NaN", true);
    expect_s(LVO_IEEESPMUL, 0x00000001u, f(0.5f), f(0.0f), CCR_Z | CCR_V,
             "IEEESPMul(smallest denormal, 0.5) underflows to +0.0 with V");
    expect_s(LVO_IEEESPMUL, f(1.0e-30f), f(-1.0e-30f), f(-0.0f), CCR_Z | CCR_V,
             "IEEESPMul(1e-30, -1e-30) underflows to -0.0 with V");
    expect_s(LVO_IEEESPMUL, 0x00000003u, f(0.5f), 0x00000002u, CCR_V,
             "IEEESPMul(3 * 2^-149, 0.5) rounds to a denormal with V");
    expect_s(LVO_IEEESPMUL, 0x00000004u, f(0.5f), 0x00000002u, CCR_POS,
             "IEEESPMul(4 * 2^-149, 0.5) is an exact denormal, V clear");
    expect_s(LVO_IEEESPMUL, 0x00800000u, 0x3F7FFFFFu, 0x00800000u, CCR_POS,
             "IEEESPMul() rounding up to the smallest normal single leaves V clear");

    expect_s(LVO_IEEESPDIV, f(2.0f), f(8.0f), f(0.25f), CCR_POS, "IEEESPDiv(2.0, 8.0) keeps the order");
    expect_s(LVO_IEEESPDIV, f(1.0f), f(0.0f), inf, CCR_V, "IEEESPDiv(1.0, +0.0) is +Inf with V");
    expect_s(LVO_IEEESPDIV, f(1.0f), f(-0.0f), ninf, CCR_N | CCR_V, "IEEESPDiv(1.0, -0.0)");
    expect_s(LVO_IEEESPDIV, f(0.0f), f(0.0f), nan, CCR_V, "IEEESPDiv(0.0, 0.0) is NaN with V", true);
    expect_s(LVO_IEEESPDIV, 0x00800000u, f(2.0f), 0x00400000u, CCR_POS,
             "IEEESPDiv(smallest normal, 2.0) is an exact denormal, V clear");
    expect_s(LVO_IEEESPDIV, 0x00800000u, f(3.0f), 0x002AAAABu, CCR_V,
             "IEEESPDiv(smallest normal, 3.0) rounds to a denormal with V");
    expect_s(LVO_IEEESPDIV, f(1.0e-30f), f(-1.0e30f), f(-0.0f), CCR_Z | CCR_V,
             "IEEESPDiv(1e-30, -1e30) underflows to -0.0 with V");
    expect_s(LVO_IEEESPDIV, f(-1.0f), inf, f(-0.0f), CCR_Z,
             "IEEESPDiv(-1.0, +Inf) is an exact -0.0, V clear");

    expect_s(LVO_IEEESPFLOOR, f(-3.7f), 0, f(-4.0f), CCR_N, "IEEESPFloor(-3.7)");
    expect_s(LVO_IEEESPFLOOR, f(-0.0f), 0, f(-0.0f), CCR_Z, "IEEESPFloor(-0.0)");
    expect_s(LVO_IEEESPFLOOR, 0x80000001u, 0, f(-1.0f), CCR_N, "IEEESPFloor(-denormal)");
    expect_s(LVO_IEEESPCEIL, f(3.2f), 0, f(4.0f), CCR_POS, "IEEESPCeil(3.2)");
    expect_s(LVO_IEEESPCEIL, f(-3.2f), 0, f(-3.0f), CCR_N, "IEEESPCeil(-3.2)");
    expect_s(LVO_IEEESPCEIL, 0x00000001u, 0, f(1.0f), CCR_POS, "IEEESPCeil(+denormal)");
}

/* The documented flags of each function, from the result and operands. */
static UWORD sp_flags(float res, bool overflow)
{
    return (res == 0.0f ? CCR_Z : 0) | (res < 0.0f ? CCR_N : 0) | (overflow ? CCR_V : 0);
}

static void test_spbasic_reference()
{
    uint32_t seed = 0x2468ACE1u;
    auto next32 = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    bool values_ok = true, flags_ok = true;
    int underflows = 0;
    /* IEEE 754 underflow with tininess after rounding: a zero or denormal
       result that is not exact. Two singles multiply exactly as doubles. */
    auto tiny = [](float res) { return res == 0.0f || std::fpclassify(res) == FP_SUBNORMAL; };

    /* Every bit pattern is a single: random ones cover NaN, Inf,
       denormals and both zeros. */
    for (int i = 0; i < 20000; i++)
    {
        uint32_t abits = next32(), bbits = next32();
        if (i % 4 == 1)
            bbits = (bbits & 0x807FFFFFu) | (abits & 0x7F800000u); /* close exponents */
        float a = bits_to_float(abits), b = bits_to_float(bbits);
        float product = a * b, quotient = a / b;
        bool mul_under = tiny(product) && (double)product != (double)a * (double)b;
        bool div_under = std::isfinite(b) && tiny(quotient) &&
                         (double)quotient * (double)b != (double)a;
        underflows += mul_under + div_under;
        struct { ULONG lvo; float res; bool overflow; } ops[] = {
            { LVO_IEEESPADD, a + b, (bool)std::isinf(a + b) },
            { LVO_IEEESPSUB, a - b, std::isfinite(a) && std::isfinite(b) && !std::isfinite(a - b) },
            { LVO_IEEESPMUL, product, mul_under ||
              (std::isfinite(a) && std::isfinite(b) && !std::isfinite(product)) },
            { LVO_IEEESPDIV, quotient, div_under ||
              (std::isfinite(a) && std::isfinite(b) && !std::isfinite(quotient)) },
        };
        for (const auto &op : ops)
        {
            uint32_t got = fixture.call_s(op.lvo, abits, bbits);
            values_ok = values_ok && same_float_bits(got, op.res);
            flags_ok = flags_ok &&
                (fixture.state.sr & CCR_NZV_MASK) == sp_flags(op.res, op.overflow);
        }

        values_ok = values_ok && same_float_bits(fixture.call_s(LVO_IEEESPABS, abits), std::fabs(a));
        values_ok = values_ok && same_float_bits(fixture.call_s(LVO_IEEESPNEG, abits), -a);
        values_ok = values_ok && same_float_bits(fixture.call_s(LVO_IEEESPFLOOR, abits), std::floor(a));
        values_ok = values_ok && same_float_bits(fixture.call_s(LVO_IEEESPCEIL, abits), std::ceil(a));

        int32_t integer = (int32_t)next32() >> (next32() % 31);
        float flt = (float)integer;
        values_ok = values_ok && fixture.call_s(LVO_IEEESPFLT, (uint32_t)integer) == float_to_bits(flt);
        flags_ok = flags_ok && (fixture.state.sr & CCR_NZV_MASK) ==
            sp_flags(flt, (double)flt != (double)integer);

        bool in_range = a >= -2147483648.0f && a < 2147483648.0f;
        int32_t want_fix = in_range ? (int32_t)a : ((abits >> 31) ? INT32_MIN : INT32_MAX);
        values_ok = values_ok && (int32_t)fixture.call_s(LVO_IEEESPFIX, abits) == want_fix;
        flags_ok = flags_ok && (fixture.state.sr & CCR_NZV_MASK) ==
            ((want_fix == 0 ? CCR_Z : 0) | (want_fix < 0 ? CCR_N : 0) | (in_range ? 0 : CCR_V));

        /* Ordered values compare as IEEE 754 does; with a NaN, by sign and
           magnitude, as AmigaOS 3.2.3 measures it. */
        int32_t want_cmp = (std::isnan(a) || std::isnan(b)) ? sign_magnitude_order(abits, bbits)
                                                          : a > b ? 1 : a < b ? -1 : 0;
        values_ok = values_ok && (int32_t)fixture.call_s(LVO_IEEESPCMP, abits, bbits) == want_cmp;
        flags_ok = flags_ok && (fixture.state.sr & CCR_NZV_MASK) ==
            ((want_cmp == 0 ? CCR_Z : 0) | (want_cmp < 0 ? CCR_N : 0));
    }
    check(values_ok, "mathieeesingbas results match host IEEE 754 singles across 20000 operand pairs");
    check(flags_ok, "mathieeesingbas sets the documented N, Z and V across 20000 operand pairs");
    check(underflows > 4000,
          "the 20000 operand pairs include more than 4000 underflowing products and quotients");
}

static void test_dptrans_functions()
{
    const double angles[] = { 0.0, -0.0, 0.25, -0.25, 0.5, -0.5, 0.85, -0.85, 1.0, -1.0 };
    const double positives[] = { 0.125, 0.5, 1.0, 2.0, 4.0, 10.0, 100.0, 1000.0 };
    double nan_val = from_bits(0x7FF8000000000001ULL);
    double neg_inf = from_bits(0xFFF0000000000000ULL);

    bool trig_ok = true;
    for (double x : angles)
    {
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPATAN, x), std::atan(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSIN, x), std::sin(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPCOS, x), std::cos(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPTAN, x), std::tan(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSINH, x), std::sinh(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPCOSH, x), std::cosh(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPTANH, x), std::tanh(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPEXP, x), std::exp(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPASIN, x), std::asin(x)) == 0;
        trig_ok = trig_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPACOS, x), std::acos(x)) == 0;
    }
    check(trig_ok, "trig, hyperbolic and exp thunks match host libm", 0, 0);

    bool log_ok = true;
    for (double x : positives)
    {
        log_ok = log_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPLOG, x), std::log(x)) == 0;
        log_ok = log_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSQRT, x), std::sqrt(x)) == 0;
        log_ok = log_ok && ulp_distance(fixture.call_d1(TRANS_BASE, LVO_IEEEDPLOG10, x), std::log10(x)) == 0;
    }
    check(log_ok, "log, log10 and sqrt thunks match host libm", 0, 0);

    /* Exact mathematical checkpoints and domain poles */
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPCOS, 0.0) == to_bits(1.0), "IEEEDPCos(0.0) == 1.0");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPEXP, 0.0) == to_bits(1.0), "IEEEDPExp(0.0) == 1.0");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPLOG, 1.0) == to_bits(0.0), "IEEEDPLog(1.0) == 0.0");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPLOG, 0.0) == to_bits(neg_inf), "IEEEDPLog(0.0) == -Inf");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPLOG10, 1000.0) == to_bits(3.0), "IEEEDPLog10(1000.0) == 3.0");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSQRT, 4.0) == to_bits(2.0), "IEEEDPSqrt(4.0) == 2.0");
    check(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSQRT, -0.0) == to_bits(-0.0), "IEEEDPSqrt(-0.0) == -0.0");
    check(same_double_bits(fixture.call_d1(TRANS_BASE, LVO_IEEEDPSQRT, -1.0), nan_val),
          "IEEEDPSqrt(-1.0) is NaN");
    check(same_double_bits(fixture.call_d1(TRANS_BASE, LVO_IEEEDPASIN, 2.0), nan_val),
          "IEEEDPAsin(2.0) is NaN");
    check(same_double_bits(fixture.call_d1(TRANS_BASE, LVO_IEEEDPACOS, -2.0), nan_val),
          "IEEEDPAcos(-2.0) is NaN");

    /* IEEEDPPow (-90): arg (base) in D0/D1, exp (exponent) in D2/D3 */
    check(fixture.call_d2(TRANS_BASE, LVO_IEEEDPPOW, 2.0, 3.0) == to_bits(8.0),
          "IEEEDPPow(exp=3.0 in D2/D3, arg=2.0 in D0/D1) == 8.0");
    check(fixture.call_d2(TRANS_BASE, LVO_IEEEDPPOW, 3.0, 2.0) == to_bits(9.0),
          "IEEEDPPow(exp=2.0 in D2/D3, arg=3.0 in D0/D1) == 9.0");
    check(fixture.call_d2(TRANS_BASE, LVO_IEEEDPPOW, 9.0, 0.5) == to_bits(3.0),
          "IEEEDPPow(exp=0.5, arg=9.0) == 3.0");
    check(fixture.call_d2(TRANS_BASE, LVO_IEEEDPPOW, 5.0, 0.0) == to_bits(1.0),
          "IEEEDPPow(exp=0.0, arg=5.0) == 1.0");
}

static void test_dptieee_dpfieee()
{
    const double samples[] = { 0.0, -0.0, 1.0, -1.0, 1.5, -42.25, 1e300, -1e300 };

    for (double val : samples)
    {
        uint64_t bits = to_bits(val);
        fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPTIEEE,
                             (uint32_t)(bits >> 32), (uint32_t)bits,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, false);
        uint32_t want_f = float_to_bits((float)val);
        check(fixture.state.d[0] == want_f, "IEEEDPTieee converts double to single bits in D0",
              fixture.state.d[0], want_f);

        fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPFIEEE,
                             want_f, 0xDEADBEEFu,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, true);
        uint64_t got_d = ((uint64_t)fixture.state.d[0] << 32) | fixture.state.d[1];
        double want_d = (double)bits_to_float(want_f);
        check(got_d == to_bits(want_d), "IEEEDPFieee converts single bits in D0 to double in D0/D1",
              got_d, to_bits(want_d));
    }

    /* Single-precision denormal and NaN */
    fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPFIEEE, 0x00000001u, 0xDEADBEEFu,
                         0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, true);
    uint64_t denorm_d = ((uint64_t)fixture.state.d[0] << 32) | fixture.state.d[1];
    check(denorm_d == to_bits((double)bits_to_float(0x00000001u)),
          "IEEEDPFieee preserves single-precision denormal");

    uint64_t nan_bits = 0x7FF8000000000001ULL;
    fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPTIEEE,
                         (uint32_t)(nan_bits >> 32), (uint32_t)nan_bits,
                         0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, false);
    check(std::isnan(bits_to_float(fixture.state.d[0])), "IEEEDPTieee(NaN) returns float NaN");
}

static void test_dpsincos()
{
    const double parm = 0.7;
    const uint64_t parm_bits = to_bits(parm);
    const uint64_t want_sin = to_bits(std::sin(parm));
    const uint64_t want_cos = to_bits(std::cos(parm));

    /* Valid guest pointers: aligned (0x200), word-aligned (0x202) and exact end of arena */
    for (uint32_t pf2 : { 0x200u, 0x202u, (uint32_t)(sizeof(fixture.memory) - 8) })
    {
        std::memset(fixture.memory, 0xA5, sizeof(fixture.memory));
        fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPSINCOS,
                             (uint32_t)(parm_bits >> 32), (uint32_t)parm_bits,
                             0xD2D2D202u, 0xD3D3D303u, pf2, true);
        uint64_t got_sin = ((uint64_t)fixture.state.d[0] << 32) | fixture.state.d[1];
        uint64_t got_cos = ((uint64_t)m68k_read32(&fixture.ctx, pf2) << 32) |
                           (uint64_t)m68k_read32(&fixture.ctx, pf2 + 4);
        check(got_sin == want_sin, "IEEEDPSincos returns sin(parm) in D0/D1", got_sin, want_sin);
        check(got_cos == want_cos, "IEEEDPSincos writes big-endian cos(parm) to *A0", got_cos, want_cos);

        bool canary_ok = true;
        for (size_t i = 0; i < sizeof(fixture.memory); i++)
        {
            if ((i >= 0x80 && i < 0x84) || (i >= pf2 && i < pf2 + 8))
                continue;
            if (fixture.memory[i] != 0xA5)
            {
                canary_ok = false;
                break;
            }
        }
        check(canary_ok, "IEEEDPSincos touches only the 8-byte output at A0", pf2, 0);
    }

    /* Invalid guest pointers: NULL, straddling end of arena, out of range and wrapping */
    for (uint32_t bad_pf2 : { 0u, (uint32_t)(sizeof(fixture.memory) - 7),
                              (uint32_t)(sizeof(fixture.memory) - 4),
                              (uint32_t)sizeof(fixture.memory),
                              0x80000000u, 0xFFFFFFFCu, 0xFFFFFFFFu })
    {
        std::memset(fixture.memory, 0x5A, sizeof(fixture.memory));
        fixture.dispatch_raw(TRANS_BASE, LVO_IEEEDPSINCOS,
                             (uint32_t)(parm_bits >> 32), (uint32_t)parm_bits,
                             0xD2D2D202u, 0xD3D3D303u, bad_pf2, true);
        uint64_t got_sin = ((uint64_t)fixture.state.d[0] << 32) | fixture.state.d[1];
        check(got_sin == want_sin, "IEEEDPSincos still returns sin(parm) when A0 is invalid",
              bad_pf2, got_sin);

        bool untouched = true;
        for (size_t i = 0; i < sizeof(fixture.memory); i++)
        {
            if (i >= 0x80 && i < 0x84)
                continue;
            if (fixture.memory[i] != 0x5A)
            {
                untouched = false;
                break;
            }
        }
        check(untouched, "IEEEDPSincos refuses invalid A0 without writing guest memory", bad_pf2, 0);
    }
}

/* Run a single-double LVO twice (with incoming CCR=0x00 and CCR=0x1F) and
   verify that N, Z and V match want_ccr while X, C and the system byte are
   preserved. */
static void check_ccr_d1(ULONG base, ULONG lvo, double parm, UWORD want_ccr,
                         bool writes_d1, const char *what, uint32_t a0 = 0xA0A0A000u)
{
    uint64_t bits = to_bits(parm);
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(base, lvo, (uint32_t)(bits >> 32), (uint32_t)bits,
                             0xD2D2D202u, 0xD3D3D303u, a0, writes_d1, init_sr);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

/* Run a two-double LVO twice (with incoming CCR=0x00 and CCR=0x1F) and
   verify that N, Z and V match want_ccr while X, C and the system byte are
   preserved. */
static void check_ccr_d2(ULONG base, ULONG lvo, double arg01, double arg23,
                         UWORD want_ccr, bool writes_d1, const char *what)
{
    uint64_t b01 = to_bits(arg01);
    uint64_t b23 = to_bits(arg23);
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(base, lvo, (uint32_t)(b01 >> 32), (uint32_t)b01,
                             (uint32_t)(b23 >> 32), (uint32_t)b23, 0xA0A0A000u,
                             writes_d1, init_sr);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void check_ccr_u32(ULONG base, ULONG lvo, uint32_t d0, UWORD want_ccr,
                          const char *what)
{
    for (UWORD init_sr : { (UWORD)0xA700u, (UWORD)0x581Fu })
    {
        fixture.dispatch_raw(base, lvo, d0, 0xDEADBEEFu,
                             0xD2D2D202u, 0xD3D3D303u, 0xA0A0A000u, true, init_sr);
        check((fixture.state.sr & CCR_NZV_MASK) == want_ccr, what,
              fixture.state.sr & CCR_NZV_MASK, want_ccr);
    }
}

static void test_condition_codes()
{
    /* mathieeedoubbas.library (-30 to -96) */
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFIX, 12.75, CCR_POS, false, "IEEEDPFix CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFIX, -12.75, CCR_N, false, "IEEEDPFix CCR negative");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFIX, 0.5, CCR_Z, false, "IEEEDPFix CCR zero");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFIX, 2147483648.0, CCR_V, false, "IEEEDPFix CCR positive overflow");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFIX, -2147483649.0, CCR_N | CCR_V, false, "IEEEDPFix CCR negative overflow");

    check_ccr_u32(BAS_BASE, LVO_IEEEDPFLT, 7u, CCR_POS, "IEEEDPFlt CCR positive");
    check_ccr_u32(BAS_BASE, LVO_IEEEDPFLT, (uint32_t)-7, CCR_N, "IEEEDPFlt CCR negative");
    check_ccr_u32(BAS_BASE, LVO_IEEEDPFLT, 0u, CCR_Z, "IEEEDPFlt CCR zero");

    check_ccr_d2(BAS_BASE, LVO_IEEEDPCMP, 4.0, 2.0, CCR_POS, false, "IEEEDPCmp CCR positive");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPCMP, 2.0, 4.0, CCR_N, false, "IEEEDPCmp CCR negative");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPCMP, 2.0, 2.0, CCR_Z, false, "IEEEDPCmp CCR zero");

    check_ccr_d1(BAS_BASE, LVO_IEEEDPTST, 4.0, CCR_POS, false, "IEEEDPTst CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPTST, -4.0, CCR_N, false, "IEEEDPTst CCR negative");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPTST, -0.0, CCR_Z, false, "IEEEDPTst CCR zero");

    check_ccr_d1(BAS_BASE, LVO_IEEEDPABS, -4.0, CCR_POS, true, "IEEEDPAbs CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPABS, -0.0, CCR_Z, true, "IEEEDPAbs CCR zero");

    check_ccr_d1(BAS_BASE, LVO_IEEEDPNEG, -4.0, CCR_POS, true, "IEEEDPNeg CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPNEG, 4.0, CCR_N, true, "IEEEDPNeg CCR negative");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPNEG, 0.0, CCR_Z, true, "IEEEDPNeg CCR zero");

    check_ccr_d2(BAS_BASE, LVO_IEEEDPADD, 1.5, 2.5, CCR_POS, true, "IEEEDPAdd CCR positive");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPADD, -1.5, -2.5, CCR_N, true, "IEEEDPAdd CCR negative");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPADD, 2.5, -2.5, CCR_Z, true, "IEEEDPAdd CCR zero");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPADD, 1e308, 1e308, CCR_V, true, "IEEEDPAdd CCR positive overflow");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPADD, -1e308, -1e308, CCR_N | CCR_V, true, "IEEEDPAdd CCR negative overflow");

    check_ccr_d2(BAS_BASE, LVO_IEEEDPSUB, 5.0, 2.0, CCR_POS, true, "IEEEDPSub CCR positive");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPSUB, 2.0, 5.0, CCR_N, true, "IEEEDPSub CCR negative");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPSUB, 5.0, 5.0, CCR_Z, true, "IEEEDPSub CCR zero");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPSUB, 1e308, -1e308, CCR_V, true, "IEEEDPSub CCR positive overflow");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPSUB, -1e308, 1e308, CCR_N | CCR_V, true, "IEEEDPSub CCR negative overflow");

    check_ccr_d2(BAS_BASE, LVO_IEEEDPMUL, 3.0, 2.0, CCR_POS, true, "IEEEDPMul CCR positive");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPMUL, -3.0, 2.0, CCR_N, true, "IEEEDPMul CCR negative");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPMUL, -3.0, 0.0, CCR_Z, true, "IEEEDPMul CCR zero");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPMUL, 1e308, 2.0, CCR_V, true, "IEEEDPMul CCR positive overflow");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPMUL, -1e308, 2.0, CCR_N | CCR_V, true, "IEEEDPMul CCR negative overflow");

    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, 6.0, 2.0, CCR_POS, true, "IEEEDPDiv CCR positive");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, -6.0, 2.0, CCR_N, true, "IEEEDPDiv CCR negative");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, -0.0, 2.0, CCR_Z, true, "IEEEDPDiv CCR zero");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, 1.0, 0.0, CCR_V, true, "IEEEDPDiv CCR positive overflow");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, -1.0, 0.0, CCR_N | CCR_V, true, "IEEEDPDiv CCR negative overflow");
    check_ccr_d2(BAS_BASE, LVO_IEEEDPDIV, 0.0, 0.0, CCR_V, true, "IEEEDPDiv CCR 0/0 NaN overflow");

    check_ccr_d1(BAS_BASE, LVO_IEEEDPFLOOR, 2.7, CCR_POS, true, "IEEEDPFloor CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFLOOR, -0.2, CCR_N, true, "IEEEDPFloor CCR negative");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPFLOOR, 0.7, CCR_Z, true, "IEEEDPFloor CCR zero");

    check_ccr_d1(BAS_BASE, LVO_IEEEDPCEIL, 0.2, CCR_POS, true, "IEEEDPCeil CCR positive");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPCEIL, -2.7, CCR_N, true, "IEEEDPCeil CCR negative");
    check_ccr_d1(BAS_BASE, LVO_IEEEDPCEIL, -0.7, CCR_Z, true, "IEEEDPCeil CCR zero");

    /* mathieeedoubtrans.library (-30 to -126) */
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPATAN, 1.0, CCR_POS, true, "IEEEDPAtan CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPATAN, -1.0, CCR_N, true, "IEEEDPAtan CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPATAN, 0.0, CCR_Z, true, "IEEEDPAtan CCR zero");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSIN, 0.5, CCR_POS, true, "IEEEDPSin CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSIN, -0.5, CCR_N, true, "IEEEDPSin CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSIN, 0.0, CCR_Z, true, "IEEEDPSin CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSIN, INFINITY, CCR_V, true, "IEEEDPSin(+Inf) CCR overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSIN, -INFINITY, CCR_V, true, "IEEEDPSin(-Inf) CCR overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOS, 0.0, CCR_POS, true, "IEEEDPCos CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOS, 3.0, CCR_N, true, "IEEEDPCos CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOS, INFINITY, CCR_V, true, "IEEEDPCos(+Inf) CCR overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOS, -INFINITY, CCR_V, true, "IEEEDPCos(-Inf) CCR overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTAN, 0.5, CCR_POS, true, "IEEEDPTan CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTAN, -0.5, CCR_N, true, "IEEEDPTan CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTAN, 0.0, CCR_Z, true, "IEEEDPTan CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTAN, INFINITY, CCR_V, true, "IEEEDPTan(+Inf) CCR overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTAN, -INFINITY, CCR_V, true, "IEEEDPTan(-Inf) CCR overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINCOS, 0.5, CCR_POS, true, "IEEEDPSincos CCR positive", 0x200u);
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINCOS, -0.5, CCR_N, true, "IEEEDPSincos CCR negative", 0x200u);
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINCOS, 0.0, CCR_Z, true, "IEEEDPSincos CCR zero", 0x200u);
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINCOS, INFINITY, CCR_V, true, "IEEEDPSincos(+Inf) CCR overflow", 0x200u);
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINCOS, -INFINITY, CCR_V, true, "IEEEDPSincos(-Inf) CCR overflow", 0x200u);

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINH, 0.5, CCR_POS, true, "IEEEDPSinh CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINH, -0.5, CCR_N, true, "IEEEDPSinh CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINH, 0.0, CCR_Z, true, "IEEEDPSinh CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINH, 1000.0, CCR_V, true, "IEEEDPSinh CCR positive overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSINH, -1000.0, CCR_N | CCR_V, true, "IEEEDPSinh CCR negative overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOSH, 0.0, CCR_POS, true, "IEEEDPCosh CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOSH, 1000.0, CCR_V, true, "IEEEDPCosh CCR overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPCOSH, -1000.0, CCR_V, true, "IEEEDPCosh CCR negative-arg overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTANH, 0.5, CCR_POS, true, "IEEEDPTanh CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTANH, -0.5, CCR_N, true, "IEEEDPTanh CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTANH, 0.0, CCR_Z, true, "IEEEDPTanh CCR zero");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPEXP, 0.0, CCR_POS, true, "IEEEDPExp CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPEXP, -1000.0, CCR_Z, true, "IEEEDPExp CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPEXP, 1000.0, CCR_V, true, "IEEEDPExp CCR overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG, 2.0, CCR_POS, true, "IEEEDPLog CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG, 0.5, CCR_N, true, "IEEEDPLog CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG, 1.0, CCR_Z, true, "IEEEDPLog CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG, 0.0, CCR_N, true, "IEEEDPLog(0.0) CCR negative without V");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG, -1.0, CCR_V, true, "IEEEDPLog CCR negative-arg overflow");

    check_ccr_d2(TRANS_BASE, LVO_IEEEDPPOW, 2.0, 3.0, CCR_POS, true, "IEEEDPPow CCR positive");
    check_ccr_d2(TRANS_BASE, LVO_IEEEDPPOW, -2.0, 3.0, CCR_N, true, "IEEEDPPow CCR negative");
    check_ccr_d2(TRANS_BASE, LVO_IEEEDPPOW, 0.0, 2.0, CCR_Z, true, "IEEEDPPow CCR zero");
    check_ccr_d2(TRANS_BASE, LVO_IEEEDPPOW, 10.0, 1000.0, CCR_V, true, "IEEEDPPow CCR positive overflow");
    check_ccr_d2(TRANS_BASE, LVO_IEEEDPPOW, -10.0, 1001.0, CCR_N | CCR_V, true, "IEEEDPPow CCR negative overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSQRT, 4.0, CCR_POS, true, "IEEEDPSqrt CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSQRT, 0.0, CCR_Z, true, "IEEEDPSqrt CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSQRT, -0.0, CCR_Z, true, "IEEEDPSqrt(-0.0) CCR zero without N");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPSQRT, -1.0, CCR_V, true, "IEEEDPSqrt CCR negative-arg overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTIEEE, 1.5, CCR_POS, false, "IEEEDPTieee CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTIEEE, -42.25, CCR_N, false, "IEEEDPTieee CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTIEEE, -0.0, CCR_Z, false, "IEEEDPTieee CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTIEEE, 1e300, CCR_V, false, "IEEEDPTieee CCR positive overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPTIEEE, -1e300, CCR_N | CCR_V, false, "IEEEDPTieee CCR negative overflow");

    check_ccr_u32(TRANS_BASE, LVO_IEEEDPFIEEE, float_to_bits(1.5f), CCR_POS, "IEEEDPFieee CCR positive");
    check_ccr_u32(TRANS_BASE, LVO_IEEEDPFIEEE, float_to_bits(-42.25f), CCR_N, "IEEEDPFieee CCR negative");
    check_ccr_u32(TRANS_BASE, LVO_IEEEDPFIEEE, float_to_bits(-0.0f), CCR_Z, "IEEEDPFieee CCR zero");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPASIN, 0.5, CCR_POS, true, "IEEEDPAsin CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPASIN, -0.5, CCR_N, true, "IEEEDPAsin CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPASIN, 0.0, CCR_Z, true, "IEEEDPAsin CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPASIN, 2.0, CCR_V, true, "IEEEDPAsin CCR out-of-range overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPASIN, -2.0, CCR_V, true, "IEEEDPAsin CCR negative out-of-range overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPACOS, 0.5, CCR_POS, true, "IEEEDPAcos CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPACOS, 1.0, CCR_Z, true, "IEEEDPAcos CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPACOS, 2.0, CCR_V, true, "IEEEDPAcos CCR out-of-range overflow");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPACOS, -2.0, CCR_V, true, "IEEEDPAcos CCR negative out-of-range overflow");

    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG10, 100.0, CCR_POS, true, "IEEEDPLog10 CCR positive");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG10, 0.1, CCR_N, true, "IEEEDPLog10 CCR negative");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG10, 1.0, CCR_Z, true, "IEEEDPLog10 CCR zero");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG10, 0.0, CCR_N, true, "IEEEDPLog10(0.0) CCR negative without V");
    check_ccr_d1(TRANS_BASE, LVO_IEEEDPLOG10, -1.0, CCR_V, true, "IEEEDPLog10 CCR negative-arg overflow");
}

int main()
{
    test_tables();
    test_dpfix();
    test_dpflt();
    test_dpcmp_dptst();
    test_dpbasic_arith();
    test_dpbasic_reference();
    test_dptrans_functions();
    test_dptieee_dpfieee();
    test_dpsincos();
    test_condition_codes();
    test_spfix_spflt();
    test_spcmp_sptst();
    test_spbasic_arith();
    test_spbasic_reference();
    std::printf("mathieeedoubbas/mathieeedoubtrans/mathieeesingbas thunks: %u/%u checks passed\n",
                checks - failures, checks);
    return failures ? 1 : 0;
}
