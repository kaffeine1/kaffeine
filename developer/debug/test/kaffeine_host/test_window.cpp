/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The windows, IDCMP ports and messages of 68k code through literal LVOs,
    the real dispatcher and heap: the 68k Window of a native one, which
    comes whole or not at all, the 68k MsgPort of an IDCMP port, 68k
    IntuiMessages from GetMsg(), WaitPort() and GT_GetIMsg() with every
    field and the window brought up to date, replies that free them, what
    WaitPort() showed of a message that something native took since,
    ModifyIDCMP() with the port that comes and goes and its undo, the
    GadTools copies of GT_FilterIMsg() and GT_PostFilterIMsg(), messages of
    GadTools only, CloseWindow() giving back every message that the program
    has not replied, 68k ports that no message can reach yet, and
    EasyRequests formatted by the bridge, with their requester windows as
    68k Windows. The native Exec, Intuition and GadTools below act as AROS
    does and record every call.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include <exec/memory.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"

/* These fixtures borrow metadata-only fonts and acquire no native opens. */
extern "C" void CloseFont(struct TextFont *) { assert(false); }
#include "kaffeine_gadgets.h"
#include "kaffeine_layers.h"
#include "kaffeine_window.h"
#include "kaffeine_dispatch.h"

/* ── The native side ─────────────────────────────────────────────── */

static std::map<MsgPort *, std::deque<IntuiMessage *>> queues;
static std::vector<IntuiMessage *> replied, made;
static std::vector<Window *> closed;
static unsigned native_calls, gt_live;
static bool fail_modify;
static ULONG gt_only;
static MsgPort ports[4];
static unsigned next_port;
static KaffeineContext *live;
static UWORD shadows_at_close;
static std::vector<Screen *> released_screens;
static Window *releasing_window;

/* A recording gadget bridge. The real gadget lists and their mirrors are
   tested in test_gadgets; here these hooks expose the window/message order. */
struct WindowGadgetCall
{
    std::string name, call;
    ULONG guest;
    Window *window;
    Gadget *head;
};
static std::vector<WindowGadgetCall> window_gadget_calls;
static std::vector<std::string> window_events;
static void record_screen_release(KaffeineContext *ctx, Screen *screen)
{
    assert(ctx == live && releasing_window && screen == releasing_window->WScreen);
    // The type-1 Window must already be gone when the screen can go.
    assert(!Kaffeine_ShadowGuest(ctx, releasing_window, KAFFEINE_SHADOW_WINDOW));
    released_screens.push_back(screen);
    window_events.push_back("screen_release");
}
static Gadget *prepared_head, *sync_native, *filter_changes;
static Gadget *filtered_address;
static std::vector<APTR> filter_saw;   /* the IAddress of each message that GadTools filters */
static ULONG sync_guest;
static bool prepare_refuses;

static Gadget *gadget_prepare(KaffeineContext *ctx, ULONG guest, const char *call)
{
    assert(ctx == live);
    window_events.push_back("prepare");
    window_gadget_calls.push_back({ "prepare", call, guest, nullptr, nullptr });
    if (prepare_refuses)
    {
        ctx->running = FALSE;
        ctx->exit_code = 20;
        return nullptr;
    }
    return prepared_head;
}

/* Off only where the 68k Window could not be made: the window takes the
   list and gives it back as it closes again at once. */
static bool expect_guest_window = true;

static void gadget_opened(KaffeineContext *ctx, Window *w, Gadget *head)
{
    assert(ctx == live && (!expect_guest_window || Kaffeine_ShadowGuest(ctx, w, KAFFEINE_SHADOW_WINDOW)));
    Gadget *at = w->FirstGadget;
    while (at && at != head) at = at->NextGadget;
    assert(at == head); // Intuition must have appended it before this hook
    window_events.push_back("opened");
    window_gadget_calls.push_back({ "opened", "", 0, w, head });
}

static void gadget_closing(KaffeineContext *ctx, Window *w)
{
    // The guest Window must still exist when the gadget bridge detaches it.
    assert(ctx == live && (!expect_guest_window || Kaffeine_ShadowGuest(ctx, w, KAFFEINE_SHADOW_WINDOW)));
    window_events.push_back("closing");
    window_gadget_calls.push_back({ "closing", "", 0, w, nullptr });
}

/* head: the address of the message, whose gadget may have come off w */
static void gadget_sync(KaffeineContext *ctx, Window *w, APTR address)
{
    assert(ctx == live && w);
    window_events.push_back("sync");
    window_gadget_calls.push_back({ "sync", "", 0, w, static_cast<Gadget *>(address) });
    if (sync_guest)
    {
        assert(sync_native);
        m68k_write16(ctx, sync_guest + 4, (UWORD)sync_native->LeftEdge);
    }
}

/* the messages that GadTools filters, as the hook saw them */
static std::vector<IntuiMessage> filtering_calls;

static void gadget_filtering(KaffeineContext *ctx, IntuiMessage *im)
{
    assert(ctx == live && im && im->IDCMPWindow);
    window_events.push_back("filtering");
    filtering_calls.push_back(*im);
}

/* the window did not open with the list that prepare gave */
static void gadget_dropped(KaffeineContext *ctx)
{
    assert(ctx == live);
    window_events.push_back("dropped");
    window_gadget_calls.push_back({ "dropped", "", 0, nullptr, nullptr });
}

/* the 68k gadget of a native one that a message names */
static ULONG gadget_guest(KaffeineContext *ctx, const void *native)
{
    assert(ctx == live);
    return Kaffeine_ShadowGuest(ctx, native, KAFFEINE_SHADOW_GADGET);
}

static const KaffeineWindowGadgets recording_gadgets =
    { gadget_prepare, gadget_opened, gadget_dropped, gadget_closing, gadget_sync, gadget_filtering,
      gadget_guest };

/* GadTools hands out a copy of each message, as AROS's GT_IntuiMessage. */
struct GTMessage
{
    IntuiMessage imsg;
    IntuiMessage *orig;
};

static IntuiMessage *message(Window *w, ULONG cls)
{
    auto *im = new IntuiMessage{};
    made.push_back(im);
    im->ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
    im->ExecMessage.mn_Node.ln_Pri = -3;
    im->ExecMessage.mn_Length = 92;
    im->Class = cls;
    im->Code = 0x1234;
    im->Qualifier = 0x8001;
    im->MouseX = -5;
    im->MouseY = 300;
    im->Seconds = 1000;
    im->Micros = 999999;
    im->IDCMPWindow = w;
    return im;
}

/* Something native takes the first message of a port and frees it, as
   SysReqHandler() does with the messages of a requester. */
static void take_natively(MsgPort *port)
{
    IntuiMessage *im = queues[port].front();
    queues[port].pop_front();
    made.erase(std::find(made.begin(), made.end(), im));
    delete im;
}

/* workbench.library is not linked here: no port has App objects. */
extern "C" BOOL Kaffeine_AppPortFill(KaffeineContext *, ULONG, BOOL) { return FALSE; }
extern "C" BOOL Kaffeine_AppReplyMsg(KaffeineContext *, ULONG) { return FALSE; }

extern "C" struct Message *GetMsg(struct MsgPort *port)
{
    ++native_calls;
    window_events.push_back("GetMsg");
    auto &q = queues[port];
    if (q.empty())
        return nullptr;
    IntuiMessage *im = q.front();
    q.pop_front();
    return &im->ExecMessage;
}

extern "C" struct Message *WaitPort(struct MsgPort *port)
{
    ++native_calls;
    window_events.push_back("WaitPort");
    assert(!queues[port].empty());
    return &queues[port].front()->ExecMessage;
}

extern "C" void ReplyMsg(struct Message *message)
{
    ++native_calls;
    window_events.push_back("ReplyMsg");
    message->mn_Node.ln_Type = NT_REPLYMSG;
    replied.push_back(reinterpret_cast<IntuiMessage *>(message));
}

extern "C" void PutMsg(struct MsgPort *, struct Message *) { assert(false); }
extern "C" void RawPutChar(UBYTE) { assert(false); }

/* As AROS: a window that gets flags gets a port, and loses it with them. */
extern "C" BOOL ModifyIDCMP(struct Window *window, ULONG flags)
{
    ++native_calls;
    if (fail_modify)
        return FALSE;
    if (!window->IDCMPFlags && flags && !window->UserPort)
    {
        window->UserPort = &ports[next_port++];
        window->UserPort->mp_SigBit = 20 + next_port;
    }
    window->IDCMPFlags = flags;
    if (!flags)
        window->UserPort = nullptr;
    return TRUE;
}

/* Intuition opens a window from one flat native tag list. */
static std::vector<TagItem> opened_tags;
static Window *open_result;
static unsigned opened;
extern "C" struct Window *OpenWindowTagList(struct NewWindow *newWindow, struct TagItem *tags)
{
    ++native_calls;
    window_events.push_back("OpenWindowTagList");
    ++opened;
    assert(!newWindow);
    opened_tags.clear();
    for (TagItem *t = tags;; ++t)
    {
        opened_tags.push_back(*t);
        if (t->ti_Tag == TAG_DONE)
            break;
    }
    if (open_result)
    {
        Gadget *head = nullptr;
        for (const TagItem &t : opened_tags)
            if (t.ti_Tag == WA_Gadgets)
                head = reinterpret_cast<Gadget *>(t.ti_Data);
        // openwindow.c appends WA_Gadgets after the system gadgets.
        Gadget **slot = &open_result->FirstGadget;
        while (*slot) slot = &(*slot)->NextGadget;
        *slot = head;
    }
    return open_result;
}

/* As AROS: the messages still in the port go back with the window. */
extern "C" void CloseWindow(struct Window *window)
{
    ++native_calls;
    window_events.push_back("CloseWindow");
    closed.push_back(window);
    shadows_at_close = live->num_shadows;
    if (window->UserPort)
        while (struct Message *msg = GetMsg(window->UserPort))
        {
            auto *im = reinterpret_cast<IntuiMessage *>(msg);
            im->IDCMPWindow = nullptr;
            im->Code = 0;
            im->Qualifier = 0;
            ReplyMsg(msg);
        }
}

/* GadTools keeps the messages of the class gt_only for itself, as AROS's
   does with those of an active scroller. */
extern "C" struct IntuiMessage *GT_FilterIMsg(struct IntuiMessage *imsg)
{
    ++native_calls;
    window_events.push_back("GT_FilterIMsg");
    filter_saw.push_back(imsg->IAddress);
    // A class can change a gadget even when GadTools consumes the message.
    if (filter_changes)
        filter_changes->LeftEdge = (WORD)(filter_changes->LeftEdge + 7);
    if (imsg->Class == gt_only)
        return nullptr;
    auto *copy = new GTMessage{};
    copy->imsg = *imsg;
    if (filtered_address)
        copy->imsg.IAddress = filtered_address; // an arrow can report its scroller instead
    copy->orig = imsg;
    ++gt_live;
    return &copy->imsg;
}

extern "C" struct IntuiMessage *GT_PostFilterIMsg(struct IntuiMessage *imsg)
{
    ++native_calls;
    auto *copy = reinterpret_cast<GTMessage *>(imsg);
    IntuiMessage *orig = copy->orig;
    if (orig->Class == IDCMP_MENUVERIFY)
        orig->Code = copy->imsg.Code;
    delete copy;
    --gt_live;
    return orig;
}

/* EasyRequests, as Intuition gets them from the bridge */
static Window requester;
static MsgPort requester_port;
static struct
{
    unsigned built, handled, freed;
    std::string title, text;
    std::vector<std::string> labels;
    ULONG flags, idcmp;
    Window *window, *freed_window;
    bool fail;
    std::deque<LONG> results;
    ULONG set_flags;
    BOOL wait;
} easy;

/* The bridge gives the text and each label as a %s argument, a native
   pointer each, of "%s" and "%s|...|%s": AROS makes one gadget more than
   the '|' of the template, and the arguments print as they are. */
extern "C" struct Window *BuildEasyRequestArgs(struct Window *window, struct EasyStruct *es,
                                               ULONG idcmp, RAWARG args)
{
    ++native_calls; ++easy.built;
    assert(es->es_StructSize == sizeof(struct EasyStruct));
    easy.window = window;
    easy.title = es->es_Title ? es->es_Title : "(null)";
    assert(std::string(es->es_TextFormat) == "%s");
    const std::string gadgets = es->es_GadgetFormat;
    const size_t count = std::count(gadgets.begin(), gadgets.end(), '|') + 1;
    std::string expected = "%s";
    for (size_t i = 1; i < count; ++i)
        expected += "|%s";
    assert(gadgets == expected);
    auto *argv = static_cast<const IPTR *>(args);
    easy.text = reinterpret_cast<const char *>(argv[0]);
    easy.labels.clear();
    for (size_t i = 0; i < count; ++i)
        easy.labels.push_back(reinterpret_cast<const char *>(argv[1 + i]));
    easy.flags = es->es_Flags;
    easy.idcmp = idcmp;
    if (easy.fail)
        return nullptr;
    requester = Window{};
    requester.Width = 220;
    requester.UserPort = &requester_port;
    requester_port.mp_SigBit = 17;
    return &requester;
}

/* As AROS: it takes the messages of the requester's port, and Intuition
   frees them once replied. */
extern "C" LONG SysReqHandler(struct Window *window, ULONG *idcmp, BOOL wait)
{
    ++native_calls; ++easy.handled;
    if (!window || window == (struct Window *)1)
        return (LONG)(IPTR)window;
    assert(window == &requester);
    while (!queues[&requester_port].empty())
        take_natively(&requester_port);
    LONG result = easy.results.empty() ? -2 : easy.results.front();
    if (!easy.results.empty())
        easy.results.pop_front();
    if (result == -1 && idcmp)
        *idcmp = easy.set_flags;
    easy.wait = wait;
    return result;
}

extern "C" void FreeSysRequest(struct Window *window)
{
    ++native_calls; ++easy.freed;
    easy.freed_window = window;
}

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* ── The guest side ──────────────────────────────────────────────── */

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };

static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }

static const M68KThunkEntry exec_manual[] = { KAFFEINE_EXEC_IDCMP_THUNKS };
static const M68KThunkEntry exec_generated[] = { { 372, poison }, { 378, poison }, { 384, poison } };
static const M68KThunkEntry intuition_manual[] =
    { KAFFEINE_INTUITION_WINDOW_THUNKS, KAFFEINE_INTUITION_REQUEST_THUNKS };
static const M68KThunkEntry intuition_generated[] =
    { { 72, poison }, { 150, poison }, { 204, poison }, { 372, poison }, { 588, poison }, { 594, poison },
      { 600, poison }, { 606, poison } };
static const M68KThunkEntry gadtools_manual[] = { KAFFEINE_GADTOOLS_IDCMP_THUNKS };
static const M68KThunkEntry gadtools_generated[] =
    { { 72, poison }, { 78, poison }, { 102, poison }, { 108, poison } };

/* Guest offsets of the NDK structures */
enum
{
    LN_SUCC = 0, LN_PRED = 4, LN_TYPE = 8, LN_PRI = 9, MN_LENGTH = 18,
    MP_FLAGS = 14, MP_SIGBIT = 15, MP_SIGTASK = 16, MP_MSGLIST = 20,
    IM_CLASS = 20, IM_CODE = 24, IM_QUALIFIER = 26, IM_IADDRESS = 28, IM_MOUSEX = 32,
    IM_MOUSEY = 34, IM_SECONDS = 36, IM_MICROS = 40, IM_IDCMPWINDOW = 44, IM_SPECIALLINK = 48
};

/* a type of shadow that no module uses, to fill the table */
static const UWORD FILLER = 99;

struct Fixture
{
    static constexpr ULONG exec = 0x1000, intuition = 0x2000, gadtools = 0x3000;
    static constexpr ULONG stack = 0x3F00, task = 0x3800, raw = 0x3A00, title = 0x3E00;
    static constexpr ULONG fillers = 0xF000;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x10000> memory;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    Window native_window = {};
    RastPort native_rp = {};
    ULONG window = 0, heap_start = 0, heap_avail = 0;

    Fixture()
    {
        queues.clear();
        replied.clear();
        made.clear();
        closed.clear();
        released_screens.clear();
        releasing_window = nullptr;
        native_calls = gt_live = 0;
        window_gadget_calls.clear();
        window_events.clear();
        prepared_head = sync_native = filter_changes = filtered_address = nullptr;
        filter_saw.clear();
        filtering_calls.clear();
        sync_guest = 0;
        prepare_refuses = false;
        opened_tags.clear();
        open_result = nullptr;
        opened = 0;
        easy.built = easy.handled = easy.freed = 0;
        easy.fail = false;
        easy.results.clear();
        easy.window = easy.freed_window = nullptr;
        fail_modify = false;
        gt_only = 0;
        next_port = 1;
        shadows_at_close = 0xFFFF;
        std::memset(ports, 0, sizeof(ports));
        ports[0].mp_SigBit = 30;
        memory.fill(0xA5);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.heap_start = 0x4000;
        ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        live = &ctx;
        ctx.m68k_sysbase = exec;
        m68k_write32(&ctx, exec + M68K_EXECBASE_THISTASK, task);
        lib(0, exec, "exec.library", exec_manual, std::size(exec_manual),
            exec_generated, std::size(exec_generated));
        lib(1, intuition, "intuition.library", intuition_manual, std::size(intuition_manual),
            intuition_generated, std::size(intuition_generated));
        lib(2, gadtools, "gadtools.library", gadtools_manual, std::size(gadtools_manual),
            gadtools_generated, std::size(gadtools_generated));
        ctx.num_libs = 3;
        m68k_write32(&ctx, stack, 0x76543210);
        std::strcpy(reinterpret_cast<char *>(memory.data() + title), "IDCMP");

        /* a window with a RastPort and an IDCMP port, as OpenWindow() gets it */
        native_window.LeftEdge = 40; native_window.TopEdge = 50;
        native_window.Width = 300; native_window.Height = 200;
        native_window.BorderTop = 21;
        native_window.Title = memory.data() + title;
        native_window.RPort = &native_rp;
        native_window.IDCMPFlags = IDCMP_INTUITICKS;
        native_window.UserPort = &ports[0];
        heap_start = Kaffeine_HeapAvail(&ctx, 0);
        window = Kaffeine_WindowCreate(&ctx, &native_window);
        assert(window);
        heap_avail = Kaffeine_HeapAvail(&ctx, 0);
    }

    ~Fixture()
    {
        for (IntuiMessage *im : made)
            delete im;
        made.clear();
        assert(!gt_live);
        live = nullptr;
    }

    void lib(int i, ULONG base, const char *name, const M68KThunkEntry *manual, ULONG nmanual,
             const M68KThunkEntry *generated, ULONG ngenerated)
    {
        auto &l = ctx.libs[i];
        l.m68k_addr = base;
        l.jt_start = base - 0xC00;
        std::strcpy(l.name, name);
        l.thunks = manual;
        l.num_thunks = nmanual;
        l.gen_thunks = generated;
        l.num_gen_thunks = ngenerated;
    }

    /* A literal LVO through the dispatcher; only D0 may change. */
    ULONG call(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
               std::initializer_list<ULONG> d = {})
    {
        unsigned i;

        for (i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        i = 0;
        for (ULONG v : a) state.a[i++] = v;
        i = 0;
        for (ULONG v : d) state.d[i++] = v;
        state.a[7] = stack;
        const auto before = state;
        ctx.running = TRUE; ctx.exit_code = 0;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);
        return state.d[0];
    }
    /* stopped with rc 20 in D0, where the CPU loop takes the exit code */
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    /* Leave n free entries in the shadow table, and take the fillers away. */
    void leave_shadows(UWORD n)
    {
        static int filler;
        for (ULONG a = fillers; ctx.num_shadows < KAFFEINE_MAX_SHADOWS - n; a += 4)
            assert(Kaffeine_ShadowAdd(&ctx, a, &filler, FILLER));
    }
    void free_shadows()
    {
        for (UWORD i = 0; i < ctx.num_shadows;)
            if (ctx.shadow_map[i].type == FILLER)
                ctx.shadow_map[i] = ctx.shadow_map[--ctx.num_shadows];
            else
                ++i;
    }

    ULONG port() { return lng(window + M68K_WIN_USERPORT); }
    void send(IntuiMessage *im) { queues[im->IDCMPWindow ? im->IDCMPWindow->UserPort : &ports[0]].push_back(im); }
    UWORD word(ULONG addr) { return m68k_read16(&ctx, addr); }
    ULONG lng(ULONG addr) { return m68k_read32(&ctx, addr); }
    ULONG getmsg(ULONG p) { return call(exec, 372, { p }); }
    ULONG waitport(ULONG p) { return call(exec, 384, { p }); }
    void reply(ULONG msg) { call(exec, 378, { 0, msg }); }
    ULONG gt_getimsg(ULONG p) { return call(gadtools, 72, { p }); }
    void gt_replyimsg(ULONG msg) { call(gadtools, 78, { 0, msg }); }
    void closewindow(ULONG w) { call(intuition, 72, { w }); }
    bool is(ULONG guest, UWORD type, const void *native)
    {
        return Kaffeine_ShadowNative(&ctx, guest, type) == native;
    }
};

static void test_registration()
{
    assert(std::size(exec_manual) == 3 && exec_manual[0].lvo == 372 && exec_manual[0].thunk == Kaffeine_GetMsg &&
           exec_manual[1].lvo == 378 && exec_manual[1].thunk == Kaffeine_ReplyMsg &&
           exec_manual[2].lvo == 384 && exec_manual[2].thunk == Kaffeine_WaitPort);
    assert(std::size(intuition_manual) == 8 &&
           intuition_manual[0].lvo == 72 && intuition_manual[0].thunk == Kaffeine_CloseWindow &&
           intuition_manual[1].lvo == 150 && intuition_manual[1].thunk == Kaffeine_ModifyIDCMP &&
           intuition_manual[2].lvo == 204 && intuition_manual[2].thunk == Kaffeine_OpenWindow &&
           intuition_manual[3].lvo == 606 && intuition_manual[3].thunk == Kaffeine_OpenWindowTagList &&
           intuition_manual[4].lvo == 372 && intuition_manual[4].thunk == Kaffeine_FreeSysRequest &&
           intuition_manual[5].lvo == 588 && intuition_manual[5].thunk == Kaffeine_EasyRequestArgs &&
           intuition_manual[6].lvo == 594 && intuition_manual[6].thunk == Kaffeine_BuildEasyRequestArgs &&
           intuition_manual[7].lvo == 600 && intuition_manual[7].thunk == Kaffeine_SysReqHandler);
    assert(std::size(gadtools_manual) == 4);
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } gt[] =
        { { 72, Kaffeine_GT_GetIMsg }, { 78, Kaffeine_GT_ReplyIMsg },
          { 102, Kaffeine_GT_FilterIMsg }, { 108, Kaffeine_GT_PostFilterIMsg } };
    for (unsigned i = 0; i < 4; ++i)
        assert(gadtools_manual[i].lvo == gt[i].lvo && gadtools_manual[i].thunk == gt[i].thunk);
}

static void test_window_create()
{
    Fixture f;
    const ULONG w = f.window;

    /* the fields, a 68k RastPort, the IDCMP port and a title in guest memory */
    assert(f.word(w + M68K_WIN_LEFTEDGE) == 40 && f.word(w + M68K_WIN_WIDTH) == 300 &&
           f.memory[w + M68K_WIN_BORDERTOP] == 21 && f.lng(w + M68K_WIN_IDCMPFLAGS) == IDCMP_INTUITICKS);
    assert(f.is(w, KAFFEINE_SHADOW_WINDOW, &f.native_window) &&
           f.is(f.lng(w + M68K_WIN_RPORT), KAFFEINE_SHADOW_RASTPORT, &f.native_rp) &&
           f.is(f.port(), KAFFEINE_SHADOW_USERPORT, &ports[0]));
    assert(f.lng(w + M68K_WIN_TITLE) == Fixture::title && f.lng(w + M68K_WIN_WSCREEN) == 0);
    assert(f.ctx.num_shadows == 3);

    /* a native title, and a screen that the program opened */
    static UBYTE native_title[] = "native";
    static int screen;
    const ULONG mscreen = 0x3D00;
    RastPort other_rp = {};
    Window other = {};
    other.Width = 100;
    other.Title = native_title;
    other.WScreen = reinterpret_cast<Screen *>(&screen);
    other.RPort = &other_rp;
    other.UserPort = &ports[3];
    assert(Kaffeine_ShadowAdd(&f.ctx, mscreen, other.WScreen, KAFFEINE_SHADOW_SCREEN));
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    ULONG o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o && f.word(o + M68K_WIN_WIDTH) == 100 && f.lng(o + M68K_WIN_TITLE) == 0 &&
           f.lng(o + M68K_WIN_WSCREEN) == mscreen);
    Kaffeine_WindowDelete(&f.ctx, o);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == 4);

    /* No room for the Window, its RastPort or its port: nothing is left. */
    for (UWORD left = 0; left < 3; ++left)
    {
        f.leave_shadows(left);
        const UWORD shadows = f.ctx.num_shadows;
        assert(Kaffeine_WindowCreate(&f.ctx, &other) == 0);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == shadows);
        assert(!Kaffeine_ShadowGuest(&f.ctx, &other, KAFFEINE_SHADOW_WINDOW) &&
               !Kaffeine_ShadowGuest(&f.ctx, &other_rp, KAFFEINE_SHADOW_RASTPORT));
        f.free_shadows();
    }
    f.leave_shadows(3);
    o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o);
    Kaffeine_WindowDelete(&f.ctx, o);
    f.free_shadows();
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == 4);

    /* WScreen of a screen that the program locked, as the public Screen
       of LockPubScreen(); one that the program opened comes first, and a
       screen that the program has no 68k Screen of gives 0 */
    static int pubscreen, unknown;
    const ULONG locked = 0x3D80, opened = 0x3DC0;
    other.WScreen = reinterpret_cast<Screen *>(&pubscreen);
    assert(Kaffeine_ShadowAdd(&f.ctx, locked, other.WScreen, KAFFEINE_SHADOW_PUBSCREEN));
    o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o && f.lng(o + M68K_WIN_WSCREEN) == locked);
    Kaffeine_WindowDelete(&f.ctx, o);
    assert(Kaffeine_ShadowAdd(&f.ctx, opened, other.WScreen, KAFFEINE_SHADOW_SCREEN));
    o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o && f.lng(o + M68K_WIN_WSCREEN) == opened);
    Kaffeine_WindowDelete(&f.ctx, o);
    other.WScreen = reinterpret_cast<Screen *>(&unknown);
    o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o && f.lng(o + M68K_WIN_WSCREEN) == 0);
    Kaffeine_WindowDelete(&f.ctx, o);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == 6);

    /* the native screen of a 68k one, and the 68k screen of a native one */
    assert(Kaffeine_ScreenNative(&f.ctx, locked) == reinterpret_cast<Screen *>(&pubscreen) &&
           Kaffeine_ScreenNative(&f.ctx, opened) == reinterpret_cast<Screen *>(&pubscreen) &&
           Kaffeine_ScreenNative(&f.ctx, mscreen) == reinterpret_cast<Screen *>(&screen));
    assert(!Kaffeine_ScreenNative(&f.ctx, 0) && !Kaffeine_ScreenNative(&f.ctx, w) &&
           !Kaffeine_ScreenNative(&f.ctx, Fixture::raw));
    assert(Kaffeine_ScreenGuest(&f.ctx, reinterpret_cast<Screen *>(&pubscreen)) == opened &&
           Kaffeine_ScreenGuest(&f.ctx, reinterpret_cast<Screen *>(&screen)) == mscreen &&
           Kaffeine_ScreenGuest(&f.ctx, reinterpret_cast<Screen *>(&unknown)) == 0 &&
           Kaffeine_ScreenGuest(&f.ctx, nullptr) == 0);
}

/* The public RastPort.BitMap at classic offset 4 names the same typed
   bitmap as its live owned screen. Neither an opaque shadow alone nor
   a mismatched native bitmap is authority to publish that handle. */
static void test_window_bitmap_handle()
{
    for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
        Fixture f;
        Screen screen{};
        BitMap backing{}, different{};
        RastPort rp{};
        Window window{};
        backing.BytesPerRow = 40; backing.Rows = 200; backing.Depth = 3;
        screen.RastPort.BitMap = &backing;
        rp.BitMap = scenario == 1 ? &different : &backing;
        window.WScreen = &screen;
        window.RPort = &rp;
        const ULONG gs = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
        assert(gs);
        const UWORD type = scenario == 2 || scenario == 4
                         ? KAFFEINE_SHADOW_PUBSCREEN : KAFFEINE_SHADOW_SCREEN;
        assert(Kaffeine_ShadowAdd(&f.ctx, gs, &screen, type));
        // The real screen bridge supplies these readable scalar fields.
        m68k_write16(&f.ctx, gs + 184, 40);
        m68k_write16(&f.ctx, gs + 186, 200);
        f.memory[gs + 189] = 3;
        if (scenario != 3)
        {
            f.ctx.pubscreens[0].native = scenario == 5
                ? reinterpret_cast<Screen *>(static_cast<std::uintptr_t>(0xDEAD0000)) : &screen;
            f.ctx.pubscreens[0].guest = gs;
            f.ctx.pubscreens[0].owned = scenario != 2;
        }
        const ULONG guest = Kaffeine_WindowCreate(&f.ctx, &window);
        assert(guest && f.lng(guest + M68K_WIN_WSCREEN) == gs);
        const ULONG port = f.lng(guest + M68K_WIN_RPORT);
        const ULONG bitmap = scenario == 0 ? gs + 184 : 0;
        assert(port && f.lng(port + 4) == bitmap);
        if (bitmap)
            assert(f.memory[bitmap + 5] == 3 && f.word(bitmap) == 40);
        // Drawing's refresh preserves the initial handle and guest edits.
        rp.FgPen = 3;
        Kaffeine_RastPortOut(&f.ctx, port, &rp);
        assert(f.lng(port + 4) == bitmap);
        m68k_write32(&f.ctx, port + 4, Fixture::raw);
        Kaffeine_RastPortOut(&f.ctx, port, &rp);
        assert(f.lng(port + 4) == Fixture::raw);
        Kaffeine_WindowDelete(&f.ctx, guest);
        assert(f.is(gs, type, &screen) && f.memory[gs + 189] == 3);
        f.ctx.pubscreens[0] = {};
        Kaffeine_ShadowForget(&f.ctx, gs, type);
        Kaffeine_MirrorRelease(&f.ctx, gs);
        assert(f.ctx.num_shadows == 3 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_screen_release_order()
{
    for (bool hook : { false, true })
        for (bool through_close : { false, true })
        {
            Fixture f;
            static int native_screen;
            f.native_window.WScreen = reinterpret_cast<Screen *>(&native_screen);
            releasing_window = &f.native_window;
            f.ctx.screen_release = hook ? record_screen_release : nullptr;
            window_events.clear();
            if (through_close)
                f.closewindow(f.window); // literal Intuition -72
            else
                Kaffeine_WindowDelete(&f.ctx, f.window);
            assert(released_screens.size() == (hook ? 1u : 0u));
            if (hook)
                assert(released_screens[0] == f.native_window.WScreen);
            assert(!Kaffeine_ShadowGuest(&f.ctx, &f.native_window, KAFFEINE_SHADOW_WINDOW));
            assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_start);
            assert(closed.size() == (through_close ? 1u : 0u));
            if (through_close && hook)
            {
                auto release = std::find(window_events.begin(), window_events.end(), "screen_release");
                auto close = std::find(window_events.begin(), window_events.end(), "CloseWindow");
                assert(release < close && close != window_events.end());
            }
            Kaffeine_WindowDelete(&f.ctx, f.window);
            f.closewindow(f.window);
            assert(released_screens.size() == (hook ? 1u : 0u));
        }
}

static void test_user_port()
{
    Fixture f;
    const ULONG port = f.port(), list = port + MP_MSGLIST;

    /* a signalling 68k port with the native signal bit and the guest task */
    assert(port && f.is(port, KAFFEINE_SHADOW_USERPORT, &ports[0]));
    assert(f.memory[port + LN_TYPE] == NT_MSGPORT && f.memory[port + MP_FLAGS] == PA_SIGNAL &&
           f.memory[port + MP_SIGBIT] == 30 && f.lng(port + MP_SIGTASK) == Fixture::task);
    assert(f.lng(list) == list + 4 && f.lng(list + 4) == 0 && f.lng(list + 8) == list);
    Kaffeine_UserPortDelete(&f.ctx, port);
    assert(!Kaffeine_ShadowNative(&f.ctx, port, KAFFEINE_SHADOW_USERPORT));
    Kaffeine_UserPortDelete(&f.ctx, Fixture::raw);
}

static void test_getmsg_fields()
{
    Fixture f;
    IntuiMessage *im = message(&f.native_window, IDCMP_NEWSIZE);

    /* The window has grown when the message comes. */
    f.native_window.Width = 340;
    f.native_window.GZZHeight = 150;
    f.send(im);
    const ULONG msg = f.getmsg(f.port());
    assert(msg >= f.ctx.heap_start && msg < f.ctx.heap_end);
    assert(f.is(msg, KAFFEINE_SHADOW_INTUIMESSAGE, im));
    assert(f.lng(msg + IM_CLASS) == IDCMP_NEWSIZE && f.word(msg + IM_CODE) == 0x1234 &&
           f.word(msg + IM_QUALIFIER) == 0x8001 && f.word(msg + IM_MOUSEX) == 0xFFFB &&
           f.word(msg + IM_MOUSEY) == 300 && f.lng(msg + IM_SECONDS) == 1000 &&
           f.lng(msg + IM_MICROS) == 999999 && f.lng(msg + IM_IDCMPWINDOW) == f.window);
    assert(f.memory[msg + LN_TYPE] == NT_MESSAGE && f.memory[msg + LN_PRI] == 0xFD &&
           f.word(msg + MN_LENGTH) == M68K_SIZEOF_INTUIMESSAGE);
    assert(f.lng(msg + LN_SUCC) == 0 && f.lng(msg + LN_PRED) == 0 &&
           f.lng(msg + IM_IADDRESS) == 0 && f.lng(msg + IM_SPECIALLINK) == 0);
    assert(f.word(f.window + M68K_WIN_WIDTH) == 340 && f.word(f.window + M68K_WIN_GZZHEIGHT) == 150);

    /* ReplyMsg() returns it to Intuition and frees it. */
    f.reply(msg);
    assert(replied.size() == 1 && replied[0] == im);
    assert(!Kaffeine_ShadowNative(&f.ctx, msg, KAFFEINE_SHADOW_INTUIMESSAGE));
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* IAddress is the 68k window when it names the window, else 0. */
    im = message(&f.native_window, IDCMP_ACTIVEWINDOW);
    im->IAddress = &f.native_window;
    f.send(im);
    ULONG m = f.getmsg(f.port());
    assert(f.lng(m + IM_IADDRESS) == f.window);
    f.reply(m);
    im = message(&f.native_window, IDCMP_ACTIVEWINDOW);
    im->IAddress = &ports[3];
    f.send(im);
    m = f.getmsg(f.port());
    assert(f.lng(m + IM_IADDRESS) == 0);
    f.reply(m);

    /* An empty port gives 0. */
    assert(f.getmsg(f.port()) == 0 && !f.stopped());
}

static void test_waitport_and_many()
{
    Fixture f;

    /* WaitPort() shows the first message, which stays in the port; GetMsg()
       gives the same 68k message, which is the program's then. */
    IntuiMessage *im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(im);
    const ULONG waited = f.waitport(f.port());
    assert(waited && queues[&ports[0]].size() == 1 && f.is(waited, KAFFEINE_SHADOW_WAITMESSAGE, im));
    assert(f.waitport(f.port()) == waited && f.ctx.num_shadows == 4);
    unsigned calls = native_calls;
    f.reply(waited);                    /* not taken yet: nothing happens */
    assert(native_calls == calls && queues[&ports[0]].size() == 1);
    assert(f.getmsg(f.port()) == waited && f.is(waited, KAFFEINE_SHADOW_INTUIMESSAGE, im) &&
           !Kaffeine_ShadowNative(&f.ctx, waited, KAFFEINE_SHADOW_WAITMESSAGE));
    f.reply(waited);
    assert(replied.size() == 1 && replied[0] == im && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* What WaitPort() showed of a message that something native took since
       goes at the next WaitPort() or GetMsg(), and that message is not read
       again: ASan sees it freed. */
    IntuiMessage *gone = message(&f.native_window, IDCMP_INTUITICKS);
    im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(gone);
    f.send(im);
    ULONG shown = f.waitport(f.port());
    assert(f.is(shown, KAFFEINE_SHADOW_WAITMESSAGE, gone));
    take_natively(&ports[0]);
    shown = f.waitport(f.port());
    assert(f.is(shown, KAFFEINE_SHADOW_WAITMESSAGE, im) && f.ctx.num_shadows == 4);
    assert(f.getmsg(f.port()) == shown);
    f.reply(shown);
    gone = message(&f.native_window, IDCMP_INTUITICKS);
    im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(gone);
    f.send(im);
    assert(f.is(f.waitport(f.port()), KAFFEINE_SHADOW_WAITMESSAGE, gone));
    take_natively(&ports[0]);
    const ULONG taken = f.getmsg(f.port());
    assert(f.is(taken, KAFFEINE_SHADOW_INTUIMESSAGE, im) && f.ctx.num_shadows == 4);
    f.reply(taken);
    assert(replied.size() == 3 && replied[2] == im);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && f.ctx.num_shadows == 3);

    /* Far more messages than shadows: each reply gives everything back. */
    for (unsigned i = 0; i < 600; ++i)
    {
        f.send(message(&f.native_window, IDCMP_INTUITICKS));
        if (i % 2)
            assert(f.waitport(f.port()));
        const ULONG m = f.getmsg(f.port());
        assert(m && f.lng(m + IM_IDCMPWINDOW) == f.window);
        f.reply(m);
    }
    assert(replied.size() == 603 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    assert(f.ctx.num_shadows == 3);

    /* The Code of IDCMP_MENUVERIFY goes back with the reply. */
    im = message(&f.native_window, IDCMP_MENUVERIFY);
    f.send(im);
    const ULONG m = f.getmsg(f.port());
    m68k_write16(&f.ctx, m + IM_CODE, 0x77);
    f.reply(m);
    assert(im->Code == 0x77);

    /* Malformed ordinary guest messages stop before reaching native Exec. */
    calls = native_calls;
    f.reply(Fixture::raw);
    assert(native_calls == calls && f.stopped());
    f.reply(0);
    assert(native_calls == calls && f.stopped());
}

static void test_guest_ports()
{
    Fixture f;
    const ULONG port = 0x3B00, list = port + MP_MSGLIST, node = 0x3C00;

    /* A complete manual PA_IGNORE port; an empty WaitPort still refuses. */
    std::memset(f.memory.data() + port, 0, 34);
    f.memory[port + 8] = 4;
    f.memory[port + 14] = 2;
    m68k_write32(&f.ctx, list, list + 4);
    m68k_write32(&f.ctx, list + 4, 0);
    m68k_write32(&f.ctx, list + 8, list);
    assert(f.getmsg(port) == 0 && !f.stopped());
    f.waitport(port);
    assert(f.stopped());

    /* A complete message: WaitPort() returns it, GetMsg() takes it. */
    std::memset(f.memory.data() + node, 0, 20);
    f.memory[node + 8] = 5;
    m68k_write16(&f.ctx, node + 18, 20);
    m68k_write32(&f.ctx, list, node);
    m68k_write32(&f.ctx, node + LN_SUCC, list + 4);
    m68k_write32(&f.ctx, node + LN_PRED, list);
    m68k_write32(&f.ctx, list + 8, node);
    assert(f.waitport(port) == node && !f.stopped());
    assert(f.getmsg(port) == node);
    assert(f.lng(list) == list + 4 && f.lng(list + 4 + LN_PRED) == list);
    assert(f.getmsg(port) == 0);

    /* nonsense addresses */
    assert(f.getmsg(0) == 20 && f.stopped());
    assert(f.getmsg(0xFFFFFFF0) == 20 && f.stopped());
    assert(!native_calls);
}

static void test_modifyidcmp()
{
    Fixture f;
    Window plain = {};
    const ULONG win = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_WINDOW, MEMF_CLEAR);

    assert(Kaffeine_ShadowAdd(&f.ctx, win, &plain, KAFFEINE_SHADOW_WINDOW));
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    /* flags bring a 68k IDCMP port */
    assert(f.call(Fixture::intuition, 150, { win }, { IDCMP_INTUITICKS }) == TRUE);
    const ULONG port = f.lng(win + M68K_WIN_USERPORT);
    assert(port && plain.UserPort && f.is(port, KAFFEINE_SHADOW_USERPORT, plain.UserPort));
    assert(f.memory[port + MP_SIGBIT] == plain.UserPort->mp_SigBit &&
           f.lng(win + M68K_WIN_IDCMPFLAGS) == IDCMP_INTUITICKS);
    /* other flags keep it */
    assert(f.call(Fixture::intuition, 150, { win }, { IDCMP_NEWSIZE }) == TRUE &&
           f.lng(win + M68K_WIN_USERPORT) == port && f.lng(win + M68K_WIN_IDCMPFLAGS) == IDCMP_NEWSIZE);
    /* no flags take it away, with what WaitPort() showed of the messages
       that Intuition leaves in it */
    f.send(message(&plain, IDCMP_NEWSIZE));
    assert(f.waitport(port));
    assert(f.call(Fixture::intuition, 150, { win }, { 0 }) == TRUE &&
           f.lng(win + M68K_WIN_USERPORT) == 0 && f.lng(win + M68K_WIN_IDCMPFLAGS) == 0);
    assert(!Kaffeine_ShadowNative(&f.ctx, port, KAFFEINE_SHADOW_USERPORT));
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && replied.empty());
    /* a native failure is FALSE */
    fail_modify = true;
    assert(f.call(Fixture::intuition, 150, { win }, { IDCMP_NEWSIZE }) == FALSE);
    fail_modify = false;
    /* No room for the 68k port: Intuition's change is undone. */
    f.leave_shadows(0);
    unsigned calls = native_calls;
    assert(f.call(Fixture::intuition, 150, { win }, { IDCMP_INTUITICKS }) == FALSE);
    assert(native_calls == calls + 2 && !plain.UserPort && plain.IDCMPFlags == 0);
    assert(f.lng(win + M68K_WIN_USERPORT) == 0 && f.lng(win + M68K_WIN_IDCMPFLAGS) == 0);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && !f.stopped());
    f.free_shadows();

    /* a port that the program set itself, or anything but a window */
    calls = native_calls;
    m68k_write32(&f.ctx, win + M68K_WIN_USERPORT, f.port());
    f.call(Fixture::intuition, 150, { win }, { IDCMP_NEWSIZE });
    assert(f.stopped());
    f.call(Fixture::intuition, 150, { Fixture::raw }, { IDCMP_NEWSIZE });
    assert(f.stopped() && native_calls == calls);
}

static void test_gadtools()
{
    Fixture f;

    /* GT_GetIMsg() gives the 68k message of the GadTools copy;
       GT_ReplyIMsg() replies the original. ReplyMsg() does not take a
       copy, nor GT_ReplyIMsg() a message from GetMsg(). */
    IntuiMessage *im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(im);
    const ULONG copy = f.gt_getimsg(f.port());
    assert(copy && gt_live == 1 && f.lng(copy + IM_IDCMPWINDOW) == f.window &&
           f.lng(copy + IM_CLASS) == IDCMP_INTUITICKS &&
           Kaffeine_ShadowNative(&f.ctx, copy, KAFFEINE_SHADOW_GTMESSAGE));
    unsigned calls = native_calls;
    f.reply(copy);
    assert(native_calls == calls && gt_live == 1);
    f.gt_replyimsg(copy);
    assert(gt_live == 0 && replied.size() == 1 && replied[0] == im);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* After WaitPort(), the 68k message that it showed goes with the
       original. */
    im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(im);
    const ULONG waited = f.waitport(f.port());
    const ULONG copy2 = f.gt_getimsg(f.port());
    assert(copy2 && copy2 != waited && f.is(waited, KAFFEINE_SHADOW_INTUIMESSAGE, im));
    f.gt_replyimsg(copy2);
    assert(replied.size() == 2 && replied[1] == im && gt_live == 0);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && f.ctx.num_shadows == 3);

    /* A message of GadTools only goes back at once, also when WaitPort()
       showed it. */
    gt_only = IDCMP_MOUSEMOVE;
    IntuiMessage *own = message(&f.native_window, IDCMP_MOUSEMOVE);
    im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(own);
    f.send(im);
    assert(f.is(f.waitport(f.port()), KAFFEINE_SHADOW_WAITMESSAGE, own));
    const ULONG copy3 = f.gt_getimsg(f.port());
    assert(replied.size() == 3 && replied[2] == own && gt_live == 1 &&
           f.lng(copy3 + IM_CLASS) == IDCMP_INTUITICKS && f.ctx.num_shadows == 4);
    f.gt_replyimsg(copy3);
    assert(replied.size() == 4 && replied[3] == im);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && f.ctx.num_shadows == 3);
    gt_only = 0;

    /* No room for the 68k copy: Intuition gets the message back. */
    f.leave_shadows(0);
    im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(im);
    assert(f.gt_getimsg(f.port()) == 0);
    assert(replied.size() == 5 && replied[4] == im && gt_live == 0);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && !f.stopped());
    f.free_shadows();

    /* GetMsg(), GT_FilterIMsg(), GT_PostFilterIMsg() and ReplyMsg() */
    im = message(&f.native_window, IDCMP_MENUVERIFY);
    f.send(im);
    const ULONG orig = f.getmsg(f.port());
    calls = native_calls;
    f.gt_replyimsg(orig);
    assert(native_calls == calls);
    const ULONG filtered = f.call(Fixture::gadtools, 102, { 0, orig });
    assert(filtered && filtered != orig && gt_live == 1);
    m68k_write16(&f.ctx, filtered + IM_CODE, 0x55);
    assert(f.call(Fixture::gadtools, 108, { 0, filtered }) == orig && gt_live == 0);
    assert(!Kaffeine_ShadowNative(&f.ctx, filtered, KAFFEINE_SHADOW_GTMESSAGE));
    f.reply(orig);
    assert(replied.size() == 6 && replied[5] == im && im->Code == 0x55);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* empty port, NULL and foreign messages */
    assert(f.gt_getimsg(f.port()) == 0);
    assert(f.gt_getimsg(Fixture::raw) == 0);
    f.gt_replyimsg(0);
    f.gt_replyimsg(Fixture::raw);
    assert(f.call(Fixture::gadtools, 108, { 0, Fixture::raw }) == 0 && !f.stopped());
    f.call(Fixture::gadtools, 102, { 0, Fixture::raw });
    assert(f.stopped() && replied.size() == 6);
}

static void test_close_window()
{
    {
        Fixture f;
        IntuiMessage *held = message(&f.native_window, IDCMP_MENUVERIFY);
        IntuiMessage *gt = message(&f.native_window, IDCMP_INTUITICKS);
        IntuiMessage *sent = message(&f.native_window, IDCMP_INTUITICKS);
        IntuiMessage *shown = message(&f.native_window, IDCMP_INTUITICKS);
        IntuiMessage *queued = message(&f.native_window, IDCMP_INTUITICKS);

        /* one message that the program took, one that GadTools has, one
           that the program took and sent on with PutMsg(), which leaves it
           to the port as WaitPort() shows one (test_exec_public checks
           that side), one that WaitPort() showed and one that nothing
           showed */
        for (IntuiMessage *im : { held, gt, sent, shown, queued })
            f.send(im);
        const ULONG a = f.getmsg(f.port());
        m68k_write16(&f.ctx, a + IM_CODE, 0x77);
        const ULONG c = f.gt_getimsg(f.port());
        const ULONG s = f.getmsg(f.port());
        const ULONG w = f.waitport(f.port());
        for (UWORD i = 0; i < f.ctx.num_shadows; ++i)
            if (f.ctx.shadow_map[i].m68k_addr == s)
                f.ctx.shadow_map[i].type = KAFFEINE_SHADOW_WAITMESSAGE;
        queues[&ports[0]].push_back(sent);
        assert(a && c && s && w && gt_live == 1 && f.ctx.num_shadows == 7);

        /* CloseWindow() gives back what the program has, and frees every
           68k part, before the native window goes; Intuition replies what
           is still in the port. Each goes back once. */
        f.closewindow(f.window);
        assert(closed.size() == 1 && closed[0] == &f.native_window && shadows_at_close == 0);
        assert((replied == std::vector<IntuiMessage *>{ gt, held, shown, queued, sent }));
        assert(held->Code == 0x77 && gt_live == 0 && !shown->IDCMPWindow && !queued->IDCMPWindow &&
               !sent->IDCMPWindow);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_start && f.ctx.num_shadows == 0);

        /* Nothing is left to reply or to close again. */
        const unsigned calls = native_calls;
        f.reply(a);
        f.gt_replyimsg(c);
        f.closewindow(f.window);
        assert(native_calls == calls && closed.size() == 1 && !f.stopped());
    }
    {
        /* What WaitPort() showed of a message that something native took
           goes without that message being read. */
        Fixture f;
        f.send(message(&f.native_window, IDCMP_INTUITICKS));
        assert(f.waitport(f.port()));
        take_natively(&ports[0]);
        f.closewindow(f.window);
        assert(replied.empty() && closed.size() == 1 && f.ctx.num_shadows == 0);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_start);
    }
}

/* A 68k EasyStruct with a title, a text and two gadgets, and its 68k
   arguments: a string, a LONG, and the label of the first gadget. */
static void easy_struct(Fixture &f, ULONG es, ULONG args)
{
    auto put = [&](ULONG addr, const char *s) {
        std::memcpy(f.memory.data() + addr, s, std::strlen(s) + 1);
    };
    put(0x3D00, "Title %s");
    put(0x3D20, "Hello %s, %ld%% done");
    put(0x3D40, "%s|Cancel");
    put(0x3D60, "world");
    put(0x3D70, "OK");
    m68k_write32(&f.ctx, es + 0, 20);
    m68k_write32(&f.ctx, es + 4, 0x1234);
    m68k_write32(&f.ctx, es + 8, 0x3D00);
    m68k_write32(&f.ctx, es + 12, 0x3D20);
    m68k_write32(&f.ctx, es + 16, 0x3D40);
    m68k_write32(&f.ctx, args, 0x3D60);
    m68k_write32(&f.ctx, args + 4, 42);
    m68k_write32(&f.ctx, args + 8, 0x3D70);
}

static void test_build_easy_request()
{
    Fixture f;
    const ULONG es = 0x3C80, args = 0x3CC0, flags = 0x3CF0;

    easy_struct(f, es, args);
    /* the bridge formats, Intuition formats again with every % doubled */
    const ULONG req = f.call(Fixture::intuition, 594, { f.window, es, 0, args }, { IDCMP_INTUITICKS });
    assert(easy.built == 1 && easy.window == &f.native_window);
    assert(easy.title == "Title %s" && easy.text == "Hello world, 42% done" &&
           easy.labels == std::vector<std::string>({ "OK", "Cancel" }) &&
           easy.flags == 0x1234 && easy.idcmp == IDCMP_INTUITICKS);
    /* the requester is a 68k Window */
    assert(req && f.is(req, KAFFEINE_SHADOW_WINDOW, &requester));
    assert(f.word(req + M68K_WIN_WIDTH) == 220 && f.lng(req + M68K_WIN_USERPORT));

    /* SysReqHandler() with the 68k IDCMP flags, both ways */
    m68k_write32(&f.ctx, flags, IDCMP_INTUITICKS);
    assert(f.call(Fixture::intuition, 600, { req, flags }, { 0 }) == 0xFFFFFFFEU);
    easy.results = { -1 };
    easy.set_flags = IDCMP_INTUITICKS;
    m68k_write32(&f.ctx, flags, 0x5555);
    assert(f.call(Fixture::intuition, 600, { req, flags }, { 1 }) == 0xFFFFFFFFU &&
           f.lng(flags) == IDCMP_INTUITICKS);
    easy.results = { 1 };
    assert(f.call(Fixture::intuition, 600, { req, 0 }, { 1 }) == 1 && easy.wait);
    /* waitInput is a WORD: the high word of D0 does not make it TRUE */
    assert(f.call(Fixture::intuition, 600, { req, 0 }, { 0x12340000 }) == 0xFFFFFFFEU && !easy.wait);

    /* WaitPort() on the requester's port, then SysReqHandler(), which takes
       that message and frees it: what WaitPort() showed of it goes without
       the message being read again, at the next WaitPort() and at
       FreeSysRequest(). ASan sees the messages freed. */
    const ULONG rport = f.lng(req + M68K_WIN_USERPORT);
    for (int i = 0; i < 3; ++i)
    {
        IntuiMessage *im = message(&requester, IDCMP_INTUITICKS);
        f.send(im);
        assert(f.is(f.waitport(rport), KAFFEINE_SHADOW_WAITMESSAGE, im));
        assert(f.call(Fixture::intuition, 600, { req, 0 }, { 0 }) == 0xFFFFFFFEU);
    }
    assert(replied.empty() && f.ctx.num_shadows == 7);

    /* FreeSysRequest() takes a menu strip off the window and frees the 68k
       Window first */
    static Window *menu_closed;
    menu_closed = nullptr;
    f.ctx.menu_closing = [](KaffeineContext *, Window *w) { menu_closed = w; };
    f.call(Fixture::intuition, 372, { req });
    assert(menu_closed == &requester);
    assert(easy.freed == 1 && easy.freed_window == &requester);
    assert(!Kaffeine_ShadowNative(&f.ctx, req, KAFFEINE_SHADOW_WINDOW));
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail && f.ctx.num_shadows == 3 && !f.stopped());

    /* NULL and 1 pass as Intuition takes them */
    easy.fail = true;
    assert(f.call(Fixture::intuition, 594, { 0, es, 0, args }, { 0 }) == 0 && !easy.window);
    assert(f.call(Fixture::intuition, 600, { 0, 0 }, { 0 }) == 0);
    assert(f.call(Fixture::intuition, 600, { 1, 0 }, { 0 }) == 1);
    menu_closed = nullptr;
    f.call(Fixture::intuition, 372, { 1 });
    assert(easy.freed_window == (Window *)1 && !f.stopped() && !menu_closed);
}

static void test_requester_window_type()
{
    Fixture f;
    auto before = f.ctx.num_shadows;
    auto heap = Kaffeine_HeapAvail(&f.ctx, 0);
    auto calls = native_calls;
    f.call(Fixture::intuition, 372, { f.window });
    assert(native_calls == calls && f.ctx.num_shadows == before &&
           Kaffeine_HeapAvail(&f.ctx, 0) == heap && !easy.freed);
    assert(f.call(Fixture::intuition, 600, { f.window, 0 }, { 0 }) == 20 && f.stopped());
    assert(!easy.handled && f.is(f.window, KAFFEINE_SHADOW_WINDOW, &f.native_window));
}

static void test_owned_window_cleanup()
{
    Fixture f;
    const ULONG es = 0x3C80, args = 0x3CC0;
    easy_struct(f, es, args);
    ULONG req = f.call(Fixture::intuition, 594, { f.window, es, 0, args }, { 0 });
    assert(req && f.is(req, KAFFEINE_SHADOW_REQUESTER_WINDOW, &requester));
    IntuiMessage *im = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(im);
    assert(f.getmsg(f.port()));
    f.ctx.window_gadgets = &recording_gadgets;
    Kaffeine_WindowsCleanup(&f.ctx);
    assert(closed == std::vector<Window *>{ &f.native_window });
    assert(easy.freed == 1 && easy.freed_window == &requester);
    assert(replied == std::vector<IntuiMessage *>{ im });
    assert(!f.ctx.num_shadows && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_start);
    assert(!window_gadget_calls.empty());
    auto calls = native_calls;
    Kaffeine_WindowsCleanup(&f.ctx);
    assert(native_calls == calls);
}

static void test_easy_request_args()
{
    Fixture f;
    const ULONG es = 0x3C80, args = 0x3CC0, flags = 0x3CF0;

    easy_struct(f, es, args);
    /* the requester, handled until it gives a result, then freed: here an
       IDCMP flag of the program closes it */
    easy.results = { -2, -2, -1 };
    easy.set_flags = IDCMP_INTUITICKS;
    m68k_write32(&f.ctx, flags, IDCMP_INTUITICKS);
    assert(f.call(Fixture::intuition, 588, { 0, es, flags, args }) == 0xFFFFFFFFU);
    assert(easy.built == 1 && easy.handled == 3 && easy.freed == 1 &&
           easy.freed_window == &requester && easy.idcmp == IDCMP_INTUITICKS);
    assert(f.lng(flags) == IDCMP_INTUITICKS && easy.text == "Hello world, 42% done");
    /* the requester window never reaches the program */
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    easy.results = { 2 };
    assert(f.call(Fixture::intuition, 588, { f.window, es, 0, args }) == 2);

    /* 68k input the bridge cannot take stops the program before Intuition */
    const unsigned built = easy.built;
    f.call(Fixture::intuition, 588, { Fixture::raw, es, 0, args });
    assert(f.stopped());
    f.call(Fixture::intuition, 588, { 0, 0xFFF0, 0, args });
    assert(f.stopped());
    m68k_write32(&f.ctx, args, 0xFFFFFF00);      /* %s outside guest memory */
    f.call(Fixture::intuition, 588, { 0, es, 0, args });
    assert(f.stopped());
    f.call(Fixture::intuition, 594, { 0, es, 0, args }, { 0 });
    assert(f.stopped());
    m68k_write32(&f.ctx, args, 0x3D60);
    f.call(Fixture::intuition, 588, { 0, es, 0xFFFFFFF0, args });
    assert(f.stopped());
    f.call(Fixture::intuition, 600, { Fixture::raw, 0 }, { 0 });
    assert(f.stopped());
    assert(easy.built == built);
    f.call(Fixture::intuition, 372, { Fixture::raw });
    assert(!f.stopped() && easy.freed == 2);
}

/* The gadgets of a template, cut as AROS cuts them */
static void test_easy_request_labels()
{
    Fixture f;
    const ULONG es = 0x3C80, args = 0x3CC0, text = 0x3D00, labels = 0x3D20;
    struct Case { const char *template_; std::vector<ULONG> words; std::vector<std::string> expect; };
    auto put = [&](ULONG addr, const char *s) {
        std::memcpy(f.memory.data() + addr, s, std::strlen(s) + 1);
    };

    put(text, "Text");
    put(0x3D60, "A|B");
    put(0x3D70, "50%");
    m68k_write32(&f.ctx, es + 0, 20);
    m68k_write32(&f.ctx, es + 4, 0);
    m68k_write32(&f.ctx, es + 8, 0);
    m68k_write32(&f.ctx, es + 12, text);
    m68k_write32(&f.ctx, es + 16, labels);
    const Case cases[] =
    {
        /* a | from an argument does not make a gadget */
        { "%s", { 0x3D60 }, { "A|B" } },
        /* the labels end at the first | of the formatted labels */
        { "%s|Cancel", { 0x3D60 }, { "A", "B|Cancel" } },
        /* a NUL from an argument ends a label, not the gadgets after it */
        { "A%c|B", { 0 }, { "A", "B" } },
        /* a % from an argument prints as it is */
        { "%s|OK", { 0x3D70 }, { "50%", "OK" } },
        /* a | that the template counts but precision 0 hides: AROS would
           look past the end for it, the bridge gives an empty label */
        { "A%.0|B", {}, { "AB", "" } },
        /* one gadget, and three */
        { "Fine", {}, { "Fine" } },
        { "Yes|No|Later", {}, { "Yes", "No", "Later" } },
    };
    for (const Case &c : cases)
    {
        put(labels, c.template_);
        ULONG at = args;
        for (ULONG w : c.words)
        {
            /* %s takes a LONG of the stream, %c a WORD */
            if (std::strstr(c.template_, "%c"))
            {
                m68k_write16(&f.ctx, at, (UWORD)w);
                at += 2;
            }
            else
            {
                m68k_write32(&f.ctx, at, w);
                at += 4;
            }
        }
        const ULONG req = f.call(Fixture::intuition, 594, { 0, es, 0, args }, { 0 });
        assert(req && !f.stopped() && easy.text == "Text" && easy.labels == c.expect);
        f.call(Fixture::intuition, 372, { req });
    }
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* More text than one requester takes fails as Intuition fails without
       memory: no requester, and the program goes on. */
    put(text, "%1100000s");
    put(labels, "OK");
    m68k_write32(&f.ctx, args, 0x3D70);
    const unsigned built = easy.built;
    assert(f.call(Fixture::intuition, 594, { 0, es, 0, args }, { 0 }) == 0 && !f.stopped());
    assert(f.call(Fixture::intuition, 588, { 0, es, 0, args }) == 0 && !f.stopped());
    assert(easy.built == built && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* OpenWindowTagList() gets one flat native list: through TAG_MORE,
   TAG_SKIP and TAG_IGNORE, the strings and the screens converted, the last
   WA_Gadgets checked and left out; a GadTools gadget stops the program,
   as do a screen tag with anything but a screen of the program and a list
   outside guest memory. */
static void test_window_tags()
{
    Fixture f;
    static int native_screen, native_gadget;
    const ULONG list = 0x3C00, more = 0x3C80, screen = 0x3CF0, gadget = 0x3CF8, classic = 0x3D00;
    ULONG named = 0xFFFFFFFF;
    auto items = [&](ULONG at, std::initializer_list<std::pair<ULONG, ULONG>> l)
    {
        for (auto &t : l)
        {
            m68k_write32(&f.ctx, at, t.first);
            m68k_write32(&f.ctx, at + 4, t.second);
            at += 8;
        }
        m68k_write32(&f.ctx, at, TAG_DONE);
    };
    auto tags = [&](ULONG at, std::vector<TagItem> &out) -> bool
    {
        ULONG size = 0;
        Gadget *head = reinterpret_cast<Gadget *>(1);
        f.ctx.running = TRUE;
        f.ctx.exit_code = 0;
        named = 0xFFFFFFFF;
        TagItem *t = Kaffeine_WindowTags(&f.ctx, at, &size, &named, &head);
        /* without the gadget bridge no native list comes back */
        assert(!head);
        out.clear();
        if (!t)
            return false;
        for (TagItem *i = t;; ++i)
        {
            out.push_back(*i);
            if (i->ti_Tag == TAG_DONE)
                break;
        }
        assert(size == out.size() * sizeof(TagItem));
        FreeMem(t, size);
        return true;
    };
    auto stopped = [&] { return !f.ctx.running && f.ctx.exit_code == 20; };
    std::vector<TagItem> out;
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + f.title), "A title");
    assert(Kaffeine_ShadowAdd(&f.ctx, screen, &native_screen, KAFFEINE_SHADOW_SCREEN));
    assert(Kaffeine_ShadowAdd(&f.ctx, gadget, &native_gadget, KAFFEINE_SHADOW_GADGET));

    /* no list */
    assert(tags(0, out) && out.size() == 1 && out[0].ti_Tag == TAG_DONE && named == 0);

    /* the conversions, also through TAG_MORE, and the controls left out */
    items(list, { { WA_Width, 200 }, { TAG_IGNORE, 0xDEADBEEF }, { TAG_SKIP, 1 }, { WA_Height, 0xDEADBEEF },
                  { WA_Title, f.title }, { TAG_MORE, more } });
    items(more, { { WA_CustomScreen, screen }, { WA_ScreenTitle, f.title }, { WA_Left, 0xFFFFFFFF } });
    assert(tags(list, out) && out.size() == 6 && !stopped());
    assert(out[0].ti_Tag == WA_Width && out[0].ti_Data == 200);
    assert(out[1].ti_Tag == WA_Title && out[1].ti_Data == (IPTR)(f.memory.data() + f.title));
    assert(out[2].ti_Tag == WA_CustomScreen && out[2].ti_Data == (IPTR)&native_screen);
    assert(out[3].ti_Tag == WA_ScreenTitle && out[3].ti_Data == (IPTR)(f.memory.data() + f.title));
    assert(out[4].ti_Tag == WA_Left && out[4].ti_Data == 0xFFFFFFFF && out[5].ti_Tag == TAG_DONE);
    assert(named == screen);

    /* WA_PubScreen: a public screen that the program locked, which
       LockPubScreen() gives as a type-13 public Screen, or a screen that it
       opened, as its native screen; NULL, the default public screen, stays
       NULL, as for WA_CustomScreen; the name, the fallback and the other
       tags as they are */
    static int native_pubscreen;
    const ULONG pubscreen = 0x3D10;
    assert(Kaffeine_ShadowAdd(&f.ctx, pubscreen, &native_pubscreen, KAFFEINE_SHADOW_PUBSCREEN));
    items(list, { { WA_PubScreen, pubscreen }, { WA_PubScreenName, f.title }, { WA_PubScreenFallBack, TRUE },
                  { WA_Width, 1 } });
    assert(tags(list, out) && out.size() == 5 && !stopped() && named == pubscreen);
    assert(out[0].ti_Tag == WA_PubScreen && out[0].ti_Data == (IPTR)&native_pubscreen);
    assert(out[1].ti_Tag == WA_PubScreenName && out[1].ti_Data == (IPTR)(f.memory.data() + f.title));
    assert(out[2].ti_Tag == WA_PubScreenFallBack && out[2].ti_Data == TRUE);
    assert(out[3].ti_Tag == WA_Width && out[3].ti_Data == 1);
    items(list, { { WA_PubScreen, screen }, { WA_CustomScreen, pubscreen } });
    assert(tags(list, out) && out.size() == 3 && !stopped() && named == pubscreen);
    assert(out[0].ti_Tag == WA_PubScreen && out[0].ti_Data == (IPTR)&native_screen);
    assert(out[1].ti_Tag == WA_CustomScreen && out[1].ti_Data == (IPTR)&native_pubscreen);
    items(list, { { WA_PubScreen, pubscreen }, { WA_PubScreen, 0 }, { WA_CustomScreen, 0 },
                  { WA_PubScreenFallBack, FALSE } });
    assert(tags(list, out) && out.size() == 5 && !stopped() && named == 0);
    assert(out[0].ti_Data == (IPTR)&native_pubscreen && out[1].ti_Tag == WA_PubScreen && out[1].ti_Data == 0 &&
           out[2].ti_Tag == WA_CustomScreen && out[2].ti_Data == 0 &&
           out[3].ti_Tag == WA_PubScreenFallBack && out[3].ti_Data == FALSE);

    /* Anything else in a screen tag stops the program before Intuition:
       an address of guest memory, a shadow that is no screen, an address
       outside guest memory, also through TAG_MORE and after a good one. */
    const unsigned calls = native_calls;
    for (ULONG bad : { ULONG(Fixture::raw), f.window, gadget, ULONG(0x00FFFFF0), ULONG(0xFFFFFFFC) })
        for (ULONG tag : { ULONG(WA_PubScreen), ULONG(WA_CustomScreen) })
        {
            items(list, { { tag, bad } });
            assert(!tags(list, out) && stopped());
            items(list, { { WA_PubScreen, pubscreen }, { WA_Width, 1 }, { TAG_MORE, more } });
            items(more, { { tag, bad }, { WA_PubScreen, pubscreen } });
            assert(!tags(list, out) && stopped());
        }
    assert(native_calls == calls);

    /* Intuition takes the last WA_Gadgets: a GadTools list that no bridge
       of gadgets knows stops the program, other gadgets stay out of the
       window */
    items(list, { { WA_Gadgets, gadget }, { WA_Width, 1 } });
    assert(!tags(list, out) && stopped());
    items(list, { { WA_Gadgets, gadget }, { WA_Gadgets, 0 } });
    assert(tags(list, out) && !stopped() && out.size() == 3 &&
           out[0].ti_Tag == TAG_IGNORE && out[1].ti_Tag == TAG_IGNORE);
    items(list, { { WA_Gadgets, 0 }, { TAG_MORE, more } });
    items(more, { { WA_Gadgets, gadget } });
    assert(!tags(list, out) && stopped());
    items(list, { { TAG_SKIP, 1 }, { WA_Gadgets, gadget }, { WA_Gadgets, classic } });
    assert(tags(list, out) && !stopped() && out.size() == 2 && out[0].ti_Tag == TAG_IGNORE);
    Gadget *head = nullptr;
    assert(Kaffeine_WindowGadgets(&f.ctx, 0, "test", &head) && !head &&
           Kaffeine_WindowGadgets(&f.ctx, classic, "test", &head) && !head);
    f.ctx.running = TRUE;
    assert(!Kaffeine_WindowGadgets(&f.ctx, gadget, "test", &head) && !head && stopped());

    /* a list outside guest memory, or in a cycle */
    items(list, { { WA_Width, 1 }, { TAG_MORE, 0xFFFFFFF0 } });
    assert(!tags(list, out) && stopped());
    items(list, { { WA_Width, 1 }, { TAG_MORE, list } });
    assert(!tags(list, out) && stopped());
    assert(!tags(0xFFFC, out) && stopped());

    /* the handler at -606 through the dispatcher: a refusal gives 20 in D0
       without Intuition, a window that Intuition does not open 0, an open
       one its 68k Window, or 0 without room for it, the native window
       closed again; the NewWindow of A0 is left out */
    const unsigned before = opened;
    items(list, { { WA_Gadgets, gadget } });
    f.call(Fixture::intuition, 606, { 0xDEAD0000, list });
    assert(f.stopped() && opened == before);
    f.call(Fixture::intuition, 606, { 0, 0xFFFC });
    assert(f.stopped() && opened == before);
    items(list, { { WA_Width, 320 }, { TAG_MORE, more } });
    items(more, { { WA_Title, Fixture::title } });
    open_result = nullptr;
    assert(f.call(Fixture::intuition, 606, { 0xDEAD0000, list }) == 0 && !f.stopped() && opened == before + 1);
    assert(opened_tags.size() == 3 && opened_tags[0].ti_Tag == WA_Width && opened_tags[0].ti_Data == 320 &&
           opened_tags[1].ti_Tag == WA_Title &&
           opened_tags[1].ti_Data == (IPTR)(f.memory.data() + Fixture::title) && opened_tags[2].ti_Tag == TAG_DONE);
    RastPort rp = {};
    Window win = {};
    win.Width = 320;
    win.RPort = &rp;
    win.UserPort = &ports[3];
    open_result = &win;
    const ULONG mw = f.call(Fixture::intuition, 606, { 0, list });
    assert(mw && !f.stopped() && f.is(mw, KAFFEINE_SHADOW_WINDOW, &win) && f.word(mw + M68K_WIN_WIDTH) == 320);
    f.closewindow(mw);
    assert(!closed.empty() && closed.back() == &win);
    f.leave_shadows(0);
    const size_t closes = closed.size();
    assert(f.call(Fixture::intuition, 606, { 0, list }) == 0 && !f.stopped() && closed.size() == closes + 1 &&
           closed.back() == &win);
    f.free_shadows();

    /* The native screen of WA_PubScreen reaches Intuition, and WScreen of
       the 68k Window is the 68k screen that the program named when the
       window lies there: of two 68k Screens of one native screen, the one
       named. Without one, or for a window that Intuition opened on another
       screen, it is the program's 68k screen of the screen that the window
       lies on. A refusal gives 20 in D0 without Intuition. */
    const ULONG again = 0x3D20;
    assert(Kaffeine_ShadowAdd(&f.ctx, again, &native_pubscreen, KAFFEINE_SHADOW_PUBSCREEN));
    win.WScreen = reinterpret_cast<Screen *>(&native_pubscreen);
    items(list, { { WA_PubScreen, again }, { WA_Left, 10 } });
    ULONG m = f.call(Fixture::intuition, 606, { 0, list });
    assert(m && !f.stopped() && opened_tags.size() == 3 && opened_tags[0].ti_Tag == WA_PubScreen &&
           opened_tags[0].ti_Data == (IPTR)&native_pubscreen && f.lng(m + M68K_WIN_WSCREEN) == again);
    f.closewindow(m);
    items(list, { { WA_PubScreen, pubscreen } });
    m = f.call(Fixture::intuition, 606, { 0, list });
    assert(m && opened_tags[0].ti_Data == (IPTR)&native_pubscreen && f.lng(m + M68K_WIN_WSCREEN) == pubscreen);
    f.closewindow(m);
    items(list, { { WA_PubScreen, 0 } });
    m = f.call(Fixture::intuition, 606, { 0, list });
    assert(m && opened_tags[0].ti_Tag == WA_PubScreen && opened_tags[0].ti_Data == 0 &&
           f.lng(m + M68K_WIN_WSCREEN) == pubscreen);
    f.closewindow(m);
    win.WScreen = reinterpret_cast<Screen *>(&native_screen);
    items(list, { { WA_PubScreen, again }, { WA_PubScreenName, f.title } });
    m = f.call(Fixture::intuition, 606, { 0, list });
    assert(m && f.lng(m + M68K_WIN_WSCREEN) == screen);
    f.closewindow(m);
    const unsigned opens = opened;
    for (ULONG bad : { ULONG(Fixture::raw), f.window, gadget })
    {
        items(list, { { WA_Width, 320 }, { WA_PubScreen, bad } });
        f.call(Fixture::intuition, 606, { 0, list });
        assert(f.stopped() && opened == opens);
    }
    win.WScreen = nullptr;
    open_result = nullptr;
}

/* OpenWindow() gives Intuition the fields of the NewWindow as tags, and
   the screen that its Type picks: WBENCHSCREEN the Workbench, whatever
   Screen holds, PUBLICSCREEN the public screen in Screen or the default
   one for NULL, another type the screen in Screen or the Workbench for
   NULL. A Screen that is not a screen of the program and a NewWindow
   outside guest memory stop the program before Intuition. */
static void test_open_window()
{
    Fixture f;
    static int native_screen, native_pubscreen, native_gadget;
    const ULONG nw = 0x3C00, opened_screen = 0x3CF0, gadget = 0x3CF8, locked = 0x3D10, again = 0x3D20;
    const ULONG last = 0x10000 - M68K_SIZEOF_NEWWINDOW;
    assert(Kaffeine_ShadowAdd(&f.ctx, opened_screen, &native_screen, KAFFEINE_SHADOW_SCREEN));
    assert(Kaffeine_ShadowAdd(&f.ctx, gadget, &native_gadget, KAFFEINE_SHADOW_GADGET));
    assert(Kaffeine_ShadowAdd(&f.ctx, locked, &native_pubscreen, KAFFEINE_SHADOW_PUBSCREEN));
    assert(Kaffeine_ShadowAdd(&f.ctx, again, &native_pubscreen, KAFFEINE_SHADOW_PUBSCREEN));
    auto newwindow = [&](ULONG at, UWORD type, ULONG screen)
    {
        std::memset(f.memory.data() + at, 0, M68K_SIZEOF_NEWWINDOW);
        m68k_write16(&f.ctx, at + M68K_NW_LEFTEDGE, 0xFFFB);
        m68k_write16(&f.ctx, at + M68K_NW_TOPEDGE, 15);
        m68k_write16(&f.ctx, at + M68K_NW_WIDTH, 240);
        m68k_write16(&f.ctx, at + M68K_NW_HEIGHT, 0x8000);
        f.memory[at + M68K_NW_DETAILPEN] = 0xFF;
        f.memory[at + M68K_NW_BLOCKPEN] = 2;
        m68k_write32(&f.ctx, at + M68K_NW_IDCMPFLAGS, IDCMP_INTUITICKS);
        m68k_write32(&f.ctx, at + M68K_NW_FLAGS, 0x0000100F);
        m68k_write32(&f.ctx, at + M68K_NW_TITLE, Fixture::title);
        m68k_write32(&f.ctx, at + M68K_NW_SCREEN, screen);
        m68k_write16(&f.ctx, at + M68K_NW_MINWIDTH, 50);
        m68k_write16(&f.ctx, at + M68K_NW_MINHEIGHT, 0xFFFF);
        m68k_write16(&f.ctx, at + M68K_NW_MAXWIDTH, 0xFFFF);
        m68k_write16(&f.ctx, at + M68K_NW_MAXHEIGHT, 400);
        m68k_write16(&f.ctx, at + M68K_NW_TYPE, type);
    };
    /* the screen tag of the last window that Intuition was asked for */
    auto screen_tag = [&] { assert(opened_tags.size() == 16); return opened_tags[9]; };
    RastPort rp = {};
    Window win = {};
    win.Width = 240;
    win.RPort = &rp;
    win.UserPort = &ports[3];

    /* no NewWindow: 0 without Intuition */
    unsigned calls = native_calls;
    assert(f.call(Fixture::intuition, 204, { 0 }) == 0 && !f.stopped() && native_calls == calls);

    /* the fields as tags, with the public screen in Screen */
    newwindow(nw, PUBLICSCREEN, again);
    open_result = &win;
    win.WScreen = reinterpret_cast<Screen *>(&native_pubscreen);
    ULONG m = f.call(Fixture::intuition, 204, { nw });
    assert(m && !f.stopped() && f.is(m, KAFFEINE_SHADOW_WINDOW, &win) && f.word(m + M68K_WIN_WIDTH) == 240);
    const std::vector<std::pair<Tag, IPTR>> fields =
        { { WA_Left, (IPTR)(WORD)-5 }, { WA_Top, 15 }, { WA_Width, 240 }, { WA_Height, (IPTR)(WORD)-32768 },
          { WA_DetailPen, 0xFF }, { WA_BlockPen, 2 }, { WA_IDCMP, IDCMP_INTUITICKS }, { WA_Flags, 0x100F },
          { WA_Title, (IPTR)(f.memory.data() + Fixture::title) }, { WA_PubScreen, (IPTR)&native_pubscreen },
          { WA_MinWidth, 50 }, { WA_MinHeight, (IPTR)(WORD)-1 }, { WA_MaxWidth, 0xFFFF }, { WA_MaxHeight, 400 },
          { TAG_IGNORE, 0 }, { TAG_DONE, 0 } };
    assert(opened_tags.size() == fields.size());
    for (size_t i = 0; i < fields.size(); ++i)
        assert(opened_tags[i].ti_Tag == fields[i].first &&
               (fields[i].first == TAG_DONE || opened_tags[i].ti_Data == fields[i].second));
    /* WScreen: the 68k screen named, of the two of that native screen */
    assert(f.lng(m + M68K_WIN_WSCREEN) == again);
    f.closewindow(m);
    newwindow(nw, PUBLICSCREEN, locked);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && screen_tag().ti_Data == (IPTR)&native_pubscreen && f.lng(m + M68K_WIN_WSCREEN) == locked);
    f.closewindow(m);

    /* PUBLICSCREEN with NULL: the default public screen */
    newwindow(nw, PUBLICSCREEN, 0);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && screen_tag().ti_Tag == WA_PubScreen && screen_tag().ti_Data == 0 &&
           f.lng(m + M68K_WIN_WSCREEN) == locked);
    f.closewindow(m);

    /* WBENCHSCREEN: Screen is not read */
    newwindow(nw, WBENCHSCREEN, 0xDEADBEEF);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && !f.stopped() && screen_tag().ti_Tag == TAG_IGNORE);
    f.closewindow(m);

    /* CUSTOMSCREEN and other types: the screen in Screen, the Workbench
       for NULL as before */
    newwindow(nw, CUSTOMSCREEN, opened_screen);
    win.WScreen = reinterpret_cast<Screen *>(&native_screen);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && screen_tag().ti_Tag == WA_CustomScreen && screen_tag().ti_Data == (IPTR)&native_screen &&
           f.lng(m + M68K_WIN_WSCREEN) == opened_screen);
    f.closewindow(m);
    newwindow(nw, 0, again);
    win.WScreen = reinterpret_cast<Screen *>(&native_pubscreen);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && screen_tag().ti_Tag == WA_CustomScreen && screen_tag().ti_Data == (IPTR)&native_pubscreen &&
           f.lng(m + M68K_WIN_WSCREEN) == again);
    f.closewindow(m);
    newwindow(nw, CUSTOMSCREEN, 0);
    m = f.call(Fixture::intuition, 204, { nw });
    assert(m && screen_tag().ti_Tag == TAG_IGNORE);
    f.closewindow(m);

    /* a NewWindow in the last bytes of guest memory */
    newwindow(last, WBENCHSCREEN, 0);
    m = f.call(Fixture::intuition, 204, { last });
    assert(m && !f.stopped() && screen_tag().ti_Tag == TAG_IGNORE);
    f.closewindow(m);

    /* Intuition opens no window: 0 */
    open_result = nullptr;
    newwindow(nw, PUBLICSCREEN, locked);
    assert(f.call(Fixture::intuition, 204, { nw }) == 0 && !f.stopped());

    /* Refusals before Intuition: a Screen that is not a screen of the
       program, a GadTools gadget, a NewWindow outside guest memory */
    open_result = &win;
    const unsigned opens = opened;
    for (UWORD type : { UWORD(PUBLICSCREEN), UWORD(CUSTOMSCREEN), UWORD(0) })
        for (ULONG bad : { ULONG(Fixture::raw), f.window, gadget, ULONG(0x00FFFFF0) })
        {
            newwindow(nw, type, bad);
            f.call(Fixture::intuition, 204, { nw });
            assert(f.stopped() && opened == opens);
        }
    newwindow(nw, WBENCHSCREEN, 0);
    m68k_write32(&f.ctx, nw + M68K_NW_FIRSTGADGET, gadget);
    f.call(Fixture::intuition, 204, { nw });
    assert(f.stopped() && opened == opens);
    for (ULONG at : { last + 1, ULONG(0xFFFFFFF0) })
    {
        f.call(Fixture::intuition, 204, { at });
        assert(f.stopped() && opened == opens);
    }

    /* without room for its 68k Window the native window goes again */
    newwindow(nw, PUBLICSCREEN, locked);
    f.leave_shadows(0);
    const size_t closes = closed.size();
    assert(f.call(Fixture::intuition, 204, { nw }) == 0 && !f.stopped() && closed.size() == closes + 1 &&
           closed.back() == &win);
    f.free_shadows();
    open_result = nullptr;
}

static void window_tagitems(Fixture &f, ULONG at,
                            std::initializer_list<std::pair<ULONG, ULONG>> items)
{
    for (const auto &t : items)
    {
        m68k_write32(&f.ctx, at, t.first);
        m68k_write32(&f.ctx, at + 4, t.second);
        at += 8;
    }
    m68k_write32(&f.ctx, at, TAG_DONE);
}

static void test_window_gadget_open_hooks()
{
    Fixture f;
    Gadget ignored{}, source{}, native[2]{}, system[2]{};
    const ULONG first = 0x3B00, last = 0x3B40, tags = 0x3C00, more = 0x3C80;
    assert(Kaffeine_ShadowAdd(&f.ctx, first, &ignored, KAFFEINE_SHADOW_GADGET));
    assert(Kaffeine_ShadowAdd(&f.ctx, last, &source, KAFFEINE_SHADOW_GADGET));
    f.ctx.window_gadgets = &recording_gadgets;
    prepared_head = &native[0]; // deliberately differs from ShadowNative(last)
    native[0].NextGadget = &native[1];
    system[0].NextGadget = &system[1];
    Window w{};
    w.FirstGadget = &system[0];
    open_result = &w;
    window_tagitems(f, tags, { { WA_Gadgets, first }, { WA_Width, 320 },
                              { TAG_SKIP, 1 }, { WA_Gadgets, 0xBAD0 }, { TAG_MORE, more } });
    window_tagitems(f, more, { { WA_Gadgets, last } });
    const ULONG guest = f.call(Fixture::intuition, 606, { 0xDEAD0000, tags });
    assert(guest && !f.stopped() && f.is(guest, KAFFEINE_SHADOW_WINDOW, &w));
    assert((window_events == std::vector<std::string>{ "prepare", "OpenWindowTagList", "opened" }));
    assert(window_gadget_calls.size() == 2 && window_gadget_calls[0].name == "prepare" &&
           window_gadget_calls[0].guest == last && window_gadget_calls[0].call == "OpenWindowTagList()" &&
           window_gadget_calls[1].name == "opened" && window_gadget_calls[1].window == &w &&
           window_gadget_calls[1].head == prepared_head);
    assert(opened_tags.size() == 4 && opened_tags[0].ti_Tag == TAG_IGNORE &&
           opened_tags[1].ti_Tag == WA_Width && opened_tags[1].ti_Data == 320 &&
           opened_tags[2].ti_Tag == WA_Gadgets && opened_tags[2].ti_Data == (IPTR)&native[0] &&
           opened_tags[3].ti_Tag == TAG_DONE);
    assert(w.FirstGadget == &system[0] && system[1].NextGadget == &native[0] &&
           native[0].NextGadget == &native[1] && !native[1].NextGadget);
    window_events.clear(); window_gadget_calls.clear();
    f.closewindow(guest);
    assert((window_events == std::vector<std::string>{ "closing", "CloseWindow" }));
    assert(window_gadget_calls.size() == 1 && window_gadget_calls[0].window == &w &&
           !Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_WINDOW) && closed.back() == &w);

    // A last NULL WA_Gadgets overrides an earlier GadTools one; a last list
    // of the program's own goes to the bridge, which may leave it off.
    prepared_head = nullptr;
    for (ULONG value : { 0u, Fixture::raw })
    {
        Window plain{};
        open_result = &plain;
        window_events.clear(); window_gadget_calls.clear();
        window_tagitems(f, tags, { { WA_Gadgets, first }, { WA_Gadgets, value } });
        const ULONG mw = f.call(Fixture::intuition, 606, { 0, tags });
        assert(mw && !f.stopped() && !plain.FirstGadget);
        if (value)
            assert(window_gadget_calls.size() == 1 && window_gadget_calls[0].name == "prepare" &&
                   window_gadget_calls[0].guest == value &&
                   (window_events == std::vector<std::string>{ "prepare", "OpenWindowTagList" }));
        else
            assert(window_gadget_calls.empty() &&
                   (window_events == std::vector<std::string>{ "OpenWindowTagList" }));
        assert(opened_tags.size() == 3 && opened_tags[0].ti_Tag == TAG_IGNORE &&
               opened_tags[1].ti_Tag == TAG_IGNORE);
        f.closewindow(mw);
    }
    prepared_head = &native[0];

    /* OpenWindow() takes the FirstGadget of its NewWindow the same way,
       as WA_Gadgets after its other fields */
    {
        Window nwin{};
        const ULONG nw = 0x3D00;
        open_result = &nwin;
        window_events.clear(); window_gadget_calls.clear();
        std::memset(f.memory.data() + nw, 0, M68K_SIZEOF_NEWWINDOW);
        m68k_write16(&f.ctx, nw + M68K_NW_WIDTH, 200);
        m68k_write16(&f.ctx, nw + M68K_NW_TYPE, WBENCHSCREEN);
        m68k_write32(&f.ctx, nw + M68K_NW_FIRSTGADGET, last);
        const ULONG mw = f.call(Fixture::intuition, 204, { nw });
        assert(mw && !f.stopped() && f.is(mw, KAFFEINE_SHADOW_WINDOW, &nwin));
        assert((window_events == std::vector<std::string>{ "prepare", "OpenWindowTagList", "opened" }));
        assert(window_gadget_calls.size() == 2 && window_gadget_calls[0].guest == last &&
               window_gadget_calls[0].call == "OpenWindow()" && window_gadget_calls[1].window == &nwin &&
               window_gadget_calls[1].head == prepared_head);
        assert(opened_tags.size() == 16 && opened_tags[14].ti_Tag == WA_Gadgets &&
               opened_tags[14].ti_Data == (IPTR)prepared_head && opened_tags[15].ti_Tag == TAG_DONE);
        f.closewindow(mw);
    }
    open_result = nullptr;
}

static void test_window_gadget_open_failures()
{
    enum Failure { PREPARE, NATIVE, WINDOW_SHADOW, RASTPORT_SHADOW, PORT_SHADOW, HEAP };
    for (Failure failure : { PREPARE, NATIVE, WINDOW_SHADOW, RASTPORT_SHADOW, PORT_SHADOW, HEAP })
    {
        Fixture f;
        Gadget source{}, native{};
        RastPort rp{};
        Window w{};
        const ULONG gadget = 0x3B00, tags = 0x3C00;
        assert(Kaffeine_ShadowAdd(&f.ctx, gadget, &source, KAFFEINE_SHADOW_GADGET));
        f.ctx.window_gadgets = &recording_gadgets;
        prepared_head = &native;
        prepare_refuses = failure == PREPARE;
        open_result = failure == NATIVE ? nullptr : &w;
        window_tagitems(f, tags, { { WA_Gadgets, gadget } });
        if (failure == WINDOW_SHADOW) f.leave_shadows(0);
        if (failure == RASTPORT_SHADOW)
        {
            w.RPort = &rp;
            f.leave_shadows(1);
        }
        if (failure == PORT_SHADOW)
        {
            w.UserPort = &ports[3];
            f.leave_shadows(1);
        }
        const ULONG size = failure == HEAP ? Kaffeine_HeapAvail(&f.ctx, MEMF_LARGEST) : 0;
        const ULONG taken = size ? Kaffeine_HeapAlloc(&f.ctx, size, MEMF_ANY) : 0;
        assert(failure != HEAP || taken);
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        const bool native_opened = failure != PREPARE && failure != NATIVE;
        expect_guest_window = !native_opened;
        assert(f.call(Fixture::intuition, 606, { 0, tags }) == (failure == PREPARE ? 20u : 0u));
        expect_guest_window = true;
        assert((failure == PREPARE) == f.stopped());
        assert(window_gadget_calls[0].name == "prepare" && window_gadget_calls[0].guest == gadget &&
               f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
        assert(opened == (failure == PREPARE ? 0u : 1u));
        assert(closed.size() == (native_opened ? 1u : 0u));
        if (native_opened)
        {
            /* the window laid the list out: it comes off before the
               window closes again, without a 68k Window */
            assert(closed[0] == &w && w.FirstGadget == &native);
            assert(window_gadget_calls.size() == 3 && window_gadget_calls[1].name == "opened" &&
                   window_gadget_calls[1].window == &w && window_gadget_calls[1].head == &native &&
                   window_gadget_calls[2].name == "closing" && window_gadget_calls[2].window == &w);
            const auto at = [&](const char *e) {
                return std::find(window_events.begin(), window_events.end(), e) - window_events.begin(); };
            assert(at("opened") < at("closing") && at("closing") < at("CloseWindow") &&
                   at("CloseWindow") < (long)window_events.size());
        }
        else
            /* a window that did not open drops what prepare made for it */
            assert(window_gadget_calls.size() == (failure == NATIVE ? 2u : 1u) &&
                   (failure != NATIVE || window_gadget_calls[1].name == "dropped") &&
                   std::count(window_events.begin(), window_events.end(), "opened") == 0 &&
                   std::count(window_events.begin(), window_events.end(), "closing") == 0);
        if (taken) Kaffeine_HeapFree(&f.ctx, taken, size);
        f.free_shadows();
        open_result = nullptr;
    }
}

static void test_window_gadget_close_order()
{
    Fixture f;
    f.ctx.window_gadgets = &recording_gadgets;
    f.closewindow(f.window);
    assert(window_gadget_calls.size() == 1 && window_gadget_calls[0].name == "closing" &&
           window_gadget_calls[0].window == &f.native_window);
    assert(window_events.size() >= 2 && window_events[0] == "closing" && window_events[1] == "CloseWindow");
    assert(closed.size() == 1 && closed[0] == &f.native_window && shadows_at_close == 0);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_start);
    const unsigned calls = native_calls;
    f.closewindow(f.window); f.closewindow(Fixture::raw); f.closewindow(0);
    assert(native_calls == calls && window_gadget_calls.size() == 1);
}

static void test_window_message_gadget_addresses()
{
    static_assert(KAFFEINE_SHADOW_GADGET == 12);
    enum Delivery { GET, WAIT, GT };
    for (Delivery delivery : { GET, WAIT, GT })
    {
        Fixture f;
        f.ctx.window_gadgets = &recording_gadgets;
        // A context, a button and both arrows can all be message addresses.
        Gadget gadgets[4]{};
        const ULONG mirrors[] = { 0x3A40, 0x3A80, 0x3AC0, 0x3B00 };
        for (unsigned i = 0; i < 4; ++i)
            assert(Kaffeine_ShadowAdd(&f.ctx, mirrors[i], &gadgets[i], KAFFEINE_SHADOW_GADGET));
        Gadget system{}, wrong_type{};
        TagItem update_tags[2]{};
        assert(Kaffeine_ShadowAdd(&f.ctx, 0x3B40, &wrong_type, FILLER));
        sync_guest = mirrors[0]; sync_native = &gadgets[0];
        const struct { APTR address; ULONG cls, expected; } cases[] =
        {
            { &gadgets[0], IDCMP_INTUITICKS, mirrors[0] },
            { &gadgets[1], IDCMP_INTUITICKS, mirrors[1] },
            { &gadgets[2], IDCMP_INTUITICKS, mirrors[2] },
            { &gadgets[3], IDCMP_INTUITICKS, mirrors[3] },
            { &f.native_window, IDCMP_ACTIVEWINDOW, f.window },
            { &gadgets[1], IDCMP_IDCMPUPDATE, 0 },
            { &f.native_window, IDCMP_IDCMPUPDATE, 0 },
            { update_tags, IDCMP_IDCMPUPDATE, 0 },
            { &system, IDCMP_INTUITICKS, 0 },
            { &wrong_type, IDCMP_INTUITICKS, 0 },
            { &ports[3], IDCMP_INTUITICKS, 0 },
            { f.memory.data() + Fixture::raw, IDCMP_INTUITICKS, 0 },
            { nullptr, IDCMP_INTUITICKS, 0 }
        };
        WORD value = -200;
        for (const auto &c : cases)
        {
            IntuiMessage *im = message(&f.native_window, c.cls);
            im->IAddress = c.address;
            im->Code = 0xBEEF; // a native termination code, not a gadget number
            f.send(im);
            gadgets[0].LeftEdge = value++;
            m68k_write16(&f.ctx, mirrors[0] + 4, 0x5555);
            window_events.clear(); window_gadget_calls.clear();
            ULONG guest = delivery == GET ? f.getmsg(f.port()) :
                          delivery == WAIT ? f.waitport(f.port()) : f.gt_getimsg(f.port());
            assert(guest && !f.stopped() && f.lng(guest + IM_IADDRESS) == c.expected &&
                   f.lng(guest + IM_IDCMPWINDOW) == f.window && f.word(guest + IM_CODE) == 0xBEEF);
            assert(window_gadget_calls.size() == 1 && window_gadget_calls[0].name == "sync" &&
                   window_gadget_calls[0].window == &f.native_window &&
                   window_gadget_calls[0].head == c.address &&
                   f.word(mirrors[0] + 4) == (UWORD)gadgets[0].LeftEdge);
            if (delivery == WAIT)
            {
                assert((window_events == std::vector<std::string>{ "WaitPort", "sync" }));
                // The same preview is filled again after the gadget changes.
                gadgets[0].LeftEdge = value++;
                assert(f.getmsg(f.port()) == guest && window_gadget_calls.size() == 2 &&
                       f.word(mirrors[0] + 4) == (UWORD)gadgets[0].LeftEdge);
            }
            else if (delivery == GET)
                assert((window_events == std::vector<std::string>{ "GetMsg", "sync" }));
            else
                assert((window_events == std::vector<std::string>{ "GetMsg", "filtering", "GT_FilterIMsg", "sync" }));
            if (delivery == GT) f.gt_replyimsg(guest); else f.reply(guest);
        }
        assert(gt_live == 0 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
        // An empty port never settles a window for which there is no message.
        window_gadget_calls.clear();
        assert(f.getmsg(f.port()) == 0 && f.gt_getimsg(f.port()) == 0 && window_gadget_calls.empty());
    }
}

static void test_window_filtered_message_sync()
{
    for (bool consumed : { false, true })
        for (bool previewed : { false, true })
            for (bool direct_filter : { false, true })
            {
                Fixture f;
                Gadget gadget{};
                const ULONG mirror = 0x3B00;
                assert(Kaffeine_ShadowAdd(&f.ctx, mirror, &gadget, KAFFEINE_SHADOW_GADGET));
                f.ctx.window_gadgets = &recording_gadgets;
                sync_guest = mirror; sync_native = filter_changes = &gadget;
                gadget.LeftEdge = 20;
                gt_only = consumed ? IDCMP_MOUSEMOVE : 0;
                IntuiMessage *im = message(&f.native_window, IDCMP_MOUSEMOVE);
                im->IAddress = &gadget;
                f.send(im);
                if (previewed) assert(f.waitport(f.port()));
                const ULONG original = direct_filter ? f.getmsg(f.port()) : 0;
                m68k_write16(&f.ctx, mirror + 4, 0x5555);
                window_events.clear(); window_gadget_calls.clear();
                const ULONG copy = direct_filter ? f.call(Fixture::gadtools, 102, { 0, original }) :
                                                  f.gt_getimsg(f.port());
                assert((copy == 0) == consumed && !f.stopped());
                assert(window_gadget_calls.size() == 1 && window_gadget_calls[0].name == "sync" &&
                       window_gadget_calls[0].window == &f.native_window &&
                       window_gadget_calls[0].head == &gadget && gadget.LeftEdge == 27 &&
                       f.word(mirror + 4) == 27);
                const auto filter = std::find(window_events.begin(), window_events.end(), "GT_FilterIMsg");
                assert(filter != window_events.end() && filter + 1 != window_events.end() && *(filter + 1) == "sync");
                if (consumed)
                {
                    assert(!gt_live);
                    if (direct_filter)
                    {
                        assert(replied.empty()); // GT_FilterIMsg leaves the original to the caller
                        f.reply(original);
                    }
                    assert(replied.size() == 1 && replied[0] == im);
                    const auto reply = std::find(window_events.begin(), window_events.end(), "ReplyMsg");
                    assert(reply > filter && *(reply - 1) == "sync");
                }
                else
                {
                    assert(f.lng(copy + IM_IADDRESS) == mirror && f.word(copy + IM_CODE) == im->Code);
                    f.gt_replyimsg(copy);
                }
                assert(!gt_live && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
            }

    // GT_GetIMsg must settle a consumed message even if a later message is delivered.
    Fixture f;
    Gadget gadget{};
    f.ctx.window_gadgets = &recording_gadgets;
    sync_guest = 0x3B00; sync_native = filter_changes = &gadget;
    gt_only = IDCMP_MOUSEMOVE;
    IntuiMessage *own = message(&f.native_window, IDCMP_MOUSEMOVE);
    IntuiMessage *next = message(&f.native_window, IDCMP_INTUITICKS);
    f.send(own); f.send(next);
    const ULONG copy = f.gt_getimsg(f.port());
    assert(copy && window_gadget_calls.size() == 2 && f.word(sync_guest + 4) == 14 &&
           replied.size() == 1 && replied[0] == own && f.lng(copy + IM_CLASS) == IDCMP_INTUITICKS);
    f.gt_replyimsg(copy);
    assert(!gt_live && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_window_filtered_address()
{
    for (bool direct_filter : { false, true })
    {
        Fixture f;
        Gadget arrow{}, scroller{};
        const ULONG child = 0x3B00, parent = 0x3B40;
        assert(Kaffeine_ShadowAdd(&f.ctx, child, &arrow, KAFFEINE_SHADOW_GADGET));
        assert(Kaffeine_ShadowAdd(&f.ctx, parent, &scroller, KAFFEINE_SHADOW_GADGET));
        f.ctx.window_gadgets = &recording_gadgets;
        sync_guest = parent; sync_native = filter_changes = filtered_address = &scroller;
        IntuiMessage *im = message(&f.native_window, IDCMP_MOUSEMOVE);
        im->IAddress = &arrow;
        im->Code = 0x8172;
        f.send(im);
        const ULONG orig = direct_filter ? f.getmsg(f.port()) : 0;
        window_gadget_calls.clear();
        const ULONG copy = direct_filter ? f.call(Fixture::gadtools, 102, { 0, orig }) : f.gt_getimsg(f.port());
        // the sync names the scroller of GadTools' copy, whose batch the arrow's is
        assert(copy && f.lng(copy + IM_IADDRESS) == parent && f.word(copy + IM_CODE) == 0x8172 &&
               im->IAddress == &arrow && window_gadget_calls.size() == 1 &&
               window_gadget_calls[0].head == &scroller && f.word(parent + 4) == 7);
        f.gt_replyimsg(copy);
        assert(!gt_live && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

/* A gadget message for a gadget that the program freed after the message
   came: GadTools never reads that gadget, and the program never sees the
   message. A GADGETUP still goes through GadTools without its gadget, so
   that GadTools forgets the gadget that it keeps active. Gadgets on the
   window, in a requester there or of the bridge still go through. */
static void test_window_stale_gadget_messages()
{
    for (bool direct_filter : { false, true })
        for (ULONG cls : { (ULONG)IDCMP_GADGETDOWN, (ULONG)IDCMP_GADGETUP, (ULONG)IDCMP_MOUSEMOVE })
        {
            Fixture f;
            f.ctx.window_gadgets = &recording_gadgets;
            Gadget system[2]{}, held{}, live_gadget{}, gone{};
            Requester older{}, newer{};
            const ULONG mirror = 0x3B00;
            assert(Kaffeine_ShadowAdd(&f.ctx, mirror, &live_gadget, KAFFEINE_SHADOW_GADGET));
            system[0].NextGadget = &system[1];
            f.native_window.FirstGadget = &system[0];
            newer.OlderRequest = &older;
            older.ReqGadget = &held;
            f.native_window.FirstRequest = &newer;
            for (Gadget *g : { &system[1], &held, &live_gadget, &gone })
            {
                const bool stale = g == &gone && cls != IDCMP_MOUSEMOVE;
                IntuiMessage *im = message(&f.native_window, cls);
                im->IAddress = g;
                f.send(im);
                const ULONG original = direct_filter ? f.getmsg(f.port()) : 0;
                assert(!direct_filter || f.lng(original + IM_IADDRESS) == (g == &live_gadget ? mirror : 0));
                window_events.clear(); window_gadget_calls.clear(); filter_saw.clear();
                filtering_calls.clear();
                const size_t replies = replied.size();
                const ULONG copy = direct_filter ? f.call(Fixture::gadtools, 102, { 0, original }) :
                                                  f.gt_getimsg(f.port());
                assert(!f.stopped());
                if (!stale)
                {
                    // the hook saw the message just before GadTools did
                    const auto filtering = std::find(window_events.begin(), window_events.end(), "filtering");
                    assert(filtering != window_events.end() && *(filtering + 1) == "GT_FilterIMsg" &&
                           filtering_calls.size() == 1 && filtering_calls[0].Class == cls &&
                           filtering_calls[0].IAddress == g);
                    assert(copy && filter_saw == std::vector<APTR>{ g } && im->IAddress == g);
                    assert(f.lng(copy + IM_IADDRESS) == (g == &live_gadget ? mirror : 0) &&
                           f.lng(copy + IM_CLASS) == cls);
                    f.gt_replyimsg(copy);
                    assert(replied.size() == replies + 1 && replied.back() == im);
                    continue;
                }
                // GadTools saw a GADGETUP without its gadget and nothing of a GADGETDOWN
                assert(!copy && !gt_live && window_gadget_calls.empty() && filtering_calls.empty());
                if (cls == IDCMP_GADGETUP)
                    assert(filter_saw == std::vector<APTR>{ nullptr } && !im->IAddress);
                else
                    assert(filter_saw.empty() && im->IAddress == &gone);
                if (direct_filter)
                {
                    assert(replied.size() == replies); // the program replies the original
                    f.reply(original);
                }
                else
                    assert((window_events.back() == "GetMsg")); // and GT_GetIMsg looked for another
                assert(replied.size() == replies + 1 && replied.back() == im);
            }
            assert(!gt_live && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
            f.native_window.FirstGadget = nullptr;
            f.native_window.FirstRequest = nullptr;
        }

    // GadTools keeps a stale GADGETUP of its own: nothing reaches the program.
    Fixture f;
    f.ctx.window_gadgets = &recording_gadgets;
    Gadget gone{};
    gt_only = IDCMP_GADGETUP;
    IntuiMessage *im = message(&f.native_window, IDCMP_GADGETUP);
    im->IAddress = &gone;
    f.send(im);
    assert(f.gt_getimsg(f.port()) == 0 && !f.stopped() && filter_saw == std::vector<APTR>{ nullptr } &&
           replied.size() == 1 && replied[0] == im && !gt_live);
}

/* What the window bridge tells the layer bridge (kaffeine_layers.c, not
   linked here): a 68k Window comes only with its 68k Layer, else nothing
   of it stays; WindowSync() writes the Flags that the layer bridge gives,
   and a message that is no VERIFY lets it take a snapshot; it hears of a close
   while the 68k Window and the native window are both still there. */
static std::vector<std::string> layer_calls;
static bool layer_open_fails;
static BOOL layer_opened(KaffeineContext *ctx, ULONG mw, Window *w)
{
    assert(ctx == live && Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW) == w);
    // everything else of the 68k Window is there already
    assert(!w->RPort || Kaffeine_ShadowNative(ctx, m68k_read32(ctx, mw + M68K_WIN_RPORT),
                                              KAFFEINE_SHADOW_RASTPORT) == w->RPort);
    assert(!w->UserPort || Kaffeine_ShadowNative(ctx, m68k_read32(ctx, mw + M68K_WIN_USERPORT),
                                                 KAFFEINE_SHADOW_USERPORT) == w->UserPort);
    layer_calls.push_back("opened");
    return !layer_open_fails;
}
static void layer_closing(KaffeineContext *ctx, ULONG mw, Window *w)
{
    assert(ctx == live && Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW) == w);
    assert(std::find(closed.begin(), closed.end(), w) == closed.end());
    layer_calls.push_back("closing");
    window_events.push_back("layer_closing");
}
static ULONG layer_flags(KaffeineContext *ctx, ULONG mw, ULONG native_flags)
{
    assert(ctx == live && mw);
    layer_calls.push_back("flags");
    // the program's backdrop bit, as the layer bridge keeps a pending one
    return (native_flags & ~(ULONG)WFLG_BACKDROP) |
           (m68k_read32(ctx, mw + M68K_WIN_FLAGS) & WFLG_BACKDROP);
}
static void layer_synced(KaffeineContext *ctx, ULONG mw, Window *w)
{
    assert(ctx == live && Kaffeine_ShadowNative(ctx, mw, KAFFEINE_SHADOW_WINDOW) == w);
    layer_calls.push_back("synced");
}
static void layer_screen(KaffeineContext *, Screen *) { assert(false); }
static const KaffeineLayerOps recording_layers =
    { layer_opened, layer_closing, layer_flags, layer_synced, layer_screen, layer_screen };

static void test_window_layer_hooks()
{
    Fixture f;
    f.ctx.layer_ops = &recording_layers;
    layer_calls.clear();
    RastPort other_rp = {};
    Window other = {};
    other.Width = 100;
    other.Flags = 0x1000;
    other.RPort = &other_rp;
    other.UserPort = &ports[3];
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;
    const std::vector<std::string> create = { "flags", "opened" };

    // no 68k Layer: no 68k Window, RastPort or port either
    layer_open_fails = true;
    assert(Kaffeine_WindowCreate(&f.ctx, &other) == 0 && layer_calls == create);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == shadows);
    assert(!Kaffeine_ShadowGuest(&f.ctx, &other, KAFFEINE_SHADOW_WINDOW) &&
           !Kaffeine_ShadowGuest(&f.ctx, &other_rp, KAFFEINE_SHADOW_RASTPORT) &&
           !Kaffeine_ShadowGuest(&f.ctx, &ports[3], KAFFEINE_SHADOW_USERPORT));
    layer_open_fails = false;
    layer_calls.clear();
    const ULONG o = Kaffeine_WindowCreate(&f.ctx, &other);
    assert(o && layer_calls == create);

    // a WFLG_BACKDROP of the program stays through WindowSync(); every
    // other flag follows Intuition
    m68k_write32(&f.ctx, o + M68K_WIN_FLAGS, 0x1000 | WFLG_BACKDROP | 0x4);
    layer_calls.clear();
    Kaffeine_WindowSync(&f.ctx, o, &other);
    assert(f.lng(o + M68K_WIN_FLAGS) == (0x1000 | WFLG_BACKDROP) && !(other.Flags & WFLG_BACKDROP));
    assert((layer_calls == std::vector<std::string>{ "flags" }));

    // a message brings the window and the snapshot of its screen up to date
    layer_calls.clear();
    f.send(message(&f.native_window, IDCMP_INTUITICKS));
    ULONG msg = f.getmsg(f.port());
    assert(msg && (layer_calls == std::vector<std::string>{ "flags", "synced" }));
    f.reply(msg);
    // but no layer lock while Intuition may wait for the reply of a VERIFY
    layer_calls.clear();
    f.send(message(&f.native_window, IDCMP_MENUVERIFY));
    msg = f.getmsg(f.port());
    assert(msg && (layer_calls == std::vector<std::string>{ "flags" }));
    f.reply(msg);

    // the close: the layer bridge first, the native window last
    layer_calls.clear();
    window_events.clear();
    f.closewindow(f.window);
    assert((layer_calls == std::vector<std::string>{ "closing" }));
    auto at = [](const char *e) {
        return std::find(window_events.begin(), window_events.end(), e) - window_events.begin();
    };
    assert(at("layer_closing") < at("CloseWindow") && at("CloseWindow") < (long)window_events.size());
    layer_calls.clear();
    Kaffeine_WindowDelete(&f.ctx, o);
    assert((layer_calls == std::vector<std::string>{ "closing" }));
    f.ctx.layer_ops = nullptr;
}

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "Layer bridge notifications: whole-or-nothing open, pending backdrop flags, close order", test_window_layer_hooks },
        { "Exec, Intuition and GadTools tables use their ABI slots", test_registration },
        { "A 68k Window comes whole or not at all", test_window_create },
        { "Window RastPorts share only live owned screen BitMaps and preserve their handles", test_window_bitmap_handle },
        { "WindowDelete() and CloseWindow() release WScreen once after the Window shadow", test_screen_release_order },
        { "An IDCMP port is a signalling 68k MsgPort", test_user_port },
        { "GetMsg() gives a 68k IntuiMessage and an up-to-date window", test_getmsg_fields },
        { "WaitPort(), messages taken natively and many in a row", test_waitport_and_many },
        { "68k ports that no message can reach yet", test_guest_ports },
        { "ModifyIDCMP() gives and takes the IDCMP port, or undoes", test_modifyidcmp },
        { "GadTools copies and messages of GadTools only", test_gadtools },
        { "CloseWindow() gives back every message of the window", test_close_window },
        { "BuildEasyRequestArgs(), SysReqHandler() and FreeSysRequest()", test_build_easy_request },
        { "Requester-only calls cannot consume an ordinary window", test_requester_window_type },
        { "Teardown closes regular windows and requesters through their own native APIs", test_owned_window_cleanup },
        { "EasyRequestArgs() and 68k input the bridge cannot take", test_easy_request_args },
        { "Gadget labels cut as AROS cuts them", test_easy_request_labels },
        { "OpenWindowTagList(): the tags of a new window, its screen and its gadgets", test_window_tags },
        { "OpenWindow(): the fields of a NewWindow and the screen of its Type", test_open_window },
        { "Window gadgets: the last WA_Gadgets uses prepare's native head and opened follows Intuition", test_window_gadget_open_hooks },
        { "Window gadgets: refused prepare and failed native open never call opened; a failed guest Window gives the list back", test_window_gadget_open_failures },
        { "Window gadgets: closing runs before deleting the shadow and closing the native window", test_window_gadget_close_order },
        { "Messages: type-12 IAddress mapping and gadget sync through GetMsg, WaitPort and GT_GetIMsg", test_window_message_gadget_addresses },
        { "Messages: GT_GetIMsg and GT_FilterIMsg sync after GadTools filters or consumes input", test_window_filtered_message_sync },
        { "Messages: a gadget message for a freed gadget never reaches GadTools' reads or the program", test_window_stale_gadget_messages },
        { "Messages: a filtered arrow address maps the returned scroller, with its native Code", test_window_filtered_address }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Windows, IDCMP messages and requesters: %zu groups passed\n", std::size(tests));
    return 0;
}
