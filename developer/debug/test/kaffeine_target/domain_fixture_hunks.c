/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   An intentionally small assembler for this original public regression.
   All operands are literal words, local labels or explicit HUNK relocations.
   Code/data size, labels, branch spans and relocation offsets are bounded. */
#include "domain_fixture_hunks.h"
#include <string.h>

enum {
    START, FAIL, FAIL_NO_DOS, FAIL_NO_REPLY, FAIL_EXIT, MSG, CHILD_HANDLE,
    TAGS, TAG_SEG, TAG_HOME, CHILD_ARGS, CHILD_NAME, COMMAND, DOS_NAME,
    CHILD_FILE, CHILD_DIR, ROOT_FILE, CHILD_INPUT, EXPECT_ROOT, EXPECT_CHILD,
    EXPECT_ARGS, EXPECT_NESTED, EXPECT_BACKEND, VAR_NAME, GATE_FILE, EXPECT_GATE,
    PROOF_FILE, FAIL_FILE, FAIL_RECORD, PHASE, BUF, STRCMP, STR_LOOP, STR_BAD,
    READ_FILE, READ_BAD, READ_END, GATE_LOOP, GATE_DELAY, GATE_DONE,
    LABEL_LIMIT
};
enum { BNE = 0x6600, BEQ = 0x6700, BMI = 0x6b00, BRA = 0x6000, BSR = 0x6100 };
struct Fix { uint32_t at; int label, add, relative; };
struct Builder {
    uint8_t code[16384]; uint32_t used, reloc[512], reloc_count;
    int32_t label[LABEL_LIMIT]; struct Fix fix[512]; unsigned fixes; int bad;
};
static struct Builder b;

static void w(uint16_t v) {
    if (b.used + 2 > sizeof(b.code)) { b.bad = 1; return; }
    b.code[b.used++] = (uint8_t)(v >> 8); b.code[b.used++] = (uint8_t)v;
}
static void l(uint32_t v) { w((uint16_t)(v >> 16)); w((uint16_t)v); }
static void mark(int id) {
    if (id < 0 || id >= LABEL_LIMIT || b.label[id] >= 0) b.bad = 1;
    else b.label[id] = (int32_t)b.used;
}
static void fix(int id, int add, int relative) {
    if (b.fixes == 512 || (!relative && b.reloc_count == 512)) { b.bad = 1; return; }
    b.fix[b.fixes++] = (struct Fix){b.used, id, add, relative};
    if (relative) w(0); else { b.reloc[b.reloc_count++] = b.used; l(0); }
}
static void align4(void) { while (b.used & 3) w(0); }
static void zero(int id, unsigned bytes) {
    align4(); mark(id); while (bytes >= 2) { w(0); bytes -= 2; }
    if (bytes) b.bad = 1;
}
static void text(int id, const char *s) {
    size_t n = strlen(s) + 1, i;
    align4(); mark(id);
    if (n > sizeof(b.code) - b.used) { b.bad = 1; return; }
    for (i = 0; i < n; ++i) b.code[b.used++] = (uint8_t)s[i];
    if (b.used & 1) b.code[b.used++] = 0;
}
static void branch(unsigned op, int id) { w((uint16_t)op); fix(id, 0, 1); }
static void call(int lvo) { w(0x4eae); w((uint16_t)-lvo); }
static void execbase(void) { w(0x2c78); w(4); }             /* movea.l 4.w,a6 */
static void ma(unsigned src, unsigned dst) { w((uint16_t)(0x2048 | (dst << 9) | src)); }
static void dosbase(void) { ma(5, 6); }
static void md(unsigned src, unsigned dst) { w((uint16_t)(0x2000 | (dst << 9) | src)); }
static void ad(unsigned src, unsigned dst) { w((uint16_t)(0x2008 | (dst << 9) | src)); }
static void da(unsigned src, unsigned dst) { w((uint16_t)(0x2040 | (dst << 9) | src)); }
static void imm(unsigned reg, uint32_t value) { w((uint16_t)(0x203c | (reg << 9))); l(value); }
static void addr(unsigned reg, int id, int add) { w((uint16_t)(0x41f9 | (reg << 9))); fix(id, add, 0); }
static void address_d(unsigned reg, int id) { addr(0, id, 0); ad(0, reg); }
static void load(unsigned base, unsigned off, unsigned reg) {
    w((uint16_t)(0x2028 | (reg << 9) | base)); w((uint16_t)off);
}
static void loada(unsigned base, unsigned off, unsigned reg) {
    w((uint16_t)(0x2068 | (reg << 9) | base)); w((uint16_t)off);
}
static void store(unsigned reg, unsigned base, unsigned off) {
    w((uint16_t)(0x2140 | (base << 9) | reg)); w((uint16_t)off);
}
static void storea(unsigned reg, unsigned base, unsigned off) {
    w((uint16_t)(0x2148 | (base << 9) | reg)); w((uint16_t)off);
}
static void save(unsigned reg, int id, int add) { w((uint16_t)(0x23c0 | reg)); fix(id, add, 0); }
static void savea(unsigned reg, int id, int add) { w((uint16_t)(0x23c8 | reg)); fix(id, add, 0); }
static void geta(int id, unsigned reg) { w((uint16_t)(0x2079 | (reg << 9))); fix(id, 0, 0); }
static void testi(unsigned reg) { w((uint16_t)(0x4a80 | reg)); }
static void cmpi(unsigned reg, uint32_t value) { w((uint16_t)(0x0c80 | reg)); l(value); }
static void cmp(unsigned src, unsigned dst) { w((uint16_t)(0xb080 | (dst << 9) | src)); }
static void error(unsigned number) { imm(6, number); }
static void nonzero(unsigned number) { error(number); testi(0); branch(BEQ, FAIL); }
/* Test the caller's flags BEFORE loading the diagnostic register: MOVE.L
   changes CCR. Skip the six-byte diagnostic and four-byte BRA on success. */
static void equal(unsigned number) { w(0x670a); error(number); branch(BRA, FAIL); }
static void different(unsigned number) { w(0x660a); error(number); branch(BRA, FAIL); }
static void flag(unsigned offset, uint32_t mask) {
    load(4, offset, 0); w(0x0080); l(mask); store(0, 4, offset);
}
static void strings(int expected, unsigned number) {
    addr(1, expected, 0); branch(BSR, STRCMP); nonzero(number);
}
static void read_expected(int file, int expected, unsigned size, unsigned number) {
    address_d(1, file); address_d(2, BUF); imm(3, size); branch(BSR, READ_FILE);
    nonzero(number); addr(0, BUF, 0); strings(expected, number + 1);
}
static void get_message(unsigned number) {
    execbase(); ma(3, 0); call(384); ma(3, 0); call(372); nonzero(number);
}
static void open_dos(void) {
    execbase(); addr(1, DOS_NAME, 0); imm(0, 36); call(552); da(0, 5); nonzero(101);
}
static void process_and_port(void) {
    execbase(); w(0x93c9); call(294); da(0, 2); nonzero(102);
    w(0x47ea); w(92);                                     /* lea 92(a2),a3 */
}

static void helpers(void) {
    mark(STRCMP); mark(STR_LOOP);
    w(0x1018);                                            /* move.b (a0)+,d0 */
    w(0xb019);                                            /* cmp.b (a1)+,d0 */
    branch(BNE, STR_BAD); w(0x4a00); branch(BNE, STR_LOOP);
    imm(0, 1); w(0x4e75);
    mark(STR_BAD); imm(0, 0); w(0x4e75);

    /* D1 path, D2 private >=16-byte buffer, D3 exact requested bytes.
       Preserve D4/D5. Every opened handle closes, even on short reads. */
    mark(READ_FILE); w(0x2f04); w(0x2f05); md(2, 5); md(3, 4);
    da(2, 0); w(0x4290); w(0x42a8); w(4); w(0x42a8); w(8); w(0x42a8); w(12);
    dosbase(); imm(2, 1005); call(30); testi(0); branch(BEQ, READ_BAD);
    w(0x2f00); md(0, 1); md(5, 2); md(4, 3); call(42); md(0, 5);
    w(0x221f); call(36); testi(0); branch(BEQ, READ_BAD);
    cmp(4, 5); branch(BNE, READ_BAD); imm(0, 1); branch(BRA, READ_END);
    mark(READ_BAD); imm(0, 0);
    mark(READ_END); w(0x2a1f); w(0x281f); w(0x4e75);
}

static void init(void) {
    unsigned i; memset(&b, 0, sizeof(b));
    for (i = 0; i < LABEL_LIMIT; ++i) b.label[i] = -1;
    mark(START);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static size_t finish(uint8_t *out, size_t capacity) {
    uint32_t words, at = 0; unsigned i;
    for (i = 0; i < b.fixes; ++i) {
        const struct Fix *f = &b.fix[i]; int32_t value;
        if (f->label < 0 || f->label >= LABEL_LIMIT || b.label[f->label] < 0) return 0;
        value = b.label[f->label] + f->add;
        if (f->relative) {
            value -= (int32_t)f->at;
            if (value < -32768 || value > 32767) return 0;
            b.code[f->at] = (uint8_t)((uint32_t)value >> 8);
            b.code[f->at + 1] = (uint8_t)value;
        } else put32(b.code + f->at, (uint32_t)value);
    }
    align4(); words = b.used / 4;
    if (b.bad || !b.reloc_count || capacity < b.used + (13 + b.reloc_count) * 4) return 0;
#define OUT(v) do { put32(out + at, (uint32_t)(v)); at += 4; } while (0)
    OUT(0x3f3); OUT(0); OUT(1); OUT(0); OUT(0); OUT(words);
    OUT(0x3e9); OUT(words); memcpy(out + at, b.code, b.used); at += b.used;
    OUT(0x3ec); OUT(b.reloc_count); OUT(0);
    for (i = 0; i < b.reloc_count; ++i) OUT(b.reloc[i]);
    OUT(0); OUT(0x3f2);
#undef OUT
    return at;
}

size_t df_build_root(uint8_t *out, size_t capacity, const char *child_name) {
    unsigned i;
    init(); savea(0, MSG, 24); open_dos(); addr(4, MSG, 0); process_and_port();
    storea(3, 4, 14); storea(2, 4, 76); ad(4, 0); store(0, 4, 28);
    dosbase(); call(492); nonzero(110); store(0, 4, 68); flag(48, 1);
    address_d(1, CHILD_FILE); dosbase(); call(150); nonzero(111); save(0, TAG_SEG, 0);
    address_d(1, CHILD_DIR); imm(2, (uint32_t)-2); call(84); nonzero(112); save(0, TAG_HOME, 0);
    address_d(1, TAGS); call(498); nonzero(113); save(0, CHILD_HANDLE, 0); da(0, 0);
    /* Destroy the caller's original guest argument source after typed CNP
       copied it. Child GetArgStr must still expose its private copy. */
    addr(0, CHILD_ARGS, 0);
    for (i = 0; i < 5; ++i) { w(0x20fc); l(0x58585858); } /* move.l #XXXX,(a0)+ */
    imm(1, DF_ROOT_IOERR); dosbase(); call(462);
    geta(CHILD_HANDLE, 0); w(0x41e8); w(92);             /* lea 92(a0),a0 */
    ma(4, 1); execbase(); call(366); get_message(114);
    store(0, 4, 36); ad(4, 1); cmp(1, 0); equal(115);
    load(4, 24, 0); store(0, 4, 44); load(4, 120, 0); cmpi(0, 0); equal(116);
    load(4, 52, 0); cmpi(0, 0x8d); equal(117);           /* CLI/dir/home/streams */
    dosbase(); call(132); store(0, 4, 56); cmpi(0, DF_ROOT_IOERR); equal(118); flag(48, 2);
    dosbase(); call(492); load(4, 68, 1); cmp(1, 0); equal(119);
    read_expected(ROOT_FILE, EXPECT_ROOT, 7, 120); flag(48, 4);
    geta(CHILD_HANDLE, 0); w(0x41e8); w(92); ma(4, 1); execbase(); call(366);
    imm(0, 55); w(0x4e75);
    mark(FAIL); geta(CHILD_HANDLE, 1); ad(1, 0); testi(0); branch(BEQ, FAIL_EXIT);
    execbase(); call(288);                               /* retire a waiting child before failing */
    mark(FAIL_EXIT); imm(0, 20); w(0x4e75);
    helpers();

    zero(MSG, DF_MESSAGE_SIZE);
    put32(b.code + b.label[MSG] + 20, 0x4b443031);        /* KD01 opaque protocol */
    b.code[b.label[MSG] + 18] = 0; b.code[b.label[MSG] + 19] = DF_MESSAGE_SIZE;
    zero(CHILD_HANDLE, 4);
    align4(); mark(TAGS);
    l(0x800003e9); mark(TAG_SEG); l(0);                  /* NP_Seglist */
    l(0x800003ea); l(1);                                /* NP_FreeSeglist */
    l(0x800003f3); l(4096);                             /* NP_StackSize */
    l(0x800003f4); fix(CHILD_NAME, 0, 0);                /* NP_Name */
    l(0x800003f5); l((uint32_t)-1);                     /* NP_Priority */
    l(0x800003f8); mark(TAG_HOME); l(0);                 /* NP_HomeDir */
    l(0x800003f9); l(1);                                /* NP_CopyVars */
    l(0x800003fa); l(1);                                /* NP_Cli */
    l(0x800003fc); fix(COMMAND, 0, 0);                   /* NP_CommandName */
    l(0x800003fd); fix(CHILD_ARGS, 0, 0);                /* NP_Arguments */
    l(0); l(0);
    text(CHILD_ARGS, DF_CHILD_ARGUMENTS); text(CHILD_NAME, child_name);
    text(COMMAND, "domain_child"); text(DOS_NAME, "dos.library");
    text(CHILD_FILE, "domain_child"); text(CHILD_DIR, "child");
    text(ROOT_FILE, "root-input"); text(EXPECT_ROOT, "ROOTDIR"); zero(BUF, 32);
    return finish(out, capacity);
}

size_t df_build_child(uint8_t *out, size_t capacity, const char *backend,
                      const char *absolute_failure_proof) {
    unsigned i;
    init(); process_and_port(); get_message(201); da(0, 4); open_dos();
    ad(4, 0); store(0, 4, 32); load(4, 28, 1); cmp(1, 0); equal(202);
    storea(2, 4, 72); dosbase(); call(492); nonzero(203); store(0, 4, 64);
    load(2, 172, 1); w(0xe589); cmp(1, 0); equal(204);    /* lsl.l #2,d1 */
    load(4, 68, 1); cmp(1, 0); different(205); flag(52, 1);
    read_expected(ROOT_FILE, EXPECT_ROOT, 7, 206);
    address_d(1, CHILD_DIR); imm(2, (uint32_t)-2); dosbase(); call(84); nonzero(208);
    md(0, 1); call(126); md(0, 1); call(90);
    read_expected(CHILD_INPUT, EXPECT_CHILD, 8, 209); flag(52, 4);
    dosbase(); call(600); nonzero(211); md(0, 2); load(2, 152, 1); call(420);
    cmpi(0, 0); equal(212); flag(52, 8);
    dosbase(); call(54); nonzero(213); load(2, 156, 1); cmp(1, 0); equal(214);
    call(60); nonzero(215); load(2, 160, 1); cmp(1, 0); equal(216);
    load(2, 224, 0); nonzero(217); flag(52, 0x80);
    imm(1, DF_CHILD_IOERR); dosbase(); call(462);
    imm(0, 1); save(0, PHASE, 0); ma(4, 1); execbase(); call(378);
    get_message(218); ad(4, 1); cmp(1, 0); equal(219);
    dosbase(); call(132); store(0, 4, 60); cmpi(0, DF_CHILD_IOERR); equal(220); flag(52, 2);

    imm(7, 999); mark(GATE_LOOP);
    address_d(1, GATE_FILE); address_d(2, BUF); imm(3, 6); branch(BSR, READ_FILE);
    testi(0); branch(BEQ, GATE_DELAY); addr(0, BUF, 0); addr(1, EXPECT_GATE, 0);
    branch(BSR, STRCMP); testi(0); branch(BNE, GATE_DONE);
    mark(GATE_DELAY); dosbase(); imm(1, 1); call(198); w(0x51cf); fix(GATE_LOOP, 0, 1);
    error(221); branch(BRA, FAIL);
    mark(GATE_DONE); imm(0, 0x47415445); store(0, 4, 80);

    dosbase(); call(534); nonzero(222); da(0, 0); ma(0, 3); strings(EXPECT_ARGS, 223);
    ma(3, 0); for (i = 0; i < 5; ++i) { w(0x2018); store(0, 4, 88 + i * 4); }
    flag(52, 0x10);
    loada(4, 24, 0); storea(0, 4, 40); ma(0, 3); strings(EXPECT_NESTED, 224);
    ma(3, 0); for (i = 0; i < 3; ++i) { w(0x2018); store(0, 4, 108 + i * 4); }
    flag(52, 0x60);
    address_d(1, VAR_NAME); address_d(2, BUF); imm(3, 16); imm(4, 0); dosbase(); call(906);
    error(225); testi(0); branch(BMI, FAIL); addr(0, BUF, 0); strings(EXPECT_BACKEND, 226); flag(52, 0x100);
    dosbase(); call(54); nonzero(227); load(2, 156, 1); cmp(1, 0); equal(228);
    call(60); nonzero(229); load(2, 160, 1); cmp(1, 0); equal(230);
    imm(0, 7); store(0, 4, 124);
    address_d(1, PROOF_FILE); imm(2, 1006); dosbase(); call(30); nonzero(231); md(0, 4);
    md(4, 1); ad(4, 2); imm(3, DF_MESSAGE_SIZE); call(48); cmpi(0, DF_MESSAGE_SIZE); equal(232);
    md(4, 1); call(36); nonzero(233); imm(1, 7); call(144); imm(0, 20); w(0x4e75);

    mark(FAIL); ad(4, 0); testi(0); branch(BEQ, FAIL_NO_REPLY); store(6, 4, 120);
    ad(5, 0); testi(0); branch(BEQ, FAIL_NO_DOS);
    save(6, FAIL_RECORD, 4); address_d(1, FAIL_FILE); imm(2, 1006); dosbase(); call(30);
    testi(0); branch(BEQ, FAIL_NO_DOS); md(0, 4); md(4, 1);
    address_d(2, FAIL_RECORD); imm(3, DF_MESSAGE_SIZE); call(48); md(4, 1); call(36);
    mark(FAIL_NO_DOS); addr(0, PHASE, 0); load(0, 0, 0); testi(0); branch(BNE, FAIL_NO_REPLY);
    ma(4, 1); execbase(); call(378);
    mark(FAIL_NO_REPLY); ad(5, 0); testi(0); branch(BEQ, FAIL_EXIT);
    dosbase(); imm(1, 20); call(144);
    mark(FAIL_EXIT); imm(0, 20); w(0x4e75);
    helpers();

    text(DOS_NAME, "dos.library"); text(ROOT_FILE, "root-input"); text(CHILD_DIR, "child");
    text(CHILD_INPUT, "child-input"); text(EXPECT_ROOT, "ROOTDIR"); text(EXPECT_CHILD, "CHILDDIR");
    text(EXPECT_ARGS, DF_CHILD_ARGUMENTS); text(EXPECT_NESTED, DF_ROOT_ARGUMENTS);
    text(EXPECT_BACKEND, backend); text(VAR_NAME, "KAFFEINE_CPU"); text(GATE_FILE, "release");
    text(EXPECT_GATE, "GATE1\n"); text(PROOF_FILE, "proof"); text(FAIL_FILE, absolute_failure_proof);
    zero(FAIL_RECORD, DF_MESSAGE_SIZE); put32(b.code + b.label[FAIL_RECORD], 0x4b444631);
    zero(PHASE, 4); zero(BUF, 32);
    return finish(out, capacity);
}
