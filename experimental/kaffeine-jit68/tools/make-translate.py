"""Author: Michele Dipace (@kaffeine1)

Makes engine/kj68_translate.c from the pinned legacy translator
rom/kaffeine/kaffeine_jit.c and writes the reuse map: every top-level
definition of the original is kept unchanged, changed or removed, and every
new one is named. Only the line ranges listed here are replaced; each range
is checked by its first and last line. Nothing is compiled or run.

usage: python3 -I -B make-translate.py ORIGINAL LEGACY_JIT_H OUT_C OUT_MAP
"""
import hashlib, json, re, sys
from pathlib import Path

ORIG_SHA = '65b150bb813aa236af66e49da02867595642cb15faf920702b8f95167edee73b'
JIT_H_SHA = '3e7972eaeea50a3fe9bf25022762d2cd272b7d0a1ccae3f663849203eb122af1'
src_path, h_path, out_c, out_map = (Path(a) for a in sys.argv[1:5])
data = src_path.read_bytes()
assert hashlib.sha256(data).hexdigest() == ORIG_SHA
hdata = h_path.read_bytes()
assert hashlib.sha256(hdata).hexdigest() == JIT_H_SHA
orig = data.decode('ascii').split('\n')
hlines = hdata.decode('ascii').split('\n')
assert len(orig) == 3797 and orig[-1] == ''          # 3796 lines and the final newline

# the exit enum and HJStats of kaffeine_jit.h, lines 16-42, moved here verbatim
assert hlines[15].startswith('/* Exit reasons of translated code') and hlines[41] == '};'
EXITS_AND_STATS = '\n'.join(hlines[15:42])

R = {}   # first line (1-based) -> (last line, expected first, expected last prefix, new text)


def region(first, last, first_text, last_prefix, new):
    assert orig[first - 1] == first_text, (first, orig[first - 1])
    assert orig[last - 1].startswith(last_prefix), (last, orig[last - 1])
    R[first] = (last, new)


region(5, 9, '    Experimental AArch64 translation backend for kaffeine, behind struct', '    written for this experiment', r'''    kaffeine-jit68 engine: the AArch64 translator of the legacy
    kaffeine.library (rom/kaffeine/kaffeine_jit.c, sha256 65b150bb), adapted
    to interface version 1 (kaffeine-jit68-abi-v1.h): guest memory through
    the host's map, exits to the host where the legacy library dispatched,
    translations shared by the CPUs of a domain. The instruction decoding
    and emission are those of the legacy file. It contains no Emu68 code.''')

region(24, 33, '#include <aros/debug.h>', '#include "kaffeine_jit.h"', '#include "kj68_intern.h"')

region(62, 64, '/* The program\'s return address, as in kaffeine_moira.cpp. */', '', None)

region(404, 440, 'struct HJState', '#endif', r'''struct HJState
{
    uint32_t d[8];              /*   0 */
    uint32_t a[8];              /*  32 */
    uint32_t pc;                /*  64 */
    uint32_t flags;             /*  68 */
    uint32_t sr_high;           /*  72: SR bits 15..8 */
    uint32_t abort;             /*  76 */
    uint32_t limit;             /*  80 */
    uint32_t slice_budget;      /*  84 */
    UBYTE *mem;                 /*  88 */
    void *fast;                 /*  96 */
    void *slow;                 /* 104 */
    struct HJCpu *backend;      /* 112 */
    uint64_t reserved;          /* 120 */
    UBYTE pages[];              /* 128: KJ68_PAGE_* of each 4 KiB guest page */
};

#define ST_PC     64
#define ST_FLAGS  68
#define ST_SRHI   72
#define ST_ABORT  76
#define ST_LIMIT  80
#define ST_BUDGET 84
#define ST_MEM    88
#define ST_FAST   96
#define ST_SLOW  104
#define ST_PAGES 128

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(offsetof(struct HJState, pc) == ST_PC, "state layout");
_Static_assert(offsetof(struct HJState, flags) == ST_FLAGS, "state layout");
_Static_assert(offsetof(struct HJState, sr_high) == ST_SRHI, "state layout");
_Static_assert(offsetof(struct HJState, abort) == ST_ABORT, "state layout");
_Static_assert(offsetof(struct HJState, limit) == ST_LIMIT, "state layout");
_Static_assert(offsetof(struct HJState, slice_budget) == ST_BUDGET, "state layout");
_Static_assert(offsetof(struct HJState, mem) == ST_MEM, "state layout");
_Static_assert(offsetof(struct HJState, fast) == ST_FAST, "state layout");
_Static_assert(offsetof(struct HJState, slow) == ST_SLOW, "state layout");
_Static_assert(offsetof(struct HJState, pages) == ST_PAGES, "state layout");
#endif''')

# The slow-path kinds gain the two divisor probes of emit_div_probe().
region(442, 443, '/* Accesses through the C slow path. */', 'enum { SLOW_R8, SLOW_R16, SLOW_R32, SLOW_W8, SLOW_W16, SLOW_W32, SLOW_KINDS };',
       r'''/* Accesses through the C slow path, and the probes of a division's
   divisor: no access, only whether the read would reach the host. */
enum { SLOW_R8, SLOW_R16, SLOW_R32, SLOW_W8, SLOW_W16, SLOW_W32, SLOW_P16, SLOW_P32, SLOW_KINDS };''')

region(457, 497, 'struct HJFast', '};', r'''struct HJFast
{
    uint64_t pc;
    uint64_t entry;
};

struct HJBlock
{
    struct HJBlock *next;   /* bucket chain */
    uint32_t pc, end;       /* guest bytes [pc, end) */
    uint32_t epoch;
    uint32_t words;
    const uint32_t *code;
    const UBYTE *guest;     /* the guest bytes when translated */
    const uint16_t *starts; /* the offset from pc of each instruction */
    uint16_t ninsns;        /* instructions in the block */
};

''' + EXITS_AND_STATS + r'''

_Static_assert(HJ_MAX_INSNS == KJ68_BLOCK_INSNS, "max_block_insns");

/* The translator state of a domain: code memory, glue, blocks and their
   guest bytes, shared by every CPU of the domain. The host makes one call
   on a domain at a time, so none of it needs a lock. */
struct HJBackend
{
    struct KJ68Domain *dom;
    uint32_t *code;
    size_t code_words, code_used, glue_words;
    uintptr_t g_enter, g_exit, g_lookup, g_budget, g_miss, g_slow[SLOW_KINDS];
    uint32_t (*enter)(struct HJState *, const uint32_t *);
    struct HJBlock *buckets[HJ_BUCKETS];
    struct HJBlock *blocks;
    unsigned nblocks;
    UBYTE *guest_bytes;
    size_t guest_used;
    uint32_t epoch;
    uint32_t fast_generation;   /* a CPU empties its fast table when it changes */
    uint32_t *scratch;
    struct HJTrans *trans;
    struct HJStats stats;
};

/* The translator state of a CPU: its registers and page flags, which
   translated code reads through R_STATE, and its fast table. */
struct HJCpu
{
    struct HJState *st;
    size_t st_bytes;
    struct KJ68CPU *owner;
    struct HJBackend *b;
    struct HJFast *fast;
    uint32_t fast_generation, pages_generation;
    int declined;               /* a divisor probe found a host read */
};''')

region(559, 567, 'struct HJInsn', '};', r'''struct HJInsn
{
    uint32_t pc, next, target;
    uint16_t opcode, mask;          /* MOVEM: registers in D0..A7 bit order */
    uint8_t kind, op, size, cond, count;
    uint8_t def, use, need;         /* HJF_* */
    struct HJEA src, dst;
    const char *why;                /* K_FAULT */
    uint32_t bad_at;                /* "fetch outside guest memory": the word */
};''')

region(569, 585, 'struct HJDec', '}', r'''struct HJDec
{
    const struct KJ68Domain *dom;
    uint32_t at;
    int bad;
    uint32_t bad_at;
};

/* Instruction fetch is always direct: from guest memory and never from a
   READ_HOST range (contract, section 4). */
static int kj_fetchable(const struct KJ68Domain *dom, uint32_t at)
{
    ULONG i;

    if (at > dom->size - 2)
        return 0;
    for (i = 0; i < dom->num_ranges; i++)
        if ((dom->ranges[i].flags & KJ68_RANGE_READ_HOST) &&
            at + 2 > dom->ranges[i].lo && at < dom->ranges[i].hi)
            return 0;
    return 1;
}

static uint16_t fetch16(struct HJDec *d)
{
    uint16_t v = 0;

    if (!kj_fetchable(d->dom, d->at))
    {
        if (!d->bad)
            d->bad_at = d->at;
        d->bad = 1;
    }
    else
        v = (uint16_t)(d->dom->base[d->at] << 8 | d->dom->base[d->at + 1]);
    d->at += 2;
    return v;
}''')

region(818, 820, 'static int decode_insn(struct KaffeineContext *ctx, uint32_t pc, struct HJInsn *in)',
       '    struct HJDec d = { ctx, pc, 0 };', r'''static int decode_insn(const struct KJ68Domain *dom, uint32_t pc, struct HJInsn *in)
{
    struct HJDec d = { dom, pc, 0, 0 };''')

region(1140, 1141, '    if (d.bad)', '        ok = fault_insn(in, "fetch outside guest memory");', r'''    if (d.bad)
    {
        ok = fault_insn(in, "fetch outside guest memory");
        in->bad_at = d.bad_at;
    }''')

region(1152, 1159, 'static void decode_backend(struct KaffeineContext *ctx, uint32_t pc,', '}', r'''static void decode_backend(const struct KJ68Domain *dom, uint32_t pc,
                           struct HJInsn *in, int library_mode)
{
    decode_insn(dom, pc, in);
    if (library_mode && (in->kind == K_MOVETOSR ||
        (in->kind == K_LOGICSR && in->size == 2)))
        fault_insn(in, "full SR write is outside the library JIT model");
}''')

region(1185, 1190, 'struct HJStub', '};', r'''#define HJ_NO_BRANCH (~0u)

struct HJStub
{
    unsigned br1, br2, br3, back;   /* br3: HJ_NO_BRANCH when there is none */
    uint8_t kind, areg;
    uint32_t pc;
};''')

region(1235, 1302, "/* One guest access through host register 'areg'. Loads leave the value in", '}', r'''/* One guest access through host register 'areg'. Loads leave the value in
   w17; stores take it from w17. The access is made inline only when it
   lies at or below mem_size - 4, inside one 4 KiB page, on a page whose
   flags in the state (taken from the domain's map at every run) leave this
   kind of access direct. Every other access goes to an out-of-line call of
   the C slow path, which applies the contract's rule exactly: ranges to the
   byte, longwords as two word accesses. */
static void emit_access(struct HJTrans *t, int kind, int areg)
{
    struct HJEmit *e = &t->e;
    int store = kind >= SLOW_W8;
    int size = (kind == SLOW_R8 || kind == SLOW_W8) ? 0 : (kind == SLOW_R16 || kind == SLOW_W16) ? 1 : 2;
    int scr = store ? R_SCR : R_VAL;
    struct HJStub *s;

    if (t->nstub >= HJ_MAX_STUBS)
    {
        e->bad = 1;
        return;
    }
    s = &t->stub[t->nstub++];
    s->br3 = HJ_NO_BRANCH;
    put(e, a64_cmp(areg, R_LIMIT));
    s->br1 = e->n;
    put(e, a64_bcond(A64_HI, 0));
    if (size)
    {
        /* a word or long that ends in the next page */
        put(e, a64_logic_const(0, scr, areg, 0xFFFu));
        put(e, a64_addsub_imm(0, 1, 1, ZR, scr, 0x1000u - (1u << size), 0));
        s->br3 = e->n;
        put(e, a64_bcond(A64_HI, 0));
    }
    put(e, a64_lsr(scr, areg, KJ68_PAGE_SHIFT));
    put(e, a64_addsub_reg(1, 0, 0, scr, R_STATE, scr, 0, 0));
    put(e, a64_ldst_uimm(0, 1, scr, scr, ST_PAGES));
    s->br2 = e->n;
    put(e, a64_tbz(1, scr, store ? 1 : 0, 0));
    if (!store)
    {
        put(e, a64_ldst_reg(size, 1, R_VAL, R_MEM, areg));
        if (size == 2)
            put(e, a64_rev32(R_VAL, R_VAL));
        else if (size == 1)
            put(e, a64_rev16(R_VAL, R_VAL));
    }
    else if (size == 0)
        put(e, a64_ldst_reg(0, 0, R_VAL, R_MEM, areg));
    else
    {
        put(e, size == 2 ? a64_rev32(R_SCR, R_VAL) : a64_rev16(R_SCR, R_VAL));
        put(e, a64_ldst_reg(size, 0, R_SCR, R_MEM, areg));
    }
    s->back = e->n;
    s->kind = (uint8_t)kind;
    s->areg = (uint8_t)areg;
    s->pc = t->in->pc;
}''')

region(1304, 1321, 'static void emit_stubs(struct HJTrans *t)', '}', r'''static void emit_stubs(struct HJTrans *t)
{
    struct HJEmit *e = &t->e;
    unsigned i;

    for (i = 0; i < t->nstub; i++)
    {
        const struct HJStub *s = &t->stub[i];

        patch(e, s->br1, e->n);
        patch(e, s->br2, e->n);
        if (s->br3 != HJ_NO_BRANCH)
            patch(e, s->br3, e->n);
        if (s->areg != R_ADDR)
            put(e, a64_mov(R_ADDR, s->areg));
        emit_mov32(e, R_F2, s->pc);
        put(e, a64_bl(words_to(e, t->g_slow[s->kind])));
        put(e, a64_b((int32_t)s->back - (int32_t)e->n));
    }
}''')

# A zero divisor: the fault exit leaves the instruction not done, so the
# source's postincrement or predecrement made by emit_read() is undone on
# that path; the nonzero path is emitted as before.
region(1804, 1804, '', '', r'''
/* A zero divisor ends the block at the instruction with nothing of it
   done (STOP_EXCEPTION, vector 5): on that path only, the postincrement
   or predecrement emit_read() made to the source address register is
   undone. Other modes changed no register. */
static void emit_undo_step(struct HJTrans *t, const struct HJEA *ea, int size)
{
    int an = REG_A(ea->reg);

    if (ea->mode == EA_POST)
        put(&t->e, a64_addsub_imm(0, 1, 0, an, an, (uint32_t)step_of(size, ea->reg), 0));
    else if (ea->mode == EA_PRE)
        put(&t->e, a64_addsub_imm(0, 0, 0, an, an, (uint32_t)step_of(size, ea->reg), 0));
}

/* Before a division reads its divisor from memory: when that read would
   reach the host (a READ_HOST range, or outside guest memory), the block
   ends at the division with nothing of it done and no host call made, as
   STOP_UNSUPPORTED, so that the host can execute the division itself,
   once. The inline test is the one of emit_access(); what it cannot decide
   goes to a probe in C, which applies the rule of the contract without an
   access. No mode of the subset reads memory to form the address. */
static void emit_div_probe(struct HJTrans *t, const struct HJInsn *in, int size)
{
    struct HJEmit *e = &t->e;
    const struct HJEA *ea = &in->src;
    struct HJStub *s;
    unsigned br_direct;
    int areg;

    if (ea->mode == EA_DN || ea->mode == EA_AN || ea->mode == EA_IMM)
        return;
    if (ea->mode == EA_PRE)
    {
        put(e, a64_addsub_imm(0, 1, 0, R_ADDR, REG_A(ea->reg), (uint32_t)step_of(size, ea->reg), 0));
        areg = R_ADDR;
    }
    else
        areg = emit_address(t, ea, size);   /* no guest register changes in these modes */
    if (t->nstub >= HJ_MAX_STUBS)
    {
        e->bad = 1;
        return;
    }
    s = &t->stub[t->nstub++];
    emit_mov32(e, R_VAL, 0);
    put(e, a64_cmp(areg, R_LIMIT));
    s->br1 = e->n;
    put(e, a64_bcond(A64_HI, 0));
    put(e, a64_logic_const(0, R_SCR, areg, 0xFFFu));
    put(e, a64_addsub_imm(0, 1, 1, ZR, R_SCR, 0x1000u - (uint32_t)size, 0));
    s->br3 = e->n;
    put(e, a64_bcond(A64_HI, 0));
    put(e, a64_lsr(R_SCR, areg, KJ68_PAGE_SHIFT));
    put(e, a64_addsub_reg(1, 0, 0, R_SCR, R_STATE, R_SCR, 0, 0));
    put(e, a64_ldst_uimm(0, 1, R_SCR, R_SCR, ST_PAGES));
    s->br2 = e->n;
    put(e, a64_tbz(1, R_SCR, 0, 0));
    s->back = e->n;
    s->kind = (uint8_t)(size == 2 ? SLOW_P16 : SLOW_P32);
    s->areg = (uint8_t)areg;
    s->pc = in->pc;
    br_direct = e->n;
    put(e, a64_cbz(0, 0, R_VAL, 0));
    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);
    patch(e, br_direct, e->n);
}
''')

# A word division reads its divisor only after emit_div_probe() found the read direct.
region(1813, 1814, '    int v = emit_read(t, &in->src, 2);', '    unsigned br_nz, br_ovf, br_done;',
       r'''    unsigned br_nz, br_ovf, br_done;
    int v;

    emit_div_probe(t, in, 2);
    v = emit_read(t, &in->src, 2);''')

region(1819, 1819, '    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);', '    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);',
       r'''    emit_undo_step(t, &in->src, 2);
    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);''')

# A long division reads its divisor only after emit_div_probe() found the read direct.
region(1857, 1858, '    int v = emit_read(t, &in->src, 4);', '    unsigned br_nz, br_ovf, br_done;',
       r'''    unsigned br_nz, br_ovf, br_done;
    int v;

    emit_div_probe(t, in, 4);
    v = emit_read(t, &in->src, 4);''')

region(1862, 1862, '    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);', '    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);',
       r'''    emit_undo_step(t, &in->src, 4);
    emit_exit_reason(t, in->pc, HJ_EXIT_FAULT);''')

region(2791, 2820, "/* Decode and emit a block of at most 'max' instructions at pc into t->e.", '}', r'''/* Decode and emit a block of at most 'max' instructions at pc into t->e.
   Returns the number of 68k instructions, sets *end, and gives in starts
   the offset from pc of each instruction. */
static unsigned translate_into(const struct KJ68Domain *dom, struct HJTrans *t, uint32_t pc,
                               unsigned max, uint32_t *end, uint16_t *starts)
{
    struct HJInsn insns[HJ_MAX_INSNS];
    unsigned n = 0, i;
    uint32_t at = pc;

    while (n < max)
    {
        decode_backend(dom, at, &insns[n], t->library_mode);
        at = insns[n].next;
        if (is_terminator(insns[n++].kind))
            break;
    }
    flag_liveness(insns, n);
    t->start = pc;
    t->nstub = 0;
    for (i = 0; i < n; i++)
    {
        starts[i] = (uint16_t)(insns[i].pc - pc);
        emit_insn(t, &insns[i]);
    }
    if (!is_terminator(insns[n - 1].kind))
        emit_exit_to(t, insns[n - 1].next);
    emit_stubs(t);
    /* After every word decoded, the extension words of a faulting
       instruction too: a new one can make it valid. translate() keeps the
       end inside guest memory. */
    *end = insns[n - 1].next;
    return n;
}''')

region(2867, 2892, '#if HJ_RUNS', '#endif', r'''#if HJ_RUNS
static void *code_alloc(void)
{
    void *code;

#ifdef KJ68_TEST_SEAMS
    if (kj_test_fail_next())
        return NULL;
#endif
#if defined(__AROS__)
    code = AllocMem(HJ_CODE_BYTES, MEMF_PUBLIC | MEMF_EXECUTABLE);
#else
#if defined(__APPLE__)
    code = mmap(NULL, HJ_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    code = mmap(NULL, HJ_CODE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (code == MAP_FAILED)
        code = NULL;
#endif
#ifdef KJ68_TEST_SEAMS
    if (code)
        kj68_test_exec_live += (long)HJ_CODE_BYTES;
#endif
    return code;
}

static void code_free(void *code)
{
    if (!code)
        return;
#ifdef KJ68_TEST_SEAMS
    kj68_test_exec_live -= (long)HJ_CODE_BYTES;
#endif
#if defined(__AROS__)
    FreeMem(code, HJ_CODE_BYTES);
#else
    munmap(code, HJ_CODE_BYTES);
#endif
}
#endif''')

region(2894, 2913, 'static void fast_clear(struct HJBackend *b)', '}', r'''static void fast_clear(struct HJCpu *c)
{
    unsigned i;

    for (i = 0; i < HJ_FAST_SIZE; i++)
    {
        c->fast[i].pc = HJ_EMPTY_TAG;
        c->fast[i].entry = 0;
    }
}

/* Empties the code memory of the domain; every CPU empties its fast table
   before it enters translated code again. */
static void reset_code(struct HJBackend *b)
{
    b->code_used = b->glue_words;
    memset(b->buckets, 0, sizeof(b->buckets));
    b->nblocks = 0;
    b->guest_used = 0;
    b->fast_generation++;
    b->stats.resets++;
    kj_count(&b->dom->counters.code_resets, 1);
}''')

region(2996, 2996, '    put(e, a64_mov(R_ADDR, ZR));', '    put(e, a64_mov(R_ADDR, ZR));',
       '    put(e, a64_mov(R_ADDR, R_F2));   /* the PC of the access the host stopped */')

region(3007, 3148, '/* ------------------------------------------------------------------', '}', r'''/* ------------------------------------------------------------------
   The C side: slow accesses, blocks
   ------------------------------------------------------------------ */

/* An access is direct when it lies inside guest memory and touches no
   range that sends this kind of access to the host (contract, section 4). */
static int kj_direct(const struct KJ68Domain *dom, uint32_t addr, uint32_t width, ULONG flag)
{
    ULONG i;

    if ((uint64_t)addr + width > dom->size)
        return 0;
    for (i = 0; i < dom->num_ranges; i++)
        if ((dom->ranges[i].flags & flag) &&
            addr < dom->ranges[i].hi && dom->ranges[i].lo < (uint64_t)addr + width)
            return 0;
    return 1;
}

/* A byte or word read from guest memory or from the host. A host answer
   of 1 stops the run at once: no other host access is made in it. */
static uint32_t kj_read(struct HJCpu *c, uint32_t addr, uint32_t width, uint32_t pc)
{
    const struct KJ68Domain *dom = c->owner->domain;
    ULONG value = 0;

    if (c->st->abort)
        return 0;
    if (kj_direct(dom, addr, width, KJ68_RANGE_READ_HOST))
        return width == 1 ? dom->base[addr] : (uint32_t)(dom->base[addr] << 8 | dom->base[addr + 1]);
    if (dom->host->read(dom->host_domain, c->owner->host_cpu, addr, width, pc, &value))
    {
        c->st->abort = 1;
        return 0;
    }
    return width == 1 ? (value & 0xFFu) : (value & 0xFFFFu);
}

static void kj_write(struct HJCpu *c, uint32_t addr, uint32_t width, uint32_t value, uint32_t pc)
{
    const struct KJ68Domain *dom = c->owner->domain;

    if (c->st->abort)
        return;
    if (kj_direct(dom, addr, width, KJ68_RANGE_WRITE_HOST))
    {
        if (width == 1)
            dom->base[addr] = (UBYTE)value;
        else
        {
            dom->base[addr] = (UBYTE)(value >> 8);
            dom->base[addr + 1] = (UBYTE)value;
        }
        return;
    }
    if (dom->host->write(dom->host_domain, c->owner->host_cpu, addr, width, value, pc))
        c->st->abort = 1;
}

/* Called from translated code. A longword is two word accesses, the word
   at addr first, each routed by itself. */
static uint32_t slow_access(struct HJState *st, uint32_t addr, uint32_t value, uint32_t kind, uint32_t pc)
{
    struct HJCpu *c = st->backend;
    uint32_t hi;

    if (kind == SLOW_P16 || kind == SLOW_P32)
    {
        /* a divisor probe: no access and no host call */
        const struct KJ68Domain *dom = c->owner->domain;

        c->declined = !kj_direct(dom, addr, 2, KJ68_RANGE_READ_HOST) ||
                      (kind == SLOW_P32 && !kj_direct(dom, addr + 2, 2, KJ68_RANGE_READ_HOST));
        return (uint32_t)c->declined;
    }
    c->b->stats.slow_accesses++;
    switch (kind)
    {
    case SLOW_R8: return kj_read(c, addr, 1, pc);
    case SLOW_R16: return kj_read(c, addr, 2, pc);
    case SLOW_R32:
        hi = kj_read(c, addr, 2, pc);
        return hi << 16 | kj_read(c, addr + 2, 2, pc);
    case SLOW_W8: kj_write(c, addr, 1, value & 0xFFu, pc); return 0;
    case SLOW_W16: kj_write(c, addr, 2, value & 0xFFFFu, pc); return 0;
    default:
        kj_write(c, addr, 2, value >> 16, pc);
        kj_write(c, addr + 2, 2, value & 0xFFFFu, pc);
        return 0;
    }
}''')

region(3152, 3158, 'static void fast_insert(struct HJBackend *b, const struct HJBlock *blk)', '}', r'''static void fast_insert(struct HJCpu *c, const struct HJBlock *blk)
{
    struct HJFast *f = &c->fast[(blk->pc >> 1) & (HJ_FAST_SIZE - 1)];

    f->pc = blk->pc;
    f->entry = (uint64_t)(uintptr_t)blk->code;
}''')

region(3160, 3219, '/* Called only from hj_execute(), between blocks: no translated code is', '}', r'''/* Called only from kj_tr_run(), between blocks: no translated code is
   running, so the code memory can be emptied here. Not reentrant. */
static struct HJBlock *translate(struct HJBackend *b, uint32_t pc)
{
    struct HJTrans *t = b->trans;
    uint64_t t0 = now_ns();
    unsigned max = HJ_MAX_INSNS, n = 0, attempt;
    uint32_t end = pc;
    uint16_t starts[HJ_MAX_INSNS];
    struct HJBlock *blk;

    for (attempt = 0; attempt < 2; attempt++)
    {
        if (b->code_words - b->code_used < HJ_SCRATCH_WORDS || b->nblocks >= HJ_MAX_BLOCKS ||
            HJ_GUEST_BYTES - b->guest_used < 4u * HJ_MAX_INSNS * 10u)
            reset_code(b);
        for (;;)
        {
            memset(&t->e, 0, sizeof(t->e));
            t->e.buf = b->scratch;
            t->e.cap = HJ_SCRATCH_WORDS;
            t->e.origin = (uintptr_t)(b->code + b->code_used);
            t->g_exit = b->g_exit; t->g_lookup = b->g_lookup; t->g_budget = b->g_budget;
            t->library_mode = 1;
            memcpy(t->g_slow, b->g_slow, sizeof(t->g_slow));
            n = translate_into(b->dom, t, pc, max, &end, starts);
            if (!t->e.bad || max == 1)
                break;
            max = n > 1 ? n / 2 : 1;
        }
        if (!t->e.bad)
            break;
    }
    if (end > b->dom->size)
        end = b->dom->size;
    if (t->e.bad || end <= pc)
        return NULL;

    code_write_begin(b);
    memcpy(b->code + b->code_used, b->scratch, 4u * t->e.n);
    code_write_end(b, b->code + b->code_used, t->e.n);

    blk = &b->blocks[b->nblocks++];
    blk->pc = pc;
    blk->end = end;
    blk->epoch = b->epoch;
    blk->words = t->e.n;
    blk->code = b->code + b->code_used;
    b->code_used += t->e.n;
    memcpy(b->guest_bytes + b->guest_used, b->dom->base + pc, end - pc);
    blk->guest = b->guest_bytes + b->guest_used;
    b->guest_used += (end - pc + 3u) & ~3u;
    /* the instruction offsets beside the guest bytes; the room check above
       leaves space for both */
    memcpy(b->guest_bytes + b->guest_used, starts, n * sizeof(starts[0]));
    blk->starts = (const uint16_t *)(const void *)(b->guest_bytes + b->guest_used);
    blk->ninsns = (uint16_t)n;
    b->guest_used += (n * sizeof(starts[0]) + 3u) & ~3u;
    blk->next = b->buckets[bucket_of(pc)];
    b->buckets[bucket_of(pc)] = blk;

    b->stats.blocks++;
    b->stats.guest_insns += n;
    b->stats.host_words += t->e.n;
    b->stats.translate_ns += (unsigned long)(now_ns() - t0);
    kj_count(&b->dom->counters.blocks, 1);
    kj_count(&b->dom->counters.insns, n);
    return blk;
}''')

region(3221, 3248, '/* A block for pc that matches the guest code, translated if needed. A', '}', r'''/* A block for pc that matches the guest code, translated if needed. A
   block from before a flush is kept when its guest bytes are unchanged. */
static struct HJBlock *get_block(struct HJCpu *c, uint32_t pc)
{
    struct HJBackend *b = c->b;
    struct HJBlock **link = &b->buckets[bucket_of(pc)], *blk;

    for (blk = *link; blk; link = &blk->next, blk = blk->next)
        if (blk->pc == pc)
            break;
    if (blk && blk->epoch != b->epoch)
    {
        if (memcmp(b->dom->base + blk->pc, blk->guest, blk->end - blk->pc) == 0)
        {
            blk->epoch = b->epoch;
            b->stats.revalidated++;
        }
        else
        {
            *link = blk->next;
            blk = NULL;
        }
    }
    if (!blk)
        blk = translate(b, pc);
    if (c->fast_generation != b->fast_generation)
    {
        fast_clear(c);
        c->fast_generation = b->fast_generation;
    }
    if (blk)
        fast_insert(c, blk);
    return blk;
}''')

# the KaffeineCPUOps section (hj_get_d .. hj_ops): the host owns what it did
assert orig[3404] == '};' and orig[3405] == '' and orig[3407] == '   Public functions'
region(3250, 3406, '/* ------------------------------------------------------------------', '', None)

region(3594, 3796, 'struct HJBackend *HJ_Create(struct KaffeineContext *ctx)', '}', r'''/* ------------------------------------------------------------------
   The engine side (kj68_intern.h), called by kj68_engine.c
   ------------------------------------------------------------------ */

int kj_tr_runs(void)
{
    return HJ_Available();
}

const char *kj_tr_exec_memory_kind(void)
{
    return HJ_ExecMemoryKind();
}

int kj_tr_encoder_self_test(char *message, unsigned long size)
{
    return HJ_EncoderSelfTest(message, size);
}

#if HJ_RUNS
static void hj_free_code(struct HJBackend *b)
{
    code_free(b->code);
    kj_free(b->blocks, HJ_MAX_BLOCKS * sizeof(*b->blocks));
    kj_free(b->guest_bytes, HJ_GUEST_BYTES);
    kj_free(b->scratch, 4u * HJ_SCRATCH_WORDS);
    kj_free(b->trans, sizeof(*b->trans));
    kj_free(b, sizeof(*b));
}
#endif

/* The code memory, block table, guest-byte store and the glue of a domain,
   shared by its CPUs. */
LONG kj_tr_domain_create(struct KJ68Domain *dom)
{
#if HJ_RUNS
    struct HJBackend *b;
    struct HJEmit e;

    b = kj_alloc(sizeof(*b));
    if (!b)
        return KJ68_ERR_NOMEM;
    b->dom = dom;
    b->code = code_alloc();
    b->blocks = kj_alloc(HJ_MAX_BLOCKS * sizeof(*b->blocks));
    b->guest_bytes = kj_alloc(HJ_GUEST_BYTES);
    b->scratch = kj_alloc(4u * HJ_SCRATCH_WORDS);
    b->trans = kj_alloc(sizeof(*b->trans));
    if (!b->code || !b->blocks || !b->guest_bytes || !b->scratch || !b->trans)
    {
        hj_free_code(b);
        return KJ68_ERR_NOMEM;
    }
    b->code_words = HJ_CODE_BYTES / 4;
    memset(&e, 0, sizeof(e));
    e.buf = b->scratch;
    e.cap = HJ_SCRATCH_WORDS;
    e.origin = (uintptr_t)b->code;
    emit_glue(b, &e);
    if (e.bad)
    {
        /* not reachable with these sizes: the glue fits the scratch buffer */
        hj_free_code(b);
        return KJ68_ERR_NOMEM;
    }
    code_write_begin(b);
    memcpy(b->code, b->scratch, 4u * e.n);
    code_write_end(b, b->code, e.n);
    b->glue_words = b->code_used = e.n;
    b->stats.write_windows = b->stats.icache_syncs = 0;
    {
        uintptr_t entry = b->g_enter;

        memcpy(&b->enter, &entry, sizeof(entry));
    }
    dom->code = b;
    return KJ68_OK;
#else
    (void)dom;
    (void)slow_access;
    (void)emit_glue;
    return KJ68_ERR_NOHOST;
#endif
}

void kj_tr_domain_destroy(struct KJ68Domain *dom)
{
#if HJ_RUNS
    if (dom->code)
        hj_free_code(dom->code);
#endif
    dom->code = NULL;
}

ULONG kj_tr_code_bytes(const struct KJ68Domain *dom)
{
    return dom->code ? HJ_CODE_BYTES : 0;
}

/* Registers as cpu_create leaves them: 0, PC 0, SR 0x2700. */
LONG kj_tr_cpu_create(struct KJ68CPU *owner)
{
    struct KJ68Domain *dom = owner->domain;
    struct HJCpu *c = kj_alloc(sizeof(*c));
    struct HJState *st;

    if (!c)
        return KJ68_ERR_NOMEM;
    c->st_bytes = sizeof(struct HJState) + dom->npages;
    c->st = kj_alloc(c->st_bytes);
    c->fast = kj_alloc(HJ_FAST_SIZE * sizeof(*c->fast));
    if (!c->st || !c->fast)
    {
        kj_free(c->fast, HJ_FAST_SIZE * sizeof(*c->fast));
        kj_free(c->st, c->st_bytes);
        kj_free(c, sizeof(*c));
        return KJ68_ERR_NOMEM;
    }
    st = c->st;
    c->owner = owner;
    c->b = dom->code;
    st->mem = dom->base;
    st->limit = dom->size - 4;
    st->fast = c->fast;
    st->slow = (void *)(uintptr_t)slow_access;
    st->backend = c;
    st->slice_budget = 1;      /* one block per entry: the run loop counts */
    st->sr_high = (KJ68_SR_AT_CREATE >> 8) & 0x27u;
    memcpy(st->pages, dom->pages, dom->npages);
    c->pages_generation = dom->pages_generation;
    fast_clear(c);
    c->fast_generation = c->b->fast_generation;
    owner->tr = c;
    return KJ68_OK;
}

void kj_tr_cpu_destroy(struct KJ68CPU *owner)
{
    struct HJCpu *c = owner->tr;

    if (!c)
        return;
    kj_free(c->fast, HJ_FAST_SIZE * sizeof(*c->fast));
    kj_free(c->st, c->st_bytes);
    kj_free(c, sizeof(*c));
    owner->tr = NULL;
}

ULONG kj_tr_get_reg(const struct KJ68CPU *owner, ULONG reg)
{
    const struct HJState *st = owner->tr->st;

    if (reg < 8)
        return st->d[reg];
    if (reg < 16)
        return st->a[reg - 8];
    if (reg == KJ68_REG_PC)
        return st->pc;
    return (ULONG)((st->sr_high << 8 | swap_cv(st->flags)) & KJ68_SR_DEFINED);
}

/* Version 1.0 has one mode: S set, T and M clear. */
LONG kj_tr_set_reg(struct KJ68CPU *owner, ULONG reg, ULONG value)
{
    struct HJState *st = owner->tr->st;

    if (reg < 8)
        st->d[reg] = value;
    else if (reg < 16)
        st->a[reg - 8] = value;
    else if (reg == KJ68_REG_PC)
        st->pc = value;
    else
    {
        if ((value & KJ68_SR_MODEL_MASK) != KJ68_SR_MODEL_VALUE)
            return KJ68_ERR_UNSUPPORTED;
        st->sr_high = (value >> 8) & 0x27u;
        st->flags = swap_cv(value & 0x1Fu);
    }
    return KJ68_OK;
}

/* As the legacy flush: a new epoch makes every block compare its guest
   bytes before it runs again, and every CPU empties its fast table. */
void kj_tr_invalidate(struct KJ68Domain *dom)
{
    struct HJBackend *b = dom->code;

    if (++b->epoch == 0)
        reset_code(b);
    b->fast_generation++;
    b->stats.flushes++;
}

/* A new map generation. Every block was decoded under the old fetch rules:
   an extension word or a later opcode of it may now lie in a READ_HOST
   range, or a fetch that failed may now succeed. The code memory of the
   domain is emptied, with no revalidation by guest bytes, so every CPU of
   the domain translates again under the new map. Counted as a code reset. */
void kj_tr_map_changed(struct KJ68Domain *dom)
{
    reset_code(dom->code);
}

/* Instructions of the block that start before pc. */
static ULONG count_before(const struct HJBlock *blk, uint32_t pc)
{
    ULONG n = 0;

    while (n < (ULONG)blk->ninsns && blk->pc + blk->starts[n] < pc)
        n++;
    return n;
}

static void kj_exit(struct KJ68Exit *x, ULONG reason, uint32_t pc, ULONG retired)
{
    x->reason = reason;
    x->pc = pc;
    x->retired = retired;
}

static void kj_text(struct KJ68Exit *x, const char *s)
{
    size_t i;

    for (i = 0; s[i] && i + 1 < KJ68_TEXT_SIZE; i++)
        x->text[i] = s[i];
    x->text[i] = 0;
}

static ULONG kj_opcode(const struct KJ68Domain *dom, uint32_t pc)
{
    return kj_fetchable(dom, pc) ? (ULONG)(dom->base[pc] << 8 | dom->base[pc + 1]) : 0;
}

/* The F-line encodings of 68040 integer-unit instructions: CINV, CPUSH,
   MOVE16. They are no Line-F exception on a 68040; this engine does not
   execute them. */
static int kj_040_integer_fline(uint16_t op)
{
    return (op & 0xFF00u) == 0xF400u || (op & 0xFFE0u) == 0xF600u || (op & 0xFFF8u) == 0xF620u;
}

/* A fault exit: the instruction at pc was not executed. An exception is
   reported only where it is certain; anything else is UNSUPPORTED, which
   the contract always allows. */
static void kj_classify(const struct KJ68Domain *dom, struct KJ68Exit *x, uint32_t pc, ULONG retired)
{
    struct HJInsn in;
    uint16_t op = (uint16_t)kj_opcode(dom, pc);

    decode_backend(dom, pc, &in, 1);
    if (in.kind == K_FAULT && in.why && strcmp(in.why, "fetch outside guest memory") == 0)
    {
        kj_exit(x, KJ68_STOP_PC_OUTSIDE, pc, retired);
        x->address = in.bad_at;
        return;
    }
    kj_exit(x, KJ68_STOP_EXCEPTION, pc, retired);
    x->opcode = op;
    if (in.kind == K_DIVW || in.kind == K_DIVL)
    {
        x->vector = 5;
        kj_text(x, "divide by zero");
    }
    else if (op == 0x4AFCu)
    {
        x->vector = 4;
        kj_text(x, "ILLEGAL");
    }
    else if ((op & 0xFFF0u) == 0x4E40u)
    {
        x->vector = 32u + (op & 15u);
        kj_text(x, "TRAP");
    }
    else if ((op & 0xF000u) == 0xF000u && !kj_040_integer_fline(op))
    {
        x->vector = 11;
        kj_text(x, "Line-F");
    }
    else
    {
        x->reason = KJ68_STOP_UNSUPPORTED;
        kj_text(x, in.why ? in.why : "unexpected fault exit");
    }
}

/* One run: blocks are entered one at a time (slice budget 1), so the count
   of retired instructions is exact: a whole block when it ends through the
   lookup or the budget, the instructions before the exit PC otherwise. No
   block starts once budget instructions have retired. */
void kj_tr_run(struct KJ68CPU *owner, ULONG budget, struct KJ68Exit *x)
{
    struct HJCpu *c = owner->tr;
    struct HJBackend *b = c->b;
    const struct KJ68Domain *dom = owner->domain;
    struct HJState *st = c->st;
    ULONG retired = 0;

    if (c->pages_generation != dom->pages_generation)
    {
        memcpy(st->pages, dom->pages, dom->npages);
        c->pages_generation = dom->pages_generation;
    }
    st->abort = 0;
    c->declined = 0;
#if HJ_RUNS
    __asm__ __volatile__("isb" ::: "memory");
#endif
    for (;;)
    {
        uint32_t pc = st->pc, reason;
        struct HJBlock *blk;

        if (pc & 1)
        {
            kj_exit(x, KJ68_STOP_EXCEPTION, pc, retired);
            x->vector = 3;
            kj_text(x, "odd PC");
            return;
        }
        if (!kj_fetchable(dom, pc))
        {
            kj_exit(x, KJ68_STOP_PC_OUTSIDE, pc, retired);
            x->address = pc;
            return;
        }
        blk = get_block(c, pc);
        if (!blk)
        {
            kj_exit(x, KJ68_STOP_FAULT, pc, retired);
            kj_text(x, "translation failed");
            return;
        }
        b->stats.enters++;
        reason = b->enter(st, blk->code);
        if (reason < HJ_EXIT_COUNT)
            b->stats.exits[reason]++;
        if (reason == HJ_EXIT_BUDGET || reason == HJ_EXIT_LOOKUP)
        {
            retired += blk->ninsns;
            if (retired >= budget)
            {
                kj_exit(x, KJ68_STOP_BUDGET, st->pc, retired);
                return;
            }
            continue;
        }
        retired += count_before(blk, st->pc);
        if (reason == HJ_EXIT_LINEA)
        {
            kj_exit(x, KJ68_STOP_ALINE, st->pc, retired);
            x->opcode = kj_opcode(dom, st->pc);
            return;
        }
        if (reason == HJ_EXIT_ABORT)
        {
            kj_exit(x, KJ68_STOP_HOST, st->pc, retired);
            kj_text(x, "a host access asked to stop");
            return;
        }
        if (c->declined)
        {
            /* a divisor probe ended the block at the division: nothing of
               it done and no host call made (contract, section 6) */
            c->declined = 0;
            kj_exit(x, KJ68_STOP_UNSUPPORTED, st->pc, retired);
            x->opcode = kj_opcode(dom, st->pc);
            kj_text(x, "divisor read through the host");
            return;
        }
        kj_classify(dom, x, st->pc, retired);
        return;
    }
}''')

# ---- assemble ---------------------------------------------------------------------------
out, i, used = [], 1, []
starts = sorted(R)
for a, b2 in zip(starts, starts[1:]):
    assert R[a][0] < b2, (a, b2)
while i <= 3796:
    if i in R:
        last, new = R[i]
        if new is not None:
            out.extend(new.split('\n'))
        used.append(i)
        i = last + 1
    else:
        out.append(orig[i - 1])
        i += 1
assert used == starts
text = '\n'.join(out) + '\n'
for bad in ('KaffeineContext', 'Kaffeine_', 'kaffeine_intern', 'callback_depth', 'jit_generation', 'CUSTOM_BASE',
            'beam_', 'POTGOR', 'RETURN_FAIL', 'sv_redirect', 'task_ops', 'cpu_factory', 'm68k_read16', 'bug('):
    assert bad not in text, bad
text.encode('ascii')
assert '\t' not in text

# ---- the reuse map, by top-level definition ------------------------------------------------
def definitions(lines):
    """Top-level definitions (functions, structs, enums, tables): name -> text,
    and name -> index of its first line. A definition ends at its closing
    brace on its own line or on its first line; a declaration that ends with
    ';' and has no brace is skipped."""
    defs, at, k, n = {}, {}, 0, len(lines)
    while k < n:
        line = lines[k]
        if not line or line[0] in ' \t#/*}' or line.startswith('_Static_assert'):
            k += 1
            continue
        if '{' in line and line.count('{') == line.count('}') and line.rstrip().endswith(('}', '};')):
            j = k
        elif line.rstrip().endswith(';') and '{' not in line:
            k += 1
            continue
        else:
            j = k + 1
            while j < n and not lines[j].startswith('}'):
                j += 1
        body = lines[k:j + 1]
        head = ' '.join(body)
        before_brace = line.split('{')[0]
        name = None
        if re.search(r'=\s*(\{.*)?$', before_brace + ('{' if '{' in line else '')) and '(' not in before_brace:
            m = re.search(r'(\w+)\s*(\[[^\]]*\])?\s*=', line)
            name = m.group(1) if m else None
        else:
            m = re.match(r'(?:static\s+)?(?:const\s+)?(struct|enum)\b\s*(\w*)', line)
            if m and '(' not in before_brace:
                if m.group(2):
                    name = m.group(1) + ' ' + m.group(2)
                else:
                    e = re.search(r'\{\s*(\w+)', head)
                    name = 'enum ' + (e.group(1) if e else '?')
            else:
                m = re.search(r'(\w+)\s*\(', line)
                name = m.group(1) if m else None
        if name:
            key = name
            while key in defs:
                key += "'"
            defs[key] = '\n'.join(body)
            at[key] = k
        k = j + 1
    return defs, at


(old_defs, old_at), (new_defs, new_at) = definitions(orig[:-1]), definitions(text.split('\n')[:-1])
old_lines = {}
for name, body in old_defs.items():
    # the parsed position, not a search for the first line: two anonymous
    # enums of the original begin with the same line
    old_lines[name] = [old_at[name] + 1, old_at[name] + body.count('\n') + 1]
    assert '\n'.join(orig[old_lines[name][0] - 1:old_lines[name][1]]) == body, name
rows = []
for name in sorted(set(old_defs) | set(new_defs), key=lambda x: (old_lines.get(x, [99999])[0], new_at.get(x, 0))):
    if name in old_defs and name in new_defs:
        status = 'unchanged' if old_defs[name] == new_defs[name] else 'changed'
    elif name in old_defs:
        status = 'removed'
    else:
        status = 'moved from kaffeine_jit.h' if name in ('struct HJStats', 'enum HJ_EXIT_LOOKUP') else 'added'
    row = {'name': name, 'status': status}
    if name in old_defs:
        row['original_lines'] = old_lines[name]
        row['original_sha256'] = hashlib.sha256(old_defs[name].encode()).hexdigest()
    if name in new_defs:
        row['new_sha256'] = hashlib.sha256(new_defs[name].encode()).hexdigest()
    rows.append(row)
counts = {}
for r in rows:
    counts[r['status']] = counts.get(r['status'], 0) + 1
reasons = {
    'changed': 'coupling: KaffeineContext to the domain, the map, host leaf calls, per-domain code shared by per-CPU states, exits to the host; '
               'emit_divw and emit_divl: a zero divisor undoes the source postincrement or predecrement before its exit',
    'removed': 'what the host now owns: library dispatch, callbacks, the CPU factory, the scheduler, custom chip emulation, the legacy run loop and entry',
    'unchanged': 'decoding, emission, encoders and helpers byte for byte'}
out_c.write_text(text, encoding='ascii')
out_map.write_text(json.dumps({
    'author': 'Michele Dipace (@kaffeine1)', 'id': 'kaffeine-jit68-translator-reuse-map',
    'original': {'path': str(src_path), 'bytes': len(data), 'sha256': ORIG_SHA, 'lines': 3796},
    'legacy_header': {'path': str(h_path), 'bytes': len(hdata), 'sha256': JIT_H_SHA, 'lines_moved': [16, 42]},
    'result': {'path': 'engine/kj68_translate.c', 'bytes': len(text), 'sha256': hashlib.sha256(text.encode()).hexdigest(),
               'lines': text.count('\n')},
    'replaced_line_ranges_of_the_original': [[a, R[a][0]] for a in starts],
    'counts': counts, 'reasons': reasons, 'definitions': rows}, indent=1, ensure_ascii=True) + '\n', encoding='ascii')
print(json.dumps({'bytes': len(text), 'sha256': hashlib.sha256(text.encode()).hexdigest(), 'lines': text.count('\n'), 'counts': counts}))
