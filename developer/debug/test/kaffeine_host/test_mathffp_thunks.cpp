/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Production C thunks, CPU accessors and dispatcher; no native mathffp.
 * Literal LVOs and a poisoned fallback detect missing or misplaced entries.
 * Arithmetic expectations use a separate frexp/ldexp numerical oracle,
 * never the production integer normalization/alignment/division algorithm.
 * Unless explicitly identified as a measurement, expectations describe the
 * bridge's contract, not observations of AmigaOS.
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks_mathffp.h"

static unsigned checks, failures, poison_calls;
static unsigned calls_by_vector[12];
static constexpr UWORD N = 8, Z = 4, V = 2, NZV = 14;
static constexpr uint32_t SIGN = 0x80, ONE = 0x80000041, MAX = 0xffffff7f;

static void check(bool passed, const char *what, uint32_t got = 0, uint32_t want = 0)
{
    ++checks;
    if (!passed)
    {
        if (failures < 30)
            std::printf("FAIL: %s (got %08x, want %08x)\n", what, got, want);
        ++failures;
    }
}

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0;
    ULONG pc = 0x2468ace0;
};

static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static void execute(void *) { std::abort(); }
static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

static IPTR poison(KaffeineContext *, void *)
{
    ++poison_calls;
    return 0xbad0beef;
}

static const M68KThunkEntry manual_table[] = { KAFFEINE_MATHFFP_THUNKS };
static const M68KThunkEntry generated_table[] =
{
    {30, poison}, {36, poison}, {42, poison}, {48, poison},
    {54, poison}, {60, poison}, {66, poison}, {72, poison},
    {78, poison}, {84, poison}, {90, poison}, {96, poison}
};

struct Result
{
    uint32_t bits;
    UWORD ccr;
};

struct Fixture
{
    KaffeineContext ctx = {};
    std::array<UBYTE, 4096> memory;
    TestCPU state;
    KaffeineCPU cpu = {&cpu_ops, &state};
    const ULONG base;

    explicit Fixture(ULONG library_base = 0x1000, unsigned salt = 0) : base(library_base)
    {
        for (size_t i = 0; i < memory.size(); ++i)
            memory[i] = static_cast<UBYTE>((i * 37u + salt) ^ (i >> 4));
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = base;
        lib.jt_start = base - 96;
        std::strcpy(lib.name, "mathffp.library");
        lib.thunks = manual_table;
        lib.num_thunks = sizeof(manual_table) / sizeof(manual_table[0]);
        lib.gen_thunks = generated_table;
        lib.num_gen_thunks = sizeof(generated_table) / sizeof(generated_table[0]);
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = 0xd0d00100u + salt * 0x100u + i;
            state.a[i] = 0xa0a00000u + salt * 0x100u + i;
        }
    }

    Result call(ULONG lvo, uint32_t d0, uint32_t d1, UWORD sr)
    {
        state.d[0] = d0;
        state.d[1] = d1;
        state.a[7] = (sr & 1u) ? 0x182 : 0x100;
        state.sr = sr;
        m68k_write32(&ctx, state.a[7], 0x76543210);
        TestCPU before = state;
        auto before_memory = memory;
        std::array<unsigned char, sizeof(ctx)> before_context;
        std::memcpy(before_context.data(), &ctx, sizeof(ctx));
        unsigned before_poison = poison_calls;
        KaffeineDispatchResult dispatch = {};

        BOOL found = Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &dispatch);
        ++calls_by_vector[(lvo - 30) / 6];
        check(found && poison_calls == before_poison, "manual LVO overrides generated fallback", lvo);
        check(dispatch.return_pc == 0x76543210 && !dispatch.supervisor &&
              state.a[7] == before.a[7] + 4, "dispatcher pops exactly one return address", lvo);
        bool preserved = state.pc == before.pc;
        for (unsigned i = 1; i < 8; ++i)
            preserved = preserved && state.d[i] == before.d[i];
        for (unsigned i = 0; i < 7; ++i)
            preserved = preserved && state.a[i] == before.a[i];
        check(preserved, "D1-D7, A0-A6 and PC preserved", lvo);
        check((state.sr & ~NZV) == (sr & ~NZV), "all SR bits outside N/Z/V preserved", state.sr, sr);
        check(memory == before_memory, "guest memory including return stack preserved", lvo);
        check(std::memcmp(&ctx, before_context.data(), sizeof(ctx)) == 0,
              "context is unchanged", lvo);
        return {state.d[0], static_cast<UWORD>(state.sr & NZV)};
    }
};

static Fixture fixture;

static void expect(Fixture &f, ULONG lvo, uint32_t d0, uint32_t d1,
                   Result want, const char *what, UWORD sr)
{
    Result got = f.call(lvo, d0, d1, sr);
    check(got.bits == want.bits, what, got.bits, want.bits);
    check(got.ccr == want.ccr, what, got.ccr, want.ccr);
}

static void expect(ULONG lvo, uint32_t d0, uint32_t d1, uint32_t bits,
                   UWORD ccr, const char *what)
{
    for (UWORD sr : {static_cast<UWORD>(0xa700), static_cast<UWORD>(0x581f)})
        expect(fixture, lvo, d0, d1, {bits, ccr}, what, sr);
}

/* Independent oracle: all FFP values are exactly representable in double
   (and long double). Products have at most 48 significant bits. Division
   checks its numerical oracle's adjacent grid boundaries with exact cross
   products, including on macOS arm64 (53-bit long double precision). */
static long double decode(uint32_t bits)
{
    if (bits == 0)
        return 0;
    long double magnitude = std::ldexp(static_cast<long double>(bits >> 8),
                                       static_cast<int>(bits & 127u) - 88);
    return bits & SIGN ? -magnitude : magnitude;
}

static Result encode(long double value, bool ties_up = true)
{
    if (value == 0)
        return {0, Z};
    bool negative = value < 0;
    int power;
    long double fraction = std::frexp(std::fabs(value), &power);
    long double scaled = std::ldexp(fraction, 24);
    long double integral = std::floor(scaled);
    long double remainder = scaled - integral;
    uint32_t mantissa = static_cast<uint32_t>(integral);
    if (remainder > 0.5L || (ties_up && remainder == 0.5L))
        ++mantissa;
    if (mantissa == 0x1000000)
    {
        mantissa = 0x800000;
        ++power;
    }
    if (power < (negative ? -64 : -63))
        return {0, Z};
    if (power > 63)
        return {MAX | (negative ? SIGN : 0), static_cast<UWORD>(V | (negative ? N : 0))};
    return {(mantissa << 8) | static_cast<uint32_t>(power + 64) | (negative ? SIGN : 0),
            static_cast<UWORD>(negative ? N : 0)};
}

static Result fix_oracle(long double value)
{
    long double integer = std::trunc(value);
    if (integer >= 2147483648.0L)
        return {0x7fffffff, V};
    if (integer < -2147483648.0L)
        return {0x80000000, Z | V};
    int32_t result = static_cast<int32_t>(integer);
    return {static_cast<uint32_t>(result), static_cast<UWORD>(result == 0 ? Z : (result < 0 ? N : 0))};
}

static Result cmp_oracle(long double left, long double right)
{
    if (left == right)
        return {0, Z};
    return left > right ? Result{1, N} : Result{0xffffffff, 0};
}

static void test_table_and_routing()
{
    check(sizeof(manual_table) / sizeof(manual_table[0]) == 12,
          "shared table contains twelve vectors");
    for (unsigned i = 0; i < 12; ++i)
        check(manual_table[i].lvo == 30u + 6u * i && manual_table[i].thunk,
              "table offsets -30 through -96 match literal ABI", i);

    /* Prove the poison detects routing failures, rather than merely existing. */
    fixture.ctx.libs[0].num_thunks = 0;
    fixture.state.a[7] = 0x100;
    m68k_write32(&fixture.ctx, 0x100, 0x76543210);
    KaffeineDispatchResult dispatch = {};
    unsigned before = poison_calls;
    check(Kaffeine_DispatchLibraryCall(&fixture.ctx, &fixture.cpu, fixture.base - 66, &dispatch) &&
          poison_calls == before + 1 && fixture.state.d[0] == 0xbad0beef,
          "disabled manual table reaches poisoned generated fallback");
    fixture.ctx.libs[0].num_thunks = sizeof(manual_table) / sizeof(manual_table[0]);
}

static void test_literal_contracts()
{
    /* Inputs and expected outputs are raw FFP words, never IEEE words. */
    expect(30, 0, 0xdeadbeef, 0, Z, "SPFix zero");
    expect(30, 0xe0000041, 0xdeadbeef, 1, 0, "SPFix +1.75 truncates");
    expect(30, 0xe00000c1, 0xdeadbeef, 0xffffffff, N, "SPFix -1.75 truncates");
    expect(30, 0xffffff40, 0, 0, Z, "SPFix just below one");
    expect(30, 0xffffffc0, 0, 0, Z, "SPFix just above minus one");
    expect(30, 0xffffff5f, 0, 0x7fffff80, 0, "SPFix largest positive FFP inside LONG range");
    expect(30, 0xffffffdf, 0, 0x80000080, N, "SPFix negative inside LONG range");
    expect(30, 0x80000060, 0, 0x7fffffff, V, "SPFix +2^31 saturates");
    expect(30, 0x800000e0, 0, 0x80000000, N, "SPFix exact INT32_MIN");
    expect(30, 0x800001e0, 0, 0x80000000, Z | V, "SPFix below INT32_MIN saturates with observed Z|V");
    expect(30, MAX, 0, 0x7fffffff, V, "SPFix maximum positive");
    expect(30, MAX | SIGN, 0, 0x80000000, Z | V, "SPFix maximum negative");
    expect(30, 0x80000001, 0, 0, Z, "SPFix smallest positive normal");
    expect(30, 0x80000081, 0, 0, Z, "SPFix smallest negative normal");

    expect(36, 0, 0xdeadbeef, 0, Z, "SPFlt zero");
    expect(36, 1, 0xdeadbeef, ONE, 0, "SPFlt one");
    expect(36, 0xffffffff, 0, ONE | SIGN, N, "SPFlt minus one");
    expect(36, 0x80000000, 0, 0x800000e0, N, "SPFlt INT32_MIN without signed overflow");
    expect(36, 0x7fffffff, 0, 0x80000060, 0, "SPFlt INT32_MAX rounding carry and V clear");
    expect(36, 0x00ffffff, 0, 0xffffff58, 0, "SPFlt largest consecutive exact integer");
    expect(36, 0x01000001, 0, 0x80000159, 0, "SPFlt halfway magnitude rounds up");
    expect(36, 0xfeffffff, 0, 0x800001d9, N, "SPFlt negative halfway magnitude rounds up");
    expect(36, 0x01000003, 0, 0x80000259, 0, "SPFlt second halfway case");

    expect(42, 0x80000042, ONE, 0xffffffff, 0, "SPCmp D1=1 less than D0=2; CCR compares D0 to D1");
    expect(42, ONE, 0x80000042, 1, N, "SPCmp reversed operands");
    expect(42, 0xc00000c2, 0x800000c2, 1, N, "SPCmp -2 greater than -3");
    expect(42, 0x800000c2, 0xc00000c2, 0xffffffff, 0, "SPCmp -3 less than -2");
    expect(42, ONE, ONE, 0, Z, "SPCmp equality");
    expect(42, 0, 0, 0, Z, "SPCmp zero equality");
    expect(42, 0, ONE | SIGN, 0xffffffff, 0, "SPCmp negative vs zero");
    expect(42, 0, ONE, 1, N, "SPCmp positive vs zero");
    expect(48, 0xffffffff, 0, 0, Z, "SPTst reads zero in D1 despite poisoned D0");
    expect(48, 0, ONE, 1, 0, "SPTst reads positive D1 despite zero D0");
    expect(48, ONE, ONE | SIGN, 0xffffffff, N, "SPTst reads negative D1");

    expect(54, ONE | SIGN, 0xdeadbeef, ONE, 0, "SPAbs clears sign bit 7");
    expect(54, 0, 0xdeadbeef, 0, Z, "SPAbs zero");
    expect(54, MAX | SIGN, 0, MAX, 0, "SPAbs largest negative");
    expect(60, ONE, 0, ONE | SIGN, N, "SPNeg positive");
    expect(60, ONE | SIGN, 0, ONE, 0, "SPNeg negative");
    expect(60, 0, 0, 0, Z, "SPNeg preserves canonical zero");

    expect(66, 0xc0000041, 0xa0000042, 0x80000043, 0, "SPAdd 1.5+2.5=4");
    expect(66, ONE | SIGN, ONE, 0, Z, "SPAdd exact cancellation");
    expect(66, ONE, 0x80000029, ONE, 0, "SPAdd measured half ULP is ignored");
    expect(66, ONE, 0xc0000029, ONE, 0, "SPAdd measured 0.75 ULP is ignored");
    expect(66, 0x80000141, 0x800000a9, 0x80000141, 0, "SPAdd measured opposite-sign half ULP is ignored");
    expect(66, 0xffffff40, 0x80000028, 0xffffff40, 0, "SPAdd measured halfway binade boundary truncates");
    expect(66, 0xffffff41, ONE, 0xbfffff42, 0, "SPAdd measured renormalized sum truncates");
    expect(66, 0xc0000041, 0x80000140, 0x80000042, 0, "SPAdd measured gap-one sum truncates");
    expect(66, 0x80000141, 0x800001c0, 0x80000040, 0,
           "SPAdd measured opposite-sign alignment rounds before cancellation");
    expect(66, 0x80000141, 0xffffffc0, 0x8000002a, 0,
           "SPAdd measured cancellation cannot recover discarded alignment bits");
    expect(66, 0x80000141, 0x800001bf, 0xc0000240, 0,
           "SPAdd measured gap-two cancellation rounds aligned operand down");
    expect(66, 0x80000141, 0xc00000aa, 0xfffffe40, 0,
           "SPAdd measured gap-23 cancellation rounds aligned halfway up");
    expect(66, 0x80000101, 0x80000081, 0, Z, "SPAdd cancellation underflows");
    expect(66, MAX, MAX, MAX, V, "SPAdd positive overflow saturates");
    expect(66, MAX | SIGN, MAX | SIGN, MAX | SIGN, N | V, "SPAdd negative overflow saturates");
    expect(72, 0xa0000043, 0x80000042, 0xc0000042, 0, "SPSub D0=5 minus D1=2 is 3");
    expect(72, 0x80000042, 0xa0000043, 0xc00000c2, N, "SPSub D0=2 minus D1=5 is -3");
    expect(72, ONE, 0, ONE, 0, "SPSub zero subtrahend stays canonical");
    expect(72, 0, ONE, ONE | SIGN, N, "SPSub zero minuend");
    expect(72, MAX, MAX | SIGN, MAX, V, "SPSub overflow");
    expect(78, 0xc0000042, 0x800000c2, 0xc00000c3, N, "SPMul 3*-2=-6");
    expect(78, 0, MAX | SIGN, 0, Z, "SPMul zero");
    expect(78, 0xc0000041, 0x80000341, 0xc0000441, 0, "SPMul measured even-lower tie truncates");
    expect(78, 0xc0000041, 0x80000141, 0xc0000141, 0, "SPMul measured odd-lower tie truncates");
    expect(78, MAX, ONE, MAX, 0, "SPMul maximum times one is finite");
    expect(78, MAX, 0x80000042, MAX, V, "SPMul overflow");
    expect(78, 0x80000001, 0x80000040, 0, Z, "SPMul underflow flushes to zero");
    expect(78, 0xa9e47412, 0x87e33caf, 0xb45c8d80, N,
           "SPMul measured negative exponent-zero output remains representable");
    expect(78, 0x800000a0, 0x80000021, 0x80000080, N,
           "SPMul measured smallest representable negative output");
    expect(78, 0xe2235c41, 0xcff59841, 0xb7b38942, 0,
           "SPMul measured fixed-point guard-byte 80 rounds down");
    expect(78, 0xb3ae5741, 0x80040f41, 0xb3b40941, 0,
           "SPMul measured left normalization discards guard-bit tail");
    expect(84, 0xc0000043, 0x80000042, 0xc0000042, 0, "SPDiv D0=6 divided by D1=2 is 3");
    expect(84, 0x80000042, 0xc0000043, 0xaaaaab3f, 0, "SPDiv D0=2 divided by D1=6 is one third");
    expect(84, ONE | SIGN, 0xc0000042, 0xaaaaabbf, N, "SPDiv -1/3 rounds");
    expect(84, 0, ONE | SIGN, 0, Z, "SPDiv zero dividend");
    expect(84, MAX, ONE, MAX, 0, "SPDiv maximum divided by one");
    expect(84, 0x80000001, ONE, 0x80000001, 0, "SPDiv minimum divided by one");
    expect(84, 0x80000001, 0x80000042, 0, Z, "SPDiv underflow");
    expect(84, MAX, 0x80000040, MAX, V, "SPDiv overflow");
    expect(84, ONE, 0, MAX, V, "SPDiv positive divided by zero policy");
    expect(84, ONE | SIGN, 0, MAX | SIGN, N | V, "SPDiv negative divided by zero policy");
    expect(84, 0, 0, MAX, V, "SPDiv measured zero divided by zero saturates");
    expect(84, 0xe2eeddc6, 0xc72cc19b, 0x91d6c06c, 0,
           "SPDiv measured eight-bit guard midpoint discards quotient tail");

    expect(90, 0xc0000041, 0, ONE, 0, "SPFloor +1.5");
    expect(90, 0xc00000c1, 0, 0x800000c2, N, "SPFloor -1.5");
    expect(96, 0xc0000041, 0, 0x80000042, 0, "SPCeil +1.5");
    expect(96, 0xc00000c1, 0, ONE | SIGN, N, "SPCeil -1.5");
    expect(90, 0xffffffc0, 0, ONE | SIGN, N, "SPFloor just above -1");
    expect(96, 0xffffff40, 0, ONE, 0, "SPCeil just below 1");
    expect(90, 0xffffff41, 0, ONE, 0, "SPFloor just below 2");
    expect(96, 0xffffff41, 0, 0x80000042, 0, "SPCeil carries into exponent");
    expect(90, 0x80000001, 0, 0, Z, "SPFloor smallest positive normal");
    expect(90, 0x80000081, 0, ONE | SIGN, N, "SPFloor smallest negative normal");
    expect(96, 0x80000001, 0, ONE, 0, "SPCeil smallest positive normal");
    expect(96, 0x80000081, 0, 0, Z, "SPCeil smallest negative normal");
    for (ULONG lvo : {90u, 96u})
    {
        expect(lvo, 0, 0, 0, Z, "floor/ceil canonical zero");
        expect(lvo, MAX, 0, MAX, 0, "floor/ceil largest FFP does not pass through LONG");
        expect(lvo, MAX | SIGN, 0, MAX | SIGN, N, "floor/ceil largest negative FFP");
        expect(lvo, ONE, 0, ONE, 0, "floor/ceil exact one");
        expect(lvo, ONE | SIGN, 0, ONE | SIGN, N, "floor/ceil exact minus one");
    }
}

static Result add_oracle(long double x, long double y)
{
    if (x != 0 && y != 0)
    {
        int px, py;
        std::frexp(std::fabs(x), &px);
        std::frexp(std::fabs(y), &py);
        if (std::abs(px - py) >= 24)
            return encode(px > py ? x : y, false);
        if ((x < 0) != (y < 0))
        {
            /* Quantize the smaller magnitude on the larger operand's
               mantissa grid, using host numerical operations instead of
               the production integer shifts and mantissa subtraction. */
            int grid = 24 - (px > py ? px : py);
            long double smaller = px > py ? y : x;
            long double aligned = std::ldexp(std::floor(std::ldexp(std::fabs(smaller), grid) + 0.5L), -grid);
            aligned = std::copysign(aligned, smaller);
            return encode(px > py ? x + aligned : y + aligned, false);
        }
    }
    return encode(x + y, false);
}

static Result div_oracle(long double x, long double y)
{
    long double quotient = x / y;
    if (quotient == 0)
        return {0, Z};
    int power;
    long double fraction = std::frexp(std::fabs(quotient), &power);
    uint64_t grid_value = static_cast<uint64_t>(std::floor(std::ldexp(fraction, 32)));
    /* A quotient just below an integer grid boundary can round onto it in
       a 53-bit host long double. Correct that numerical-floor ambiguity
       using exact cross products, not the production integer division. */
    int px, py;
    uint64_t mx = static_cast<uint64_t>(std::ldexp(std::frexp(std::fabs(x), &px), 24));
    uint64_t my = static_cast<uint64_t>(std::ldexp(std::frexp(std::fabs(y), &py), 24));
    uint64_t numerator = mx << static_cast<unsigned>(px - py - power + 32);
    if (grid_value * my > numerator)
        --grid_value;
    else if ((grid_value + 1) * my <= numerator)
        ++grid_value;
    long double retained = std::ldexp(static_cast<long double>(grid_value), power - 32);
    return encode(std::copysign(retained, quotient), false);
}

static Result mul_oracle(long double x, long double y)
{
    int px, py;
    long double fx = std::frexp(std::fabs(x), &px);
    long double fy = std::frexp(std::fabs(y), &py);
    /* The product of the two fixed-point fractions is truncated on a
       2^-32 grid BEFORE its leading bit is normalized. The numerical
       product is exact here (at most 48 significant bits). */
    long double grid = std::floor(std::ldexp(fx * fy, 32));
    long double retained = std::ldexp(grid, px + py - 32);
    return encode(std::copysign(retained, x * y), false);
}

static void numerical_case(Fixture &f, uint32_t a, uint32_t b, UWORD sr)
{
    long double x = decode(a), y = decode(b);
    expect(f, 66, a, b, add_oracle(x, y), "SPAdd numerical oracle", sr);
    expect(f, 72, a, b, add_oracle(x, -y), "SPSub numerical oracle", sr);
    expect(f, 78, a, b, mul_oracle(x, y), "SPMul numerical oracle", sr);
    if (b != 0)
        expect(f, 84, a, b, div_oracle(x, y), "SPDiv numerical oracle", sr);
    expect(f, 42, a, b, cmp_oracle(y, x), "SPCmp numerical oracle", sr);
    expect(f, 48, a, b,
           {y == 0 ? 0u : (y < 0 ? 0xffffffffu : 1u),
            static_cast<UWORD>(y == 0 ? Z : (y < 0 ? N : 0))}, "SPTst numerical oracle", sr);
    expect(f, 30, a, b, fix_oracle(x), "SPFix numerical oracle", sr);
    expect(f, 54, a, b, encode(std::fabs(x)), "SPAbs numerical oracle", sr);
    expect(f, 60, a, b, encode(-x), "SPNeg numerical oracle", sr);
    expect(f, 90, a, b, encode(std::floor(x)), "SPFloor numerical oracle", sr);
    expect(f, 96, a, b, encode(std::ceil(x)), "SPCeil numerical oracle", sr);
}

static uint32_t random_word(uint32_t &state)
{
    /* Fixed seed and fixed-width operations give identical inputs on GCC,
       clang, little/big endian and signed/unsigned plain-char hosts. */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static uint32_t random_ffp(uint32_t &state)
{
    uint32_t r = random_word(state);
    return (r & 31u) == 0 ? 0 :
        ((random_word(state) | 0x80000000u) & 0xffffff00u) |
        (r & SIGN) | (1u + ((r >> 8) % 127u));
}

static void test_boundary_grid()
{
    const uint32_t words[] =
    {
        0, 0x80000001, 0xffffff01, 0x80000020, 0x80000028, 0x80000029,
        0x8000003f, 0x80000040, 0xffffff40, ONE, 0x80000141, 0x80000241,
        0xc0000041, 0xffffff41, 0x80000042, 0xffffff58, 0x80000059,
        0xffffff5f, 0x80000060, 0x80000160, 0x80000061, 0x8000007f, MAX
    };
    for (uint32_t a : words)
        for (uint32_t b : words)
            for (uint32_t signs = 0; signs < 4; ++signs)
                numerical_case(fixture, a == 0 ? 0 : a | ((signs & 1u) ? SIGN : 0),
                               b == 0 ? 0 : b | ((signs & 2u) ? SIGN : 0),
                               (signs & 1u) ? 0x581f : 0xa700);

    /* Sweep every exponent with extremes and values on either side of the
       24-bit mantissa rounding boundary. Exponent zero is intentionally not
       assigned a canonical-input guarantee by these tests. */
    for (unsigned e = 1; e <= 127; ++e)
        for (uint32_t mantissa : {0x800000u, 0x800001u, 0xbfffffu, 0xc00000u, 0xfffffeu, 0xffffffu})
            for (uint32_t sign : {0u, SIGN})
            {
                uint32_t a = (mantissa << 8) | e | sign;
                numerical_case(fixture, a, ONE, 0x581f);
                numerical_case(fixture, a, a ^ SIGN, 0xa700);
            }
}

static void test_deterministic_arithmetic()
{
    uint32_t seed = 0x4b464650;
    Fixture other(0x3000, 91);
    for (unsigned i = 0; i < 6000; ++i)
    {
        uint32_t a = random_ffp(seed), b = random_ffp(seed);
        numerical_case((i & 1u) ? other : fixture, a, b, (i & 2u) ? 0x581f : 0xa700);
        uint32_t integer = random_word(seed);
        int64_t signed_integer = (integer & 0x80000000u) ?
            static_cast<int64_t>(integer) - 4294967296LL : integer;
        expect(fixture, 36, integer, b, encode(static_cast<long double>(signed_integer)),
               "SPFlt deterministic integer oracle", (i & 1u) ? 0xa700 : 0x581f);
    }
    /* Also walk rounding transitions at each integer exponent, both signs. */
    for (unsigned e = 0; e < 31; ++e)
        for (int delta = -4; delta <= 4; ++delta)
        {
            int64_t value = (INT64_C(1) << e) + delta;
            for (int sign : {-1, 1})
            {
                int64_t integer = value * sign;
                if (integer < INT32_MIN || integer > INT32_MAX)
                    continue;
                expect(fixture, 36, static_cast<uint32_t>(integer), 0xfeedface,
                       encode(static_cast<long double>(integer)), "SPFlt rounding boundary", 0x581f);
            }
        }
}

static void test_repeated_calls_and_contexts()
{
    Fixture a(0x4000, 12), b(0x6000, 34);
    Result value = {ONE, 0};
    for (unsigned i = 1; i <= 128; ++i)
    {
        /* Carry the preceding output and CCR into the next real call. */
        value = a.call(66, value.bits, ONE, a.state.sr);
        Result want = encode(static_cast<long double>(i + 1));
        check(value.bits == want.bits && value.ccr == want.ccr, "repeated accumulation");
        std::array<unsigned char, sizeof(a.state)> saved_cpu;
        std::memcpy(saved_cpu.data(), &a.state, sizeof(a.state));
        auto saved_memory = a.memory;
        expect(b, 78, ONE | SIGN, ONE, {ONE | SIGN, N}, "independent second context", 0x581f);
        check(std::memcmp(saved_cpu.data(), &a.state, sizeof(a.state)) == 0 && a.memory == saved_memory,
              "call on second context leaves first CPU and arena unchanged");
        expect(a, 72, ONE, ONE, {0, Z}, "positive to zero CCR transition", a.state.sr);
        expect(a, 60, ONE, 0, {ONE | SIGN, N}, "zero to negative CCR transition", a.state.sr);
        expect(a, 54, ONE | SIGN, 0, {ONE, 0}, "negative to positive CCR transition", a.state.sr);
    }
    expect(a, 66, MAX, MAX, {MAX, V}, "overflow in first context", 0x581f);
    expect(b, 66, ONE, 0, {ONE, 0}, "overflow CCR does not leak to another context", 0xa700);
    expect(a, 66, ONE, 0, {ONE, 0}, "subsequent normal call clears overflow", a.state.sr);
}

static void test_division_native_boundaries()
{
    /* Supervisor's fourth AmigaOS 3.2.3 / mathffp 46.1 probe, 26 Oct 2026.
       These are explicit, known native differences, not exact matches.
       The bound applies only to mantissa magnitude; sign, exponent, CCR,
       registers, memory and dispatcher routing are still checked exactly.
       --reference never uses this tolerance and returns failure on them. */
    const uint32_t rows[][3] =
    {
        {0xc14f7641, 0x8309d641, 0xbcd3fc41},
        {0xe5461741, 0xa891ac41, 0xae185941},
        {0x91c65a41, 0xa664c341, 0xe0470e40},
        {0xc20ab941, 0xb5f7b441, 0x887e4a41},
        {0xd25b7141, 0x8896ab41, 0xc5215141},
        {0xe5de4441, 0xff0c3541, 0xe6b9fd40},
        {0xe5372341, 0xaa639741, 0xac311041},
        {0xfaaffa41, 0x8643a441, 0xeefdc941}
    };
    unsigned differences = 0;
    for (const auto &row : rows)
        for (UWORD sr : {static_cast<UWORD>(0xa700), static_cast<UWORD>(0x581f)})
        {
            Result got = fixture.call(84, row[0], row[1], sr);
            uint32_t gm = got.bits >> 8, wm = row[2] >> 8;
            uint32_t distance = gm >= wm ? gm - wm : wm - gm;
            check((got.bits & 255u) == (row[2] & 255u) && distance <= 1,
                  "known native SPDiv boundary stays within one mantissa ULP", got.bits, row[2]);
            check(got.ccr == 0, "native SPDiv boundary CCR matches exactly", got.ccr);
            differences += got.bits != row[2];
        }
    std::printf("SPDiv native guard boundaries: %u/16 calls differ from native by one mantissa ULP\n",
                differences);
}

static void test_negative_exponent_zero()
{
    /* Supervisor's fifth AmigaOS 3.2.3 probe: these valid negative lower
       bounds expose legacy low-byte classification in the sign/rounding
       vectors. Keep their raw returned words and flags, not ideal floor. */
    for (uint32_t bits : {0x80000080u, 0xc0000080u, 0xffffff80u})
        for (ULONG lvo : {54u, 60u, 90u, 96u})
            expect(lvo, bits, 0, bits & ~SIGN, Z, "negative exponent-zero sign/round result");
    expect(48, 0x12345678, 0x80000000u, 0, Z, "SPTst positive exponent-zero word");
    expect(84, 0x80000080u, ONE, 0, Z, "SPDiv negative exponent-zero dividend");
}

/* Optional replay of a supervisor-supplied probe log. It checks returned D0
   and N/Z/V; native D1 and X/C are deliberately not treated as preserved.
   The default suite contains no dependency on this external evidence. */
static void test_reference(const char *path)
{
    FILE *file = std::fopen(path, "r");
    if (!file)
    {
        std::perror(path);
        std::exit(2);
    }
    char line[1024], name[128];
    unsigned rows = 0;
    while (std::fgets(line, sizeof(line), file))
    {
        int lvo;
        unsigned d0, d1, sr, out0, out1, ccr;
        if (std::sscanf(line, "V %127s %d %x %x %x -> %x %x %x",
                        name, &lvo, &d0, &d1, &sr, &out0, &out1, &ccr) != 8)
            continue;
        if (lvo > -30 || lvo < -96 || (-lvo - 30) % 6 != 0)
        {
            check(false, "reference contains an invalid mathffp LVO");
            continue;
        }
        expect(fixture, static_cast<ULONG>(-lvo), d0, d1,
               {out0, static_cast<UWORD>(ccr & NZV)}, name, static_cast<UWORD>(0xa700 | sr));
        ++rows;
    }
    check(!std::ferror(file) && rows != 0, "reference rows read successfully", rows);
    std::fclose(file);
    std::printf("Reference replay: %u rows from %s\n", rows, path);
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--reference") == 0)
        test_reference(argv[2]);
    else if (argc != 1)
    {
        std::fprintf(stderr, "Usage: %s [--reference supervisor-probe.txt]\n", argv[0]);
        return 2;
    }
    else
    {
        test_table_and_routing();
        test_literal_contracts();
        test_boundary_grid();
        test_deterministic_arithmetic();
        test_repeated_calls_and_contexts();
        test_division_native_boundaries();
        test_negative_exponent_zero();
        for (unsigned i = 0; i < 12; ++i)
            check(calls_by_vector[i] != 0, "each of twelve literal LVOs was executed", i);
    }
    std::printf("mathffp thunks: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
