/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Workbench calls through literal 68k LVOs, the production dispatcher,
    heap, shadow engine, window hooks, Exec I/O hooks and icon.library
    snapshots. Native Workbench records its arguments; native Exec supplies
    a queue and counts memory; native icon.library records every original,
    clone, part and base. Guest structures use the NDK 3.2 byte offsets,
    never host sizeof().
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <exec/memory.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include "kaffeine_dispatch.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_icon.h"
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"

/* These fixtures borrow metadata-only fonts and acquire no native opens. */
extern "C" void CloseFont(struct TextFont *) { std::abort(); }
#include "kaffeine_window.h"
#include "kaffeine_workbench.h"

static const char *case_name;
static unsigned checks;
static void require(bool ok, const char *what)
{
    ++checks;
    if (!ok)
    {
        std::fprintf(stderr, "FAIL %s: %s\n", case_name, what);
        std::exit(EXIT_FAILURE);
    }
}

/* Native memory and Exec. A wait can deliver a message after observing an
   empty native queue, without blocking the host test process. */
struct Block { ULONG size; bool vec; };
static std::map<APTR, Block> blocks;
static std::map<MsgPort *, std::deque<Message *>> queues;
static std::vector<Message *> replies;
static std::vector<std::string> events;
static Task owner;
static ULONG signals;
static unsigned get_calls, wait_calls, signal_allocations;
static bool waited_empty;
static Message *on_wait;
static MsgPort *on_wait_port;
static KaffeineContext *live;

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    APTR p = std::malloc(size ? size : 1);
    require(p != nullptr, "native allocation succeeds");
    std::memset(p, (flags & MEMF_CLEAR) ? 0 : 0xA5, size);
    require(blocks.emplace(p, Block{size, false}).second, "native block is unique");
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    if (!p) return;
    auto it = blocks.find(p);
    require(it != blocks.end() && it->second.size == size && !it->second.vec,
            "FreeMem matches the allocation and its size");
    bool app_port = false;
    for (const auto &entry : live->app_ports) app_port |= entry.guest && entry.native == p;
    events.push_back(app_port ? "FreeAppPort" : "FreeMem");
    blocks.erase(it);
    std::free(p);
}
extern "C" APTR AllocVec(ULONG size, ULONG flags)
{
    APTR p = AllocMem(size, flags);
    blocks.at(p).vec = true;
    return p;
}
static bool linked(ULONG list, ULONG msg);
extern "C" void FreeVec(APTR p)
{
    if (!p) return;
    auto it = blocks.find(p);
    require(it != blocks.end() && it->second.vec, "FreeVec matches a live vector");
    /* Observe the boundary itself: unlinking after freeing the carrier is too late. */
    for (const auto &copy : live->app_messages)
        if (copy.guest && copy.native == p)
            require(!linked(live->app_ports[copy.port].guest + 20, copy.guest),
                    "a carrier goes after its guest copy is unlinked");
    blocks.erase(it);
    std::free(p);
}
static size_t vector_count()
{
    return std::count_if(blocks.begin(), blocks.end(), [](const auto &b) { return b.second.vec; });
}
static bool copied_native(const void *p, size_t size = 1)
{
    const IPTR address = reinterpret_cast<IPTR>(p);
    for (const auto &[block, info] : blocks)
    {
        const IPTR start = reinterpret_cast<IPTR>(block);
        if (info.vec && address >= start && address - start < info.size &&
            size <= info.size - (address - start)) return true;
    }
    return false;
}
extern "C" BYTE AllocSignal(LONG n)
{
    ++signal_allocations;
    if (n < 0)
        for (n = 31; n >= 16 && (signals & (1u << n)); --n) {}
    if (n < 0 || n > 31 || (signals & (1u << n))) return -1;
    signals |= 1u << n;
    return static_cast<BYTE>(n);
}
extern "C" void FreeSignal(LONG n)
{
    require(n >= 0 && n < 32 && (signals & (1u << n)), "only an allocated signal is freed");
    events.push_back("FreeSignal");
    signals &= ~(1u << n);
}
extern "C" Task *FindTask(CONST_STRPTR name)
{
    require(!name, "FindTask asks for the current task");
    return &owner;
}
extern "C" Message *GetMsg(MsgPort *port)
{
    if ((port->mp_Flags & PF_ACTION) != PA_CALL) ++get_calls;
    events.push_back("GetMsg");
    auto &queue = queues[port];
    if (queue.empty()) return nullptr;
    Message *msg = queue.front();
    queue.pop_front();
    return msg;
}
extern "C" Message *WaitPort(MsgPort *port)
{
    ++wait_calls;
    events.push_back("WaitPort");
    auto &queue = queues[port];
    waited_empty = queue.empty();
    if (queue.empty() && on_wait)
    {
        require(port == on_wait_port + 1, "wait uses the inbox behind the intended front port");
        Message *msg = on_wait;
        on_wait = nullptr;
        PutMsg(on_wait_port, msg);
    }
    require(!queue.empty(), "native wait has a delivery and cannot hang");
    return queue.front();
}
static bool linked(ULONG list, ULONG msg)
{
    ULONG node = m68k_read32(live, list);
    for (unsigned n = 0; node && node != list + 4 && n < 32; ++n)
    {
        if (node == msg) return true;
        node = m68k_read32(live, node);
    }
    return false;
}
extern "C" void ReplyMsg(Message *msg)
{
    require(std::find(replies.begin(), replies.end(), msg) == replies.end(),
            "native message is replied exactly once");
    replies.push_back(msg);
    events.push_back("ReplyMsg");
    msg->mn_Node.ln_Type = NT_REPLYMSG;
}

/* None of these unrelated native paths should be needed by a guest App port. */
extern "C" void Signal(Task *, ULONG) { require(false, "unexpected native Signal"); }
extern "C" MsgPort *CreateMsgPort() { require(false, "App ports must share the guest signal"); return nullptr; }
extern "C" void DeleteMsgPort(MsgPort *) { require(false, "App ports do not own another signal"); }
extern "C" APTR CreateIORequest(MsgPort *, ULONG) { require(false, "unexpected I/O request"); return nullptr; }
extern "C" void DeleteIORequest(APTR) { require(false, "unexpected I/O request deletion"); }
extern "C" LONG OpenDevice(CONST_STRPTR, IPTR, IORequest *, ULONG) { require(false, "unexpected OpenDevice"); return 0; }
extern "C" void CloseDevice(IORequest *) { require(false, "unexpected CloseDevice"); }
extern "C" LONG DoIO(IORequest *) { require(false, "unexpected DoIO"); return 0; }
/* PutMsg as exec has it: the message goes on the port, then PA_SIGNAL would
   signal and PA_CALL calls the function in mp_SigTask with the port. */
static std::vector<Message *> sent;
extern "C" void PutMsg(MsgPort *port, Message *msg)
{
    events.push_back("PutMsg");
    msg->mn_Node.ln_Type = NT_MESSAGE;
    for (const auto &entry : live->app_ports)
        if (entry.guest && entry.native == port) sent.push_back(msg);
    queues[port].push_back(msg);
    if ((port->mp_Flags & PF_ACTION) == PA_CALL)
        reinterpret_cast<void (*)(MsgPort *, ExecBase *)>(port->mp_SigTask)(port, nullptr);
    else if ((port->mp_Flags & PF_ACTION) == PA_SIGNAL)
        require(port->mp_SigTask == &owner, "an inbox signals the task of the program");
}
/* Each duplicate is a new value, mapped to the lock it duplicates. */
static std::map<BPTR, BPTR> dups;
static IPTR next_dup;
extern "C" BPTR DupLock(BPTR lock)
{
    require(lock != nullptr, "no NULL lock is duplicated");
    BPTR dup = reinterpret_cast<BPTR>(next_dup += 0x10);
    dups[dup] = lock;
    return dup;
}
extern "C" void UnLock(BPTR lock)
{
    require(dups.erase(lock) == 1, "only a duplicate that the bridge made is unlocked, once");
    events.push_back("UnLock");
}
extern "C" void RawPutChar(UBYTE) { require(false, "unexpected RawPutChar"); }
extern "C" Window *OpenWindowTagList(NewWindow *, TagItem *) { require(false, "unexpected OpenWindow"); return nullptr; }
extern "C" void FreeSysRequest(Window *) { require(false, "unexpected FreeSysRequest"); }
extern "C" void CloseWindow(Window *) { require(false, "unexpected CloseWindow"); }
extern "C" BOOL ModifyIDCMP(Window *, ULONG) { require(false, "unexpected ModifyIDCMP"); return FALSE; }
extern "C" IntuiMessage *GT_FilterIMsg(IntuiMessage *) { require(false, "unexpected IDCMP filter"); return nullptr; }
extern "C" IntuiMessage *GT_PostFilterIMsg(IntuiMessage *) { require(false, "unexpected IDCMP postfilter"); return nullptr; }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { require(false, "unexpected OpenLibrary"); return 0; }
/* Timer requests are covered by test_exec_io; this fixture never opens one. */
extern "C" const M68KThunkEntry kaffeine_thunks_timer[] = {{0, nullptr}};
extern "C" const ULONG kaffeine_thunks_timer_count = 0;
extern "C" const M68KThunkEntry kaffeine_thunks_console[] = {{0, nullptr}};
extern "C" const ULONG kaffeine_thunks_console_count = 0;

/* Native icon.library. GetDiskObject(), which kaffeine_icon.c calls through
   the global base, gives an original; DupDiskObjectA() through a base that
   OpenLibrary() gave gives a clone. Every image, image data, string and
   array of each object is a part of its own, so that a clone provably
   shares nothing with its original, nor with guest memory. */
struct IconObject { Library *base; std::vector<void *> parts; };
static Library icon_library;
static std::map<DiskObject *, IconObject> originals, clones;
static unsigned icon_opens, icon_open_calls, dup_calls;
static bool fail_open, fail_dup;
static const char original_tool[] = "SYS:Utilities/Clock", original_window[] = "CON:0/0/200/50/Clock";
static const char original_type0[] = "DONOTWAIT", original_type1[] = "FORMAT=digital";
static const UWORD original_render[] = {0x8001, 0x4002, 0x2004, 0x1008}, original_select[] = {0xF00F, 0x0FF0, 0x3C3C, 0xC3C3};
static void *part(IconObject &owner, const void *from, size_t size)
{
    void *p = std::malloc(size);
    require(p != nullptr, "icon part allocation succeeds");
    std::memcpy(p, from, size);
    owner.parts.push_back(p);
    return p;
}
static bool part_of(const IconObject &owner, const void *p)
{
    return std::find(owner.parts.begin(), owner.parts.end(), p) != owner.parts.end();
}
static void release(IconObject &owner, DiskObject *d)
{
    for (void *p : owner.parts) std::free(p);
    delete d;
}
/* Depth planes of rows of words, as DupDiskObjectA() copies them */
static size_t image_bytes(const Image *im) { return ((im->Width + 15) >> 4) * 2 * im->Height * im->Depth; }
static Image *image_part(IconObject &owner, const Image &from, const void *data)
{
    auto *im = static_cast<Image *>(part(owner, &from, sizeof(from)));
    im->ImageData = static_cast<UWORD *>(part(owner, data, image_bytes(&from)));
    im->NextImage = nullptr;
    return im;
}
static STRPTR text_part(IconObject &owner, const char *s) { return static_cast<STRPTR>(part(owner, s, std::strlen(s) + 1)); }
static DiskObject *make_original()
{
    auto *d = new DiskObject{};
    IconObject &o = originals[d];
    d->do_Magic = WB_DISKMAGIC; d->do_Version = WB_DISKVERSION; d->do_Type = WBPROJECT;
    auto &g = d->do_Gadget;
    g.LeftEdge = -2; g.TopEdge = 3; g.Width = 16; g.Height = 3;
    g.Flags = GFLG_GADGIMAGE | GFLG_GADGHIMAGE; g.Activation = 0x103; g.GadgetType = GTYP_BOOLGADGET;
    g.MutualExclude = 0x2468; g.GadgetID = 0x4321;
    /* the revision of the icon, which the snapshot does not show */
    g.UserData = reinterpret_cast<APTR>(static_cast<IPTR>(1));
    g.GadgetRender = image_part(o, Image{0, 0, 16, 2, 2, nullptr, 3, 0, nullptr}, original_render);
    g.SelectRender = image_part(o, Image{0, 1, 16, 2, 2, nullptr, 3, 0, nullptr}, original_select);
    d->do_DefaultTool = text_part(o, original_tool);
    d->do_ToolWindow = text_part(o, original_window);
    STRPTR types[] = {text_part(o, original_type0), text_part(o, original_type1), nullptr};
    d->do_ToolTypes = static_cast<STRPTR *>(part(o, types, sizeof(types)));
    d->do_CurrentX = 120; d->do_CurrentY = -40; d->do_StackSize = 8192;
    return d;
}
extern "C" DiskObject *GetDiskObject(CONST_STRPTR name)
{
    require(name && std::strcmp(name, "SYS:Clock") == 0, "GetDiskObject gets the guest name");
    events.push_back("GetDiskObject");
    return make_original();
}
extern "C" void FreeDiskObject(DiskObject *d)
{
    require(!clones.count(d), "a clone never goes through the global icon.library base");
    auto it = originals.find(d);
    require(it != originals.end(), "the global FreeDiskObject frees a live original, once");
    events.push_back("FreeOriginal");
    release(it->second, d);
    originals.erase(it);
}
extern "C" DiskObject *GetDiskObjectNew(CONST_STRPTR) { require(false, "unexpected GetDiskObjectNew"); return nullptr; }
extern "C" DiskObject *GetDefDiskObject(LONG) { require(false, "unexpected GetDefDiskObject"); return nullptr; }
extern "C" DiskObject *NewDiskObject(ULONG) { require(false, "unexpected NewDiskObject"); return nullptr; }
extern "C" UBYTE *FindToolType(const STRPTR *, const STRPTR) { require(false, "unexpected FindToolType"); return nullptr; }
extern "C" BOOL MatchToolValue(UBYTE *, UBYTE *) { require(false, "unexpected MatchToolValue"); return FALSE; }
extern "C" Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    ++icon_open_calls;
    require(name && std::strcmp(name, "icon.library") == 0 && version >= 44,
            "an AppIcon opens icon.library 44, which has DupDiskObjectA()");
    events.push_back("OpenIcon");
    if (fail_open) return nullptr;
    ++icon_opens;
    return &icon_library;
}
extern "C" void CloseLibrary(Library *library)
{
    require(library == &icon_library && icon_opens > 0, "only an open icon.library base is closed");
    events.push_back("CloseIcon");
    --icon_opens;
    require(clones.size() <= icon_opens, "no reference closes while its clone lives");
}
extern "C" DiskObject *Test_DupDiskObjectA_WB(Library *base, DiskObject *from, TagItem *tags)
{
    ++dup_calls;
    require(base == &icon_library && icon_opens > clones.size(),
            "DupDiskObjectA goes through a base of its own, open, not the global one");
    require(originals.count(from) == 1, "DupDiskObjectA duplicates a live native original, never guest memory");
    require(!tags, "DupDiskObjectA has no tags: it duplicates images, data and strings");
    events.push_back("DupDiskObject");
    if (fail_dup) return nullptr;
    auto *d = new DiskObject(*from);
    IconObject &c = clones[d];
    c.base = base;
    for (APTR *render : {&d->do_Gadget.GadgetRender, &d->do_Gadget.SelectRender})
        if (*render)
        {
            const auto *im = static_cast<const Image *>(*render);
            *render = image_part(c, *im, im->ImageData);
        }
    if (from->do_DefaultTool) d->do_DefaultTool = text_part(c, from->do_DefaultTool);
    if (from->do_ToolWindow) d->do_ToolWindow = text_part(c, from->do_ToolWindow);
    if (from->do_ToolTypes)
    {
        size_t n = 0;
        while (from->do_ToolTypes[n]) ++n;
        std::vector<STRPTR> types(n + 1, nullptr);
        for (size_t i = 0; i < n; ++i) types[i] = text_part(c, from->do_ToolTypes[i]);
        d->do_ToolTypes = static_cast<STRPTR *>(part(c, types.data(), types.size() * sizeof(STRPTR)));
    }
    return d;
}
static std::vector<std::pair<APTR, const DiskObject *>> retained_icons();
extern "C" void Test_FreeDiskObject_WB(Library *base, DiskObject *d)
{
    auto it = clones.find(d);
    require(it != clones.end() && it->second.base == base, "a clone goes through its own base, once");
    require(base == &icon_library && icon_opens >= clones.size(), "the base of a clone is still open when it goes");
    for (const auto &[object, icon] : retained_icons())
        require(icon != d, "no clone goes while Workbench keeps its AppIcon");
    events.push_back("FreeClone");
    release(it->second, d);
    clones.erase(it);
}

/* Each native object has a distinct address, unrelated to its guest token. */
enum Kind { WINDOW = 1, ICON, MENU };
struct NativeObject { Kind kind; MsgPort *port; DiskObject *icon; const char *text; };
struct AddCall
{
    Kind kind;
    IPTR id, userdata;
    Window *window;
    MsgPort *port;
    TagItem *tags;
    const char *text;
    BPTR lock;
    DiskObject *icon;
    APTR result;
};
static std::deque<NativeObject> object_storage;
static std::map<APTR, NativeObject> objects;
static std::vector<AddCall> adds;
static std::vector<APTR> removes;
static BOOL remove_result, info_result;
static bool fail_add;
static BPTR info_lock;
static const char *info_name;
static Screen *info_screen;
static unsigned info_calls;
static APTR record_add(AddCall call)
{
    if (!fail_add)
    {
        object_storage.push_back({call.kind, call.port, call.icon, call.text});
        call.result = &object_storage.back();
        objects.emplace(call.result, object_storage.back());
    }
    adds.push_back(call);
    return call.result;
}
/* The DiskObject that each AppIcon on Workbench keeps, as AROS keeps it */
static std::vector<std::pair<APTR, const DiskObject *>> retained_icons()
{
    std::vector<std::pair<APTR, const DiskObject *>> icons;
    for (const auto &[object, native] : objects)
        if (native.kind == ICON) icons.emplace_back(object, native.icon);
    return icons;
}
/* What Workbench reads of an App object while it keeps it: its label and
   the DiskObject of an AppIcon, a native copy or a clone with its base */
static void check_kept(const NativeObject &native)
{
    if (native.kind == WINDOW) return;
    require(copied_native(native.text, 1), "the label of an App object lives while Workbench keeps it");
    if (native.kind != ICON) return;
    if (clones.count(native.icon))
        require(icon_opens >= clones.size(), "the clone of an AppIcon and its base live while Workbench keeps it");
    else
        require(copied_native(native.icon, sizeof(DiskObject)), "the copy of an AppIcon lives while Workbench keeps it");
}
extern "C" AppWindow *AddAppWindowA(IPTR id, IPTR ud, Window *window, MsgPort *port, TagItem *tags)
{
    return static_cast<AppWindow *>(record_add({WINDOW, id, ud, window, port, tags, nullptr, nullptr, nullptr, nullptr}));
}
extern "C" AppIcon *AddAppIconA(IPTR id, IPTR ud, CONST_STRPTR text, MsgPort *port,
                                BPTR lock, DiskObject *icon, TagItem *tags)
{
    events.push_back("AddAppIcon");
    NativeObject kept{ICON, port, icon, text};
    check_kept(kept);
    return static_cast<AppIcon *>(record_add({ICON, id, ud, nullptr, port, tags, text, lock, icon, nullptr}));
}
extern "C" AppMenuItem *AddAppMenuItemA(IPTR id, IPTR ud, APTR text, MsgPort *port, TagItem *tags)
{
    return static_cast<AppMenuItem *>(record_add({MENU, id, ud, nullptr, port, tags,
                                                static_cast<const char *>(text), nullptr, nullptr, nullptr}));
}
/* A removal takes the lock of Workbench, which the SendAppWindowMessage()
   of AROS holds while it waits for its reply: an AppMessage without a reply
   then would stop both. during_remove delivers while the removal waits. */
static std::function<void()> during_remove;
static BOOL record_remove(APTR object, Kind kind)
{
    auto it = objects.find(object);
    require(it != objects.end() && it->second.kind == kind, "native removal has the correct object and kind");
    check_kept(it->second);
    if (during_remove) { auto deliver = during_remove; during_remove = nullptr; deliver(); }
    for (Message *msg : sent)
        require(std::count(replies.begin(), replies.end(), msg) == 1,
                "no AppMessage waits for a reply while a removal takes the lock of Workbench");
    removes.push_back(object);
    events.push_back("RemoveAppObject");
    /* a refused object stays on Workbench, with what it reads */
    if (remove_result) objects.erase(it);
    return remove_result;
}
extern "C" BOOL RemoveAppWindow(AppWindow *p) { return record_remove(p, WINDOW); }
extern "C" BOOL RemoveAppIcon(AppIcon *p) { return record_remove(p, ICON); }
extern "C" BOOL RemoveAppMenuItem(AppMenuItem *p) { return record_remove(p, MENU); }
extern "C" BOOL WBInfo(BPTR lock, CONST_STRPTR name, Screen *screen)
{
    ++info_calls;
    info_lock = lock; info_name = name; info_screen = screen;
    return info_result;
}

/* NDK 3.2 guest offsets, independent of the native stub layouts. */
enum
{
    LN_SUCC = 0, LN_PRED = 4, LN_TYPE = 8, MN_LENGTH = 18,
    MP_FLAGS = 14, MP_SIGBIT = 15, MP_SIGTASK = 16, MP_LIST = 20,
    AM_TYPE = 20, AM_USERDATA = 22, AM_ID = 26, AM_NUMARGS = 30,
    AM_ARGLIST = 34, AM_VERSION_OFF = 38, AM_CLASS = 40, AM_MOUSEX = 42,
    AM_MOUSEY = 44, AM_SECONDS = 46, AM_MICROS = 50, AM_SIZE = 86,
    ARG_LOCK = 0, ARG_NAME = 4, ARG_SIZE = 8,
    DO_MAGIC = 0, DO_VERSION = 2, DO_GADGET = 4, DO_TYPE = 48,
    DO_TOOL = 50, DO_TYPES = 54, DO_X = 58, DO_Y = 62, DO_DRAWER = 66, DO_WINDOW = 70,
    DO_STACK = 74, DO_SIZE = 78,
    GG_NEXT = 0, GG_LEFT = 4, GG_TOP = 6, GG_WIDTH = 8, GG_HEIGHT = 10, GG_FLAGS = 12,
    GG_ACTIVATION = 14, GG_TYPE = 16, GG_RENDER = 18, GG_SELECT = 22, GG_TEXT = 26,
    GG_MUTEX = 30, GG_SPECIAL = 34, GG_ID = 38, GG_USERDATA = 40,
    IM_LEFT = 0, IM_TOP = 2, IM_WIDTH = 4, IM_HEIGHT = 6, IM_DEPTH = 8,
    IM_DATA = 10, IM_PICK = 14, IM_ONOFF = 15, IM_SIZE = 20
};
struct TestCPU
{
    ULONG d[8]{}, a[8]{};
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
static const KaffeineCPUOps ops = {get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr};
static const M68KThunkEntry exec_thunks[] = {KAFFEINE_EXEC_IO_THUNKS, KAFFEINE_EXEC_IDCMP_THUNKS};

/* A render Image: its Depth, its PlanePick and the planes of data that the
   program gives, which are those that drawing takes */
struct Planes { UWORD depth; UBYTE pick; unsigned data; };
/* The highlight of the icon's gadget: GADGHCOMP with a SelectRender to
   ignore, GADGHIMAGE with a select image, or GADGHNONE, which has the bit
   of GADGHIMAGE, with a select image */
enum Highlight { NO_SELECT, SELECT_IMAGE, NONE_WITH_SELECT };
static UWORD highlight_flags(Highlight highlight)
{
    return highlight == NO_SELECT ? GFLG_GADGHCOMP : highlight == SELECT_IMAGE ? GFLG_GADGHIMAGE : GFLG_GADGHNONE;
}

struct Delivery
{
    AppMessage am{};
    std::array<WBArg, 3> args{};
    std::array<char, 32> first{}, second{};
};
struct Fixture
{
    static constexpr ULONG exec = 0x2000, wb = 0x4000, stack = 0x800, task = 0x6000;
    static constexpr ULONG screen_addr = 0x6500, disk = 0x6800, image = 0x6900, select = 0x6940;
    static constexpr ULONG data = 0x6980, select_data = 0x69C0, text = 0x6A00, tool = 0x6B00;
    static constexpr ULONG types = 0x6C00, type1 = 0x6D00, type2 = 0x6E00, tags = 0x6F00;
    static constexpr ULONG bad_text = 0x7000, raw_message = 0x7800, icon_name = 0x7500, iconlib = 0x5000;
    static constexpr ULONG id = 0x89ABCDEF, userdata = 0xFEDCBA98, lock = 0xDA123456;
    KaffeineContext *ctx;
    TestCPU state;
    KaffeineCPU cpu{&ops, &state};
    Window window{};
    Screen screen{};
    ULONG guest_window;
    std::vector<std::unique_ptr<Delivery>> deliveries;

    Fixture()
    {
        require(blocks.empty(), "previous case released all native allocations");
        queues.clear(); replies.clear(); events.clear(); objects.clear(); adds.clear(); removes.clear();
        object_storage.clear();
        require(dups.empty(), "previous case unlocked every duplicated lock");
        next_dup = 0x51000000; owner.tc_Node.ln_Type = NT_PROCESS; during_remove = nullptr; sent.clear();
        signals = 0xFFFF; signal_allocations = get_calls = wait_calls = info_calls = 0;
        waited_empty = fail_add = false; on_wait = nullptr; on_wait_port = nullptr;
        remove_result = info_result = TRUE;
        info_lock = nullptr; info_name = nullptr; info_screen = nullptr;
        require(originals.empty() && clones.empty() && icon_opens == 0,
                "previous case released every icon and closed every icon.library reference");
        icon_open_calls = dup_calls = 0; fail_open = fail_dup = false;
        ctx = static_cast<KaffeineContext *>(AllocMem(sizeof(*ctx), MEMF_CLEAR));
        live = ctx;
        ctx->mem_size = 0x20000;
        ctx->mem = static_cast<UBYTE *>(AllocMem(ctx->mem_size + KAFFEINE_MEM_GUARD, MEMF_CLEAR));
        ctx->heap_start = 0x10000; ctx->heap_end = 0x1E000;
        require(Kaffeine_HeapInit(ctx), "production guest heap initializes");
        ctx->exec_task = &owner;
        ctx->m68k_sysbase = exec;
        m68k_write32(ctx, exec + M68K_EXECBASE_THISTASK, task);
        ctx->num_libs = 3;
        library(ctx->libs[0], exec, 700, "exec.library", exec_thunks, std::size(exec_thunks));
        library(ctx->libs[1], wb, 96, "workbench.library", kaffeine_thunks_workbench, kaffeine_thunks_workbench_count);
        library(ctx->libs[2], iconlib, 204, "icon.library", kaffeine_thunks_icon_manual, kaffeine_thunks_icon_manual_count);
        guest_window = Kaffeine_WindowCreate(ctx, &window);
        require(guest_window && Kaffeine_ShadowNative(ctx, guest_window, KAFFEINE_SHADOW_WINDOW) == &window,
                "production WindowCreate registers a WINDOW shadow");
        require(Kaffeine_ShadowAdd(ctx, screen_addr, &screen, KAFFEINE_SHADOW_SCREEN), "screen shadow registers");
        write_text(text, "Guest icon"); write_text(tool, "SYS:Utilities/Tool");
        write_text(type1, "MODE=fast"); write_text(type2, "FLAG"); write_text(icon_name, "SYS:Clock");
        std::memset(ctx->mem + bad_text, 'X', 1025);
    }
    ~Fixture()
    {
        Kaffeine_IOCleanup(ctx);
        require(objects.empty(), "cleanup removes every native App object");
        require(vector_count() == 0, "cleanup releases all native text and DiskObject copies");
        require(clones.empty() && icon_opens == 0, "cleanup frees every clone and closes every icon.library reference");
        /* the program's own icon.library objects go after, as at its end */
        Kaffeine_IconsCleanup(ctx);
        require(originals.empty(), "icon cleanup frees every original");
        require(signals == 0xFFFF, "cleanup returns every owned native signal");
        for (const auto &[port, queue] : queues)
        {
            require(queue.empty(), "cleanup drains native messages");
            require(!blocks.count(port), "cleanup frees native backing ports");
        }
        for (const auto &delivery : deliveries)
            require(std::count(replies.begin(), replies.end(), &delivery->am.am_Message) == 1,
                    "every native delivery eventually gets one reply");
        require(dups.empty(), "cleanup unlocks every duplicated lock");
        Kaffeine_WindowDelete(ctx, guest_window);
        Kaffeine_DestroyContext(ctx);
        live = nullptr;
        require(blocks.empty(), "case leaves no native allocation behind");
    }
    static void library(M68KFakeLibBase &lib, ULONG base, ULONG span, const char *name,
                        const M68KThunkEntry *table, ULONG count)
    {
        lib.m68k_addr = base; lib.jt_start = base - span;
        std::strcpy(lib.name, name); lib.thunks = table; lib.num_thunks = count;
    }
    void write_text(ULONG addr, const char *s) { std::strcpy(reinterpret_cast<char *>(ctx->mem + addr), s); }
    void word(ULONG addr, UWORD value) { m68k_write16(ctx, addr, value); }
    void longword(ULONG addr, ULONG value) { m68k_write32(ctx, addr, value); }
    ULONG read(ULONG addr) const { return m68k_read32(ctx, addr); }
    UWORD read_word(ULONG addr) const { return m68k_read16(ctx, addr); }
    bool valid(ULONG addr, ULONG size) const { return addr && addr < ctx->mem_size && size <= ctx->mem_size - addr; }
    /* This enters the real dispatcher, including its D0 result and RTS behavior. */
    ULONG call(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {}, ULONG d0 = id, ULONG d1 = userdata)
    {
        for (unsigned i = 0; i < 8; ++i) { state.d[i] = 0xD0000000 + i; state.a[i] = 0xA0000000 + i; }
        std::copy(a.begin(), a.end(), state.a);
        state.d[0] = d0; state.d[1] = d1; state.a[7] = stack;
        longword(stack, 0x76543210);
        const auto before = state;
        ctx->running = TRUE; ctx->exit_code = 0;
        KaffeineDispatchResult result{};
        require(Kaffeine_DispatchLibraryCall(ctx, &cpu, base - lvo, &result), "literal LVO dispatches");
        require(result.return_pc == 0x76543210 && !result.supervisor && state.a[7] == stack + 4,
                "dispatcher returns through the guest stack");
        for (unsigned i = 1; i < 8; ++i) require(state.d[i] == before.d[i], "non-result D register is preserved");
        for (unsigned i = 0; i < 7; ++i) require(state.a[i] == before.a[i], "argument A register is preserved");
        require(state.sr == before.sr && state.pc == before.pc, "SR and PC are preserved");
        return state.d[0];
    }
    void refused(ULONG result)
    {
        require(result == 20 && state.d[0] == 20 && !ctx->running && ctx->exit_code == 20,
                "refusal stops the guest with running FALSE, exit_code 20 and D0 20");
    }
    ULONG port()
    {
        ULONG p = call(exec, 666);
        require(valid(p, 34) && ctx->mem[p + MP_FLAGS] == PA_SIGNAL && read(p + MP_SIGTASK) == task,
                "CreateMsgPort returns a signalling 68k port of the current guest task");
        empty(p);
        return p;
    }
    void empty(ULONG p)
    {
        const ULONG list = p + MP_LIST;
        require(read(list) == list + 4 && read(list + 4) == 0 && read(list + 8) == list,
                "guest list has correct empty head, tail and predecessor sentinels");
    }
    ULONG add_window(ULONG p, ULONG tag = 0) { return call(wb, 48, {guest_window, p, tag}); }
    ULONG add_icon(ULONG p, ULONG tag = 0) { return call(wb, 60, {text, p, lock, disk, tag}); }
    /* A DiskObject of icon.library through its literal LVO: a metadata-only
       snapshot, with its native original behind it */
    ULONG snapshot()
    {
        const size_t before = originals.size();
        ULONG s = call(iconlib, 78, {icon_name});
        require(valid(s, DO_SIZE) && originals.size() == before + 1 && original_of(s),
                "GetDiskObject gives an owned guest snapshot of a new native original");
        require(!read(s + DO_GADGET + GG_RENDER) && !read(s + DO_GADGET + GG_SELECT),
                "the snapshot is metadata-only, without images");
        return s;
    }
    ULONG add_snapshot(ULONG p, ULONG s, ULONG label = text) { return call(wb, 60, {label, p, lock, s, 0}); }
    ULONG free_snapshot(ULONG s) { return call(iconlib, 90, {s}); }
    DiskObject *original_of(ULONG s) const
    {
        for (const auto &icon : ctx->icons) if (icon.guest == s) return icon.native;
        return nullptr;
    }
    std::vector<UBYTE> snapshot_bytes(ULONG s) const
    {
        for (const auto &icon : ctx->icons)
            if (icon.guest == s) return {ctx->mem + s, ctx->mem + s + icon.size};
        return {};
    }
    bool in_guest(const void *p) const
    {
        const IPTR address = reinterpret_cast<IPTR>(p), start = reinterpret_cast<IPTR>(ctx->mem);
        return address >= start && address - start < ctx->mem_size + KAFFEINE_MEM_GUARD;
    }
    ULONG add_menu(ULONG p, ULONG label = text, ULONG tag = 0) { return call(wb, 72, {label, p, tag, 0xFFFFFFFF}); }
    ULONG get(ULONG p) { return call(exec, 372, {p}); }
    ULONG wait(ULONG p) { return call(exec, 384, {p}); }
    void reply(ULONG msg) { require(call(exec, 378, {0, msg}) == 0, "ReplyMsg returns zero"); }
    MsgPort *native_port(ULONG p)
    {
        for (const auto &entry : ctx->app_ports) if (entry.guest == p) return entry.native;
        return nullptr;
    }
    bool token_present(ULONG token) const
    {
        for (const auto &entry : ctx->app_objects) if (entry.guest == token) return true;
        return false;
    }
    bool copy_present(ULONG msg) const
    {
        for (const auto &entry : ctx->app_messages) if (entry.guest == msg) return true;
        return false;
    }
    void check_token(ULONG token)
    {
        require(valid(token, 4) && token_present(token), "App token is tracked in guest memory");
        require(read(token) == 0, "new guest token memory is cleared");
    }
    /* An Image of 17 by 2 pixels (16 by 1 when small), whose data at pixels
       has words words */
    void image_at(ULONG at, ULONG pixels, UWORD depth, UBYTE pick, unsigned words, bool small = false)
    {
        std::memset(ctx->mem + at, 0, IM_SIZE);
        word(at + IM_LEFT, static_cast<UWORD>(-3)); word(at + IM_TOP, 9);
        word(at + IM_WIDTH, small ? 16 : 17); word(at + IM_HEIGHT, small ? 1 : 2);
        word(at + IM_DEPTH, depth); longword(at + IM_DATA, pixels);
        ctx->mem[at + IM_PICK] = pick; ctx->mem[at + IM_ONOFF] = 0x80;
        for (unsigned i = 0; i < words; ++i) word(pixels + 2 * i, 0x1234 + 0x111 * i);
    }
    /* The data of the render Image: two planes inside guest memory, one at
       its very end, where reading another plane would leave it, or with no
       plane picked a pointer outside guest memory, which nothing reads */
    ULONG render_data(unsigned planes) const { return planes == 2 ? data : planes ? ctx->mem_size - 8 : 0xFFFFFF00; }
    void diskobject(Planes planes = {2, 3, 2}, Highlight highlight = SELECT_IMAGE, bool built = false)
    {
        std::memset(ctx->mem + disk, 0, DO_SIZE);
        if (!built) { word(disk + DO_MAGIC, WB_DISKMAGIC); word(disk + DO_VERSION, WB_DISKVERSION); }
        ULONG g = disk + DO_GADGET;
        word(g + GG_LEFT, static_cast<UWORD>(-7)); word(g + GG_TOP, 11);
        word(g + GG_WIDTH, 31); word(g + GG_HEIGHT, 19);
        word(g + GG_FLAGS, (built ? 0 : GFLG_GADGIMAGE) | highlight_flags(highlight));
        word(g + GG_ACTIVATION, 0x103); word(g + GG_TYPE, GTYP_BOOLGADGET); word(g + GG_ID, 0xCDEF);
        longword(g + GG_RENDER, image);
        longword(g + GG_SELECT, highlight == NO_SELECT ? 0xFFFFFFF0 : select);
        ctx->mem[disk + DO_TYPE] = WBPROJECT;
        longword(disk + DO_TOOL, tool); longword(disk + DO_TYPES, types);
        longword(disk + DO_X, 0x80000000); longword(disk + DO_Y, 0x12345678); longword(disk + DO_STACK, 0x10203040);
        longword(types, type1); longword(types + 4, type2); longword(types + 8, 0);
        image_at(image, render_data(planes.data), planes.depth, planes.pick, planes.data * 4);
        image_at(select, select_data, 1, 1, 1, true);
    }
    Delivery *delivery(MsgPort *port, bool queued = true)
    {
        auto d = std::make_unique<Delivery>();
        std::strcpy(d->first.data(), "First file"); std::strcpy(d->second.data(), "Second:entry");
        d->args = {{{reinterpret_cast<BPTR>(0xAB123456), reinterpret_cast<BYTE *>(d->first.data())},
                    {reinterpret_cast<BPTR>(0xCD654321), reinterpret_cast<BYTE *>(d->second.data())},
                    {reinterpret_cast<BPTR>(0xEF765432), nullptr}}};
        auto &am = d->am;
        am.am_Message.mn_Node.ln_Type = NT_MESSAGE;
        am.am_Message.mn_Length = sizeof(AppMessage);
        am.am_Type = AMTYPE_APPWINDOW; am.am_UserData = userdata; am.am_ID = id;
        am.am_NumArgs = d->args.size(); am.am_ArgList = d->args.data();
        am.am_Version = AM_VERSION; am.am_Class = 0x1357; am.am_MouseX = -123; am.am_MouseY = 321;
        am.am_Seconds = 0x89ABCDEF; am.am_Micros = 987654;
        Delivery *p = d.get(); deliveries.push_back(std::move(d));
        if (queued) PutMsg(port, &p->am.am_Message);
        return p;
    }
    /* The inbox, where the carriers wait, follows the front port that
       Workbench gets. */
    static MsgPort *inbox(MsgPort *front) { return front + 1; }
    bool replied(const Delivery *d) const
    {
        return std::count(replies.begin(), replies.end(), &d->am.am_Message) == 1;
    }
};

static void check_image(Image *im, Planes planes, bool small = false)
{
    const unsigned plane = small ? 1 : 4, words = planes.depth * plane;
    require(copied_native(im, sizeof(*im)), "Image is in a native copy");
    require(im->LeftEdge == -3 && im->TopEdge == 9 && im->Width == (small ? 16 : 17) &&
            im->Height == (small ? 1 : 2) && im->Depth == planes.depth, "Image dimensions and signed offsets are copied");
    require(im->PlanePick == planes.pick && im->PlaneOnOff == 0x80 && !im->NextImage,
            "Image plane selection and terminator are copied");
    require(copied_native(im->ImageData, words * 2),
            "Image data has its Depth planes in native memory, as DupDiskObject() copies them");
    for (unsigned i = 0; i < planes.data * plane; ++i)
        require(im->ImageData[i] == 0x1234 + 0x111 * i,
                "big-endian words of the planes that drawing takes become native word values");
    for (unsigned i = planes.data * plane; i < words; ++i)
        require(im->ImageData[i] == 0, "the planes that drawing does not take are zero");
}
static void check_icon(Fixture &f, ULONG port, Planes planes, Highlight highlight)
{
    const auto &call = adds.back();
    require(call.kind == ICON && call.id == Fixture::id && call.userdata == Fixture::userdata &&
            call.port == f.native_port(port) && !call.tags,
            "AddAppIconA receives its scalar arguments, native port and NULL tags");
    require(call.lock == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::lock)), "icon lock passes without translation");
    require(copied_native(call.text, 11) && std::strcmp(call.text, "Guest icon") == 0, "icon label is copied");
    const DiskObject *d = call.icon;
    require(copied_native(d, sizeof(*d)), "DiskObject is a native copy");
    require(d->do_Magic == WB_DISKMAGIC && d->do_Version == WB_DISKVERSION && d->do_Type == WBPROJECT,
            "the copy has the magic and version of an icon, and the type");
    const auto &g = d->do_Gadget;
    require(g.LeftEdge == -7 && g.TopEdge == 11 && g.Width == 31 && g.Height == 19 &&
            g.Flags == (GFLG_GADGIMAGE | highlight_flags(highlight)) &&
            g.Activation == 0x103 && g.GadgetType == GTYP_BOOLGADGET && g.GadgetID == 0xCDEF,
            "all supported gadget fields have their guest values, and GFLG_GADGIMAGE");
    require(!g.NextGadget && !g.GadgetText && !g.SpecialInfo && !g.UserData,
            "unused gadget pointers are cleared");
    require(d->do_CurrentX == static_cast<LONG>(0x80000000) && d->do_CurrentY == 0x12345678 && d->do_StackSize == 0x10203040,
            "DiskObject coordinates and stack size are copied");
    require(copied_native(d->do_DefaultTool, 19) && std::strcmp(d->do_DefaultTool, "SYS:Utilities/Tool") == 0,
            "default tool string is copied");
    require(copied_native(d->do_ToolTypes, 3 * sizeof(STRPTR)) && d->do_ToolTypes[0] && d->do_ToolTypes[1] && !d->do_ToolTypes[2],
            "tool types are a native pointer array with a NULL end");
    require(copied_native(d->do_ToolTypes[0], 10) && copied_native(d->do_ToolTypes[1], 5) &&
            std::strcmp(d->do_ToolTypes[0], "MODE=fast") == 0 && std::strcmp(d->do_ToolTypes[1], "FLAG") == 0,
            "both tool type strings are copied");
    check_image(static_cast<Image *>(g.GadgetRender), planes);
    if (highlight != NO_SELECT) check_image(static_cast<Image *>(g.SelectRender), {1, 1, 1}, true);
    else require(!g.SelectRender, "SelectRender is ignored without GFLG_GADGHIMAGE");
    f.ctx->mem[Fixture::text] = 'x'; f.ctx->mem[Fixture::tool] = 'x'; f.ctx->mem[Fixture::type1] = 'x';
    if (planes.data) f.word(f.render_data(planes.data), 0);
    require(call.text[0] == 'G' && d->do_DefaultTool[0] == 'S' && d->do_ToolTypes[0][0] == 'M' &&
            (!planes.data || static_cast<Image *>(g.GadgetRender)->ImageData[0] == 0x1234),
            "native copies survive guest edits");
}

static void test_window_add_remove()
{
    Fixture f;
    ULONG p = f.port();
    const unsigned allocated = signal_allocations;
    ULONG available = Kaffeine_HeapAvail(f.ctx, 0);
    ULONG token = f.add_window(p);
    f.check_token(token);
    const auto call = adds.back();
    require(call.kind == WINDOW && call.window == &f.window && call.id == Fixture::id && call.userdata == Fixture::userdata &&
            call.port == f.native_port(p) && !call.tags, "AddAppWindowA gets the native WINDOW and arguments");
    MsgPort *inbox = Fixture::inbox(call.port);
    require(call.port && call.port->mp_Node.ln_Type == NT_MSGPORT && call.port->mp_Flags == PA_CALL &&
            call.port->mp_SigTask && call.port->mp_SigTask != &owner,
            "the port that Workbench gets calls a function (PA_CALL) for each AppMessage");
    require(inbox->mp_Node.ln_Type == NT_MSGPORT && inbox->mp_Flags == PA_SIGNAL &&
            inbox->mp_SigBit == f.ctx->mem[p + MP_SIGBIT] && inbox->mp_SigTask == &owner,
            "the inbox uses PA_SIGNAL, the guest signal bit and the current native task");
    for (MsgPort *port : {call.port, inbox})
        require(port->mp_MsgList.lh_Head == reinterpret_cast<Node *>(&port->mp_MsgList.lh_Tail) &&
                !port->mp_MsgList.lh_Tail &&
                port->mp_MsgList.lh_TailPred == reinterpret_cast<Node *>(&port->mp_MsgList.lh_Head),
                "both native ports start with an empty native list");
    require(signal_allocations == allocated, "backing port allocates no extra signal");
    require(f.call(Fixture::wb, 54, {token}) == TRUE && removes.back() == call.result,
            "RemoveAppWindow returns native TRUE for the native AppWindow");
    require(!f.token_present(token) && Kaffeine_HeapAvail(f.ctx, 0) == available, "removed token is forgotten and its guest memory is freed");
    f.refused(f.call(Fixture::wb, 54, {token}));
}
static void test_window_remove_false()
{
    Fixture f; ULONG p = f.port(), token = f.add_window(p); APTR native = adds.back().result;
    remove_result = FALSE;
    require(f.call(Fixture::wb, 54, {token}) == FALSE && f.token_present(token) && f.ctx->running && objects.count(native),
            "native FALSE preserves the window token and backing port for another removal");
    remove_result = TRUE;
    require(f.call(Fixture::wb, 54, {token}) == TRUE && !f.token_present(token) && !objects.count(native),
            "the same window token can be retried after Workbench refuses removal");
}
static void test_shared_port()
{
    Fixture f; ULONG p = f.port(), first = f.add_window(p); MsgPort *native = adds.back().port;
    f.longword(Fixture::tags, TAG_DONE);
    ULONG second = f.add_menu(p, Fixture::text, Fixture::tags);
    require(first != second && adds.size() == 2 && adds.back().port == native && signal_allocations == 1,
            "two App kinds on one guest port share one native port and signal");
    require(f.call(Fixture::wb, 54, {first}) == TRUE && f.native_port(p) == native && f.token_present(second),
            "removing one object preserves the other and its native port");
    require(f.call(Fixture::wb, 78, {second}) == TRUE && !f.token_present(second), "menu removes through its own LVO");
}
static void test_unknown_window()
{
    Fixture f; ULONG p = f.port();
    f.refused(f.call(Fixture::wb, 48, {0x1234, p, 0}));
    require(adds.empty() && !f.native_port(p), "unknown window never reaches native Workbench");
}
static void test_tags(Kind kind)
{
    Fixture f; ULONG p = f.port(); f.diskobject();
    f.longword(Fixture::tags, TAG_USER + 12); f.longword(Fixture::tags + 4, 0x12345678);
    f.longword(Fixture::tags + 8, TAG_DONE);
    f.refused(kind == WINDOW ? f.add_window(p, Fixture::tags) :
              kind == ICON ? f.add_icon(p, Fixture::tags) : f.add_menu(p, Fixture::text, Fixture::tags));
    require(adds.empty() && !f.native_port(p) && vector_count() == 0, "nonempty tags are refused before native allocation or calls");
}
static void test_bad_port(bool unowned)
{
    Fixture f; ULONG p = f.port();
    if (unowned) f.ctx->mem[p + MP_SIGBIT] = 5;
    else f.ctx->mem[p + MP_FLAGS] = PA_IGNORE;
    f.refused(f.add_window(p));
    require(adds.empty() && !f.native_port(p), "invalid guest port never gets a native backing port");
}
static void test_wrong_kind(Kind kind)
{
    Fixture f; ULONG p = f.port();
    ULONG token = kind == WINDOW ? f.add_menu(p) : f.add_window(p);
    f.refused(f.call(Fixture::wb, kind == WINDOW ? 54 : kind == ICON ? 66 : 78, {token}));
    require(removes.empty() && f.token_present(token), "wrong-kind removal preserves the real object");
}
static void test_unknown_token(ULONG lvo)
{
    Fixture f;
    f.refused(f.call(Fixture::wb, lvo, {0xDEADBEEF}));
    require(removes.empty(), "unknown nonzero token never reaches native removal");
}
static void test_null_icon()
{
    Fixture f;
    require(f.call(Fixture::wb, 66, {0}) == FALSE && f.ctx->running && f.ctx->exit_code == 0 && removes.empty(),
            "RemoveAppIcon(NULL) returns FALSE without stopping or a native call");
}
static void test_icon(Planes planes, Highlight highlight = SELECT_IMAGE, bool built = false,
                      const std::function<void(Fixture &)> &edit = nullptr)
{
    Fixture f; ULONG p = f.port(); f.diskobject(planes, highlight, built);
    if (edit) edit(f);
    ULONG token = f.add_icon(p); f.check_token(token);
    check_icon(f, p, planes, highlight);
    APTR native = adds.back().result;
    require(f.call(Fixture::wb, 66, {token}) == TRUE && removes.back() == native && !f.token_present(token) && vector_count() == 0,
            "RemoveAppIcon removes its native object and releases the complete copy");
}
static void test_bad_icon(const std::function<void(Fixture &)> &break_guest)
{
    Fixture f; ULONG p = f.port(); f.diskobject(); break_guest(f);
    f.refused(f.add_icon(p));
    require(adds.empty() && vector_count() == 0, "invalid icon is refused without a native call or a native icon copy");
    for (const auto &entry : f.ctx->app_objects) require(!entry.guest, "refused icon leaves no token");
}
/* A DiskObject that AddAppIconA() of AmigaOS 3.2.3 does not take, built as
   its autodoc has it */
static void test_failing_icon(const std::function<void(Fixture &)> &break_guest)
{
    Fixture f; ULONG p = f.port(); f.diskobject({2, 3, 2}, SELECT_IMAGE, true); break_guest(f);
    require(f.add_icon(p) == 0 && f.ctx->running && f.ctx->exit_code == 0,
            "AddAppIconA gives zero as AmigaOS 3.2.3 does, without stopping the program");
    require(adds.empty() && vector_count() == 0, "no native call and no native icon copy");
    for (const auto &entry : f.ctx->app_objects) require(!entry.guest, "no token");
}
static void test_icon_native_failure()
{
    Fixture f; ULONG p = f.port(); f.add_window(p); f.diskobject();
    const size_t before = blocks.size(); const ULONG available = Kaffeine_HeapAvail(f.ctx, 0);
    fail_add = true;
    require(f.add_icon(p) == 0 && f.ctx->running && f.ctx->exit_code == 0, "native AddAppIconA NULL becomes guest zero without stopping");
    require(adds.back().kind == ICON && !adds.back().result && blocks.size() == before && vector_count() == 0 &&
            Kaffeine_HeapAvail(f.ctx, 0) == available && objects.size() == 1,
            "failed native icon frees every new native allocation and the guest token");
}
/* A refused RemoveAppIcon() of a DiskObject that the program built: Workbench
   may still read the copy, which stays with the token until it lets go */
static void test_icon_remove_refused()
{
    Fixture f; ULONG p = f.port(); f.diskobject();
    ULONG token = f.add_icon(p); APTR native = adds.back().result; const DiskObject *copy = adds.back().icon;
    remove_result = FALSE;
    require(f.call(Fixture::wb, 66, {token}) == FALSE && f.ctx->running && f.token_present(token) &&
            objects.count(native) && copied_native(copy, sizeof(*copy)) && vector_count() == 1,
            "a refused AppIcon keeps its token and the copy that Workbench reads");
    remove_result = TRUE;
    require(f.call(Fixture::wb, 66, {token}) == TRUE && !f.token_present(token) && !objects.count(native) &&
            vector_count() == 0, "the copy goes once Workbench lets the AppIcon go");
}

/* ── AppIcons of icon.library DiskObjects ─────────────────────────── */

static size_t event_count(const char *name) { return std::count(events.begin(), events.end(), name); }
static size_t event_at(const char *name)
{
    auto it = std::find(events.begin(), events.end(), name);
    require(it != events.end(), "an expected native event happened");
    return static_cast<size_t>(it - events.begin());
}
static void check_clone_image(const IconObject &c, const IconObject &o, const void *clone, const void *original)
{
    const auto *ci = static_cast<const Image *>(clone), *oi = static_cast<const Image *>(original);
    require(ci && oi && ci != oi && part_of(c, ci) && !part_of(o, ci), "the clone has an Image of its own");
    require(ci->LeftEdge == oi->LeftEdge && ci->TopEdge == oi->TopEdge && ci->Width == oi->Width &&
            ci->Height == oi->Height && ci->Depth == oi->Depth && ci->PlanePick == oi->PlanePick &&
            ci->PlaneOnOff == oi->PlaneOnOff && !ci->NextImage, "the Image of the clone has the fields of the original");
    require(ci->ImageData && ci->ImageData != oi->ImageData && part_of(c, ci->ImageData) &&
            std::memcmp(ci->ImageData, oi->ImageData, image_bytes(oi)) == 0,
            "the clone has a copy of every plane of the image data");
}
static void check_clone_text(const IconObject &c, const char *clone, const char *original)
{
    require(clone && original && clone != original && part_of(c, clone) && std::strcmp(clone, original) == 0,
            "the clone has a copy of the string of the original");
}
/* The DiskObject that Workbench got last: a clone of the original behind
   the snapshot, at the position x, y */
static DiskObject *check_clone(Fixture &f, ULONG port, DiskObject *original, LONG x, LONG y)
{
    const auto &call = adds.back();
    require(call.kind == ICON && call.id == Fixture::id && call.userdata == Fixture::userdata &&
            call.port == f.native_port(port) && !call.tags &&
            call.lock == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::lock)),
            "AddAppIconA gets its scalar arguments, the native port, the lock and NULL tags");
    require(copied_native(call.text, 11) && std::strcmp(call.text, "Guest icon") == 0, "the label is a native copy");
    DiskObject *d = call.icon;
    require(d && d != original && clones.count(d) && !originals.count(d) && !f.in_guest(d),
            "Workbench gets a native clone, neither the original nor guest memory");
    const IconObject &c = clones.at(d), &o = originals.at(original);
    const auto &g = d->do_Gadget, &og = original->do_Gadget;
    require(d->do_Magic == original->do_Magic && d->do_Version == original->do_Version &&
            d->do_Type == original->do_Type && d->do_StackSize == original->do_StackSize && !d->do_DrawerData,
            "the clone has the scalars of the original");
    require(g.LeftEdge == og.LeftEdge && g.TopEdge == og.TopEdge && g.Width == og.Width && g.Height == og.Height &&
            g.Flags == og.Flags && g.Activation == og.Activation && g.GadgetType == og.GadgetType &&
            g.MutualExclude == og.MutualExclude && g.GadgetID == og.GadgetID && g.UserData == og.UserData &&
            !g.NextGadget && !g.GadgetText && !g.SpecialInfo,
            "the gadget of the clone is the original's, also where the snapshot shows nothing");
    require(d->do_CurrentX == x && d->do_CurrentY == y, "the clone has the position of the snapshot");
    check_clone_image(c, o, g.GadgetRender, og.GadgetRender);
    check_clone_image(c, o, g.SelectRender, og.SelectRender);
    check_clone_text(c, d->do_DefaultTool, original->do_DefaultTool);
    check_clone_text(c, d->do_ToolWindow, original->do_ToolWindow);
    require(d->do_ToolTypes && d->do_ToolTypes != original->do_ToolTypes && part_of(c, d->do_ToolTypes) &&
            !d->do_ToolTypes[2], "the clone has a tool type array of its own with its NULL end");
    for (unsigned i = 0; i < 2; ++i) check_clone_text(c, d->do_ToolTypes[i], original->do_ToolTypes[i]);
    return d;
}
/* The clone after its original has gone */
static void check_clone_alone(DiskObject *d)
{
    const auto *render = static_cast<const Image *>(d->do_Gadget.GadgetRender);
    const auto *select = static_cast<const Image *>(d->do_Gadget.SelectRender);
    require(clones.count(d) && std::strcmp(d->do_DefaultTool, original_tool) == 0 &&
            std::strcmp(d->do_ToolWindow, original_window) == 0 &&
            std::strcmp(d->do_ToolTypes[0], original_type0) == 0 && std::strcmp(d->do_ToolTypes[1], original_type1) == 0 &&
            std::memcmp(render->ImageData, original_render, sizeof(original_render)) == 0 &&
            std::memcmp(select->ImageData, original_select, sizeof(original_select)) == 0,
            "the clone needs nothing of its original");
}
static void no_icon_calls(Fixture &f, const char *what)
{
    require(adds.empty() && icon_open_calls == 0 && dup_calls == 0 && clones.empty() && vector_count() == 0, what);
    for (const auto &entry : f.ctx->app_objects) require(!entry.guest, "no token");
}

static void test_snapshot_icon()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(); DiskObject *original = f.original_of(s);
    const auto bytes = f.snapshot_bytes(s);
    events.clear();
    ULONG token = f.add_snapshot(p, s); f.check_token(token);
    require(f.ctx->running && f.snapshot_bytes(s) == bytes, "AddAppIconA takes the snapshot as it is and leaves it so");
    DiskObject *clone = check_clone(f, p, original, 120, -40);
    APTR native = adds.back().result;
    require(event_count("OpenIcon") == 1 && event_count("DupDiskObject") == 1 && icon_opens == 1 && clones.size() == 1 &&
            event_at("OpenIcon") < event_at("DupDiskObject") && event_at("DupDiskObject") < event_at("AddAppIcon"),
            "a reference to icon.library of its own, then the clone, then Workbench");
    /* the program frees its snapshot, and its icon.library objects go */
    require(f.free_snapshot(s) == 0 && f.ctx->running && !originals.count(original) && event_count("FreeOriginal") == 1,
            "the program frees the snapshot through its literal LVO, which frees the original");
    Kaffeine_IconsCleanup(f.ctx);
    require(objects.count(native) && f.token_present(token) && icon_opens == 1,
            "the AppIcon outlives the original and the program's icon.library objects");
    check_clone_alone(clone);
    events.clear();
    require(f.call(Fixture::wb, 66, {token}) == TRUE && removes.back() == native && f.ctx->running,
            "RemoveAppIcon removes the native AppIcon");
    require(event_count("RemoveAppObject") == 1 && event_count("FreeClone") == 1 && event_count("CloseIcon") == 1 &&
            event_at("RemoveAppObject") < event_at("FreeClone") && event_at("FreeClone") < event_at("CloseIcon"),
            "the clone goes after Workbench lets the AppIcon go, through its base, which then closes");
    require(!f.token_present(token) && clones.empty() && icon_opens == 0 && vector_count() == 0,
            "the token, the clone, the reference and the label go");
    f.refused(f.call(Fixture::wb, 66, {token}));
    require(event_count("FreeClone") == 1 && event_count("CloseIcon") == 1 && removes.size() == 1,
            "a second removal of the token frees nothing again");
}
static void test_snapshot_position()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(); DiskObject *original = f.original_of(s);
    f.longword(s + DO_X, 0x80000000); f.longword(s + DO_Y, 0xFFFFFFFB);
    require(f.ctx->mem[s + DO_X] == 0x80 && f.ctx->mem[s + DO_X + 3] == 0 && f.ctx->mem[s + DO_Y] == 0xFF &&
            f.ctx->mem[s + DO_Y + 3] == 0xFB, "the positions are big-endian 32-bit fields");
    f.check_token(f.add_snapshot(p, s));
    check_clone(f, p, original, static_cast<LONG>(0x80000000), -5);
    require(original->do_CurrentX == 120 && original->do_CurrentY == -40, "the original keeps its own position");
}
static void test_snapshot_edit(const std::function<void(Fixture &, ULONG)> &edit)
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(); edit(f, s);
    const ULONG available = Kaffeine_HeapAvail(f.ctx, 0);
    f.refused(f.add_snapshot(p, s));
    no_icon_calls(f, "an unsupported edit stops the program before a label, a reference, a clone or Workbench");
    require(Kaffeine_HeapAvail(f.ctx, 0) == available && originals.size() == 1, "and before a token; the original stays the program's");
}
static void test_snapshot_bad_label()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot();
    f.refused(f.add_snapshot(p, s, Fixture::bad_text));
    no_icon_calls(f, "a label without NUL in 1025 bytes stops the program before any native call");
}
/* An address that is no live snapshot is a DiskObject of the program's
   own, however alike its bytes: without images, AddAppIconA() gives NULL */
enum Foreign { INTERIOR, COPIED, FREED };
static void test_snapshot_not_promoted(Foreign how)
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(), dobj = s;
    if (how == INTERIOR) dobj = s + 4;
    else if (how == COPIED) { std::memcpy(f.ctx->mem + Fixture::disk, f.ctx->mem + s, DO_SIZE); dobj = Fixture::disk; }
    else require(f.free_snapshot(s) == 0 && originals.empty(), "the program frees its snapshot");
    ULONG result = f.add_snapshot(p, dobj);
    if (how == FREED) require(result == 0 || result == 20, "a freed snapshot is taken as any guest DiskObject");
    else require(result == 0 && f.ctx->running && f.ctx->exit_code == 0,
                 "a DiskObject without images that is no live snapshot gives NULL");
    no_icon_calls(f, "it is never promoted to a native original");
}
/* A snapshot that the program gave images of its own is one that it
   built: copied from guest memory, never cloned */
static void test_snapshot_given_images()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot();
    f.image_at(Fixture::image, Fixture::data, 2, 3, 8);
    f.image_at(Fixture::select, Fixture::select_data, 1, 1, 1, true);
    f.longword(s + DO_GADGET + GG_RENDER, Fixture::image); f.longword(s + DO_GADGET + GG_SELECT, Fixture::select);
    f.check_token(f.add_snapshot(p, s));
    const DiskObject *d = adds.back().icon;
    require(icon_open_calls == 0 && dup_calls == 0 && clones.empty() && copied_native(d, sizeof(*d)),
            "the DiskObject is a copy of the guest one");
    check_image(static_cast<Image *>(d->do_Gadget.GadgetRender), {2, 3, 2});
    check_image(static_cast<Image *>(d->do_Gadget.SelectRender), {1, 1, 1}, true);
    require(std::strcmp(d->do_DefaultTool, original_tool) == 0 && std::strcmp(d->do_ToolTypes[1], original_type1) == 0 &&
            d->do_CurrentX == 120 && d->do_CurrentY == -40, "the copy has the guest fields of the snapshot");
}
enum IconFailure { NO_BASE, NO_CLONE, NO_APPICON, NO_ENTRY, NO_GUEST_MEMORY };
static void test_snapshot_failure(IconFailure failure)
{
    Fixture f; ULONG p = f.port(), s = f.snapshot();
    std::vector<ULONG> menus, drained;
    if (failure == NO_ENTRY)
        while (menus.size() < KAFFEINE_MAX_APP_OBJECTS)
        {
            menus.push_back(f.add_menu(p));
            require(menus.back() != 0, "every App object entry is taken");
        }
    if (failure == NO_GUEST_MEMORY)
        for (ULONG size : {256u, 4u})
            for (ULONG block; (block = Kaffeine_HeapAlloc(f.ctx, size, MEMF_PUBLIC)); ) drained.push_back(block);
    fail_open = failure == NO_BASE; fail_dup = failure == NO_CLONE; fail_add = failure == NO_APPICON;
    const ULONG available = Kaffeine_HeapAvail(f.ctx, 0);
    const size_t vectors = vector_count(), added = adds.size();
    events.clear();
    require(f.add_snapshot(p, s) == 0 && f.ctx->running && f.ctx->exit_code == 0,
            "a native or table failure gives NULL without stopping the program");
    std::vector<std::string> expected;
    if (failure == NO_BASE) expected = {"OpenIcon"};
    if (failure == NO_CLONE) expected = {"OpenIcon", "DupDiskObject", "CloseIcon"};
    if (failure == NO_APPICON) expected = {"OpenIcon", "DupDiskObject", "AddAppIcon", "FreeClone", "CloseIcon"};
    std::vector<std::string> native_events;
    for (const auto &event : events)
        if (event != "FreeMem") native_events.push_back(event);
    require(native_events == expected, "what was taken goes back in the opposite order, and nothing after the failure is taken");
    require(clones.empty() && icon_opens == 0 && vector_count() == vectors && Kaffeine_HeapAvail(f.ctx, 0) == available &&
            adds.size() == added + (failure == NO_APPICON) && objects.size() == menus.size(),
            "no clone, reference, label copy, token or native AppIcon is left");
    size_t tokens = 0;
    for (const auto &entry : f.ctx->app_objects) tokens += entry.guest != 0;
    require(tokens == menus.size() && originals.size() == 1, "only the earlier tokens and the original remain");
    for (ULONG block : drained) Kaffeine_HeapFree(f.ctx, block, 0);
}
static void test_snapshot_remove_refused()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(), token = f.add_snapshot(p, s);
    APTR native = adds.back().result; DiskObject *clone = adds.back().icon; const char *label = adds.back().text;
    remove_result = FALSE; events.clear();
    require(f.call(Fixture::wb, 66, {token}) == FALSE && f.ctx->running && f.ctx->exit_code == 0,
            "the FALSE of Workbench reaches the program, which goes on");
    require(f.token_present(token) && objects.count(native) && clones.count(clone) && icon_opens == 1 &&
            copied_native(label, 11) && !event_count("FreeClone") && !event_count("CloseIcon"),
            "a refused AppIcon keeps its token, clone, reference and label");
    require(f.free_snapshot(s) == 0 && originals.empty() && clones.count(clone), "the original may go meanwhile");
    remove_result = TRUE; events.clear();
    require(f.call(Fixture::wb, 66, {token}) == TRUE && std::count(removes.begin(), removes.end(), native) == 2,
            "another RemoveAppIcon asks Workbench again");
    require(event_at("RemoveAppObject") < event_at("FreeClone") && event_at("FreeClone") < event_at("CloseIcon") &&
            event_count("FreeClone") == 1 && !f.token_present(token) && clones.empty() && icon_opens == 0 &&
            vector_count() == 0, "everything goes once, after Workbench lets the AppIcon go");
}
/* Workbench refuses to let the AppIcon go up to the end of the program */
static void test_snapshot_refused_at_end()
{
    Fixture f; ULONG p = f.port(), s = f.snapshot(), token = f.add_snapshot(p, s);
    APTR native = adds.back().result; DiskObject *clone = adds.back().icon; const char *label = adds.back().text;
    MsgPort *front = adds.back().port;
    const ULONG port_bytes = blocks.at(front).size;
    remove_result = FALSE; events.clear();
    Kaffeine_IOCleanup(f.ctx);
    require(std::count(removes.begin(), removes.end(), native) == 1 && !f.token_present(token) && !f.native_port(p),
            "cleanup asks Workbench once and forgets the token");
    require(objects.count(native) && clones.count(clone) && icon_opens == 1 && copied_native(label, 11) &&
            !event_count("FreeClone") && !event_count("CloseIcon"), "what Workbench keeps is never freed under it");
    require(blocks.count(front) && Fixture::inbox(front)->mp_Flags == PA_IGNORE &&
            !Fixture::inbox(front)->mp_SigTask && signals == 0xFFFF,
            "Workbench's retained port stays valid and has no departing task or freed signal");
    const size_t native_blocks = blocks.size();
    Delivery *late = f.delivery(front);
    require(f.replied(late) && queues[front].empty() && queues[Fixture::inbox(front)].empty() &&
            blocks.size() == native_blocks && dups.empty(),
            "a late delivery to the detached port is replied without a carrier, lock or task signal");
    Kaffeine_IOCleanup(f.ctx);
    require(std::count(removes.begin(), removes.end(), native) == 1 && clones.count(clone), "a repeated cleanup leaves it alone");
    /* Workbench lets the AppIcon go later; the fixture releases what the bridge left to it */
    objects.erase(native);
    Test_FreeDiskObject_WB(&icon_library, clone);
    CloseLibrary(&icon_library);
    FreeVec(const_cast<char *>(label));
    FreeMem(front, port_bytes);
    remove_result = TRUE;
}
enum PortEnd { DELETE_PORT, FREE_SIGNAL, END_OF_PROGRAM };
static void test_snapshot_port_end(PortEnd end)
{
    Fixture f; ULONG p = f.port(), other = f.port(), s1 = f.snapshot(), s2 = f.snapshot();
    ULONG t1 = f.add_snapshot(p, s1); APTR n1 = adds.back().result; DiskObject *c1 = adds.back().icon;
    ULONG t2 = f.add_snapshot(other, s2); APTR n2 = adds.back().result; DiskObject *c2 = adds.back().icon;
    require(c1 != c2 && clones.size() == 2 && icon_opens == 2, "each AppIcon has a clone and a reference of its own");
    require(f.free_snapshot(s1) == 0 && f.free_snapshot(s2) == 0 && originals.empty(), "the program frees both snapshots first");
    UBYTE bit = f.ctx->mem[p + MP_SIGBIT]; events.clear();
    if (end == DELETE_PORT) require(f.call(Fixture::exec, 672, {p}) == 0, "DeleteMsgPort returns zero");
    else if (end == FREE_SIGNAL) require(f.call(Fixture::exec, 336, {}, bit) == 0, "FreeSignal returns zero");
    else Kaffeine_IOCleanup(f.ctx);
    require(!clones.count(c1) && !objects.count(n1) && std::count(removes.begin(), removes.end(), n1) == 1 &&
            !f.token_present(t1), "the AppIcon of the port goes, and its clone");
    require(event_at("RemoveAppObject") < event_at("FreeClone") && event_at("FreeClone") < event_at("CloseIcon") &&
            event_at("CloseIcon") < event_at("FreeAppPort"),
            "Workbench lets the AppIcon go, then its clone and its reference go, then the native port");
    if (end != END_OF_PROGRAM)
        require(clones.count(c2) && objects.count(n2) && f.token_present(t2) && icon_opens == 1 &&
                event_count("FreeClone") == 1, "another port keeps its AppIcon, clone and reference");
    Kaffeine_IOCleanup(f.ctx); Kaffeine_IOCleanup(f.ctx);
    require(clones.empty() && icon_opens == 0 && event_count("FreeClone") == 2 && event_count("CloseIcon") == 2 &&
            std::count(removes.begin(), removes.end(), n2) == 1 && !f.token_present(t2),
            "every clone goes once through its base, and every reference closes once");
}
static void test_menu()
{
    Fixture f; ULONG p = f.port(), token = f.add_menu(p); f.check_token(token);
    const auto call = adds.back();
    require(call.kind == MENU && call.id == Fixture::id && call.userdata == Fixture::userdata &&
            call.port == f.native_port(p) && !call.tags && copied_native(call.text, 11) && std::strcmp(call.text, "Guest icon") == 0,
            "AddAppMenuItemA gets its arguments and a copied label");
    f.ctx->mem[Fixture::text] = 'x'; require(call.text[0] == 'G', "menu label survives a guest edit");
    require(f.call(Fixture::wb, 78, {token}) == TRUE && removes.back() == call.result && !f.token_present(token) && vector_count() == 0,
            "RemoveAppMenuItem frees its native text and token");
}
static void test_menu_bad_text()
{
    Fixture f; ULONG p = f.port();
    f.refused(f.add_menu(p, Fixture::bad_text));
    require(adds.empty() && vector_count() == 0, "a menu label without NUL in 1025 bytes never reaches Workbench");
}
static void test_menu_text_limit()
{
    Fixture f; ULONG p = f.port(); f.ctx->mem[Fixture::bad_text + 1024] = 0;
    f.check_token(f.add_menu(p, Fixture::bad_text));
    require(std::strlen(adds.back().text) == 1024 && copied_native(adds.back().text, 1025), "a 1024-character menu label is accepted and copied");
}

static void check_message(Fixture &f, ULONG msg, const Delivery &d)
{
    const auto &am = d.am;
    require(f.valid(msg, AM_SIZE) && f.copy_present(msg), "AppMessage is a tracked guest copy");
    require(f.ctx->mem[msg + LN_TYPE] == NT_MESSAGE && f.read_word(msg + MN_LENGTH) == 86,
            "copy has NT_MESSAGE and the 86-byte NDK length");
    require(f.read_word(msg + AM_TYPE) == am.am_Type && f.read(msg + AM_USERDATA) == am.am_UserData &&
            f.read(msg + AM_ID) == am.am_ID && f.read(msg + AM_NUMARGS) == 3 &&
            f.read_word(msg + AM_VERSION_OFF) == am.am_Version && f.read_word(msg + AM_CLASS) == am.am_Class &&
            static_cast<WORD>(f.read_word(msg + AM_MOUSEX)) == am.am_MouseX &&
            static_cast<WORD>(f.read_word(msg + AM_MOUSEY)) == am.am_MouseY &&
            f.read(msg + AM_SECONDS) == am.am_Seconds && f.read(msg + AM_MICROS) == am.am_Micros,
            "every AppMessage field uses its NDK offset and guest byte order");
    require(f.ctx->mem[msg + AM_USERDATA] == 0xFE && f.ctx->mem[msg + AM_USERDATA + 3] == 0x98,
            "guest fields are physically big-endian");
    ULONG args = f.read(msg + AM_ARGLIST);
    require(f.valid(args, 3 * ARG_SIZE), "WBArg array is in guest memory");
    for (unsigned i = 0; i < 3; ++i)
    {
        BPTR lock = reinterpret_cast<BPTR>(static_cast<IPTR>(f.read(args + i * ARG_SIZE + ARG_LOCK)));
        require(dups.count(lock) && dups.at(lock) == d.args[i].wa_Lock,
                "WBArg lock passes as a native lock value, a duplicate of the lock of Workbench");
        ULONG name = f.read(args + i * ARG_SIZE + ARG_NAME);
        if (i == 2) require(!name, "NULL WBArg name remains NULL");
        else
        {
            const char *expected = reinterpret_cast<const char *>(d.args[i].wa_Name);
            require(f.valid(name, std::strlen(expected) + 1) &&
                    std::strcmp(reinterpret_cast<const char *>(f.ctx->mem + name), expected) == 0,
                    "WBArg name is copied into guest memory");
        }
    }
    for (unsigned i = 54; i < 86; ++i) require(f.ctx->mem[msg + i] == 0, "reserved AppMessage bytes are cleared");
}
static void test_get_message()
{
    Fixture f; ULONG p = f.port(); f.add_window(p); MsgPort *front = f.native_port(p);
    Delivery *d = f.delivery(front);
    require(f.replied(d) && queues[front].empty() && queues[Fixture::inbox(front)].size() == 1 && dups.size() == 3,
            "PutMsg replies the AppMessage at once and leaves a carrier with duplicated locks in the inbox");
    ULONG available = Kaffeine_HeapAvail(f.ctx, 0), msg = f.get(p);
    check_message(f, msg, *d); f.empty(p);
    require(wait_calls == 0 && get_calls == 1 && queues[Fixture::inbox(front)].empty(), "GetMsg takes the carrier without waiting");
    d->first[0] = 'x';
    require(f.ctx->mem[f.read(f.read(msg + AM_ARGLIST) + ARG_NAME)] == 'F', "guest WBArg name is independent of native name storage");
    const size_t vectors = vector_count();
    f.reply(msg);
    require(replies.size() == 1 && !f.copy_present(msg) && dups.empty() && vector_count() == vectors - 1 &&
            Kaffeine_HeapAvail(f.ctx, 0) == available, "ReplyMsg frees the guest copy and the carrier and unlocks its duplicates");
    f.reply(msg);
    require(replies.size() == 1 && f.ctx->running, "a second ReplyMsg changes nothing");
}
static void test_wait_message()
{
    Fixture f; ULONG p = f.port(); f.add_window(p); MsgPort *native = f.native_port(p);
    Delivery *d = f.delivery(native, false); on_wait = &d->am.am_Message; on_wait_port = native;
    ULONG msg = f.wait(p); check_message(f, msg, *d);
    require(waited_empty && wait_calls == 1 && get_calls == 1 && !on_wait && f.read(p + MP_LIST) == msg && f.replied(d),
            "WaitPort waits natively on an empty inbox, Workbench has its reply at once, and the copy stays on the guest list");
    require(f.read(msg + LN_PRED) == p + MP_LIST && f.read(msg + LN_SUCC) == p + MP_LIST + 4 &&
            f.read(p + MP_LIST + 8) == msg, "WaitPort links the copy with both guest sentinels");
    require(f.wait(p) == msg && wait_calls == 1 && get_calls == 1, "another WaitPort returns the same listed copy without native calls");
    require(f.get(p) == msg && get_calls == 1, "GetMsg takes the existing WaitPort copy without touching the native queue");
    f.empty(p); f.reply(msg);
}
static void test_guest_message_first()
{
    Fixture f; ULONG p = f.port(); f.add_window(p); MsgPort *native = f.native_port(p);
    Delivery *d = f.delivery(native);
    ULONG list = p + MP_LIST, own = Fixture::raw_message;
    f.longword(list, own); f.longword(list + 8, own);
    f.longword(own + LN_SUCC, list + 4); f.longword(own + LN_PRED, list);
    f.ctx->mem[own + LN_TYPE] = NT_MESSAGE; f.word(own + MN_LENGTH, 20);
    require(f.wait(p) == own && f.get(p) == own && get_calls == 0 && wait_calls == 0 &&
            queues[Fixture::inbox(native)].size() == 1 && f.replied(d), "an existing 68k message precedes the carriers and leaves the inbox untouched");
    f.empty(p);
    ULONG msg = f.get(p); check_message(f, msg, *d); f.reply(msg);
}
static void test_reply_listed_copy()
{
    Fixture f; ULONG p = f.port(); f.add_window(p);
    Delivery *d = f.delivery(f.native_port(p)); ULONG msg = f.wait(p);
    f.reply(msg); f.empty(p);
    require(!f.copy_present(msg) && replies.size() == 1 && replies[0] == &d->am.am_Message && dups.empty(),
            "replying a WaitPort copy unlinks it before freeing it and its carrier");
    require(f.get(p) == 0 && f.ctx->running, "GetMsg sees no stale copy after a listed reply");
    f.reply(msg); require(replies.size() == 1, "listed copy cannot be replied twice");
}
static void test_copy_limit(bool waiting)
{
    Fixture f; ULONG p = f.port(); f.add_window(p); MsgPort *native = f.native_port(p);
    std::array<ULONG, 32> copies{};
    for (auto &copy : copies) { f.delivery(native); copy = f.get(p); require(copy != 0, "one of all 32 copy entries is available"); }
    require(std::count_if(std::begin(f.ctx->app_messages), std::end(f.ctx->app_messages),
                          [](const auto &entry) { return entry.guest != 0; }) == 32, "all 32 independent copy entries are occupied");
    Delivery *extra = f.delivery(native); ULONG available = Kaffeine_HeapAvail(f.ctx, 0);
    const size_t vectors = vector_count();
    ULONG result = waiting ? f.wait(p) : f.get(p);
    if (waiting) f.refused(result);
    else require(result == 0 && f.ctx->running && f.ctx->exit_code == 0, "GetMsg returns NULL when a copy cannot be registered");
    require(f.replied(extra) && queues[Fixture::inbox(native)].empty() && vector_count() == vectors - 1 &&
            dups.size() == 32 * 3 && Kaffeine_HeapAvail(f.ctx, 0) == available,
            "the overflow carrier goes with its locks, and no guest allocation leaks");
    f.empty(p);
    require(wait_calls == (waiting ? 1u : 0u), "overflow WaitPort makes one native wait and cannot loop forever");
    for (ULONG copy : copies) f.reply(copy);
    require(replies.size() == 33 && dups.empty(), "all original copies can still be replied after overflow");
}

struct PortBundle
{
    ULONG guest, taken_copy, listed_copy;
    MsgPort *native;
    std::array<ULONG, 3> tokens;
    std::array<APTR, 3> native_objects;
    std::array<Delivery *, 4> messages;
};
static PortBundle bundle(Fixture &f)
{
    PortBundle b{}; b.guest = f.port(); f.diskobject();
    b.tokens[0] = f.add_window(b.guest); b.native_objects[0] = adds.back().result;
    b.tokens[1] = f.add_icon(b.guest); b.native_objects[1] = adds.back().result;
    b.tokens[2] = f.add_menu(b.guest); b.native_objects[2] = adds.back().result;
    for (ULONG token : b.tokens) f.check_token(token);
    b.native = f.native_port(b.guest);
    b.messages[0] = f.delivery(b.native); b.taken_copy = f.get(b.guest);
    b.messages[1] = f.delivery(b.native); b.listed_copy = f.wait(b.guest);
    b.messages[2] = f.delivery(b.native); b.messages[3] = f.delivery(b.native);
    return b;
}
static void gone(Fixture &f, const PortBundle &b)
{
    require(!f.native_port(b.guest) && !blocks.count(b.native), "cleanup forgets and frees the native backing port");
    for (ULONG token : b.tokens) require(!f.token_present(token), "cleanup forgets each App token");
    for (APTR object : b.native_objects)
        require(!objects.count(object) && std::count(removes.begin(), removes.end(), object) == 1,
                "cleanup removes each native App kind exactly once");
    require(!f.copy_present(b.taken_copy) && !f.copy_present(b.listed_copy), "cleanup frees taken and listed copies");
    for (Delivery *d : b.messages) require(f.replied(d), "Workbench had the reply of every AppMessage at once");
    require(queues[b.native].empty() && queues[Fixture::inbox(b.native)].empty(), "cleanup takes the carriers from the inbox");
}
static void intact(Fixture &f, const PortBundle &b)
{
    require(f.native_port(b.guest) == b.native && blocks.count(b.native), "another native port remains allocated");
    for (ULONG token : b.tokens) require(f.token_present(token), "another port keeps its tokens");
    for (APTR object : b.native_objects) require(objects.count(object), "another port keeps its native objects");
    require(f.copy_present(b.taken_copy) && f.copy_present(b.listed_copy) &&
            f.read(b.guest + MP_LIST) == b.listed_copy && queues[Fixture::inbox(b.native)].size() == 2 &&
            dups.size() == 4 * 3, "another port keeps both copies, its guest list, its carriers and their locks");
}
static void cleanup_order()
{
    auto removed = std::find(events.begin(), events.end(), "RemoveAppObject");
    auto unlocked = std::find(events.begin(), events.end(), "UnLock");
    auto freed_port = std::find(events.begin(), events.end(), "FreeAppPort");
    auto freed_signal = std::find(events.begin(), events.end(), "FreeSignal");
    require(std::count(events.begin(), unlocked, "RemoveAppObject") >= 3 && removed < unlocked &&
            unlocked < freed_port && freed_port < freed_signal,
            "native objects go before the carriers, then the native ports and the signal");
}
static void test_delete_port()
{
    Fixture f; PortBundle first = bundle(f), other = bundle(f); events.clear();
    UBYTE bit = f.ctx->mem[first.guest + MP_SIGBIT];
    require(f.call(Fixture::exec, 672, {first.guest}) == 0, "DeleteMsgPort returns zero");
    gone(f, first); intact(f, other); cleanup_order();
    require(!(f.ctx->guest_signals & (1u << bit)) && !(signals & (1u << bit)), "DeleteMsgPort frees the owned signal");
}
static void test_free_signal()
{
    Fixture f; PortBundle first = bundle(f), other = bundle(f); events.clear();
    UBYTE bit = f.ctx->mem[first.guest + MP_SIGBIT];
    require(f.call(Fixture::exec, 336, {}, bit) == 0, "FreeSignal returns zero");
    gone(f, first); intact(f, other); cleanup_order(); f.empty(first.guest);
    require(!(f.ctx->guest_signals & (1u << bit)) && !(signals & (1u << bit)), "FreeSignal releases the signal after App cleanup");
    f.call(Fixture::exec, 672, {first.guest});
    require(f.ctx->running, "deleting the guest port afterwards does not free the signal twice");
}
static void test_io_cleanup()
{
    Fixture f; PortBundle first = bundle(f), other = bundle(f); events.clear();
    Kaffeine_IOCleanup(f.ctx);
    gone(f, first); gone(f, other); cleanup_order();
    require(objects.empty() && vector_count() == 0 && dups.empty() && !f.ctx->guest_signals && signals == 0xFFFF,
            "IOCleanup handles every port, App object, copy, carrier and signal");
    const size_t removed = removes.size(), replied = replies.size();
    Kaffeine_IOCleanup(f.ctx);
    require(removes.size() == removed && replies.size() == replied, "IOCleanup is harmless when repeated");
}
/* The program removes its AppWindow with an AppMessage that it has not
   taken, and another comes while the removal waits for the lock of
   Workbench: both had their reply in PutMsg, and the program takes both. */
static void test_remove_while_sender_waits()
{
    Fixture f; ULONG p = f.port(); ULONG token = f.add_window(p); MsgPort *front = f.native_port(p);
    Delivery *first = f.delivery(front), *late = f.delivery(front, false);
    during_remove = [&] { PutMsg(front, &late->am.am_Message); };
    require(f.call(Fixture::wb, 54, {token}) == TRUE && f.ctx->running && !during_remove,
            "RemoveAppWindow returns while AppMessages come");
    require(f.replied(first) && f.replied(late), "both AppMessages had their reply before the removal went on");
    ULONG a = f.get(p), b = f.get(p);
    check_message(f, a, *first); check_message(f, b, *late);
    f.reply(a); f.reply(b);
    require(dups.empty() && f.get(p) == 0, "the program takes both after the removal, with their duplicated locks");
}
/* DeleteMsgPort() of a port with an AppMessage that the program took and
   another in the inbox, while one more comes during the removal */
static void test_delete_port_while_sender_waits()
{
    Fixture f; ULONG p = f.port(); f.add_window(p); MsgPort *front = f.native_port(p);
    Delivery *taken = f.delivery(front), *pending = f.delivery(front), *late = f.delivery(front, false);
    ULONG copy = f.get(p);
    during_remove = [&] { PutMsg(front, &late->am.am_Message); };
    require(f.call(Fixture::exec, 672, {p}) == 0 && f.ctx->running && !during_remove,
            "DeleteMsgPort returns while an AppMessage comes");
    require(f.replied(taken) && f.replied(pending) && f.replied(late) && !f.copy_present(copy) &&
            dups.empty() && vector_count() == 0 && !blocks.count(front),
            "cleanup frees the copy and every carrier, also one that came during the removal");
}
/* A sender that is no process cannot duplicate a lock: its AppMessage has
   its reply all the same, and its locks reach the program as none. */
static void test_sender_not_a_process()
{
    Fixture f; ULONG p = f.port(); f.add_window(p);
    owner.tc_Node.ln_Type = NT_TASK;
    Delivery *d = f.delivery(f.native_port(p));
    owner.tc_Node.ln_Type = NT_PROCESS;
    ULONG msg = f.get(p), args = f.read(msg + AM_ARGLIST);
    require(f.replied(d) && dups.empty() && f.read(msg + AM_NUMARGS) == 3, "the AppMessage has its reply, without duplicates");
    for (unsigned i = 0; i < 3; ++i) require(f.read(args + i * ARG_SIZE + ARG_LOCK) == 0, "its locks reach the program as none");
    f.reply(msg);
}
static void test_wbinfo(bool use_screen)
{
    Fixture f;
    require(f.call(Fixture::wb, 90, {Fixture::lock, Fixture::text, use_screen ? Fixture::screen_addr : 0}) == TRUE &&
            info_calls == 1 && info_lock == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::lock)) &&
            info_name == reinterpret_cast<const char *>(f.ctx->mem + Fixture::text) &&
            info_screen == (use_screen ? &f.screen : nullptr), "WBInfo passes its lock, guest name and native screen");
    info_result = FALSE;
    require(f.call(Fixture::wb, 90, {Fixture::lock, Fixture::text, 0}) == FALSE && f.ctx->running, "WBInfo returns native FALSE without stopping");
}
static void test_wbinfo_bad(bool bad_screen)
{
    Fixture f;
    f.refused(f.call(Fixture::wb, 90, {Fixture::lock, bad_screen ? Fixture::text : Fixture::bad_text,
                                     bad_screen ? 0x1234u : 0u}));
    require(info_calls == 0, "invalid WBInfo arguments never reach native Workbench");
}

int main(int argc, char **argv)
{
    using Test = std::pair<const char *, std::function<void()>>;
    const Test tests[] = {
        {"window_add_remove", test_window_add_remove},
        {"window_remove_false", test_window_remove_false},
        {"shared_native_port", test_shared_port},
        {"unknown_window", test_unknown_window},
        {"window_tags", [] { test_tags(WINDOW); }},
        {"icon_tags", [] { test_tags(ICON); }},
        {"menu_tags", [] { test_tags(MENU); }},
        {"port_not_signalling", [] { test_bad_port(false); }},
        {"port_signal_not_owned", [] { test_bad_port(true); }},
        {"remove_window_wrong_kind", [] { test_wrong_kind(WINDOW); }},
        {"remove_icon_wrong_kind", [] { test_wrong_kind(ICON); }},
        {"remove_menu_wrong_kind", [] { test_wrong_kind(MENU); }},
        {"remove_window_unknown_token", [] { test_unknown_token(54); }},
        {"remove_icon_unknown_token", [] { test_unknown_token(66); }},
        {"remove_menu_unknown_token", [] { test_unknown_token(78); }},
        {"remove_null_icon", test_null_icon},
        {"icon_two_planes_and_select", [] { test_icon({2, 3, 2}); }},
        {"icon_sparse_plane", [] { test_icon({2, 1, 1}, NO_SELECT); }},
        {"icon_plane_above_depth", [] { test_icon({1, 2, 1}, NO_SELECT); }},
        {"icon_picks_beyond_depth", [] { test_icon({1, 3, 1}, NO_SELECT); }},
        {"icon_no_plane_picked", [] { test_icon({2, 0, 0}, NO_SELECT); }},
        {"icon_select_requires_flag", [] { test_icon({2, 3, 2}, NO_SELECT); }},
        {"icon_built_for_appicon", [] { test_icon({2, 3, 2}, NO_SELECT, true); }},
        {"icon_built_with_select", [] { test_icon({2, 3, 2}, SELECT_IMAGE, true); }},
        {"icon_no_highlight_with_select", [] { test_icon({2, 3, 2}, NONE_WITH_SELECT, true); }},
        {"icon_other_magic", [] { test_icon({2, 3, 2}, SELECT_IMAGE, true, [](Fixture &f) {
             f.word(Fixture::disk + DO_MAGIC, 0x1234); f.word(Fixture::disk + DO_VERSION, 7); }); }},
        {"icon_select_left_out", [] { test_failing_icon([](Fixture &f) { f.longword(Fixture::disk + DO_GADGET + GG_SELECT, 0); }); }},
        {"icon_no_highlight_without_select", [] { test_failing_icon([](Fixture &f) {
             f.word(Fixture::disk + DO_GADGET + GG_FLAGS, GFLG_GADGHNONE); f.longword(Fixture::disk + DO_GADGET + GG_SELECT, 0); }); }},
        {"icon_without_render", [] { test_failing_icon([](Fixture &f) { f.longword(Fixture::disk + DO_GADGET + GG_RENDER, 0); }); }},
        {"icon_zero_width", [] { test_failing_icon([](Fixture &f) { f.word(Fixture::image + IM_WIDTH, 0); }); }},
        {"icon_zero_height", [] { test_failing_icon([](Fixture &f) { f.word(Fixture::image + IM_HEIGHT, 0); }); }},
        {"icon_zero_depth", [] { test_failing_icon([](Fixture &f) { f.word(Fixture::image + IM_DEPTH, 0); }); }},
        {"icon_nine_planes", [] { test_failing_icon([](Fixture &f) { f.word(Fixture::image + IM_DEPTH, 9); }); }},
        {"icon_without_image_data", [] { test_failing_icon([](Fixture &f) { f.longword(Fixture::image + IM_DATA, 0); }); }},
        {"icon_select_zero_width", [] { test_failing_icon([](Fixture &f) { f.word(Fixture::select + IM_WIDTH, 0); }); }},
        {"icon_render_outside_memory", [] { test_bad_icon([](Fixture &f) { f.longword(Fixture::disk + DO_GADGET + GG_RENDER, f.ctx->mem_size - 4); }); }},
        {"icon_excessive_width", [] { test_bad_icon([](Fixture &f) { f.word(Fixture::image + IM_WIDTH, 1025); }); }},
        {"icon_data_outside_memory", [] { test_bad_icon([](Fixture &f) { f.longword(Fixture::image + IM_DATA, f.ctx->mem_size - 2); }); }},
        {"icon_select_data_outside_memory", [] { test_bad_icon([](Fixture &f) { f.longword(Fixture::select + IM_DATA, f.ctx->mem_size - 1); }); }},
        {"icon_unterminated_text", [] { test_bad_icon([](Fixture &f) { std::memset(f.ctx->mem + Fixture::text, 'X', 1025); }); }},
        {"icon_unterminated_tool_type", [] { test_bad_icon([](Fixture &f) { f.longword(Fixture::types, Fixture::bad_text); }); }},
        {"icon_unterminated_default_tool", [] { test_bad_icon([](Fixture &f) { f.longword(Fixture::disk + DO_TOOL, Fixture::bad_text); }); }},
        {"icon_native_failure", test_icon_native_failure},
        {"icon_remove_refused", test_icon_remove_refused},
        {"snapshot_icon_clone", test_snapshot_icon},
        {"snapshot_icon_position", test_snapshot_position},
        {"snapshot_icon_bad_label", test_snapshot_bad_label},
        {"snapshot_edit_magic", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.word(s + DO_MAGIC, 0); }); }},
        {"snapshot_edit_version", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.word(s + DO_VERSION, 2); }); }},
        {"snapshot_edit_next_gadget", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_GADGET + GG_NEXT, s); }); }},
        {"snapshot_edit_width", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.word(s + DO_GADGET + GG_WIDTH, 17); }); }},
        {"snapshot_edit_flags", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.word(s + DO_GADGET + GG_FLAGS, GFLG_GADGIMAGE); }); }},
        {"snapshot_edit_select_render", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.image_at(Fixture::select, Fixture::select_data, 1, 1, 1, true);
             f.longword(s + DO_GADGET + GG_SELECT, Fixture::select); }); }},
        {"snapshot_edit_gadget_text", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_GADGET + GG_TEXT, Fixture::text); }); }},
        {"snapshot_edit_mutual_exclude", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_GADGET + GG_MUTEX, 0); }); }},
        {"snapshot_edit_special_info", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_GADGET + GG_SPECIAL, Fixture::raw_message); }); }},
        {"snapshot_edit_gadget_id", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.word(s + DO_GADGET + GG_ID, 1); }); }},
        {"snapshot_edit_user_data", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_GADGET + GG_USERDATA, 1); }); }},
        {"snapshot_edit_type", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.ctx->mem[s + DO_TYPE] = 3; }); }},
        {"snapshot_edit_stack", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.longword(s + DO_STACK, 4096); }); }},
        {"snapshot_edit_drawer_data", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.longword(s + DO_DRAWER, Fixture::raw_message); }); }},
        {"snapshot_edit_default_tool_moved", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.write_text(Fixture::tool, original_tool); f.longword(s + DO_TOOL, Fixture::tool); }); }},
        {"snapshot_edit_default_tool_text", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.ctx->mem[f.read(s + DO_TOOL)] = 'X'; }); }},
        {"snapshot_edit_default_tool_cleared", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.longword(s + DO_TOOL, 0); }); }},
        {"snapshot_edit_tool_window_cleared", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.longword(s + DO_WINDOW, 0); }); }},
        {"snapshot_edit_tool_types_cleared", [] { test_snapshot_edit([](Fixture &f, ULONG s) { f.longword(s + DO_TYPES, 0); }); }},
        {"snapshot_edit_tool_type_text", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.ctx->mem[f.read(f.read(s + DO_TYPES))] = 'X'; }); }},
        {"snapshot_edit_tool_type_moved", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             f.write_text(Fixture::type1, original_type0); f.longword(f.read(s + DO_TYPES), Fixture::type1); }); }},
        {"snapshot_edit_tool_types_unterminated", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             ULONG array = f.read(s + DO_TYPES); f.longword(array + 8, f.read(array)); }); }},
        {"snapshot_edit_tool_type_array_moved", [] { test_snapshot_edit([](Fixture &f, ULONG s) {
             ULONG array = f.read(s + DO_TYPES);
             for (ULONG i = 0; i < 3; ++i) f.longword(Fixture::types + 4 * i, f.read(array + 4 * i));
             f.longword(s + DO_TYPES, Fixture::types); }); }},
        {"snapshot_interior_not_promoted", [] { test_snapshot_not_promoted(INTERIOR); }},
        {"snapshot_copy_not_promoted", [] { test_snapshot_not_promoted(COPIED); }},
        {"snapshot_freed_not_promoted", [] { test_snapshot_not_promoted(FREED); }},
        {"snapshot_given_images_copied", test_snapshot_given_images},
        {"snapshot_no_icon_library", [] { test_snapshot_failure(NO_BASE); }},
        {"snapshot_no_clone", [] { test_snapshot_failure(NO_CLONE); }},
        {"snapshot_native_add_failure", [] { test_snapshot_failure(NO_APPICON); }},
        {"snapshot_no_object_entry", [] { test_snapshot_failure(NO_ENTRY); }},
        {"snapshot_no_guest_token", [] { test_snapshot_failure(NO_GUEST_MEMORY); }},
        {"snapshot_remove_refused", test_snapshot_remove_refused},
        {"snapshot_refused_at_end", test_snapshot_refused_at_end},
        {"snapshot_delete_port", [] { test_snapshot_port_end(DELETE_PORT); }},
        {"snapshot_free_signal", [] { test_snapshot_port_end(FREE_SIGNAL); }},
        {"snapshot_end_of_program", [] { test_snapshot_port_end(END_OF_PROGRAM); }},
        {"menu_copied_text", test_menu},
        {"menu_unterminated_text", test_menu_bad_text},
        {"menu_text_limit", test_menu_text_limit},
        {"appmessage_get_reply", test_get_message},
        {"appmessage_wait_get", test_wait_message},
        {"guest_message_first", test_guest_message_first},
        {"reply_listed_copy", test_reply_listed_copy},
        {"copy_limit_get", [] { test_copy_limit(false); }},
        {"copy_limit_wait", [] { test_copy_limit(true); }},
        {"delete_port_cleanup", test_delete_port},
        {"free_signal_cleanup", test_free_signal},
        {"io_cleanup_all_ports", test_io_cleanup},
        {"remove_while_sender_waits", test_remove_while_sender_waits},
        {"delete_port_while_sender_waits", test_delete_port_while_sender_waits},
        {"sender_not_a_process", test_sender_not_a_process},
        {"wbinfo_screen", [] { test_wbinfo(true); }},
        {"wbinfo_default_screen", [] { test_wbinfo(false); }},
        {"wbinfo_unknown_screen", [] { test_wbinfo_bad(true); }},
        {"wbinfo_unterminated_name", [] { test_wbinfo_bad(false); }},
    };
    unsigned ran = 0;
    for (const auto &[name, run] : tests)
    {
        if (argc > 1 && std::strcmp(argv[1], name)) continue;
        case_name = name; run(); ++ran;
        std::printf("PASS %s\n", name);
    }
    if (!ran) { std::fprintf(stderr, "Unknown Workbench test case\n"); return EXIT_FAILURE; }
    std::printf("Workbench: %u cases, %u checks passed\n", ran, checks);
    return 0;
}
