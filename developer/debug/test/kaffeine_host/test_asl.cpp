/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The asl.library file requester of kaffeine_asl.c through the production
    dispatcher, CPU accessors and guest heap, with literal LVOs, against
    recorded native ASL calls that apply their tags as native ASL does: the
    vectors and their refusals, the root-only gate, the tag rules by number,
    the 68k FileRequester as a snapshot of the native one after every call,
    one request per requester, the Screen checked at each request, the
    retained string copies, memory failures before anything native, Free
    and cleanup. FindTask(), the shadow lookup and AllocVec() are
    collaborators that reproduce the production rules they stand for;
    AllocVec() can fail on demand.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_rastport.h"
#include "kaffeine_sched.h"
#include "kaffeine_asl.h"

#include <dos/dos.h>
#include <exec/tasks.h>
#include <intuition/screens.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>

static unsigned checks, failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    if (failures < 40) std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

enum : ULONG
{
    MEM_SIZE = 0x40000, HEAP_START = 0x10000, HEAP_END = 0x3F000,
    STACK = 0x1000, ASL_BASE = 0x4000, TAGS = 0x5000, TAGS2 = 0x5800, STR = 0x6000,
    SCREEN_OWNED = 0x8000, SCREEN_LOCKED = 0x8400, SCREEN_BORROWED = 0x8800,
    SCREEN_FORGED = 0x8C00, RETURN_PC = 0x76543210
};

/* ── collaborators ── */

/* the native tasks, only their node type as asl_stubs/exec/tasks.h has
   them: the root is a DOS Process unless a case makes it a plain Task; the
   other one stands for an identity only */
static struct Task root_native, other_native;
static struct Task *const root_task = &root_native;
static struct Task *const other_task = &other_native;
static struct Task *current_native = root_task;

extern "C" struct Task *FindTask(const char *name)
{
    return name ? nullptr : current_native;
}

/* kaffeine_rastport.c's shadow lookup and Kaffeine_ScreenNative() */
extern "C" void *Kaffeine_ShadowNative(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{
    if (!guest)
        return nullptr;
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == guest && ctx->shadow_map[i].type == type)
            return ctx->shadow_map[i].native_ptr;
    return nullptr;
}

extern "C" struct Screen *Kaffeine_ScreenNative(struct KaffeineContext *ctx, ULONG guest)
{
    struct Screen *scr = (struct Screen *)Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_SCREEN);
    return scr ? scr : (struct Screen *)Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_PUBSCREEN);
}

/* Kaffeine_AutoOpenLibs() of kaffeine_memory.c, which links it, is never called here */
extern "C" ULONG Kaffeine_OpenLibrary(struct KaffeineContext *, const char *) { std::abort(); }

static SIPTR io_err;
extern "C" SIPTR IoErr(void) { return io_err; }
extern "C" SIPTR SetIoErr(SIPTR value) { SIPTR old = io_err; io_err = value; return old; }

/* AllocVec(): calloc, counted, the n-th call failing when asked */
static std::set<void *> live_vec;
static int vec_calls, fail_vec_at = -1;

extern "C" void *AllocVec(unsigned long size, unsigned long)
{
    if (vec_calls++ == fail_vec_at)
        return nullptr;
    void *p = std::calloc(1, size);
    live_vec.insert(p);
    return p;
}

extern "C" void FreeVec(void *p)
{
    if (!p)
        return;
    CHECK(live_vec.erase(p) == 1);
    std::free(p);
}

/* ── native ASL, applying tags as ParseCommonTags() and FRTagHook() do ── */

struct RecTag { ULONG tag; IPTR data; bool text; std::string value; };
struct Native
{
    struct FileRequester fr;
    std::vector<std::string *> strings;
    std::set<IPTR> retained;   /* every common string pointer it was given */
};
static std::map<APTR, Native *> natives;
static std::vector<std::vector<RecTag>> alloc_lists, request_lists;
static unsigned alloc_calls, request_calls, free_calls;
static bool fail_native_alloc, request_early;
static std::function<BOOL(struct FileRequester *)> on_request;

static bool common_tag(ULONG tag)
{
    return tag == ASLFR_TitleText || tag == ASLFR_PositiveText || tag == ASLFR_NegativeText ||
           tag == ASLFR_PubScreenName;
}

static bool text_tag(ULONG tag)
{
    return common_tag(tag) || tag == ASLFR_InitialFile || tag == ASLFR_InitialDrawer;
}

static std::vector<RecTag> record(struct TagItem *tags)
{
    std::vector<RecTag> v;
    for (struct TagItem *t = tags; t && t->ti_Tag != TAG_DONE; t++)
    {
        RecTag r{ t->ti_Tag, t->ti_Data, text_tag(t->ti_Tag) && t->ti_Data, {} };
        if (r.text)
            r.value = (const char *)t->ti_Data;
        v.push_back(r);
    }
    return v;
}

static STRPTR own(Native *n, const char *s)
{
    n->strings.push_back(new std::string(s ? s : ""));
    return (STRPTR)n->strings.back()->c_str();
}

static void apply(Native *n, struct TagItem *tags)
{
    for (struct TagItem *t = tags; t && t->ti_Tag != TAG_DONE; t++)
    {
        switch (t->ti_Tag)
        {
            case ASLFR_InitialFile: n->fr.fr_File = own(n, (const char *)t->ti_Data); break;
            case ASLFR_InitialDrawer: n->fr.fr_Drawer = own(n, (const char *)t->ti_Data); break;
            case ASLFR_UserData: n->fr.fr_UserData = (APTR)t->ti_Data; break;
            case ASLFR_InitialLeftEdge: n->fr.fr_LeftEdge = (WORD)t->ti_Data; break;
            case ASLFR_InitialTopEdge: n->fr.fr_TopEdge = (WORD)t->ti_Data; break;
            case ASLFR_InitialWidth: n->fr.fr_Width = (WORD)t->ti_Data; break;
            case ASLFR_InitialHeight: n->fr.fr_Height = (WORD)t->ti_Data; break;
            default:
                if (common_tag(t->ti_Tag) && t->ti_Data)
                    n->retained.insert(t->ti_Data);
                break;
        }
    }
}

/* every common string native ASL may still point to is alive */
static bool retained_alive(Native *n)
{
    for (IPTR p : n->retained)
        if (!live_vec.count((void *)p))
            return false;
    return true;
}

extern "C" APTR AllocAslRequest(ULONG type, struct TagItem *tags)
{
    ++alloc_calls;
    alloc_lists.push_back(record(tags));
    if (type != ASL_FileRequest || fail_native_alloc)
        return nullptr;
    Native *n = new Native{};
    apply(n, tags);
    natives[&n->fr] = n;
    return &n->fr;
}

extern "C" BOOL AslRequest(APTR req, struct TagItem *tags)
{
    ++request_calls;
    request_lists.push_back(record(tags));
    CHECK(natives.count(req) == 1);
    Native *n = natives[req];
    if (request_early)
        return FALSE;           /* returns before it parses the tags */
    apply(n, tags);
    CHECK(retained_alive(n));
    return on_request ? on_request(&n->fr) : FALSE;
}

extern "C" void FreeAslRequest(APTR req)
{
    ++free_calls;
    CHECK(natives.count(req) == 1);
    Native *n = natives[req];
    CHECK(retained_alive(n));
    natives.erase(req);
    for (auto *s : n->strings)
        delete s;
    delete n;
}

/* a string that the native request itself owns, as a result of the dialog */
static STRPTR result_string(struct FileRequester *fr, const char *s)
{
    return own(natives[fr], s);
}

/* ── the CPU and the fixture ── */

struct TestCPU { ULONG d[8] = {}, a[8] = {}; UWORD sr = 0x2715; ULONG pc = 0; };
static ULONG get_d(void *p, int n) { return static_cast<TestCPU *>(p)->d[n]; }
static ULONG get_a(void *p, int n) { return static_cast<TestCPU *>(p)->a[n]; }
static void set_d(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->d[n] = v; }
static void set_a(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->a[n] = v; }
static UWORD get_sr(void *p) { return static_cast<TestCPU *>(p)->sr; }
static void set_sr(void *p, UWORD v) { static_cast<TestCPU *>(p)->sr = v; }
static ULONG get_pc(void *p) { return static_cast<TestCPU *>(p)->pc; }
static void set_pc(void *p, ULONG v) { static_cast<TestCPU *>(p)->pc = v; }
static void execute(void *) { std::abort(); }
static const KaffeineCPUOps cpu_ops = {
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

static BOOL root_yes(struct KaffeineContext *, void *) { return TRUE; }
static BOOL root_no(struct KaffeineContext *, void *) { return FALSE; }
static BOOL info_held(struct KaffeineContext *) { return TRUE; }

struct Fixture
{
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory = std::vector<UBYTE>(MEM_SIZE, 0);
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };
    struct Screen owned{}, locked{}, borrowed{};
    ULONG heap_initial = 0;

    Fixture()
    {
        ctx.mem = memory.data();
        ctx.mem_size = MEM_SIZE;
        ctx.heap_start = HEAP_START;
        ctx.heap_end = HEAP_END;
        CHECK(Kaffeine_HeapInit(&ctx));
        ctx.running = TRUE;
        ctx.exec_task = root_task;
        ctx.cpu = &cpu;
        current_native = root_task;
        root_task->tc_Node.ln_Type = NT_PROCESS;
        other_task->tc_Node.ln_Type = NT_PROCESS;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = ASL_BASE;
        lib.jt_start = ASL_BASE - 6u * KAFFEINE_MAX_LVO;
        std::strcpy(lib.name, "asl.library");
        lib.thunks = kaffeine_thunks_asl;
        lib.num_thunks = kaffeine_thunks_asl_count;
        lib.gen_thunks = nullptr;
        lib.num_gen_thunks = 0;
        screen(0, &owned, SCREEN_OWNED, TRUE, 0, KAFFEINE_SHADOW_SCREEN);
        screen(1, &locked, SCREEN_LOCKED, FALSE, 1, KAFFEINE_SHADOW_PUBSCREEN);
        screen(2, &borrowed, SCREEN_BORROWED, FALSE, 0, KAFFEINE_SHADOW_PUBSCREEN);
        natives.clear();
        alloc_lists.clear();
        request_lists.clear();
        alloc_calls = request_calls = free_calls = 0;
        fail_native_alloc = request_early = false;
        on_request = nullptr;
        vec_calls = 0;
        fail_vec_at = -1;
        io_err = 0;
        heap_initial = Kaffeine_HeapAvail(&ctx, 0);
    }

    /* whatever a case left, as the end of the program frees it */
    ~Fixture()
    {
        Kaffeine_AslCleanup(&ctx);
        CHECK(natives.empty() && live_vec.empty());
    }

    void screen(int i, struct Screen *native, ULONG guest, BOOL is_owned, UWORD locks, UWORD type)
    {
        ctx.pubscreens[i].native = native;
        ctx.pubscreens[i].guest = guest;
        ctx.pubscreens[i].owned = is_owned;
        ctx.pubscreens[i].locks = locks;
        ctx.shadow_map[ctx.num_shadows].m68k_addr = guest;
        ctx.shadow_map[ctx.num_shadows].native_ptr = native;
        ctx.shadow_map[ctx.num_shadows].type = type;
        ctx.num_shadows++;
    }

    ULONG str(ULONG at, const char *s)
    {
        std::memcpy(ctx.mem + at, s, std::strlen(s) + 1);
        return at;
    }

    ULONG tags(ULONG at, std::initializer_list<ULONG> items)
    {
        ULONG p = at;
        for (ULONG v : items) { m68k_write32(&ctx, p, v); p += 4; }
        return at;
    }

    ULONG call(ULONG lvo, ULONG a0 = 0, ULONG a1 = 0, ULONG d0 = 0)
    {
        for (int i = 0; i < 8; ++i) { state.d[i] = 0xd0d0d000u + i; state.a[i] = 0xa0a0a000u + i; }
        state.a[0] = a0;
        state.a[1] = a1;
        state.d[0] = d0;
        state.a[7] = STACK;
        m68k_write32(&ctx, STACK, RETURN_PC);
        KaffeineDispatchResult result = {};
        CHECK(Kaffeine_DispatchLibraryCall(&ctx, &cpu, ASL_BASE - lvo, &result));
        CHECK(result.return_pc == RETURN_PC && state.a[7] == STACK + 4);
        return state.d[0];
    }

    ULONG alloc(ULONG list) { return call(48, list, 0, ASL_FileRequest); }
    ULONG request(ULONG fr, ULONG list = 0) { return call(60, fr, list); }
    ULONG release(ULONG fr) { return call(54, fr); }

    bool stopped() const { return !ctx.running && ctx.exit_code == 20; }
    ULONG L(ULONG at) { return m68k_read32(&ctx, at); }
    UWORD W(ULONG at) { return m68k_read16(&ctx, at); }
    std::string S(ULONG at) { return at ? std::string((const char *)ctx.mem + at) : std::string("<0>"); }
    KaffeineAslRequest *record_of(ULONG guest)
    {
        for (auto &r : ctx.asl_requests)
            if (r.guest == guest && r.native)
                return &r;
        return nullptr;
    }
    /* the record's fields, -1 when there is no record */
    int consumed(ULONG guest) { auto *r = record_of(guest); return r ? r->consumed : -1; }
    int retained(ULONG guest) { auto *r = record_of(guest); return r ? r->retained : -1; }
    long screen_of(ULONG guest) { auto *r = record_of(guest); return r ? (long)r->screen : -1; }
    bool balanced() { return natives.empty() && live_vec.empty() && Kaffeine_HeapAvail(&ctx, 0) == heap_initial; }
};

static const RecTag *find(const std::vector<RecTag> &v, ULONG tag)
{
    const RecTag *last = nullptr;
    for (const auto &t : v)
        if (t.tag == tag)
            last = &t;
    return last;
}

/* the i-th recorded list, empty when there was none */
static const std::vector<RecTag> &nth(const std::vector<std::vector<RecTag>> &lists, size_t i)
{
    static const std::vector<RecTag> none;
    return i < lists.size() ? lists[i] : none;
}

/* the data or text of the last such tag, a marker when there is none */
static IPTR data_of(const std::vector<RecTag> &v, ULONG tag)
{
    const RecTag *t = find(v, tag);
    return t ? t->data : ~(IPTR)0;
}

static std::string value_of(const std::vector<RecTag> &v, ULONG tag)
{
    const RecTag *t = find(v, tag);
    return t ? t->value : std::string("<none>");
}

static unsigned count(const std::vector<RecTag> &v, ULONG tag)
{
    unsigned n = 0;
    for (const auto &t : v)
        n += t.tag == tag;
    return n;
}

/* the last two native tags: Window NULL, then the Screen */
static bool forced(const std::vector<RecTag> &v, IPTR screen)
{
    return v.size() >= 2 && v[v.size() - 2].tag == ASLFR_Window && !v[v.size() - 2].data &&
           v.back().tag == ASLFR_Screen && v.back().data == screen &&
           count(v, ASLFR_Window) == 1 && count(v, ASLFR_Screen) == 1;
}

/* AmiPixelShop's ask_file tags, at TAGS */
static ULONG aps_tags(Fixture &f, ULONG screen = SCREEN_OWNED)
{
    ULONG title = f.str(STR, "Save As (.aps, .png, .jpg, .iff)");
    ULONG drawer = f.str(STR + 0x100, "RAM:");
    ULONG file = f.str(STR + 0x200, "Untitled-1.aps");
    return f.tags(TAGS, { ASLFR_Screen, screen, ASLFR_TitleText, title, ASLFR_DoSaveMode, 1,
                          ASLFR_RejectIcons, 1, ASLFR_InitialDrawer, drawer,
                          ASLFR_InitialFile, file, ASLFR_InitialWidth, 520,
                          ASLFR_InitialHeight, 512, ASLFR_InitialLeftEdge, 252,
                          ASLFR_InitialTopEdge, 128, TAG_DONE, 0 });
}

/* ── 1. the table ── */

static void test_table()
{
    static const ULONG lvos[] = { 30, 36, 42, 48, 54, 60, 78, 84 };
    CHECK(kaffeine_thunks_asl_count == 8);
    for (ULONG i = 0; i < 8; ++i)
        CHECK(kaffeine_thunks_asl[i].lvo == lvos[i] && kaffeine_thunks_asl[i].thunk);
    CHECK(kaffeine_thunks_asl[8].thunk == nullptr);
    for (ULONG lvo : { 30u, 36u, 42u, 78u, 84u })
    {   /* named refusals, nothing native */
        Fixture f;
        CHECK(f.call(lvo, TAGS, TAGS) == 20 && f.stopped());
        CHECK(!alloc_calls && !request_calls && !free_calls && live_vec.empty());
    }
    {   /* a vector without a manual entry reaches no generated wrapper */
        Fixture f;
        CHECK(f.call(66) == 0 && f.ctx.running && !alloc_calls);
    }
}

/* ── 2. the root-only gate, on a DOS Process ── */

static void test_gate()
{
    static const KaffeineTaskOps child_ops = [] { KaffeineTaskOps o{}; o.is_root = root_no; return o; }();
    static const KaffeineTaskOps root_ops = [] { KaffeineTaskOps o{}; o.is_root = root_yes; return o; }();
    static const KaffeineLayerOps held = [] { KaffeineLayerOps o{}; o.info_locked = info_held; return o; }();
    for (int c = 0; c < 8; ++c)
        for (ULONG lvo : { 48u, 60u, 54u })
        {
            Fixture f;
            ULONG fr = f.alloc(aps_tags(f));
            CHECK(fr && f.ctx.running);
            unsigned calls = alloc_calls + request_calls + free_calls;
            KaffeineCPU other = { &cpu_ops, &f.state };
            switch (c)
            {
                case 0: current_native = other_task; break;
                case 1: f.ctx.cpu = &other; break;
                case 2: f.ctx.callback_depth = 1; break;
                case 3: f.ctx.broker_call = TRUE; break;
                case 4: f.ctx.sched = (struct KaffeineScheduler *)other_task; f.ctx.task_ops = &child_ops; break;
                case 5: f.ctx.layer_ops = &held; break;
                /* the right task, CPU and root, but a plain Task */
                case 6: root_task->tc_Node.ln_Type = NT_TASK; break;
                case 7: root_task->tc_Node.ln_Type = NT_TASK;
                        f.ctx.sched = (struct KaffeineScheduler *)other_task; f.ctx.task_ops = &root_ops; break;
            }
            CHECK(f.call(lvo, lvo == 48 ? TAGS : fr, 0, ASL_FileRequest) == 20 && f.stopped());
            CHECK(alloc_calls + request_calls + free_calls == calls);
            CHECK(f.consumed(fr) == 0);
            current_native = root_task;
            root_task->tc_Node.ln_Type = NT_PROCESS;
        }
    {   /* a broker owner too */
        Fixture f;
        f.ctx.broker_owner = other_task;
        CHECK(f.alloc(aps_tags(f)) == 20 && f.stopped() && !alloc_calls);
    }
    {   /* the root under a scheduler is the program */
        Fixture f;
        f.ctx.sched = (struct KaffeineScheduler *)other_task;
        f.ctx.task_ops = &root_ops;
        ULONG fr = f.alloc(aps_tags(f));
        CHECK(fr && f.ctx.running && alloc_calls == 1);
        CHECK(f.release(fr) == 0 && f.ctx.running && free_calls == 1);
        CHECK(f.balanced());
    }
}

/* ── 3. Alloc: native tags and the 68k FileRequester ── */

static void test_alloc_mirror()
{
    Fixture f;
    ULONG fr = f.alloc(aps_tags(f));
    CHECK(fr && f.ctx.running && alloc_calls == 1);
    const auto &v = nth(alloc_lists, 0);
    const RecTag *width = find(v, ASLFR_InitialWidth), *title = find(v, ASLFR_TitleText);
    CHECK(width && width->data == 520);                      /* a scalar, not a pointer */
    CHECK(data_of(v, ASLFR_InitialHeight) == 512 && data_of(v, ASLFR_DoSaveMode) == 1);
    CHECK(title && title->value == "Save As (.aps, .png, .jpg, .iff)");
    CHECK(title && live_vec.count((void *)title->data));      /* a native copy */
    CHECK(title && (title->data < (IPTR)f.ctx.mem || title->data >= (IPTR)f.ctx.mem + MEM_SIZE));
    const RecTag *file = find(v, ASLFR_InitialFile);
    CHECK(file && file->data == (IPTR)(f.ctx.mem + STR + 0x200));   /* cloned by native */
    CHECK(forced(v, 0));                                     /* no Screen between calls */
    /* the mirror: exact layout, big-endian, guest strings in its buffers */
    CHECK(f.L(fr + 4) == fr + 56 && f.S(f.L(fr + 4)) == "Untitled-1.aps");
    CHECK(f.L(fr + 8) == fr + 56 + 257 && f.S(f.L(fr + 8)) == "RAM:");
    CHECK(f.L(fr + 52) == 0);                                /* native NULL pattern */
    CHECK(f.W(fr + 22) == 252 && f.W(fr + 24) == 128 && f.W(fr + 26) == 520 && f.W(fr + 28) == 512);
    CHECK(f.L(fr + 32) == 0 && f.L(fr + 36) == 0 && f.L(fr + 40) == 0);
    for (ULONG i : { 0u, 1u, 2u, 3u, 12u, 21u, 30u, 31u, 44u, 51u })
        CHECK(f.memory[fr + i] == 0);
    CHECK(f.record_of(fr) && f.screen_of(fr) == SCREEN_OWNED && f.consumed(fr) == 0);
    CHECK(f.retained(fr) == 1);
    CHECK(f.release(fr) == 0 && f.balanced());
}

/* ── 4. the one request: TRUE, FALSE, early return, then refused ── */

static void test_one_request()
{
    {   /* accept: the native results in the mirror, the screen at the end */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        on_request = [](struct FileRequester *n) {
            n->fr_File = result_string(n, "picture.png");
            n->fr_Drawer = result_string(n, "Work:Art");
            n->fr_LeftEdge = 10; n->fr_TopEdge = 20; n->fr_Width = 600; n->fr_Height = 400;
            return (BOOL)TRUE;
        };
        CHECK(f.request(fr) == 1 && f.ctx.running && request_calls == 1);
        const auto &v = nth(request_lists, 0);
        CHECK(forced(v, (IPTR)&f.owned));
        CHECK(!find(v, ASLFR_InitialFile) && !find(v, ASLFR_TitleText));  /* no replay */
        CHECK(f.S(f.L(fr + 4)) == "picture.png" && f.S(f.L(fr + 8)) == "Work:Art");
        CHECK(f.W(fr + 22) == 10 && f.W(fr + 26) == 600);
        CHECK(f.consumed(fr) == 1);
        CHECK(f.release(fr) == 0 && f.balanced());
    }
    for (ULONG list : { 0u, (ULONG)TAGS })
    {   /* a second request after TRUE is refused before anything native */
        Fixture g;
        ULONG gr = g.alloc(aps_tags(g));
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(g.request(gr) == 1);
        CHECK(g.request(gr, list) == 20 && g.stopped() && request_calls == 1);
    }
    {   /* cancel: what native changed, also on FALSE */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        ULONG file = f.str(STR + 0x300, "new.txt");
        ULONG list = f.tags(TAGS2, { ASLFR_InitialFile, file, ASLFR_UserData, 0x12345678,
                                     ASLFR_InitialWidth, 600, TAG_DONE, 0 });
        on_request = [](struct FileRequester *n) { n->fr_LeftEdge = 33; return (BOOL)FALSE; };
        CHECK(f.request(fr, list) == 0 && f.ctx.running);
        CHECK(f.S(f.L(fr + 4)) == "new.txt" && f.L(fr + 40) == 0x12345678);
        CHECK(f.W(fr + 26) == 600 && f.W(fr + 22) == 33);
        CHECK(f.request(fr) == 20 && f.stopped() && request_calls == 1);
        CHECK(f.record_of(fr) != nullptr);
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(f.balanced());
    }
    {   /* a native return before parsing: the mirror stays the native state */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        request_early = true;
        ULONG title = f.str(STR + 0x300, "Other title");
        ULONG list = f.tags(TAGS2, { ASLFR_TitleText, title, TAG_DONE, 0 });
        CHECK(f.request(fr, list) == 0 && f.ctx.running);
        CHECK(f.S(f.L(fr + 4)) == "Untitled-1.aps" && f.retained(fr) == 2);
        CHECK(f.request(fr) == 20 && f.stopped() && request_calls == 1);
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(f.balanced());
    }
    {   /* every new dialog is a new requester */
        Fixture f;
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        for (int i = 0; i < 3; ++i)
        {
            ULONG fr = f.alloc(aps_tags(f));
            CHECK(fr && f.request(fr) == 1 && f.release(fr) == 0 && f.ctx.running);
        }
        CHECK(alloc_calls == 3 && request_calls == 3 && free_calls == 3 && f.balanced());
    }
}

/* ── 5. tags by number ── */

static void test_tags()
{
    static const ULONG refused[] = {
        ASLFR_Window, ASLFR_SleepWindow, ASLFR_HookFunc, ASLFR_FilterFunc, ASLFR_IntuiMsgFunc,
        ASLFR_Flags1, ASLFR_Flags2, ASLFR_RejectPattern, ASLFR_AcceptPattern,
        ASLFR_InitialPattern, ASLFR_SetSortBy, ASLFR_GetSortBy, ASLFR_SetSortDrawers,
        ASLFR_GetSortDrawers, ASLFR_SetSortOrder, ASLFR_GetSortOrder, ASL_TB + 99, TAG_USER + 1
    };
    for (ULONG tag : refused)
        for (bool in_request : { false, true })
        {
            Fixture f;
            ULONG fr = in_request ? f.alloc(0) : 0;
            ULONG list = f.tags(TAGS2, { tag, 0, TAG_DONE, 0 });
            CHECK((in_request ? f.request(fr, list) : f.alloc(list)) == 20 && f.stopped());
            CHECK(alloc_calls == (in_request ? 1u : 0u) && !request_calls);
            if (in_request)
                CHECK(f.consumed(fr) == 0);
        }
    for (ULONG tag : { (ULONG)ASLFR_DoMultiSelect, (ULONG)ASLFR_DoPatterns, (ULONG)ASLFR_Locale,
                       (ULONG)ASLFR_TextAttr })
    {   /* only 0 */
        {
            Fixture f;
            CHECK(f.alloc(f.tags(TAGS2, { tag, 0, TAG_DONE, 0 })) != 0 && f.ctx.running);
        }
        Fixture g;
        CHECK(g.alloc(g.tags(TAGS2, { tag, 1, TAG_DONE, 0 })) == 20 && g.stopped() && !alloc_calls);
    }
    {   /* the scalars pass as given */
        Fixture f;
        ULONG list = f.tags(TAGS2, { ASLFR_DrawersOnly, 1, ASLFR_FilterDrawers, 1,
                                     ASLFR_InitialShowVolumes, 1, ASLFR_PopToFront, 0,
                                     ASLFR_Activate, 1, ASLFR_PrivateIDCMP, 1,
                                     ASLFR_UserData, 0xCAFEF00D, ASLFR_InitialLeftEdge, 0xFFFFFFFF,
                                     TAG_DONE, 0 });
        ULONG fr = f.alloc(list);
        CHECK(fr && data_of(nth(alloc_lists, 0), ASLFR_UserData) == 0xCAFEF00D);
        CHECK(data_of(nth(alloc_lists, 0), ASLFR_InitialLeftEdge) == 0xFFFFFFFF);
        CHECK(f.L(fr + 40) == 0xCAFEF00D);
    }
    {   /* TAG_IGNORE, TAG_SKIP and TAG_MORE as NextTagItem() takes them */
        Fixture f;
        ULONG more = f.tags(TAGS2, { ASLFR_InitialWidth, 300, TAG_DONE, 0 });
        ULONG list = f.tags(TAGS, { TAG_IGNORE, 7, TAG_SKIP, 1, ASLFR_Window, 1,
                                    ASLFR_InitialHeight, 200, TAG_MORE, more });
        CHECK(f.alloc(list) && f.ctx.running);
        CHECK(data_of(nth(alloc_lists, 0), ASLFR_InitialWidth) == 300 &&
              data_of(nth(alloc_lists, 0), ASLFR_InitialHeight) == 200 && forced(nth(alloc_lists, 0), 0));
    }
    {   /* duplicates: the last non-NULL common string once, NULL alone nothing */
        Fixture f;
        ULONG a = f.str(STR, "first"), b = f.str(STR + 0x40, "second");
        ULONG list = f.tags(TAGS2, { ASLFR_TitleText, a, ASLFR_TitleText, b, ASLFR_TitleText, 0,
                                     ASLFR_PositiveText, 0, TAG_DONE, 0 });
        ULONG fr = f.alloc(list);
        CHECK(fr && count(nth(alloc_lists, 0), ASLFR_TitleText) == 1);
        CHECK(value_of(nth(alloc_lists, 0), ASLFR_TitleText) == "second");
        CHECK(!find(nth(alloc_lists, 0), ASLFR_PositiveText) && f.retained(fr) == 1);
    }
    {   /* duplicate Screen tags: the last one, 0 included */
        Fixture f;
        ULONG fr = f.alloc(f.tags(TAGS2, { ASLFR_Screen, SCREEN_OWNED, ASLFR_Screen, 0, TAG_DONE, 0 }));
        CHECK(fr && f.screen_of(fr) == 0);
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(f.request(fr) == 1 && forced(nth(request_lists, 0), 0));
    }
    {   /* across TAG_MORE */
        Fixture f;
        ULONG a = f.str(STR, "one"), b = f.str(STR + 0x40, "two");
        ULONG more = f.tags(TAGS2, { ASLFR_TitleText, b, TAG_DONE, 0 });
        CHECK(f.alloc(f.tags(TAGS, { ASLFR_TitleText, a, TAG_MORE, more })));
        CHECK(count(nth(alloc_lists, 0), ASLFR_TitleText) == 1 &&
              value_of(nth(alloc_lists, 0), ASLFR_TitleText) == "two");
    }
    /* malformed lists stop before anything native */
    {
        Fixture f;
        CHECK(f.alloc(TAGS + 1) == 20 && f.stopped() && !alloc_calls);
    }
    {
        Fixture g;
        CHECK(g.alloc(MEM_SIZE - 4) == 20 && g.stopped() && !alloc_calls);
    }
    {
        Fixture h;
        h.tags(TAGS, { TAG_MORE, TAGS });          /* a cycle */
        CHECK(h.alloc(TAGS) == 20 && h.stopped() && !alloc_calls);
    }
    {
        Fixture k;
        k.tags(TAGS, { TAG_SKIP, 0x7fffffff });
        CHECK(k.alloc(TAGS) == 20 && k.stopped() && !alloc_calls);
    }
    {
        Fixture m;
        ULONG p = TAGS;
        for (int i = 0; i < 64; ++i, p += 8) m.tags(p, { TAG_IGNORE, 0 });
        m.tags(p, { TAG_DONE, 0 });
        CHECK(m.alloc(TAGS) == 20 && m.stopped() && !alloc_calls);   /* 65 steps */
    }
    {
        Fixture n;
        ULONG p = TAGS;
        for (int i = 0; i < 63; ++i, p += 8) n.tags(p, { TAG_IGNORE, 0 });
        n.tags(p, { TAG_DONE, 0 });
        CHECK(n.alloc(TAGS) && n.ctx.running);                       /* 64 */
    }
    {   /* string limits */
        std::string t256(256, 't'), t257(257, 't'), d1024(1024, 'd'), d1025(1025, 'd');
        struct Case { ULONG tag; const std::string *s; bool ok; };
        const Case cases[] = {
            { ASLFR_TitleText, &t256, true }, { ASLFR_TitleText, &t257, false },
            { ASLFR_PubScreenName, &t257, false }, { ASLFR_InitialFile, &t256, true },
            { ASLFR_InitialFile, &t257, false }, { ASLFR_InitialDrawer, &d1024, true },
            { ASLFR_InitialDrawer, &d1025, false },
        };
        for (const auto &c : cases)
        {
            Fixture f;
            f.str(STR, c.s->c_str());
            ULONG r = f.alloc(f.tags(TAGS2, { c.tag, STR, TAG_DONE, 0 }));
            CHECK(c.ok ? r != 0 && f.ctx.running : r == 20 && f.stopped() && !alloc_calls);
        }
    }
    {
        Fixture f;                                  /* no NUL before the end of memory */
        std::memset(f.ctx.mem + MEM_SIZE - 8, 'x', 8);
        CHECK(f.alloc(f.tags(TAGS2, { ASLFR_TitleText, MEM_SIZE - 8, TAG_DONE, 0 })) == 20 &&
              f.stopped() && !alloc_calls);
    }
}

/* ── 6. screens ── */

static void test_screens()
{
    for (ULONG scr : { (ULONG)SCREEN_OWNED, (ULONG)SCREEN_LOCKED })
    {
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f, scr));
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(fr && forced(nth(alloc_lists, 0), 0) && f.request(fr) == 1);
        CHECK(forced(nth(request_lists, 0), (IPTR)(scr == SCREEN_OWNED ? &f.owned : &f.locked)));
    }
    for (ULONG scr : { (ULONG)SCREEN_BORROWED, (ULONG)SCREEN_FORGED })
    {   /* held by nothing of the program, or no screen at all */
        Fixture f;
        CHECK(f.alloc(aps_tags(f, scr)) == 20 && f.stopped() && !alloc_calls);
    }
    {   /* an owned screen whose typed shadow is not that of an opened one */
        Fixture f;
        f.ctx.shadow_map[0].type = KAFFEINE_SHADOW_PUBSCREEN;
        CHECK(f.alloc(aps_tags(f)) == 20 && f.stopped() && !alloc_calls);
    }
    {   /* closed after Alloc: refused at the request, freed all the same */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        f.ctx.pubscreens[0].native = nullptr;
        f.ctx.pubscreens[0].guest = 0;
        CHECK(f.request(fr) == 20 && f.stopped() && !request_calls && f.consumed(fr) == 0);
        f.ctx.running = TRUE;
        CHECK(f.release(fr) == 0 && f.balanced());
    }
    {   /* a lock given back after Alloc */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f, SCREEN_LOCKED));
        f.ctx.pubscreens[1].locks = 0;
        CHECK(f.request(fr) == 20 && f.stopped() && !request_calls);
    }
    {   /* a Screen in the request's own list replaces the stored one */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(f.request(fr, f.tags(TAGS2, { ASLFR_Screen, SCREEN_LOCKED, TAG_DONE, 0 })) == 1);
        CHECK(forced(nth(request_lists, 0), (IPTR)&f.locked) && f.screen_of(fr) == SCREEN_LOCKED);
    }
}

/* ── 7. retained copies ── */

static void test_retained()
{
    {   /* every copy kept until Free, the request's included */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        ULONG a = f.str(STR + 0x300, "OK"), b = f.str(STR + 0x340, "No"),
              c = f.str(STR + 0x380, "Another"), d = f.str(STR + 0x3c0, "Workbench");
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(f.request(fr, f.tags(TAGS2, { ASLFR_PositiveText, a, ASLFR_NegativeText, b,
                                            ASLFR_TitleText, c, ASLFR_PubScreenName, d,
                                            TAG_DONE, 0 })) == 1);
        CHECK(f.retained(fr) == 5 && live_vec.size() == 5);
        CHECK(f.release(fr) == 0 && f.balanced());   /* FreeAslRequest saw them alive */
    }
    {   /* fault injection: the cap of 16, unreachable with real calls (at most
           8: four at Alloc, four at the one request), set in the record */
        Fixture f;
        ULONG fr = f.alloc(0);
        KaffeineAslRequest *r = f.record_of(fr);
        CHECK(r != nullptr);
        if (!r)
            return;
        while (r->retained < 15)
            r->kept[r->retained++] = (char *)AllocVec(1, 0);
        ULONG a = f.str(STR, "x"), b = f.str(STR + 0x40, "y");
        CHECK(f.request(fr, f.tags(TAGS2, { ASLFR_TitleText, a, ASLFR_PositiveText, b,
                                            TAG_DONE, 0 })) == 0);
        CHECK(f.ctx.running && io_err == ERROR_NO_FREE_STORE && !request_calls && !r->consumed);
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(f.request(fr, f.tags(TAGS2, { ASLFR_TitleText, a, TAG_DONE, 0 })) == 1);
        CHECK(r->retained == 16 && r->consumed);
        CHECK(f.release(fr) == 0 && f.balanced());
    }
}

/* ── 8. memory failures before anything native ── */

static void test_memory()
{
    {   /* a copy at Alloc */
        Fixture f;
        aps_tags(f);
        fail_vec_at = 0;
        CHECK(f.alloc(TAGS) == 0 && f.ctx.running && io_err == ERROR_NO_FREE_STORE);
        CHECK(!alloc_calls && f.balanced());
    }
    {   /* a copy at the request leaves the one request */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        ULONG a = f.str(STR + 0x300, "OK"), b = f.str(STR + 0x340, "No");
        ULONG list = f.tags(TAGS2, { ASLFR_PositiveText, a, ASLFR_NegativeText, b, TAG_DONE, 0 });
        fail_vec_at = vec_calls + 1;               /* the second copy */
        CHECK(f.request(fr, list) == 0 && f.ctx.running && io_err == ERROR_NO_FREE_STORE);
        CHECK(!request_calls && f.consumed(fr) == 0 && f.retained(fr) == 1);
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        CHECK(f.request(fr, list) == 1 && request_calls == 1);
        CHECK(f.release(fr) == 0 && f.balanced());
    }
    {   /* the mirror */
        Fixture f;
        for (ULONG size : { 0x1000u, 0x100u, 0x10u })
            while (Kaffeine_HeapAlloc(&f.ctx, size, 0)) {}
        ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
        aps_tags(f);
        CHECK(f.alloc(TAGS) == 0 && f.ctx.running && io_err == ERROR_NO_FREE_STORE && !alloc_calls);
        CHECK(live_vec.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
    }
    {   /* the table: eight requesters, then none */
        Fixture f;
        ULONG frs[KAFFEINE_MAX_ASL_REQUESTS];
        for (auto &fr : frs)
            CHECK((fr = f.alloc(0)) != 0);
        CHECK(f.alloc(0) == 0 && f.ctx.running && io_err == ERROR_NO_FREE_STORE);
        CHECK(alloc_calls == KAFFEINE_MAX_ASL_REQUESTS);
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(f.balanced());
    }
    {   /* native Alloc fails: nothing stays */
        Fixture f;
        fail_native_alloc = true;
        CHECK(f.alloc(aps_tags(f)) == 0 && f.ctx.running && alloc_calls == 1 && f.balanced());
    }
}

/* ── 9. snapshot limits ── */

static void test_snapshot_limits()
{
    {   /* a native multiple selection the mirror cannot show */
        Fixture f;
        ULONG fr = f.alloc(0);
        on_request = [](struct FileRequester *n) { n->fr_NumArgs = 1; return (BOOL)TRUE; };
        CHECK(f.request(fr) == 20 && f.stopped());
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(f.balanced());
    }
    {   /* a native name longer than its buffer */
        Fixture f;
        ULONG fr = f.alloc(0);
        on_request = [](struct FileRequester *n) {
            n->fr_File = result_string(n, std::string(257, 'f').c_str());
            return (BOOL)TRUE;
        };
        CHECK(f.request(fr) == 20 && f.stopped());
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(f.balanced());
    }
    {   /* the bridge never reads its mirror back */
        Fixture f;
        ULONG fr = f.alloc(aps_tags(f));
        for (ULONG i = 0; i < 56; i += 4)
            m68k_write32(&f.ctx, fr + i, 0xDEADBEEF);
        on_request = [](struct FileRequester *n) { n->fr_File = result_string(n, "a"); return (BOOL)TRUE; };
        CHECK(f.request(fr) == 1 && f.S(f.L(fr + 4)) == "a" && f.L(fr + 52) == 0);
        CHECK(f.release(fr) == 0 && f.balanced());
    }
}

/* ── 10. Free and cleanup ── */

static void test_free()
{
    {
        Fixture f;
        CHECK(f.release(0) == 0 && f.ctx.running && !free_calls);
        ULONG fr = f.alloc(0);
        CHECK(f.release(fr + 4) == 20 && f.stopped() && !free_calls);   /* inside a mirror */
    }
    {
        Fixture f;
        CHECK(f.release(TAGS) == 20 && f.stopped());                    /* unknown */
    }
    {
        Fixture f;
        ULONG fr = f.alloc(0);
        CHECK(f.release(fr) == 0 && free_calls == 1);
        CHECK(f.release(fr) == 20 && f.stopped() && free_calls == 1);  /* twice */
    }
    {   /* cleanup of several, consumed or not, and again */
        Fixture f;
        on_request = [](struct FileRequester *) { return (BOOL)TRUE; };
        ULONG a = f.alloc(aps_tags(f)), b = f.alloc(aps_tags(f, SCREEN_LOCKED)), c = f.alloc(0);
        CHECK(a && b && c && f.request(b) == 1);
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(free_calls == 3 && f.balanced());
        Kaffeine_AslCleanup(&f.ctx);
        CHECK(free_calls == 3);
        for (const auto &r : f.ctx.asl_requests)
            CHECK(!r.guest && !r.native && !r.retained);
    }
}

int main()
{
    test_table();
    test_gate();
    test_alloc_mirror();
    test_one_request();
    test_tags();
    test_screens();
    test_retained();
    test_memory();
    test_snapshot_limits();
    test_free();
    std::printf("asl.library file requester: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
