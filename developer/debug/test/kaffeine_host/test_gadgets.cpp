/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    GadTools gadgets of 68k code through literal LVOs, the real dispatcher
    and heap: CreateContext() and CreateGadgetA() for every kind, the
    native NewGadget and tags that GadTools gets, the 68k
    mirror of each gadget with its label, its StringInfo or PropInfo and the
    gadgets that GadTools puts before it, the shadows that the generated
    thunks find, FreeGadgets(), refusals and lack of memory, and
    GT_GetGadgetAttrsA() and GT_SetGadgetAttrsA() with the attributes of
    every kind, OnGadget(), OffGadget(), and gadget lists on windows. The native
    GadTools below builds gadget lists as AROS does, checks that it gets
    only the tags that AROS reads for a kind, keeps what the classes keep
    and reads it again before it frees a gadget, and answers a get as
    AROS's classes do.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

#include <exec/lists.h>
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include "kaffeine_intern.h"
#include "kaffeine_rastport.h"

#include "kaffeine_menus.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_classic.h"
#include "kaffeine_copy.h"
#include "kaffeine_window.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_offsets.h"

/* The StringExtend fixture below supplies one explicitly held native
   font. Other fixtures only borrow their metadata-only RastPort fonts. */
static TextFont *held_fixture_font;
static unsigned fixture_font_closes;
extern "C" void CloseFont(struct TextFont *font)
{
    assert(font && font == held_fixture_font && font->tf_Accessors == 1);
    --font->tf_Accessors;
    held_fixture_font = nullptr;
    ++fixture_font_closes;
}

/* ── The native side ─────────────────────────────────────────────── */

static unsigned native_calls;
static unsigned forbid_calls, permit_calls, forbid_depth, forbid_peak;
static std::function<void(bool)> scheduler_hook;
extern "C" void kaffeine_gadget_test_forbid(void)
{
    ++forbid_calls;
    forbid_peak = std::max(forbid_peak, ++forbid_depth);
    if (scheduler_hook) scheduler_hook(true);
}
extern "C" void kaffeine_gadget_test_permit(void)
{
    assert(forbid_depth);
    ++permit_calls;
    if (scheduler_hook) scheduler_hook(false);
    --forbid_depth;
}
/* Install boundary observations only while all captured objects live. */
struct SchedulerProbe
{
    unsigned nesting = forbid_depth;
    explicit SchedulerProbe(std::function<void(bool)> hook)
    {
        assert(!scheduler_hook);
        scheduler_hook = std::move(hook);
    }
    ~SchedulerProbe()
    {
        scheduler_hook = {};
        assert(forbid_depth == nesting);
    }
};
static std::vector<std::string> debug_lines;
extern "C" void kaffeine_gadget_test_debug(const char *format, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    debug_lines.emplace_back(line);
}

/* AllocVec blocks of the gadget bridge, including keep() copies. */
static std::set<APTR> native_blocks;
static std::map<APTR, ULONG> native_sizes;
static unsigned native_allocations;
static int allocations_before_failure = -1;
static void assert_block_unlinked(APTR block);
/* workbench.library is not linked here: no port has App objects. */
extern "C" BOOL Kaffeine_AppPortFill(KaffeineContext *, ULONG, BOOL) { return FALSE; }
extern "C" BOOL Kaffeine_AppPortMatches(KaffeineContext *, ULONG, Task *, UBYTE) { return TRUE; }
extern "C" BOOL Kaffeine_AppReplyMsg(KaffeineContext *, ULONG) { return FALSE; }

extern "C" APTR AllocVec(ULONG size, ULONG)
{
    if (allocations_before_failure == 0)
    {
        allocations_before_failure = -1;
        return nullptr;
    }
    if (allocations_before_failure > 0)
        --allocations_before_failure;
    APTR block = std::calloc(1, size);
    if (block)
    {
        assert(native_blocks.insert(block).second);
        native_sizes[block] = size;
        ++native_allocations;
    }
    return block;
}
extern "C" void FreeVec(APTR block)
{
    if (block)
    {
        assert_block_unlinked(block);
        assert(native_blocks.erase(block) == 1);
        assert(native_sizes.erase(block) == 1);
    }
    std::free(block);
}

static APTR block_holding(const void *p)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    for (auto &block : native_sizes)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(block.first);
        if (address >= base && address - base < block.second)
            return block.first;
    }
    return nullptr;
}

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* VisualInfos */
struct VisualInfo { struct Screen *screen; };
static std::set<VisualInfo *> visualinfos;

/* Native public-screen visitor accounting for VisualInfo lifetime. */
static KaffeineContext *visual_context;
static std::map<Screen *, unsigned> visual_locks;
static bool visual_lock_denied;
static List visual_list{};
static PubScreenNode visual_nodes[16]{};
static unsigned visual_node_count, visual_list_depth;
extern "C" List *LockPubScreenList(void)
{
    ++visual_list_depth;
    visual_node_count = 0;
    for (UWORD i = 0; i < visual_context->num_shadows && visual_node_count < 16; ++i)
        if (visual_context->shadow_map[i].type == KAFFEINE_SHADOW_PUBSCREEN)
        {
            auto &p = visual_nodes[visual_node_count++];
            p = PubScreenNode{};
            p.psn_Screen = static_cast<Screen *>(visual_context->shadow_map[i].native_ptr);
            p.psn_Node.ln_Name = const_cast<STRPTR>("fixture-public");
        }
    visual_list = List{};
    visual_list.lh_Head = visual_node_count ? &visual_nodes[0].psn_Node : reinterpret_cast<Node *>(&visual_list.lh_Tail);
    for (unsigned i = 0; i < visual_node_count; ++i)
        visual_nodes[i].psn_Node.ln_Succ = i + 1 < visual_node_count ? &visual_nodes[i+1].psn_Node : reinterpret_cast<Node *>(&visual_list.lh_Tail);
    return &visual_list;
}
extern "C" void UnlockPubScreenList(void) { assert(visual_list_depth); --visual_list_depth; }
extern "C" Screen *LockPubScreen(CONST_STRPTR name)
{
    assert(visual_list_depth);
    if (visual_lock_denied) return nullptr;
    for (unsigned i = 0; i < visual_node_count; ++i)
        if (name == visual_nodes[i].psn_Node.ln_Name)
        {
            ++visual_locks[visual_nodes[i].psn_Screen];
            return visual_nodes[i].psn_Screen;
        }
    return nullptr;
}
extern "C" void UnlockPubScreen(UBYTE *name, Screen *screen)
{
    assert(!name && visual_locks[screen]);
    --visual_locks[screen];
}

extern "C" APTR GetVisualInfoA(struct Screen *screen, struct TagItem *tagList)
{
    ++native_calls;
    assert(!tagList);
    auto *vi = new VisualInfo{ screen };
    visualinfos.insert(vi);
    return vi;
}

extern "C" void FreeVisualInfo(APTR vi)
{
    ++native_calls;
    auto *v = static_cast<VisualInfo *>(vi);
    assert(visualinfos.erase(v));
    delete v;
}

/* The menu calls of kaffeine_menus.c, which these tests never make */
extern "C" struct Menu *CreateMenusA(struct NewMenu *, struct TagItem *) { assert(false); return nullptr; }
extern "C" void FreeMenus(struct Menu *) { assert(false); }
extern "C" BOOL LayoutMenusA(struct Menu *, APTR, struct TagItem *) { assert(false); return FALSE; }
extern "C" BOOL LayoutMenuItemsA(struct MenuItem *, APTR, struct TagItem *) { assert(false); return FALSE; }
extern "C" void DrawBevelBoxA(struct RastPort *, WORD, WORD, WORD, WORD, struct TagItem *) { assert(false); }
/* Intuition's list operations, from rom/intuition/{addglist,addgadget,
   removeglist}.c. System gadgets are ordinary native Gadgets, without
   bridge shadows. In particular RemoveGList deliberately retains the
   last removed gadget's NextGadget; the GadTools batch bridge repairs it. */
struct GadgetCall
{
    const char *name;
    Window *window;
    Gadget *head;
    UWORD position;
    WORD num;
    Requester *requester;
    UWORD result;
    Gadget *last, *after;
};
static std::vector<GadgetCall> gadget_calls;
static std::vector<Window *> watched_windows;
static void assert_block_unlinked(APTR block)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(block);
    const ULONG size = native_sizes.at(block);
    for (Window *w : watched_windows)
    {
        unsigned n = 0;
        for (Gadget *g = w->FirstGadget; g; g = g->NextGadget)
        {
            assert(++n < 100);
            const uintptr_t p = reinterpret_cast<uintptr_t>(g);
            assert(p < base || p - base >= size);
        }
    }
}
static std::vector<std::string> gadget_events;
static bool layout_gadgets;
/* RemoveGList() deactivates what it takes off, and RefreshGList() renders
   strings, counting their text as UpdateStringInfo() of strgadgets.c */
static bool remove_deselects, refresh_renders;
static Gadget *intuition_active;        /* Intuition's active gadget, of ActivateGadget() */
static void render_strings(Gadget *g, int num);
struct GadgetState { Gadget *gadget; UWORD flags, activation; };
static std::vector<GadgetState> refresh_saw;
static GadgetState enable_saw;
static UWORD refresh_toggle_flags;

/* Observe string state at native call entry, then stand in for changes
   made by Intuition during that call. Disabled for the older groups. */
struct StringState
{
    WORD pos, disp, undo, chars, count;
    LONG number;
    bool operator==(const StringState &) const = default;
};
static StringState string_state(const StringInfo &si)
{
    return { si.BufferPos, si.DispPos, si.UndoPos, si.NumChars, si.DispCount, si.LongInt };
}
static void string_state(StringInfo &si, const StringState &s)
{
    si.BufferPos = s.pos; si.DispPos = s.disp; si.UndoPos = s.undo;
    si.NumChars = s.chars; si.DispCount = s.count; si.LongInt = s.number;
}
struct StringCall { const char *name; Gadget *gadget; Window *window; StringState state; UWORD flags; };
static std::vector<StringCall> string_calls;
static std::vector<GadgetState> enable_list_saw;
static bool watch_strings, change_strings;
static bool count_string_texts;
static const UBYTE *string_allocation_end;
static StringState string_after;
static void native_strings(const char *name, Gadget *g, Window *w, WORD num)
{
    if (!watch_strings) return;
    for (unsigned n = 0; g && num; g = g->NextGadget, --num)
    {
        assert(++n < 100);
        if ((g->GadgetType & GTYP_GTYPEMASK) != GTYP_STRGADGET) continue;
        auto *si = static_cast<StringInfo *>(g->SpecialInfo);
        assert(si);
        string_calls.push_back({ name, g, w, string_state(*si), g->Flags });
        if (change_strings) string_state(*si, string_after);
        if (count_string_texts)
        {
            // UpdateStringInfo counts beyond MaxChars, up to 0x7FFE, and
            // rendering writes at NumChars, possibly in the native guard.
            // Check every access against the physical allocation in the fake.
            WORD chars = 0;
            while (chars < 0x7FFE)
            {
                assert(si->Buffer + chars < string_allocation_end);
                if (!si->Buffer[chars]) break;
                ++chars;
            }
            assert(si->Buffer + chars < string_allocation_end);
            si->NumChars = chars;
            si->DispCount = std::min<WORD>(chars, 7);
            si->Buffer[chars] = 0;
        }
    }
}
static void native_enable_strings(const char *name, Gadget *g, Window *w)
{
    if (!watch_strings || !block_holding(g) || !w) return;
    enable_list_saw.clear();
    for (Gadget *at = w->FirstGadget; at; at = at->NextGadget)
    {
        assert(enable_list_saw.size() < 100);
        enable_list_saw.push_back({ at, at->Flags, at->Activation });
    }
    native_strings(name, w->FirstGadget, w, -1);
}

/* Observe the native call boundary, without implementing Intuition's
   geometry calculation or drawing. Changes below are explicit test input. */
using PropWords = std::array<UWORD, 11>;
using KnobPlace = std::array<WORD, 4>;
struct PropState
{
    PropWords info;
    KnobPlace place;
    bool operator==(const PropState &) const = default;
};
static PropWords prop_words(const PropInfo &pi)
{
    return { pi.Flags, pi.HorizPot, pi.VertPot, pi.HorizBody, pi.VertBody,
             pi.CWidth, pi.CHeight, pi.HPotRes, pi.VPotRes, pi.LeftBorder, pi.TopBorder };
}
static void prop_words(PropInfo &pi, const PropWords &s)
{
    UWORD *fields[] = { &pi.Flags, &pi.HorizPot, &pi.VertPot, &pi.HorizBody, &pi.VertBody,
                       &pi.CWidth, &pi.CHeight, &pi.HPotRes, &pi.VPotRes, &pi.LeftBorder, &pi.TopBorder };
    for (unsigned i = 0; i < s.size(); ++i) *fields[i] = s[i];
}
static PropState prop_state(Gadget *g)
{
    auto *pi = static_cast<PropInfo *>(g->SpecialInfo);
    auto *im = static_cast<Image *>(g->GadgetRender);
    assert(pi);
    return { prop_words(*pi), im ? KnobPlace{ im->LeftEdge, im->TopEdge, im->Width, im->Height } : KnobPlace{} };
}
static void prop_state(Gadget *g, const PropState &s)
{
    prop_words(*static_cast<PropInfo *>(g->SpecialInfo), s.info);
    if (auto *im = static_cast<Image *>(g->GadgetRender))
    {
        im->LeftEdge = s.place[0]; im->TopEdge = s.place[1];
        im->Width = s.place[2]; im->Height = s.place[3];
    }
}
struct PropCall { const char *name; Gadget *gadget; Window *window; PropState state; UWORD flags; };
static std::vector<PropCall> prop_calls;
static bool watch_props, change_props;
static PropState prop_after;
static void native_props(const char *name, Gadget *g, Window *w, WORD num)
{
    if (!watch_props) return;
    assert(!forbid_depth); // bridge critical sections end before native work
    for (unsigned n = 0; g && num; g = g->NextGadget, --num)
    {
        assert(++n < 100);
        if ((g->GadgetType & GTYP_GTYPEMASK) != GTYP_PROPGADGET) continue;
        prop_calls.push_back({ name, g, w, prop_state(g), g->Flags });
        if (change_props) prop_state(g, prop_after);
    }
}
static void native_enable_props(const char *name, Gadget *g, Window *w)
{
    if (block_holding(g) && w) native_props(name, w->FirstGadget, w, -1);
}
struct ModifyPropCall
{
    const char *name;
    Gadget *gadget;
    Window *window;
    Requester *requester;
    std::array<UWORD, 5> values;
    WORD num;
};
static std::vector<ModifyPropCall> modify_calls;
static std::vector<GadgetState> modify_list_saw;

static UWORD insert_gadgets(Window *w, Gadget *g, UWORD position, WORD num,
                            Gadget **last_out, Gadget **after_out)
{
    if (!w || !g || !num)
        return (UWORD)~0;
    Gadget **slot = &w->FirstGadget;
    UWORD index = 0;
    while (position && *slot)
    {
        --position;
        ++index;
        slot = &(*slot)->NextGadget;
    }
    Gadget *last = g;
    /* AROS uses a UWORD counter, so -1 walks to the end of a short list. */
    UWORD remaining = (UWORD)num;
    while (last->NextGadget && --remaining)
        last = last->NextGadget;
    if (layout_gadgets)
        for (Gadget *at = g;; at = at->NextGadget)
        {
            /* Stand in for GM_LAYOUT changing geometry and state. */
            at->LeftEdge = (WORD)(at->LeftEdge + 11);
            at->Flags ^= GFLG_SELECTED;
            if (at == last)
                break;
        }
    *last_out = last;
    *after_out = *slot;
    last->NextGadget = *slot;
    *slot = g;
    return index;
}

extern "C" UWORD AddGList(Window *w, Gadget *g, UWORD position, WORD num, Requester *r)
{
    ++native_calls;
    gadget_events.push_back("AddGList");
    assert(!r);                         // requesters must be refused by the bridge
    Gadget *last = nullptr, *after = nullptr;
    const UWORD result = insert_gadgets(w, g, position, num, &last, &after);
    gadget_calls.push_back({ "AddGList", w, g, position, num, r, result, last, after });
    if (last) native_strings("AddGList", g, w, num);
    if (last) native_props("AddGList", g, w, num);
    return result;
}

extern "C" UWORD AddGadget(Window *w, Gadget *g, UWORD position)
{
    ++native_calls;
    gadget_events.push_back("AddGadget");
    Gadget *last = nullptr, *after = nullptr;
    const UWORD result = w && g ? insert_gadgets(w, g, position, 1, &last, &after) : 0;
    gadget_calls.push_back({ "AddGadget", w, g, position, 1, nullptr, result, last, after });
    if (last) native_strings("AddGadget", g, w, 1);
    if (last) native_props("AddGadget", g, w, 1);
    return result;
}

extern "C" UWORD RemoveGList(Window *w, Gadget *g, WORD num)
{
    ++native_calls;
    gadget_events.push_back("RemoveGList");
    Gadget *last = nullptr, *after = nullptr;
    UWORD result = (UWORD)~0;
    if (w && g && num)
    {
        Gadget **slot = &w->FirstGadget;
        UWORD index = 0;
        while (*slot && *slot != g)
        {
            slot = &(*slot)->NextGadget;
            ++index;
        }
        if (*slot)
        {
            native_props("RemoveGList", g, w, num);
            last = g;
            LONG remaining = num;
            while (last->NextGadget && --remaining)
                last = last->NextGadget;
            after = last->NextGadget;
            *slot = after;
            result = index;
            assert(last->NextGadget == after); // AROS does not clear this
            for (Gadget *at = g;; at = at->NextGadget)
            {
                /* removeglist.c sends the active gadget GM_GOINACTIVE */
                if (at == intuition_active)
                {
                    at->Activation &= ~GACT_ACTIVEGADGET;
                    intuition_active = nullptr;
                }
                if (remove_deselects)
                    at->Flags &= ~GFLG_SELECTED;
                if (at == last)
                    break;
            }
        }
    }
    gadget_calls.push_back({ "RemoveGList", w, g, 0, num, nullptr, result, last, after });
    return result;
}

extern "C" void RefreshGList(Gadget *g, Window *w, Requester *r, WORD num)
{
    ++native_calls;
    gadget_events.push_back("RefreshGList");
    gadget_calls.push_back({ "RefreshGList", w, g, 0, num, r, 0, nullptr, nullptr });
    native_strings("RefreshGList", g, w, num);
    native_props("RefreshGList", g, w, num);
    refresh_saw.clear();
    int remaining = num;
    for (Gadget *at = g; at && remaining; at = at->NextGadget, --remaining)
    {
        refresh_saw.push_back({ at, at->Flags, at->Activation });
        at->Flags ^= refresh_toggle_flags;
    }
    if (refresh_renders)
        render_strings(g, num);
}

/* Real native port lists let the classic bridge see queued messages too. */
static void empty_port(MsgPort &port)
{
    port.mp_MsgList.lh_Head = reinterpret_cast<Node *>(&port.mp_MsgList.lh_Tail);
    port.mp_MsgList.lh_Tail = nullptr;
    port.mp_MsgList.lh_TailPred = reinterpret_cast<Node *>(&port.mp_MsgList.lh_Head);
}
static void queue_message(MsgPort &port, IntuiMessage &im)
{
    Node *n = &im.ExecMessage.mn_Node;
    n->ln_Succ = reinterpret_cast<Node *>(&port.mp_MsgList.lh_Tail);
    n->ln_Pred = port.mp_MsgList.lh_TailPred;
    n->ln_Pred->ln_Succ = n;
    port.mp_MsgList.lh_TailPred = n;
}
extern "C" Message *GetMsg(MsgPort *port)
{
    ++native_calls;
    Node *n = port->mp_MsgList.lh_Head;
    if (!n->ln_Succ)
        return nullptr;
    port->mp_MsgList.lh_Head = n->ln_Succ;
    n->ln_Succ->ln_Pred = reinterpret_cast<Node *>(&port->mp_MsgList.lh_Head);
    return reinterpret_cast<Message *>(n);
}
extern "C" Message *WaitPort(MsgPort *port)
{
    ++native_calls;
    Node *n = port->mp_MsgList.lh_Head;
    return n->ln_Succ ? reinterpret_cast<Message *>(n) : nullptr;
}
static std::vector<Message *> replied_messages;
extern "C" void ReplyMsg(Message *im) { ++native_calls; replied_messages.push_back(im); }

static Window *open_result;
static std::vector<TagItem> opened_tags;
static std::vector<Window *> closed_windows;
extern "C" Window *OpenWindowTagList(NewWindow *nw, TagItem *tags)
{
    ++native_calls;
    assert(!nw);
    opened_tags.clear();
    Gadget *head = nullptr;
    for (TagItem *t = tags; t->ti_Tag != TAG_DONE; ++t)
    {
        opened_tags.push_back(*t);
        if (t->ti_Tag == WA_Gadgets)
            head = reinterpret_cast<Gadget *>(t->ti_Data);
    }
    opened_tags.push_back({ TAG_DONE, 0 });
    if (open_result)
    {
        Gadget **slot = &open_result->FirstGadget;
        while (*slot) slot = &(*slot)->NextGadget;
        *slot = head;
        native_strings("OpenWindowTagList", head, open_result, -1);
        native_props("OpenWindowTagList", head, open_result, -1);
    }
    return open_result;
}
extern "C" void FreeSysRequest(Window *) { assert(false); }
extern "C" void CloseWindow(Window *w)
{
    ++native_calls;
    closed_windows.push_back(w);
    if (w->UserPort)
        while (Message *im = GetMsg(w->UserPort)) ReplyMsg(im);
}
extern "C" BOOL ModifyIDCMP(Window *, ULONG) { assert(false); return FALSE; }
extern "C" BOOL SetMenuStrip(struct Window *, struct Menu *) { assert(false); return FALSE; }
extern "C" BOOL ResetMenuStrip(struct Window *, struct Menu *) { assert(false); return FALSE; }
extern "C" void ClearMenuStrip(struct Window *) { assert(false); }
extern "C" struct MenuItem *ItemAddress(struct Menu *, UWORD) { assert(false); return nullptr; }
extern "C" void OffMenu(struct Window *, UWORD) { assert(false); }
extern "C" void OnMenu(struct Window *, UWORD) { assert(false); }

static std::vector<APTR> disposed;
extern "C" void DisposeObject(APTR object)
{
    ++native_calls;
    disposed.push_back(object);
}

/* The Intuition calls that the bridge passes on for other objects, with
   the generic tag conversion that their generated thunks use */
static std::vector<std::string> passed;
static APTR passed_object, passed_window, passed_requester;
static IPTR passed_more;
static unsigned converted_lists;
extern "C" struct TagItem *m68k_to_native_taglist(KaffeineContext *, ULONG tags)
{
    ++converted_lists;
    if (!tags)
        return nullptr;
    auto *list = static_cast<TagItem *>(AllocVec(2 * sizeof(TagItem), MEMF_CLEAR));
    list[0].ti_Tag = TAG_IGNORE;
    list[0].ti_Data = tags;
    list[1].ti_Tag = TAG_DONE;
    return list;
}
/* AROS activates whatever gadget it gets: the bridge passes only one on
   that window's list. Activation sets GACT_ACTIVEGADGET, and Intuition
   keeps the gadget as its active one until RemoveGList() deactivates it. */
static BOOL activate_result;
extern "C" BOOL ActivateGadget(struct Gadget *gadget, struct Window *window, struct Requester *requester)
{
    ++native_calls;
    passed.push_back("ActivateGadget");
    passed_object = gadget;
    passed_window = window;
    passed_requester = requester;
    Gadget *at = window ? window->FirstGadget : nullptr;
    while (at && at != gadget)
        at = at->NextGadget;
    assert(gadget && at == gadget && !requester);
    native_strings("ActivateGadget", gadget, window, 1);
    native_props("ActivateGadget", gadget, window, 1);
    if (activate_result)
    {
        gadget->Activation |= GACT_ACTIVEGADGET;
        intuition_active = gadget;
    }
    return activate_result;
}
extern "C" ULONG SetAttrsA(APTR object, struct TagItem *tags)
{
    ++native_calls;
    passed.push_back("SetAttrsA");
    passed_object = object;
    passed_more = tags ? tags[0].ti_Data : 0;
    return 7;
}
extern "C" ULONG GetAttr(ULONG id, Object *object, IPTR *storage)
{
    ++native_calls;
    passed.push_back("GetAttr");
    passed_object = object;
    passed_more = (IPTR)storage ^ id;
    return 1;
}
extern "C" IPTR SetGadgetAttrsA(struct Gadget *gadget, struct Window *window, struct Requester *requester,
                                struct TagItem *tags)
{
    ++native_calls;
    passed.push_back("SetGadgetAttrsA");
    passed_object = gadget;
    passed_window = window;
    passed_requester = requester;
    passed_more = tags ? tags[0].ti_Data : 0;
    return 0x55;
}
extern "C" IPTR DoGadgetMethodA(struct Gadget *gadget, struct Window *window, struct Requester *requester,
                                Msg msg)
{
    ++native_calls;
    passed.push_back("DoGadgetMethodA");
    passed_object = gadget;
    passed_window = window;
    passed_requester = requester;
    passed_more = (IPTR)msg;
    return 0x66;
}

/* A gadget as AROS's GadTools makes one, and what its class keeps */
struct Native
{
    ExtGadget gad{};
    ULONG kind = 0;
    bool context = false, part = false;
    /* the label, as makeitext() copies it with its font, and the key
       letter again in an underlined copy of the font */
    IntuiText it[2]{};
    TextAttr ta{}, ta2{};
    std::string label, font;
    char key[2] = {};
    Image arrow{};
    StringInfo si{};
    PropInfo pi{};
    UBYTE *buffer = nullptr, *undo = nullptr;
    int extension = 0;
    /* what the classes keep and read again when they render */
    STRPTR *labels = nullptr;
    CONST_STRPTR text = nullptr, format = nullptr;
    std::string copied;
    const UBYTE *colors = nullptr;
    UWORD numcolors = 0;
    /* the private state of the classes that their getters give */
    ULONG active = 0;                   /* cycle and mx */
    UBYTE color = 0, offset = 0;        /* palette: an index, the first colour */
    WORD min = 0, max = 0, level = 0;   /* slider */
    UWORD top = 0, total = 0, visible = 0;  /* scroller */
    Native *arrows[2] = {};           /* scroller's button-class children */
    /* listviewclass.c's WORD state, and the List it holds without copying */
    List *entries = nullptr;
    WORD lv_top = 0, selected = 0, num_entries = 0, itemheight = 8, spacing = 0;
    bool read_only = false;
    IPTR show_selected = ~(IPTR)0;
    Native *scroller = nullptr;
    /* and what the text class keeps */
    LONG number = 0;
    UBYTE frontpen = 1, backpen = 0, justification = 0;
};
static std::map<Gadget *, Native *> natives;
static std::vector<TagItem> created_tags;
static NewGadget created_ng;
static ULONG created_kind;
static std::string created_string;
static bool create_fails, context_fails, listview_fails_after_move;
static volatile size_t sink;

static Gadget *G(Native *n) { return reinterpret_cast<Gadget *>(&n->gad); }

/* num gadgets from g, or all for -1, as RefreshGList() renders them */
static void render_strings(Gadget *g, int num)
{
    for (; g && num; g = g->NextGadget, --num)
    {
        auto it = natives.find(g);
        if (it != natives.end() && it->second->buffer)
            it->second->si.NumChars = (WORD)std::strlen((const char *)it->second->buffer);
    }
}
static int dispatcher;

/* GetTagData(): the first counts */
static IPTR first_data(TagItem *tags, Tag tag, IPTR def)
{
    for (TagItem *t = tags; t && t->ti_Tag != TAG_DONE; ++t)
        if (t->ti_Tag == tag)
            return t->ti_Data;
    return def;
}

/* A NextTagItem() loop of a make function: the last counts */
static IPTR last_data(TagItem *tags, Tag tag, IPTR def)
{
    for (TagItem *t = tags; t && t->ti_Tag != TAG_DONE; ++t)
        if (t->ti_Tag == tag)
            def = t->ti_Data;
    return def;
}

/* The tags of the program that AROS's GadTools reads for a kind */
static bool reads(ULONG kind, Tag tag)
{
    static const std::map<ULONG, std::set<Tag>> kinds =
    {
        { GENERIC_KIND, {} },
        { BUTTON_KIND, { GA_Disabled, GA_Immediate } },
        { CHECKBOX_KIND, { GA_Disabled, GTCB_Checked, GTCB_Scaled } },
        { CYCLE_KIND, { GA_Disabled, GTCY_Labels, GTCY_Active } },
        { LISTVIEW_KIND, { GA_Disabled, GTLV_Top, GTLV_MakeVisible, GTLV_Labels, GTLV_Selected,
                           GTLV_ItemHeight, GTLV_CallBack, GTLV_MaxPen, GTLV_ReadOnly,
                           LAYOUTA_Spacing, GTLV_ShowSelected, GTLV_ScrollWidth } },
        { MX_KIND, { GA_Disabled, GTMX_Labels, GTMX_Active, GTMX_Spacing, GTMX_Scaled, GTMX_TitlePlace } },
        { PALETTE_KIND, { GA_Disabled, GTPA_Depth, GTPA_Color, GTPA_ColorOffset, GTPA_IndicatorWidth,
                          GTPA_IndicatorHeight, GTPA_NumColors, GTPA_ColorTable } },
        { TEXT_KIND, { GTTX_Text, GTTX_CopyText, GTTX_Clipped, GTTX_Border, GTTX_FrontPen,
                       GTTX_BackPen, GTTX_Justification } },
        { NUMBER_KIND, { GTNM_Number, GTNM_Format, GTNM_Clipped, GTNM_Border, GTNM_FrontPen,
                         GTNM_BackPen, GTNM_Justification, GTNM_MaxNumberLen } },
        { SLIDER_KIND, { GA_Disabled, GA_RelVerify, GA_Immediate, GTSL_Min, GTSL_Max, GTSL_Level,
                         PGA_Freedom, GTSL_LevelFormat, GTSL_MaxLevelLen, GTSL_MaxPixelLen,
                         GTSL_LevelPlace, GTSL_Justification, GTSL_DispFunc } },
        { SCROLLER_KIND, { GTSC_Top, GTSC_Total, GTSC_Visible, PGA_Freedom, GA_Disabled,
                           GA_RelVerify, GA_Immediate, GTSC_Arrows } },
        { STRING_KIND, { GA_Disabled, GA_Immediate, GA_RelVerify, GA_TabCycle, GTST_String,
                         GTST_MaxChars, GTST_EditHook, STRINGA_ExitHelp, STRINGA_Justification,
                         STRINGA_ReplaceMode } },
        { INTEGER_KIND, { GA_Disabled, GA_Immediate, GA_RelVerify, GA_TabCycle, GTIN_Number,
                          GTIN_MaxChars, GTIN_EditHook, STRINGA_ExitHelp, STRINGA_Justification,
                          STRINGA_ReplaceMode } },
    };
    auto k = kinds.find(kind);
    return tag == GT_Underscore || (k != kinds.end() && k->second.count(tag));
}

static ULONG count_labels(STRPTR *labels)
{
    ULONG n = 0;
    while (labels && labels[n])
        ++n;
    return n;
}

static Native *make(ULONG kind, NewGadget *ng)
{
    auto *n = new Native{};
    n->kind = kind;
    n->gad.LeftEdge = ng->ng_LeftEdge;
    n->gad.TopEdge = ng->ng_TopEdge;
    n->gad.Width = ng->ng_Width;
    n->gad.Height = ng->ng_Height;
    n->gad.Flags = GFLG_EXTENDED;
    n->gad.Activation = GACT_RELVERIFY;
    n->gad.GadgetType = GTYP_CUSTOMGADGET | GTYP_GADTOOLS;
    n->gad.GadgetID = ng->ng_GadgetID;
    n->gad.UserData = ng->ng_UserData;
    /* gadgetclass sets AROS's GMORE_BOOPSIGADGET on every gadget */
    n->gad.MoreFlags = GMORE_BOUNDS | GMORE_GADGETHELP | GMORE_BOOPSIGADGET;
    n->gad.BoundsLeftEdge = ng->ng_LeftEdge;
    n->gad.BoundsTopEdge = ng->ng_TopEdge;
    n->gad.BoundsWidth = ng->ng_Width;
    n->gad.BoundsHeight = ng->ng_Height;
    /* native pointers that must not reach the program: a render image,
       and the dispatcher that gadgetclass keeps in MutualExclude */
    n->gad.GadgetRender = &n->arrow;
    n->gad.MutualExclude = (IPTR)&dispatcher;
    natives[G(n)] = n;
    return n;
}

/* A child that GadTools makes itself, with no label and no UserData */
static Native *child(ULONG kind, NewGadget *ng, UWORD id)
{
    Native *n = make(kind, ng);
    n->part = true;
    n->gad.GadgetID = id;
    n->gad.UserData = nullptr;
    return n;
}

static void label(Native *n, NewGadget *ng, TagItem *tags)
{
    if (!ng->ng_GadgetText)
        return;
    const char underscore = (char)first_data(tags, GT_Underscore, 1);
    n->label = ng->ng_GadgetText;
    n->font = ng->ng_TextAttr ? (const char *)ng->ng_TextAttr->ta_Name : "topaz.font";
    /* makeitext() finds a NUL underscore at the end of the label and then
       copies -1 bytes, and counts the lengths in WORDs */
    assert(underscore != 0);
    assert(n->label.size() + n->font.size() + 100 < 32768);
    n->ta.ta_Name = (STRPTR)n->font.c_str();
    n->ta.ta_YSize = ng->ng_TextAttr ? ng->ng_TextAttr->ta_YSize : 8;
    n->it[0].FrontPen = (ng->ng_Flags & NG_HIGHLABEL) ? 2 : 1;
    n->it[0].DrawMode = JAM1;
    n->it[0].LeftEdge = -40;
    n->it[0].ITextFont = &n->ta;
    const size_t at = n->label.find(underscore);
    if (at != std::string::npos)
    {
        n->label.erase(at, 1);
        n->key[0] = n->label[at];
        n->ta2 = n->ta;
        n->ta2.ta_Style |= FSF_UNDERLINED;
        n->it[1] = n->it[0];
        n->it[1].IText = (UBYTE *)n->key;
        n->it[1].ITextFont = &n->ta2;
        n->it[1].LeftEdge = (WORD)(-40 + 8 * at);
        n->it[0].NextText = &n->it[1];
    }
    n->it[0].IText = (UBYTE *)n->label.c_str();
    n->gad.GadgetText = &n->it[0];
}

static void prop(Native *n, TagItem *tags, UWORD body)
{
    n->pi.Flags = AUTOKNOB | (last_data(tags, PGA_Freedom, LORIENT_HORIZ) == LORIENT_HORIZ ? FREEHORIZ : FREEVERT);
    n->pi.HorizPot = 0x4000;
    n->pi.HorizBody = body;
    n->pi.VertBody = 0xFFFF;
    n->pi.CWidth = (UWORD)(n->gad.Width - 4);
    n->pi.CHeight = (UWORD)(n->gad.Height - 2);
    n->pi.HPotRes = 0x100;
    n->pi.LeftBorder = 2;
    n->pi.TopBorder = 1;
    n->gad.SpecialInfo = &n->pi;
    n->gad.Activation |= GACT_IMMEDIATE;
}

/* The knob that propgclass.c's FindScrollerValues() works out from the
   Total, Visible and Top that a slider or a scroller gives it */
static void knob(Native *n, UWORD total, UWORD visible, UWORD top)
{
    const UWORD hidden = total > visible ? (UWORD)(total - visible) : 0;
    if (top > hidden)
        top = hidden;
    const UWORD body = hidden > 0 ? (UWORD)(((ULONG)visible * MAXBODY) / total) : MAXBODY;
    const UWORD pot = hidden > 0 ? (UWORD)(((ULONG)top * MAXPOT) / hidden) : 0;
    if (n->pi.Flags & FREEHORIZ)
    {
        n->pi.HorizBody = body;
        n->pi.HorizPot = pot;
    }
    else
    {
        n->pi.VertBody = body;
        n->pi.VertPot = pot;
    }
}

static void render(Native *n);

/* listviewclass.c: NumItemsFit returns a WORD after a UWORD assignment;
   ShownEntries differs from it when the list is shorter than the view. */
static WORD num_items_fit(Native *n)
{
    return (WORD)(UWORD)((n->gad.Height + 2 - 2 * 3) / (n->itemheight + n->spacing));
}

static WORD shown_entries(Native *n)
{
    return std::min(n->num_entries, num_items_fit(n));
}

static void disable(Native *n, IPTR value)
{
    n->gad.Flags = (UWORD)(value ? n->gad.Flags | GFLG_DISABLED : n->gad.Flags & ~GFLG_DISABLED);
    if (n->kind == SCROLLER_KIND)
        for (Native *a : n->arrows)
            if (a)
                disable(a, value);
    if (n->scroller)
        disable(n->scroller, value);
}

static void update_scroller(Native *n)
{
    if (Native *sc = n->scroller)
    {
        /* UpdateScroller's empty-list defaults are 0, 1, 1. */
        sc->total = n->num_entries > 0 ? (UWORD)n->num_entries : 1;
        sc->visible = n->num_entries > 0 ? (UWORD)shown_entries(n) : 1;
        sc->top = n->num_entries > 0 ? (UWORD)n->lv_top : 0;
        sc->top = sc->total > sc->visible ? std::min<UWORD>(sc->top, sc->total - sc->visible) : 0;
        knob(sc, sc->total, sc->visible, sc->top);
    }
}

/* listviewclass.c's DoShowSelected(), through native GadTools rather than
   a bridge setter: in particular, the entry name has not been cut. */
static void show_selected(Native *n)
{
    if (!n->show_selected || n->show_selected == ~(IPTR)0)
        return;
    TagItem tags[] = { { GTST_String, (IPTR)"" }, { TAG_DONE, 0 } };
    if (n->selected >= 0 && n->selected < n->num_entries)
    {
        WORD i = 0;
        Node *node;
        ForeachNode(n->entries, node)
            if (i++ == n->selected)
            {
                tags[0].ti_Data = (IPTR)node->ln_Name;
                break;
            }
    }
    GT_SetGadgetAttrsA((Gadget *)n->show_selected, nullptr, nullptr, tags);
}

/* listview_set() in tag order, without a GInfo. In particular Labels
   resets Top but preserves Selected; Top and MakeVisible narrow to WORD. */
static void listview_set(Native *n, TagItem *tags)
{
    bool update = false;
    for (TagItem *t = tags; t->ti_Tag != TAG_DONE; ++t)
    {
        const IPTR v = t->ti_Data;
        switch (t->ti_Tag)
        {
        case GA_Disabled:
            disable(n, v);
            break;
        case GTLV_Top:
        {
            WORD top = (WORD)v;
            if (top < 0)
                top = 0;
            else if (top > n->num_entries - shown_entries(n))
                top = (WORD)(n->num_entries - shown_entries(n));
            update |= n->lv_top != top;
            n->lv_top = top;
            break;
        }
        case GTLV_MakeVisible:
        {
            WORD entry = (WORD)v;
            if (entry < 0)
                entry = 0;
            else if (entry >= n->num_entries)
                entry = std::max<WORD>(0, (WORD)(n->num_entries - 1));
            if (entry < n->lv_top)
            {
                n->lv_top = entry;
                update = true;
            }
            else if (entry >= n->lv_top + num_items_fit(n))
            {
                n->lv_top = (WORD)(entry - (num_items_fit(n) - 1));
                update = true;
            }
            break;
        }
        case GTLV_Labels:
        {
            n->entries = (List *)v;
            n->lv_top = n->num_entries = 0;
            if (n->entries && v != ~(IPTR)0)
            {
                Node *node;
                ForeachNode(n->entries, node)
                    ++n->num_entries;
            }
            update = true;
            /* Probe each intermediate attachment before the call returns. */
            render(n);
            show_selected(n);
            break;
        }
        case GTLV_Selected:
            if (n->selected != (WORD)v)
            {
                n->selected = (WORD)v;
                show_selected(n);
            }
            break;
        case GTLV_ItemHeight: n->itemheight = (WORD)(UWORD)v; break;
        case GTLV_ReadOnly: n->read_only = v != 0; break;
        case LAYOUTA_Spacing: n->spacing = (WORD)(UWORD)v; break;
        /* MaxPen is read by makelistview but ignored by the class. */
        case GTLV_MaxPen: break;
        default: assert(false);
        }
    }
    if (update)
        update_scroller(n);
}

/* strgclass.c's set_buffer_text() for STRINGA_TextVal: MaxChars includes
   the NUL, and a nonpositive MaxChars leaves the native buffer alone. */
static void set_buffer_text(StringInfo *si, const char *text)
{
    if (si->MaxChars < 1)
        return;
    size_t len = 0;
    while (len < (size_t)si->MaxChars - 1 && text[len])
        ++len;
    std::memcpy(si->Buffer, text, len);
    si->Buffer[len] = '\0';
}

/* strgclass.c's set_buffer_str(): unsigned digits, so that LONG_MIN gets
   its own, cut to MaxChars. */
static void set_buffer_str(StringInfo *si)
{
    char buf[64];
    char *ptr = buf + sizeof(buf);
    ULONG value = (ULONG)si->LongInt;
    int len;

    if (si->LongInt < 0)
        value = 0u - value;
    do
    {
        *--ptr = (char)('0' + value % 10);
        value /= 10;
    }
    while (value != 0);
    if (si->LongInt < 0)
        *--ptr = '-';
    len = (int)(buf + sizeof(buf) - ptr);
    if (len >= si->MaxChars)
        len = si->MaxChars - 1;
    si->Buffer[len] = '\0';
    std::memcpy(si->Buffer, ptr, len);
}

extern "C" struct Gadget *CreateContext(struct Gadget **glist)
{
    ++native_calls;
    if (context_fails)
    {
        *glist = nullptr;
        return nullptr;
    }
    auto *n = new Native{};
    n->context = true;
    n->gad.Flags = GFLG_GADGHNONE | GFLG_DISABLED;
    n->gad.GadgetType = GTYP_GADTOOLS;
    natives[G(n)] = n;
    *glist = G(n);
    return G(n);
}

extern "C" struct Gadget *CreateGadgetA(ULONG kind, struct Gadget *previous, struct NewGadget *ng,
                                        struct TagItem *tags)
{
    ++native_calls;
    created_kind = kind;
    created_tags.clear();
    for (TagItem *t = tags; t; ++t)
    {
        created_tags.push_back(*t);
        if (t->ti_Tag == TAG_DONE)
            break;
        /* only the tags that GadTools reads for the kind, flat */
        assert(reads(kind, t->ti_Tag));
    }
    created_ng = ng ? *ng : NewGadget{};
    if (!previous || !ng || !ng->ng_VisualInfo)
        return nullptr;
    assert(visualinfos.count(static_cast<VisualInfo *>(ng->ng_VisualInfo)));
    previous->NextGadget = nullptr;
    if (create_fails)
        return nullptr;

    std::vector<Native *> chain;
    Native *n = nullptr;
    switch (kind)
    {
    case GENERIC_KIND:
        n = make(kind, ng);
        n->gad.GadgetType = GTYP_GADTOOLS;
        n->gad.Activation = 0;
        n->gad.GadgetRender = nullptr;
        n->gad.MutualExclude = 0;
        n->gad.MoreFlags &= ~GMORE_BOOPSIGADGET;
        break;
    case BUTTON_KIND:
        n = make(kind, ng);
        if (first_data(tags, GA_Disabled, FALSE))
            n->gad.Flags |= GFLG_DISABLED;
        if (first_data(tags, GA_Immediate, FALSE))
            n->gad.Activation |= GACT_IMMEDIATE;
        break;
    case CHECKBOX_KIND:
        n = make(kind, ng);
        if (first_data(tags, GTCB_Checked, FALSE))
            n->gad.Flags |= GFLG_SELECTED;
        if (!first_data(tags, GTCB_Scaled, FALSE))
        {
            n->gad.Width = 26;
            n->gad.Height = 11;
        }
        break;
    case CYCLE_KIND:
        n = make(kind, ng);
        n->labels = (STRPTR *)first_data(tags, GTCY_Labels, 0);
        n->active = (UWORD)first_data(tags, GTCY_Active, 0);
        /* cycleclass.c draws labels[active] of an array, NULL it leaves alone */
        assert(!n->labels || (n->labels[0] && n->active < count_labels(n->labels)));
        break;
    case LISTVIEW_KIND:
    {
        const WORD fontheight = ng->ng_TextAttr ? (WORD)ng->ng_TextAttr->ta_YSize : 8;
        const IPTR itemheight = last_data(tags, GTLV_ItemHeight, 0);
        const IPTR spacing = last_data(tags, LAYOUTA_Spacing, 0);
        const WORD scrollwidth = (WORD)(UWORD)last_data(tags, GTLV_ScrollWidth, 16);
        assert(scrollwidth > 0 && !last_data(tags, GTLV_CallBack, 0));
        /* creategadgeta.c corrects the height before makelistview takes
           its bounding box and rounds the view to whole items. */
        const WORD corrected = (WORD)(ng->ng_Height - (ng->ng_TextAttr ? fontheight - 4 : 7));
        WORD height = corrected;
        const IPTR shown = last_data(tags, GTLV_ShowSelected, ~(IPTR)0);
        if (shown && shown != ~(IPTR)0)
        {
            auto it = natives.find((Gadget *)shown);
            assert(it != natives.end() && it->second->kind == STRING_KIND && !it->second->part);
            ExtGadget &sg = it->second->gad;
            if (sg.BoundsWidth != ng->ng_Width || sg.BoundsHeight >= height)
                return nullptr;
            height = (WORD)(height - sg.BoundsHeight);
            sg.BoundsLeftEdge = ng->ng_LeftEdge;
            sg.BoundsTopEdge = (WORD)(ng->ng_TopEdge + height);
            sg.LeftEdge = (WORD)(sg.BoundsLeftEdge + 4);
            sg.TopEdge = (WORD)(sg.BoundsTopEdge + 2);
        }
        /* NewObjectA(listviewclass) can fail after the string was moved. */
        if (listview_fails_after_move)
            return nullptr;
        const WORD totalitemheight = (WORD)(itemheight ? itemheight + spacing : fontheight + spacing);
        const WORD viewheight = (WORD)(height - 2 * 3);
        n = make(kind, ng);
        n->gad.Width = (WORD)(ng->ng_Width - scrollwidth - 2);
        n->gad.Height = (WORD)(height - viewheight % totalitemheight);
        n->gad.BoundsHeight = corrected;
        n->itemheight = fontheight;
        n->show_selected = shown;
        /* makelistview's fixed order, regardless of the program's order:
           Disabled has no scroller yet; Labels undoes Top/MakeVisible. */
        TagItem lv[] =
        {
            { GA_Disabled, last_data(tags, GA_Disabled, FALSE) },
            { GTLV_Top, last_data(tags, GTLV_Top, 0) },
            { GTLV_MakeVisible, last_data(tags, GTLV_MakeVisible, 0) },
            { GTLV_Labels, last_data(tags, GTLV_Labels, 0) },
            { GTLV_Selected, last_data(tags, GTLV_Selected, ~(IPTR)0) },
            { GTLV_ItemHeight, itemheight ? itemheight : (IPTR)(SIPTR)fontheight },
            { GTLV_MaxPen, last_data(tags, GTLV_MaxPen, 0) },
            { GTLV_ReadOnly, last_data(tags, GTLV_ReadOnly, 0) },
            { LAYOUTA_Spacing, spacing },
            { TAG_DONE, 0 }
        };
        listview_set(n, lv);

        NewGadget scr{};
        scr.ng_LeftEdge = (WORD)(n->gad.LeftEdge + n->gad.Width - 1 + 2);
        scr.ng_TopEdge = n->gad.TopEdge;
        scr.ng_Width = scrollwidth;
        scr.ng_Height = n->gad.Height;
        Native *sc = child(SCROLLER_KIND, &scr, ng->ng_GadgetID);
        /* makescroller: subtract both arrows, then the prop borders. */
        sc->gad.LeftEdge += 2;
        sc->gad.TopEdge += 2;
        sc->gad.Width -= 4;
        sc->gad.Height = (WORD)(sc->gad.Height - 2 * scrollwidth - 4);
        TagItem vertical[] = { { PGA_Freedom, LORIENT_VERT }, { TAG_DONE, 0 } };
        prop(sc, vertical, MAXBODY);
        for (int i = 0; i < 2; ++i)
        {
            NewGadget arrow{};
            arrow.ng_LeftEdge = scr.ng_LeftEdge;
            arrow.ng_TopEdge = (WORD)(sc->gad.TopEdge + 2 + sc->gad.Height + (i == 0 ? scrollwidth : 0));
            arrow.ng_Width = arrow.ng_Height = scrollwidth;
            Native *a = child(BUTTON_KIND, &arrow, ng->ng_GadgetID);
            a->gad.Activation |= GACT_IMMEDIATE;
            a->gad.Flags |= GFLG_LABELIMAGE;
            a->arrow.Width = a->arrow.Height = scrollwidth;
            a->arrow.Depth = 1;
            a->gad.GadgetText = reinterpret_cast<IntuiText *>(&a->arrow);
            sc->arrows[1 - i] = a;
            chain.push_back(a); /* increment, decrement, prop, listview */
        }
        chain.push_back(sc);
        n->scroller = sc;
        update_scroller(n); /* GTA_Listview_Scroller, before bridge replay */
        break;
    }
    case MX_KIND:
        if (!first_data(tags, GTMX_Labels, 0))
            return nullptr;
        n = make(kind, ng);
        n->labels = (STRPTR *)first_data(tags, GTMX_Labels, 0);
        n->active = (ULONG)first_data(tags, GTMX_Active, 0);
        n->gad.Width = 17;
        break;
    case PALETTE_KIND:
    {
        IPTR colors = 2;
        for (TagItem *t = tags; t->ti_Tag != TAG_DONE; ++t)
            if (t->ti_Tag == GTPA_Depth)
                /* Define the CPU's native-width shift for an odd depth. */
                colors = (IPTR)1 << (t->ti_Data % (8 * sizeof(IPTR)));
            else if (t->ti_Tag == GTPA_NumColors)
                colors = t->ti_Data;
        n = make(kind, ng);
        n->colors = (const UBYTE *)last_data(tags, GTPA_ColorTable, 0);
        n->numcolors = (UWORD)colors;
        /* paletteclass.c looks for the pen with a WORD index */
        /* the pen becomes an index into the table, or an offset */
        n->offset = (UBYTE)last_data(tags, GTPA_ColorOffset, 0);
        n->color = (UBYTE)last_data(tags, GTPA_Color, 0);
        if (n->colors)
        {
            for (WORD i = 0; i < n->numcolors; ++i)
            {
                assert(i >= 0);
                if (n->colors[i] == n->color)
                {
                    n->color = (UBYTE)i;
                    break;
                }
            }
        }
        else
            n->color = (UBYTE)(n->color - n->offset);
        if (n->color > n->numcolors - 1)
            n->color = 0;
        break;
    }
    case TEXT_KIND:
    {
        n = make(kind, ng);
        /* textclass.c indexes dri_Pens with its pens */
        assert(last_data(tags, GTNM_FrontPen, 0) < NUMDRIPENS && last_data(tags, GTNM_BackPen, 0) < NUMDRIPENS);
        n->frontpen = (UBYTE)last_data(tags, GTTX_FrontPen, 1);
        n->backpen = (UBYTE)last_data(tags, GTTX_BackPen, 0);
        n->justification = (UBYTE)last_data(tags, GTTX_Justification, GTJ_LEFT);
        auto text = (CONST_STRPTR)last_data(tags, GTTX_Text, 0);
        if (last_data(tags, GTTX_CopyText, FALSE) && text)
        {
            n->copied = text;
            n->text = n->copied.c_str();
        }
        else
            n->text = text;
        break;
    }
    case NUMBER_KIND:
        n = make(kind, ng);
        assert(last_data(tags, GTNM_FrontPen, 0) < NUMDRIPENS && last_data(tags, GTNM_BackPen, 0) < NUMDRIPENS);
        n->frontpen = (UBYTE)last_data(tags, GTNM_FrontPen, 1);
        n->backpen = (UBYTE)last_data(tags, GTNM_BackPen, 0);
        n->justification = (UBYTE)last_data(tags, GTNM_Justification, GTJ_CENTER);
        n->format = (CONST_STRPTR)last_data(tags, GTNM_Format, (IPTR)"%ld");
        n->number = (LONG)last_data(tags, GTNM_Number, 0);
        break;
    case STRING_KIND:
    case INTEGER_KIND:
    {
        n = make(kind, ng);
        n->gad.LeftEdge += 4;
        n->gad.TopEdge += 2;
        n->gad.Width -= 8;
        n->gad.Height -= 4;
        n->gad.Flags |= GFLG_STRINGEXTEND;
        const WORD maxchars = (WORD)((WORD)last_data(tags, kind == STRING_KIND ? GTST_MaxChars : GTIN_MaxChars,
                                                     kind == STRING_KIND ? 64 : 10) + 1);
        assert(maxchars > 0);
        n->buffer = new UBYTE[maxchars];
        n->undo = new UBYTE[maxchars];
        n->buffer[0] = n->undo[0] = 0;
        n->si.Buffer = n->buffer;
        n->si.UndoBuffer = n->undo;
        n->si.MaxChars = maxchars;
        n->si.Extension = reinterpret_cast<StringExtend *>(&n->extension);
        n->si.AltKeyMap = reinterpret_cast<KeyMap *>(&n->extension);
        n->si.CLeft = 3;
        n->si.CTop = 2;
        n->si.DispCount = 9;
        if (kind == STRING_KIND)
        {
            /* as the string class; the text that the bridge passed is kept,
               to check the bridge's own cut */
            if (auto s = (CONST_STRPTR)last_data(tags, GTST_String, 0))
            {
                created_string = (const char *)s;
                set_buffer_text(&n->si, s);
            }
        }
        else
        {
            n->si.LongInt = (LONG)last_data(tags, GTIN_Number, 0);
            set_buffer_str(&n->si);
            n->gad.Activation |= GACT_LONGINT;
        }
        n->si.NumChars = (WORD)std::strlen((const char *)n->buffer);
        n->si.BufferPos = n->si.NumChars;
        n->gad.SpecialInfo = &n->si;
        break;
    }
    case SLIDER_KIND:
    {
        auto lformat = (CONST_STRPTR)last_data(tags, GTSL_LevelFormat, 0);
        /* the level display goes between previous and the slider */
        if (lformat || last_data(tags, GTSL_MaxLevelLen, 0) || last_data(tags, GTSL_MaxPixelLen, 0))
        {
            Native *level = child(TEXT_KIND, ng, 0);
            level->format = lformat ? lformat : "%ld";
            level->gad.LeftEdge -= 30;
            chain.push_back(level);
        }
        n = make(kind, ng);
        prop(n, tags, 0x1000);
        n->min = (WORD)last_data(tags, GTSL_Min, 0);
        n->max = (WORD)last_data(tags, GTSL_Max, 15);
        n->level = (WORD)last_data(tags, GTSL_Level, 0);
        n->level = std::max(n->min, std::min(n->max, n->level));
        break;
    }
    case SCROLLER_KIND:
    {
        const WORD arrows = (WORD)last_data(tags, GTSC_Arrows, 0);
        if (arrows)
        {
            /* increment, then decrement, then the prop */
            for (int i = 0; i < 2; ++i)
            {
                Native *a = child(BUTTON_KIND, ng, ng->ng_GadgetID);
                a->arrow.Width = arrows;
                a->arrow.Height = (WORD)(ng->ng_Height + i);
                a->arrow.Depth = 1;
                a->gad.Flags |= GFLG_LABELIMAGE;
                a->gad.GadgetText = reinterpret_cast<IntuiText *>(&a->arrow);
                a->gad.LeftEdge = (WORD)(ng->ng_LeftEdge + ng->ng_Width - arrows * (2 - i));
                a->gad.Width = arrows;
                chain.push_back(a);
            }
        }
        n = make(kind, ng);
        if (arrows)
        {
            n->arrows[0] = chain[0];
            n->arrows[1] = chain[1];
        }
        n->gad.Width = (WORD)(n->gad.Width - 2 * arrows);
        prop(n, tags, 0x2000);
        n->top = (UWORD)last_data(tags, GTSC_Top, 0);
        n->total = (UWORD)last_data(tags, GTSC_Total, 0);
        n->visible = (UWORD)last_data(tags, GTSC_Visible, 2);
        /* propgclass.c keeps Top within Total - Visible */
        n->top = n->total > n->visible ? std::min<UWORD>(n->top, (UWORD)(n->total - n->visible)) : 0;
        break;
    }
    default:
        return nullptr;
    }
    label(n, ng, tags);
    chain.push_back(n);
    Gadget *prev = previous;
    for (Native *c : chain)
    {
        prev->NextGadget = G(c);
        prev = G(c);
    }
    prev->NextGadget = nullptr;
    return G(n);
}

/* GT_GetGadgetAttrsA() of AROS: GetAttr() for every tag, which the classes
   answer from what they keep. The cycle and palette classes fill theirs in
   but return 0, the text class has no getter, and a GENERIC gadget or the
   context is no BOOPSI object, which a get would send a method into. */
static unsigned native_gets;
static std::vector<Tag> get_tags;
static bool gets_fail;
extern "C" LONG GT_GetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req,
                                   struct TagItem *taglist)
{
    ++native_calls;
    ++native_gets;
    assert(!win && !req && gad && taglist);
    /* a class that does not know the attribute: nothing written */
    if (gets_fail)
        return 0;
    auto it = natives.find(gad);
    assert(it != natives.end());
    Native *n = it->second;
    assert(!n->context && n->kind != GENERIC_KIND);
    LONG count = 0;
    for (TagItem *t = taglist; t->ti_Tag != TAG_DONE; ++t)
    {
        get_tags.push_back(t->ti_Tag);
        IPTR *storage = (IPTR *)t->ti_Data, value = 0;
        bool counted = true;
        switch (t->ti_Tag)
        {
        case GA_Disabled:
            value = (n->gad.Flags & GFLG_DISABLED) ? TRUE : FALSE;
            break;
        case GTCB_Checked:
            assert(n->kind == CHECKBOX_KIND);
            value = (n->gad.Flags & GFLG_SELECTED) ? TRUE : FALSE;
            break;
        case GTCY_Active:
            assert(n->kind == CYCLE_KIND);
            value = (UWORD)n->active;
            counted = false;
            break;
        case GTCY_Labels:
            assert(n->kind == CYCLE_KIND);
            value = (IPTR)n->labels;
            counted = false;
            break;
        case GTLV_Top:
        case GTLV_Selected:
        case GTLV_Labels:
        case GTLV_Total:
        case GTLV_Visible:
            assert(n->kind == LISTVIEW_KIND);
            value = t->ti_Tag == GTLV_Top ? (IPTR)(SIPTR)n->lv_top
                  : t->ti_Tag == GTLV_Selected ? (IPTR)(SIPTR)n->selected
                  : t->ti_Tag == GTLV_Labels ? (IPTR)n->entries
                  : t->ti_Tag == GTLV_Total ? (IPTR)(SIPTR)n->num_entries
                  : (IPTR)(SIPTR)num_items_fit(n);
            break;
        case GTMX_Active:
            assert(n->kind == MX_KIND);
            value = n->active;
            break;
        case GTPA_Color:
            assert(n->kind == PALETTE_KIND);
            value = n->colors ? n->colors[n->color] : (UWORD)(n->color + n->offset);
            counted = false;
            break;
        case GTPA_ColorOffset:
            assert(n->kind == PALETTE_KIND);
            value = n->offset;
            counted = false;
            break;
        case GTPA_ColorTable:
            assert(n->kind == PALETTE_KIND);
            value = (IPTR)n->colors;
            counted = false;
            break;
        case GTSL_Min:
            assert(n->kind == SLIDER_KIND);
            value = (IPTR)(SIPTR)n->min;
            break;
        case GTSL_Max:
            assert(n->kind == SLIDER_KIND);
            value = (IPTR)(SIPTR)n->max;
            break;
        case GTSL_Level:
            assert(n->kind == SLIDER_KIND);
            value = (IPTR)(SIPTR)n->level;
            break;
        case GTSC_Top:
            assert(n->kind == SCROLLER_KIND);
            value = n->top;
            break;
        case GTSC_Total:
            assert(n->kind == SCROLLER_KIND);
            value = n->total;
            break;
        case GTSC_Visible:
            assert(n->kind == SCROLLER_KIND);
            value = n->visible;
            break;
        case GTST_String:
            assert(n->kind == STRING_KIND || n->kind == INTEGER_KIND);
            value = (n->gad.Activation & GACT_LONGINT) ? 0 : (IPTR)n->si.Buffer;
            break;
        case GTIN_Number:
            assert(n->kind == STRING_KIND || n->kind == INTEGER_KIND);
            value = (n->gad.Activation & GACT_LONGINT) ? (IPTR)(SIPTR)n->si.LongInt : 0;
            break;
        default:
            /* the bridge asks only for what the autodoc gives for a kind,
               and answers GTTX_Text and GTNM_Number itself */
            assert(false);
        }
        *storage = value;
        count += counted;
    }
    return count;
}

/* GT_SetGadgetAttrsA() of AROS: a custom gadget gets the whole list in one
   SetGadgetAttrsA() or SetAttrsA(), and each class takes what it knows, in
   order: the button only the first GA_Disabled, a cycle's labels reset its
   choice and a choice beyond them becomes the first, an mx ignores a choice
   beyond its labels, the slider clamps its level as it comes, the prop
   class keeps Top within Total less Visible, the palette turns a pen into
   an index of its table after the list, and the string class cuts texts
   to MaxChars - 1, so the tests check the texts that the bridge passes.
   A GENERIC gadget and the context only take GA_Disabled into
   Flags; the custom parts take it through their button or text class. */
static unsigned native_sets;
static Window *set_window;              /* the window of the last native setter */
static unsigned set_attributes;
static std::vector<TagItem> set_tags;
static std::set<APTR> blocks_during_set;
static std::vector<APTR> set_entry_blocks;
struct StringSet { Gadget *gadget; std::string text; };
static std::vector<StringSet> string_sets;
extern "C" void GT_SetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req,
                                   struct TagItem *taglist)
{
    ++native_calls;
    ++native_sets;
    assert(gad && !req && taglist);
    /* GadTools draws on the window that it gets, which holds the gadget */
    set_window = win;
    for (Gadget *at = win ? win->FirstGadget : gad; at != gad; at = at->NextGadget)
        assert(at);
    auto it = natives.find(gad);
    assert(it != natives.end());
    Native *n = it->second;
    set_attributes = 0;
    set_tags.clear();
    set_entry_blocks.clear();
    blocks_during_set = native_blocks;
    for (TagItem *t = taglist; t->ti_Tag != TAG_DONE; ++t)
    {
        set_tags.push_back(*t);
        ++set_attributes;
    }
    if (n->kind == LISTVIEW_KIND)
    {
        for (TagItem &t : set_tags)
            if (t.ti_Tag == GTLV_Labels && t.ti_Data && t.ti_Data != ~(IPTR)0)
            {
                APTR block = block_holding((List *)t.ti_Data);
                assert(block);
                set_entry_blocks.push_back(block);
            }
        render(n); /* the old attachment must still be alive at entry */
        listview_set(n, taglist);
        render(n);
        return;
    }
    if ((gad->GadgetType & GTYP_GTYPEMASK) != GTYP_CUSTOMGADGET)
    {
        assert(n->context || n->kind == GENERIC_KIND);
        for (TagItem *t = taglist; t->ti_Tag != TAG_DONE; ++t)
            if (t->ti_Tag == GA_Disabled)
                n->gad.Flags = (UWORD)(t->ti_Data ? n->gad.Flags | GFLG_DISABLED : n->gad.Flags & ~GFLG_DISABLED);
        render(n);
        return;
    }
    bool disabled_seen = false, color_seen = false, scroll_seen = false, slide_seen = false;
    for (TagItem *t = taglist; t->ti_Tag != TAG_DONE; ++t)
    {
        const IPTR v = t->ti_Data;
        const bool texts = n->kind == TEXT_KIND || n->kind == NUMBER_KIND;
        switch (t->ti_Tag)
        {
        case GA_Disabled:
            if (n->kind == BUTTON_KIND && !n->part && disabled_seen)
                break;
            disabled_seen = true;
            n->gad.Flags = (UWORD)(v ? n->gad.Flags | GFLG_DISABLED : n->gad.Flags & ~GFLG_DISABLED);
            if (n->kind == SCROLLER_KIND)
                for (Native *a : n->arrows)
                    if (a)
                        a->gad.Flags = (UWORD)(v ? a->gad.Flags | GFLG_DISABLED : a->gad.Flags & ~GFLG_DISABLED);
            break;
        case GTCB_Checked:
            assert(n->kind == CHECKBOX_KIND);
            n->gad.Flags = (UWORD)(v ? n->gad.Flags | GFLG_SELECTED : n->gad.Flags & ~GFLG_SELECTED);
            break;
        case GTCY_Labels:
            assert(n->kind == CYCLE_KIND);
            n->labels = (STRPTR *)v;
            assert(!n->labels || n->labels[0]);
            n->active = 0;
            break;
        case GTCY_Active:
            assert(n->kind == CYCLE_KIND);
            n->active = (UWORD)v;
            if (n->active >= count_labels(n->labels))
                n->active = 0;
            break;
        case GTMX_Active:
            assert(n->kind == MX_KIND);
            if (v < count_labels(n->labels))
                n->active = (ULONG)v;
            break;
        case GTIN_Number:
            assert(n->kind == INTEGER_KIND);
            n->si.LongInt = (LONG)v;
            set_buffer_str(&n->si);
            n->gad.Activation |= GACT_LONGINT;
            break;
        case GTST_String:
            assert(n->kind == STRING_KIND);
            string_sets.push_back({ gad, v ? (const char *)v : "" });
            set_buffer_text(&n->si, v ? (const char *)v : "");
            n->gad.Activation &= ~GACT_LONGINT;
            break;
        case GTNM_Number:
            assert(n->kind == NUMBER_KIND);
            n->number = (LONG)v;
            break;
        case GTTX_Text:
            assert(n->kind == TEXT_KIND);
            /* the class frees a copied text at the first new one */
            n->copied.clear();
            n->text = (CONST_STRPTR)v;
            break;
        case GTNM_Format:
            assert(n->kind == NUMBER_KIND && v);
            n->format = (CONST_STRPTR)v;
            break;
        case GTNM_FrontPen:
        case GTNM_BackPen:
            assert(texts && v < NUMDRIPENS);
            (t->ti_Tag == GTNM_FrontPen ? n->frontpen : n->backpen) = (UBYTE)v;
            break;
        case GTNM_Justification:
            assert(texts);
            n->justification = (UBYTE)v;
            break;
        case GTPA_Color:
            assert(n->kind == PALETTE_KIND);
            n->color = (UBYTE)v;
            color_seen = true;
            break;
        case GTPA_ColorOffset:
            assert(n->kind == PALETTE_KIND);
            n->offset = (UBYTE)v;
            break;
        case GTPA_ColorTable:
            assert(n->kind == PALETTE_KIND);
            n->colors = (const UBYTE *)v;
            break;
        case GTSC_Top:
        case GTSC_Total:
        case GTSC_Visible:
            assert(n->kind == SCROLLER_KIND);
            (t->ti_Tag == GTSC_Top ? n->top : t->ti_Tag == GTSC_Total ? n->total : n->visible) = (UWORD)v;
            scroll_seen = true;
            break;
        case GTSL_Min:
            assert(n->kind == SLIDER_KIND);
            n->min = (WORD)v;
            slide_seen = true;
            break;
        case GTSL_Max:
            assert(n->kind == SLIDER_KIND);
            n->max = (WORD)v;
            slide_seen = true;
            break;
        case GTSL_Level:
            assert(n->kind == SLIDER_KIND);
            n->level = std::max(n->min, std::min(n->max, (WORD)v));
            slide_seen = true;
            break;
        case GTSL_LevelFormat:
            /* the slider of AROS leaves its level display alone */
            assert(n->kind == SLIDER_KIND);
            if (v)
                sink = sink + std::strlen((const char *)v);
            break;
        case GTSL_Justification:
            assert(n->kind == SLIDER_KIND);
            break;
        case GTSL_DispFunc:
            assert(n->kind == SLIDER_KIND && !v);
            break;
        default:
            assert(false);
        }
    }
    if (color_seen)
    {
        if (n->colors)
        {
            for (WORD i = 0; i < n->numcolors; ++i)
            {
                assert(i >= 0);
                if (n->colors[i] == n->color)
                {
                    n->color = (UBYTE)i;
                    break;
                }
            }
        }
        else
            n->color = (UBYTE)(n->color - n->offset);
    }
    if (scroll_seen)
    {
        n->top = n->total > n->visible ? std::min<UWORD>(n->top, (UWORD)(n->total - n->visible)) : 0;
        knob(n, n->total, n->visible, n->top);
    }
    /* sliderclass.c gives the prop class max - min + 1 positions, one of
       them visible, and the level from the end of its freedom */
    if (slide_seen)
        knob(n, (UWORD)(n->max - n->min + 1), 1,
             (UWORD)((n->pi.Flags & FREEHORIZ) ? n->level - n->min : n->max - n->level));
    render(n);
}

/* Only other objects reach native OnGadget() and OffGadget(). */
static std::vector<std::string> changed;
static APTR changed_object, changed_window, changed_requester;
static void record(const char *call, Gadget *g, Window *w, Requester *r)
{
    ++native_calls;
    changed.push_back(call);
    changed_object = g;
    changed_window = w;
    changed_requester = r;
}
extern "C" void OnGadget(struct Gadget *g, struct Window *w, struct Requester *r)
{
    assert(!natives.count(g));
    native_enable_strings("OnGadget", g, w);
    native_enable_props("OnGadget", g, w);
    if (block_holding(g))
    {
        enable_saw = { g, g->Flags, g->Activation };
        g->Flags &= ~GFLG_DISABLED;
    }
    record("OnGadget", g, w, r);
}
extern "C" void OffGadget(struct Gadget *g, struct Window *w, struct Requester *r)
{
    assert(!natives.count(g));
    native_enable_strings("OffGadget", g, w);
    native_enable_props("OffGadget", g, w);
    if (block_holding(g))
    {
        enable_saw = { g, g->Flags, g->Activation };
        g->Flags |= GFLG_DISABLED;
    }
    record("OffGadget", g, w, r);
}
/* NewModifyProp() and ModifyProp() as AROS: the values into the PropInfo
   of a proportional gadget on a window, recorded */
static void modify_prop(const char *call, Gadget *g, Window *w, Requester *r, UWORD flags,
                        UWORD hpot, UWORD vpot, UWORD hbody, UWORD vbody, WORD num)
{
    record(call, g, w, r);
    modify_calls.push_back({ call, g, w, r, { flags, hpot, vpot, hbody, vbody }, num });
    if (watch_props && w)
    {
        // Observe all siblings on the copy's actual window, also when
        // Intuition was asked to draw into a different window.
        Window *owner = w;
        for (Window *candidate : watched_windows)
            for (Gadget *at = candidate->FirstGadget; at; at = at->NextGadget)
                if (at == g) owner = candidate;
        modify_list_saw.clear();
        for (Gadget *at = owner->FirstGadget; at; at = at->NextGadget)
        {
            assert(modify_list_saw.size() < 100);
            modify_list_saw.push_back({ at, at->Flags, at->Activation });
        }
        native_strings(call, owner->FirstGadget, w, -1);
        native_props(call, owner->FirstGadget, w, -1);
    }
    if ((g->GadgetType & GTYP_GTYPEMASK) != GTYP_PROPGADGET || !g->SpecialInfo || !w)
        return;
    PropInfo *pi = static_cast<PropInfo *>(g->SpecialInfo);
    pi->Flags = flags;
    pi->HorizPot = hpot;
    pi->VertPot = vpot;
    pi->HorizBody = hbody;
    pi->VertBody = vbody;
}
extern "C" void ModifyProp(struct Gadget *g, struct Window *w, struct Requester *r, UWORD flags,
                           UWORD hpot, UWORD vpot, UWORD hbody, UWORD vbody)
{
    modify_prop("ModifyProp", g, w, r, flags, hpot, vpot, hbody, vbody, -1);
}
extern "C" void NewModifyProp(struct Gadget *g, struct Window *w, struct Requester *r, UWORD flags,
                              UWORD hpot, UWORD vpot, UWORD hbody, UWORD vbody, WORD num)
{
    modify_prop("NewModifyProp", g, w, r, flags, hpot, vpot, hbody, vbody, num);
}

/* A liveness probe for kept data, also before FreeGadgets(), not AROS's
   rendering: without a window the classes draw nothing. Palette reads
   stay within the bridge's colors[offset .. offset+numcolors-1] and
   colors[color] guarantees. */
static void render(Native *n)
{
    if (n->entries && (IPTR)n->entries != ~(IPTR)0)
    {
        Node *node;
        ForeachNode(n->entries, node)
            sink = sink + std::strlen(node->ln_Name);
    }
    for (STRPTR *l = n->labels; l && *l; ++l)
        sink = sink + std::strlen(*l);
    if (n->text)
        sink = sink + std::strlen(n->text);
    if (n->format)
        sink = sink + std::strlen(n->format);
    for (UWORD i = 0; n->colors && i < n->numcolors; ++i)
        sink = sink + n->colors[n->offset + i];
    if (n->colors)
        sink = sink + n->colors[n->color];
    if (n->it[0].IText)
        sink = sink + std::strlen((const char *)n->it[0].IText);
}

static unsigned freed;
struct FreedList
{
    Gadget *head;
    std::vector<std::pair<Gadget *, Gadget *>> links;
};
static std::vector<FreedList> free_gadget_calls;

/* GadTools' filter as far as the bridge relies on it, after
   gt_filterimsg.c: the first context on the window keeps the gadget of a
   GADGETDOWN active until a GADGETUP of any gadget, and reads it again at
   a tick or a mouse move */
static std::map<Gadget *, Gadget *> held_by;
static unsigned filtered;
static std::map<IntuiMessage *, IntuiMessage *> filtered_originals;
static bool filter_fails_once;
struct FilterCall { ULONG cls; Gadget *context, *address; bool failed; };
static std::vector<FilterCall> filter_calls;

extern "C" struct IntuiMessage *GT_FilterIMsg(struct IntuiMessage *im)
{
    ++native_calls;
    ++filtered;
    Gadget *c = im->IDCMPWindow->FirstGadget;
    for (; c; c = c->NextGadget)
    {
        auto it = natives.find(c);
        if (it != natives.end() && it->second->context)
            break;
    }
    const bool failed = filter_fails_once;
    filter_fails_once = false;
    filter_calls.push_back({ im->Class, c, static_cast<Gadget *>(im->IAddress), failed });
    // Allocation fails before GadTools changes the held gadget or makes a copy.
    if (failed)
        return nullptr;
    if (c && im->Class == IDCMP_GADGETDOWN)
        held_by[c] = static_cast<Gadget *>(im->IAddress);
    else if (c && im->Class == IDCMP_GADGETUP)
        held_by[c] = nullptr;
    auto *copy = new IntuiMessage(*im);
    filtered_originals[copy] = im;
    return copy;
}

extern "C" struct IntuiMessage *GT_PostFilterIMsg(struct IntuiMessage *copy)
{
    ++native_calls;
    auto it = filtered_originals.find(copy);
    assert(it != filtered_originals.end());
    IntuiMessage *original = it->second;
    filtered_originals.erase(it);
    delete copy;
    return original;
}

extern "C" void FreeGadgets(struct Gadget *glist)
{
    ++native_calls;
    gadget_events.push_back("FreeGadgets");
    /* A context that stays must not keep a gadget that goes active. */
    std::set<Gadget *> going;
    FreedList call{ glist, {} };
    for (Gadget *at = glist; at; at = at->NextGadget)
    {
        assert(going.size() < 100 && going.insert(at).second);
        call.links.push_back({ at, at->NextGadget });
    }
    free_gadget_calls.push_back(call);
    for (auto &h : held_by)
        assert(!h.second || !going.count(h.second) || going.count(h.first));
    for (Gadget *at : going)
        held_by.erase(at);
    assert(!intuition_active || !going.count(intuition_active)); // never the active gadget
    Gadget *last = nullptr, *next;
    for (; glist; glist = next)
    {
        /* Cleanup must remove every run before the first native free. */
        for (Window *w : watched_windows)
            for (Gadget *at = w->FirstGadget; at; at = at->NextGadget)
                assert(at != glist);
        next = glist->NextGadget;
        if (glist->GadgetType & GTYP_GADTOOLS)
        {
            auto it = natives.find(glist);
            assert(it != natives.end());
            render(it->second);
            delete[] it->second->buffer;
            delete[] it->second->undo;
            delete it->second;
            natives.erase(it);
            ++freed;
        }
        else
        {
            if (last)
                last->NextGadget = glist;
            last = glist;
        }
    }
    if (last)
        last->NextGadget = nullptr;
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

static const M68KThunkEntry gadtools_manual[] =
    { KAFFEINE_GADTOOLS_GADGET_THUNKS, KAFFEINE_GADTOOLS_MENU_THUNKS, KAFFEINE_GADTOOLS_IDCMP_THUNKS };
static const M68KThunkEntry intuition_manual[] = { KAFFEINE_INTUITION_GADGET_THUNKS };
static const M68KThunkEntry intuition_with_windows[] =
    { KAFFEINE_INTUITION_GADGET_THUNKS, KAFFEINE_INTUITION_WINDOW_THUNKS };
static const M68KThunkEntry exec_manual[] = { KAFFEINE_EXEC_IDCMP_THUNKS };
static const M68KThunkEntry intuition_generated[] =
    { { 42, poison }, { 156, poison }, { 174, poison }, { 186, poison }, { 222, poison },
      { 228, poison }, { 432, poison }, { 438, poison }, { 444, poison }, { 462, poison }, { 468, poison },
      { 642, poison }, { 648, poison }, { 654, poison }, { 660, poison }, { 810, poison } };
static const M68KThunkEntry gadtools_generated[] =
    { { 30, poison }, { 36, poison }, { 42, poison }, { 114, poison }, { 126, poison },
      { 132, poison }, { 174, poison } };

/* Guest offsets of the NDK structures */
enum
{
    NG_SIZE = 30,
    GD_NEXTGADGET = 0, GD_LEFTEDGE = 4, GD_TOPEDGE = 6, GD_WIDTH = 8, GD_HEIGHT = 10,
    GD_FLAGS = 12, GD_ACTIVATION = 14, GD_GADGETTYPE = 16, GD_GADGETRENDER = 18,
    GD_SELECTRENDER = 22, GD_GADGETTEXT = 26, GD_MUTUALEXCLUDE = 30, GD_SPECIALINFO = 34,
    GD_GADGETID = 38, GD_USERDATA = 40, GD_MOREFLAGS = 44, GD_BOUNDSLEFTEDGE = 48,
    GD_BOUNDSTOPEDGE = 50, GD_BOUNDSWIDTH = 52, GD_BOUNDSHEIGHT = 54,
    SI_BUFFER = 0, SI_UNDOBUFFER = 4, SI_BUFFERPOS = 8, SI_MAXCHARS = 10, SI_DISPPOS = 12,
    SI_UNDOPOS = 14, SI_NUMCHARS = 16,
    SI_DISPCOUNT = 18, SI_CLEFT = 20, SI_CTOP = 22, SI_EXTENSION = 24, SI_LONGINT = 28,
    SI_ALTKEYMAP = 32, STRINGINFO_SIZE = 36,
    SE_FONT = 0, SE_PENS = 4, SE_ACTIVEPENS = 6, SE_INITIALMODES = 8, SE_EDITHOOK = 12,
    SE_WORKBUFFER = 16, SE_RESERVED = 20, STRINGEXTEND_SIZE = 36,
    PI_FLAGS = 0, PI_HORIZPOT = 2, PI_VERTPOT = 4, PI_HORIZBODY = 6, PI_VERTBODY = 8, PI_CWIDTH = 10,
    PI_CHEIGHT = 12, PI_HPOTRES = 14, PI_VPOTRES = 16, PI_LEFTBORDER = 18, PI_TOPBORDER = 20,
    PROPINFO_SIZE = 22,
    IT_FRONTPEN = 0, IT_DRAWMODE = 2, IT_LEFTEDGE = 4, IT_ITEXTFONT = 8, IT_ITEXT = 12,
    IT_NEXTTEXT = 16,
    TA_NAME = 0, TA_YSIZE = 4, TA_STYLE = 6,
    IM_LEFTEDGE = 0, IM_TOPEDGE = 2, IM_WIDTH = 4, IM_HEIGHT = 6, IM_DEPTH = 8, IM_IMAGEDATA = 10,
    IM_PLANEPICK = 14, IM_PLANEONOFF = 15, IM_NEXTIMAGE = 16, IMAGE_SIZE = 20,
    LH_HEAD = 0, LH_TAIL = 4, LH_TAILPRED = 8, LIST_SIZE = 14,
    LN_SUCC = 0, LN_PRED = 4, LN_TYPE = 8, LN_PRI = 9, LN_NAME = 10, NODE_SIZE = 14
};

static const UWORD FILLER = 99;

struct Fixture
{
    static constexpr ULONG gadtools = 0x3000, intuition = 0x2000, execbase = 0x1000;
    static constexpr ULONG stack = 0x3F00, raw = 0x3A00;
    static constexpr ULONG strings = 0x4000, newgadget = 0x5800, tags = 0x5900, more = 0x5A00;
    static constexpr ULONG textattr = 0x5B00, labels = 0x5B20, glist = 0x5B80, colors = 0x5C00;
    static constexpr ULONG screen = 0x5D00;
    static constexpr ULONG fillers = 0xF000;
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    int native_screen = 0;
    ULONG vi = 0, heap_avail = 0, next_string = strings;

    explicit Fixture(size_t mem_size = 0x10000, bool guarded = false)
        : memory(mem_size + (guarded ? KAFFEINE_MEM_GUARD : 0), 0xA5)
    {
        assert(native_blocks.empty());
        assert(!forbid_depth && !scheduler_hook);
        forbid_calls = permit_calls = forbid_peak = 0;
        allocations_before_failure = -1;
        native_calls = freed = 0;
        debug_lines.clear();
        free_gadget_calls.clear();
        gadget_calls.clear();
        gadget_events.clear();
        refresh_saw.clear();
        enable_saw = {};
        refresh_toggle_flags = 0;
        string_calls.clear();
        enable_list_saw.clear();
        watch_strings = change_strings = false;
        count_string_texts = false;
        string_allocation_end = nullptr;
        string_after = {};
        prop_calls.clear();
        modify_calls.clear();
        modify_list_saw.clear();
        watch_props = change_props = false;
        prop_after = {};
        open_result = nullptr;
        opened_tags.clear();
        closed_windows.clear();
        replied_messages.clear();
        assert(filtered_originals.empty());
        watched_windows.clear();
        layout_gadgets = remove_deselects = refresh_renders = false;
        activate_result = TRUE;
        intuition_active = nullptr;
        set_window = nullptr;
        held_by.clear();
        filtered = 0;
        filter_fails_once = false;
        filter_calls.clear();
        create_fails = context_fails = listview_fails_after_move = false;
        string_sets.clear();
        created_string.clear();
        visual_context = &ctx;
        visual_locks.clear(); visual_lock_denied = false; visual_list_depth = 0;
        ctx.mem = memory.data();
        ctx.mem_size = mem_size;
        if (guarded) std::fill(memory.begin() + mem_size, memory.end(), 0);
        ctx.heap_start = 0x6000;
        ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        auto &l = ctx.libs[0];
        l.m68k_addr = gadtools;
        l.jt_start = gadtools - 0xC00;
        std::strcpy(l.name, "gadtools.library");
        l.thunks = gadtools_manual;
        l.num_thunks = std::size(gadtools_manual);
        l.gen_thunks = gadtools_generated;
        l.num_gen_thunks = std::size(gadtools_generated);
        auto &i = ctx.libs[1];
        i.m68k_addr = intuition;
        i.jt_start = intuition - 0xC00;
        std::strcpy(i.name, "intuition.library");
        i.thunks = intuition_with_windows;
        i.num_thunks = std::size(intuition_with_windows);
        i.gen_thunks = intuition_generated;
        i.num_gen_thunks = std::size(intuition_generated);
        auto &e = ctx.libs[2];
        e.m68k_addr = execbase;
        e.jt_start = execbase - 0xC00;
        std::strcpy(e.name, "exec.library");
        e.thunks = exec_manual;
        e.num_thunks = std::size(exec_manual);
        ctx.num_libs = 3;
        disposed.clear();
        m68k_write32(&ctx, stack, 0x76543210);
        assert(Kaffeine_ShadowAdd(&ctx, screen, &native_screen, KAFFEINE_SHADOW_SCREEN));
        vi = call(126, { screen, 0 });
        assert(vi);
        heap_avail = Kaffeine_HeapAvail(&ctx, 0);
    }

    ~Fixture()
    {
        /* what a test left goes as at the end of a program */
        Kaffeine_GadgetsCleanup(&ctx);
        assert(natives.empty() && !ctx.gadgets && !ctx.classic);
        assert(filtered_originals.empty());
        assert(native_blocks.empty());
        assert(native_sizes.empty());
        assert(!forbid_depth && forbid_calls == permit_calls && !scheduler_hook);
        for (VisualInfo *v : visualinfos)
            delete v;
        visualinfos.clear();
    }

    /* A literal LVO through the dispatcher; only D0 may change. */
    ULONG call(ULONG lvo, std::initializer_list<ULONG> a = {}, std::initializer_list<ULONG> d = {})
    {
        return call_at(gadtools, lvo, a, d);
    }
    ULONG call_at(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
                  std::initializer_list<ULONG> d = {})
    {
        unsigned i;
        const unsigned nesting = forbid_depth;

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
        assert(forbid_depth == nesting && forbid_calls == permit_calls + nesting);
        return state.d[0];
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    void leave_shadows(UWORD n)
    {
        static int filler;
        for (ULONG a = fillers; ctx.num_shadows < KAFFEINE_MAX_SHADOWS - n; a += 4)
            assert(Kaffeine_ShadowAdd(&ctx, a, &filler, FILLER));
    }

    ULONG lng(ULONG addr) { return m68k_read32(&ctx, addr); }
    UWORD word(ULONG addr) { return m68k_read16(&ctx, addr); }
    std::string str(ULONG addr) { return reinterpret_cast<const char *>(memory.data() + addr); }
    bool guest(const void *p) const
    {
        auto b = static_cast<const UBYTE *>(p);
        return b >= memory.data() && b < memory.data() + ctx.mem_size;
    }
    ULONG at(const void *p) const { return (ULONG)(static_cast<const UBYTE *>(p) - memory.data()); }

    ULONG s(const char *text)
    {
        const ULONG a = next_string;
        std::strcpy(reinterpret_cast<char *>(memory.data() + a), text);
        next_string += std::strlen(text) + 1;
        return a;
    }
    void newgad(WORD left, WORD top, WORD width, WORD height, ULONG text, ULONG ta, UWORD id,
                ULONG flags, ULONG visualinfo, ULONG userdata)
    {
        m68k_write16(&ctx, newgadget + 0, (UWORD)left);
        m68k_write16(&ctx, newgadget + 2, (UWORD)top);
        m68k_write16(&ctx, newgadget + 4, (UWORD)width);
        m68k_write16(&ctx, newgadget + 6, (UWORD)height);
        m68k_write32(&ctx, newgadget + 8, text);
        m68k_write32(&ctx, newgadget + 12, ta);
        m68k_write16(&ctx, newgadget + 16, id);
        m68k_write32(&ctx, newgadget + 18, flags);
        m68k_write32(&ctx, newgadget + 22, visualinfo);
        m68k_write32(&ctx, newgadget + 26, userdata);
    }
    void tagitems(std::initializer_list<std::pair<ULONG, ULONG>> list, ULONG at_ = tags)
    {
        ULONG a = at_;
        for (auto &t : list)
        {
            m68k_write32(&ctx, a, t.first);
            m68k_write32(&ctx, a + 4, t.second);
            a += 8;
        }
        m68k_write32(&ctx, a, TAG_DONE);
    }
    ULONG context() { return call(114, { glist }); }
    ULONG create(ULONG kind, ULONG previous, ULONG taglist = tags)
    {
        return call(30, { previous, newgadget, taglist }, { kind });
    }
    void free(ULONG list) { call(36, { list }); }
    Native *native(ULONG guest_gadget)
    {
        auto *g = static_cast<Gadget *>(Kaffeine_ShadowNative(&ctx, guest_gadget, KAFFEINE_SHADOW_GADGET));
        auto it = natives.find(g);
        return it == natives.end() ? nullptr : it->second;
    }
    ULONG count(ULONG list)
    {
        ULONG n = 0;
        for (ULONG g = list; g; g = lng(g + GD_NEXTGADGET))
            ++n;
        return n;
    }

    /* Take all of the guest heap, and give it back. */
    std::vector<std::pair<ULONG, ULONG>> taken;
    void exhaust_heap(ULONG leave = 0)
    {
        for (ULONG size = 0x1000; size >= 8; size /= 2)
            while (Kaffeine_HeapAvail(&ctx, 0) >= size + leave)
            {
                ULONG block = Kaffeine_HeapAlloc(&ctx, size, MEMF_ANY);
                if (!block)
                    break;
                taken.push_back({ block, size });
            }
    }
    void release_heap()
    {
        for (auto &b : taken)
            Kaffeine_HeapFree(&ctx, b.first, b.second);
        taken.clear();
    }
};

static void test_public_screen_visualinfo()
{
    Fixture f;
    static int public_screen;
    const ULONG guest = 0x3A80;
    assert(Kaffeine_ShadowAdd(&f.ctx, guest, &public_screen, KAFFEINE_SHADOW_PUBSCREEN));
    const ULONG vi = f.call(126, { guest, 0 });
    auto *info = static_cast<VisualInfo *>(Kaffeine_VisualInfoNative(&f.ctx, vi));
    assert(vi && info && info->screen == reinterpret_cast<Screen *>(&public_screen));
    assert(f.call(132, { vi }) == 0 && !f.stopped());
    Kaffeine_ShadowForget(&f.ctx, guest, KAFFEINE_SHADOW_PUBSCREEN);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static TagItem *native_tag(Tag tag)
{
    for (TagItem &t : created_tags)
        if (t.ti_Tag == tag)
            return &t;
    return nullptr;
}

static unsigned native_count(Tag tag)
{
    unsigned n = 0;
    for (TagItem &t : created_tags)
        n += t.ti_Tag == tag;
    return n;
}

/* Guest lists deliberately use the 14-byte NDK layout, not host structs. */
static const ULONG ENTRIES = 0x5000, NODES = 0x5100;
static void guest_list(Fixture &f, unsigned count, ULONG list = ENTRIES, ULONG nodes = NODES, ULONG name = 0)
{
    assert(list + LIST_SIZE <= f.memory.size() && nodes + count * NODE_SIZE <= f.memory.size());
    m68k_write32(&f.ctx, list + LH_HEAD, count ? nodes : list + LH_TAIL);
    m68k_write32(&f.ctx, list + LH_TAIL, 0);
    m68k_write32(&f.ctx, list + LH_TAILPRED, count ? nodes + (count - 1) * NODE_SIZE : list);
    f.memory[list + 12] = NT_MESSAGE;
    f.memory[list + 13] = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const ULONG node = nodes + i * NODE_SIZE;
        m68k_write32(&f.ctx, node + LN_SUCC, i + 1 < count ? node + NODE_SIZE : list + LH_TAIL);
        m68k_write32(&f.ctx, node + LN_PRED, i ? node - NODE_SIZE : list);
        f.memory[node + LN_TYPE] = (UBYTE)(NT_MESSAGE + i % 3);
        f.memory[node + LN_PRI] = (UBYTE)(-(int)(i % 127) - 1);
        m68k_write32(&f.ctx, node + LN_NAME, name);
    }
}

/* Read the mirrors directly: a getter would settle them and hide a missing
   settle after the listview's creation or setter. */
static void assert_shown(Fixture &f, ULONG string, const char *text)
{
    Native *s = f.native(string);
    assert(s && s->kind == STRING_KIND);
    const ULONG si = f.lng(string + GD_SPECIALINFO), buffer = f.lng(si + SI_BUFFER);
    assert(si && buffer && std::strcmp((const char *)s->si.Buffer, text) == 0 && f.str(buffer) == text);
    assert(f.word(si + SI_MAXCHARS) == s->si.MaxChars);
    const size_t len = std::strlen(text);
    assert(len < (size_t)s->si.MaxChars && !s->si.Buffer[len] && !f.memory[buffer + len]);
}

static void assert_string_position(Fixture &f, ULONG string, WORD left, WORD top, WORD width, WORD height)
{
    const ExtGadget &s = f.native(string)->gad;
    assert(s.BoundsLeftEdge == left && s.BoundsTopEdge == top && s.BoundsWidth == width &&
           s.BoundsHeight == height && s.LeftEdge == left + 4 && s.TopEdge == top + 2 &&
           s.Width == width - 8 && s.Height == height - 4);
    assert((WORD)f.word(string + GD_BOUNDSLEFTEDGE) == left &&
           (WORD)f.word(string + GD_BOUNDSTOPEDGE) == top &&
           (WORD)f.word(string + GD_BOUNDSWIDTH) == width &&
           (WORD)f.word(string + GD_BOUNDSHEIGHT) == height &&
           (WORD)f.word(string + GD_LEFTEDGE) == left + 4 &&
           (WORD)f.word(string + GD_TOPEDGE) == top + 2 &&
           (WORD)f.word(string + GD_WIDTH) == width - 8 &&
           (WORD)f.word(string + GD_HEIGHT) == height - 4);
}

static void assert_string_sets(Fixture &f, ULONG string, std::initializer_list<const char *> names)
{
    assert(string_sets.size() == names.size());
    size_t i = 0;
    for (const char *name : names)
    {
        assert(string_sets[i].gadget == G(f.native(string)) && string_sets[i].text == name);
        ++i;
    }
}

static void test_listview_creation()
{
    Fixture f;
    const ULONG c = f.context(), alpha = f.s("Alpha"), omega = f.s("Omega");
    guest_list(f, 3);
    m68k_write32(&f.ctx, NODES + LN_NAME, alpha);
    m68k_write32(&f.ctx, NODES + 2 * NODE_SIZE + LN_NAME, omega);
    m68k_write32(&f.ctx, Fixture::textattr + TA_NAME, f.s("topaz.font"));
    m68k_write16(&f.ctx, Fixture::textattr + TA_YSIZE, 8);
    f.newgad(10, 20, 160, 100, f.s("Entries"), Fixture::textattr, 23, 0, f.vi, 0xCAFE0023);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 2 }, { GTLV_ReadOnly, TRUE },
                 { GTLV_MaxPen, 7 }, { GTLV_CallBack, 0 }, { GTLV_ShowSelected, 0 } });
    const unsigned calls = native_calls, sets = native_sets;
    const UWORD shadows = f.ctx.num_shadows;
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped() && native_calls == calls + 1 && native_sets == sets);
    Native *n = f.native(lv);
    assert(n && n->kind == LISTVIEW_KIND && n->num_entries == 3 && n->lv_top == 0 &&
           n->selected == 2 && n->itemheight == 8 && n->spacing == 0 && n->read_only && !n->show_selected);
    assert(native_tag(GTLV_MaxPen)->ti_Data == 7 && native_tag(GTLV_CallBack)->ti_Data == 0);
    List *list = n->entries;
    assert(list && !f.guest(list) && list == (List *)native_tag(GTLV_Labels)->ti_Data && !list->lh_Tail);
    APTR block = block_holding(list);
    assert(block);
    Node *prev = reinterpret_cast<Node *>(&list->lh_Head), *node;
    unsigned i = 0;
    ForeachNode(list, node)
    {
        assert(i < 3 && block_holding(node) == block && node->ln_Pred == prev);
        assert(node->ln_Type == f.memory[NODES + i * NODE_SIZE + LN_TYPE] &&
               node->ln_Pri == (BYTE)f.memory[NODES + i * NODE_SIZE + LN_PRI]);
        const ULONG name = f.lng(NODES + i * NODE_SIZE + LN_NAME);
        assert(name ? node->ln_Name == (char *)f.memory.data() + name : std::strcmp(node->ln_Name, "") == 0);
        prev = node;
        ++i;
    }
    assert(i == 3 && node == reinterpret_cast<Node *>(&list->lh_Tail) && list->lh_TailPred == prev);
    assert(f.lng(ENTRIES + LH_HEAD) == NODES && f.lng(NODES + LN_SUCC) == NODES + NODE_SIZE);
    render(n);

    /* creategadgeta: 100 - (8 - 4); makelistview: 96 - (90 % 8).
       Its bounds retain 96, while the actual listview height is 94. */
    assert(n->gad.Width == 142 && n->gad.Height == 94 && n->gad.BoundsWidth == 160 &&
           n->gad.BoundsHeight == 96 && num_items_fit(n) == 11 && shown_entries(n) == 3);
    const ULONG inc = f.lng(c + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    const ULONG sc = f.lng(dec + GD_NEXTGADGET), pi = f.lng(sc + GD_SPECIALINFO);
    assert(inc && dec && sc && pi && f.lng(sc + GD_NEXTGADGET) == lv && !f.lng(lv + GD_NEXTGADGET));
    assert(f.count(c) == 5 && natives.size() == 5 && f.ctx.num_shadows == shadows + 4);
    assert(f.native(sc) == n->scroller && f.native(sc)->part && f.native(sc)->kind == SCROLLER_KIND);
    assert(f.word(sc + GD_LEFTEDGE) == 155 && f.word(sc + GD_TOPEDGE) == 22 &&
           f.word(sc + GD_WIDTH) == 12 && f.word(sc + GD_HEIGHT) == 58);
    assert(f.word(inc + GD_TOPEDGE) == 98 && f.word(dec + GD_TOPEDGE) == 82);
    for (ULONG g : { inc, dec, sc, lv })
    {
        Native *part = f.native(g);
        assert(part && m68k_to_host_or_shadow(&f.ctx, g) == G(part) && f.word(g + GD_GADGETID) == 23);
        assert(f.lng(g + GD_USERDATA) == (g == lv ? 0xCAFE0023 : 0));
        assert(!f.lng(g + GD_GADGETRENDER) && !f.lng(g + GD_SELECTRENDER) && !f.lng(g + GD_MUTUALEXCLUDE));
        assert(f.lng(g + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP));
        if (g != sc)
            assert(!f.lng(g + GD_SPECIALINFO) && !part->gad.SpecialInfo);
    }
    for (ULONG a : { inc, dec })
    {
        assert(f.native(a)->part && f.native(a)->kind == BUTTON_KIND && f.word(a + GD_LEFTEDGE) == 153);
        assert(f.word(a + GD_WIDTH) == 16 && f.word(a + GD_HEIGHT) == 16);
        const ULONG image = f.lng(a + GD_GADGETTEXT);
        assert(image && f.word(image + IM_WIDTH) == 16 && f.word(image + IM_HEIGHT) == 16);
    }
    assert(f.str(f.lng(f.lng(lv + GD_GADGETTEXT) + IT_ITEXT)) == "Entries");
    assert(f.word(pi + PI_FLAGS) == (AUTOKNOB | FREEVERT) && f.word(pi + PI_VERTPOT) == 0 &&
           f.word(pi + PI_VERTBODY) == MAXBODY && n->scroller->total == 3 && n->scroller->visible == 3);
    f.free(c);
    assert(native_blocks.empty() && natives.empty() && freed == 5 && f.ctx.num_shadows == shadows - 1 &&
           Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_listview_lists()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    const size_t live = native_blocks.size();
    /* The canonical empty list can end at the guest memory boundary.
       Its tail sentinel has fewer bytes than a complete Node. */
    const ULONG empty = (ULONG)f.memory.size() - LIST_SIZE;
    guest_list(f, 0, empty);
    f.tagitems({ { GTLV_Labels, empty } });
    ULONG lv = f.create(LISTVIEW_KIND, c);
    Native *n = f.native(lv);
    assert(lv && !f.stopped() && n->num_entries == 0 && n->entries && !n->entries->lh_Tail &&
           n->entries->lh_Head == reinterpret_cast<Node *>(&n->entries->lh_Tail) &&
           n->entries->lh_TailPred == reinterpret_cast<Node *>(&n->entries->lh_Head));
    assert(num_items_fit(n) == 10 && shown_entries(n) == 0 &&
           n->scroller->top == 0 && n->scroller->total == 1 && n->scroller->visible == 1);
    f.free(f.lng(c + GD_NEXTGADGET));
    assert(native_blocks.size() == live);

    /* A zero-successor last node is excluded, even with an invalid name. */
    guest_list(f, 3);
    m68k_write32(&f.ctx, NODES + 2 * NODE_SIZE + LN_SUCC, 0);
    m68k_write32(&f.ctx, NODES + 2 * NODE_SIZE + LN_NAME, 0xFFFFFFFF);
    f.tagitems({ { GTLV_Labels, ENTRIES } });
    lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped() && f.native(lv)->num_entries == 2);
    render(f.native(lv));
    f.free(f.lng(c + GD_NEXTGADGET));

    /* A noncanonical terminator needs only its four successor bytes. */
    const ULONG terminal = (ULONG)f.memory.size() - 4;
    guest_list(f, 1);
    m68k_write32(&f.ctx, NODES + LN_SUCC, terminal);
    m68k_write32(&f.ctx, terminal, 0);
    lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped() && f.native(lv)->num_entries == 1);
    f.free(f.lng(c + GD_NEXTGADGET));

    for (ULONG labels : { 0u, 0xFFFFFFFFu })
    {
        f.tagitems({ { GTLV_Labels, labels }, { GTLV_ShowSelected, labels } });
        lv = f.create(LISTVIEW_KIND, c);
        assert(lv && !f.stopped());
        n = f.native(lv);
        assert((IPTR)n->entries == (labels ? ~(IPTR)0 : 0) && n->show_selected == (IPTR)n->entries &&
               n->num_entries == 0 && n->selected == -1 && !block_holding(n->entries));
        render(n);
        f.free(f.lng(c + GD_NEXTGADGET));
        assert(native_blocks.size() == live);
    }
    f.free(c);
    assert(native_blocks.empty());
}

static void test_listview_limits()
{
    Fixture f(0x90000);
    const ULONG c = f.context(), list = 0x10000, nodes = list + 16;
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    const size_t live = native_blocks.size();
    guest_list(f, 32767, list, nodes, f.s("Entry"));
    f.tagitems({ { GTLV_Labels, list } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped() && f.native(lv)->num_entries == 32767);
    List *copy = f.native(lv)->entries;
    assert(block_holding(copy) == block_holding(copy->lh_TailPred) &&
           std::strcmp(copy->lh_TailPred->ln_Name, "Entry") == 0);
    render(f.native(lv));
    f.free(f.lng(c + GD_NEXTGADGET));
    assert(native_blocks.size() == live);

    unsigned calls = native_calls;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    guest_list(f, 32768, list, nodes);
    f.create(LISTVIEW_KIND, c);
    assert(f.stopped() && native_calls == calls && native_blocks.size() == live &&
           Kaffeine_HeapAvail(&f.ctx, 0) == heap && !f.lng(c + GD_NEXTGADGET));
    guest_list(f, 3, list, nodes);
    m68k_write32(&f.ctx, nodes + 2 * NODE_SIZE + LN_SUCC, nodes);
    f.create(LISTVIEW_KIND, c);
    assert(f.stopped() && native_calls == calls && native_blocks.size() == live &&
           Kaffeine_HeapAvail(&f.ctx, 0) == heap && !f.lng(c + GD_NEXTGADGET));
}

static void test_listview_refused()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    const size_t live = native_blocks.size();
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    struct Case { const char *what; void (*setup)(Fixture &); };
    const Case cases[] =
    {
        { "List header outside", [](Fixture &x) { x.tagitems({ { GTLV_Labels, 0x10000 } }); } },
        { "List header crossing end", [](Fixture &x) { x.tagitems({ { GTLV_Labels, 0xFFF8 } }); } },
        { "Node outside", [](Fixture &x) { m68k_write32(&x.ctx, ENTRIES + LH_HEAD, 0x10000); } },
        { "Node crossing end", [](Fixture &x) {
              m68k_write32(&x.ctx, ENTRIES + LH_HEAD, 0xFFF8);
              m68k_write32(&x.ctx, 0xFFF8, ENTRIES + LH_TAIL); } },
        { "successor crossing end", [](Fixture &x) { m68k_write32(&x.ctx, NODES + LN_SUCC, 0xFFFE); } },
        { "name outside", [](Fixture &x) { m68k_write32(&x.ctx, NODES + LN_NAME, 0x10000); } },
        { "name without NUL", [](Fixture &x) {
              x.memory.back() = 'x';
              m68k_write32(&x.ctx, NODES + LN_NAME, 0xFFFF); } },
        { "ShowSelected non-gadget", [](Fixture &x) { x.tagitems({ { GTLV_ShowSelected, Fixture::raw } }); } },
        { "CallBack hook", [](Fixture &x) { x.tagitems({ { GTLV_CallBack, Fixture::raw } }); } },
        { "zero ScrollWidth", [](Fixture &x) { x.tagitems({ { GTLV_ScrollWidth, 0 } }); } },
        { "negative ScrollWidth", [](Fixture &x) { x.tagitems({ { GTLV_ScrollWidth, 0x8000 } }); } },
        { "zero ItemHeight and TextAttr", [](Fixture &x) {
              m68k_write32(&x.ctx, Fixture::textattr + TA_NAME, x.s("topaz.font"));
              m68k_write16(&x.ctx, Fixture::textattr + TA_YSIZE, 0);
              x.memory[Fixture::textattr + TA_STYLE] = FS_NORMAL;
              x.newgad(10, 20, 160, 100, 0, Fixture::textattr, 1, 0, x.vi, 0);
              x.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_ItemHeight, 0 } }); } },
        { "zero WORD ItemHeight", [](Fixture &x) { x.tagitems({ { GTLV_ItemHeight, 0x10000 } }); } },
        { "negative ItemHeight", [](Fixture &x) { x.tagitems({ { GTLV_ItemHeight, 0x8000 } }); } },
        { "negative Spacing", [](Fixture &x) {
              x.tagitems({ { GTLV_Labels, ENTRIES }, { LAYOUTA_Spacing, 0xFFFF } }); } },
    };
    for (const Case &k : cases)
    {
        guest_list(f, 1);
        f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
        f.tagitems({ { GTLV_Labels, ENTRIES } });
        k.setup(f);
        const unsigned calls = native_calls;
        f.create(LISTVIEW_KIND, c);
        if (!f.stopped() || native_calls != calls || native_blocks.size() != live ||
            Kaffeine_HeapAvail(&f.ctx, 0) != heap || f.lng(c + GD_NEXTGADGET))
        {
            std::printf("listview not refused cleanly: %s\n", k.what);
            assert(false);
        }
    }
}

static void test_listview_tags()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    /* Invalid earlier values must not be converted. makelistview uses
       the last occurrence, even for pointers and narrowed dimensions. */
    f.tagitems({ { GTLV_Labels, 0xDEADBEEF }, { GTLV_Labels, ENTRIES },
                 { GTLV_ShowSelected, Fixture::raw }, { GTLV_ShowSelected, 0xFFFFFFFF },
                 { GTLV_CallBack, Fixture::raw }, { GTLV_CallBack, 0 },
                 { GTLV_ScrollWidth, 0x8000 }, { GTLV_ScrollWidth, 1 },
                 { GTLV_ItemHeight, 0x8000 }, { GTLV_ItemHeight, 0x10005 },
                 { LAYOUTA_Spacing, 0xFFFF }, { LAYOUTA_Spacing, 2 },
                 { GTLV_Selected, 0xFFFFFFFF }, { GTLV_Selected, 7 },
                 { GTLV_ReadOnly, 0 }, { GTLV_ReadOnly, 1 },
                 { GTLV_MaxPen, 2 }, { GTLV_MaxPen, 5 },
                 { GA_Disabled, 0 }, { GA_Disabled, 1 },
                 { GTLV_Top, 1 }, { GTLV_Top, 5 },
                 { GTLV_MakeVisible, 0x8000 }, { GTLV_MakeVisible, 19 } });
    const unsigned sets = native_sets;
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped() && native_sets == sets + 1);
    Native *n = f.native(lv);
    assert(n->itemheight == 5 && n->spacing == 2 && n->selected == 7 && n->read_only &&
           n->show_selected == ~(IPTR)0 && n->num_entries == 20 && n->gad.Width == 157);
    assert(n->gad.Height == 90 && num_items_fit(n) == 12 && n->lv_top == 8);
    for (Tag tag : { GTLV_Labels, GTLV_ShowSelected, GTLV_CallBack, GTLV_ScrollWidth, GTLV_ItemHeight,
                     LAYOUTA_Spacing, GTLV_Selected, GTLV_ReadOnly, GTLV_MaxPen,
                     GA_Disabled, GTLV_Top, GTLV_MakeVisible })
        assert(native_count(tag) == 1);
    assert(native_tag(GTLV_ItemHeight)->ti_Data == 0x10005 && native_tag(GTLV_MaxPen)->ti_Data == 5);
    assert(set_tags.size() == 3 && set_tags[0].ti_Tag == GA_Disabled && set_tags[0].ti_Data == 1 &&
           set_tags[1].ti_Tag == GTLV_Top && set_tags[1].ti_Data == 5 &&
           set_tags[2].ti_Tag == GTLV_MakeVisible && set_tags[2].ti_Data == 19);
}

static void test_listview_replay()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    /* Each replay is independently observable; default MakeVisible=0 in
       the builder must not be replayed when only Top was supplied. */
    const struct { Tag tag; ULONG value; WORD top; } cases[] =
        { { GTLV_Top, 5, 5 }, { GTLV_MakeVisible, 19, 10 }, { GA_Disabled, 1, 0 },
          { GTLV_Top, 0, 0 }, { GTLV_MakeVisible, 0, 0 }, { GA_Disabled, 0, 0 } };
    for (const auto &k : cases)
    {
        f.tagitems({ { GTLV_Labels, ENTRIES }, { k.tag, k.value } });
        const unsigned sets = native_sets, calls = native_calls;
        const ULONG lv = f.create(LISTVIEW_KIND, c);
        Native *n = f.native(lv);
        assert(lv && !f.stopped() && native_sets == sets + 1 && native_calls == calls + 2 &&
               set_tags.size() == 1 && set_tags[0].ti_Tag == k.tag && n->lv_top == k.top);
        const ULONG inc = f.lng(c + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
        const ULONG sc = f.lng(dec + GD_NEXTGADGET), pi = f.lng(sc + GD_SPECIALINFO);
        for (ULONG g : { lv, sc, inc, dec })
            assert(!!(f.native(g)->gad.Flags & GFLG_DISABLED) == (k.tag == GA_Disabled && k.value) &&
                   !!(f.word(g + GD_FLAGS) & GFLG_DISABLED) == (k.tag == GA_Disabled && k.value));
        assert(n->scroller->top == k.top && n->scroller->total == 20 && n->scroller->visible == 10 &&
               f.word(pi + PI_VERTPOT) == (UWORD)((ULONG)k.top * MAXPOT / 10) &&
               f.word(pi + PI_VERTBODY) == MAXBODY / 2);
        f.free(f.lng(c + GD_NEXTGADGET));
    }
}

static void test_listview_failures()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES } });
    const auto live = native_blocks;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;
    /* The batch, conversion records, native tags and snapshot allocation. */
    for (int fail = 0; fail < 4; ++fail)
    {
        const unsigned calls = native_calls, allocations = native_allocations;
        allocations_before_failure = fail;
        assert(f.create(LISTVIEW_KIND, c) == 0 && !f.stopped());
        assert(allocations_before_failure == -1 && native_allocations == allocations + fail &&
               native_calls == calls && native_blocks == live && natives.size() == 1 &&
               f.ctx.num_shadows == shadows && !f.lng(c + GD_NEXTGADGET) &&
               Kaffeine_HeapAvail(&f.ctx, 0) == heap);
    }
    create_fails = true;
    unsigned calls = native_calls;
    assert(f.create(LISTVIEW_KIND, c) == 0 && !f.stopped() && native_calls == calls + 1 &&
           native_blocks == live && natives.size() == 1 && !f.lng(c + GD_NEXTGADGET));
    create_fails = false;
    /* Native creation succeeds, then a failed mirror must dispose all four
       gadgets before releasing the List that render() will still read. */
    f.exhaust_heap();
    calls = native_calls;
    unsigned before = freed;
    assert(f.create(LISTVIEW_KIND, c) == 0 && !f.stopped() && native_calls == calls + 2 &&
           freed == before + 4 && native_blocks == live && natives.size() == 1 &&
           f.ctx.num_shadows == shadows && !f.lng(c + GD_NEXTGADGET));
    f.release_heap();
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == heap);
    f.leave_shadows(3);
    const UWORD full = f.ctx.num_shadows;
    calls = native_calls;
    before = freed;
    assert(f.create(LISTVIEW_KIND, c) == 0 && !f.stopped() && native_calls == calls + 2 &&
           freed == before + 4 && native_blocks == live && natives.size() == 1 &&
           f.ctx.num_shadows == full && !f.lng(c + GD_NEXTGADGET) &&
           Kaffeine_HeapAvail(&f.ctx, 0) == heap);
}

static void test_listview_showsel_creation()
{
    const struct { ULONG selected; const char *text; } cases[] =
        { { 0, "Alpha" }, { 1, "Beta" }, { 2, "Longe" }, { 3, "" }, { 0xFFFFFFFF, "" } };
    for (const auto &k : cases)
    {
        Fixture f;
        const ULONG c = f.context();
        f.newgad(-30, 7, 160, 14, f.s("Name"), 0, 2, 0, f.vi, 0xCAFE0002);
        f.tagitems({ { GTST_String, f.s("Seed") }, { GTST_MaxChars, 5 } });
        const ULONG string = f.create(STRING_KIND, c);
        assert(string && !f.stopped());
        const ULONG si = f.lng(string + GD_SPECIALINFO), buffer = f.lng(si + SI_BUFFER);
        assert_shown(f, string, "Seed");
        assert_string_position(f, string, -30, 7, 160, 14);
        /* ShowSelected can name an earlier batch, not just previous. */
        f.newgad(10, 5, 80, 14, 0, 0, 3, 0, f.vi, 0);
        f.tagitems({});
        const ULONG button = f.create(BUTTON_KIND, string);
        guest_list(f, 3, ENTRIES, NODES, f.s("Alpha"));
        m68k_write32(&f.ctx, NODES + NODE_SIZE + LN_NAME, f.s("Beta"));
        m68k_write32(&f.ctx, NODES + 2 * NODE_SIZE + LN_NAME, f.s("Longer than MaxChars"));
        f.newgad(10, 20, 160, 100, 0, 0, 4, 0, f.vi, 0);
        /* The builder puts Labels before Selected even in this order. */
        f.tagitems({ { GTLV_Selected, k.selected }, { GTLV_ShowSelected, string },
                     { GTLV_Labels, ENTRIES } });
        const unsigned calls = native_calls, sets = native_sets;
        const ULONG lv = f.create(LISTVIEW_KIND, button);
        assert(lv && !f.stopped() && f.native(lv)->show_selected == (IPTR)G(f.native(string)) &&
               native_tag(GTLV_ShowSelected)->ti_Data == (IPTR)G(f.native(string)));
        assert(native_calls == calls + (k.selected == 0 ? 2 : 3) &&
               native_sets == sets + (k.selected == 0 ? 1 : 2));
        if (k.selected == 0)
            assert_string_sets(f, string, { "Alpha" });
        else
            assert_string_sets(f, string, { "Alpha", k.selected == 2 ? "Longer than MaxChars" : k.text });
        assert_shown(f, string, k.text);
        /* 100 - 7 correction - 14 string = 79, then round to 78.
           The string is placed before rounding, at 20 + 79. */
        assert_string_position(f, string, 10, 99, 160, 14);
        assert(f.word(lv + GD_HEIGHT) == 78 && f.word(lv + GD_BOUNDSHEIGHT) == 93 &&
               f.native(lv)->gad.Height == 78 && f.native(lv)->gad.BoundsHeight == 93);
        assert(f.lng(string + GD_SPECIALINFO) == si && f.lng(si + SI_BUFFER) == buffer &&
               f.word(si + SI_MAXCHARS) == 6 && f.lng(string + GD_USERDATA) == 0xCAFE0002 &&
               f.lng(c + GD_NEXTGADGET) == string && f.lng(string + GD_NEXTGADGET) == button &&
               f.count(c) == 7);
        f.free(c);
        assert(!f.stopped() && natives.empty() && native_blocks.empty() && freed == 7 &&
               Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_listview_showsel_failures()
{
    enum Failure { DIMENSIONS, BEFORE_MOVE, AFTER_MOVE, GUEST_HEAP, SHADOWS };
    const struct { WORD width, height; Failure failure; } cases[] =
    {
        { 159, 14, DIMENSIONS }, { 161, 14, DIMENSIONS },
        { 160, 93, DIMENSIONS }, { 160, 94, DIMENSIONS },
        { 160, 14, BEFORE_MOVE }, { 160, 14, AFTER_MOVE },
        { 160, 14, GUEST_HEAP }, { 160, 14, SHADOWS }
    };
    for (const auto &k : cases)
    {
        Fixture f;
        const ULONG c = f.context();
        f.newgad(-30, 7, k.width, k.height, 0, 0, 2, 0, f.vi, 0);
        f.tagitems({ { GTST_String, f.s("Seed") }, { GTST_MaxChars, 5 } });
        const ULONG string = f.create(STRING_KIND, c);
        assert(string && !f.stopped());
        guest_list(f, 2, ENTRIES, NODES, f.s("Alpha"));
        m68k_write32(&f.ctx, NODES + NODE_SIZE + LN_NAME, f.s("Beta"));
        f.newgad(10, 20, 160, 100, 0, 0, 3, 0, f.vi, 0);
        f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 1 }, { GTLV_ShowSelected, string } });
        create_fails = k.failure == BEFORE_MOVE;
        listview_fails_after_move = k.failure == AFTER_MOVE;
        if (k.failure == GUEST_HEAP)
            f.exhaust_heap();
        if (k.failure == SHADOWS)
            f.leave_shadows(3);
        const auto live = native_blocks;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        const UWORD shadows = f.ctx.num_shadows;
        const unsigned calls = native_calls, sets = native_sets, before = freed;
        assert(f.create(LISTVIEW_KIND, string) == 0 && !f.stopped());
        const bool built = k.failure == GUEST_HEAP || k.failure == SHADOWS;
        assert(native_calls == calls + (built ? 4 : 1) && native_sets == sets + (built ? 2 : 0) &&
               freed == before + (built ? 4 : 0) && native_blocks == live && natives.size() == 2 &&
               f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap &&
               f.lng(c + GD_NEXTGADGET) == string && !f.lng(string + GD_NEXTGADGET) &&
               !G(f.native(string))->NextGadget);
        const bool moved = built || k.failure == AFTER_MOVE;
        assert_string_position(f, string, moved ? 10 : -30, moved ? 99 : 7, k.width, k.height);
        assert_shown(f, string, built ? "Beta" : "Seed");
        if (built)
            assert_string_sets(f, string, { "Alpha", "Beta" });
        else
            assert(string_sets.empty());
        f.release_heap();
        /* No failed listview may leave a dependency on the string. */
        f.free(string);
        assert(!f.stopped() && natives.size() == 1 && f.native(c) && !f.lng(c + GD_NEXTGADGET));
        f.free(c);
        assert(!f.stopped() && natives.empty() && native_blocks.empty() &&
               Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_listview_showsel_refused()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 20, 160, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTSC_Arrows, 8 } });
    const ULONG scroller = f.create(SCROLLER_KIND, button), arrow = f.lng(button + GD_NEXTGADGET);
    f.tagitems({ { GTIN_Number, 12 } });
    const ULONG integer = f.create(INTEGER_KIND, scroller);
    assert(button && scroller && arrow && integer && !f.stopped() && f.native(arrow)->part);
    guest_list(f, 1, ENTRIES, NODES, f.s("Entry"));
    const auto live = native_blocks;
    const auto live_natives = natives;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;
    const unsigned calls = native_calls;
    for (ULONG bad : { button, arrow, scroller, integer, c, Fixture::raw,
                       f.lng(integer + GD_SPECIALINFO), 0x10000u })
    {
        f.newgad(10, 20, 160, 100, 0, 0, 3, 0, f.vi, 0);
        f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_ShowSelected, bad } });
        f.create(LISTVIEW_KIND, integer);
        assert(f.stopped() && native_calls == calls && native_blocks == live && natives == live_natives &&
               f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap &&
               !f.lng(integer + GD_NEXTGADGET) && !G(f.native(integer))->NextGadget &&
               f.lng(button + GD_NEXTGADGET) == arrow && freed == 0);
    }
    f.free(c);
    assert(!f.stopped() && natives.empty() && native_blocks.empty());
}

static void test_registration()
{
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } gt[] =
        { { 30, Kaffeine_CreateGadgetA }, { 36, Kaffeine_FreeGadgets },
          { 42, Kaffeine_GT_SetGadgetAttrsA }, { 114, Kaffeine_CreateGadgetContext },
          { 174, Kaffeine_GT_GetGadgetAttrsA } };
    const M68KThunkEntry gadgets[] = { KAFFEINE_GADTOOLS_GADGET_THUNKS };

    assert(std::size(gadgets) == std::size(gt));
    for (unsigned i = 0; i < std::size(gt); ++i)
        assert(gadgets[i].lvo == gt[i].lvo && gadgets[i].thunk == gt[i].thunk);
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } in[] =
        { { 42, Kaffeine_AddGadget }, { 156, Kaffeine_ModifyProp },
          { 174, Kaffeine_OffGadget }, { 186, Kaffeine_OnGadget },
          { 222, Kaffeine_RefreshGadgets }, { 228, Kaffeine_RemoveGadget },
          { 432, Kaffeine_RefreshGList }, { 438, Kaffeine_AddGList }, { 444, Kaffeine_RemoveGList },
          { 462, Kaffeine_ActivateGadget }, { 468, Kaffeine_NewModifyProp }, { 492, Kaffeine_SetEditHook },
          { 642, Kaffeine_DisposeObject },
          { 648, Kaffeine_SetAttrsA }, { 654, Kaffeine_GetAttr },
          { 660, Kaffeine_SetGadgetAttrsA }, { 810, Kaffeine_DoGadgetMethodA } };
    assert(std::size(intuition_manual) == std::size(in));
    for (unsigned i = 0; i < std::size(in); ++i)
        assert(intuition_manual[i].lvo == in[i].lvo && intuition_manual[i].thunk == in[i].thunk);
}

static void test_context()
{
    Fixture f;

    const ULONG c = f.context();
    assert(c && !f.stopped() && f.lng(Fixture::glist) == c && natives.size() == 1);
    Native *n = f.native(c);
    assert(n && n->context);
    assert(f.lng(c + GD_NEXTGADGET) == 0 && f.word(c + GD_FLAGS) == (GFLG_GADGHNONE | GFLG_DISABLED) &&
           f.word(c + GD_GADGETTYPE) == GTYP_GADTOOLS && f.lng(c + GD_USERDATA) == 0 &&
           f.lng(c + GD_GADGETTEXT) == 0 && f.lng(c + GD_SPECIALINFO) == 0 &&
           f.lng(c + GD_MOREFLAGS) == 0);
    /* a shadow, so that the generated thunks of Intuition find the native gadget */
    assert(m68k_to_host_or_shadow(&f.ctx, c) == G(n));
    f.free(c);
    assert(natives.empty() && freed == 1 && !f.stopped() && !f.ctx.gadgets);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    assert(!Kaffeine_ShadowNative(&f.ctx, c, KAFFEINE_SHADOW_GADGET));
    /* freeing it again does nothing, where GadTools would crash */
    unsigned calls = native_calls;
    f.free(c);
    assert(!f.stopped() && native_calls == calls);

    /* a gadget pointer outside guest memory stops the program before
       GadTools */
    f.call(114, { 0 });
    assert(f.stopped());
    f.call(114, { 0xFFFE });
    assert(f.stopped() && native_calls == calls);

    /* GadTools without memory: 0 in both places */
    context_fails = true;
    m68k_write32(&f.ctx, Fixture::glist, 0xDEADBEEF);
    assert(f.context() == 0 && f.lng(Fixture::glist) == 0 && !f.stopped());
    context_fails = false;

    /* without guest memory or room for the shadow, the native context goes
       again */
    f.exhaust_heap();
    m68k_write32(&f.ctx, Fixture::glist, 0xDEADBEEF);
    assert(f.context() == 0 && f.lng(Fixture::glist) == 0 && natives.empty() && !f.stopped());
    f.release_heap();
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    f.leave_shadows(0);
    calls = f.ctx.num_shadows;
    assert(f.context() == 0 && natives.empty() && f.ctx.num_shadows == calls && !f.ctx.gadgets);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* A button with a label and a font of the program; the tags go through
   TAG_MORE, TAG_SKIP and TAG_IGNORE, and only those that GadTools reads for
   a button reach it. */
static void test_button()
{
    Fixture f;
    const ULONG c = f.context();
    const ULONG text = f.s("_Ok"), font = f.s("helvetica.font");
    m68k_write32(&f.ctx, Fixture::textattr + TA_NAME, font);
    m68k_write16(&f.ctx, Fixture::textattr + TA_YSIZE, 13);
    f.newgad(10, 20, 80, 14, text, Fixture::textattr, 7, PLACETEXT_IN | NG_HIGHLABEL, f.vi, 0xCAFE0001);
    f.tagitems({ { GT_Underscore, '_' }, { GTCY_Labels, 0xDEADBEEF }, { TAG_MORE, Fixture::more } });
    f.tagitems({ { TAG_IGNORE, 0xDEADBEEF }, { TAG_SKIP, 1 }, { GA_Disabled, 0xDEADBEEF },
                 { GA_Disabled, 1 }, { GA_Immediate, 0xFFFFFFFF }, { GTST_EditHook, 0xDEADBEEF } },
               Fixture::more);
    const ULONG b = f.create(BUTTON_KIND, c);
    assert(b && !f.stopped() && created_kind == BUTTON_KIND);

    /* the NewGadget as native GadTools gets it */
    assert(created_ng.ng_LeftEdge == 10 && created_ng.ng_TopEdge == 20 && created_ng.ng_Width == 80 &&
           created_ng.ng_Height == 14 && created_ng.ng_GadgetID == 7 &&
           created_ng.ng_Flags == (PLACETEXT_IN | NG_HIGHLABEL) &&
           created_ng.ng_UserData == (APTR)(IPTR)0xCAFE0001);
    assert(f.guest(created_ng.ng_GadgetText) && f.at(created_ng.ng_GadgetText) == text);
    assert(static_cast<VisualInfo *>(created_ng.ng_VisualInfo)->screen ==
           reinterpret_cast<Screen *>(&f.native_screen));
    /* a native copy of the TextAttr, with the program's name */
    TextAttr *ta = created_ng.ng_TextAttr;
    assert(ta && !f.guest(ta) && f.guest(ta->ta_Name) && f.at(ta->ta_Name) == font && ta->ta_YSize == 13);
    /* GT_Underscore, then GA_Disabled and GA_Immediate in their order, the
       skipped one left out, sign-extended as LONGs */
    assert(created_tags.size() == 4 && created_tags[0].ti_Tag == GT_Underscore &&
           created_tags[0].ti_Data == '_' && created_tags[1].ti_Tag == GA_Disabled &&
           created_tags[1].ti_Data == 1 && created_tags[2].ti_Tag == GA_Immediate &&
           created_tags[2].ti_Data == (IPTR)(SIPTR)-1 && created_tags[3].ti_Tag == TAG_DONE);

    /* the 68k gadget */
    Native *n = f.native(b);
    assert(n && n->kind == BUTTON_KIND && f.lng(c + GD_NEXTGADGET) == b && f.lng(b + GD_NEXTGADGET) == 0);
    assert(f.word(b + GD_LEFTEDGE) == 10 && f.word(b + GD_TOPEDGE) == 20 && f.word(b + GD_WIDTH) == 80 &&
           f.word(b + GD_HEIGHT) == 14);
    assert(f.word(b + GD_FLAGS) == (GFLG_EXTENDED | GFLG_DISABLED) &&
           f.word(b + GD_ACTIVATION) == (GACT_RELVERIFY | GACT_IMMEDIATE) &&
           f.word(b + GD_GADGETTYPE) == (GTYP_CUSTOMGADGET | GTYP_GADTOOLS));
    assert(f.word(b + GD_GADGETID) == 7 && f.lng(b + GD_USERDATA) == 0xCAFE0001);
    /* no native pointers: render images, the dispatcher in MutualExclude,
       nor SpecialInfo for a button */
    assert(n->gad.MutualExclude && f.lng(b + GD_MUTUALEXCLUDE) == 0);
    assert(f.lng(b + GD_GADGETRENDER) == 0 && f.lng(b + GD_SELECTRENDER) == 0 &&
           f.lng(b + GD_SPECIALINFO) == 0);
    assert(f.lng(b + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP) &&
           f.word(b + GD_BOUNDSLEFTEDGE) == 10 && f.word(b + GD_BOUNDSTOPEDGE) == 20 &&
           f.word(b + GD_BOUNDSWIDTH) == 80 && f.word(b + GD_BOUNDSHEIGHT) == 14);
    /* the label: "Ok", and the O again in an underlined copy of the font
       that GadTools copied */
    const ULONG it = f.lng(b + GD_GADGETTEXT), under = f.lng(it + IT_NEXTTEXT);
    assert(it && f.str(f.lng(it + IT_ITEXT)) == "Ok" && f.memory[it + IT_FRONTPEN] == 2 &&
           f.memory[it + IT_DRAWMODE] == JAM1 && (WORD)f.word(it + IT_LEFTEDGE) == -40);
    assert(under && f.str(f.lng(under + IT_ITEXT)) == "O" && f.lng(under + IT_NEXTTEXT) == 0);
    const ULONG gta = f.lng(it + IT_ITEXTFONT), uta = f.lng(under + IT_ITEXTFONT);
    assert(gta && gta != Fixture::textattr && f.str(f.lng(gta + TA_NAME)) == "helvetica.font" &&
           f.word(gta + TA_YSIZE) == 13 && f.memory[gta + TA_STYLE] == FS_NORMAL);
    assert(uta && uta != gta && f.str(f.lng(uta + TA_NAME)) == "helvetica.font" &&
           f.memory[uta + TA_STYLE] == FSF_UNDERLINED);
    assert(m68k_to_host_or_shadow(&f.ctx, b) == G(n));

    /* a NULL TextAttr gives none, and no label no IntuiText */
    f.newgad(10, 40, 80, 14, 0, 0, 8, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b2 = f.create(BUTTON_KIND, b);
    assert(b2 && created_ng.ng_TextAttr == nullptr && created_ng.ng_GadgetText == nullptr);
    assert(f.lng(b + GD_NEXTGADGET) == b2 && f.lng(b2 + GD_GADGETTEXT) == 0 && created_tags.size() == 1);

    /* a tag list of NULL is no tag */
    f.newgad(10, 60, 80, 14, 0, 0, 9, 0, f.vi, 0);
    assert(f.create(BUTTON_KIND, b2, 0) && created_tags.size() == 1);
    f.free(c);
    assert(natives.empty() && freed == 4 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* The other kinds of one gadget: checkbox, cycle, mx, palette, text,
   number, generic */
static void test_simple_kinds()
{
    Fixture f;
    const ULONG c = f.context();
    ULONG prev = c;

    /* checkbox: GadTools sets the size and GFLG_SELECTED */
    f.newgad(10, 10, 40, 40, 0, 0, 1, 0, f.vi, 1);
    f.tagitems({ { GTCB_Checked, 1 }, { GA_Disabled, 0 } });
    const ULONG cb = f.create(CHECKBOX_KIND, prev);
    assert(cb && f.word(cb + GD_WIDTH) == 26 && f.word(cb + GD_HEIGHT) == 11 &&
           (f.word(cb + GD_FLAGS) & GFLG_SELECTED) && f.lng(cb + GD_USERDATA) == 1);
    prev = cb;

    /* cycle: a native array of the program's strings, kept after the call */
    const ULONG one = f.s("One"), two = f.s("Two"), three = f.s("Three");
    m68k_write32(&f.ctx, Fixture::labels, one);
    m68k_write32(&f.ctx, Fixture::labels + 4, two);
    m68k_write32(&f.ctx, Fixture::labels + 8, three);
    m68k_write32(&f.ctx, Fixture::labels + 12, 0);
    f.newgad(10, 30, 100, 14, f.s("Mode"), 0, 2, PLACETEXT_LEFT, f.vi, 2);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Active, 2 } });
    const ULONG cy = f.create(CYCLE_KIND, prev);
    assert(cy && !f.stopped());
    auto *lab = (STRPTR *)native_tag(GTCY_Labels)->ti_Data;
    assert(lab && !f.guest(lab) && f.at(lab[0]) == one && f.at(lab[1]) == two && f.at(lab[2]) == three &&
           lab[3] == nullptr && native_tag(GTCY_Active)->ti_Data == 2);
    assert(f.native(cy)->labels == lab && f.str(f.lng(f.lng(cy + GD_GADGETTEXT) + IT_ITEXT)) == "Mode");
    prev = cy;

    /* mx with its own labels */
    m68k_write32(&f.ctx, Fixture::labels + 16, three);
    m68k_write32(&f.ctx, Fixture::labels + 20, 0);
    f.newgad(10, 50, 100, 30, 0, 0, 3, 0, f.vi, 3);
    f.tagitems({ { GTMX_Labels, Fixture::labels + 16 }, { GTMX_Spacing, 4 } });
    const ULONG mx = f.create(MX_KIND, prev);
    lab = (STRPTR *)native_tag(GTMX_Labels)->ti_Data;
    assert(mx && f.at(lab[0]) == three && lab[1] == nullptr && f.word(mx + GD_WIDTH) == 17);
    prev = mx;

    /* palette: the colour table of the program, for the colours that
       GadTools works out, the last of GTPA_Depth and GTPA_NumColors */
    for (ULONG i = 0; i < 8; ++i)
        f.memory[Fixture::colors + i] = (UBYTE)(7 - i);
    f.newgad(10, 90, 100, 20, 0, 0, 4, 0, f.vi, 4);
    f.tagitems({ { GTPA_NumColors, 64 }, { GTPA_Depth, 3 }, { GTPA_ColorTable, Fixture::colors },
                 { GTPA_Color, 5 } });
    const ULONG pa = f.create(PALETTE_KIND, prev);
    assert(pa && f.guest(f.native(pa)->colors) && f.at(f.native(pa)->colors) == Fixture::colors &&
           f.native(pa)->numcolors == 8);
    prev = pa;

    /* text: the program's string, which the class keeps */
    const ULONG hello = f.s("Hello");
    f.newgad(10, 120, 100, 14, 0, 0, 5, 0, f.vi, 5);
    f.tagitems({ { GTTX_Text, hello }, { GTTX_Border, 1 }, { GTTX_FrontPen, 3 } });
    const ULONG tx = f.create(TEXT_KIND, prev);
    assert(tx && f.at(f.native(tx)->text) == hello && native_tag(GTNM_FrontPen)->ti_Data == 3);
    prev = tx;

    /* number: the format is checked and copied */
    const ULONG fmt = f.s("%3ld%%");
    f.newgad(10, 140, 100, 14, 0, 0, 6, 0, f.vi, 6);
    f.tagitems({ { GTNM_Number, 0xFFFFFFFF }, { GTNM_Format, fmt } });
    const ULONG nm = f.create(NUMBER_KIND, prev);
    assert(nm && native_tag(GTNM_Number)->ti_Data == (IPTR)(SIPTR)-1);
    assert(f.native(nm)->format && !f.guest(f.native(nm)->format) &&
           std::string(f.native(nm)->format) == "%3ld%%");
    prev = nm;

    /* generic: a plain gadget of GadTools, which takes no tags */
    f.newgad(200, 10, 50, 50, 0, 0, 7, 0, f.vi, 7);
    f.tagitems({ { GA_Disabled, 1 }, { GT_Underscore, '_' } });
    const ULONG ge = f.create(GENERIC_KIND, prev);
    assert(ge && created_tags.size() == 2 && created_tags[0].ti_Tag == GT_Underscore);
    assert(f.word(ge + GD_FLAGS) == GFLG_EXTENDED && f.word(ge + GD_GADGETTYPE) == GTYP_GADTOOLS &&
           f.word(ge + GD_ACTIVATION) == 0 && f.lng(ge + GD_USERDATA) == 7 &&
           f.lng(ge + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP));

    /* the program changes its labels array: the native copy stays */
    m68k_write32(&f.ctx, Fixture::labels, 0);
    assert(f.count(c) == 8 && natives.size() == 8);
    f.free(c);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* Slider and scroller: the gadgets that GadTools puts before them, and a
   68k PropInfo */
static void test_compound_kinds()
{
    Fixture f;
    const ULONG c = f.context();

    /* a slider with a level display: previous -> level -> slider */
    const ULONG lfmt = f.s("%2ld");
    f.newgad(50, 10, 100, 12, f.s("Volume"), 0, 11, 0, f.vi, 0x1111);
    f.tagitems({ { GTSL_Min, (ULONG)-10 }, { GTSL_Max, 10 }, { GTSL_Level, 3 },
                 { GTSL_LevelFormat, lfmt }, { GTSL_MaxLevelLen, 3 }, { GTSL_DispFunc, 0 } });
    const ULONG sl = f.create(SLIDER_KIND, c);
    assert(sl && !f.stopped() && native_tag(GTSL_Min)->ti_Data == (IPTR)(SIPTR)-10);
    const ULONG level = f.lng(c + GD_NEXTGADGET);
    assert(level && level != sl && f.lng(level + GD_NEXTGADGET) == sl && f.lng(sl + GD_NEXTGADGET) == 0);
    /* the level display has no label, no UserData, no SpecialInfo */
    assert(f.lng(level + GD_GADGETTEXT) == 0 && f.lng(level + GD_USERDATA) == 0 &&
           f.lng(level + GD_SPECIALINFO) == 0 && f.native(level)->kind == TEXT_KIND);
    assert(std::string(f.native(level)->format) == "%2ld" && !f.guest(f.native(level)->format));
    /* the slider has the label, the UserData and a 68k PropInfo */
    const ULONG pi = f.lng(sl + GD_SPECIALINFO);
    assert(pi && f.lng(sl + GD_USERDATA) == 0x1111 && f.word(sl + GD_GADGETID) == 11);
    assert(f.word(pi + PI_FLAGS) == (AUTOKNOB | FREEHORIZ) && f.word(pi + PI_HORIZPOT) == 0x4000 &&
           f.word(pi + PI_HORIZBODY) == 0x1000 && f.word(pi + PI_VERTBODY) == 0xFFFF &&
           f.word(pi + PI_CWIDTH) == 96 && f.word(pi + PI_HPOTRES) == 0x100 && f.word(pi + PI_TOPBORDER) == 1);
    assert(f.str(f.lng(f.lng(sl + GD_GADGETTEXT) + IT_ITEXT)) == "Volume");

    /* a vertical scroller with arrows: previous -> increment -> decrement
       -> prop; the arrows have the ID, an image label and no UserData */
    f.newgad(200, 10, 16, 100, 0, 0, 12, 0, f.vi, 0x2222);
    f.tagitems({ { GTSC_Arrows, 8 }, { PGA_Freedom, LORIENT_VERT }, { GTSC_Total, 50 },
                 { GTSC_Visible, 10 }, { GTSC_Overlap, 0xDEADBEEF } });
    const ULONG sc = f.create(SCROLLER_KIND, sl);
    const ULONG inc = f.lng(sl + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    assert(sc && inc && dec && f.lng(dec + GD_NEXTGADGET) == sc && f.lng(sc + GD_NEXTGADGET) == 0);
    assert(!native_tag(GTSC_Overlap) && native_tag(PGA_Freedom)->ti_Data == LORIENT_VERT);
    for (ULONG a : { inc, dec })
    {
        assert(f.word(a + GD_GADGETID) == 12 && f.lng(a + GD_USERDATA) == 0 &&
               (f.word(a + GD_FLAGS) & GFLG_LABELMASK) == GFLG_LABELIMAGE);
        const ULONG im = f.lng(a + GD_GADGETTEXT);
        assert(im && f.word(im + IM_WIDTH) == 8 && f.word(im + IM_DEPTH) == 1 && f.lng(im + IM_IMAGEDATA) == 0);
    }
    assert(f.word(f.lng(dec + GD_GADGETTEXT) + IM_HEIGHT) == 101);
    assert(f.lng(sc + GD_USERDATA) == 0x2222 && f.word(f.lng(sc + GD_SPECIALINFO) + PI_FLAGS) == (AUTOKNOB | FREEVERT));
    assert(f.count(c) == 6 && natives.size() == 6);

    /* without a level or arrows, one gadget each */
    f.newgad(50, 40, 100, 12, 0, 0, 13, 0, f.vi, 0);
    f.tagitems({ { GTSL_Level, 1 } });
    const ULONG sl2 = f.create(SLIDER_KIND, sc);
    f.newgad(50, 60, 100, 12, 0, 0, 14, 0, f.vi, 0);
    f.tagitems({ { GTSC_Arrows, 0 } });
    const ULONG sc2 = f.create(SCROLLER_KIND, sl2);
    assert(f.lng(sc + GD_NEXTGADGET) == sl2 && f.lng(sl2 + GD_NEXTGADGET) == sc2 && f.count(c) == 8);
    f.free(c);
    assert(natives.empty() && freed == 8 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* String and integer: a 68k StringInfo with its buffer in guest memory */
static void test_string_kinds()
{
    Fixture f;
    const ULONG c = f.context();

    /* GTST_String longer than MaxChars: the bridge already sends it cut,
       independently of the string class's own limit. */
    f.newgad(10, 10, 120, 14, f.s("Name"), 0, 21, 0, f.vi, 0x21);
    f.tagitems({ { GTST_String, f.s("abcdefghij") }, { GTST_MaxChars, 4 }, { GA_TabCycle, 0 },
                 { STRINGA_Justification, 0x200 } });
    const ULONG st = f.create(STRING_KIND, c);
    assert(st && !f.stopped());
    auto *copy = (const char *)native_tag(GTST_String)->ti_Data;
    assert(copy && !f.guest(copy) && created_string == "abcd");
    Native *n = f.native(st);
    assert(std::string((const char *)n->buffer) == "abcd" && n->si.MaxChars == 5);
    const ULONG si = f.lng(st + GD_SPECIALINFO);
    const ULONG buf = f.lng(si + SI_BUFFER);
    assert(si && buf && f.str(buf) == "abcd" && f.word(si + SI_MAXCHARS) == 5 && f.word(si + SI_NUMCHARS) == 4);
    assert(f.lng(si + SI_UNDOBUFFER) == 0 && f.lng(si + SI_EXTENSION) == 0 && f.lng(si + SI_ALTKEYMAP) == 0);
    assert(f.word(si + SI_CLEFT) == 3 && f.word(si + SI_CTOP) == 2 && f.word(si + SI_DISPCOUNT) == 9);
    /* the extension flag stays out with the extension */
    assert(f.word(st + GD_FLAGS) == GFLG_EXTENDED && f.word(st + GD_LEFTEDGE) == 14 &&
           f.word(st + GD_WIDTH) == 112 && f.lng(st + GD_USERDATA) == 0x21);

    /* a string that fits comes whole, and the default MaxChars is 64 */
    f.newgad(10, 30, 120, 14, 0, 0, 22, 0, f.vi, 0);
    f.tagitems({ { GTST_String, f.s("fits") } });
    const ULONG st2 = f.create(STRING_KIND, st);
    assert(f.str(f.lng(f.lng(st2 + GD_SPECIALINFO) + SI_BUFFER)) == "fits" &&
           f.word(f.lng(st2 + GD_SPECIALINFO) + SI_MAXCHARS) == 65);

    /* integer: LongInt and the buffer */
    f.newgad(10, 50, 120, 14, 0, 0, 23, 0, f.vi, 0);
    f.tagitems({ { GTIN_Number, (ULONG)-1234 }, { GTIN_MaxChars, 8 } });
    const ULONG in = f.create(INTEGER_KIND, st2);
    const ULONG isi = f.lng(in + GD_SPECIALINFO);
    assert(in && (LONG)f.lng(isi + SI_LONGINT) == -1234 && f.str(f.lng(isi + SI_BUFFER)) == "-1234" &&
           f.word(isi + SI_MAXCHARS) == 9 && (f.word(in + GD_ACTIVATION) & GACT_LONGINT));
    assert(!native_tag(GTST_String));
    assert(f.count(c) == 4);
    f.free(c);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* Of a repeated tag GadTools uses only the first (GetTagData() for
   GT_Underscore and the make functions of buttons, checkboxes, cycles and
   mxes) or the last (the loops of the others), and only that one is
   checked and reaches it. A NUL GT_Underscore never does. */
static void test_tags_used()
{
    Fixture f;
    const ULONG c = f.context();
    const ULONG one = f.s("One");
    m68k_write32(&f.ctx, Fixture::labels, one);
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);

    f.newgad(10, 10, 80, 14, f.s("Ok"), 0, 1, 0, f.vi, 0);
    f.tagitems({ { GA_Disabled, 1 }, { GT_Underscore, 0 }, { GA_Disabled, 0 }, { GT_Underscore, '_' } });
    ULONG g = f.create(BUTTON_KIND, c);
    assert(g && native_count(GA_Disabled) == 1 && native_tag(GA_Disabled)->ti_Data == 1);
    assert(native_count(GT_Underscore) == 1 && native_tag(GT_Underscore)->ti_Data == 1);
    assert(f.str(f.lng(f.lng(g + GD_GADGETTEXT) + IT_ITEXT)) == "Ok" &&
           f.lng(f.lng(g + GD_GADGETTEXT) + IT_NEXTTEXT) == 0);

    f.newgad(10, 30, 80, 14, 0, 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Labels, 0xFFFFFFF0 } });
    g = f.create(CYCLE_KIND, g);
    assert(g && !f.stopped() && native_count(GTCY_Labels) == 1 &&
           f.at(((STRPTR *)native_tag(GTCY_Labels)->ti_Data)[0]) == one);

    f.newgad(10, 50, 80, 14, 0, 0, 3, 0, f.vi, 0);
    f.tagitems({ { GTNM_Format, 0 }, { GTNM_Format, 0xFFFFFFF0 }, { GTNM_Format, f.s("%ld") } });
    g = f.create(NUMBER_KIND, g);
    assert(g && !f.stopped() && native_count(GTNM_Format) == 1 &&
           std::string((const char *)native_tag(GTNM_Format)->ti_Data) == "%ld");

    f.newgad(10, 70, 80, 14, 0, 0, 4, 0, f.vi, 0);
    f.tagitems({ { GTST_MaxChars, 2 }, { GT_Underscore, '_' }, { GTST_String, 0xFFFFFFF0 },
                 { GTST_String, f.s("abcdef") }, { GTST_MaxChars, 4 }, { GT_Underscore, '#' } });
    g = f.create(STRING_KIND, g);
    assert(g && !f.stopped() && native_count(GTST_MaxChars) == 1 && native_tag(GTST_MaxChars)->ti_Data == 4);
    assert(native_count(GT_Underscore) == 1 && native_tag(GT_Underscore)->ti_Data == '_');
    assert(f.str(f.lng(f.lng(g + GD_SPECIALINFO) + SI_BUFFER)) == "abcd");

    /* the MaxChars that counts is the last one, for strings and integers */
    f.newgad(10, 80, 80, 14, 0, 0, 4, 0, f.vi, 0);
    f.tagitems({ { GTST_MaxChars, 0xFFFFFFFF }, { GTST_MaxChars, 3 }, { GTST_String, f.s("abcdef") } });
    g = f.create(STRING_KIND, g);
    assert(g && !f.stopped() && f.str(f.lng(f.lng(g + GD_SPECIALINFO) + SI_BUFFER)) == "abc");
    f.tagitems({ { GTIN_MaxChars, 65534 }, { GTIN_MaxChars, 5 }, { GTIN_Number, 123456789 } });
    g = f.create(INTEGER_KIND, g);
    assert(g && !f.stopped() && f.word(f.lng(g + GD_SPECIALINFO) + SI_MAXCHARS) == 6 &&
           f.str(f.lng(f.lng(g + GD_SPECIALINFO) + SI_BUFFER)) == "12345");

    /* GTPA_Depth and GTPA_NumColors keep their order */
    for (ULONG i = 0; i < 8; ++i)
        f.memory[Fixture::colors + i] = (UBYTE)i;
    f.newgad(10, 90, 80, 14, 0, 0, 5, 0, f.vi, 0);
    f.tagitems({ { GTPA_Depth, 2 }, { GTPA_NumColors, 6 }, { GTPA_Depth, 3 },
                 { GTPA_ColorTable, Fixture::colors } });
    g = f.create(PALETTE_KIND, g);
    assert(g && created_tags.size() == 4 && created_tags[0].ti_Tag == GTPA_NumColors &&
           created_tags[1].ti_Tag == GTPA_Depth && created_tags[1].ti_Data == 3 &&
           f.native(g)->numcolors == 8);
    f.free(c);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* The output of AROS's RawDoFmt() (InternalRawDoFmt() of rom/exec) for
   a format and the ULONG that the text class of GadTools passes, on a
   little-endian machine: widths in ULONG arithmetic, each number fetched
   from the bytes of the ULONG in turn, a WORD for no size, a LONG for 'l',
   an IPTR for 'i'. -1 for a format that fetches a pointer or reads past
   the ULONG. */
static long aros_output(const char *f, ULONG value)
{
    long out = 1;
    unsigned used = 0;
    UBYTE bytes[4];

    std::memcpy(bytes, &value, 4);
    while (*f)
    {
        if (*f != '%')
        {
            ++out;
            ++f;
            continue;
        }
        ++f;
        if (*f == '-')
            ++f;
        if (*f == '0')
            ++f;
        uint32_t minwidth = 0, maxwidth = ~0u, width = 0, minus = 0;
        while (*f >= '0' && *f <= '9')
            minwidth = minwidth * 10 + (uint32_t)(*f++ - '0');
        if (*f == '.')
        {
            ++f;
            if (*f >= '0' && *f <= '9')
                for (maxwidth = 0; *f >= '0' && *f <= '9'; ++f)
                    maxwidth = maxwidth * 10 + (uint32_t)(*f - '0');
        }
        unsigned size = 2;
        if (*f == 'l')
            size = 4, ++f;
        else if (*f == 'i')
            size = 8, ++f;
        switch (*f)
        {
        case 'b': case 's': case 'p': case 'P':
            return -1;
        case 'd': case 'D': case 'u': case 'U': case 'x': case 'X': case 'c':
        {
            if (used + size > 4)
                return -1;
            uint64_t raw = 0;
            std::memcpy(&raw, bytes + used, size);
            used += size;
            int64_t number = (int64_t)raw;
            if (*f == 'd' || *f == 'D')
            {
                number = size == 2 ? (int64_t)(int16_t)raw : (int64_t)(int32_t)raw;
                if (number < 0)
                {
                    minus = 1;
                    number = -number;
                }
            }
            if (*f == 'c')
                width = 1;
            else
            {
                const int base = *f == 'x' || *f == 'X' ? 16 : 10;
                do
                {
                    ++width;
                    number /= base;
                } while (number);
            }
            break;
        }
        case '\0':
            --f;
            break;
        default:
            width = 1;
            break;
        }
        ++f;
        width = std::min(width, maxwidth);
        out += (long)std::max(width + minus, minwidth);
    }
    return out;
}

static std::string a246() { return std::string(246, 'a'); }
static std::string a247() { return std::string(247, 'a'); }

/* 68k input that the bridge refuses, before GadTools sees it */
static void test_refused()
{
    Fixture f;
    const ULONG c = f.context();
    const ULONG ok = f.s("ok");
    unsigned calls = native_calls;

    /* NULL previous, NewGadget or VisualInfo: 0, as GadTools */
    f.newgad(0, 0, 10, 10, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    assert(f.create(BUTTON_KIND, 0) == 0 && !f.stopped());
    assert(f.call(30, { c, 0, Fixture::tags }, { BUTTON_KIND }) == 0 && !f.stopped());
    f.newgad(0, 0, 10, 10, 0, 0, 1, 0, 0, 0);
    assert(f.create(BUTTON_KIND, c) == 0 && !f.stopped() && native_calls == calls);

    struct Case { const char *what; void (*setup)(Fixture &); ULONG kind; ULONG prev; };
    static ULONG s_ok, s_long;
    s_ok = ok;
    /* a label of 16000 characters, too long for makeitext() */
    s_long = Kaffeine_HeapAlloc(&f.ctx, 16001, MEMF_CLEAR);
    assert(s_long);
    std::memset(f.memory.data() + s_long, 'a', 16000);
    const Case cases[] =
    {
        { "previous not a gadget", [](Fixture &x) { x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0); },
          BUTTON_KIND, Fixture::raw },
        { "NewGadget outside", [](Fixture &) {}, BUTTON_KIND, 0 },
        { "not a VisualInfo", [](Fixture &x) { x.newgad(0, 0, 10, 10, 0, 0, 1, 0, Fixture::raw, 0); },
          BUTTON_KIND, 0 },
        { "label", [](Fixture &x) { x.newgad(0, 0, 10, 10, 0xFFFF0, 0, 1, 0, x.vi, 0); },
          BUTTON_KIND, 0 },
        { "long label", [](Fixture &x) { x.newgad(0, 0, 10, 10, s_long, 0, 1, 0, x.vi, 0); },
          BUTTON_KIND, 0 },
        { "TextAttr", [](Fixture &x) { x.newgad(0, 0, 10, 10, 0, 0xFFFFC, 1, 0, x.vi, 0); },
          BUTTON_KIND, 0 },
        { "long font name", [](Fixture &x) {
              m68k_write32(&x.ctx, Fixture::textattr + TA_NAME, s_long);
              x.newgad(0, 0, 10, 10, s_ok, Fixture::textattr, 1, 0, x.vi, 0); }, BUTTON_KIND, 0 },
        { "tags outside", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GA_Disabled, 1 }, { TAG_MORE, 0xFFFFF0 } }); }, BUTTON_KIND, 0 },
        { "tags in a cycle", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GA_Disabled, 1 }, { TAG_MORE, Fixture::tags } }); }, BUTTON_KIND, 0 },
        { "edit hook", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTST_EditHook, Fixture::raw } }); }, INTEGER_KIND, 0 },
        { "level function", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTSL_DispFunc, Fixture::raw } }); }, SLIDER_KIND, 0 },
        { "labels", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              m68k_write32(&x.ctx, Fixture::labels, s_ok);
              m68k_write32(&x.ctx, Fixture::labels + 4, 0xFFFFF);
              x.tagitems({ { GTCY_Labels, Fixture::labels } }); }, CYCLE_KIND, 0 },
        { "labels without an end", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              m68k_write32(&x.ctx, 0xFFF8, s_ok);
              m68k_write32(&x.ctx, 0xFFFC, s_ok);
              x.tagitems({ { GTMX_Labels, 0xFFF8 } }); }, MX_KIND, 0 },
        { "text", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTTX_Text, 0x10000 } }); }, TEXT_KIND, 0 },
        { "GTST_String", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTST_String, 0x10004 } }); }, STRING_KIND, 0 },
        { "colour table", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTPA_ColorTable, 0xFFF0 }, { GTPA_Depth, 5 } }); }, PALETTE_KIND, 0 },
        { "colour depth", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTPA_ColorTable, Fixture::colors }, { GTPA_Depth, 64 } }); }, PALETTE_KIND, 0 },
        { "colours for a WORD index", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTPA_ColorTable, 0x6000 }, { GTPA_NumColors, 32768 } }); }, PALETTE_KIND, 0 },
        { "MaxChars of an integer", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTIN_MaxChars, 65534 }, { GTIN_Number, 1 } }); }, INTEGER_KIND, 0 },
        { "MaxChars of -1", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTST_MaxChars, 0xFFFFFFFF } }); }, STRING_KIND, 0 },
        { "MaxChars past a WORD", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTST_MaxChars, 10 }, { GTST_MaxChars, 32767 } }); }, STRING_KIND, 0 },
        { "NULL number format", [](Fixture &x) {
              x.newgad(0, 0, 10, 10, 0, 0, 1, 0, x.vi, 0);
              x.tagitems({ { GTNM_Format, 0 } }); }, NUMBER_KIND, 0 },
    };
    for (const Case &k : cases)
    {
        k.setup(f);
        calls = native_calls;
        f.call(30, { k.prev ? k.prev : c, std::strcmp(k.what, "NewGadget outside") ? Fixture::newgadget : 0xFFF0,
                     Fixture::tags }, { k.kind });
        if (!f.stopped() || native_calls != calls)
        {
            std::printf("not refused: %s\n", k.what);
            assert(false);
        }
    }

    /* the longest label and font name that makeitext() takes, one at a
       time for the guest memory of the test */
    f.memory[s_long + 15999] = 0;
    f.newgad(0, 0, 10, 10, s_long, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    ULONG prev = f.create(BUTTON_KIND, c);
    assert(prev && !f.stopped() && f.str(f.lng(f.lng(prev + GD_GADGETTEXT) + IT_ITEXT)).size() == 15999);
    f.free(prev);
    m68k_write32(&f.ctx, Fixture::textattr + TA_NAME, s_long);
    f.newgad(0, 0, 10, 10, s_ok, Fixture::textattr, 1, 0, f.vi, 0);
    prev = f.create(BUTTON_KIND, c);
    assert(prev && !f.stopped());
    f.free(prev);
    prev = c;

    /* formats: one integer or character of a WORD or a LONG, and no more
       than 255 characters of output for any number, as AROS's RawDoFmt()
       gives them; the bridge decides as the reference above */
    const std::string a242(242, 'a'), a243(243, 'a');
    const std::string a244(244, 'a'), a245(245, 'a'), a250(250, 'a'), a251(251, 'a');
    const std::string a253(253, 'a'), a254(254, 'a'), a255(255, 'a'), a256(256, 'a');
    const std::vector<std::string> formats =
    {
        "%ld", "%lu", "%lx", "%d", "%u", "%x", "%c", "%lc", "%%", "%", "%5", "%-3lu%%",
        "Level %04lx", "%.2ld", "x%0-5ld", "%.0ld", "%.ld", "%255ld", "%256ld", "%s", "%ld%ld",
        "%b", "%p", "%P", "%ild", "%lc%ld", "%d%%%u", a244 + "%ld", a245 + "%ld", a253 + "%.1ld",
        a254 + "%.1ld", a250 + "%x", a251 + "%lx", a255, a256, a254 + "%", a250 + "%10",
        a244 + "%-11ld", a245 + "%-11ld", a250 + "%.3lu", "%249ld......", "%250ld......",
        "%i%", "%d/%d", "%d%u%x", "%4294967296u", "%4294967552u", "%.4294967297ld", a253 + "%.4294967297ld",
        "%lc", "%c%c", "%lc%c", "%ix", "%ic", a242 + "%d/%d", a243 + "%d/%d", a243 + "%u/%d",
        a244 + "%u/%d", a243 + "%d/%u", a244 + "%d/%u", a246() + "%x/%x", a247() + "%x/%x"
    };
    const ULONG buffer = Kaffeine_HeapAlloc(&f.ctx, 512, MEMF_CLEAR);
    /* each WORD of the ULONG at its extremes too, for formats of two WORDs */
    const ULONG values[] = { 0, 1, 9, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x8000, 0x7FFF, 0xFFFF,
                             0x12345678, 0xFFFF8000, 0x80008000, 0x80007FFF, 0x7FFF8000, 0x8000FFFF,
                             0xFFFF0000 };
    unsigned accepted = 0;
    for (const std::string &fmt : formats)
    {
        long longest = 0;
        for (ULONG v : values)
            longest = aros_output(fmt.c_str(), v) < 0 ? 100000 : std::max(longest, aros_output(fmt.c_str(), v));
        const bool fits = longest <= 256;
        std::memcpy(f.memory.data() + buffer, fmt.c_str(), fmt.size() + 1);
        f.newgad(0, 0, 10, 10, 0, 0, 1, 0, f.vi, 0);
        f.tagitems({ { GTNM_Format, buffer } });
        calls = native_calls;
        ULONG g = f.create(NUMBER_KIND, prev);
        if (f.stopped())
            g = 0;
        if (fits != (g != 0) || (!fits && native_calls != calls))
        {
            std::printf("format %s: AROS gives %ld characters, the bridge %s it\n", fmt.c_str(), longest,
                        g ? "took" : "refused");
            assert(false);
        }
        if (g)
        {
            assert(std::string(f.native(g)->format) == fmt);
            prev = g;
            ++accepted;
        }
    }
    assert(accepted > 15 && accepted < formats.size());
    Kaffeine_HeapFree(&f.ctx, buffer, 512);
    f.free(c);
    Kaffeine_HeapFree(&f.ctx, s_long, 16001);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* the most colours that the WORD index of the class reaches */
    const ULONG table = Kaffeine_HeapAlloc(&f.ctx, 32767, MEMF_CLEAR);
    assert(table);
    const ULONG c2 = f.context();
    f.newgad(0, 0, 10, 10, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTPA_NumColors, 32767 }, { GTPA_ColorTable, table } });
    assert(f.create(PALETTE_KIND, c2) && !f.stopped());
    f.free(c2);
    Kaffeine_HeapFree(&f.ctx, table, 32767);
}

/* Each call makes a batch, and GadTools frees whole batches: the previous
   gadget must be one that a call returned, FreeGadgets() starts at a
   context or at the first gadget of a batch, and a chain that GadTools cut
   off stays until the program frees it. */
static void test_batches()
{
    Fixture f;
    const ULONG c = f.context();
    const ULONG one = f.s("One");
    m68k_write32(&f.ctx, Fixture::labels, one);
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);

    /* context -> cycle A; context -> button B cuts A off */
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    const ULONG a = f.create(CYCLE_KIND, c);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    assert(a && b && f.lng(c + GD_NEXTGADGET) == b && f.count(c) == 2 && natives.size() == 3);
    /* freeing the list leaves A, whose class still keeps its labels */
    f.free(c);
    assert(natives.size() == 1 && f.native(a) && !f.native(b) && !f.stopped());
    /* A is a list of its own now; freeing it reads the labels again */
    f.free(a);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* context -> button -> scroller with arrows -> button */
    const ULONG c2 = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    const ULONG b1 = f.create(BUTTON_KIND, c2);
    f.tagitems({ { GTSC_Arrows, 8 } });
    const ULONG sc = f.create(SCROLLER_KIND, b1);
    f.tagitems({});
    const ULONG b2 = f.create(BUTTON_KIND, sc);
    const ULONG inc = f.lng(b1 + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    assert(b1 && sc && b2 && f.count(c2) == 6 && natives.size() == 6);
    unsigned calls = native_calls;
    /* an arrow is no previous gadget, nor a place for FreeGadgets() to
       start; GadTools returns NULL before it looks at the previous gadget
       when the NewGadget has no VisualInfo */
    f.newgad(10, 30, 80, 14, 0, 0, 2, 0, 0, 0);
    assert(f.create(BUTTON_KIND, inc) == 0 && !f.stopped() && native_calls == calls);
    f.newgad(10, 30, 80, 14, 0, 0, 2, 0, f.vi, 0);
    f.create(BUTTON_KIND, inc);
    assert(f.stopped() && native_calls == calls);
    f.free(sc);
    assert(f.stopped() && native_calls == calls);
    f.free(dec);
    assert(f.stopped() && native_calls == calls && natives.size() == 6);
    /* from the first gadget of the scroller on: GadTools frees it and the
       button after it, and the button before it no longer leads there */
    f.free(inc);
    assert(!f.stopped() && natives.size() == 2 && f.native(b1) && !f.native(b2));
    assert(f.lng(b1 + GD_NEXTGADGET) == 0 && G(f.native(b1))->NextGadget == nullptr);
    assert(!Kaffeine_ShadowNative(&f.ctx, sc, KAFFEINE_SHADOW_GADGET) &&
           !Kaffeine_ShadowNative(&f.ctx, b2, KAFFEINE_SHADOW_GADGET));
    /* the list goes on from the button */
    f.newgad(10, 30, 80, 14, 0, 0, 2, 0, f.vi, 0);
    const ULONG b3 = f.create(BUTTON_KIND, b1);
    assert(b3 && f.count(c2) == 3);
    f.free(c2);
    assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* GadTools fails, or the bridge runs out of guest memory or shadows */
static void test_failures()
{
    Fixture f;
    const ULONG c = f.context();
    const ULONG one = f.s("One");
    m68k_write32(&f.ctx, Fixture::labels, one);
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);

    f.newgad(10, 10, 80, 14, f.s("First"), 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;

    /* GadTools fails: previous has no NextGadget then, also in the mirror */
    m68k_write32(&f.ctx, b + GD_NEXTGADGET, 0x1234);
    create_fails = true;
    f.newgad(10, 30, 80, 14, f.s("Second"), 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    assert(f.create(CYCLE_KIND, b) == 0 && !f.stopped() && f.lng(b + GD_NEXTGADGET) == 0);
    create_fails = false;
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == shadows);

    /* an unknown kind: GadTools fails as well */
    assert(f.create(10, b) == 0 && !f.stopped() && f.create(99, b) == 0 && !f.stopped());

    /* without guest memory for the mirror, the gadgets of the call go
       again: a scroller with arrows, three gadgets, and a string; each
       amount of free memory fails at another place */
    for (ULONG kind : { (ULONG)SCROLLER_KIND, (ULONG)STRING_KIND })
        for (ULONG leave = 0; leave < 400; leave += 8)
        {
            f.newgad(10, 50, 80, 14, f.s("Third"), 0, 3, 0, f.vi, 0);
            f.tagitems({ { GTSC_Arrows, 8 }, { GTST_String, one } });
            f.exhaust_heap(leave);
            const ULONG before = Kaffeine_HeapAvail(&f.ctx, 0);
            const size_t live = natives.size();
            const UWORD taken = f.ctx.num_shadows;
            const ULONG g = f.create(kind, b);
            assert(!f.stopped());
            if (g)
            {
                assert(natives.size() > live && f.lng(b + GD_NEXTGADGET) && f.count(c) > 2);
                f.release_heap();
                break;
            }
            assert(natives.size() == live && f.lng(b + GD_NEXTGADGET) == 0 && f.count(c) == 2);
            assert(Kaffeine_HeapAvail(&f.ctx, 0) == before && f.ctx.num_shadows == taken);
            f.release_heap();
        }
    /* the last call cut the earlier gadgets off, as GadTools does */
    f.newgad(10, 50, 80, 14, 0, 0, 3, 0, f.vi, 0);
    f.tagitems({});
    assert(f.create(BUTTON_KIND, b) && f.count(c) == 3);

    /* no room for a shadow */
    const UWORD before = f.ctx.num_shadows;
    f.leave_shadows(2);
    const size_t live = natives.size();
    f.tagitems({ { GTSC_Arrows, 8 } });
    assert(f.create(SCROLLER_KIND, b) == 0 && !f.stopped() && natives.size() == live &&
           f.ctx.num_shadows == KAFFEINE_MAX_SHADOWS - 2 && f.lng(b + GD_NEXTGADGET) == 0);
    while (f.ctx.num_shadows > before)
        Kaffeine_ShadowForget(&f.ctx, f.ctx.shadow_map[f.ctx.num_shadows - 1].m68k_addr, FILLER);
    /* GadTools frees what the list still reaches; the chains cut off stay
       until the end of the program: the scroller with its arrows, the
       string and the last button */
    f.free(c);
    assert(natives.size() == 5 && f.ctx.gadgets);
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    assert(f.ctx.num_shadows == shadows - 2);
}

/* ── Attributes ──────────────────────────────────────────────────── */

/* Where the tests let GT_GetGadgetAttrsA() fill in LONGs */
static const ULONG OUT = 0x3C00;
static const ULONG UNTOUCHED = 0x5A5A5A5A;

static void untouched(Fixture &f, unsigned n = 16)
{
    for (unsigned i = 0; i < n; ++i)
        m68k_write32(&f.ctx, OUT + 4 * i, UNTOUCHED);
}

/* GT_GetGadgetAttrsA() through -174, with the tags at Fixture::more */
static ULONG get(Fixture &f, ULONG gad, std::initializer_list<std::pair<ULONG, ULONG>> list)
{
    f.tagitems(list, Fixture::more);
    return f.call(174, { gad, 0, 0, Fixture::more });
}

/* NDK numbers belong to the guest tests only. The native getter above
   knows the AROS extension tags, GTLV_Total/Visible (+150/+151). */
static const ULONG NDK_LV_TOTAL = GT_TagBase + 92, NDK_LV_VISIBLE = GT_TagBase + 93;

static void test_listview_get()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Top, 5 }, { GA_Disabled, 1 },
                 { GTLV_Selected, 0xFFFFFFFF } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped());
    untouched(f);
    const unsigned gets = native_gets;
    const size_t tags = get_tags.size();
    assert(get(f, lv, { { GA_Disabled, OUT }, { GTLV_Top, OUT + 4 }, { GTLV_Selected, OUT + 8 },
                       { GTLV_Labels, OUT + 12 }, { NDK_LV_TOTAL, OUT + 16 },
                       { NDK_LV_VISIBLE, OUT + 20 } }) == 6 && !f.stopped());
    assert(f.lng(OUT) == 1 && f.lng(OUT + 4) == 5 && f.lng(OUT + 8) == 0xFFFFFFFF &&
           f.lng(OUT + 12) == ENTRIES && f.lng(OUT + 16) == 20 && f.lng(OUT + 20) == 10 &&
           f.lng(OUT + 24) == UNTOUCHED);
    assert(native_gets == gets + 6 && get_tags.size() == tags + 6 &&
           get_tags[tags + 4] == GTLV_Total && get_tags[tags + 5] == GTLV_Visible);
    /* AROS numbers supplied by a 68k program are not listview attrs;
       even their invalid storage pointers must be ignored. */
    untouched(f);
    const unsigned calls = native_calls;
    assert(get(f, lv, { { GTLV_Total, OUT }, { GTLV_Visible, OUT + 4 },
                       { GTLV_Total, 0xFFFFFFFF }, { GTLV_Visible, 0xFFFFFFFE },
                       { GTLV_ReadOnly, OUT + 8 }, { GTLV_ShowSelected, OUT + 12 },
                       { GTLV_ItemHeight, OUT + 16 }, { LAYOUTA_Spacing, OUT + 20 } }) == 0 && !f.stopped());
    assert(native_calls == calls && native_gets == gets + 6);
    for (unsigned i = 0; i < 6; ++i)
        assert(f.lng(OUT + 4 * i) == UNTOUCHED);
    f.free(f.lng(c + GD_NEXTGADGET));

    for (ULONG labels : { 0xFFFFFFFFu, 0u })
    {
        f.tagitems({ { GTLV_Labels, labels } });
        const ULONG empty = f.create(LISTVIEW_KIND, c);
        assert(empty && !f.stopped());
        assert(get(f, empty, { { GTLV_Labels, OUT }, { GTLV_Selected, OUT + 4 },
                              { NDK_LV_TOTAL, OUT + 8 }, { NDK_LV_VISIBLE, OUT + 12 } }) == 4 && !f.stopped());
        assert(f.lng(OUT) == labels && f.lng(OUT + 4) == 0xFFFFFFFF && f.lng(OUT + 8) == 0 &&
               f.lng(OUT + 12) == 10);
        f.free(f.lng(c + GD_NEXTGADGET));
    }
}

/* The kinds whose classes give their attributes: counted as the autodoc
   counts them, also the cycle and palette ones that AROS fills in without
   counting */
static void test_get_kinds()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GA_Disabled, TRUE } });
    const ULONG button = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTCB_Checked, TRUE } });
    const ULONG checkbox = f.create(CHECKBOX_KIND, button);
    m68k_write32(&f.ctx, Fixture::labels, f.s("Low"));
    m68k_write32(&f.ctx, Fixture::labels + 4, f.s("Middle"));
    m68k_write32(&f.ctx, Fixture::labels + 8, f.s("High"));
    m68k_write32(&f.ctx, Fixture::labels + 12, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Active, 2 } });
    const ULONG cycle = f.create(CYCLE_KIND, checkbox);
    f.tagitems({ { GTMX_Labels, Fixture::labels }, { GTMX_Active, 1 } });
    const ULONG mx = f.create(MX_KIND, cycle);
    const UBYTE pens[4] = { 3, 2, 1, 0 };
    std::memcpy(f.memory.data() + Fixture::colors, pens, 4);
    f.tagitems({ { GTPA_NumColors, 4 }, { GTPA_ColorTable, Fixture::colors }, { GTPA_Color, 2 } });
    const ULONG palette = f.create(PALETTE_KIND, mx);
    f.tagitems({ { GTPA_Depth, 3 }, { GTPA_ColorOffset, 4 }, { GTPA_Color, 9 } });
    const ULONG offset = f.create(PALETTE_KIND, palette);
    assert(button && checkbox && cycle && mx && palette && offset && !f.stopped());
    const unsigned before = native_gets;

    untouched(f);
    assert(get(f, button, { { GA_Disabled, OUT } }) == 1 && f.lng(OUT) == 1);
    assert(get(f, checkbox, { { GTCB_Checked, OUT }, { GA_Disabled, OUT + 4 } }) == 2);
    assert(f.lng(OUT) == 1 && f.lng(OUT + 4) == 0);
    assert(get(f, cycle, { { GTCY_Active, OUT }, { GTCY_Labels, OUT + 4 }, { GA_Disabled, OUT + 8 } }) == 3);
    /* the program's own label array, not the native copy */
    assert(f.lng(OUT) == 2 && f.lng(OUT + 4) == Fixture::labels && f.lng(OUT + 8) == 0);
    assert(get(f, mx, { { GTMX_Active, OUT } }) == 1 && f.lng(OUT) == 1);
    assert(get(f, palette, { { GTPA_Color, OUT }, { GTPA_ColorOffset, OUT + 4 },
                             { GTPA_ColorTable, OUT + 8 }, { GA_Disabled, OUT + 12 } }) == 4);
    assert(f.lng(OUT) == 2 && f.lng(OUT + 4) == 0 && f.lng(OUT + 8) == Fixture::colors && f.lng(OUT + 12) == 0);
    /* without a table: the pen is the index plus the offset */
    assert(get(f, offset, { { GTPA_Color, OUT }, { GTPA_ColorOffset, OUT + 4 },
                            { GTPA_ColorTable, OUT + 8 } }) == 3);
    assert(f.lng(OUT) == 9 && f.lng(OUT + 4) == 4 && f.lng(OUT + 8) == 0);
    /* one native get for each attribute */
    assert(native_gets - before == 1 + 2 + 3 + 1 + 4 + 3 && !f.stopped());
}

static void test_get_values()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    const ULONG hello = f.s("Hello"), first = f.s("First"), second = f.s("Second");
    f.tagitems({ { GTTX_Text, hello } });
    const ULONG text = f.create(TEXT_KIND, c);
    f.tagitems({ { GTTX_Text, first }, { GTTX_Text, second } });
    const ULONG last = f.create(TEXT_KIND, text);
    const ULONG buffer = f.s("Copied");
    f.tagitems({ { GTTX_Text, buffer }, { GTTX_CopyText, TRUE } });
    const ULONG copied = f.create(TEXT_KIND, last);
    f.tagitems({ { GTTX_CopyText, TRUE } });
    const ULONG empty = f.create(TEXT_KIND, copied);
    f.tagitems({ { GTNM_Number, (ULONG)-1234 } });
    const ULONG number = f.create(NUMBER_KIND, empty);
    f.tagitems({});
    const ULONG zero = f.create(NUMBER_KIND, number);
    f.tagitems({ { GTSL_Min, (ULONG)-10 }, { GTSL_Max, 10 }, { GTSL_Level, (ULONG)-5 },
                 { GTSL_MaxLevelLen, 3 } });
    const ULONG slider = f.create(SLIDER_KIND, zero);
    f.tagitems({ { GTSC_Arrows, 12 }, { GTSC_Top, 5 }, { GTSC_Total, 100 }, { GTSC_Visible, 10 } });
    const ULONG scroller = f.create(SCROLLER_KIND, slider);
    f.tagitems({ { GTST_String, hello } });
    const ULONG string = f.create(STRING_KIND, scroller);
    f.tagitems({ { GTIN_Number, (ULONG)-42 } });
    const ULONG integer = f.create(INTEGER_KIND, string);
    /* the PGA values of a scroller are UWORDs, and Top stays within Total
       less Visible */
    f.tagitems({ { GTSC_Top, 200 }, { GTSC_Total, 100 }, { GTSC_Visible, 10 } });
    const ULONG clamped = f.create(SCROLLER_KIND, integer);
    f.tagitems({ { GTSC_Top, 200 }, { GTSC_Total, 0xFFFF }, { GTSC_Visible, 10 } });
    const ULONG wide = f.create(SCROLLER_KIND, clamped);
    assert(text && last && copied && empty && number && zero && slider && scroller && string &&
           integer && clamped && wide && !f.stopped());
    /* the program reuses its buffer, as GTTX_CopyText allows */
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + buffer), "Reused");
    const unsigned before = native_gets;

    untouched(f);
    assert(get(f, text, { { GTTX_Text, OUT } }) == 1 && f.lng(OUT) == hello);
    assert(get(f, last, { { GTTX_Text, OUT } }) == 1 && f.lng(OUT) == second);
    assert(get(f, copied, { { GTTX_Text, OUT } }) == 1);
    const ULONG copy = f.lng(OUT);
    assert(copy != buffer && copy >= f.ctx.heap_start && copy < f.ctx.heap_end && f.str(copy) == "Copied");
    assert(get(f, empty, { { GTTX_Text, OUT } }) == 1 && f.lng(OUT) == 0);
    assert(get(f, number, { { GTNM_Number, OUT } }) == 1 && f.lng(OUT) == (ULONG)-1234);
    assert(get(f, zero, { { GTNM_Number, OUT } }) == 1 && f.lng(OUT) == 0);
    /* AROS's text class has no getter: the bridge answers these itself */
    assert(native_gets == before);
    assert(get(f, slider, { { GTSL_Min, OUT }, { GTSL_Max, OUT + 4 }, { GTSL_Level, OUT + 8 },
                            { GA_Disabled, OUT + 12 } }) == 4);
    assert(f.lng(OUT) == (ULONG)-10 && f.lng(OUT + 4) == 10 && f.lng(OUT + 8) == (ULONG)-5 &&
           f.lng(OUT + 12) == 0);
    assert(get(f, scroller, { { GTSC_Top, OUT }, { GTSC_Total, OUT + 4 }, { GTSC_Visible, OUT + 8 } }) == 3);
    assert(f.lng(OUT) == 5 && f.lng(OUT + 4) == 100 && f.lng(OUT + 8) == 10);
    assert(get(f, clamped, { { GTSC_Top, OUT } }) == 1 && f.lng(OUT) == 90);
    assert(get(f, wide, { { GTSC_Top, OUT }, { GTSC_Total, OUT + 4 } }) == 2);
    assert(f.lng(OUT) == 200 && f.lng(OUT + 4) == 0x0000FFFF);
    /* the buffer of the 68k StringInfo, with the text of the gadget */
    assert(get(f, string, { { GTST_String, OUT } }) == 1);
    const ULONG si = f.lng(string + GD_SPECIALINFO);
    assert(si && f.lng(OUT) == f.lng(si + SI_BUFFER) && f.str(f.lng(OUT)) == "Hello");
    assert(get(f, integer, { { GTIN_Number, OUT }, { GA_Disabled, OUT + 4 } }) == 2);
    assert(f.lng(OUT) == (ULONG)-42 && f.lng(OUT + 4) == 0);
    /* attributes of the other string kind */
    untouched(f);
    assert(get(f, integer, { { GTST_String, OUT } }) == 0 && get(f, string, { { GTIN_Number, OUT } }) == 0);
    assert(f.lng(OUT) == UNTOUCHED && !f.stopped());

    /* the copy goes with its gadget */
    f.free(c);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* What counts: every occurrence, through the control tags; nothing for an
   attribute of another kind or a gadget without attributes, whose
   storage nobody looks at */
static void test_get_lists()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTSL_Min, (ULONG)-10 }, { GTSL_Max, 10 }, { GTSL_Level, 3 }, { GTSL_MaxLevelLen, 3 } });
    const ULONG slider = f.create(SLIDER_KIND, button);
    f.tagitems({ { GTSC_Arrows, 12 } });
    const ULONG scroller = f.create(SCROLLER_KIND, slider);
    f.tagitems({});
    const ULONG generic = f.create(GENERIC_KIND, scroller);
    assert(button && slider && scroller && generic && !f.stopped());

    untouched(f);
    assert(get(f, slider, { { GTSL_Level, OUT }, { GTSL_Level, OUT + 4 } }) == 2);
    assert(f.lng(OUT) == 3 && f.lng(OUT + 4) == 3);
    /* TAG_IGNORE, TAG_SKIP over a storage outside guest memory, TAG_MORE */
    untouched(f);
    f.tagitems({ { GTSL_Max, OUT + 8 } }, Fixture::tags);
    f.tagitems({ { TAG_IGNORE, 0 }, { TAG_SKIP, 1 }, { GTSL_Min, 0xFFFFFFF0 }, { GTSL_Min, OUT },
                 { TAG_MORE, Fixture::tags } }, Fixture::more);
    assert(f.call(174, { slider, 0, 0, Fixture::more }) == 2 && !f.stopped());
    assert(f.lng(OUT) == (ULONG)-10 && f.lng(OUT + 8) == 10 && f.lng(OUT + 4) == UNTOUCHED);

    const unsigned calls = native_calls;
    untouched(f);
    /* an attribute of another kind: passed over, its storage too */
    assert(get(f, button, { { GTSL_Level, 0xFFFFFFF0 }, { GTSC_Top, OUT } }) == 0 && !f.stopped());
    /* a generic gadget, the context and the gadgets before a slider and a
       scroller have none, and get no method */
    assert(get(f, generic, { { GA_Disabled, OUT } }) == 0 && !f.stopped());
    assert(get(f, c, { { GA_Disabled, OUT } }) == 0 && !f.stopped());
    const ULONG level = f.lng(button + GD_NEXTGADGET), arrow = f.lng(slider + GD_NEXTGADGET);
    assert(level && level != slider && arrow && arrow != scroller);
    assert(get(f, level, { { GA_Disabled, OUT }, { GTSL_Level, OUT } }) == 0 && !f.stopped());
    assert(get(f, arrow, { { GA_Disabled, OUT }, { GTSC_Top, OUT } }) == 0 && !f.stopped());
    /* nothing without a gadget or a list, as GadTools */
    assert(f.call(174, { 0, 0, 0, Fixture::more }) == 0 && f.call(174, { button, 0, 0, 0 }) == 0);
    assert(native_calls == calls && f.lng(OUT) == UNTOUCHED && !f.stopped());
    /* win and req are not read */
    assert(f.call(174, { slider, 0xFFFFFFF0, 0xFFFFFFF4, Fixture::tags }) == 1 && !f.stopped());
    /* what a class does not fill in is not counted, nor written */
    gets_fail = true;
    untouched(f);
    assert(get(f, slider, { { GTSL_Level, OUT }, { GA_Disabled, OUT + 4 } }) == 0 && !f.stopped());
    gets_fail = false;
    assert(f.lng(OUT) == UNTOUCHED && f.lng(OUT + 4) == UNTOUCHED);
}

/* What the bridge refuses, before it writes anything */
static void test_get_refused()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    assert(button && !f.stopped());
    const unsigned calls = native_calls;

    untouched(f);
    /* a mirror that the program changed: no refusal brings it up to date */
    m68k_write16(&f.ctx, button + GD_LEFTEDGE, 999);
    /* not a gadget of GadTools: native GadTools would take it for an object */
    get(f, Fixture::raw, { { GA_Disabled, OUT } });
    assert(f.stopped());
    /* storage outside guest memory, after a good one: nothing written */
    for (ULONG bad : { 0u, 0xFFFFFFF0u, (ULONG)f.memory.size() - 2, (ULONG)f.memory.size() })
    {
        get(f, button, { { GA_Disabled, OUT }, { GA_Disabled, bad } });
        assert(f.stopped());
    }
    /* a list outside guest memory, or in a cycle */
    f.tagitems({ { GA_Disabled, OUT }, { TAG_MORE, 0xFFFFFFF0 } }, Fixture::more);
    f.call(174, { button, 0, 0, Fixture::more });
    assert(f.stopped());
    f.tagitems({ { GA_Disabled, OUT }, { TAG_MORE, Fixture::more } }, Fixture::more);
    f.call(174, { button, 0, 0, Fixture::more });
    assert(f.stopped());
    assert(native_calls == calls && f.lng(OUT) == UNTOUCHED && f.word(button + GD_LEFTEDGE) == 999);

    /* no more attributes than MAX_TAGS */
    const ULONG list = Kaffeine_HeapAlloc(&f.ctx, 8 * 514, MEMF_ANY);
    assert(list);
    for (ULONG i = 0; i < 513; ++i)
    {
        m68k_write32(&f.ctx, list + 8 * i, GA_Disabled);
        m68k_write32(&f.ctx, list + 8 * i + 4, OUT);
    }
    m68k_write32(&f.ctx, list + 8 * 513, TAG_DONE);
    f.call(174, { button, 0, 0, list });
    assert(f.stopped() && native_calls == calls && f.lng(OUT) == UNTOUCHED &&
           f.word(button + GD_LEFTEDGE) == 999);
    m68k_write32(&f.ctx, list + 8 * 512, TAG_DONE);
    assert(f.call(174, { button, 0, 0, list }) == 512 && !f.stopped() && f.lng(OUT) == 0 &&
           f.word(button + GD_LEFTEDGE) == 10);
    Kaffeine_HeapFree(&f.ctx, list, 8 * 514);
}

/* Where the storage lies: anywhere in guest memory with room for a LONG,
   written as four big-endian bytes and nothing else, shared or in the list
   itself, which the bridge reads once, before it writes */
static void test_get_aliasing()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTSL_Min, (ULONG)-10 }, { GTSL_Max, 10 }, { GTSL_Level, 3 } });
    const ULONG slider = f.create(SLIDER_KIND, button);
    assert(button && slider && !f.stopped());
    const ULONG end = (ULONG)f.memory.size();

    /* two-byte aligned, at the end of guest memory, and their neighbours */
    untouched(f);
    m68k_write32(&f.ctx, end - 8, UNTOUCHED);
    m68k_write32(&f.ctx, end - 4, UNTOUCHED);
    assert(get(f, slider, { { GTSL_Min, OUT + 2 }, { GTSL_Max, end - 4 } }) == 2 && !f.stopped());
    const UBYTE *m = f.memory.data();
    assert(m[OUT] == 0x5A && m[OUT + 1] == 0x5A && m[OUT + 2] == 0xFF && m[OUT + 3] == 0xFF &&
           m[OUT + 4] == 0xFF && m[OUT + 5] == 0xF6 && m[OUT + 6] == 0x5A && m[OUT + 7] == 0x5A);
    assert(f.lng(end - 4) == 10 && f.lng(end - 8) == UNTOUCHED);
    get(f, slider, { { GTSL_Max, end - 3 } });
    assert(f.stopped());

    /* one storage for two attributes: the later one stays */
    untouched(f);
    assert(get(f, slider, { { GTSL_Min, OUT }, { GTSL_Max, OUT } }) == 2 && f.lng(OUT) == 10);

    /* the first storage is the ti_Data of the second item: the second
       still goes where the list said */
    untouched(f);
    f.tagitems({ { GTSL_Min, Fixture::more + 12 }, { GTSL_Max, OUT } }, Fixture::more);
    assert(f.call(174, { slider, 0, 0, Fixture::more }) == 2 && !f.stopped());
    assert(f.lng(Fixture::more + 12) == (ULONG)-10 && f.lng(OUT) == 10);

    /* a list in the mirror of the gadget, which the refresh rewrites */
    untouched(f);
    const ULONG list = button + GD_LEFTEDGE;
    m68k_write32(&f.ctx, list, GA_Disabled);
    m68k_write32(&f.ctx, list + 4, OUT);
    m68k_write32(&f.ctx, list + 8, TAG_DONE);
    assert(f.call(174, { button, 0, 0, list }) == 1 && !f.stopped() && f.lng(OUT) == 0);
    assert(f.word(button + GD_LEFTEDGE) == 10 && f.word(button + GD_TOPEDGE) == 10 &&
           f.word(button + GD_WIDTH) == 80 && f.word(button + GD_HEIGHT) == 14 &&
           (f.word(button + GD_FLAGS) & GFLG_EXTENDED));

    /* MAX_TAGS counts attributes only: ignored tags and tags of other
       kinds around them do not count */
    const ULONG big = Kaffeine_HeapAlloc(&f.ctx, 8 * 1540, MEMF_ANY);
    assert(big);
    ULONG at = big;
    for (ULONG i = 0; i < 512; ++i)
    {
        m68k_write32(&f.ctx, at, TAG_IGNORE);
        m68k_write32(&f.ctx, at + 4, 0);
        m68k_write32(&f.ctx, at + 8, GTSC_Top);
        m68k_write32(&f.ctx, at + 12, 0xFFFFFFF0);
        at += 16;
        if (i == 0)
        {
            /* the first attribute has a storage of its own */
            m68k_write32(&f.ctx, at, GTSL_Level);
            m68k_write32(&f.ctx, at + 4, OUT + 8);
            at += 8;
            continue;
        }
        m68k_write32(&f.ctx, at, GTSL_Level);
        m68k_write32(&f.ctx, at + 4, OUT);
        at += 8;
    }
    m68k_write32(&f.ctx, at, TAG_DONE);
    untouched(f);
    assert(f.call(174, { slider, 0, 0, big }) == 512 && !f.stopped() && f.lng(OUT + 8) == 3);
    m68k_write32(&f.ctx, at, GTSL_Level);
    m68k_write32(&f.ctx, at + 4, OUT);
    m68k_write32(&f.ctx, at + 8, TAG_DONE);
    untouched(f);
    f.call(174, { slider, 0, 0, big });
    assert(f.stopped() && f.lng(OUT + 8) == UNTOUCHED);
    Kaffeine_HeapFree(&f.ctx, big, 8 * 1540);
}

/* Lack of memory for the copy of a text, native failures, pointers that
   are not the batch's, mirrors that the program frees, and gadgets that
   are gone */
static void test_get_lifetime()
{
    {
        /* the copy of a text holds its NUL: one byte more than the text,
           in the 8-byte steps of the heap */
        Fixture g;
        const ULONG c = g.context();
        g.newgad(10, 10, 80, 14, 0, 0, 1, 0, g.vi, 0);
        g.tagitems({});
        ULONG prev = g.create(BUTTON_KIND, c);
        for (ULONG len : { 7u, 8u, 15u, 16u })
        {
            const std::string s(len, 'x');
            const ULONG text = g.s(s.c_str());
            g.tagitems({ { GTTX_Text, text } });
            ULONG before = Kaffeine_HeapAvail(&g.ctx, 0);
            const ULONG plain = g.create(TEXT_KIND, prev);
            const ULONG mirror = before - Kaffeine_HeapAvail(&g.ctx, 0);
            g.tagitems({ { GTTX_Text, text }, { GTTX_CopyText, TRUE } });
            before = Kaffeine_HeapAvail(&g.ctx, 0);
            const ULONG copied = g.create(TEXT_KIND, plain);
            const ULONG used = before - Kaffeine_HeapAvail(&g.ctx, 0);
            assert(plain && copied && used - mirror == ((len + 1 + 7) & ~7UL) + KAFFEINE_HEAP_HDR);
            assert(get(g, copied, { { GTTX_Text, OUT } }) == 1 && g.str(g.lng(OUT)) == s);
            prev = copied;
        }
    }
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    assert(b && !f.stopped());

    /* each amount of free memory fails at another place, or works */
    const ULONG text = f.s("Copied text");
    ULONG made = 0;
    for (ULONG leave = 0; leave < 400 && !made; leave += 4)
    {
        f.newgad(10, 30, 80, 14, f.s("Label"), 0, 2, 0, f.vi, 0);
        f.tagitems({ { GTTX_Text, text }, { GTTX_CopyText, TRUE } });
        f.exhaust_heap(leave);
        const ULONG before = Kaffeine_HeapAvail(&f.ctx, 0);
        const size_t live = natives.size();
        const UWORD taken = f.ctx.num_shadows;
        const ULONG g = f.create(TEXT_KIND, b);
        assert(!f.stopped());
        if (g)
        {
            made = g;
            f.release_heap();
            untouched(f);
            assert(get(f, g, { { GTTX_Text, OUT } }) == 1 && f.lng(OUT) != text &&
                   f.str(f.lng(OUT)) == "Copied text");
            break;
        }
        assert(natives.size() == live && f.lng(b + GD_NEXTGADGET) == 0);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == before && f.ctx.num_shadows == taken);
        f.release_heap();
    }
    assert(made);
    /* a native failure makes no copy; GadTools cuts the text off the list,
       where it stays until the program frees it */
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    create_fails = true;
    f.tagitems({ { GTTX_Text, text }, { GTTX_CopyText, TRUE } });
    assert(f.create(TEXT_KIND, b) == 0 && !f.stopped());
    create_fails = false;
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.lng(b + GD_NEXTGADGET) == 0);
    assert(get(f, made, { { GTTX_Text, OUT } }) == 1 && f.str(f.lng(OUT)) == "Copied text");

    /* gets again and again take no guest memory */
    f.tagitems({ { GTST_String, f.s("Hello") } });
    const ULONG string = f.create(STRING_KIND, b);
    assert(string);
    const ULONG steady = Kaffeine_HeapAvail(&f.ctx, 0);
    for (int i = 0; i < 50; ++i)
        assert(get(f, string, { { GTST_String, OUT }, { GA_Disabled, OUT + 4 } }) == 2);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == steady);

    /* a label array that is not the batch's own is not given */
    m68k_write32(&f.ctx, Fixture::labels, f.s("A"));
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    const ULONG one = f.create(CYCLE_KIND, string);
    const ULONG two = f.create(CYCLE_KIND, one);
    assert(one && two);
    Native *n1 = f.native(one), *n2 = f.native(two);
    STRPTR *own = n1->labels;
    n1->labels = n2->labels;
    untouched(f);
    assert(get(f, one, { { GTCY_Labels, OUT }, { GTCY_Active, OUT + 4 } }) == 1);
    assert(f.lng(OUT) == UNTOUCHED && f.lng(OUT + 4) == 0);
    static STRPTR stranger[] = { nullptr };
    n1->labels = stranger;
    assert(get(f, one, { { GTCY_Labels, OUT } }) == 0 && f.lng(OUT) == UNTOUCHED);
    n1->labels = own;
    assert(get(f, one, { { GTCY_Labels, OUT } }) == 1 && f.lng(OUT) == Fixture::labels);

    /* the program frees a mirror: the block stays the bridge's */
    const ULONG held = Kaffeine_HeapAvail(&f.ctx, 0);
    Kaffeine_HeapFree(&f.ctx, string, 56);
    const ULONG si = f.lng(string + GD_SPECIALINFO);
    Kaffeine_HeapFree(&f.ctx, si, 36);
    Kaffeine_HeapFree(&f.ctx, f.lng(si + SI_BUFFER), 65);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == held);
    assert(get(f, string, { { GTST_String, OUT } }) == 1 && f.str(f.lng(OUT)) == "Hello");

    /* a gadget that is gone is no gadget */
    f.free(c);
    get(f, string, { { GTST_String, OUT } });
    assert(f.stopped() && natives.size() == 1);
    f.free(made);
    get(f, made, { { GTTX_Text, OUT } });
    assert(f.stopped());
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* Before a get the mirrors of the batch follow the native gadgets: their
   fields, but not the program's UserData, a string buffer no larger than
   the guest one, and only the bits of MoreFlags that the NDK knows */
static void test_get_settles()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0x1234);
    f.tagitems({ { GTSC_Arrows, 12 } });
    const ULONG scroller = f.create(SCROLLER_KIND, c);
    f.newgad(10, 30, 80, 14, f.s("Name"), 0, 2, 0, f.vi, 0x5678);
    f.tagitems({ { GTST_String, f.s("Hello") }, { GTST_MaxChars, 5 } });
    const ULONG string = f.create(STRING_KIND, scroller);
    assert(scroller && string && !f.stopped());
    for (ULONG g = c; g; g = f.lng(g + GD_NEXTGADGET))
        assert(!(f.lng(g + GD_MOREFLAGS) & ~(ULONG)(GMORE_BOUNDS | GMORE_GADGETHELP | GMORE_SCROLLRASTER)));
    assert(f.lng(string + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP));

    /* native code changes the scroller and its arrows behind the bridge,
       and the program the UserData of every gadget */
    Native *n = f.native(scroller);
    const ULONG arrow = f.lng(c + GD_NEXTGADGET);
    Native *a = f.native(arrow);
    n->gad.Flags |= GFLG_DISABLED;
    n->gad.LeftEdge = 77;
    n->gad.MoreFlags |= 0x400 | 0x80000000;
    n->pi.HorizPot = 0x1234;
    a->gad.Flags |= GFLG_DISABLED;
    a->gad.MoreFlags |= GMORE_SCROLLRASTER | 0x10;
    std::vector<std::pair<ULONG, ULONG>> links;
    ULONG mark = 0xCAFE0000;
    for (ULONG g = c; g; g = f.lng(g + GD_NEXTGADGET))
    {
        m68k_write32(&f.ctx, g + GD_USERDATA, ++mark);
        links.push_back({ g, f.lng(g + GD_NEXTGADGET) });
    }
    /* links that the program changed stay too: the refresh leaves them */
    ULONG bogus = 0xBEEF0000;
    for (auto &l : links)
        m68k_write32(&f.ctx, l.first + GD_NEXTGADGET, ++bogus);
    untouched(f);
    assert(get(f, scroller, { { GA_Disabled, OUT } }) == 1 && f.lng(OUT) == 1);
    bogus = 0xBEEF0000;
    for (auto &l : links)
    {
        assert(f.lng(l.first + GD_NEXTGADGET) == ++bogus);
        m68k_write32(&f.ctx, l.first + GD_NEXTGADGET, l.second);
    }
    assert((f.word(scroller + GD_FLAGS) & GFLG_DISABLED) && f.word(scroller + GD_LEFTEDGE) == 77 &&
           f.word(f.lng(scroller + GD_SPECIALINFO) + PI_HORIZPOT) == 0x1234);
    assert(f.lng(scroller + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP));
    assert((f.word(arrow + GD_FLAGS) & GFLG_DISABLED) &&
           f.lng(arrow + GD_MOREFLAGS) == (GMORE_BOUNDS | GMORE_GADGETHELP | GMORE_SCROLLRASTER));
    mark = 0xCAFE0000;
    for (auto &l : links)
        assert(f.lng(l.first + GD_USERDATA) == ++mark && f.lng(l.first + GD_NEXTGADGET) == l.second);
    /* also a call with no attribute of the gadget, or on an arrow, brings
       the mirrors of the batch up to date */
    n->gad.LeftEdge = 55;
    assert(get(f, scroller, { { GTSL_Level, OUT } }) == 0 && f.word(scroller + GD_LEFTEDGE) == 55);
    n->gad.LeftEdge = 66;
    assert(get(f, arrow, { { GA_Disabled, OUT } }) == 0 && f.word(scroller + GD_LEFTEDGE) == 66);

    /* a native MaxChars above the buffer, which no setter made larger */
    Native *s = f.native(string);
    const ULONG si = f.lng(string + GD_SPECIALINFO), buffer = f.lng(si + SI_BUFFER);
    assert(f.word(si + SI_MAXCHARS) == 6 && f.str(buffer) == "Hello");
    std::memcpy(s->buffer, "HelloX", 6);
    s->si.MaxChars = 50;
    assert(get(f, string, { { GTST_String, OUT } }) == 1 && f.lng(OUT) == buffer);
    assert(f.str(buffer) == "Hello" && f.word(si + SI_MAXCHARS) == 6);
    s->si.MaxChars = 4;
    std::memcpy(s->buffer, "Hey", 4);
    assert(get(f, string, { { GTST_String, OUT } }) == 1 && f.str(buffer) == "Hey" &&
           f.word(si + SI_MAXCHARS) == 4);
    /* no room at all: an empty text, and no MaxChars */
    for (WORD none : { (WORD)1, (WORD)0, (WORD)-5 })
    {
        s->si.MaxChars = none;
        assert(get(f, string, { { GTST_String, OUT } }) == 1 && f.lng(OUT) == buffer);
        assert(f.str(buffer).empty() && f.word(si + SI_MAXCHARS) == (none > 0 ? 1 : 0));
    }
    s->si.MaxChars = 6;

    /* a new native buffer gets a new guest one; without guest memory for
       it the attribute is not filled in, and the old fields stay */
    UBYTE other[8] = "Other";
    s->si.Buffer = other;
    s->si.MaxChars = 8;
    assert(get(f, string, { { GTST_String, OUT } }) == 1);
    const ULONG second = f.lng(OUT);
    assert(second != buffer && f.lng(si + SI_BUFFER) == second && f.str(second) == "Other");
    UBYTE third[8] = "Third";
    s->si.Buffer = third;
    f.exhaust_heap();
    untouched(f);
    assert(get(f, string, { { GTST_String, OUT } }) == 0 && f.lng(OUT) == UNTOUCHED && !f.stopped());
    assert(f.lng(si + SI_BUFFER) == second);
    f.release_heap();
    assert(get(f, string, { { GTST_String, OUT } }) == 1 && f.str(f.lng(OUT)) == "Third");
    s->si.Buffer = s->buffer;

    /* A new native StringInfo or label: with each amount of free guest
       memory the mirror has either the old one, as it was, or the whole
       new one, never a part of it. */
    struct NewSI { StringInfo si{}; UBYTE buffer[16] = "Partial"; };
    struct NewLabel { IntuiText it[2]{}; TextAttr ta{}; char font[16] = "other.font"; char one[8] = "One",
                      two[8] = "Two"; };
    std::vector<std::unique_ptr<NewSI>> sis;
    std::vector<std::unique_ptr<NewLabel>> labels;
    const ULONG old_si = f.lng(string + GD_SPECIALINFO), old_buffer = f.lng(old_si + SI_BUFFER);
    const std::string old_text = f.str(old_buffer);
    const ULONG old_label = f.lng(string + GD_GADGETTEXT);
    const ULONG old_itext = f.lng(old_label + IT_ITEXT), old_next = f.lng(old_label + IT_NEXTTEXT);
    assert(old_si && old_buffer && old_label && old_itext);
    bool whole_si = false, whole_label = false;
    for (ULONG leave = 0; leave < 600 && !(whole_si && whole_label); leave += 8)
    {
        if (!whole_si)
        {
            sis.push_back(std::make_unique<NewSI>());
            NewSI &n2 = *sis.back();
            n2.si = s->si;
            n2.si.Buffer = n2.buffer;
            n2.si.MaxChars = 16;
            s->gad.SpecialInfo = &n2.si;
        }
        if (!whole_label)
        {
            labels.push_back(std::make_unique<NewLabel>());
            NewLabel &l = *labels.back();
            l.ta.ta_Name = (STRPTR)l.font;
            l.ta.ta_YSize = 9;
            /* the font of the second text, with its name, is the last copy
               that the chain needs */
            l.it[0].IText = (UBYTE *)l.one;
            l.it[0].ITextFont = s->it[0].ITextFont;
            l.it[0].NextText = &l.it[1];
            l.it[1].ITextFont = &l.ta;
            s->gad.GadgetText = &l.it[0];
        }
        f.exhaust_heap(leave);
        assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
        f.release_heap();
        const ULONG nsi = f.lng(string + GD_SPECIALINFO), nlabel = f.lng(string + GD_GADGETTEXT);
        if (nsi == old_si)
            assert(f.lng(old_si + SI_BUFFER) == old_buffer && f.str(old_buffer) == old_text);
        else
        {
            assert(f.lng(nsi + SI_BUFFER) && f.str(f.lng(nsi + SI_BUFFER)) == "Partial" &&
                   f.word(nsi + SI_MAXCHARS) == 16);
            whole_si = true;
        }
        if (nlabel == old_label)
            assert(f.lng(old_label + IT_ITEXT) == old_itext && f.lng(old_label + IT_NEXTTEXT) == old_next);
        else
        {
            const ULONG second = f.lng(nlabel + IT_NEXTTEXT);
            assert(f.str(f.lng(nlabel + IT_ITEXT)) == "One" && second && !f.lng(second + IT_ITEXT) &&
                   !f.lng(second + IT_NEXTTEXT) && f.lng(nlabel + IT_ITEXTFONT) == f.lng(old_label + IT_ITEXTFONT) &&
                   f.lng(second + IT_ITEXTFONT) &&
                   f.lng(f.lng(second + IT_ITEXTFONT) + TA_NAME) &&
                   f.str(f.lng(f.lng(second + IT_ITEXTFONT) + TA_NAME)) == "other.font");
            whole_label = true;
        }
    }
    assert(whole_si && whole_label);
    s->gad.SpecialInfo = &s->si;
    s->gad.GadgetText = &s->it[0];

    /* a new font of a published text: the TextAttr only with its name,
       which here needs more guest memory than the TextAttr */
    struct NewFont { TextAttr ta{}; char name[48] = "a-font-with-a-name-longer-than-a-textattr.font"; };
    std::vector<std::unique_ptr<NewFont>> fonts;
    TextAttr *const font = s->it[0].ITextFont;
    const ULONG old_font = f.lng(old_label + IT_ITEXTFONT);
    bool whole_font = false;
    for (ULONG leave = 0; leave < 200 && !whole_font; leave += 4)
    {
        fonts.push_back(std::make_unique<NewFont>());
        NewFont &nf = *fonts.back();
        nf.ta.ta_Name = (STRPTR)nf.name;
        nf.ta.ta_YSize = 9;
        s->it[0].ITextFont = &nf.ta;
        f.exhaust_heap(leave);
        assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
        f.release_heap();
        const ULONG ta = f.lng(old_label + IT_ITEXTFONT);
        if (ta == old_font)
            continue;
        assert(f.lng(ta + TA_NAME) && f.str(f.lng(ta + TA_NAME)) == nf.name && f.word(ta + TA_YSIZE) == 9);
        whole_font = true;
    }
    assert(whole_font);
    s->it[0].ITextFont = font;

    /* A new chain of two texts, each with its string: failing at any of
       its copies, the second string too, leaves the old label. */
    struct TwoTexts { IntuiText it[2]{}; char one[8] = "First", two[8] = "Second"; };
    std::vector<std::unique_ptr<TwoTexts>> twos;
    bool whole_two = false;
    for (ULONG leave = 0; leave < 300 && !whole_two; leave += 4)
    {
        twos.push_back(std::make_unique<TwoTexts>());
        TwoTexts &w = *twos.back();
        w.it[0].IText = (UBYTE *)w.one;
        w.it[0].ITextFont = font;
        w.it[0].NextText = &w.it[1];
        w.it[1].IText = (UBYTE *)w.two;
        w.it[1].ITextFont = font;
        s->gad.GadgetText = &w.it[0];
        const ULONG before = f.lng(string + GD_GADGETTEXT);
        const ULONG before_text = f.lng(before + IT_ITEXT), before_next = f.lng(before + IT_NEXTTEXT);
        f.exhaust_heap(leave);
        assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
        f.release_heap();
        const ULONG now = f.lng(string + GD_GADGETTEXT);
        if (now == before)
        {
            assert(f.lng(before + IT_ITEXT) == before_text && f.lng(before + IT_NEXTTEXT) == before_next);
            continue;
        }
        const ULONG second = f.lng(now + IT_NEXTTEXT);
        assert(f.str(f.lng(now + IT_ITEXT)) == "First" && second && f.str(f.lng(second + IT_ITEXT)) == "Second" &&
               !f.lng(second + IT_NEXTTEXT));
        whole_two = true;
    }
    assert(whole_two);
    s->gad.GadgetText = &s->it[0];
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && f.lng(string + GD_GADGETTEXT) == old_label);

    /* A published font that changed, in a chain that cannot be copied
       whole: the guest font stays as it was too. */
    IntuiText grown{};
    char grown_text[8] = "Grown";
    grown.IText = (UBYTE *)grown_text;
    grown.ITextFont = font;
    const ULONG font_copy = f.lng(old_label + IT_ITEXTFONT);
    const UWORD ysize = f.word(font_copy + TA_YSIZE);
    font->ta_YSize = (UWORD)(ysize + 5);
    s->it[0].NextText = &grown;
    f.exhaust_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
    assert(f.word(font_copy + TA_YSIZE) == ysize && f.lng(old_label + IT_NEXTTEXT) == 0);
    f.release_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1);
    assert(f.word(font_copy + TA_YSIZE) == ysize + 5 && f.lng(old_label + IT_NEXTTEXT));
    s->it[0].NextText = nullptr;
    font->ta_YSize = ysize;

    /* A label of another kind without guest memory for its copy: the old
       label stays with the kind of its own in Flags. */
    char native_label[16] = "String label";
    const UWORD kind = f.word(string + GD_FLAGS) & GFLG_LABELMASK;
    assert(kind == GFLG_LABELITEXT);
    s->gad.Flags = (UWORD)((s->gad.Flags & ~GFLG_LABELMASK) | GFLG_LABELSTRING);
    s->gad.GadgetText = (IntuiText *)native_label;
    f.exhaust_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
    assert(f.lng(string + GD_GADGETTEXT) == old_label && (f.word(string + GD_FLAGS) & GFLG_LABELMASK) == kind);
    f.release_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1);
    assert((f.word(string + GD_FLAGS) & GFLG_LABELMASK) == GFLG_LABELSTRING &&
           f.str(f.lng(string + GD_GADGETTEXT)) == "String label");
    s->gad.Flags = (UWORD)((s->gad.Flags & ~GFLG_LABELMASK) | GFLG_LABELITEXT);
    s->gad.GadgetText = &s->it[0];

    /* The StringInfo that the mirror has, with a new native buffer and new
       counts but no guest memory for the copy: none of it changes. */
    const ULONG keep_si = f.lng(string + GD_SPECIALINFO);
    UBYTE replaced[12] = "Replaced";
    UBYTE *const own_buffer = s->si.Buffer;
    const WORD own_numchars = s->si.NumChars, own_pos = s->si.BufferPos, own_max = s->si.MaxChars;
    std::array<UBYTE, 36> before_si;
    std::memcpy(before_si.data(), f.memory.data() + keep_si, 36);
    s->si.Buffer = replaced;
    s->si.MaxChars = 12;
    s->si.NumChars = 8;
    s->si.BufferPos = 7;
    f.exhaust_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1 && !f.stopped());
    assert(f.lng(string + GD_SPECIALINFO) == keep_si &&
           !std::memcmp(before_si.data(), f.memory.data() + keep_si, 36));
    f.release_heap();
    assert(get(f, string, { { GA_Disabled, OUT } }) == 1);
    assert(f.str(f.lng(keep_si + SI_BUFFER)) == "Replaced" && f.word(keep_si + SI_NUMCHARS) == 8);
    s->si.Buffer = own_buffer;
    s->si.MaxChars = own_max;
    s->si.NumChars = own_numchars;
    s->si.BufferPos = own_pos;

    /* a published chain that grows: without guest memory for the text put
       into it, the chain stays as it was, not cut where the new text goes */
    f.newgad(10, 50, 80, 14, f.s("_Key"), 0, 3, 0, f.vi, 0);
    f.tagitems({ { GT_Underscore, '_' } });
    const ULONG keyed = f.create(BUTTON_KIND, string);
    Native *k = f.native(keyed);
    const ULONG first = f.lng(keyed + GD_GADGETTEXT), old_second = f.lng(first + IT_NEXTTEXT);
    assert(keyed && first && old_second && f.str(f.lng(old_second + IT_ITEXT)) == "K");
    IntuiText inserted{};
    char middle[8] = "Middle";
    inserted.IText = (UBYTE *)middle;
    inserted.ITextFont = k->it[0].ITextFont;
    inserted.NextText = &k->it[1];
    k->it[0].NextText = &inserted;
    f.exhaust_heap();
    assert(get(f, keyed, { { GA_Disabled, OUT } }) == 1);
    assert(f.lng(keyed + GD_GADGETTEXT) == first && f.lng(first + IT_NEXTTEXT) == old_second);
    f.release_heap();
    assert(get(f, keyed, { { GA_Disabled, OUT } }) == 1 && f.lng(keyed + GD_GADGETTEXT) == first);
    const ULONG mid = f.lng(first + IT_NEXTTEXT);
    assert(mid != old_second && f.str(f.lng(mid + IT_ITEXT)) == "Middle" && f.lng(mid + IT_NEXTTEXT) == old_second);
    k->it[0].NextText = &k->it[1];
    f.free(c);
    assert(natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

/* Intuition calls that would reach a GadTools gadget past the bridge stop
   the program; other objects go on as the generated thunks pass them */
static void test_intuition_guards()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    assert(b && !f.stopped());
    const unsigned calls = native_calls;
    passed.clear();
    converted_lists = 0;

    for (ULONG gad : { b, c })
    {
        // ActivateGadget() of a gadget on no window gives FALSE
        assert(f.call_at(Fixture::intuition, 462, { gad, 0, 0 }) == 0 && !f.stopped());
        f.call_at(Fixture::intuition, 648, { gad, Fixture::tags });
        assert(f.stopped());
        f.call_at(Fixture::intuition, 654, { gad, OUT }, { GA_Disabled });
        assert(f.stopped());
        f.call_at(Fixture::intuition, 660, { gad, 0, 0, Fixture::tags });
        assert(f.stopped());
        f.call_at(Fixture::intuition, 810, { gad, 0, 0, Fixture::tags });
        assert(f.stopped());
    }
    assert(native_calls == calls && passed.empty() && converted_lists == 0);

    /* other objects, a window and a requester, each passed on by itself */
    static int object, window, requester;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3B90, &object, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BA0, &window, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BB0, &requester, FILLER));
    auto by_itself = [&](const char *call) {
        return passed.back() == call && passed_object == &object && passed_window == &window &&
               passed_requester == &requester;
    };
    // what no window holds is not activated, nor passed to Intuition
    assert(f.call_at(Fixture::intuition, 462, { 0x3B90, 0x3BA0, 0x3BB0 }) == 0 && !f.stopped() &&
           passed.empty());
    assert(f.call_at(Fixture::intuition, 648, { 0x3B90, Fixture::tags }) == 0 && !f.stopped());
    assert(passed.back() == "SetAttrsA" && passed_object == &object && passed_more == Fixture::tags);
    assert(f.call_at(Fixture::intuition, 654, { 0x3B90, OUT }, { 0x1234 }) == 1 && !f.stopped());
    assert(passed.back() == "GetAttr" && passed_object == &object &&
           passed_more == ((IPTR)(f.memory.data() + OUT) ^ 0x1234));
    assert(f.call_at(Fixture::intuition, 660, { 0x3B90, 0x3BA0, 0x3BB0, Fixture::tags }) == 0x55 &&
           !f.stopped());
    assert(by_itself("SetGadgetAttrsA") && passed_more == Fixture::tags);
    assert(f.call_at(Fixture::intuition, 810, { 0x3B90, 0x3BA0, 0x3BB0, Fixture::tags }) == 0x66 &&
           !f.stopped());
    assert(by_itself("DoGadgetMethodA") && passed_more == (IPTR)(f.memory.data() + Fixture::tags));
    assert(passed.size() == 4 && converted_lists == 2);
    for (ULONG a : { 0x3B90u, 0x3BA0u, 0x3BB0u })
        Kaffeine_ShadowForget(&f.ctx, a, FILLER);
}

/* ── Changing attributes ─────────────────────────────────────────── */

/* GT_SetGadgetAttrsA() through -42, with the tags at Fixture::more */
static void set(Fixture &f, ULONG gad, std::initializer_list<std::pair<ULONG, ULONG>> list, ULONG win = 0)
{
    f.tagitems(list, Fixture::more);
    f.call(42, { gad, win, 0, Fixture::more });
}

static void test_listview_showsel_set()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(-30, 7, 160, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTST_String, f.s("Seed") }, { GTST_MaxChars, 5 } });
    const ULONG string = f.create(STRING_KIND, c);
    guest_list(f, 3, ENTRIES, NODES, f.s("Alpha"));
    m68k_write32(&f.ctx, NODES + NODE_SIZE + LN_NAME, f.s("Beta"));
    m68k_write32(&f.ctx, NODES + 2 * NODE_SIZE + LN_NAME, f.s("Longer than MaxChars"));
    guest_list(f, 2, 0x5300, 0x5400, f.s("Cedar"));
    m68k_write32(&f.ctx, 0x5400 + NODE_SIZE + LN_NAME, f.s("Elm"));
    f.newgad(10, 20, 160, 100, 0, 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 1 }, { GTLV_ShowSelected, string } });
    const ULONG lv = f.create(LISTVIEW_KIND, string);
    assert(lv && !f.stopped());
    Native *n = f.native(lv), *s = f.native(string);
    const ULONG si = f.lng(string + GD_SPECIALINFO), buffer = f.lng(si + SI_BUFFER);
    assert_shown(f, string, "Beta");
    string_sets.clear();
    unsigned sets = native_sets;
    set(f, lv, { { GTLV_Selected, 1 } });
    assert(!f.stopped() && native_sets == sets + 1 && string_sets.empty());
    assert_shown(f, string, "Beta");
    for (const auto &k : { std::pair<ULONG, const char *>{ 0, "Alpha" },
                           { 2, "Longe" }, { 0xFFFFFFFF, "" }, { 99, "" }, { 1, "Beta" } })
    {
        sets = native_sets;
        set(f, lv, { { GTLV_Selected, k.first } });
        assert(!f.stopped() && native_sets == sets + 2 && n->selected == (WORD)k.first);
        assert_shown(f, string, k.second);
        if (k.first == 2)
            assert(string_sets.back().text == "Longer than MaxChars" && s->si.MaxChars == 6);
    }
    sets = native_sets;
    set(f, lv, { { GTLV_Labels, 0x5300 } });
    assert(!f.stopped() && native_sets == sets + 2 && n->selected == 1);
    assert_shown(f, string, "Elm");
    /* Setting even the same Labels refreshes the selected name. */
    m68k_write32(&f.ctx, 0x5400 + NODE_SIZE + LN_NAME, f.s("Fir"));
    set(f, lv, { { GTLV_Labels, 0x5300 } });
    assert(!f.stopped() && n->selected == 1);
    assert_shown(f, string, "Fir");
    for (ULONG detached : { 0xFFFFFFFFu, 0u })
    {
        const APTR old = block_holding(n->entries);
        sets = native_sets;
        set(f, lv, { { GTLV_Labels, detached } });
        assert(!f.stopped() && native_sets == sets + 2 && n->selected == 1 && n->num_entries == 0 &&
               (IPTR)n->entries == (detached ? ~(IPTR)0 : 0) && !native_blocks.count(old));
        assert_shown(f, string, "");
        set(f, lv, { { GTLV_Selected, 2 } });
        assert(!f.stopped() && n->selected == 2);
        assert_shown(f, string, "");
        set(f, lv, { { GTLV_Labels, ENTRIES } });
        assert(!f.stopped() && n->selected == 2);
        assert_shown(f, string, "Longe");
        set(f, lv, { { GTLV_Labels, 0x5300 } });
        assert(!f.stopped() && n->selected == 2 && n->num_entries == 2);
        assert_shown(f, string, ""); /* the selection is beyond the new list */
        set(f, lv, { { GTLV_Selected, 1 } });
        assert(!f.stopped());
        assert_shown(f, string, "Fir");
    }
    /* Unlike creation, setters run in the program's tag order, including
       every intermediate Labels snapshot and selection. */
    string_sets.clear();
    sets = native_sets;
    set(f, lv, { { GTLV_Selected, 0 }, { GTLV_Labels, ENTRIES },
                 { GTLV_Selected, 1 }, { GTLV_Labels, 0x5300 } });
    assert(!f.stopped() && native_sets == sets + 5 && n->selected == 1);
    assert_string_sets(f, string, { "Cedar", "Alpha", "Beta", "Fir" });
    assert_shown(f, string, "Fir");
    /* Every listview setter settles the string's batch, even when no
       DoShowSelected runs: native changes must become visible as well. */
    std::memcpy(s->si.Buffer, "Local", 6);
    s->gad.BoundsTopEdge -= 7;
    s->gad.TopEdge -= 7;
    assert(f.str(buffer) == "Fir" && f.word(string + GD_BOUNDSTOPEDGE) == 99);
    string_sets.clear();
    sets = native_sets;
    set(f, lv, { { GTLV_Top, 1 } });
    assert(!f.stopped() && native_sets == sets + 1 && string_sets.empty());
    assert_shown(f, string, "Local");
    assert_string_position(f, string, 10, 92, 160, 14);
    assert(f.lng(string + GD_SPECIALINFO) == si && f.lng(si + SI_BUFFER) == buffer);
}

static void assert_showsel_free_refused(Fixture &f, ULONG start, unsigned native_attempts = 0)
{
    const auto live = native_blocks;
    const auto live_natives = natives;
    const auto memory = f.memory;
    const auto batches = f.ctx.gadgets;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;
    const unsigned calls = native_calls, before = freed;
    std::vector<std::pair<Gadget *, Gadget *>> links;
    for (auto &n : natives)
        links.push_back({ n.first, n.first->NextGadget });
    f.free(start);
    assert(f.stopped() && native_calls == calls + native_attempts && freed == before && native_blocks == live &&
           natives == live_natives && f.memory == memory && f.ctx.gadgets == batches &&
           f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
    for (auto &link : links)
    {
        assert(link.first->NextGadget == link.second);
        const ULONG guest = Kaffeine_ShadowGuest(&f.ctx, link.first, KAFFEINE_SHADOW_GADGET);
        assert(guest && f.native(guest) == live_natives.at(link.first));
    }
}

static void test_listview_showsel_free()
{
    {
        Fixture f;
        const ULONG c = f.context();
        f.newgad(0, 0, 160, 14, 0, 0, 1, 0, f.vi, 0);
        f.tagitems({ { GTST_MaxChars, 8 } });
        const ULONG string = f.create(STRING_KIND, c);
        f.tagitems({});
        const ULONG button = f.create(BUTTON_KIND, string), lc = f.context();
        guest_list(f, 2, ENTRIES, NODES, f.s("Alpha"));
        m68k_write32(&f.ctx, NODES + NODE_SIZE + LN_NAME, f.s("Beta"));
        f.newgad(10, 20, 160, 100, 0, 0, 2, 0, f.vi, 0);
        f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 1 }, { GTLV_ShowSelected, string } });
        const ULONG lv = f.create(LISTVIEW_KIND, lc), first = f.lng(lc + GD_NEXTGADGET);
        assert(lv && !f.stopped());
        const APTR entries = block_holding(f.native(lv)->entries);
        assert_shown(f, string, "Beta");
        assert_showsel_free_refused(f, string);
        assert_showsel_free_refused(f, c);
        /* A successful free must not inherit doomed on the context or
           string from either refusal. Nothing else may be released. */
        unsigned before = freed;
        f.free(button);
        assert(!f.stopped() && freed == before + 1 && natives.size() == 7 && f.native(c) &&
               f.native(string) && f.native(lc) && f.native(lv) && native_blocks.count(entries) &&
               !f.lng(string + GD_NEXTGADGET));
        set(f, lv, { { GTLV_Selected, 0 } });
        assert(!f.stopped());
        assert_shown(f, string, "Alpha");
        before = freed;
        f.free(first); /* the listview's complete four-gadget batch */
        assert(!f.stopped() && freed == before + 4 && natives.size() == 3 &&
               f.native(string) && f.native(c) && f.native(lc) && !native_blocks.count(entries) &&
               !f.native(lv) && !f.lng(lc + GD_NEXTGADGET));
        assert_shown(f, string, "Alpha");
        f.free(string);
        assert(!f.stopped() && natives.size() == 2 && !f.lng(c + GD_NEXTGADGET));
        f.free(c);
        f.free(lc);
        assert(!f.stopped() && natives.empty() && native_blocks.empty() &&
               Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
    for (bool from_context : { false, true })
    {
        Fixture f;
        const ULONG c = f.context();
        const auto context_blocks = native_blocks;
        f.newgad(0, 0, 160, 14, 0, 0, 1, 0, f.vi, 0);
        f.tagitems({ { GTST_MaxChars, 8 } });
        const ULONG string = f.create(STRING_KIND, c);
        guest_list(f, 1, ENTRIES, NODES, f.s("Both"));
        f.newgad(10, 20, 160, 100, 0, 0, 2, 0, f.vi, 0);
        f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 0 }, { GTLV_ShowSelected, string } });
        const ULONG lv = f.create(LISTVIEW_KIND, string);
        assert(lv && !f.stopped());
        assert_shown(f, string, "Both");
        f.free(from_context ? c : string);
        assert(!f.stopped() && freed == (from_context ? 6u : 5u) &&
               !f.native(string) && !f.native(lv));
        if (!from_context)
        {
            assert(natives.size() == 1 && f.native(c) && !f.lng(c + GD_NEXTGADGET) &&
                   native_blocks == context_blocks);
            f.free(c);
        }
        assert(natives.empty() && native_blocks.empty() && !f.ctx.gadgets &&
               Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_listview_showsel_shared()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(0, 0, 160, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTST_MaxChars, 5 } });
    const ULONG string = f.create(STRING_KIND, c);
    guest_list(f, 2, ENTRIES, NODES, f.s("Alpha"));
    m68k_write32(&f.ctx, NODES + NODE_SIZE + LN_NAME, f.s("Beta"));
    guest_list(f, 2, 0x5300, 0x5400, f.s("Cedar"));
    m68k_write32(&f.ctx, 0x5400 + NODE_SIZE + LN_NAME, f.s("Elm"));
    f.newgad(10, 20, 160, 100, 0, 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 1 }, { GTLV_ShowSelected, string } });
    const ULONG a = f.create(LISTVIEW_KIND, string), first_a = f.lng(string + GD_NEXTGADGET);
    assert(a && !f.stopped());
    assert_string_position(f, string, 10, 99, 160, 14);
    assert_shown(f, string, "Beta");
    const ULONG c2 = f.context();
    f.newgad(70, 100, 160, 120, 0, 0, 3, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, 0x5300 }, { GTLV_Selected, 0 }, { GTLV_ShowSelected, string } });
    const ULONG b = f.create(LISTVIEW_KIND, c2), first_b = f.lng(c2 + GD_NEXTGADGET);
    assert(b && !f.stopped() && f.native(a)->show_selected == f.native(b)->show_selected &&
           f.native(b)->show_selected == (IPTR)G(f.native(string)));
    assert_string_sets(f, string, { "Alpha", "Beta", "Cedar" });
    assert_string_position(f, string, 70, 199, 160, 14);
    assert_shown(f, string, "Cedar");
    set(f, a, { { GTLV_Selected, 0 } });
    assert(!f.stopped() && f.native(b)->selected == 0);
    assert_shown(f, string, "Alpha");
    set(f, b, { { GTLV_Selected, 1 } });
    assert(!f.stopped() && f.native(a)->selected == 0);
    assert_shown(f, string, "Elm");
    List *other = f.native(b)->entries;
    m68k_write32(&f.ctx, NODES + LN_NAME, f.s("Oak"));
    set(f, a, { { GTLV_Labels, ENTRIES } });
    assert(!f.stopped() && f.native(b)->entries == other && f.native(b)->selected == 1);
    assert_shown(f, string, "Oak");
    /* These chains include the string and listview A, but B stays. All
       doomed flags, including A's and its context's, must be reset. */
    assert_showsel_free_refused(f, c);
    assert_showsel_free_refused(f, string);
    unsigned before = freed;
    f.free(first_a);
    assert(!f.stopped() && freed == before + 4 && natives.size() == 7 && f.native(c) &&
           f.native(string) && f.native(c2) && f.native(b) && !f.native(a) &&
           !f.lng(string + GD_NEXTGADGET));
    assert_showsel_free_refused(f, c);
    set(f, b, { { GTLV_Selected, 0 } });
    assert(!f.stopped());
    assert_shown(f, string, "Cedar");
    assert_string_position(f, string, 70, 199, 160, 14);
    before = freed;
    f.free(first_b);
    assert(!f.stopped() && freed == before + 4 && natives.size() == 3 &&
           f.native(string) && f.native(c) && f.native(c2) && !f.lng(c2 + GD_NEXTGADGET));
    f.free(c);
    assert(!f.stopped() && natives.size() == 1 && f.native(c2));
    f.free(c2);
    assert(!f.stopped() && natives.empty() && native_blocks.empty() && !f.ctx.gadgets &&
           Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_listview_set()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 3 } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    Native *n = f.native(lv);
    assert(lv && !f.stopped());
    const ULONG inc = f.lng(c + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    const ULONG sc = f.lng(dec + GD_NEXTGADGET), pi = f.lng(sc + GD_SPECIALINFO);
    const unsigned sets = native_sets;
    set(f, lv, { { GTLV_Top, 5 } });
    assert(!f.stopped() && native_sets == sets + 1 && n->lv_top == 5 && n->scroller->top == 5 &&
           n->scroller->total == 20 && n->scroller->visible == 10);
    /* Inspect before any getter (which would settle mirrors itself). */
    assert(f.word(pi + PI_VERTPOT) == MAXPOT / 2 && f.word(pi + PI_VERTBODY) == MAXBODY / 2);
    set(f, lv, { { GTLV_Top, 100 } });
    assert(!f.stopped() && n->lv_top == 10 && n->scroller->top == 10 &&
           f.word(pi + PI_VERTPOT) == MAXPOT);
    set(f, lv, { { GTLV_Top, 0x8000 } });
    assert(!f.stopped() && n->lv_top == 0 && f.word(pi + PI_VERTPOT) == 0);
    set(f, lv, { { GTLV_MakeVisible, 100 } });
    assert(!f.stopped() && n->lv_top == 10 && f.word(pi + PI_VERTPOT) == MAXPOT);
    set(f, lv, { { GTLV_MakeVisible, 0x8000 } });
    assert(!f.stopped() && n->lv_top == 0 && f.word(pi + PI_VERTPOT) == 0);
    set(f, lv, { { GTLV_Top, 5 }, { GTLV_MakeVisible, 7 } });
    assert(!f.stopped() && n->lv_top == 5 && f.word(pi + PI_VERTPOT) == MAXPOT / 2);
    set(f, lv, { { GTLV_MakeVisible, 2 } });
    assert(!f.stopped() && n->lv_top == 2 && n->scroller->top == 2 &&
           f.word(pi + PI_VERTPOT) == (ULONG)2 * MAXPOT / 10);
    set(f, lv, { { GTLV_Selected, 0xFFFFFFFF } });
    assert(!f.stopped() && n->selected == -1);
    assert(get(f, lv, { { GTLV_Selected, OUT } }) == 1 && f.lng(OUT) == 0xFFFFFFFF);
    for (ULONG disabled : { 1u, 0u })
    {
        const unsigned before = native_sets;
        set(f, lv, { { GA_Disabled, disabled } });
        assert(!f.stopped() && native_sets == before + 1);
        for (ULONG g : { lv, sc, inc, dec })
            assert(!!(f.native(g)->gad.Flags & GFLG_DISABLED) == !!disabled &&
                   !!(f.word(g + GD_FLAGS) & GFLG_DISABLED) == !!disabled);
    }

    /* A new shorter list resets Top and preserves Selected. UpdateScroller
       uses ShownEntries=4, while the Visible getter still says 10. */
    guest_list(f, 4, 0x5300, 0x5400, f.s("Short"));
    set(f, lv, { { GTLV_Labels, 0x5300 } });
    assert(!f.stopped() && n->lv_top == 0 && n->selected == -1 && n->num_entries == 4 &&
           n->scroller->top == 0 && n->scroller->total == 4 && n->scroller->visible == 4 &&
           f.word(pi + PI_VERTPOT) == 0 && f.word(pi + PI_VERTBODY) == MAXBODY);
    assert(get(f, lv, { { GTLV_Top, OUT }, { GTLV_Labels, OUT + 4 },
                       { NDK_LV_TOTAL, OUT + 8 }, { NDK_LV_VISIBLE, OUT + 12 } }) == 4);
    assert(f.lng(OUT) == 0 && f.lng(OUT + 4) == 0x5300 && f.lng(OUT + 8) == 4 && f.lng(OUT + 12) == 10);
    set(f, lv, { { GTLV_Top, 100 } });
    assert(!f.stopped() && n->lv_top == 0 && f.word(pi + PI_VERTPOT) == 0);
}

/* Preserve the native edge cases identified in the design review: the
   positive item/spacing guards do not promise that a row actually fits. */
static void test_listview_geometry()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 1);
    m68k_write32(&f.ctx, Fixture::textattr + TA_NAME, f.s("topaz.font"));
    m68k_write16(&f.ctx, Fixture::textattr + TA_YSIZE, 8);
    f.newgad(10, 20, 160, 16, 0, Fixture::textattr, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_ItemHeight, 8 } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped());
    Native *n = f.native(lv);
    assert(n->gad.Height == 6 && num_items_fit(n) == 0 && shown_entries(n) == 0);
    set(f, lv, { { GTLV_MakeVisible, 0 } });
    assert(!f.stopped() && n->lv_top == 1 && n->scroller->top == 1 &&
           n->scroller->total == 1 && n->scroller->visible == 0);
    assert(get(f, lv, { { GTLV_Top, OUT }, { NDK_LV_VISIBLE, OUT + 4 } }) == 2 &&
           f.lng(OUT) == 1 && f.lng(OUT + 4) == 0);
    f.free(f.lng(c + GD_NEXTGADGET));

    /* Builder: WORD(32767+32767)=-2. Class: promoted WORD fields
       add to 65534. These are intentionally different divisors. */
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_ItemHeight, 32767 }, { LAYOUTA_Spacing, 32767 } });
    const ULONG large = f.create(LISTVIEW_KIND, c);
    assert(large && !f.stopped() && f.native(large)->gad.Height == 92 &&
           f.native(large)->itemheight == 32767 && f.native(large)->spacing == 32767 &&
           num_items_fit(f.native(large)) == 0);
}

static void test_listview_labels()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20, ENTRIES, NODES, f.s("Original"));
    guest_list(f, 6, 0x5300, 0x5400, f.s("Other"));
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Selected, 3 } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped());
    Native *n = f.native(lv);
    const size_t live = native_blocks.size();
    List *old = n->entries;
    APTR old_block = block_holding(old);
    unsigned allocations = native_allocations, sets = native_sets;
    set(f, lv, { { GTLV_Labels, ENTRIES } });
    assert(!f.stopped() && native_sets == sets + 1 && native_allocations == allocations + 2 &&
           n->entries != old && n->num_entries == 20 && native_blocks.size() == live &&
           !native_blocks.count(old_block) && blocks_during_set.count(old_block));
    assert(blocks_during_set.size() == live + 2 && set_entry_blocks.size() == 1 &&
           native_blocks.count(set_entry_blocks[0]));
    render(n);

    /* Detach, alter the same guest List, attach a fresh snapshot. */
    old_block = block_holding(n->entries);
    set(f, lv, { { GTLV_Labels, 0xFFFFFFFF } });
    assert(!f.stopped() && (IPTR)n->entries == ~(IPTR)0 && n->num_entries == 0 && n->selected == 3 &&
           native_blocks.size() == live - 1 && !native_blocks.count(old_block) &&
           blocks_during_set.count(old_block));
    guest_list(f, 5, ENTRIES, NODES, f.s("Changed while detached"));
    f.memory[NODES + LN_TYPE] = 0x7E;
    f.memory[NODES + LN_PRI] = (UBYTE)-12;
    allocations = native_allocations;
    set(f, lv, { { GTLV_Labels, ENTRIES } });
    assert(!f.stopped() && native_allocations == allocations + 2 && native_blocks.size() == live &&
           n->num_entries == 5 && n->selected == 3 &&
           std::strcmp(n->entries->lh_Head->ln_Name, "Changed while detached") == 0 &&
           n->entries->lh_Head->ln_Type == 0x7E && n->entries->lh_Head->ln_Pri == -12);
    assert(get(f, lv, { { GTLV_Labels, OUT } }) == 1 && f.lng(OUT) == ENTRIES);
    old_block = block_holding(n->entries);
    allocations = native_allocations;
    sets = native_sets;
    set(f, lv, { { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0x5300 }, { GTLV_Labels, ENTRIES } });
    assert(!f.stopped() && native_sets == sets + 1 && set_attributes == 3 &&
           native_allocations == allocations + 4 && native_blocks.size() == live &&
           blocks_during_set.size() == live + 4 && blocks_during_set.count(old_block) &&
           !native_blocks.count(old_block));
    assert(set_entry_blocks.size() == 3 && !native_blocks.count(set_entry_blocks[0]) &&
           !native_blocks.count(set_entry_blocks[1]) && native_blocks.count(set_entry_blocks[2]));
    assert(set_tags[0].ti_Data != set_tags[2].ti_Data &&
           (IPTR)n->entries == set_tags[2].ti_Data && n->num_entries == 5 && n->selected == 3);
    render(n);

    /* A final sentinel keeps none of the call's temporary snapshots. */
    set(f, lv, { { GTLV_Labels, 0x5300 }, { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0 } });
    assert(!f.stopped() && !n->entries && n->num_entries == 0 && native_blocks.size() == live - 1);
    assert(set_entry_blocks.size() == 2 && !native_blocks.count(set_entry_blocks[0]) &&
           !native_blocks.count(set_entry_blocks[1]));
    assert(get(f, lv, { { GTLV_Labels, OUT } }) == 1 && f.lng(OUT) == 0);
    set(f, lv, { { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0x5300 }, { GTLV_Labels, 0xFFFFFFFF } });
    assert(!f.stopped() && (IPTR)n->entries == ~(IPTR)0 && n->num_entries == 0 &&
           native_blocks.size() == live - 1);
    assert(set_entry_blocks.size() == 2 && !native_blocks.count(set_entry_blocks[0]) &&
           !native_blocks.count(set_entry_blocks[1]));
    assert(get(f, lv, { { GTLV_Labels, OUT } }) == 1 && f.lng(OUT) == 0xFFFFFFFF);
    /* NULL can also be followed by another attachment of the same address. */
    set(f, lv, { { GTLV_Labels, 0 } });
    guest_list(f, 2, ENTRIES, NODES, f.s("After NULL"));
    set(f, lv, { { GTLV_Labels, ENTRIES } });
    assert(!f.stopped() && n->num_entries == 2 && native_blocks.size() == live &&
           std::strcmp(n->entries->lh_Head->ln_Name, "After NULL") == 0);
    render(n);
    f.free(c);
    assert(native_blocks.empty());
}

static void test_listview_staging()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20);
    guest_list(f, 6, 0x5300, 0x5400, f.s("Replacement"));
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Top, 5 }, { GTLV_Selected, 3 } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped());
    Native *n = f.native(lv);
    List *old = n->entries;
    const auto live = native_blocks;
    const unsigned calls = native_calls, sets = native_sets;
    unsigned allocations = native_allocations;
    /* Both earlier snapshots must be discarded when a later one is bad;
       other staged attributes must not reach the class either. */
    set(f, lv, { { GTLV_Top, 1 }, { GTLV_Selected, 4 }, { GA_Disabled, 1 },
                 { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0x5300 }, { GTLV_Labels, 0xFFF8 } });
    assert(f.stopped() && native_calls == calls && native_sets == sets && native_blocks == live &&
           native_allocations == allocations + 3 && n->entries == old && n->num_entries == 20 &&
           n->lv_top == 5 && n->selected == 3 && !(n->gad.Flags & GFLG_DISABLED));
    render(n);
    /* A malformed tag chain after the snapshots takes the same rollback. */
    allocations = native_allocations;
    set(f, lv, { { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0x5300 }, { TAG_MORE, 0xFFFFFFF0 } });
    assert(f.stopped() && native_calls == calls && native_blocks == live && n->entries == old &&
           native_allocations == allocations + 3 && n->lv_top == 5);
    /* Fail the tag allocation, then each successive snapshot allocation. */
    for (int fail = 0; fail < 4; ++fail)
    {
        allocations = native_allocations;
        allocations_before_failure = fail;
        set(f, lv, { { GTLV_Labels, ENTRIES }, { GTLV_Labels, 0x5300 }, { GTLV_Labels, ENTRIES } });
        assert(!f.stopped() && native_calls == calls && native_sets == sets && native_blocks == live &&
               allocations_before_failure == -1 && native_allocations == allocations + fail &&
               n->entries == old && n->num_entries == 20 && n->lv_top == 5 && n->selected == 3);
        render(n);
    }
    /* The next ordinary call still works and frees the original only after
       native GadTools has used it at entry. */
    set(f, lv, { { GTLV_Labels, 0x5300 } });
    assert(!f.stopped() && native_sets == sets + 1 && native_blocks.size() == live.size() &&
           n->entries != old && n->num_entries == 6 && n->lv_top == 0 && n->selected == 3);
    render(n);
}

static void test_listview_enable_free()
{
    Fixture f;
    const ULONG c = f.context();
    guest_list(f, 20, ENTRIES, NODES, f.s("Held until FreeGadgets"));
    f.newgad(10, 20, 160, 100, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTLV_Labels, ENTRIES }, { GTLV_Top, 5 } });
    const ULONG lv = f.create(LISTVIEW_KIND, c);
    assert(lv && !f.stopped());
    Native *n = f.native(lv);
    List *copy = n->entries;
    const auto live = native_blocks;
    const ULONG inc = f.lng(c + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    const ULONG sc = f.lng(dec + GD_NEXTGADGET), pi = f.lng(sc + GD_SPECIALINFO);
    static int native_window;
    const ULONG win = 0x3BC0;
    assert(Kaffeine_ShadowAdd(&f.ctx, win, &native_window, KAFFEINE_SHADOW_WINDOW));
    changed.clear();
    for (ULONG lvo : { 174u, 186u })
    {
        unsigned calls = native_calls, sets = native_sets;
        assert(f.call_at(Fixture::intuition, lvo, { lv, 0, 0 }) == 0 && !f.stopped() &&
               native_calls == calls && native_sets == sets);
        assert(f.call_at(Fixture::intuition, lvo, { lv, win, 0x3BD0 }) == 0 && !f.stopped());
        assert(native_calls == calls + 1 && native_sets == sets + 1 && changed.empty() &&
               n->entries == copy && native_blocks == live);
        for (ULONG g : { lv, sc, inc, dec })
            assert(!!(f.native(g)->gad.Flags & GFLG_DISABLED) == (lvo == 174) &&
                   !!(f.word(g + GD_FLAGS) & GFLG_DISABLED) == (lvo == 174));
        assert(f.lng(sc + GD_SPECIALINFO) == pi && f.word(pi + PI_FLAGS) == (AUTOKNOB | FREEVERT) &&
               f.word(pi + PI_VERTPOT) == MAXPOT / 2 && f.word(pi + PI_VERTBODY) == MAXBODY / 2);
    }
    /* The returned last gadget is not the start of its native batch. */
    unsigned calls = native_calls;
    f.free(lv);
    assert(f.stopped() && native_calls == calls && native_blocks == live && n->entries == copy);
    f.free(c);
    assert(!f.stopped() && native_blocks.empty() && native_sizes.empty() && natives.empty() &&
           freed == 5 && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    for (ULONG g : { c, lv, sc, inc, dec })
        assert(!Kaffeine_ShadowNative(&f.ctx, g, KAFFEINE_SHADOW_GADGET));
    Kaffeine_ShadowForget(&f.ctx, win, KAFFEINE_SHADOW_WINDOW);
}

/* Every attribute of the autodoc of every kind, as the classes take it, in
   the native gadget, in its mirror and through GT_GetGadgetAttrsA() */
static void test_set_kinds()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    const ULONG checkbox = f.create(CHECKBOX_KIND, button);
    const ULONG low = f.s("Low"), high = f.s("High"), cold = f.s("Cold"), warm = f.s("Warm"), hot = f.s("Hot");
    m68k_write32(&f.ctx, Fixture::labels, low);
    m68k_write32(&f.ctx, Fixture::labels + 4, high);
    m68k_write32(&f.ctx, Fixture::labels + 8, 0);
    const ULONG other = Fixture::labels + 16;
    m68k_write32(&f.ctx, other, cold);
    m68k_write32(&f.ctx, other + 4, warm);
    m68k_write32(&f.ctx, other + 8, hot);
    m68k_write32(&f.ctx, other + 12, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Active, 1 } });
    const ULONG cycle = f.create(CYCLE_KIND, checkbox);
    f.tagitems({ { GTMX_Labels, other } });
    const ULONG mx = f.create(MX_KIND, cycle);
    f.tagitems({ { GTSL_Min, 0 }, { GTSL_Max, 10 }, { GTSL_Level, 5 } });
    const ULONG slider = f.create(SLIDER_KIND, mx);
    f.tagitems({ { GTSC_Total, 100 }, { GTSC_Visible, 10 } });
    const ULONG scroller = f.create(SCROLLER_KIND, slider);
    f.tagitems({ { GTST_MaxChars, 5 } });
    const ULONG string = f.create(STRING_KIND, scroller);
    f.tagitems({});
    const ULONG integer = f.create(INTEGER_KIND, string);
    assert(button && checkbox && cycle && mx && slider && scroller && string && integer && !f.stopped());
    ULONG a = 0, b = 0;
    auto get1 = [&](ULONG gad, ULONG tag) {
        m68k_write32(&f.ctx, OUT, UNTOUCHED);
        assert(get(f, gad, { { tag, OUT } }) == 1);
        return f.lng(OUT);
    };

    /* the button takes the first GA_Disabled; the mirror follows */
    set(f, button, { { GA_Disabled, TRUE } });
    assert(!f.stopped() && (f.native(button)->gad.Flags & GFLG_DISABLED) && (f.word(button + GD_FLAGS) & GFLG_DISABLED));
    set(f, button, { { GA_Disabled, FALSE }, { GA_Disabled, TRUE } });
    assert(!(f.word(button + GD_FLAGS) & GFLG_DISABLED) && get1(button, GA_Disabled) == 0);
    set(f, checkbox, { { GTCB_Checked, TRUE }, { GA_Disabled, TRUE } });
    assert((f.word(checkbox + GD_FLAGS) & GFLG_SELECTED) && get1(checkbox, GTCB_Checked) == 1 &&
           get1(checkbox, GA_Disabled) == 1);

    /* a cycle: new labels, its choice back to the first, then a choice of
       the same list; a choice beyond the labels becomes the first */
    set(f, cycle, { { GTCY_Labels, other } });
    a = get1(cycle, GTCY_Labels);
    b = get1(cycle, GTCY_Active);
    assert(a == other && b == 0 && count_labels(f.native(cycle)->labels) == 3);
    set(f, cycle, { { GTCY_Labels, Fixture::labels }, { GTCY_Active, 1 } });
    assert(get1(cycle, GTCY_Labels) == Fixture::labels && get1(cycle, GTCY_Active) == 1);
    set(f, cycle, { { GTCY_Active, 7 } });
    assert(get1(cycle, GTCY_Active) == 0);
    set(f, cycle, { { GTCY_Labels, 0 } });
    assert(!f.stopped() && !f.native(cycle)->labels && get1(cycle, GTCY_Labels) == 0);

    /* an mx ignores a choice beyond its labels */
    set(f, mx, { { GTMX_Active, 2 } });
    assert(get1(mx, GTMX_Active) == 2);
    set(f, mx, { { GTMX_Active, 9 } });
    assert(get1(mx, GTMX_Active) == 2);

    /* the knob of a PropInfo in its mirror, right after a set and before
       any get, which would bring the mirror up to date itself */
    auto knob_mirrored = [&](ULONG gad, UWORD pot, UWORD body) {
        const PropInfo &pi = f.native(gad)->pi;
        const ULONG m = f.lng(gad + GD_SPECIALINFO);
        const bool horiz = pi.Flags & FREEHORIZ;
        return (horiz ? pi.HorizPot : pi.VertPot) == pot && (horiz ? pi.HorizBody : pi.VertBody) == body &&
               f.word(m + PI_HORIZPOT) == pi.HorizPot && f.word(m + PI_VERTPOT) == pi.VertPot &&
               f.word(m + PI_HORIZBODY) == pi.HorizBody && f.word(m + PI_VERTBODY) == pi.VertBody;
    };

    /* the slider clamps its level as it comes: 11 positions, the last */
    set(f, slider, { { GTSL_Level, 50 } });
    assert(!f.stopped() && knob_mirrored(slider, MAXPOT, MAXBODY / 11));
    assert(get1(slider, GTSL_Level) == 10);
    /* 51 positions, the sixth */
    set(f, slider, { { GTSL_Min, (ULONG)-20 }, { GTSL_Level, (ULONG)-15 }, { GTSL_Max, 30 } });
    assert(!f.stopped() && knob_mirrored(slider, (UWORD)(5UL * MAXPOT / 50), MAXBODY / 51));
    assert(get1(slider, GTSL_Min) == (ULONG)-20 && get1(slider, GTSL_Level) == (ULONG)-15 && get1(slider, GTSL_Max) == 30);

    /* the scroller keeps Top within Total less Visible, and its PropInfo
       mirror follows */
    set(f, scroller, { { GTSC_Top, 95 } });
    assert(!f.stopped() && knob_mirrored(scroller, MAXPOT, (UWORD)(10UL * MAXBODY / 100)));
    assert(get1(scroller, GTSC_Top) == 90);
    set(f, scroller, { { GTSC_Total, 50 }, { GTSC_Visible, 20 }, { GTSC_Top, 40 } });
    assert(!f.stopped() && knob_mirrored(scroller, MAXPOT, (UWORD)(20UL * MAXBODY / 50)));
    assert(get1(scroller, GTSC_Top) == 30 && get1(scroller, GTSC_Total) == 50 && get1(scroller, GTSC_Visible) == 20);

    /* a string cut to its MaxChars before the native class sees it, and
       empty for NULL; an integer */
    set(f, string, { { GTST_String, f.s("Hello again") } });
    assert(!string_sets.empty() && string_sets.back().text == "Hello");
    const ULONG si = f.lng(string + GD_SPECIALINFO);
    assert(!f.stopped() && f.str(f.lng(si + SI_BUFFER)) == "Hello" && f.str(get1(string, GTST_String)) == "Hello");
    set(f, string, { { GTST_String, 0 } });
    assert(f.str(f.lng(si + SI_BUFFER)).empty());
    set(f, integer, { { GTIN_Number, (ULONG)-12345 } });
    const ULONG isi = f.lng(integer + GD_SPECIALINFO);
    assert(get1(integer, GTIN_Number) == (ULONG)-12345 && f.lng(isi + SI_LONGINT) == (ULONG)-12345 &&
           f.str(f.lng(isi + SI_BUFFER)) == "-12345");
    /* attributes of another kind are left out */
    const unsigned sets = native_sets;
    set(f, integer, { { GTST_String, f.s("text") }, { GTSL_Level, 3 } });
    assert(!f.stopped() && native_sets == sets && f.lng(isi + SI_LONGINT) == (ULONG)-12345);
}

/* Texts, numbers, formats, pens and palettes, and what the bridge refuses
   before native GadTools sees anything */
static void test_set_conversions()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    const ULONG hello = f.s("Hello");
    f.tagitems({ { GTTX_Text, hello }, { GTTX_CopyText, TRUE } });
    const ULONG text = f.create(TEXT_KIND, c);
    f.tagitems({ { GTNM_Number, 5 } });
    const ULONG number = f.create(NUMBER_KIND, text);
    const UBYTE pens[4] = { 3, 2, 1, 0 };
    std::memcpy(f.memory.data() + Fixture::colors, pens, 4);
    f.tagitems({ { GTPA_NumColors, 4 }, { GTPA_ColorTable, Fixture::colors } });
    const ULONG palette = f.create(PALETTE_KIND, number);
    f.tagitems({ { GTSL_Max, 10 } });
    const ULONG slider = f.create(SLIDER_KIND, palette);
    assert(text && number && palette && slider && !f.stopped());
    auto get1 = [&](ULONG gad, ULONG tag) {
        m68k_write32(&f.ctx, OUT, UNTOUCHED);
        assert(get(f, gad, { { tag, OUT } }) == 1);
        return f.lng(OUT);
    };
    Native *nt = f.native(text), *nn = f.native(number), *np = f.native(palette);

    /* a new text is the program's own, also after a copied one */
    const ULONG world = f.s("World");
    set(f, text, { { GTTX_Text, world } });
    assert(!f.stopped() && nt->copied.empty() && nt->text == (CONST_STRPTR)f.memory.data() + world &&
           get1(text, GTTX_Text) == world);
    set(f, text, { { GTTX_Text, 0 } });
    assert(!nt->text && get1(text, GTTX_Text) == 0);
    /* numbers, and a format copied for the class */
    set(f, number, { { GTNM_Number, (ULONG)-77 }, { GTNM_Number, 66 } });
    assert(nn->number == 66 && get1(number, GTNM_Number) == 66);
    const ULONG fmt = f.s("%04ld");
    set(f, number, { { GTNM_Format, fmt } });
    assert(!f.stopped() && nn->format && !f.guest(nn->format) && std::strcmp(nn->format, "%04ld") == 0);
    f.memory[fmt] = 'X';
    assert(std::strcmp(nn->format, "%04ld") == 0);
    /* pens beyond the DrawInfo pens are left out, also at creation */
    set(f, number, { { GTNM_FrontPen, 2 }, { GTNM_BackPen, 15 }, { GTNM_Justification, GTJ_RIGHT } });
    assert(!f.stopped() && nn->frontpen == 2 && nn->backpen == 0 && nn->justification == GTJ_RIGHT);
    set(f, number, { { GTNM_BackPen, 1 } });
    assert(!f.stopped() && nn->frontpen == 2 && nn->backpen == 1 && nn->justification == GTJ_RIGHT);
    set(f, text, { { GTTX_FrontPen, 3 }, { GTTX_BackPen, 2 }, { GTTX_Justification, GTJ_CENTER } });
    assert(!f.stopped() && nt->frontpen == 3 && nt->backpen == 2 && nt->justification == GTJ_CENTER);
    f.tagitems({ { GTNM_FrontPen, NUMDRIPENS }, { GTNM_BackPen, 1 }, { GTNM_Justification, GTJ_RIGHT } });
    const ULONG pens_dropped = f.create(NUMBER_KIND, slider);
    assert(pens_dropped && !f.stopped() && !native_tag(GTNM_FrontPen) &&
           f.native(pens_dropped)->frontpen == 1 && f.native(pens_dropped)->backpen == 1 &&
           f.native(pens_dropped)->justification == GTJ_RIGHT);
    f.tagitems({ { GTTX_FrontPen, 3 }, { GTTX_BackPen, 2 }, { GTTX_Justification, GTJ_CENTER } });
    const ULONG text_pens = f.create(TEXT_KIND, pens_dropped);
    assert(text_pens && !f.stopped() && f.native(text_pens)->frontpen == 3 &&
           f.native(text_pens)->backpen == 2 && f.native(text_pens)->justification == GTJ_CENTER);

    /* a palette: a pen of its table, an offset, a new table, and a table
       with less room in guest memory than the class reads */
    set(f, palette, { { GTPA_Color, 1 } });
    assert(np->color == 2 && get1(palette, GTPA_Color) == 1);
    set(f, palette, { { GTPA_ColorOffset, 2 } });
    assert(!f.stopped() && np->offset == 2 && get1(palette, GTPA_ColorOffset) == 2);
    const ULONG table2 = Fixture::colors + 8;
    set(f, palette, { { GTPA_ColorTable, table2 } });
    assert(!f.stopped() && get1(palette, GTPA_ColorTable) == table2);
    const unsigned sets = native_sets;
    const unsigned calls = native_calls;
    const ULONG near_end = (ULONG)f.memory.size() - 5;
    set(f, palette, { { GTPA_ColorTable, near_end } });
    assert(f.stopped() && native_sets == sets && native_calls == calls);
    assert(get1(palette, GTPA_ColorTable) == table2);

    /* refusals, all before native GadTools: a NULL GTNM_Format, a format
       of a pointer, a text outside guest memory, a function of 68k code */
    set(f, number, { { GTNM_Number, 1 }, { GTNM_Format, 0 } });
    assert(f.stopped() && native_sets == sets && nn->number == 66 && get1(number, GTNM_Number) == 66);
    set(f, number, { { GTNM_Format, f.s("%s") } });
    assert(f.stopped() && native_sets == sets);
    set(f, text, { { GTTX_Text, 0xFFFFFFF0 } });
    assert(f.stopped() && native_sets == sets);
    set(f, slider, { { GTSL_DispFunc, 0x1000 } });
    assert(f.stopped() && native_sets == sets);
    /* a NULL DispFunc, and the level format and justification that AROS's
       slider leaves alone */
    set(f, slider, { { GTSL_DispFunc, 0 }, { GTSL_LevelFormat, f.s("%2ld") }, { GTSL_Justification, GTJ_CENTER } });
    assert(!f.stopped() && native_sets == sets + 1);
}

/* Who gets the attributes: a gadget of a call, its window checked; not
   the context, a generic gadget or a part, nor anything else */
static void test_set_targets()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTSC_Arrows, 12 } });
    const ULONG scroller = f.create(SCROLLER_KIND, c);
    f.tagitems({});
    const ULONG generic = f.create(GENERIC_KIND, scroller);
    assert(scroller && generic && !f.stopped());
    const ULONG arrow = f.lng(c + GD_NEXTGADGET);
    unsigned sets = native_sets;

    set(f, generic, { { GA_Disabled, TRUE } });
    set(f, c, { { GA_Disabled, TRUE } });
    set(f, arrow, { { GA_Disabled, TRUE } });
    assert(!f.stopped() && native_sets == sets);
    /* NULL: nothing, as GadTools */
    assert(f.call(42, { 0, 0, 0, Fixture::more }) == 0 && f.call(42, { scroller, 0, 0, 0 }) == 0);
    assert(!f.stopped() && native_sets == sets);
    /* not a gadget of the bridge, a list in a cycle or outside guest
       memory, a window that is not one of the program's */
    set(f, Fixture::raw, { { GA_Disabled, TRUE } });
    assert(f.stopped());
    f.tagitems({ { GTSC_Top, 5 }, { TAG_MORE, Fixture::more } }, Fixture::more);
    f.call(42, { scroller, 0, 0, Fixture::more });
    assert(f.stopped());
    f.tagitems({ { GTSC_Top, 5 }, { TAG_MORE, 0xFFFFFFF0 } }, Fixture::more);
    f.call(42, { scroller, 0, 0, Fixture::more });
    assert(f.stopped());
    set(f, scroller, { { GTSC_Top, 5 } }, Fixture::raw);
    assert(f.stopped() && native_sets == sets);
    /* a 68k window is checked; native GadTools gets neither it nor req */
    static int window;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BC0, &window, KAFFEINE_SHADOW_WINDOW));
    f.tagitems({ { TAG_IGNORE, 0 }, { GTSC_Total, 50 }, { TAG_SKIP, 1 }, { GTSC_Top, 999 }, { GTSC_Top, 7 } }, Fixture::more);
    f.call(42, { scroller, 0x3BC0, 0x3BD0, Fixture::more });
    assert(!f.stopped() && native_sets == sets + 1);
    assert(f.native(scroller)->top == 7 && f.native(scroller)->total == 50);
    Kaffeine_ShadowForget(&f.ctx, 0x3BC0, KAFFEINE_SHADOW_WINDOW);
    sets = native_sets;
    /* an empty label array of a cycle, before native GadTools */
    f.newgad(10, 30, 80, 14, 0, 0, 2, 0, f.vi, 0);
    m68k_write32(&f.ctx, Fixture::labels, f.s("One"));
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    const ULONG cycle = f.create(CYCLE_KIND, generic);
    m68k_write32(&f.ctx, Fixture::labels + 8, 0);
    set(f, cycle, { { GTCY_Labels, Fixture::labels + 8 } });
    assert(f.stopped() && native_sets == sets && count_labels(f.native(cycle)->labels) == 1);
}

/* What CreateGadgetA() now checks or changes for the classes: an empty
   label array and an active choice beyond the labels of a cycle, the room
   of a colour table from its offset */
static void test_set_creation()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    m68k_write32(&f.ctx, Fixture::labels, f.s("A"));
    m68k_write32(&f.ctx, Fixture::labels + 4, f.s("B"));
    m68k_write32(&f.ctx, Fixture::labels + 8, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Active, 5 } });
    const ULONG cycle = f.create(CYCLE_KIND, c);
    assert(cycle && !f.stopped() && f.native(cycle)->active == 0 && native_tag(GTCY_Active)->ti_Data == 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels }, { GTCY_Active, 1 } });
    const ULONG second = f.create(CYCLE_KIND, cycle);
    assert(second && f.native(second)->active == 1);
    f.tagitems({});
    const ULONG none = f.create(CYCLE_KIND, second);
    assert(none && !f.stopped() && !f.native(none)->labels);
    f.tagitems({ { GTCY_Labels, Fixture::labels + 8 } });
    f.create(CYCLE_KIND, none);
    assert(f.stopped());
    /* a table needs guest memory for 256 colours past its offset, whatever
       its count, as the class reads it at its choices, which are UBYTEs:
       258 bytes for 4 colours from offset 2 */
    const ULONG end = (ULONG)f.memory.size();
    unsigned calls = native_calls;
    f.tagitems({ { GTPA_NumColors, 4 }, { GTPA_ColorOffset, 2 }, { GTPA_ColorTable, end - 257 } });
    f.create(PALETTE_KIND, none);
    assert(f.stopped() && native_calls == calls);
    f.tagitems({ { GTPA_NumColors, 4 }, { GTPA_ColorOffset, 2 }, { GTPA_ColorTable, end - 258 } });
    const ULONG palette = f.create(PALETTE_KIND, none);
    assert(palette && !f.stopped());
    /* an absent pen stays the choice, which that room covers */
    const unsigned sets = native_sets;
    set(f, palette, { { GTPA_Color, 6 } });
    assert(!f.stopped() && native_sets == sets + 1 && f.native(palette)->color == 6);
}

/* Palette tables: the colours, the offset and the choice that the class
   keeps, including creation's relayout. A table needs guest memory for 256
   colours past its offset, or for all of them when it has more: the class
   reads it at its choice and at its old choice, plus the offset, and both
   can be any UBYTE, as a pen that is not in the table stays the choice.
   Refusals precede every native call. */
static void test_set_palettes()
{
    Fixture f;
    const ULONG c = f.context(), end = (ULONG)f.memory.size(), last = end - 256;
    const UBYTE pens[4] = { 10, 20, 30, 40 };
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTPA_NumColors, 4 } });
    const ULONG palette = f.create(PALETTE_KIND, c);
    assert(palette && !f.stopped());
    Native *n = f.native(palette);
    auto taken = [&](ULONG gad, std::initializer_list<std::pair<ULONG, ULONG>> list) {
        const unsigned calls = native_calls, sets = native_sets;
        set(f, gad, list);
        assert(!f.stopped() && native_calls == calls + 1 && native_sets == sets + 1);
    };
    auto refused = [&](ULONG gad, std::initializer_list<std::pair<ULONG, ULONG>> list) {
        const unsigned calls = native_calls, sets = native_sets;
        set(f, gad, list);
        assert(f.stopped() && native_calls == calls && native_sets == sets);
    };
    auto color = [&](ULONG gad) {
        assert(get(f, gad, { { GTPA_Color, OUT } }) == 1 && !f.stopped());
        return f.lng(OUT);
    };

    /* a table in the last 256 bytes of guest memory, and not one byte later */
    std::memcpy(f.memory.data() + last, pens, sizeof(pens));
    taken(palette, { { GTPA_ColorTable, last }, { GTPA_Color, 30 } });
    assert(n->colors == f.memory.data() + last && n->color == 2 && color(palette) == 30);
    refused(palette, { { GTPA_ColorTable, last + 1 } });
    assert(n->colors == f.memory.data() + last && n->color == 2);

    /* an absent pen stays the choice: the getter reads table[pen] */
    f.memory[last + 7] = 99;
    taken(palette, { { GTPA_Color, 7 } });
    assert(n->color == 7 && color(palette) == 99);
    /* two in one call: the class keeps the first pen as the old choice,
       which the table's room covers too */
    taken(palette, { { GTPA_Color, 255 }, { GTPA_Color, 10 } });
    assert(n->color == 0 && color(palette) == 10);

    /* without a table the choice is pen less offset; a later table keeps
       that choice when no GTPA_Color comes with it, and needs room for 256
       colours past the offset */
    taken(palette, { { GTPA_ColorTable, 0 }, { GTPA_ColorOffset, 2 }, { GTPA_Color, 9 } });
    assert(!n->colors && n->color == 7 && color(palette) == 9);
    refused(palette, { { GTPA_ColorTable, last } });
    assert(!n->colors && n->color == 7);
    std::memcpy(f.memory.data() + last - 2, pens, sizeof(pens));
    f.memory[last - 2 + 7] = 77;
    taken(palette, { { GTPA_ColorTable, last - 2 } });
    assert(n->color == 7 && color(palette) == 77);
    taken(palette, { { GTPA_ColorOffset, 0 }, { GTPA_ColorTable, last } });
    assert(n->offset == 0 && n->color == 7 && color(palette) == 99);

    /* a UWORD count beyond a WORD search index cannot take a table, even
       where the table would have room for all the colours */
    f.tagitems({ { GTPA_NumColors, 32768 } });
    const ULONG large = f.create(PALETTE_KIND, palette);
    assert(large && !f.stopped() && f.native(large)->numcolors == 32768 && !f.native(large)->colors);
    refused(large, { { GTPA_ColorTable, Fixture::colors } });
    assert(!f.native(large)->colors);
    /* more than 256 colours need room for all of them past the offset */
    f.tagitems({ { GTPA_NumColors, 300 } });
    const ULONG many = f.create(PALETTE_KIND, large);
    assert(many && !f.stopped());
    refused(many, { { GTPA_ColorTable, end - 299 } });
    taken(many, { { GTPA_ColorTable, end - 300 } });

    /* the class keeps NumColors as a UWORD, also at creation */
    std::memcpy(f.memory.data() + last, pens, sizeof(pens));
    f.tagitems({ { GTPA_NumColors, 0x10004 }, { GTPA_ColorTable, last }, { GTPA_Color, 20 } });
    const ULONG wrapped = f.create(PALETTE_KIND, many);
    assert(wrapped && !f.stopped() && f.native(wrapped)->numcolors == 4 && color(wrapped) == 20);
    taken(wrapped, { { GTPA_ColorTable, last } });
    assert(f.native(wrapped)->color == 1);

    /* an odd depth without a table is left to GadTools, but the bridge
       cannot know its shifted count well enough to take a table */
    f.tagitems({ { GTPA_Depth, 64 } });
    const ULONG odd = f.create(PALETTE_KIND, wrapped);
    assert(odd && !f.stopped() && !f.native(odd)->colors);
    refused(odd, { { GTPA_ColorTable, Fixture::colors } });
    assert(!f.native(odd)->colors);

    /* an offset of 2 needs two bytes more */
    refused(wrapped, { { GTPA_ColorOffset, 2 } });
    assert(f.native(wrapped)->offset == 0 && f.native(wrapped)->colors == f.memory.data() + last);
    const UBYTE offset_pens[6] = { 10, 20, 30, 40, 50, 60 };
    std::memcpy(f.memory.data() + last - 2, offset_pens, sizeof(offset_pens));
    taken(wrapped, { { GTPA_ColorTable, last - 2 }, { GTPA_ColorOffset, 2 }, { GTPA_Color, 30 } });
    assert(f.native(wrapped)->offset == 2 && f.native(wrapped)->color == 2 && color(wrapped) == 30);

    /* creation relayout takes an absent pen above colors-1 back to zero; a
       setter keeps it, which the room of the table covers */
    std::memcpy(f.memory.data() + last, pens, sizeof(pens));
    f.memory[last + 255] = 55;
    f.tagitems({ { GTPA_NumColors, 4 }, { GTPA_ColorTable, last }, { GTPA_Color, 255 } });
    const ULONG laid_out = f.create(PALETTE_KIND, odd);
    assert(laid_out && !f.stopped() && f.native(laid_out)->color == 0 && color(laid_out) == 10);
    taken(laid_out, { { GTPA_Color, 255 } });
    assert(f.native(laid_out)->color == 255 && color(laid_out) == 55);
}

/* Native MaxChars can change without another buffer. Strings use the
   smaller room; integers may use only the MaxChars that actually fits. */
static void test_set_strings()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({ { GTST_MaxChars, 5 }, { GTST_String, f.s("Start") } });
    const ULONG string = f.create(STRING_KIND, c);
    f.tagitems({ { GTIN_MaxChars, 11 }, { GTIN_Number, 42 } });
    const ULONG integer = f.create(INTEGER_KIND, string);
    assert(string && integer && !f.stopped());
    Native *s = f.native(string), *i = f.native(integer);
    const ULONG si = f.lng(string + GD_SPECIALINFO), isi = f.lng(integer + GD_SPECIALINFO);
    const ULONG buffer = f.lng(si + SI_BUFFER), ibuffer = f.lng(isi + SI_BUFFER);
    const ULONG longtext = f.s("Longer than the original buffer");

    /* Even an increased native MaxChars must not let the bridge send more
       than the original six-byte buffer takes. */
    s->si.MaxChars = 50;
    unsigned sets = native_sets;
    set(f, string, { { GTST_String, longtext } });
    assert(!f.stopped() && native_sets == sets + 1 && std::strcmp((const char *)s->buffer, "Longe") == 0 &&
           f.lng(si + SI_BUFFER) == buffer && f.str(buffer) == "Longe" && f.word(si + SI_MAXCHARS) == 6);
    i->si.MaxChars = 50;
    sets = native_sets;
    unsigned calls = native_calls;
    set(f, integer, { { GTIN_Number, (ULONG)-12345 } });
    assert(f.stopped() && native_sets == sets && native_calls == calls && i->si.LongInt == 42 &&
           f.str(ibuffer) == "42" && std::strcmp((const char *)i->buffer, "42") == 0);

    /* a smaller native room cuts the string further */
    s->si.MaxChars = 4;
    set(f, string, { { GTST_String, longtext } });
    assert(!f.stopped() && f.str(buffer) == "Lon" && f.word(si + SI_MAXCHARS) == 4);
    s->si.MaxChars = 0;
    sets = native_sets;
    set(f, string, { { GTST_String, longtext } });
    assert(!f.stopped() && native_sets == sets + 1 && std::strcmp((const char *)s->buffer, "Lon") == 0 &&
           f.str(buffer).empty() &&
           f.word(si + SI_MAXCHARS) == 0);
    for (WORD none : { (WORD)0, (WORD)-1 })
    {
        i->si.MaxChars = none;
        sets = native_sets;
        calls = native_calls;
        set(f, integer, { { GTIN_Number, 123 } });
        assert(f.stopped() && native_sets == sets && native_calls == calls && i->si.LongInt == 42 &&
               f.str(ibuffer) == "42");
    }

    /* one byte is enough for an integer's empty text, and a smaller
       MaxChars keeps the prefix that set_buffer_str() copies */
    i->si.MaxChars = 1;
    set(f, integer, { { GTIN_Number, 123 } });
    assert(!f.stopped() && i->buffer[0] == 0 && f.str(ibuffer).empty());
    i->si.MaxChars = 5;
    set(f, integer, { { GTIN_Number, (ULONG)-12345 } });
    assert(!f.stopped() && f.str(ibuffer) == "-123");

    /* LONG_MIN gets its own digits, which fill a MaxChars of 12 */
    i->si.MaxChars = 12;
    set(f, integer, { { GTIN_Number, 0x80000000 } });
    const char expected[] = "-2147483648";
    assert(!f.stopped() && i->si.LongInt == (LONG)0x80000000 && f.lng(isi + SI_LONGINT) == 0x80000000 &&
           std::memcmp(i->buffer, expected, sizeof(expected)) == 0 && f.str(ibuffer) == expected);
}

/* Copies that a class keeps join the batch only when the call happens. */
static void test_set_staging()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    m68k_write32(&f.ctx, Fixture::labels, f.s("Old"));
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);
    const ULONG replacement = Fixture::labels + 16, empty = Fixture::labels + 32;
    m68k_write32(&f.ctx, replacement, f.s("New"));
    m68k_write32(&f.ctx, replacement + 4, 0);
    m68k_write32(&f.ctx, empty, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    const ULONG cycle = f.create(CYCLE_KIND, c);
    f.tagitems({});
    const ULONG number = f.create(NUMBER_KIND, cycle);
    assert(cycle && number && !f.stopped());
    Native *cy = f.native(cycle), *nm = f.native(number);
    STRPTR *old_labels = cy->labels;
    CONST_STRPTR old_format = nm->format;
    const ULONG format = f.s("%04ld"), second = f.s("%2ld");
    const size_t live = native_blocks.size();
    const unsigned calls = native_calls, sets = native_sets;
    unsigned allocations = native_allocations;

    /* one staged label array, then a refused attribute */
    set(f, cycle, { { GTCY_Labels, replacement }, { GTCY_Labels, empty } });
    assert(f.stopped() && native_calls == calls && native_sets == sets && native_blocks.size() == live &&
           native_allocations >= allocations + 2 && cy->labels == old_labels);
    allocations = native_allocations;
    set(f, number, { { GTNM_Format, format }, { GTNM_Format, 0 } });
    assert(f.stopped() && native_calls == calls && native_sets == sets && native_blocks.size() == live &&
           native_allocations == allocations + 2 && nm->format == old_format);

    /* tags and a first copy succeed; the second copy runs out of memory */
    allocations = native_allocations;
    allocations_before_failure = 2;
    set(f, cycle, { { GTCY_Labels, replacement }, { GTCY_Labels, replacement } });
    assert(!f.stopped() && native_calls == calls && native_sets == sets && native_blocks.size() == live &&
           native_allocations == allocations + 2 && allocations_before_failure == -1 && cy->labels == old_labels);
    allocations = native_allocations;
    allocations_before_failure = 2;
    set(f, number, { { GTNM_Format, format }, { GTNM_Format, second } });
    assert(!f.stopped() && native_calls == calls && native_sets == sets && native_blocks.size() == live &&
           native_allocations == allocations + 2 && allocations_before_failure == -1 && nm->format == old_format);

    /* a successful call keeps its copy until FreeGadgets() reads it */
    set(f, cycle, { { GTCY_Labels, replacement } });
    assert(!f.stopped() && native_sets == sets + 1 && native_blocks.size() == live + 1 &&
           cy->labels != old_labels && std::strcmp(cy->labels[0], "New") == 0);
    set(f, number, { { GTNM_Format, format } });
    assert(!f.stopped() && native_sets == sets + 2 && native_blocks.size() == live + 2 &&
           nm->format != old_format && std::strcmp(nm->format, "%04ld") == 0);
    f.free(c);
    assert(native_blocks.empty());
}

/* Setter limits and malformed input, before native GadTools gets a list. */
static void test_set_boundaries()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    m68k_write32(&f.ctx, Fixture::labels, f.s("Old"));
    m68k_write32(&f.ctx, Fixture::labels + 4, 0);
    f.tagitems({ { GTCY_Labels, Fixture::labels } });
    const ULONG cycle = f.create(CYCLE_KIND, button);
    f.tagitems({ { GTST_MaxChars, 5 }, { GTST_String, f.s("Start") } });
    const ULONG string = f.create(STRING_KIND, cycle);
    f.tagitems({});
    const ULONG slider = f.create(SLIDER_KIND, string);
    assert(button && cycle && string && slider && !f.stopped());

    /* 512 attributes reach the class together, including their order */
    const ULONG list = 0x800;
    for (ULONG j = 0; j < 512; ++j)
    {
        m68k_write32(&f.ctx, list + 8 * j, GA_Disabled);
        m68k_write32(&f.ctx, list + 8 * j + 4, j == 511);
    }
    m68k_write32(&f.ctx, list + 8 * 512, TAG_DONE);
    unsigned sets = native_sets, calls = native_calls;
    f.call(42, { button, 0, 0, list });
    assert(!f.stopped() && native_sets == sets + 1 && native_calls == calls + 1 && set_attributes == 512 &&
           !(f.native(button)->gad.Flags & GFLG_DISABLED) && !(f.word(button + GD_FLAGS) & GFLG_DISABLED));
    /* a 513th attribute refuses the entire call */
    m68k_write32(&f.ctx, list + 8 * 512, GA_Disabled);
    m68k_write32(&f.ctx, list + 8 * 512 + 4, TRUE);
    m68k_write32(&f.ctx, list + 8 * 513, TAG_DONE);
    sets = native_sets;
    calls = native_calls;
    f.call(42, { button, 0, 0, list });
    assert(f.stopped() && native_sets == sets && native_calls == calls && !(f.word(button + GD_FLAGS) & GFLG_DISABLED));

    /* a non-string entry in a replacement label array */
    STRPTR *old_labels = f.native(cycle)->labels;
    m68k_write32(&f.ctx, Fixture::labels + 16, f.s("New"));
    m68k_write32(&f.ctx, Fixture::labels + 20, 0xFFFFFFF0);
    m68k_write32(&f.ctx, Fixture::labels + 24, 0);
    const size_t live = native_blocks.size();
    set(f, cycle, { { GTCY_Labels, Fixture::labels + 16 } });
    assert(f.stopped() && native_sets == sets && native_calls == calls && native_blocks.size() == live &&
           f.native(cycle)->labels == old_labels);
    /* a string outside guest memory, and one without a NUL at its end */
    const ULONG end = (ULONG)f.memory.size(), si = f.lng(string + GD_SPECIALINFO);
    std::memset(f.memory.data() + end - 3, 'X', 3);
    for (ULONG bad : { 0xFFFFFFF0u, end - 3 })
    {
        set(f, string, { { GTST_String, bad } });
        assert(f.stopped() && native_sets == sets && native_calls == calls &&
               f.str(f.lng(si + SI_BUFFER)) == "Start" && native_blocks.size() == live);
    }
    /* the slider keeps WORDs: the full signed range is valid */
    set(f, slider, { { GTSL_Min, (ULONG)-32768 }, { GTSL_Max, 32767 }, { GTSL_Level, 32767 } });
    assert(!f.stopped() && native_sets == sets + 1 && native_calls == calls + 1 &&
           f.native(slider)->min == -32768 && f.native(slider)->max == 32767 && f.native(slider)->level == 32767);
    assert(get(f, slider, { { GTSL_Min, OUT }, { GTSL_Max, OUT + 4 }, { GTSL_Level, OUT + 8 } }) == 3 &&
           f.lng(OUT) == (ULONG)-32768 && f.lng(OUT + 4) == 32767 && f.lng(OUT + 8) == 32767);
}

/* OnGadget() and OffGadget() need a window, set without one natively and
   settle the batch; RefreshGadgets() does nothing for unattached gadgets. */
static void test_intuition_enable_refresh()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTSC_Total, 100 }, { GTSC_Visible, 10 }, { GTSC_Arrows, 12 } });
    const ULONG scroller = f.create(SCROLLER_KIND, button);
    f.tagitems({});
    const ULONG generic = f.create(GENERIC_KIND, scroller);
    f.tagitems({ { GTSL_MaxLevelLen, 3 } });
    const ULONG slider = f.create(SLIDER_KIND, generic);
    assert(button && scroller && generic && slider && !f.stopped());
    const ULONG arrow1 = f.lng(button + GD_NEXTGADGET), arrow2 = f.lng(arrow1 + GD_NEXTGADGET);
    const ULONG level = f.lng(generic + GD_NEXTGADGET);
    assert(arrow1 != scroller && arrow2 != scroller && level != slider);
    assert(f.native(arrow1)->kind == BUTTON_KIND && f.native(level)->kind == TEXT_KIND);
    changed.clear();
    unsigned calls = native_calls, sets = native_sets;
    const auto before = f.memory;
    std::map<Native *, UWORD> flags;
    for (auto &entry : natives)
        flags[entry.second] = entry.second->gad.Flags;
    for (ULONG gad : { button, scroller, c, generic, arrow1, level })
    {
        for (ULONG lvo : { 174u, 186u })
        {
            assert(f.call_at(Fixture::intuition, lvo, { gad, 0, 0 }) == 0 && !f.stopped());
            assert(native_calls == calls && native_sets == sets && changed.empty() && f.memory == before);
            for (auto &entry : flags)
                assert(entry.first->gad.Flags == entry.second);
        }
    }
    /* with a 68k window the native class and every mirror follow now */
    static int native_window;
    const ULONG win = 0x3BC0;
    assert(Kaffeine_ShadowAdd(&f.ctx, win, &native_window, KAFFEINE_SHADOW_WINDOW));
    for (ULONG gad : { button, scroller, c, generic, arrow1, level })
        for (ULONG lvo : { 174u, 186u })
        {
            const bool off = lvo == 174;
            calls = native_calls;
            sets = native_sets;
            assert(f.call_at(Fixture::intuition, lvo, { gad, win, 0x3BD0 }) == 0 && !f.stopped());
            assert(native_calls == calls + 1 && native_sets == sets + 1 && changed.empty());
            assert(!!(f.native(gad)->gad.Flags & GFLG_DISABLED) == off &&
                   !!(f.word(gad + GD_FLAGS) & GFLG_DISABLED) == off);
            if (gad == scroller)
                for (ULONG arrow : { arrow1, arrow2 })
                    assert(!!(f.native(arrow)->gad.Flags & GFLG_DISABLED) == off &&
                           !!(f.word(arrow + GD_FLAGS) & GFLG_DISABLED) == off);
        }
    /* a non-window A1 stops before native code */
    calls = native_calls;
    sets = native_sets;
    for (ULONG lvo : { 174u, 186u })
    {
        assert(f.call_at(Fixture::intuition, lvo, { button, Fixture::raw, 0 }) == 20 && f.stopped());
        assert(native_calls == calls && native_sets == sets && !(f.word(button + GD_FLAGS) & GFLG_DISABLED));
    }
    /* another object, with its window and requester */
    static int object, window, requester;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3B90, &object, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BA0, &window, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BB0, &requester, FILLER));
    for (ULONG lvo : { 174u, 186u })
    {
        calls = native_calls;
        sets = native_sets;
        const size_t changes = changed.size();
        f.call_at(Fixture::intuition, lvo, { 0x3B90, 0x3BA0, 0x3BB0 });
        assert(!f.stopped() && native_calls == calls + 1 && native_sets == sets && changed.size() == changes + 1 &&
               changed.back() == (lvo == 174 ? "OffGadget" : "OnGadget") &&
               changed_object == &object && changed_window == &window &&
               changed_requester == &requester);
    }
    /* RefreshGadgets() leaves this unattached GadTools list and other addresses alone */
    calls = native_calls;
    sets = native_sets;
    const auto refreshed = f.memory;
    const size_t changes = changed.size();
    assert(f.call_at(Fixture::intuition, 222, { c, win, 0x3BD0 }) == 0 && !f.stopped());
    assert(f.call_at(Fixture::intuition, 222, { 0xFFFFFFF0, Fixture::raw, 0xFFFFFFF4 }) == 0 && !f.stopped());
    assert(native_calls == calls && native_sets == sets && changed.size() == changes && f.memory == refreshed);
    Kaffeine_ShadowForget(&f.ctx, win, KAFFEINE_SHADOW_WINDOW);
    for (ULONG a : { 0x3B90u, 0x3BA0u, 0x3BB0u })
        Kaffeine_ShadowForget(&f.ctx, a, FILLER);
}

/* Windows have three native system gadgets before any application list.
   Keep them alive through cleanup, and watch native frees for dangling
   gadgets still reachable from a window. No private batch fields are read. */
struct WindowFixture : Fixture
{
    static constexpr ULONG win0 = 0x3B00, win1 = 0x3B40;
    Window windows[2]{};
    Gadget system[2][3]{};

    explicit WindowFixture(size_t mem_size = 0x10000, bool guarded = false) : Fixture(mem_size, guarded)
    {
        for (unsigned w = 0; w < 2; ++w)
        {
            for (unsigned i = 0; i < 2; ++i)
                system[w][i].NextGadget = &system[w][i + 1];
            windows[w].FirstGadget = &system[w][0];
            assert(Kaffeine_ShadowAdd(&ctx, address(w), &windows[w], KAFFEINE_SHADOW_WINDOW));
            watched_windows.push_back(&windows[w]);
        }
    }
    ~WindowFixture()
    {
        Kaffeine_GadgetsCleanup(&ctx);
        watched_windows.clear();
    }
    ULONG address(unsigned w) const { assert(w < 2); return w ? win1 : win0; }
    Gadget *g(ULONG a) { assert(native(a)); return G(native(a)); }
    ULONG add(ULONG a, ULONG pos = 0xFFFF, ULONG num = 0xFFFFFFFF, unsigned w = 0, ULONG req = 0)
    {
        return call_at(intuition, 438, { address(w), a, req }, { pos, num });
    }
    ULONG add_one(ULONG a, ULONG pos = 0xFFFF, unsigned w = 0)
    {
        return call_at(intuition, 42, { address(w), a }, { pos });
    }
    ULONG remove(ULONG a, ULONG num = 0xFFFFFFFF, unsigned w = 0)
    {
        return call_at(intuition, 444, { address(w), a }, { num });
    }
    ULONG remove_one(ULONG a, unsigned w = 0)
    {
        return call_at(intuition, 228, { address(w), a });
    }
    Gadget *prepare(ULONG a)
    {
        assert(ctx.window_gadgets);
        ctx.running = TRUE;
        ctx.exit_code = 0;
        return ctx.window_gadgets->prepare(&ctx, a, "host window prepare");
    }
    void sync(unsigned w, APTR address = nullptr)
    {
        const unsigned nesting = forbid_depth;
        ctx.window_gadgets->sync(&ctx, &windows[w], address);
        assert(forbid_depth == nesting && forbid_calls == permit_calls + nesting);
    }
    /* A message of window w that GadTools filters, as the windows of the
       bridge pass it */
    void filter(ULONG cls, Gadget *address, unsigned w = 0)
    {
        IntuiMessage im{};
        im.Class = cls;
        im.IAddress = address;
        im.IDCMPWindow = &windows[w];
        ctx.window_gadgets->filtering(&ctx, &im);
        GT_PostFilterIMsg(GT_FilterIMsg(&im));
    }
    void closing(unsigned w) { ctx.window_gadgets->closing(&ctx, &windows[w]); }
    std::vector<Gadget *> list(unsigned w)
    {
        std::vector<Gadget *> out;
        for (Gadget *at = windows[w].FirstGadget; at; at = at->NextGadget)
        {
            assert(out.size() < 100); // a broken splice must fail, not hang
            out.push_back(at);
        }
        return out;
    }
    void only_system(unsigned w)
    {
        assert((list(w) == std::vector<Gadget *>{ &system[w][0], &system[w][1], &system[w][2] }));
    }
    void mirrors(const std::vector<ULONG> &gadgets)
    {
        for (ULONG a : gadgets)
        {
            Gadget *at = g(a);
            assert(word(a + GD_LEFTEDGE) == (UWORD)at->LeftEdge &&
                   word(a + GD_FLAGS) == (at->Flags & ~GFLG_STRINGEXTEND));
            assert(lng(a + GD_NEXTGADGET) ==
                   Kaffeine_ShadowGuest(&ctx, at->NextGadget, KAFFEINE_SHADOW_GADGET));
        }
    }
};

struct WindowChain
{
    ULONG context, button, inc, dec, scroller, tail;
    std::vector<ULONG> all() const { return { context, button, inc, dec, scroller, tail }; }
};

static WindowChain window_chain(Fixture &f)
{
    const ULONG c = f.context();
    f.newgad(10, 20, 120, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    f.tagitems({ { GTSC_Arrows, 8 } });
    const ULONG sc = f.create(SCROLLER_KIND, b);
    f.tagitems({});
    const ULONG tail = f.create(BUTTON_KIND, sc);
    const ULONG inc = f.lng(b + GD_NEXTGADGET), dec = f.lng(inc + GD_NEXTGADGET);
    assert(c && b && sc && tail && inc && dec && f.count(c) == 6 && !f.stopped());
    return { c, b, inc, dec, sc, tail };
}

static void assert_gadget_call(const char *name, Window *w, Gadget *g, WORD num, UWORD result)
{
    assert(!gadget_calls.empty());
    const GadgetCall &c = gadget_calls.back();
    assert(std::strcmp(c.name, name) == 0 && c.window == w && c.head == g &&
           c.num == num && c.result == result && !c.requester);
}

static void test_native_window_lists()
{
    /* Check the oracle independently of the bridge: prefix insertion,
       WORD -1, beyond-end insertion, retained removal tails and not-found. */
    Fixture f;
    Gadget sys[3]{}, app[3]{}, stray{};
    Window w{};
    w.FirstGadget = &sys[0];
    sys[0].NextGadget = &sys[1]; sys[1].NextGadget = &sys[2];
    app[0].NextGadget = &app[1]; app[1].NextGadget = &app[2];
    assert(AddGList(&w, &app[0], 1, 2, nullptr) == 1);
    assert(sys[0].NextGadget == &app[0] && app[1].NextGadget == &sys[1]);
    assert(RemoveGList(&w, &app[0], 2) == 1 && sys[0].NextGadget == &sys[1]);
    assert(app[1].NextGadget == &sys[1]);
    assert(RemoveGList(&w, &stray, -1) == (UWORD)~0);
    app[1].NextGadget = &app[2];
    assert(AddGList(&w, &app[0], 60000, -1, nullptr) == 3);
    assert(sys[2].NextGadget == &app[0] && !app[2].NextGadget);
    assert(RemoveGList(&w, &app[0], -1) == 3 && !sys[2].NextGadget);
    /* AddGadget overwrites its old successor, and never inserts that one. */
    assert(AddGadget(&w, &app[0], 0) == 0 && w.FirstGadget == &app[0]);
    assert(app[0].NextGadget == &sys[0] && sys[2].NextGadget == nullptr);
    assert(RemoveGList(&w, &app[0], 1) == 0 && app[0].NextGadget == &sys[0]);
    assert(gadget_calls.size() == 7);
}

static void test_window_add_positions()
{
    for (ULONG position : { 0u, 1u, 3u, 0xFFFFu, 60000u, 0x12340002u })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        const UWORD pos = (UWORD)position, actual = std::min<UWORD>(pos, 3);
        const unsigned calls = native_calls;
        layout_gadgets = true;
        assert(f.add(c.context, position) == actual && !f.stopped() && native_calls == calls + 1);
        assert_gadget_call("AddGList", &f.windows[0], f.g(c.context), 6, actual);
        assert(gadget_calls.size() == 1 && gadget_calls.back().position == pos);
        std::vector<Gadget *> expected = { &f.system[0][0], &f.system[0][1], &f.system[0][2] };
        std::vector<Gadget *> app;
        for (ULONG a : c.all()) app.push_back(f.g(a));
        expected.insert(expected.begin() + actual, app.begin(), app.end());
        assert(f.list(0) == expected);
        f.only_system(1);
        f.mirrors(c.all());
        assert(f.g(c.context)->LeftEdge == 11 && f.word(c.button + GD_LEFTEDGE) == 21);
        assert(f.g(c.tail)->NextGadget == (actual < 3 ? &f.system[0][actual] : nullptr));
        assert(!f.lng(c.tail + GD_NEXTGADGET)); // system gadgets have no shadow
        assert(f.remove(c.context, 6) == actual);
        assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.context), 6, actual);
        assert(!f.g(c.tail)->NextGadget && !f.lng(c.tail + GD_NEXTGADGET));
        f.only_system(0);
        f.free(c.context);
        assert(!f.stopped() && natives.empty());
    }
}

static void test_window_add_counts_and_links()
{
    for (ULONG num : { 5u, 6u, 25u, 0x12340005u, 0x1234FFFFu })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        const WORD native_count = (WORD)num == 5 ? 5 : 6;
        assert(f.add(c.context, 1, num) == 1 && !f.stopped());
        assert_gadget_call("AddGList", &f.windows[0], f.g(c.context), native_count, 1);
        f.mirrors(c.all());
        if (native_count == 5)
        {
            assert(f.g(c.scroller)->NextGadget == &f.system[0][1]);
            const UWORD old = f.word(c.tail + GD_LEFTEDGE);
            f.g(c.tail)->LeftEdge = 91;
            f.sync(0);
            assert(f.word(c.tail + GD_LEFTEDGE) == old); // suffix was not attached
            assert(f.add(c.tail, 1, 1) == 1);
            // The inserted tail now points at another mirrored bridge gadget.
            assert(f.g(c.tail)->NextGadget == f.g(c.context) && f.lng(c.tail + GD_NEXTGADGET) == c.context);
            assert(f.remove(c.tail, 1) == 1 && !f.stopped() && !f.g(c.tail)->NextGadget &&
                   f.lng(c.tail + GD_NEXTGADGET) == c.context);
            const unsigned calls = native_calls;
            const size_t frees = free_gadget_calls.size();
            f.free(c.tail);
            assert(f.stopped() && native_calls == calls && free_gadget_calls.size() == frees);
            m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, 0); // the program cuts its list
            f.free(c.tail);
            assert(!f.stopped() && f.native(c.context));
        }
        f.closing(0);
        f.only_system(0);
        f.free(c.context);
        assert(!f.stopped() && natives.empty());
    }
}

static void test_window_add_refusals()
{
    enum Bad { WINDOW, REQUESTER, INSIDE, CUT, RELINKED, REORDERED, FOREIGN_TAIL, ATTACHED, CYCLE, CYCLE_INSIDE };
    for (Bad bad : { WINDOW, REQUESTER, INSIDE, CUT, RELINKED, REORDERED, FOREIGN_TAIL, ATTACHED, CYCLE,
                     CYCLE_INSIDE })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        const ULONG spare = f.context();
        ULONG window = f.win0, head = c.context, num = 0xFFFFFFFF, req = 0;
        switch (bad)
        {
        case WINDOW: window = Fixture::raw; break;
        case REQUESTER: req = Fixture::raw; break;
        case INSIDE: head = c.scroller; break;
        case CUT: num = 3; break; // context, button, first arrow: cuts a batch
        case RELINKED: m68k_write32(&f.ctx, c.context + GD_NEXTGADGET, c.scroller); break;
        case REORDERED:
            // Reordering inside one batch still refuses; batch boundaries may be linked freely.
            m68k_write32(&f.ctx, c.inc + GD_NEXTGADGET, c.scroller);
            break;
        case FOREIGN_TAIL: m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, Fixture::raw); break;
        case ATTACHED: assert(f.add(c.tail, 0xFFFF, 1, 1) == 3); break;
        case CYCLE:
            f.g(c.tail)->NextGadget = f.g(c.context);
            m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, c.context);
            break;
        case CYCLE_INSIDE:
            f.g(c.inc)->NextGadget = f.g(c.inc);
            m68k_write32(&f.ctx, c.inc + GD_NEXTGADGET, c.inc);
            break;
        }
        const unsigned calls = native_calls;
        const size_t records = gadget_calls.size();
        const auto memory = f.memory;
        const auto blocks = native_blocks;
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.call_at(Fixture::intuition, 438, { window, head, req }, { 0, num }) == 20);
        assert(f.stopped() && native_calls == calls && gadget_calls.size() == records &&
               f.memory == memory && native_blocks == blocks && f.ctx.num_shadows == shadows &&
               Kaffeine_HeapAvail(&f.ctx, 0) == heap);
        f.only_system(0);
        if (bad == ATTACHED)
            assert(f.remove(c.tail, 1, 1) == 3);
        f.g(c.tail)->NextGadget = nullptr;
        f.g(c.inc)->NextGadget = f.g(c.dec);
        m68k_write32(&f.ctx, c.context + GD_NEXTGADGET, c.button);
        m68k_write32(&f.ctx, c.button + GD_NEXTGADGET, c.inc);
        m68k_write32(&f.ctx, c.inc + GD_NEXTGADGET, c.dec);
        m68k_write32(&f.ctx, c.scroller + GD_NEXTGADGET, c.tail);
        m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, 0);
        // A valid unrelated call must not attach batches marked by the refusal.
        assert(f.add(spare, 0xFFFF, 1, 1) == 3 && !f.stopped());
        const UWORD before = f.word(c.button + GD_LEFTEDGE);
        f.g(c.button)->LeftEdge = 101;
        f.sync(1);
        assert(f.word(c.button + GD_LEFTEDGE) == before);
        assert(f.add(c.context) == 3 && !f.stopped());
        f.closing(0); f.closing(1);
        f.free(c.context); f.free(spare);
        assert(!f.stopped() && natives.empty());
    }
}

static void test_window_add_empty_and_own()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const unsigned calls = native_calls;
    for (const auto &a : std::vector<std::array<ULONG, 3>>{
             { 0, c.context, 6 }, { f.win0, 0, 6 }, { f.win0, c.context, 0 },
             { f.win0, c.context, 0x12340000 }, { f.win0, Fixture::raw, 1 } })
        assert(f.call_at(Fixture::intuition, 438, { a[0], a[1], 0 }, { 0, a[2] }) == 0xFFFF && !f.stopped());
    assert(f.add_one(Fixture::raw) == 0xFFFF && !f.stopped());
    // AddGadget's NULL convention differs from AddGList, as in AROS.
    assert(f.call_at(Fixture::intuition, 42, { 0, c.context }, { 0 }) == 0 && !f.stopped());
    assert(f.add_one(0) == 0 && !f.stopped());
    assert(native_calls == calls && gadget_calls.empty());
    f.only_system(0); f.only_system(1);
    assert(f.add(c.context) == 3);
}

static void test_window_add_one()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    layout_gadgets = true;
    assert(f.add_one(c.context, 0) == 0 && !f.stopped());
    assert_gadget_call("AddGadget", &f.windows[0], f.g(c.context), 1, 0);
    assert(f.g(c.context)->NextGadget == &f.system[0][0] && !f.lng(c.context + GD_NEXTGADGET));
    assert(f.g(c.button)->NextGadget == f.g(c.inc)); // following batches were not added
    f.mirrors({ c.context });
    assert(f.add_one(c.button, 0x12340001) == 1 && !f.stopped());
    assert_gadget_call("AddGadget", &f.windows[0], f.g(c.button), 1, 1);
    assert(gadget_calls.back().position == 1 && f.g(c.button)->NextGadget == &f.system[0][0]);
    f.mirrors({ c.button });
    const unsigned calls = native_calls;
    assert(f.add_one(c.inc) == 20 && f.stopped() && native_calls == calls);
    assert(f.add(c.inc, 0xFFFF, 3, 1) == 3 && !f.stopped()); // refusal cleared marks
    f.closing(0); f.closing(1);
    f.free(c.context); f.free(c.button); f.free(c.inc);
    assert(!f.stopped() && f.native(c.tail));
    f.free(c.tail);
    assert(natives.empty());
}

static void test_window_remove_lists()
{
    for (ULONG num : { 6u, 25u, 0xFFFFFFFFu, 0x12340006u })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        assert(f.add(c.context) == 3);
        f.g(c.inc)->LeftEdge = -44;
        const unsigned calls = native_calls;
        assert(f.remove(c.context, num) == 3 && !f.stopped() && native_calls == calls + 1);
        assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.context), 6, 3);
        assert(!f.g(c.tail)->NextGadget && !f.lng(c.tail + GD_NEXTGADGET));
        f.mirrors(c.all());
        f.only_system(0);
        assert(f.word(c.inc + GD_LEFTEDGE) == (UWORD)-44);
        const size_t records = gadget_calls.size();
        assert(f.remove(c.context) == 0xFFFF && !f.stopped() && gadget_calls.size() == records);
        f.free(c.context);
        assert(!f.stopped() && natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_window_remove_prefix_and_tail()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context, 1) == 1);
    const unsigned calls = native_calls;
    assert(f.remove(c.context) == 20 && f.stopped() && native_calls == calls);
    assert(f.remove(c.context, 3) == 20 && f.stopped() && native_calls == calls);
    assert(f.remove(c.dec, 2) == 20 && f.stopped() && native_calls == calls);
    // A prefix ending at the scroller's last gadget leaves the tail batch on the window.
    assert(f.remove(c.context, 5) == 1 && !f.stopped());
    assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.context), 5, 1);
    assert(gadget_calls.back().after == f.g(c.tail)); // native tail was deliberately retained
    assert(!f.g(c.scroller)->NextGadget && f.lng(c.scroller + GD_NEXTGADGET) == c.tail);
    assert((f.list(0) == std::vector<Gadget *>{ &f.system[0][0], f.g(c.tail),
                                               &f.system[0][1], &f.system[0][2] }));
    f.mirrors({ c.context, c.button, c.inc, c.dec });
    const size_t frees = free_gadget_calls.size();
    const unsigned guarded = native_calls;
    f.free(c.context);
    assert(f.stopped() && native_calls == guarded && free_gadget_calls.size() == frees);
    m68k_write32(&f.ctx, c.scroller + GD_NEXTGADGET, 0); // leave the window's batch out
    f.free(c.context);
    assert(!f.stopped() && natives.size() == 1 && f.native(c.tail));
    assert(f.remove_one(c.tail) == 1 && !f.stopped());
    assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.tail), 1, 1);
    assert(gadget_calls.back().after == &f.system[0][1]);
    assert(!f.g(c.tail)->NextGadget && !f.lng(c.tail + GD_NEXTGADGET));
    f.only_system(0);
    f.free(c.tail);
    assert(!f.stopped() && natives.empty());
}

static void test_window_remove_one_and_not_found()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3);
    unsigned calls = native_calls;
    assert(f.remove(c.context, 6, 1) == 0xFFFF && !f.stopped());
    assert(f.remove_one(c.context, 1) == 0xFFFF && !f.stopped());
    assert(f.remove(Fixture::raw) == 0xFFFF && !f.stopped());
    assert(f.remove_one(Fixture::raw) == 0xFFFF && !f.stopped());
    assert(f.remove(0) == 0xFFFF && f.remove_one(0) == 0xFFFF);
    assert(f.call_at(Fixture::intuition, 444, { 0, c.context }, { 6 }) == 0xFFFF);
    assert(f.call_at(Fixture::intuition, 228, { 0, c.context }) == 0xFFFF);
    assert(f.remove(c.context, 0x12340000) == 0xFFFF && !f.stopped());
    assert(native_calls == calls);
    assert(f.remove_one(c.inc) == 20 && f.stopped() && native_calls == calls); // cuts the batch
    assert(f.remove_one(c.context) == 3 && !f.stopped());
    assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.context), 1, 3);
    assert(gadget_calls.back().after == f.g(c.button) && !f.g(c.context)->NextGadget &&
           f.lng(c.context + GD_NEXTGADGET) == c.button);
    assert(f.remove_one(c.button) == 3);
    assert(!f.g(c.button)->NextGadget && f.lng(c.button + GD_NEXTGADGET) == c.inc);
    assert(f.remove(c.inc) == 3); // -1 removes the whole scroller and remaining button
    f.only_system(0);
    calls = native_calls;
    assert(f.remove_one(c.button) == 0xFFFF && !f.stopped() && native_calls == calls);
    f.free(c.context); f.free(c.button); f.free(c.inc);
    assert(!f.stopped() && natives.empty());
}

static void test_window_refresh()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG detached = f.context();
    assert(f.add(c.context) == 3);
    for (ULONG count : { 1u, 3u, 0xFFFFFFFFu, 0x12340002u })
    {
        const unsigned calls = native_calls;
        assert(f.call_at(Fixture::intuition, 432, { c.inc, f.win0, Fixture::raw }, { count }) == 0);
        assert(!f.stopped() && native_calls == calls + 1);
        assert_gadget_call("RefreshGList", &f.windows[0], f.g(c.inc), (WORD)count, 0);
    }
    assert(f.call_at(Fixture::intuition, 222, { c.context, f.win0, Fixture::raw }) == 0);
    assert_gadget_call("RefreshGList", &f.windows[0], f.g(c.context), -1, 0);
    const unsigned calls = native_calls;
    const auto memory = f.memory;
    for (ULONG lvo : { 432u, 222u })
        for (const auto &a : std::vector<std::array<ULONG, 2>>{
                 { c.context, f.win1 }, { c.context, Fixture::raw }, { c.context, 0 },
                 { detached, f.win0 }, { Fixture::raw, f.win0 }, { 0, f.win0 } })
            assert(f.call_at(Fixture::intuition, lvo, { a[0], a[1], 0 }, { 2 }) == 0 && !f.stopped());
    assert(f.call_at(Fixture::intuition, 432, { c.context, f.win0, 0 }, { 0x12340000 }) == 0);
    assert(native_calls == calls && f.memory == memory);
    f.closing(0);
    assert(f.call_at(Fixture::intuition, 222, { c.context, f.win0, 0 }) == 0);
    assert(gadget_calls.back().name == std::string("RemoveGList"));
}

static void test_window_free_and_create_guards()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG unrelated = f.context();
    // Only the last batch is attached: FreeGadgets must inspect the entire chain.
    assert(f.add(c.tail, 0xFFFF, 1) == 3);
    const auto blocks = native_blocks;
    const auto memory = f.memory;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    const unsigned calls = native_calls, before = freed;
    f.free(c.context);
    assert(f.stopped() && native_calls == calls && native_blocks == blocks &&
           f.memory == memory && freed == before && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
    // A stale doomed bit anywhere would refuse this unrelated free or discard live shadows.
    f.free(unrelated);
    assert(!f.stopped() && !f.native(unrelated));
    for (ULONG a : c.all()) assert(f.native(a));
    f.tagitems({});
    const unsigned guarded = native_calls;
    assert(f.create(BUTTON_KIND, c.tail) == 20 && f.stopped() && native_calls == guarded);
    assert(f.remove_one(c.tail) == 3);
    const ULONG b = f.create(BUTTON_KIND, c.tail);
    assert(b && !f.stopped()); // removing the batch releases the creation guard
    f.free(c.context);
    assert(!f.stopped() && natives.empty() && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

struct ShownChain { ULONG context, string, first, view; };
static ShownChain shown_chain(WindowFixture &f)
{
    const ULONG c = f.context();
    f.newgad(-30, 7, 160, 14, 0, 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTST_String, f.s("Seed") }, { GTST_MaxChars, 12 } });
    const ULONG string = f.create(STRING_KIND, c);
    f.newgad(10, 20, 160, 100, 0, 0, 3, 0, f.vi, 0);
    f.tagitems({ { GTLV_ShowSelected, string } });
    const ULONG view = f.create(LISTVIEW_KIND, string), first = f.lng(string + GD_NEXTGADGET);
    assert(c && string && view && first && f.count(c) == 6 && !f.stopped());
    return { c, string, first, view };
}

static void test_window_showsel_create_guard()
{
    WindowFixture f;
    const ULONG c = f.context();
    f.newgad(-30, 7, 160, 14, 0, 0, 2, 0, f.vi, 0);
    f.tagitems({ { GTST_String, f.s("Seed") } });
    const ULONG string = f.create(STRING_KIND, c), other = f.context();
    assert(f.add(string, 0xFFFF, 1) == 3);
    f.newgad(10, 20, 160, 100, 0, 0, 3, 0, f.vi, 0);
    f.tagitems({ { GTLV_ShowSelected, string } });
    const auto memory = f.memory;
    const auto blocks = native_blocks;
    const unsigned calls = native_calls;
    const UWORD shadows = f.ctx.num_shadows;
    assert(f.create(LISTVIEW_KIND, other) == 20 && f.stopped() && native_calls == calls &&
           f.memory == memory && native_blocks == blocks && f.ctx.num_shadows == shadows);
    assert(f.remove_one(string) == 3);
    assert(f.create(LISTVIEW_KIND, other) && !f.stopped());
    f.free(other); f.free(c);
    assert(!f.stopped() && natives.empty());
}

static void test_window_showsel_placement()
{
    WindowFixture f;
    const ShownChain c = shown_chain(f);
    unsigned calls = native_calls;
    assert(f.add(c.first, 0xFFFF, 4) == 20 && f.stopped() && native_calls == calls);
    assert(!f.prepare(c.first) && !f.ctx.running && f.ctx.exit_code == 20 && native_calls == calls);
    // A valid unrelated attach must not carry the refused listview with it.
    const ULONG spare = f.context();
    assert(f.add(spare, 0xFFFF, 1) == 3 && !f.stopped());
    const UWORD old = f.word(c.view + GD_LEFTEDGE);
    f.g(c.view)->LeftEdge = 77;
    f.sync(0);
    assert(f.word(c.view + GD_LEFTEDGE) == old);
    assert(f.remove_one(spare) == 3);
    f.free(spare);
    assert(f.add(c.context) == 3 && !f.stopped()); // both on one window
    calls = native_calls;
    assert(f.add(c.string, 0xFFFF, 1, 1) == 20 && f.stopped() && native_calls == calls);
    assert(f.remove_one(c.string) == 20 && f.stopped() && native_calls == calls);
    assert(f.remove(c.context, 2) == 20 && f.stopped() && native_calls == calls);
    assert(f.remove(c.context) == 3 && !f.stopped()); // both leave together
    assert(!f.g(c.view)->NextGadget && !f.lng(c.view + GD_NEXTGADGET));
    assert(f.add(c.context, 0xFFFF, 6, 1) == 3); // both can move to another window
    assert(f.remove(c.first, 4, 1) == 5); // removing the view while its string stays is safe
    assert(f.remove_one(c.string, 1) == 4);
    f.closing(1);
    f.free(c.first); f.free(c.string); f.free(c.context);
    assert(!f.stopped() && natives.empty());
}

static void test_window_showsel_other_window()
{
    WindowFixture f;
    const ShownChain c = shown_chain(f);
    // Set up the native/window-hook boundary directly: a view on window 0
    // and its string still unattached. This exercises showsel_apart's
    // dependency check, independently of run_mark's already-attached check.
    assert(AddGList(&f.windows[0], f.g(c.first), (UWORD)~0, 4, nullptr) == 3);
    f.ctx.window_gadgets->opened(&f.ctx, &f.windows[0], f.g(c.first));
    const unsigned calls = native_calls;
    const auto memory = f.memory;
    assert(f.add(c.string, 0xFFFF, 1, 1) == 20 && f.stopped() &&
           native_calls == calls && f.memory == memory);
    f.only_system(1);
    // The refusal unmarked the string; the same-window placement can succeed.
    assert(f.add(c.string, 0xFFFF, 1) == 7 && !f.stopped());
    assert(f.remove(c.first, 4) == 3 && !f.stopped());
    assert(!f.g(c.view)->NextGadget && f.lng(c.view + GD_NEXTGADGET) == c.string);
    assert(f.remove_one(c.string) == 3 && !f.stopped());
    f.free(c.first); f.free(c.context);
    assert(!f.stopped() && natives.empty());
}

static void test_window_prepare_refusals()
{
    enum Bad { INSIDE, RELINKED, FOREIGN_TAIL, ATTACHED, CYCLE, CYCLE_INSIDE, CUT_NATIVE };
    for (Bad bad : { INSIDE, RELINKED, FOREIGN_TAIL, ATTACHED, CYCLE, CYCLE_INSIDE, CUT_NATIVE })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        const ULONG spare = f.context();
        ULONG head = c.context;
        Gadget *saved = f.g(c.inc)->NextGadget;
        switch (bad)
        {
        case INSIDE: head = c.scroller; break;
        case RELINKED: m68k_write32(&f.ctx, c.context + GD_NEXTGADGET, c.scroller); break;
        case FOREIGN_TAIL: m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, Fixture::raw); break;
        case ATTACHED: assert(f.add(c.tail, 0xFFFF, 1, 1) == 3); break;
        case CYCLE:
            f.g(c.tail)->NextGadget = f.g(c.context);
            m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, c.context);
            break;
        case CYCLE_INSIDE:
            f.g(c.inc)->NextGadget = f.g(c.inc);
            m68k_write32(&f.ctx, c.inc + GD_NEXTGADGET, c.inc);
            break;
        case CUT_NATIVE: f.g(c.inc)->NextGadget = nullptr; break;
        }
        const unsigned calls = native_calls;
        const auto memory = f.memory;
        assert(!f.prepare(head) && !f.ctx.running && f.ctx.exit_code == 20 &&
               native_calls == calls && f.memory == memory);
        if (bad == ATTACHED) assert(f.remove_one(c.tail, 1) == 3);
        f.g(c.inc)->NextGadget = saved;
        f.g(c.tail)->NextGadget = nullptr;
        m68k_write32(&f.ctx, c.context + GD_NEXTGADGET, c.button);
        m68k_write32(&f.ctx, c.inc + GD_NEXTGADGET, c.dec);
        m68k_write32(&f.ctx, c.tail + GD_NEXTGADGET, 0);
        assert(f.add(spare, 0xFFFF, 1, 1) == 3);
        const UWORD before = f.word(c.button + GD_LEFTEDGE);
        f.g(c.button)->LeftEdge = 111;
        f.sync(1);
        assert(f.word(c.button + GD_LEFTEDGE) == before);
        assert(f.prepare(c.context) == f.g(c.context) && f.ctx.running && !f.ctx.exit_code);
        // Successful prepare must also unmark: it did not attach anything.
        assert(f.add(c.context) == 3 && !f.stopped());
    }
}

static void test_window_hooks_open_sync_close()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG detached = f.context(), other = f.context();
    const unsigned calls = native_calls;
    assert(f.prepare(c.context) == f.g(c.context) && f.ctx.running && native_calls == calls);
    assert(f.prepare(c.context) == f.g(c.context)); // prepare left no marks
    // Intuition appends WA_Gadgets after its own gadgets, then opened attaches it.
    layout_gadgets = true;
    assert(AddGList(&f.windows[0], f.g(c.context), (UWORD)~0, -1, nullptr) == 3);
    f.ctx.window_gadgets->opened(&f.ctx, &f.windows[0], f.g(c.context));
    f.mirrors(c.all());
    assert(f.word(c.button + GD_LEFTEDGE) == 21);
    assert(f.add(other, 0xFFFF, 1, 1) == 3);
    unsigned guarded = native_calls;
    assert(f.add(c.context, 0xFFFF, 6, 1) == 20 && f.stopped() && native_calls == guarded);
    const UWORD old_other = f.word(other + GD_LEFTEDGE), old_detached = f.word(detached + GD_LEFTEDGE);
    f.g(other)->LeftEdge = 211; f.g(detached)->LeftEdge = 212;
    for (ULONG a : c.all()) f.g(a)->LeftEdge = -123;
    f.sync(0);
    for (ULONG a : c.all()) assert(f.word(a + GD_LEFTEDGE) == (UWORD)-123);
    assert(f.word(other + GD_LEFTEDGE) == old_other && f.word(detached + GD_LEFTEDGE) == old_detached);
    f.sync(1);
    assert(f.word(other + GD_LEFTEDGE) == 211 && f.word(detached + GD_LEFTEDGE) == old_detached);
    gadget_calls.clear();
    f.closing(0);
    assert(gadget_calls.size() == 1);
    assert_gadget_call("RemoveGList", &f.windows[0], f.g(c.context), 6, 3);
    assert(!f.g(c.tail)->NextGadget && !f.lng(c.tail + GD_NEXTGADGET));
    f.only_system(0);
    f.g(c.button)->LeftEdge = 313;
    f.sync(0);
    assert(f.word(c.button + GD_LEFTEDGE) == (UWORD)-123); // closing released attachment
    f.free(c.context);
    assert(!f.stopped() && f.native(other));
    f.closing(1); f.free(other); f.free(detached);
    assert(!f.stopped() && natives.empty());
}

static void test_window_closing_separate_runs()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG apart = f.context(), other = f.context();
    assert(f.add(c.context, 1) == 1);
    assert(f.add(apart, 0xFFFF, 1) == 9);
    assert(f.add(other, 0xFFFF, 1, 1) == 3);
    gadget_calls.clear();
    f.closing(0);
    assert(gadget_calls.size() == 2);
    assert(gadget_calls[0].head == f.g(c.context) && gadget_calls[0].num == 6 &&
           gadget_calls[0].result == 1 && gadget_calls[0].after == &f.system[0][1]);
    assert(gadget_calls[1].head == f.g(apart) && gadget_calls[1].num == 1 && gadget_calls[1].result == 3);
    assert(!f.g(c.tail)->NextGadget && !f.lng(c.tail + GD_NEXTGADGET) &&
           !f.g(apart)->NextGadget && !f.lng(apart + GD_NEXTGADGET));
    f.only_system(0);
    const unsigned calls = native_calls;
    f.closing(0);
    assert(native_calls == calls); // closing is harmless again
    f.free(c.context); f.free(apart);
    assert(!f.stopped() && f.native(other));
    f.free(other);
    assert(f.stopped()); // closing one window did not detach another one's run
    f.closing(1); f.free(other);
    assert(!f.stopped() && natives.empty());
}

static void test_window_cleanup_open_windows()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG apart = f.context(), other = f.context();
    assert(f.add(c.context, 1) == 1 && f.add(apart, 0xFFFF, 1) == 9 && f.add(other, 1, 1, 1) == 1);
    gadget_calls.clear(); gadget_events.clear();
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(gadget_calls.size() == 3);
    assert(std::count(gadget_events.begin(), gadget_events.end(), "RemoveGList") == 3);
    const auto first_free = std::find(gadget_events.begin(), gadget_events.end(), "FreeGadgets");
    assert(first_free != gadget_events.end() && first_free - gadget_events.begin() == 3);
    f.only_system(0); f.only_system(1);
    assert(natives.empty() && !f.ctx.gadgets && native_blocks.empty() &&
           Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    for (ULONG a : c.all()) assert(!f.native(a));
    assert(!f.native(apart) && !f.native(other));
}

/* AROS splices the gadget before the run as well as the tail
   (addglist.c:135-136, removeglist.c:119), and that gadget may be one of
   the bridge's on the window, whose 68k link must follow. */
static void expect_window_link(WindowFixture &f, ULONG predecessor, ULONG expected)
{
    assert(f.g(predecessor)->NextGadget == f.g(expected));
    assert(f.lng(predecessor + GD_NEXTGADGET) == expected);
}

static void test_window_existing_predecessor_add()
{
    WindowFixture f;
    const ShownChain c = shown_chain(f);
    assert(f.add(c.context, 0xFFFF, 2) == 3);
    assert(f.add(c.first, 0xFFFF, 4) == 5 && !f.stopped());
    expect_window_link(f, c.string, c.first);
    /* Without that link a later whole-list add fails in run_mark(). */
    assert(f.remove(c.context) == 3 && !f.stopped());
    assert(f.add(c.context, 0xFFFF, 6, 1) == 3 && !f.stopped());
}

static void test_window_existing_predecessor_remove()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3 && f.remove(c.inc, 3) == 5);
    expect_window_link(f, c.button, c.tail);
    assert(!f.g(c.scroller)->NextGadget && f.lng(c.scroller + GD_NEXTGADGET) == c.tail);
    const unsigned calls = native_calls;
    const size_t frees = free_gadget_calls.size();
    f.free(c.inc);
    assert(f.stopped() && native_calls == calls && free_gadget_calls.size() == frees);
    m68k_write32(&f.ctx, c.scroller + GD_NEXTGADGET, 0);
    f.free(c.inc);
    assert(!f.stopped() && !f.native(c.inc));
    // Without the repair the preceding 68k gadget still points into freed mirrors.
}

/* paletteclass.c lays a palette out as a window takes it, and divides by
   the columns and rows that it works out there, and by their cells on a
   click: a palette whose colours would get none stays off windows, through
   AddGList() as through the list of a window about to open, and one that
   the class can lay out goes on. */
static void test_window_palette_layout()
{
    struct Case { WORD width, height; ULONG depth, indw, indh; bool fits; };
    const Case cases[] =
    {
        { 100, 8, 1, 0, 0, false },     // no height within the borders
        { 100, 30, 1, 0, 18, false },   // the indicator above takes the rest
        { 30, 40, 1, 18, 0, false },    // the one at the left leaves no column
        { 30, 40, 1, 18, 4, true },     // an indicator height goes before a width
        { 9, 9, 2, 0, 0, false },       // two columns in one pixel
        { 9, 264, 8, 0, 0, false },     // 256 rows of a pixel, none as a UBYTE
        { 100, 100, 16, 0, 0, false },  // 65536 colours, none in the class's UWORD
        { 1000, 1000, 64, 0, 0, false }, // a depth that the shift of the CPU decides
        { 100, 9, 1, 0, 0, true },      // thirteen columns of seven pixels in one row
        { 40, 40, 2, 10, 0, true },
    };
    for (const Case &k : cases)
        for (bool opening : { false, true })
        {
            WindowFixture f;
            const ULONG c = f.context();
            f.newgad(10, 10, k.width, k.height, 0, 0, 1, 0, f.vi, 0);
            f.tagitems({ { GTPA_Depth, k.depth }, { GTPA_IndicatorWidth, k.indw },
                         { GTPA_IndicatorHeight, k.indh } });
            const ULONG palette = f.create(PALETTE_KIND, c);
            assert(palette && !f.stopped());
            const unsigned calls = native_calls;
            const auto memory = f.memory;
            if (opening)
                assert(f.prepare(c) == (k.fits ? f.g(c) : nullptr) && !!f.ctx.running == k.fits &&
                       native_calls == calls && f.memory == memory);
            else if (k.fits)
                assert(f.add(c) == 3 && !f.stopped() && native_calls == calls + 1);
            else
                assert(f.add(c) == 20 && f.stopped() && native_calls == calls && f.memory == memory);
            // A refusal leaves no mark: another list goes on without the palette.
            const ULONG spare = f.context();
            assert(f.add(spare, 0xFFFF, 1, 1) == 3 && !f.stopped());
            f.g(palette)->LeftEdge = 99;
            f.sync(1);
            assert(f.word(palette + GD_LEFTEDGE) != 99);
            f.closing(0); f.closing(1);
            f.free(c); f.free(spare);
            assert(!f.stopped() && natives.empty());
        }
}

/* A list goes on a window between the batches there, not between the
   gadgets of one, such as the arrows of a scroller, where Intuition would
   put it at that position (addglist.c walks the window's list) */
static void test_window_insert_inside_batch()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    // system gadgets 0 to 2, context 3, button 4, arrows 5 and 6, scroller 7, tail 8
    assert(f.add(c.context) == 3);
    const ULONG spare = f.context();
    const unsigned calls = native_calls;
    const auto memory = f.memory;
    for (ULONG position : { 6u, 7u, 0x12340006u })
    {
        assert(f.add(spare, position, 1) == 20 && f.stopped());
        assert(f.add_one(spare, position) == 20 && f.stopped());
    }
    assert(native_calls == calls && f.memory == memory);
    for (ULONG position : { 0u, 3u, 4u, 5u, 8u, 9u, 0xFFFFu })
    {
        const ULONG at = std::min<ULONG>(position, 9);
        assert(f.add(spare, position, 1) == at && !f.stopped());
        assert(f.remove_one(spare) == at && !f.stopped());
        assert(f.add_one(spare, position) == at && !f.stopped());
        assert(f.remove(spare, 1) == at && !f.stopped());
    }
    // The scroller's batch still comes off whole.
    assert(f.remove(c.inc, 3) == 5 && !f.stopped());
    f.closing(0);
    f.only_system(0);
    f.free(c.context); f.free(c.inc); f.free(spare);
    assert(!f.stopped() && natives.empty());
}

/* Rendering sets fields such as the NumChars of a string, which
   strgadgets.c counts as it draws the text: after RefreshGList() and
   RefreshGadgets() the mirrors of the gadgets on that window follow, and
   those of a list on another window stay. */
static void test_window_refresh_settles()
{
    for (ULONG lvo : { 432u, 222u })
    {
        WindowFixture f;
        const ULONG c = f.context(), other = f.context();
        f.newgad(10, 10, 100, 14, 0, 0, 1, 0, f.vi, 0);
        f.tagitems({ { GTST_String, f.s("abc") } });
        const ULONG string = f.create(STRING_KIND, c);
        f.tagitems({ { GTST_String, f.s("xyz") } });
        const ULONG apart = f.create(STRING_KIND, other);
        assert(string && apart && f.add(c) == 3 && f.add(other, 0xFFFF, 2, 1) == 3);
        const ULONG si = f.lng(string + GD_SPECIALINFO), si2 = f.lng(apart + GD_SPECIALINFO);
        assert(f.word(si + SI_NUMCHARS) == 3 && f.word(si2 + SI_NUMCHARS) == 3);
        // the class took new texts without a window, which only rendering counts
        std::strcpy((char *)f.native(string)->buffer, "abcd");
        std::strcpy((char *)f.native(apart)->buffer, "wxyz!");
        refresh_renders = true;
        assert(f.call_at(Fixture::intuition, lvo, { string, f.win0, 0 }, { 1 }) == 0 && !f.stopped());
        assert(f.native(string)->si.NumChars == 4 && f.word(si + SI_NUMCHARS) == 4 &&
               f.str(f.lng(si + SI_BUFFER)) == "abcd");
        assert(f.native(apart)->si.NumChars == 3 && f.word(si2 + SI_NUMCHARS) == 3 &&
               f.str(f.lng(si2 + SI_BUFFER)) == "xyz");
    }
}

/* Intuition deactivates the gadgets that RemoveGList() takes off, after
   the button goes up, which removeglist.c waits for: the mirrors follow
   the state that the removal left. */
static void test_window_remove_deactivates()
{
    for (ULONG lvo : { 444u, 228u })
    {
        WindowFixture f;
        const WindowChain c = window_chain(f);
        assert(f.add(c.context) == 3);
        f.g(c.context)->Flags |= GFLG_SELECTED;
        f.sync(0);
        assert(f.word(c.context + GD_FLAGS) & GFLG_SELECTED);
        remove_deselects = true;
        assert(f.call_at(Fixture::intuition, lvo, { f.win0, c.context }, { 1 }) == 3 && !f.stopped());
        assert(!(f.g(c.context)->Flags & GFLG_SELECTED) && !(f.word(c.context + GD_FLAGS) & GFLG_SELECTED));
    }
}

/* A RemoveGList() that Intuition refuses, the list being on the window no
   more as far as it knows: (UWORD)~0, and the gadgets are on no window for
   the bridge either, so that the program can free them, put others on and
   close the window. */
static void test_window_remove_native_failure()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3);
    f.system[0][2].NextGadget = nullptr;   // behind the bridge
    gadget_calls.clear();
    assert(f.remove(c.context) == 0xFFFF && !f.stopped());
    assert(gadget_calls.size() == 1 && gadget_calls[0].result == (UWORD)~0);
    f.only_system(0);
    const UWORD old = f.word(c.button + GD_LEFTEDGE);
    f.g(c.button)->LeftEdge = 66;
    f.sync(0);
    assert(f.word(c.button + GD_LEFTEDGE) == old);
    // Another list goes on without them, and they go before the window closes.
    const ULONG spare = f.context();
    assert(f.add(spare) == 3 && !f.stopped());
    f.g(c.button)->LeftEdge = 67;
    f.sync(0);
    assert(f.word(c.button + GD_LEFTEDGE) == old);
    f.free(c.context);
    assert(!f.stopped() && !f.native(c.context) && f.native(spare));
    gadget_calls.clear();
    f.closing(0);
    assert(gadget_calls.size() == 1 && gadget_calls[0].head == f.g(spare));
    f.only_system(0);
    f.free(spare);
    assert(!f.stopped() && natives.empty());
}

/* Such a list may still lead into the window's own gadgets, which
   CloseWindow() frees: FreeGadgets() frees the gadgets of the bridge and
   never reads those. */
static void test_window_free_stops_at_window_gadgets()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    Gadget *own = new Gadget[2]{};
    own[0].NextGadget = &own[1];
    f.windows[0].FirstGadget = &own[0];
    assert(f.add(c.context, 0) == 0);
    f.windows[0].FirstGadget = &own[0];   // behind the bridge
    assert(f.remove(c.context, 6) == 0xFFFF && !f.stopped());
    assert(f.g(c.tail)->NextGadget == &own[0]);
    f.windows[0].FirstGadget = nullptr;
    delete[] own;                          // as the window closes
    f.free(c.context);
    assert(!f.stopped() && natives.empty());
}

/* GadTools keeps the gadget of a GADGETDOWN active in the first context on
   the window until a GADGETUP, which does not come when the button goes up
   outside an arrow: FreeGadgets() of that gadget makes GadTools forget it
   first, also for a context on no window. What a context keeps of a gadget
   that stays, or a context that goes with its gadget, is left alone. */
static void test_window_held_gadget_forgotten()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG other = f.context();
    f.newgad(10, 60, 80, 14, 0, 0, 9, 0, f.vi, 0);
    f.tagitems({});
    const ULONG button = f.create(BUTTON_KIND, other);
    assert(button && f.add(c.context) == 3 && f.add(other, 0xFFFF, 2) == 9);
    Gadget *context = f.g(c.context);
    f.filter(IDCMP_GADGETDOWN, f.g(c.inc));
    assert(held_by[context] == f.g(c.inc) && !held_by.count(f.g(other)));
    assert(f.remove(c.inc, 3) == 5);
    unsigned before = filtered;
    const unsigned calls = native_calls;
    const size_t frees = free_gadget_calls.size();
    assert(!f.g(c.scroller)->NextGadget && f.lng(c.scroller) == c.tail);
    f.free(c.inc);
    assert(f.stopped() && native_calls == calls && free_gadget_calls.size() == frees &&
           filtered == before && held_by[context] == f.g(c.inc));
    m68k_write32(&f.ctx, c.scroller, 0);
    f.free(c.inc);
    assert(!f.stopped() && filtered == before + 1 && !held_by[context] && !f.native(c.inc));
    // a gadget that stays
    f.filter(IDCMP_GADGETDOWN, f.g(c.button));
    assert(f.remove(other, 2) == 6);
    before = filtered;
    f.free(other);
    assert(!f.stopped() && filtered == before && held_by[context] == f.g(c.button));
    // a context on no window, whose button goes with the tail after it
    f.closing(0);
    before = filtered;
    f.free(c.button);
    assert(!f.stopped() && filtered == before + 1 && !held_by[context] && f.native(c.context) &&
           !f.native(c.tail));
    // a context that goes with the gadget that it keeps
    assert(f.add_one(c.context) == 3);
    f.filter(IDCMP_GADGETDOWN, context);
    assert(held_by[context] == context && f.remove_one(c.context) == 3);
    before = filtered;
    f.free(c.context);
    assert(!f.stopped() && filtered == before && natives.empty());
}

/* GadTools changes the gadget that a message names as it filters the
   message, also when the program took its batch off the window after the
   message came: the sync of that message settles that batch too, and an
   address that is no gadget of the bridge settles only the window. */
static void test_window_sync_message_gadget()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG other = f.context();
    assert(f.add(c.context) == 3 && f.remove(c.inc, 3) == 5);
    const UWORD old_other = f.word(other + GD_LEFTEDGE);
    f.g(c.scroller)->LeftEdge = 61;
    f.g(other)->LeftEdge = 62;
    f.g(c.tail)->LeftEdge = 63;
    f.sync(0, f.g(c.inc));
    assert(f.word(c.scroller + GD_LEFTEDGE) == 61 && f.word(c.tail + GD_LEFTEDGE) == 63 &&
           f.word(other + GD_LEFTEDGE) == old_other);
    f.g(c.scroller)->LeftEdge = 71;
    f.g(c.tail)->LeftEdge = 72;
    f.sync(0, &f.windows[0]);
    f.sync(0, &f.system[0][1]);
    assert(f.word(c.scroller + GD_LEFTEDGE) == 61 && f.word(c.tail + GD_LEFTEDGE) == 72 &&
           f.word(other + GD_LEFTEDGE) == old_other);
}

/* With the window that its batch is on, a setter, OnGadget() and
   OffGadget() let GadTools draw there, and ActivateGadget() reaches
   Intuition and gives its BOOL; with another window, none or a requester
   the setters change without drawing, and ActivateGadget() gives FALSE
   without Intuition, as for a list on no window. */
static void test_window_setters_and_activation()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    const ULONG apart = f.context();
    f.newgad(10, 40, 100, 14, 0, 0, 5, 0, f.vi, 0);
    f.tagitems({ { GTST_String, f.s("abc") } });
    const ULONG string = f.create(STRING_KIND, apart);
    assert(string && f.add(c.context) == 3);
    set(f, c.button, { { GA_Disabled, 1 } }, f.win0);
    assert(!f.stopped() && set_window == &f.windows[0]);
    set(f, c.button, { { GA_Disabled, 0 } }, f.win1);
    assert(!f.stopped() && !set_window);
    set(f, c.button, { { GA_Disabled, 1 } });
    assert(!f.stopped() && !set_window);
    set(f, string, { { GTST_String, f.s("xy") } }, f.win0);
    assert(!f.stopped() && !set_window);
    for (ULONG lvo : { 174u, 186u })
    {
        assert(f.call_at(Fixture::intuition, lvo, { c.scroller, f.win0, 0 }) == 0 && !f.stopped() &&
               set_window == &f.windows[0]);
        assert(f.call_at(Fixture::intuition, lvo, { c.scroller, f.win1, 0 }) == 0 && !f.stopped() &&
               !set_window);
    }
    // ActivateGadget() on the window of the string's batch
    assert(f.add(apart, 0xFFFF, 2, 1) == 3);
    unsigned calls = native_calls;
    for (const auto &a : std::vector<std::array<ULONG, 3>>{
             { string, f.win0, 0 }, { string, f.win1, 0x3BB0 }, { string, 0, 0 },
             { string, Fixture::raw, 0 }, { Fixture::raw, f.win1, 0 }, { 0, f.win1, 0 } })
        assert(f.call_at(Fixture::intuition, 462, { a[0], a[1], a[2] }) == 0 && !f.stopped());
    assert(native_calls == calls);
    activate_result = FALSE;
    assert(f.call_at(Fixture::intuition, 462, { string, f.win1, 0 }) == 0 && !f.stopped() &&
           native_calls == calls + 1 && passed_object == f.g(string) && passed_window == &f.windows[1]);
    activate_result = TRUE;
    assert(f.call_at(Fixture::intuition, 462, { string, f.win1, 0 }) == 1 && !f.stopped());
    assert(f.word(string + GD_ACTIVATION) & GACT_ACTIVEGADGET);
    // taken off its window it is inactive, and is not activated again
    assert(f.remove(apart, 2, 1) == 3);
    assert(!intuition_active && !(f.word(string + GD_ACTIVATION) & GACT_ACTIVEGADGET));
    calls = native_calls;
    assert(f.call_at(Fixture::intuition, 462, { string, f.win1, 0 }) == 0 && native_calls == calls);
    f.free(apart);
    assert(!f.stopped());
    // the window closing, and the end of the program, deactivate it first
    assert(f.call_at(Fixture::intuition, 462, { c.button, f.win0, 0 }) == 1 && intuition_active);
    f.closing(0);
    assert(!intuition_active && !(f.word(c.button + GD_ACTIVATION) & GACT_ACTIVEGADGET));
    assert(f.add(c.context, 0xFFFF, 6, 1) == 3);
    assert(f.call_at(Fixture::intuition, 462, { c.tail, f.win1, 0 }) == 1 && intuition_active);
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(!intuition_active && natives.empty());
}

/* The calls of a later part, and what the program leaves at its end */
static void test_later_and_end()
{
    Fixture f;
    const ULONG c = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    f.tagitems({});
    const ULONG b = f.create(BUTTON_KIND, c);
    unsigned calls = native_calls;

    /* nothing, and strangers, are left alone */
    f.free(0);
    f.free(Fixture::raw);
    assert(!f.stopped() && native_calls == calls);

    /* GT_SetGadgetAttrsA(): nothing for NULL, as GadTools, nor for an
       empty list */
    assert(f.call(42, { 0, 0, 0, Fixture::tags }) == 0 && !f.stopped());
    assert(f.call(42, { b, 0, 0, Fixture::tags }) == 0 && !f.stopped() && native_calls == calls);

    /* DisposeObject(): GadTools frees its gadgets with FreeGadgets(); other
       objects go to Intuition as the generated thunk passes them */
    f.call_at(Fixture::intuition, 642, { b });
    assert(f.stopped() && native_calls == calls && disposed.empty());
    static int object;
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x3B90, &object, FILLER));
    f.call_at(Fixture::intuition, 642, { 0x3B90 });
    assert(!f.stopped() && disposed.size() == 1 && disposed[0] == &object);
    f.call_at(Fixture::intuition, 642, { 0 });
    assert(!f.stopped() && disposed.size() == 2 && disposed[1] == nullptr);
    Kaffeine_ShadowForget(&f.ctx, 0x3B90, FILLER);

    /* two lists, one freed, one left for the end of the program */
    m68k_write32(&f.ctx, Fixture::glist, 0);
    const ULONG c2 = f.context();
    f.newgad(10, 10, 80, 14, 0, 0, 1, 0, f.vi, 0);
    const ULONG b2 = f.create(BUTTON_KIND, c2);
    assert(c2 && b2 && natives.size() == 4 && f.native(b2) && f.native(b));
    f.free(c);
    assert(natives.size() == 2 && !Kaffeine_ShadowNative(&f.ctx, b, KAFFEINE_SHADOW_GADGET) && f.native(b2));
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* the VisualInfo goes last */
    f.call(132, { f.vi });
    assert(visualinfos.empty());
}

/* The program's own NDK structures. These are bytes in guest memory,
   never host structs, so native padding cannot conceal an offset error. */
static constexpr ULONG OWN = 0x4600, BORDER = 0x4800, XY = 0x4900, IMAGE = 0x4A00;
static void own_gadget(Fixture &f, ULONG at = OWN, ULONG next = 0, UWORD flags = GFLG_GADGHNONE)
{
    std::memset(f.memory.data() + at, 0, flags & GFLG_EXTENDED ? 56 : 44);
    m68k_write32(&f.ctx, at, next);
    m68k_write16(&f.ctx, at + 8, 80);
    m68k_write16(&f.ctx, at + 10, 12);
    m68k_write16(&f.ctx, at + 12, flags);
    m68k_write16(&f.ctx, at + 14, GACT_RELVERIFY);
    m68k_write16(&f.ctx, at + 16, GTYP_BOOLGADGET);
}
static std::vector<ULONG> own_chain(Fixture &f, unsigned n = 3)
{
    std::vector<ULONG> out;
    for (unsigned i = 0; i < n; ++i)
    {
        const ULONG at = OWN + i * 64;
        own_gadget(f, at, i + 1 == n ? 0 : at + 64);
        out.push_back(at);
    }
    return out;
}
static void own_border(Fixture &f, ULONG at = BORDER, BYTE count = 2, ULONG xy = XY, ULONG next = 0)
{
    std::memset(f.memory.data() + at, 0, 16);
    m68k_write16(&f.ctx, at, (UWORD)-123);
    m68k_write16(&f.ctx, at + 2, (UWORD)-32768);
    f.memory[at + 4] = 201;
    f.memory[at + 5] = 102;
    f.memory[at + 6] = 3;
    f.memory[at + 7] = (UBYTE)count;
    m68k_write32(&f.ctx, at + 8, xy);
    m68k_write32(&f.ctx, at + 12, next);
}
static void own_image(Fixture &f, ULONG at = IMAGE, ULONG next = 0)
{
    std::memset(f.memory.data() + at, 0, 20);
    m68k_write16(&f.ctx, at, (UWORD)-19);
    m68k_write16(&f.ctx, at + 2, (UWORD)-23);
    m68k_write16(&f.ctx, at + 4, 17);
    m68k_write16(&f.ctx, at + 6, 2);
    m68k_write16(&f.ctx, at + 8, 1);
    m68k_write32(&f.ctx, at + 10, XY);
    f.memory[at + 14] = 1;
    f.memory[at + 15] = 2;
    m68k_write32(&f.ctx, at + 16, next);
}

template<class T> struct Copied
{
    KaffeineCopy copy{};
    APTR block;
    T *head;
    Copied(Fixture &f, ULONG guest, T *(*fn)(KaffeineContext *, KaffeineCopy *, ULONG))
    {
        const unsigned allocations = native_allocations;
        assert(!fn(&f.ctx, &copy, guest) && !copy.why && copy.used);
        assert(native_allocations == allocations); // the first run only counts
        const ULONG size = copy.used, objects = copy.objects;
        block = AllocVec(size, MEMF_CLEAR);
        assert(block);
        Kaffeine_CopyFill(&copy, static_cast<UBYTE *>(block));
        head = fn(&f.ctx, &copy, guest);
        assert(head && !copy.why && copy.used == size && copy.room == size && copy.objects == objects);
        assert(block_holding(head) == block);
    }
    ~Copied() { FreeVec(block); }
};

struct ClassicFixture : WindowFixture
{
    MsgPort ports[2]{};
    ULONG guest_ports[2]{};
    explicit ClassicFixture(size_t mem_size = 0x10000, bool guarded = false) : WindowFixture(mem_size, guarded)
    {
        Kaffeine_GadgetsSetup(&ctx);
        static_assert(KAFFEINE_SHADOW_CLASSIC == 14);
        for (unsigned w = 0; w < 2; ++w)
        {
            empty_port(ports[w]);
            windows[w].UserPort = &ports[w];
        }
    }
    ~ClassicFixture()
    {
        Kaffeine_GadgetsCleanup(&ctx);
        for (ULONG port : guest_ports) Kaffeine_UserPortDelete(&ctx, port);
    }
    Gadget *copy(ULONG a)
    {
        auto *g = static_cast<Gadget *>(Kaffeine_ShadowNative(&ctx, a, KAFFEINE_SHADOW_CLASSIC));
        assert(g && Kaffeine_ClassicOn(&ctx, a) && block_holding(g));
        assert(!Kaffeine_ShadowNative(&ctx, a, KAFFEINE_SHADOW_GADGET));
        assert(Kaffeine_ClassicGuest(&ctx, g) == a);
        return g;
    }
    ULONG port(unsigned w = 0)
    {
        if (!guest_ports[w]) guest_ports[w] = Kaffeine_UserPortCreate(&ctx, &ports[w]);
        assert(guest_ports[w]);
        return guest_ports[w];
    }
    void message(IntuiMessage &im, Gadget *g, ULONG cls = IDCMP_GADGETUP, unsigned w = 0)
    {
        im = {};
        im.IAddress = g;
        im.Class = cls;
        im.IDCMPWindow = &windows[w];
        queue_message(ports[w], im);
    }
};

static void test_classic_border_offsets()
{
    Fixture f;
    own_border(f, BORDER, 2, XY, BORDER + 16);
    own_border(f, BORDER + 16, 1, XY + 8);
    const std::array<WORD, 6> values = { -32768, 32767, -2, 0x1234, 29, -301 };
    for (unsigned i = 0; i < values.size(); ++i)
        m68k_write16(&f.ctx, XY + i * 2, (UWORD)values[i]);
    const auto memory = f.memory;
    Copied<Border> c(f, BORDER, Kaffeine_CopyBorders);
    Border *b = c.head;
    assert(b->LeftEdge == -123 && b->TopEdge == -32768 && b->FrontPen == 201 &&
           b->BackPen == 102 && b->DrawMode == 3 && b->Count == 2);
    assert(block_holding(b->XY) == c.block && b->XY != reinterpret_cast<WORD *>(f.memory.data() + XY));
    assert(std::equal(values.begin(), values.begin() + 4, b->XY));
    assert(b->NextBorder && b->NextBorder->Count == 1 && !b->NextBorder->NextBorder &&
           b->NextBorder->XY[0] == 29 && b->NextBorder->XY[1] == -301);
    assert(f.memory == memory);
    m68k_write16(&f.ctx, XY, 123);
    assert(b->XY[0] == -32768); // the XY WORDs belong to the copy
}

static void test_classic_border_counts_and_bounds()
{
    Fixture f;
    for (int count : { 0, -1, -128 })
    {
        own_border(f, BORDER, (BYTE)count, 0xFFFFFFF0);
        Copied<Border> c(f, BORDER, Kaffeine_CopyBorders);
        assert(c.head->Count == count && !c.head->XY && c.copy.objects == 1);
    }
    own_border(f, BORDER, 127);
    for (unsigned i = 0; i < 254; ++i) m68k_write16(&f.ctx, XY + i * 2, (UWORD)(i - 127));
    {
        Copied<Border> c(f, BORDER, Kaffeine_CopyBorders);
        assert(c.head->Count == 127 && c.head->XY[0] == -127 && c.head->XY[253] == 126);
    }
    for (ULONG xy : { 0u, 0xFFFFu, 0xFFFCu, 0x10000u, 0xFFFFFFFCu })
    {
        own_border(f, BORDER, 2, xy);
        KaffeineCopy c{};
        assert(!Kaffeine_CopyBorders(&f.ctx, &c, BORDER) && c.why && !c.used);
    }
    m68k_write16(&f.ctx, 0xFFFC, (UWORD)-15);
    m68k_write16(&f.ctx, 0xFFFE, 999);
    // Keep the 16-byte Border separate from the final four XY bytes.
    own_border(f, BORDER, 1, 0xFFFC);
    Copied<Border> c(f, BORDER, Kaffeine_CopyBorders);
    assert(c.head->XY[0] == -15 && c.head->XY[1] == 999);
    for (ULONG at : { 0xFFF1u, 0xFFFFFFF8u })
    {
        KaffeineCopy bad{};
        assert(!Kaffeine_CopyBorders(&f.ctx, &bad, at) && bad.why);
    }
    own_border(f, 0xFFF0, 0, 0xFFFFFFF0);
    Copied<Border> edge(f, 0xFFF0, Kaffeine_CopyBorders); // exactly 16 guest bytes
    assert(!edge.head->XY && !edge.head->NextBorder);
}

static void test_classic_copy_chain_limits()
{
    Fixture f(0x20000);
    for (unsigned i = 0; i < 1025; ++i)
        own_border(f, 0x10000 + i * 16, 0, 0, i == 1023 ? 0 : 0x10000 + (i + 1) * 16);
    {
        Copied<Border> c(f, 0x10000, Kaffeine_CopyBorders);
        unsigned n = 0;
        for (Border *b = c.head; b; b = b->NextBorder) { assert(++n <= 1024); }
        assert(n == 1024);
    }
    for (ULONG next : { 0x14000u, 0x10000u })
    {
        m68k_write32(&f.ctx, 0x13FF0 + 12, next);
        m68k_write32(&f.ctx, 0x14000 + 12, 0);
        KaffeineCopy c{};
        assert(!Kaffeine_CopyBorders(&f.ctx, &c, 0x10000) && c.why);
    }
    // Image chains reached through GadgetRender have the same 1024 limit.
    for (unsigned i = 0; i < 1025; ++i)
        own_image(f, 0x10000 + i * 20, i == 1023 ? 0 : 0x10000 + (i + 1) * 20);
    own_gadget(f, OWN, 0, GFLG_GADGIMAGE);
    m68k_write32(&f.ctx, OWN + 18, 0x10000);
    {
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        unsigned n = 0;
        for (auto *im = static_cast<Image *>(c.head->GadgetRender); im; im = im->NextImage)
            assert(++n <= 1024);
        assert(n == 1024);
    }
    for (ULONG next : { 0x15000u, 0x10000u })
    {
        m68k_write32(&f.ctx, 0x10000 + 1023 * 20 + 16, next);
        m68k_write32(&f.ctx, 0x15000 + 16, 0);
        KaffeineCopy c{};
        assert(!Kaffeine_CopyGadget(&f.ctx, &c, OWN) && c.why);
    }
}

static void test_classic_gadget_offsets()
{
    Fixture f;
    for (bool extended : { false, true })
    {
        const ULONG size = extended ? 56 : 44, at = f.ctx.mem_size - size;
        const UWORD flags = GFLG_SELECTED | GFLG_GADGHBOX | (extended ? GFLG_EXTENDED : 0);
        own_gadget(f, at, 0xFFFFFFF0, flags);
        m68k_write16(&f.ctx, at + 4, (UWORD)-32768);
        m68k_write16(&f.ctx, at + 6, (UWORD)-123);
        m68k_write16(&f.ctx, at + 8, (UWORD)-77);
        m68k_write16(&f.ctx, at + 10, 32767);
        m68k_write16(&f.ctx, at + 14, 0x8103);
        m68k_write16(&f.ctx, at + 16, 0x2001);
        m68k_write32(&f.ctx, at + 22, 0xFFFFFFF4); // unused SelectRender
        m68k_write32(&f.ctx, at + 30, 0xFEDCBA98);
        m68k_write32(&f.ctx, at + 34, 0xFFFFFFF8); // no SpecialInfo copy
        m68k_write16(&f.ctx, at + 38, 0xCDEF);
        m68k_write32(&f.ctx, at + 40, 0x87654321);
        if (extended)
        {
            m68k_write32(&f.ctx, at + 44, 0xFFFFFFF5);
            m68k_write16(&f.ctx, at + 48, (UWORD)-300);
            m68k_write16(&f.ctx, at + 50, (UWORD)-32768);
            m68k_write16(&f.ctx, at + 52, (UWORD)-1);
            m68k_write16(&f.ctx, at + 54, 32767);
        }
        const auto memory = f.memory;
        Copied<Gadget> c(f, at, Kaffeine_CopyGadget);
        Gadget *g = c.head;
        assert(!g->NextGadget && g->LeftEdge == -32768 && g->TopEdge == -123 &&
               g->Width == -77 && g->Height == 32767 && g->Flags == flags &&
               g->Activation == 0x8103 && g->GadgetType == 0x2001 && g->GadgetID == 0xCDEF &&
               (IPTR)g->MutualExclude == 0xFEDCBA98 && (IPTR)g->UserData == 0x87654321 &&
               !g->GadgetRender && !g->SelectRender && !g->GadgetText && !g->SpecialInfo);
        assert(c.copy.objects == 1 && f.memory == memory);
        if (extended)
        {
            auto *eg = reinterpret_cast<ExtGadget *>(g);
            assert(eg->MoreFlags == 5 && eg->BoundsLeftEdge == -300 && eg->BoundsTopEdge == -32768 &&
                   eg->BoundsWidth == -1 && eg->BoundsHeight == 32767);
        }
        KaffeineCopy bad{};
        if (extended) m68k_write16(&f.ctx, at + 1 + 12, GFLG_EXTENDED);
        assert(!Kaffeine_CopyGadget(&f.ctx, &bad, at + 1) && bad.why);
    }
    own_gadget(f, OWN, 0, GFLG_EXTENDED);
    for (ULONG more : { 0u, 1u, 2u, 4u, 7u, 0x400u, 0xFFFFFFFFu })
    {
        m68k_write32(&f.ctx, OWN + 44, more);
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        // GMORE_GADGETHELP stays out: Intuition keeps a help gadget past RemoveGList()
        assert(reinterpret_cast<ExtGadget *>(c.head)->MoreFlags == (more & 5));
    }
}

static void test_classic_renders_and_labels()
{
    Fixture f;
    own_border(f, BORDER, 0, 0xFFFFFFFC, BORDER + 16);
    own_border(f, BORDER + 16, 0);
    own_image(f, IMAGE, IMAGE + 20);
    own_image(f, IMAGE + 20);
    for (UWORD kind : { (UWORD)0, (UWORD)GFLG_GADGIMAGE })
        for (UWORD highlight : { (UWORD)GFLG_GADGHCOMP, (UWORD)GFLG_GADGHBOX,
                                (UWORD)GFLG_GADGHIMAGE, (UWORD)GFLG_GADGHNONE })
        {
            own_gadget(f, OWN, 0, kind | highlight);
            m68k_write32(&f.ctx, OWN + 18, kind ? IMAGE : BORDER);
            m68k_write32(&f.ctx, OWN + 22, highlight == GFLG_GADGHIMAGE ? (kind ? IMAGE : BORDER) : 0xFFFFFFFC);
            Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
            assert(c.head->GadgetRender && !!c.head->SelectRender == (highlight == GFLG_GADGHIMAGE));
            for (APTR render : { c.head->GadgetRender, c.head->SelectRender })
            {
                if (!render) continue;
                assert(block_holding(render) == c.block);
                if (kind)
                {
                    auto *im = static_cast<Image *>(render);
                    assert(im->LeftEdge == -19 && im->TopEdge == -23 && im->Width == 17 && im->Height == 2 &&
                           im->Depth == 1 && im->PlanePick == 1 && im->PlaneOnOff == 2 &&
                           im->ImageData == reinterpret_cast<UWORD *>(f.memory.data() + XY) &&
                           im->NextImage && !im->NextImage->NextImage);
                }
                else
                {
                    auto *b = static_cast<Border *>(render);
                    assert(b->LeftEdge == -123 && b->Count == 0 && b->NextBorder && !b->NextBorder->NextBorder);
                }
            }
            if (highlight == GFLG_GADGHIMAGE)
            {
                m68k_write32(&f.ctx, OWN + 22, 0xFFFFFFFC);
                KaffeineCopy bad{};
                assert(!Kaffeine_CopyGadget(&f.ctx, &bad, OWN) && bad.why);
            }
        }
    const ULONG text = f.s("Classic label"), it = 0x4B00;
    std::memset(f.memory.data() + it, 0, 40);
    m68k_write16(&f.ctx, it + 4, (UWORD)-17);
    m68k_write32(&f.ctx, it + 12, text);
    m68k_write32(&f.ctx, it + 16, it + 20);
    m68k_write32(&f.ctx, it + 20 + 12, text);
    for (UWORD kind : { (UWORD)GFLG_LABELITEXT, (UWORD)GFLG_LABELSTRING,
                       (UWORD)GFLG_LABELIMAGE, (UWORD)GFLG_LABELMASK })
    {
        own_gadget(f, OWN, 0, kind);
        const ULONG label = kind == GFLG_LABELITEXT ? it : kind == GFLG_LABELSTRING ? text :
                            kind == GFLG_LABELIMAGE ? IMAGE : 0xFFFFFFFC;
        m68k_write32(&f.ctx, OWN + 26, label);
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        if (kind == GFLG_LABELMASK) assert(!c.head->GadgetText);
        else
        {
            assert(block_holding(c.head->GadgetText) == c.block);
            if (kind == GFLG_LABELITEXT)
                assert(c.head->GadgetText->LeftEdge == -17 && c.head->GadgetText->NextText &&
                       !c.head->GadgetText->NextText->NextText &&
                       !std::strcmp(reinterpret_cast<char *>(c.head->GadgetText->IText), "Classic label"));
            else if (kind == GFLG_LABELSTRING)
                assert(!std::strcmp(reinterpret_cast<char *>(c.head->GadgetText), "Classic label"));
            else
                assert(reinterpret_cast<Image *>(c.head->GadgetText)->NextImage);
            m68k_write32(&f.ctx, OWN + 26, 0xFFFFFFFC);
            KaffeineCopy bad{};
            assert(!Kaffeine_CopyGadget(&f.ctx, &bad, OWN) && bad.why);
        }
    }
}

/* This fixture opens a real bridge Window, rather than a pre-made window
   shadow. The native open stub appends copies after its system gadgets. */
struct ClassicOpenFixture : Fixture
{
    Window window{};
    Gadget system[3]{};
    MsgPort port{};
    ULONG mw = 0;
    ClassicOpenFixture()
    {
        Kaffeine_GadgetsSetup(&ctx);
        system[0].NextGadget = &system[1]; system[1].NextGadget = &system[2];
        window.FirstGadget = system;
        empty_port(port);
        window.UserPort = &port;
        watched_windows.push_back(&window);
    }
    ~ClassicOpenFixture()
    {
        if (mw) close();
        Kaffeine_GadgetsCleanup(&ctx);
        watched_windows.clear();
    }
    ULONG open(ULONG lvo, ULONG head, bool fails = false)
    {
        open_result = fails ? nullptr : &window;
        ULONG result;
        if (lvo == 606)
        {
            tagitems({ { WA_Gadgets, head } });
            result = call_at(intuition, 606, { 0, tags });
        }
        else
        {
            const ULONG nw = 0x4D00;
            std::memset(memory.data() + nw, 0, M68K_SIZEOF_NEWWINDOW);
            m68k_write32(&ctx, nw + M68K_NW_FIRSTGADGET, head);
            m68k_write16(&ctx, nw + M68K_NW_TYPE, WBENCHSCREEN);
            result = call_at(intuition, 204, { nw });
        }
        if (result && Kaffeine_ShadowNative(&ctx, result, KAFFEINE_SHADOW_WINDOW) == &window)
            mw = result;
        return result;
    }
    void close()
    {
        assert(mw && call_at(intuition, 72, { mw }) == 0 && !stopped());
        mw = 0;
    }
};

/* Keep the native calls' links as they were at entry, before GadTools
   disposes anything. Batch order is private to the bridge. */
static void assert_free_calls(size_t before, const std::vector<std::vector<Gadget *>> &batches)
{
    assert(free_gadget_calls.size() == before + batches.size());
    for (const auto &batch : batches)
    {
        assert(!batch.empty());
        const auto call = std::find_if(free_gadget_calls.begin() + before, free_gadget_calls.end(),
                                      [&](const FreedList &c) { return c.head == batch.front(); });
        assert(call != free_gadget_calls.end() && call->links.size() == batch.size());
        for (size_t n = 0; n < batch.size(); ++n)
            assert(call->links[n].first == batch[n] &&
                   call->links[n].second == (n + 1 < batch.size() ? batch[n + 1] : nullptr));
    }
}

struct FreeWalkDeadline
{
    using Handler = void (*)(int);
    Handler previous;
    FreeWalkDeadline()
    {
        previous = std::signal(SIGALRM, [](int) {
            std::fputs("FreeGadgets walk exceeded five seconds\n", stderr);
            std::_Exit(1);
        });
        alarm(5);
    }
    ~FreeWalkDeadline() { alarm(0); std::signal(SIGALRM, previous); }
};

static Gadget *native_gadget(Fixture &f, ULONG guest)
{
    assert(f.native(guest));
    return G(f.native(guest));
}

static std::vector<std::vector<Gadget *>> window_batches(Fixture &f, const WindowChain &c)
{
    return { { native_gadget(f, c.context) }, { native_gadget(f, c.button) },
             { native_gadget(f, c.inc), native_gadget(f, c.dec), native_gadget(f, c.scroller) },
             { native_gadget(f, c.tail) } };
}

static void assert_kept_chain(Fixture &f, const std::vector<ULONG> &list)
{
    for (size_t n = 0; n < list.size(); ++n)
    {
        const ULONG next = n + 1 < list.size() ? list[n + 1] : 0;
        assert(native_gadget(f, list[n])->NextGadget == (next ? native_gadget(f, next) : nullptr));
        assert(f.lng(list[n] + GD_NEXTGADGET) == next);
    }
}

/* Remove complete batches one by one, from the front. The detached
   native runs end, while the program's list keeps all six gadgets. */
static void remove_batches_one_by_one(Fixture &f, Window *window, ULONG win, const WindowChain &c)
{
    const std::array<ULONG, 4> first = { c.context, c.button, c.inc, c.tail };
    const std::array<ULONG, 4> last = { c.context, c.button, c.scroller, c.tail };
    const std::array<ULONG, 4> after = { c.button, c.inc, c.tail, 0 };
    const std::array<WORD, 4> count = { 1, 1, 3, 1 };
    for (size_t n = 0; n < first.size(); ++n)
    {
        const unsigned calls = native_calls;
        const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
        assert(f.call_at(Fixture::intuition, 444, { win, first[n] }, { (ULONG)count[n] }) == 3);
        assert(f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1 &&
               gadget_calls.size() == records + 1 && free_gadget_calls.size() == frees);
        assert_gadget_call("RemoveGList", window, native_gadget(f, first[n]), count[n], 3);
        assert(gadget_calls.back().last == native_gadget(f, last[n]) &&
               gadget_calls.back().after == (after[n] ? native_gadget(f, after[n]) : nullptr));
        assert(!native_gadget(f, last[n])->NextGadget && f.lng(last[n] + GD_NEXTGADGET) == after[n]);
    }
    assert(native_gadget(f, c.inc)->NextGadget == native_gadget(f, c.dec) &&
           native_gadget(f, c.dec)->NextGadget == native_gadget(f, c.scroller));
    const auto all = c.all();
    for (size_t n = 0; n < all.size(); ++n)
        assert(f.lng(all[n] + GD_NEXTGADGET) == (n + 1 < all.size() ? all[n + 1] : 0));
}

static void test_kept_remove_successors()
{
    enum Following { MIRROR, CLASSIC, SYSTEM, END };
    for (bool one : { false, true })
        for (Following following : { MIRROR, CLASSIC, SYSTEM, END })
        {
            ClassicFixture f;
            const WindowChain c = window_chain(f);
            assert(f.add(c.context, 1) == 1);
            ULONG removed = c.button, guest_after = c.inc;
            Gadget *after = f.g(c.inc);
            if (following == CLASSIC)
            {
                own_gadget(f);
                assert(f.add_one(OWN, 3) == 3);
                after = f.copy(OWN);
                guest_after = OWN;
            }
            else if (following == SYSTEM)
            {
                removed = c.tail;
                after = &f.system[0][1];
                guest_after = 0;
            }
            else if (following == END)
            {
                const ULONG apart = f.context();
                assert(f.add_one(apart) == 9);
                removed = apart;
                after = nullptr;
                guest_after = 0;
            }
            const auto list = f.list(0);
            const auto at = std::find(list.begin(), list.end(), f.g(removed));
            assert(at != list.end());
            const UWORD position = (UWORD)(at - list.begin());
            const unsigned calls = native_calls;
            const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
            assert((one ? f.remove_one(removed) : f.remove(removed, 1)) == position);
            assert(f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1 &&
                   gadget_calls.size() == records + 1 && free_gadget_calls.size() == frees);
            assert_gadget_call("RemoveGList", &f.windows[0], f.g(removed), 1, position);
            assert(gadget_calls.back().last == f.g(removed) && gadget_calls.back().after == after);
            assert(!f.g(removed)->NextGadget && f.lng(removed + GD_NEXTGADGET) == guest_after);
            auto expected = list;
            expected.erase(expected.begin() + position);
            assert(f.list(0) == expected);
            // Avoid freeing an uncut kept list into the remaining window during fixture cleanup.
            f.closing(0);
        }
}

static void test_kept_close_classic_separators()
{
    ClassicOpenFixture f;
    const WindowChain c = window_chain(f);
    own_gadget(f); own_gadget(f, OWN + 64);
    assert(f.open(606, c.context));
    assert(f.call_at(Fixture::intuition, 42, { f.mw, OWN }, { 4 }) == 4 &&
           f.call_at(Fixture::intuition, 42, { f.mw, OWN + 64 }, { 9 }) == 9);
    Gadget *first_copy = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, OWN, KAFFEINE_SHADOW_CLASSIC));
    Gadget *second_copy = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, OWN + 64, KAFFEINE_SHADOW_CLASSIC));
    assert(first_copy && second_copy);
    const std::vector<ULONG> program = { c.context, OWN, c.button, c.inc, c.dec, c.scroller, OWN + 64, c.tail };
    std::vector<std::pair<ULONG, ULONG>> links;
    for (size_t n = 0; n < program.size(); ++n)
    {
        const ULONG next = f.lng(program[n] + GD_NEXTGADGET);
        assert(next == (n + 1 < program.size() ? program[n + 1] : 0));
        links.push_back({ program[n], next });
    }
    std::array<std::vector<UBYTE>, 2> own;
    for (size_t n = 0; n < own.size(); ++n)
        own[n].assign(f.memory.begin() + OWN + n * 64, f.memory.begin() + OWN + n * 64 + 44);
    const auto batches = window_batches(f, c);
    const unsigned calls = native_calls;
    const size_t records = gadget_calls.size(), frees = free_gadget_calls.size(), closed = closed_windows.size();
    f.close(); // the actual CloseWindow LVO, including the gadget and classic closing hooks
    // Three GadTools runs, one classic run, CloseWindow and its empty-port GetMsg.
    assert(f.ctx.running && !f.ctx.exit_code && native_calls == calls + 6 &&
           gadget_calls.size() == records + 4 && free_gadget_calls.size() == frees &&
           closed_windows.size() == closed + 1 && closed_windows.back() == &f.window);
    const std::array<Gadget *, 4> heads = { native_gadget(f, c.context), native_gadget(f, c.button),
                                          native_gadget(f, c.tail), first_copy };
    const std::array<Gadget *, 4> lasts = { native_gadget(f, c.context), native_gadget(f, c.scroller),
                                          native_gadget(f, c.tail), second_copy };
    const std::array<Gadget *, 4> after = { first_copy, second_copy, nullptr, nullptr };
    const std::array<WORD, 4> count = { 1, 4, 1, 2 };
    const std::array<UWORD, 4> pos = { 3, 4, 5, 3 };
    for (size_t n = 0; n < heads.size(); ++n)
    {
        const GadgetCall &call = gadget_calls[records + n];
        assert(std::strcmp(call.name, "RemoveGList") == 0 && call.window == &f.window &&
               call.head == heads[n] && call.last == lasts[n] && call.after == after[n] &&
               call.num == count[n] && call.result == pos[n] && !call.requester);
    }
    for (const auto &link : links) assert(f.lng(link.first + GD_NEXTGADGET) == link.second);
    for (size_t n = 0; n < own.size(); ++n)
        assert(std::equal(own[n].begin(), own[n].end(), f.memory.begin() + OWN + n * 64));
    assert(!native_gadget(f, c.context)->NextGadget && !native_gadget(f, c.scroller)->NextGadget &&
           !native_gadget(f, c.tail)->NextGadget &&
           native_gadget(f, c.button)->NextGadget == native_gadget(f, c.inc) &&
           native_gadget(f, c.inc)->NextGadget == native_gadget(f, c.dec) &&
           native_gadget(f, c.dec)->NextGadget == native_gadget(f, c.scroller));
    assert(f.window.FirstGadget == &f.system[0] && f.system[0].NextGadget == &f.system[1] &&
           f.system[1].NextGadget == &f.system[2] && !f.system[2].NextGadget && !f.ctx.classic);
    const unsigned before = native_calls, before_freed = freed;
    assert(f.call(36, { c.context }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == before + 4);
    assert_free_calls(frees, batches);
    assert(freed == before_freed + 6 && natives.empty() && !f.ctx.gadgets && native_blocks.empty() &&
           gadget_calls.size() == records + 4 && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    for (ULONG a : c.all()) assert(!f.native(a));
    for (size_t n = 0; n < own.size(); ++n)
        assert(std::equal(own[n].begin(), own[n].end(), f.memory.begin() + OWN + n * 64));
}

static void test_kept_free_filter_failure()
{
    ClassicFixture f;
    const WindowChain c = window_chain(f);
    const ULONG extra = f.context(), observer = f.context(), spare = f.context();
    assert(f.add(c.context) == 3);
    Gadget *context = f.g(c.context), *held = f.g(c.inc), *extra_g = f.g(extra), *spare_g = f.g(spare);
    IntuiMessage down{};
    f.message(down, held, IDCMP_GADGETDOWN);
    const ULONG message = f.call(72, { f.port() });
    assert(message && !f.stopped() && held_by[context] == held && filtered_originals.size() == 1);
    auto *copy = static_cast<IntuiMessage *>(Kaffeine_ShadowNative(&f.ctx, message, KAFFEINE_SHADOW_GTMESSAGE));
    assert(copy && filtered_originals.at(copy) == &down);
    const auto originals = filtered_originals;

    assert(f.remove(c.inc, 3) == 5);
    m68k_write32(&f.ctx, c.scroller, extra); // two batches will be doomed; the on-window tail stays
    m68k_write32(&f.ctx, c.button, c.inc); // a surviving guest link into the scroller batch
    f.g(observer)->NextGadget = f.g(c.dec); // an independent surviving native link into its middle
    m68k_write32(&f.ctx, observer, extra); // this guest link names the other doomed batch
    const std::vector<std::vector<Gadget *>> going = { { held, f.g(c.dec), f.g(c.scroller) }, { extra_g } };
    std::map<Native *, std::vector<UBYTE>> native_data;
    for (const auto &n : natives)
    {
        const auto *bytes = reinterpret_cast<const UBYTE *>(n.second);
        native_data[n.second] = { bytes, bytes + sizeof(*n.second) };
    }
    const size_t frees = free_gadget_calls.size(), window_calls = gadget_calls.size(), filters = filter_calls.size();
    const unsigned before = filtered;
    filter_fails_once = true;
    // The helper compares all guest memory, native links, identities,
    // shadows, blocks and heap space; only the rejected filter is called.
    assert_showsel_free_refused(f, c.inc, 1);
    assert(!filter_fails_once && filtered == before + 1 && filter_calls.size() == filters + 1 &&
           free_gadget_calls.size() == frees && gadget_calls.size() == window_calls &&
           filtered_originals == originals && held_by[context] == held);
    const FilterCall &failed = filter_calls.back();
    assert(failed.failed && failed.cls == IDCMP_GADGETUP && failed.context == context && !failed.address);
    for (const auto &n : native_data)
        assert(std::memcmp(n.first, n.second.data(), n.second.size()) == 0);
    assert(Kaffeine_ShadowNative(&f.ctx, message, KAFFEINE_SHADOW_GTMESSAGE) == copy);

    // An unrelated valid free must release only itself. Any stale doomed
    // flag on either pending batch would free it or cut its incoming links.
    unsigned calls = native_calls;
    assert(f.call(36, { spare }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1);
    assert_free_calls(frees, { { spare_g } });
    assert(f.native(c.inc) && f.native(extra) && !f.native(spare) && held_by[context] == held &&
           filtered == before + 1 && filtered_originals == originals &&
           f.g(observer)->NextGadget == f.g(c.dec) && f.lng(observer) == extra && f.lng(c.button) == c.inc);

    // Retry without rearming: the one-shot failure has gone, and the
    // bridge must still remember which gadget the surviving context held.
    calls = native_calls;
    const size_t retry = free_gadget_calls.size();
    assert(f.call(36, { c.inc }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 4);
    assert_free_calls(retry, going); // filter + postfilter + one native free per batch
    assert(filtered == before + 2 && filter_calls.size() == filters + 2 && !held_by[context] &&
           filtered_originals == originals && !f.native(c.inc) && !f.native(c.dec) &&
           !f.native(c.scroller) && !f.native(extra));
    const FilterCall &succeeded = filter_calls.back();
    assert(!succeeded.failed && succeeded.cls == IDCMP_GADGETUP && succeeded.context == context && !succeeded.address);
    assert(!f.g(observer)->NextGadget && !f.lng(observer) && !f.lng(c.button) &&
           f.g(c.button)->NextGadget == f.g(c.tail) && f.g(c.context)->NextGadget == f.g(c.button) &&
           f.lng(c.context) == c.button);
    assert(f.call(78, { 0, message }) == 0 && !f.stopped() && filtered_originals.empty() &&
           replied_messages.size() == 1 && replied_messages.back() == &down.ExecMessage);
    f.closing(0);
    f.free(c.context); f.free(c.tail); f.free(observer);
    assert(!f.stopped() && natives.empty() && !f.ctx.gadgets);
}

static void test_kept_free_unrelated_held_gadget()
{
    ClassicFixture f;
    const WindowChain c = window_chain(f);
    const ULONG apart = f.context();
    Gadget *apart_g = f.g(apart), *context = f.g(c.context), *held = f.g(c.inc);
    assert(f.add(c.context) == 3);
    IntuiMessage down{};
    f.message(down, held, IDCMP_GADGETDOWN);
    const ULONG message = f.call(72, { f.port() });
    assert(message && !f.stopped() && held_by[context] == held && filtered_originals.size() == 1);
    const auto originals = filtered_originals;
    const auto list = f.list(0);
    const unsigned calls = native_calls, before = filtered;
    const size_t frees = free_gadget_calls.size(), filters = filter_calls.size(), window_calls = gadget_calls.size();
    filter_fails_once = true;
    assert(f.call(36, { apart }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1);
    assert_free_calls(frees, { { apart_g } });
    assert(filter_fails_once && filtered == before && filter_calls.size() == filters &&
           filtered_originals == originals && held_by[context] == held && !f.native(apart) &&
           gadget_calls.size() == window_calls && f.list(0) == list);
    assert_kept_chain(f, c.all());

    // Finish the outstanding message and the interaction before closing.
    filter_fails_once = false;
    assert(f.call(78, { 0, message }) == 0 && !f.stopped() && filtered_originals.empty());
    f.filter(IDCMP_GADGETUP, nullptr);
    assert(!held_by[context]);
    f.closing(0);
    const auto batches = window_batches(f, c);
    const size_t later = free_gadget_calls.size();
    f.free(c.context);
    assert(!f.stopped() && natives.empty() && !f.ctx.gadgets);
    assert_free_calls(later, batches);
}

static void test_kept_readd_removed_batches()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3);
    remove_batches_one_by_one(f, &f.windows[0], f.win0, c);
    f.only_system(0);
    const unsigned calls = native_calls;
    const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
    assert(f.add(c.context) == 3 && f.ctx.running && !f.ctx.exit_code);
    assert(native_calls == calls + 1 && gadget_calls.size() == records + 1 && free_gadget_calls.size() == frees);
    assert_gadget_call("AddGList", &f.windows[0], f.g(c.context), 6, 3);
    assert(gadget_calls.back().position == 0xFFFF && gadget_calls.back().last == f.g(c.tail) &&
           !gadget_calls.back().after);
    assert_kept_chain(f, c.all());
    std::vector<Gadget *> expected = { &f.system[0][0], &f.system[0][1], &f.system[0][2] };
    for (ULONG a : c.all()) expected.push_back(f.g(a));
    assert(f.list(0) == expected);
    f.closing(0);
    const auto batches = window_batches(f, c);
    const size_t before = free_gadget_calls.size();
    f.free(c.context);
    assert(!f.stopped() && natives.empty());
    assert_free_calls(before, batches);
}

static void test_kept_open_removed_batches()
{
    ClassicOpenFixture f;
    const WindowChain c = window_chain(f);
    assert(f.open(606, c.context) && !f.stopped());
    remove_batches_one_by_one(f, &f.window, f.mw, c);
    f.close();
    const unsigned calls = native_calls;
    const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
    assert(f.open(606, c.context) && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1);
    assert(gadget_calls.size() == records && free_gadget_calls.size() == frees &&
           last_data(opened_tags.data(), WA_Gadgets, 0) == reinterpret_cast<IPTR>(native_gadget(f, c.context)));
    assert(f.system[2].NextGadget == native_gadget(f, c.context));
    assert_kept_chain(f, c.all());
    f.close();
    assert_gadget_call("RemoveGList", &f.window, native_gadget(f, c.context), 6, 3);
    f.free(c.context);
    assert(!f.stopped() && natives.empty());
}

static void test_kept_add_attached_successor()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3 && f.remove_one(c.context) == 3);
    assert(!f.g(c.context)->NextGadget && f.lng(c.context) == c.button);
    const unsigned calls = native_calls;
    const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
    const auto memory = f.memory;
    const auto list = f.list(0);
    assert(f.add(c.context) == 20 && f.stopped() && native_calls == calls &&
           gadget_calls.size() == records && free_gadget_calls.size() == frees && f.memory == memory);
    assert(f.list(0) == list && !f.g(c.context)->NextGadget && f.lng(c.context) == c.button);
    m68k_write32(&f.ctx, c.context, 0);
    assert(f.add(c.context, 0xFFFF, 0xFFFFFFFF, 1) == 3 && !f.stopped());
    assert_gadget_call("AddGList", &f.windows[1], f.g(c.context), 1, 3);
    assert(!f.g(c.context)->NextGadget && !f.lng(c.context));
    f.closing(0); f.closing(1);
    f.free(c.context); f.free(c.button);
    assert(!f.stopped() && natives.empty());
}

static void test_kept_boundary_cuts()
{
    for (bool opening : { false, true })
        for (size_t cut_batch : { 0u, 1u, 2u })
        {
            ClassicOpenFixture f;
            const WindowChain c = window_chain(f);
            const auto batches = window_batches(f, c);
            const std::array<ULONG, 3> cut = { c.context, c.button, c.scroller };
            const std::array<ULONG, 3> suffix = { c.button, c.inc, c.tail };
            const std::array<size_t, 3> count = { 1, 2, 5 };
            assert(native_gadget(f, cut[cut_batch])->NextGadget == native_gadget(f, suffix[cut_batch]));
            m68k_write32(&f.ctx, cut[cut_batch], 0);
            if (!opening) assert(f.open(606, 0));
            const unsigned calls = native_calls;
            const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
            if (opening)
            {
                assert(f.open(606, c.context));
                assert(gadget_calls.size() == records &&
                       last_data(opened_tags.data(), WA_Gadgets, 0) == reinterpret_cast<IPTR>(native_gadget(f, c.context)));
            }
            else
            {
                assert(f.call_at(Fixture::intuition, 438, { f.mw, c.context, 0 }, { 0xFFFF, 0xFFFFFFFF }) == 3);
                assert(gadget_calls.size() == records + 1);
                assert_gadget_call("AddGList", &f.window, native_gadget(f, c.context), (WORD)count[cut_batch], 3);
                assert(gadget_calls.back().position == 0xFFFF && !gadget_calls.back().after);
            }
            assert(f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1 && free_gadget_calls.size() == frees);
            assert(f.system[2].NextGadget == native_gadget(f, c.context));
            const auto all = c.all();
            assert_kept_chain(f, { all.begin(), all.begin() + count[cut_batch] });
            assert_kept_chain(f, { all.begin() + count[cut_batch], all.end() });
            f.close();
            assert_gadget_call("RemoveGList", &f.window, native_gadget(f, c.context), (WORD)count[cut_batch], 3);
            const unsigned before = native_calls;
            f.free(c.context);
            assert(f.ctx.running && !f.ctx.exit_code && native_calls == before + cut_batch + 1);
            assert_free_calls(frees, { batches.begin(), batches.begin() + cut_batch + 1 });
            for (size_t n = count[cut_batch]; n < all.size(); ++n) assert(f.native(all[n]));
            const size_t later = free_gadget_calls.size();
            f.free(suffix[cut_batch]);
            assert(!f.stopped() && natives.empty());
            assert_free_calls(later, { batches.begin() + cut_batch + 1, batches.end() });
        }
}

static void test_kept_link_off_window_batches()
{
    for (bool opening : { false, true })
    {
        ClassicOpenFixture f;
        const ULONG a = f.context(), b = f.context();
        Gadget *ga = native_gadget(f, a), *gb = native_gadget(f, b);
        assert(!ga->NextGadget && !gb->NextGadget);
        m68k_write32(&f.ctx, a, b); // only the program's link joins these batches
        if (!opening) assert(f.open(606, 0));
        const unsigned calls = native_calls;
        const size_t records = gadget_calls.size(), frees = free_gadget_calls.size();
        if (opening)
        {
            assert(f.open(606, a) && gadget_calls.size() == records);
            assert(last_data(opened_tags.data(), WA_Gadgets, 0) == reinterpret_cast<IPTR>(ga));
        }
        else
        {
            assert(f.call_at(Fixture::intuition, 438, { f.mw, a, 0 }, { 0xFFFF, 0xFFFFFFFF }) == 3);
            assert(gadget_calls.size() == records + 1);
            assert_gadget_call("AddGList", &f.window, ga, 2, 3);
            assert(gadget_calls.back().last == gb && !gadget_calls.back().after);
        }
        assert(f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1 && free_gadget_calls.size() == frees);
        assert(f.system[2].NextGadget == ga);
        assert_kept_chain(f, { a, b });
        f.close();
        const unsigned before = native_calls;
        f.free(a);
        assert(!f.stopped() && native_calls == before + 2 && natives.empty());
        assert_free_calls(frees, { { ga }, { gb } });
    }
}

static void test_kept_free_removed_batches()
{
    WindowFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3);
    remove_batches_one_by_one(f, &f.windows[0], f.win0, c);
    f.only_system(0);
    const auto batches = window_batches(f, c);
    const unsigned calls = native_calls, before = freed;
    const size_t records = free_gadget_calls.size(), window_calls = gadget_calls.size();
    assert(f.call(36, { c.context }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 4);
    assert_free_calls(records, batches);
    assert(freed == before + 6 && natives.empty() && !f.ctx.gadgets && gadget_calls.size() == window_calls &&
           Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    for (ULONG a : c.all()) assert(!f.native(a));
}

static void test_kept_free_through_own_gadgets()
{
    Fixture f;
    const ULONG a = f.context();
    const WindowChain c = window_chain(f);
    own_gadget(f, OWN, OWN + 64); own_gadget(f, OWN + 64, c.context);
    m68k_write32(&f.ctx, a, OWN);
    assert(!native_gadget(f, a)->NextGadget && !Kaffeine_ShadowNative(&f.ctx, OWN, KAFFEINE_SHADOW_GADGET));
    const std::vector<UBYTE> own(f.memory.begin() + OWN, f.memory.begin() + OWN + 108);
    auto batches = window_batches(f, c);
    batches.push_back({ native_gadget(f, a) });
    const unsigned calls = native_calls, before = freed;
    const size_t records = free_gadget_calls.size();
    assert(f.call(36, { a }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 5);
    assert_free_calls(records, batches);
    assert(freed == before + 7 && natives.empty() && !f.ctx.gadgets &&
           std::equal(own.begin(), own.end(), f.memory.begin() + OWN) &&
           Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_kept_free_program_cut()
{
    Fixture f;
    const WindowChain c = window_chain(f);
    const auto batches = window_batches(f, c);
    m68k_write32(&f.ctx, c.button, 0); // native still leads to the scroller's first arrow
    assert(native_gadget(f, c.button)->NextGadget == native_gadget(f, c.inc));
    const unsigned calls = native_calls;
    const size_t records = free_gadget_calls.size();
    assert(f.call(36, { c.context }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 2);
    assert_free_calls(records, { batches[0], batches[1] });
    assert(!f.native(c.context) && !f.native(c.button) && natives.size() == 4 && f.ctx.gadgets);
    assert_kept_chain(f, { c.inc, c.dec, c.scroller, c.tail });
    const unsigned later_calls = native_calls;
    const size_t later = free_gadget_calls.size();
    assert(f.call(36, { c.inc }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == later_calls + 2);
    assert_free_calls(later, { batches[2], batches[3] });
    assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
}

static void test_kept_free_refusals_reset()
{
    enum Bad { MIDDLE, CYCLE, CYCLE_THROUGH_OWN };
    for (Bad bad : { MIDDLE, CYCLE, CYCLE_THROUGH_OWN })
    {
        Fixture f;
        const WindowChain c = window_chain(f);
        const auto batches = window_batches(f, c);
        if (bad == MIDDLE) m68k_write32(&f.ctx, c.button, c.dec);
        else if (bad == CYCLE) m68k_write32(&f.ctx, c.tail, c.context);
        else
        {
            own_gadget(f, OWN, c.context);
            m68k_write32(&f.ctx, c.tail, OWN);
        }
        const size_t records = free_gadget_calls.size();
        // The batch-cycle mutant must fail promptly even if its walk has no counter.
        {
            FreeWalkDeadline deadline;
            assert_showsel_free_refused(f, c.context); // snapshots native links, guest bytes, heap and shadows
        }
        assert(free_gadget_calls.size() == records);
        // Cut all batch boundaries, then free each in turn. A stale doomed
        // flag on any batch would refuse or free more than this one batch.
        for (ULONG a : { c.context, c.button, c.scroller, c.tail }) m68k_write32(&f.ctx, a, 0);
        const std::array<ULONG, 4> head = { c.context, c.button, c.inc, c.tail };
        for (size_t n = 0; n < head.size(); ++n)
        {
            const unsigned calls = native_calls, before = freed;
            const size_t frees = free_gadget_calls.size();
            assert(f.call(36, { head[n] }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1);
            assert_free_calls(frees, { batches[n] });
            assert(freed == before + batches[n].size());
            for (size_t k = n + 1; k < head.size(); ++k) assert(f.native(head[k]));
        }
        assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_kept_free_survivor_links()
{
    Fixture f;
    const WindowChain dead = window_chain(f), live = window_chain(f);
    const auto batches = window_batches(f, dead);
    auto g = [&](ULONG a) { return native_gadget(f, a); };
    g(live.context)->NextGadget = g(dead.inc); // native only
    m68k_write32(&f.ctx, live.button, dead.dec); // guest only, into the middle of a doomed batch
    g(live.inc)->NextGadget = g(dead.tail);
    m68k_write32(&f.ctx, live.inc, dead.context); // both, on a non-last gadget of a surviving batch
    g(live.scroller)->NextGadget = g(dead.button); // its guest link to live.tail stays
    g(live.tail)->NextGadget = g(live.context); // a native link to another survivor stays
    const unsigned calls = native_calls;
    const size_t records = free_gadget_calls.size();
    assert(f.call(36, { dead.context }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 4);
    assert_free_calls(records, batches);
    assert(!g(live.context)->NextGadget && f.lng(live.context) == live.button &&
           g(live.button)->NextGadget == g(live.inc) && !f.lng(live.button) &&
           !g(live.inc)->NextGadget && !f.lng(live.inc) &&
           g(live.dec)->NextGadget == g(live.scroller) && f.lng(live.dec) == live.scroller &&
           !g(live.scroller)->NextGadget && f.lng(live.scroller) == live.tail &&
           g(live.tail)->NextGadget == g(live.context) && !f.lng(live.tail));
    assert(natives.size() == 6);
    for (ULONG a : dead.all()) assert(!f.native(a));
    // Repair the deliberately inconsistent native/guest fixture before its valid final free.
    const auto all = live.all();
    for (size_t n = 0; n < all.size(); ++n)
    {
        const ULONG next = n + 1 < all.size() ? all[n + 1] : 0;
        g(all[n])->NextGadget = next ? g(next) : nullptr;
        m68k_write32(&f.ctx, all[n], next);
    }
    const auto surviving = window_batches(f, live);
    const size_t later = free_gadget_calls.size();
    f.free(live.context);
    assert(!f.stopped() && natives.empty());
    assert_free_calls(later, surviving);
}

static void test_kept_free_guest_bounds()
{
    for (bool through_own : { false, true })
        for (ULONG outside : { 0x10000u, 0xFFFDu, 0xFFFFFFFCu, 0xFFFFFFFEu })
        {
            Fixture f;
            const ULONG a = f.context(), stays = f.context();
            Gadget *ga = native_gadget(f, a), *gs = native_gadget(f, stays);
            own_gadget(f, OWN, outside);
            m68k_write32(&f.ctx, a, through_own ? OWN : outside);
            const unsigned calls = native_calls;
            const size_t records = free_gadget_calls.size();
            assert(!ga->NextGadget && !gs->NextGadget);
            assert(f.call(36, { a }) == 0 && f.ctx.running && !f.ctx.exit_code && native_calls == calls + 1);
            assert_free_calls(records, { { ga } });
            assert(!f.native(a) && f.native(stays) && f.lng(OWN) == outside && !f.lng(stays));
            const size_t later = free_gadget_calls.size();
            f.free(stays);
            assert(!f.stopped() && natives.empty());
            assert_free_calls(later, { { gs } });
        }
    for (bool ring : { false, true })
    {
        Fixture f;
        const ULONG a = f.context(), b = f.context();
        Gadget *ga = native_gadget(f, a), *gb = native_gadget(f, b);
        // Only NextGadget is read: exactly four bytes at the guest end
        // suffice. An own-gadget ring without a mirror stops at MAX_CHAIN.
        const ULONG own = ring ? OWN : f.ctx.mem_size - 4;
        m68k_write32(&f.ctx, a, own);
        m68k_write32(&f.ctx, own, ring ? own : b);
        const unsigned calls = native_calls;
        const size_t records = free_gadget_calls.size();
        {
            FreeWalkDeadline deadline;
            assert(f.call(36, { a }) == 0 && f.ctx.running && !f.ctx.exit_code);
        }
        assert(native_calls == calls + (ring ? 1 : 2) && f.lng(own) == (ring ? own : b));
        assert_free_calls(records, ring ? std::vector<std::vector<Gadget *>>{ { ga } } :
                                         std::vector<std::vector<Gadget *>>{ { ga }, { gb } });
        if (ring)
        {
            assert(f.native(b) && !gb->NextGadget && !f.lng(b));
            f.free(b);
            assert(!f.stopped());
        }
        assert(natives.empty() && !f.ctx.gadgets && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    }
}

static void test_classic_window_open()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        const unsigned allocations = native_allocations, calls = native_calls;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        const ULONG mw = f.open(lvo, a[0]);
        assert(mw && !f.stopped() && native_calls == calls + 1 && native_allocations == allocations + 6);
        Gadget *g = f.system[2].NextGadget;
        assert(g && g != reinterpret_cast<Gadget *>(f.memory.data() + a[0]));
        assert(last_data(opened_tags.data(), WA_Gadgets, 0) == reinterpret_cast<IPTR>(g));
        for (ULONG guest : a)
        {
            assert(g && Kaffeine_ClassicOn(&f.ctx, guest) &&
                   Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_CLASSIC) == g &&
                   Kaffeine_ClassicNative(&f.ctx, &f.window, guest) == g &&
                   f.ctx.window_gadgets->gadget(&f.ctx, g) == guest);
            assert(f.lng(guest) == (guest == a.back() ? 0 : guest + 64));
            g = g->NextGadget;
        }
        assert(!g);
        f.close();
        assert(!f.system[2].NextGadget && !f.ctx.classic && native_blocks.empty() &&
               Kaffeine_HeapAvail(&f.ctx, 0) == heap && closed_windows.back() == &f.window);
    }
}

static void test_classic_window_open_failure()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        const auto memory = f.memory;
        assert(!f.open(lvo, a[0], true) && !f.stopped());
        assert(!f.ctx.classic && native_blocks.empty() && f.ctx.num_shadows == shadows &&
               std::equal(memory.begin() + OWN, memory.begin() + OWN + 3 * 64, f.memory.begin() + OWN) &&
               Kaffeine_HeapAvail(&f.ctx, 0) == heap && !f.system[2].NextGadget);
        assert(f.open(lvo, a[0]) && !f.stopped()); // no pending marks survive the dropped hook
        f.close();
        assert(native_blocks.empty() && !f.ctx.classic);
    }
    // Failure to make the guest Window also detaches and frees the copies.
    ClassicOpenFixture f;
    own_chain(f);
    f.exhaust_heap();
    const UWORD shadows = f.ctx.num_shadows;
    assert(!f.open(606, OWN) && !f.stopped() && closed_windows.size() == 1 &&
           !f.ctx.classic && native_blocks.empty() && !f.system[2].NextGadget && f.ctx.num_shadows == shadows);
    f.release_heap();
}

static void test_classic_add_positions()
{
    for (ULONG pos : { 0u, 1u, 3u, 99u, 0xFFFFu, 0x12340001u })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        const UWORD expected = std::min<UWORD>((UWORD)pos, 3);
        const unsigned calls = native_calls;
        assert(f.add(a[0], pos) == expected && !f.stopped() && native_calls == calls + 1);
        std::vector<Gadget *> want = { &f.system[0][0], &f.system[0][1], &f.system[0][2] };
        std::vector<Gadget *> copies;
        for (ULONG at : a) copies.push_back(f.copy(at));
        want.insert(want.begin() + expected, copies.begin(), copies.end());
        assert(f.list(0) == want);
        assert_gadget_call("AddGList", &f.windows[0], copies[0], 3, expected);
        assert(gadget_calls.back().position == (UWORD)pos);
        for (unsigned i = 0; i < a.size(); ++i)
            assert(f.lng(a[i]) == (i + 1 == a.size() ? 0 : a[i + 1]));
        assert(f.remove(a[0], 3) == expected && !f.stopped());
        f.only_system(0);
        assert(!f.ctx.classic && native_blocks.empty());
    }
}

static void test_classic_add_counts()
{
    for (ULONG num : { 0u, 1u, 2u, 3u, 9u, 0x10002u, 0xFFFFu, 0xFFFEu })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        const WORD count = (WORD)num;
        const unsigned n = count < 0 ? 3 : std::min<unsigned>(count, 3);
        const unsigned calls = native_calls;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.add(a[0], 0xFFFF, num) == (n ? 3u : 0xFFFFu) && !f.stopped());
        assert(native_calls == calls + !!n && f.list(0).size() == n + 3 && native_blocks.size() == n * 2);
        for (unsigned i = 0; i < a.size(); ++i)
            assert(!!Kaffeine_ClassicOn(&f.ctx, a[i]) == (i < n));
        if (n)
        {
            assert_gadget_call("AddGList", &f.windows[0], f.copy(a[0]), (WORD)n, 3);
            assert(!f.copy(a[n - 1])->NextGadget && !f.lng(a[n - 1]));
        }
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == heap); // own gadgets need no guest mirror allocation
    }
    for (bool one : { false, true })
    {
        ClassicFixture f;
        own_gadget(f, OWN, 0xFFFFFFF0);
        assert((one ? f.add_one(OWN, 1) : f.add(OWN, 1, 1)) == 1 && !f.stopped());
        assert_gadget_call(one ? "AddGadget" : "AddGList", &f.windows[0], f.copy(OWN), 1, 1);
        assert(!f.lng(OWN) && f.copy(OWN)->NextGadget == &f.system[0][1] && native_blocks.size() == 2);
    }
}

static void test_classic_unsupported_runs()
{
    enum Kind { STRING, CUSTOM, REQUESTER, SCREEN, SYSTEM, SUBTYPE, MASKED, OBJECT };
    for (Kind kind : { STRING, CUSTOM, REQUESTER, SCREEN, SYSTEM, SUBTYPE, MASKED, OBJECT })
        for (bool on_tail : { false, true })
        {
            ClassicFixture f;
            const auto a = own_chain(f);
            const ULONG bad = on_tail ? a[2] : a[0];
            UWORD type = GTYP_BOOLGADGET;
            switch (kind)
            {
            case STRING:
                // a string gadget with a keymap of its own, in a StringInfo
                // at BORDER
                type = GTYP_STRGADGET;
                std::memset(f.memory.data() + BORDER, 0, 36);
                m68k_write32(&f.ctx, BORDER + 32, 0x4C00);
                m68k_write16(&f.ctx, bad + GD_ACTIVATION, GACT_ALTKEYMAP);
                m68k_write32(&f.ctx, bad + GD_SPECIALINFO, BORDER);
                break;
            case CUSTOM: type = GTYP_CUSTOMGADGET; break;
            case REQUESTER: type |= GTYP_REQGADGET; break;
            case SCREEN: type |= GTYP_SCRGADGET; break;
            case SYSTEM: type |= GTYP_SYSGADGET; break;
            case SUBTYPE: type |= 0x0010; break; // GTYP_SIZING without GTYP_SYSGADGET
            case MASKED: m68k_write16(&f.ctx, bad + GD_ACTIVATION, GACT_BOOLEXTEND); break;
            case OBJECT:
                m68k_write16(&f.ctx, bad + GD_FLAGS, GFLG_EXTENDED);
                m68k_write32(&f.ctx, bad + GD_MOREFLAGS, 0x400); // NDK bit, not AROS's 0x8
                break;
            }
            m68k_write16(&f.ctx, bad + GD_GADGETTYPE, type);
            const auto memory = f.memory;
            const unsigned calls = native_calls, allocations = native_allocations;
            const UWORD shadows = f.ctx.num_shadows;
            assert(f.add(a[0]) == 0xFFFF && !f.stopped());
            assert(native_calls == calls && native_allocations == allocations && native_blocks.empty() &&
                   !f.ctx.classic && f.ctx.num_shadows == shadows && f.memory == memory);
            f.only_system(0);
        }
    // Intuition ignores the requester of gadgets without GTYP_REQGADGET:
    // the run goes on the window, and Intuition gets no requester.
    ClassicFixture f;
    const auto a = own_chain(f);
    assert(f.add(OWN, 0xFFFF, 3, 0, 0xFFFFFFF0) == 3 && !f.stopped() && f.ctx.classic);
    assert_gadget_call("AddGList", &f.windows[0], f.copy(a[0]), 3, 3);
    assert(f.remove(OWN, 3) == 3 && !f.stopped());
}

/* ActivateGadget() passes Intuition the copy on its window, whose BOOL it
   returns, and GACT_ACTIVEGADGET reaches the program's gadget; another
   window, no window or a requester gives FALSE without Intuition. */
static void test_classic_activate()
{
    ClassicFixture f;
    own_gadget(f, OWN);
    assert(f.add_one(OWN) == 3 && !f.stopped());
    Gadget *g = f.copy(OWN);
    for (BOOL result : { FALSE, TRUE })
    {
        activate_result = result;
        const unsigned calls = native_calls;
        assert(f.call_at(Fixture::intuition, 462, { OWN, f.address(0), 0 }) == (ULONG)result &&
               !f.stopped());
        assert(native_calls == calls + 1 && passed.back() == std::string("ActivateGadget") &&
               passed_object == g && passed_window == &f.windows[0] && !passed_requester);
        assert(!!(f.word(OWN + GD_ACTIVATION) & GACT_ACTIVEGADGET) == !!result);
    }
    const unsigned calls = native_calls;
    assert(f.call_at(Fixture::intuition, 462, { OWN, f.address(1), 0 }) == 0);
    assert(f.call_at(Fixture::intuition, 462, { OWN, f.address(0), 0x4E80 }) == 0);
    assert(f.call_at(Fixture::intuition, 462, { OWN, 0, 0 }) == 0);
    assert(native_calls == calls && !f.stopped());
    g->Activation &= ~GACT_ACTIVEGADGET;
    intuition_active = nullptr;
    activate_result = FALSE;
    assert(f.remove_one(OWN) == 3 && !f.stopped());
    assert(!(f.word(OWN + GD_ACTIVATION) & GACT_ACTIVEGADGET));
}

/* A run of the program's own does not go between the gadgets of one
   GadTools call on the window, which stay together. */
static void test_classic_add_split()
{
    ClassicFixture f;
    const WindowChain c = window_chain(f);
    assert(f.add(c.context) == 3 && !f.stopped());
    own_gadget(f, OWN);
    // system gadgets 1 to 3, the context 4, the button 5, the scroller's arrow 6
    for (bool one : { false, true })
    {
        const unsigned calls = native_calls;
        const auto blocks = native_blocks;
        const UWORD shadows = f.ctx.num_shadows;
        assert((one ? f.add_one(OWN, 6) : f.add(OWN, 6, 1)) == 20 && f.stopped());
        assert(native_calls == calls && native_blocks == blocks && f.ctx.num_shadows == shadows &&
               !f.ctx.classic);
    }
    assert(f.add(OWN, 5, 1) == 5 && !f.stopped());
    assert(f.g(c.button)->NextGadget == f.copy(OWN) && f.lng(c.button + GD_NEXTGADGET) == OWN);
    assert(f.remove_one(OWN) == 5 && !f.stopped());
    f.closing(0);
    f.free(c.context); f.free(c.button); f.free(c.inc); f.free(c.tail);
    assert(!f.stopped());
}

static void test_classic_unsupported_window_open()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        own_gadget(f);
        m68k_write16(&f.ctx, OWN + GD_GADGETTYPE, GTYP_CUSTOMGADGET);
        assert(f.open(lvo, OWN) && !f.stopped() && !f.system[2].NextGadget &&
               !f.ctx.classic && native_blocks.empty());
        for (const TagItem &t : opened_tags) assert(t.ti_Tag != WA_Gadgets);
    }
}

static void test_classic_add_refusals()
{
    enum Bad { OUTSIDE, SHORT_GADGET, SHORT_EXT, BAD_XY, BAD_RENDER, MIXED, TWICE, TWICE_MASKED, CYCLE,
               ATTACHED };
    for (Bad bad : { OUTSIDE, SHORT_GADGET, SHORT_EXT, BAD_XY, BAD_RENDER, MIXED, TWICE, TWICE_MASKED,
                     CYCLE, ATTACHED })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        ULONG head = a[0], num = 0xFFFFFFFF;
        switch (bad)
        {
        case OUTSIDE: m68k_write32(&f.ctx, a[2], 0xFFFFFFF0); break;
        case SHORT_GADGET: head = 0xFFF0; break;
        case SHORT_EXT:
            head = 0xFFD0;
            own_gadget(f, head);
            m68k_write16(&f.ctx, head + GD_FLAGS, GFLG_EXTENDED);
            break;
        case BAD_XY:
            own_border(f, BORDER, 2, 0xFFFC);
            m68k_write32(&f.ctx, a[2] + GD_GADGETRENDER, BORDER);
            break;
        case BAD_RENDER: m68k_write32(&f.ctx, a[2] + GD_GADGETRENDER, 0xFFFFFFF0); break;
        case MIXED: m68k_write32(&f.ctx, a[2], f.context()); break;
        case TWICE: m68k_write32(&f.ctx, a[2], a[1]); num = 4; break;
        case TWICE_MASKED: // the twice comes first, before what the bridge leaves off
            m68k_write32(&f.ctx, a[2], a[1]);
            m68k_write16(&f.ctx, a[1] + GD_ACTIVATION, GACT_BOOLEXTEND);
            num = 4;
            break;
        case CYCLE: m68k_write32(&f.ctx, a[2], a[0]); break;
        case ATTACHED: assert(f.add_one(a[2], 0xFFFF, 1) == 3); break;
        }
        const unsigned calls = native_calls;
        const auto blocks = native_blocks;
        const auto memory = f.memory;
        const UWORD shadows = f.ctx.num_shadows;
        assert(f.add(head, 0xFFFF, num) == 20 && f.stopped());
        assert(native_calls == calls && native_blocks == blocks && f.memory == memory && f.ctx.num_shadows == shadows);
        f.only_system(0);
        own_gadget(f, 0x4700);
        assert(f.add_one(0x4700) == 3 && !f.stopped()); // failed copies left no marks
        if (bad == ATTACHED) assert(f.remove_one(a[2], 1) == 3);
        else f.only_system(1);
    }
}

static void test_classic_add_run_limit()
{
    Fixture f(0x40000);
    Window window{};
    const ULONG win = 0x3B00, first = 0x10000, size = 44, count = 4097;
    Kaffeine_GadgetsSetup(&f.ctx);
    assert(Kaffeine_ShadowAdd(&f.ctx, win, &window, KAFFEINE_SHADOW_WINDOW));
    // Distinct, contiguous NDK Gadgets, all in guest memory, ending at NULL.
    for (ULONG i = 0; i < count; ++i)
        own_gadget(f, first + i * size, i + 1 == count ? 0 : first + (i + 1) * size);
    const auto memory = f.memory;
    const UWORD shadows = f.ctx.num_shadows;
    const unsigned calls = native_calls, allocations = native_allocations;
    assert(f.call_at(Fixture::intuition, 438, { win, first, 0 }, { 0xFFFF, 0xFFFF }) == 20 && f.stopped());
    assert(native_calls == calls && native_allocations == allocations && native_blocks.empty() &&
           f.ctx.num_shadows == shadows && !f.ctx.classic && !window.FirstGadget && f.memory == memory);
}

static void test_classic_allocation_failures()
{
    for (unsigned fail = 0; fail < 6; ++fail)
    {
        ClassicFixture f;
        own_chain(f);
        const unsigned calls = native_calls;
        const UWORD shadows = f.ctx.num_shadows;
        const auto memory = f.memory;
        allocations_before_failure = fail;
        assert(f.add(OWN) == 0xFFFF && !f.stopped());
        assert(!f.ctx.classic && native_blocks.empty() && f.ctx.num_shadows == shadows &&
               native_calls == calls && f.memory == memory);
        f.only_system(0);
        assert(f.add(OWN) == 3 && !f.stopped());
    }
    for (ULONG lvo : { 606u, 204u })
        for (unsigned fail = 0; fail < 6; ++fail)
        {
            ClassicOpenFixture f;
            own_chain(f);
            const UWORD shadows = f.ctx.num_shadows;
            allocations_before_failure = fail;
            assert(f.open(lvo, OWN) && !f.stopped() && !f.ctx.classic && native_blocks.empty() &&
                   !f.system[2].NextGadget);
            f.close();
            assert(f.ctx.num_shadows == shadows && native_blocks.empty());
        }
    ClassicFixture f;
    own_chain(f);
    f.leave_shadows(1); // first copy can get a shadow; the second cannot
    const UWORD shadows = f.ctx.num_shadows;
    assert(f.add(OWN) == 0xFFFF && !f.stopped() && f.ctx.num_shadows == shadows &&
           native_blocks.empty() && !f.ctx.classic);
    f.only_system(0);
}

static void test_classic_remove_runs()
{
    for (unsigned start : { 0u, 1u })
        for (ULONG num : { 1u, 2u, 9u, 0xFFFFu })
        {
            ClassicFixture f;
            const auto a = own_chain(f);
            assert(f.add(a[0]) == 3);
            std::vector<Gadget *> copies;
            IntuiMessage messages[3]{};
            for (unsigned i = 0; i < a.size(); ++i)
            {
                copies.push_back(f.copy(a[i]));
                f.message(messages[i], copies.back());
            }
            const unsigned n = (WORD)num < 0 ? 3 - start : std::min<unsigned>(num, 3 - start);
            assert(f.remove(a[start], num) == 3 + start && !f.stopped());
            assert_gadget_call("RemoveGList", &f.windows[0], copies[start], n, 3 + start);
            const unsigned last = start + n - 1;
            const ULONG next = last + 1 < a.size() ? a[last + 1] : 0;
            assert(copies[last]->NextGadget == (next ? copies[last + 1] : nullptr) && f.lng(a[last]) == next);
            if (start)
            {
                assert(f.lng(a[start - 1]) == next &&
                       copies[start - 1]->NextGadget == (next ? copies[last + 1] : nullptr));
            }
            for (unsigned i = start; i <= last; ++i)
                assert(!Kaffeine_ClassicOn(&f.ctx, a[i]) &&
                       !Kaffeine_ShadowNative(&f.ctx, a[i], KAFFEINE_SHADOW_CLASSIC) &&
                       Kaffeine_ClassicGuest(&f.ctx, copies[i]) == a[i]);
            assert(native_blocks.size() == 6); // queued messages retain the removed copies
            while (GetMsg(&f.ports[0])) {}
            own_gadget(f, 0x4700);
            assert(f.add_one(0x4700) == 6 - n && !f.stopped());
            assert(native_blocks.size() == (4 - n) * 2);
            for (unsigned i = start; i <= last; ++i)
                assert(!Kaffeine_ClassicGuest(&f.ctx, copies[i]));
        }
}

static void test_classic_remove_one_and_windows()
{
    ClassicFixture f;
    const auto a = own_chain(f);
    assert(f.add(a[0]) == 3);
    Gadget *first = f.copy(a[0]), *middle = f.copy(a[1]), *last = f.copy(a[2]);
    IntuiMessage im{};
    f.message(im, middle);
    assert(f.remove_one(a[1]) == 4 && !f.stopped());
    assert_gadget_call("RemoveGList", &f.windows[0], middle, 1, 4);
    assert(first->NextGadget == last && f.lng(a[0]) == a[2] &&
           middle->NextGadget == last && f.lng(a[1]) == a[2]);
    const unsigned calls = native_calls;
    const auto memory = f.memory;
    assert(f.remove_one(a[0], 1) == 0xFFFF && !f.stopped());
    assert(f.remove_one(a[1]) == 0xFFFF && !f.stopped());
    assert(f.remove_one(0x4700) == 0xFFFF && !f.stopped());
    assert(f.remove(a[0], 0) == 0xFFFF && !f.stopped()); // native zero-count convention
    assert(native_calls == calls && f.memory == memory && first->NextGadget == last);
    assert(f.remove_one(a[2]) == 4 && !f.stopped() && !first->NextGadget && !f.lng(a[0]));
    assert(f.remove_one(a[0]) == 3 && !f.stopped());
    f.only_system(0);
}

static void test_classic_removed_last_links()
{
    enum Following { OWN_GADGET, GADTOOLS, SYSTEM, END };
    for (bool one : { false, true })
        for (Following following : { OWN_GADGET, GADTOOLS, SYSTEM, END })
        {
            ClassicFixture f;
            const unsigned count = one ? 1 : 2;
            const ULONG gt = following == GADTOOLS ? f.context() : 0;
            if (gt) assert(f.add(gt, 0xFFFF, 1) == 3);
            const auto a = own_chain(f, count + (following == OWN_GADGET));
            const UWORD position = following == SYSTEM ? 1 : 3;
            assert(f.add(a[0], position) == position && !f.stopped());
            std::vector<Gadget *> copies;
            std::array<IntuiMessage, 2> messages{};
            for (unsigned i = 0; i < count; ++i)
            {
                copies.push_back(f.copy(a[i]));
                f.message(messages[i], copies.back()); // keep the removed copies inspectable
            }
            Gadget *after = following == OWN_GADGET ? f.copy(a.back()) : following == GADTOOLS ? f.g(gt) :
                            following == SYSTEM ? &f.system[0][1] : nullptr;
            const ULONG guest_after = following == OWN_GADGET ? a.back() : gt;
            assert(copies.back()->NextGadget == after && f.lng(a[count - 1]) == guest_after);
            const auto before = f.list(0);
            const auto blocks = native_blocks;
            assert((one ? f.remove_one(a[0]) : f.remove(a[0], count)) == position && !f.stopped());
            assert_gadget_call("RemoveGList", &f.windows[0], copies[0], count, position);
            assert(gadget_calls.back().after == after && native_blocks == blocks);
            // AmigaOS 3.2.3 and AROS retain the removed run's last links.
            assert(copies.back()->NextGadget == after && f.lng(a[count - 1]) == guest_after);
            for (unsigned i = 0; i < count; ++i)
            {
                assert(!Kaffeine_ClassicOn(&f.ctx, a[i]) &&
                       !Kaffeine_ShadowNative(&f.ctx, a[i], KAFFEINE_SHADOW_CLASSIC) &&
                       Kaffeine_ClassicGuest(&f.ctx, copies[i]) == a[i]);
                if (i + 1 < count)
                    assert(copies[i]->NextGadget == copies[i + 1] && f.lng(a[i]) == a[i + 1]);
            }
            auto expected = before;
            expected.erase(expected.begin() + position, expected.begin() + position + count);
            assert(f.list(0) == expected && before[position - 1]->NextGadget == after);
            while (GetMsg(&f.ports[0])) {}
            f.closing(0);
            if (gt) f.free(gt);
            assert(!f.ctx.classic && native_blocks.empty());
            f.only_system(0);
        }
}

static void test_classic_mixed_window_links()
{
    ClassicFixture f;
    const ULONG gt = f.context();
    const auto a = own_chain(f);
    assert(f.add(a[0], 0) == 0);
    assert(f.add(gt, 1, 1) == 1 && !f.stopped());
    Gadget *first = f.copy(a[0]), *middle = f.copy(a[1]);
    assert(first->NextGadget == f.g(gt) && f.lng(a[0]) == gt &&
           f.g(gt)->NextGadget == middle && f.lng(gt) == a[1] && !f.lng(a[2]));
    assert(f.remove(a[1], 2) == 2 && !f.stopped());
    assert(f.g(gt)->NextGadget == &f.system[0][0] && !f.lng(gt) &&
           first->NextGadget == f.g(gt) && f.lng(a[0]) == gt);
    // Insert another own run after a GadTools mirror and remove its prefix.
    own_gadget(f, a[1], a[2]); own_gadget(f, a[2]);
    assert(f.add(a[1], 2) == 2 && f.lng(gt) == a[1]);
    assert(f.remove_one(a[1]) == 2 && f.lng(gt) == a[2] && f.g(gt)->NextGadget == f.copy(a[2]));
    assert(f.remove_one(a[2]) == 2 && !f.lng(gt));
    assert(f.remove_one(gt) == 1 && !f.stopped() && !f.lng(a[0]) && first->NextGadget == &f.system[0][0]);
    f.free(gt);
    assert(!f.stopped() && f.remove_one(a[0]) == 0);
    f.only_system(0);
    assert(native_blocks.empty());
}

static void test_classic_remove_foreign_run()
{
    for (bool gadtools : { false, true })
    {
        ClassicFixture f;
        const ULONG gt = gadtools ? f.context() : 0;
        const auto a = own_chain(f);
        assert(f.add(a[0], 0) == 0);
        if (gadtools) assert(f.add(gt, 1, 1) == 1);
        const auto list = f.list(0);
        const auto blocks = native_blocks;
        const auto memory = f.memory;
        const unsigned calls = native_calls;
        assert(f.remove(a[0]) == 20 && f.stopped() && native_calls == calls &&
               f.list(0) == list && f.memory == memory && native_blocks == blocks);
        // No stale removal marks may retire the foreign-side own gadgets.
        assert(f.remove_one(a[0]) == 0 && !f.stopped());
        assert(Kaffeine_ClassicOn(&f.ctx, a[1]) && Kaffeine_ClassicOn(&f.ctx, a[2]));
        if (gadtools) assert(f.remove_one(gt) == 0);
        assert(f.remove(a[1], 2) == 0 && !f.stopped());
        f.only_system(0);
    }
}

static void test_classic_refresh_flags()
{
    ClassicFixture f;
    const auto a = own_chain(f);
    assert(f.add(a[0]) == 3);
    Gadget *first = f.copy(a[0]), *second = f.copy(a[1]), *third = f.copy(a[2]);
    const UWORD base = GFLG_GADGHNONE;
    // The program selects one bit while Intuition disables a different bit.
    m68k_write16(&f.ctx, a[0] + GD_FLAGS, base | GFLG_SELECTED | GFLG_LABELSTRING);
    first->Flags |= GFLG_DISABLED;
    m68k_write16(&f.ctx, a[1] + GD_FLAGS, base | GFLG_SELECTED);
    m68k_write16(&f.ctx, a[2] + GD_FLAGS, base | GFLG_DISABLED);
    assert(f.call_at(Fixture::intuition, 432, { a[0], f.win0, 0 }, { 2 }) == 0 && !f.stopped());
    assert_gadget_call("RefreshGList", &f.windows[0], first, 2, 0);
    assert(refresh_saw.size() == 2 && refresh_saw[0].flags == (base | GFLG_SELECTED | GFLG_DISABLED) &&
           refresh_saw[1].flags == (base | GFLG_SELECTED));
    assert(first->Flags == (base | GFLG_SELECTED | GFLG_DISABLED) && second->Flags == (base | GFLG_SELECTED) &&
           third->Flags == base && f.word(a[0] + GD_FLAGS) == (base | GFLG_SELECTED | GFLG_DISABLED | GFLG_LABELSTRING));
    // RefreshGadgets imports the full remaining list, with requesters left out.
    assert(f.call_at(Fixture::intuition, 222, { a[1], f.win0, 0xFFFFFFF0 }) == 0 && !f.stopped());
    assert_gadget_call("RefreshGList", &f.windows[0], second, -1, 0);
    assert(third->Flags == (base | GFLG_DISABLED));
    // Both sides clear different shared bits relative to the new snapshot.
    m68k_write16(&f.ctx, a[0] + GD_FLAGS, base | GFLG_DISABLED | GFLG_LABELSTRING);
    first->Flags &= ~GFLG_DISABLED;
    assert(f.call_at(Fixture::intuition, 432, { a[0], f.win0, 0 }, { 1 }) == 0);
    assert(first->Flags == base && f.word(a[0] + GD_FLAGS) == (base | GFLG_LABELSTRING));
    const unsigned calls = native_calls;
    assert(f.call_at(Fixture::intuition, 432, { a[0], f.win1, 0 }, { 1 }) == 0);
    assert(f.call_at(Fixture::intuition, 432, { a[0], f.win0, 0 }, { 0 }) == 0 && native_calls == calls);
}

static void test_classic_imported_flags_snapshot()
{
    for (UWORD bit : { (UWORD)GFLG_SELECTED, (UWORD)GFLG_DISABLED })
        for (bool set : { false, true })
            for (bool during_refresh : { false, true })
            {
                ClassicFixture f;
                const UWORD before = GFLG_GADGHNONE | (set ? 0 : bit);
                const UWORD imported = before ^ bit;
                own_gadget(f, OWN, 0, before);
                assert(f.add_one(OWN) == 3);
                Gadget *g = f.copy(OWN);
                m68k_write16(&f.ctx, OWN + GD_FLAGS, imported);
                // Rendering may reverse the imported bit before the bridge's
                // automatic export. The snapshot must already include that import.
                refresh_toggle_flags = during_refresh ? bit : 0;
                assert(f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 }) == 0 && !f.stopped());
                assert(refresh_saw.size() == 1 && refresh_saw[0].flags == imported);
                refresh_toggle_flags = 0;
                if (during_refresh)
                    assert(g->Flags == before && f.word(OWN + GD_FLAGS) == before);
                else
                {
                    assert(g->Flags == imported && f.word(OWN + GD_FLAGS) == imported);
                    g->Flags ^= bit; // Intuition changes it after RefreshGList returns
                }
                IntuiMessage im{};
                f.message(im, g);
                const ULONG msg = f.call_at(Fixture::execbase, 372, { f.port() });
                assert(msg && f.lng(msg + 28) == OWN && f.word(OWN + GD_FLAGS) == before);
                assert(f.call_at(Fixture::execbase, 378, { 0, msg }) == 0 && !f.stopped());
            }
}

static void test_classic_message_flags()
{
    ClassicFixture f;
    own_gadget(f);
    const UWORD base = GFLG_GADGHNONE, activation = GACT_RELVERIFY;
    m68k_write16(&f.ctx, OWN + GD_ACTIVATION, activation | GACT_ACTIVEGADGET);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    assert(g->Activation == activation); // a stale guest active bit must not activate Intuition's copy
    for (bool reverse : { false, true })
    {
        const UWORD in = reverse ? GFLG_DISABLED : GFLG_SELECTED;
        const UWORD out = reverse ? GFLG_SELECTED : GFLG_DISABLED;
        m68k_write16(&f.ctx, OWN + GD_ACTIVATION, activation);
        // Agree on zero before checking both directions of a by-bit merge.
        m68k_write16(&f.ctx, OWN + GD_FLAGS, base);
        assert(f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 }) == 0);
        m68k_write16(&f.ctx, OWN + GD_FLAGS, base | in | GFLG_LABELIMAGE);
        g->Flags |= out;
        g->Activation |= GACT_ACTIVEGADGET;
        IntuiMessage im{};
        f.message(im, g);
        const ULONG msg = f.call_at(Fixture::execbase, 372, { f.port() });
        assert(msg && f.lng(msg + 28) == OWN &&
               f.word(OWN + GD_FLAGS) == (base | in | out | GFLG_LABELIMAGE) &&
               f.word(OWN + GD_ACTIVATION) == (activation | GACT_ACTIVEGADGET));
        assert(!(g->Flags & in)); // delivery exports; it does not import the pending guest bit
        assert(f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 }) == 0 && (g->Flags & in));
        // Program writes to ACTIVEGADGET and other Activation bits never enter the copy.
        m68k_write16(&f.ctx, OWN + GD_ACTIVATION, GACT_IMMEDIATE);
        assert(f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 }) == 0 &&
               g->Activation == (activation | GACT_ACTIVEGADGET));
        g->Activation &= ~GACT_ACTIVEGADGET;
        f.sync(0);
        assert(f.word(OWN + GD_ACTIVATION) == GACT_IMMEDIATE);
        f.call_at(Fixture::execbase, 378, { 0, msg });
        assert(replied_messages.back() == &im.ExecMessage);
    }
}

static void test_classic_enable_flags()
{
    for (ULONG window : { ClassicFixture::win0, ClassicFixture::win1, 0u })
    {
        ClassicFixture f;
        own_gadget(f);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        for (ULONG lvo : { 174u, 186u })
        {
            const bool off = lvo == 174;
            const UWORD base = GFLG_GADGHNONE, shared = GFLG_SELECTED | GFLG_DISABLED;
            // Set a shared snapshot, then change independent bits on the two sides.
            m68k_write16(&f.ctx, OWN + GD_FLAGS, base | (off ? 0 : shared));
            f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 });
            m68k_write16(&f.ctx, OWN + GD_FLAGS, base | (off ? GFLG_SELECTED : GFLG_DISABLED));
            g->Flags ^= GFLG_DISABLED;
            const unsigned calls = native_calls;
            const size_t changes = changed.size();
            assert(f.call_at(Fixture::intuition, lvo, { OWN, window, 0xFFFFFFF0 }) == 0 && !f.stopped());
            const UWORD expected = base | (off ? shared : 0);
            assert(g->Flags == expected && f.word(OWN + GD_FLAGS) == expected);
            if (window == f.win0)
            {
                assert(native_calls == calls + 1 && changed.size() == changes + 1 &&
                       changed_object == g && changed_window == &f.windows[0] && !changed_requester);
                assert(enable_saw.gadget == g && enable_saw.flags == (base | (off ? shared : 0)));
            }
            else assert(native_calls == calls && changed.size() == changes);
        }
    }
}

static void test_classic_unattached_and_shadow_enable()
{
    ClassicFixture f;
    own_gadget(f, OWN, 0, GFLG_GADGHBOX | GFLG_SELECTED);
    const unsigned calls = native_calls;
    const UWORD shadows = f.ctx.num_shadows;
    for (ULONG window : { f.win0, f.win1, 0u })
        for (ULONG lvo : { 174u, 186u })
        {
            auto memory = f.memory;
            const UWORD flags = GFLG_GADGHBOX | GFLG_SELECTED | (lvo == 174 ? GFLG_DISABLED : 0);
            memory[OWN + GD_FLAGS] = (UBYTE)(flags >> 8);
            memory[OWN + GD_FLAGS + 1] = (UBYTE)flags;
            assert(f.call_at(Fixture::intuition, lvo, { OWN, window, 0 }) == 0 && !f.stopped() &&
                   f.memory == memory && native_calls == calls && native_blocks.empty() &&
                   !f.ctx.classic && f.ctx.num_shadows == shadows);
        }
    Gadget native{};
    Window window{};
    Requester requester{};
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x4E00, &native, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x4E40, &window, FILLER));
    assert(Kaffeine_ShadowAdd(&f.ctx, 0x4E80, &requester, FILLER));
    for (ULONG lvo : { 174u, 186u })
    {
        const unsigned before = native_calls;
        const auto memory = f.memory;
        assert(f.call_at(Fixture::intuition, lvo, { 0x4E00, 0x4E40, 0x4E80 }) == 0 && !f.stopped());
        assert(native_calls == before + 1 && changed_object == &native && changed_window == &window &&
               changed_requester == &requester && f.memory == memory && !f.ctx.classic);
    }
}

static void test_classic_message_addresses()
{
    for (bool gt : { false, true })
        for (ULONG cls : { (ULONG)IDCMP_GADGETUP, (ULONG)IDCMP_GADGETDOWN })
        {
            ClassicFixture f;
            own_gadget(f);
            assert(f.add_one(OWN) == 3);
            Gadget *g = f.copy(OWN);
            IntuiMessage im{}, retired{};
            f.message(im, g, cls);
            const ULONG msg = gt ? f.call(72, { f.port() }) : f.call_at(Fixture::execbase, 372, { f.port() });
            assert(msg && !f.stopped() && f.lng(msg + 20) == cls && f.lng(msg + 28) == OWN &&
                   f.lng(msg + 44) == f.win0);
            f.message(retired, g, cls);
            assert(f.remove_one(OWN) == 3 && !f.stopped() && !Kaffeine_ClassicOn(&f.ctx, OWN));
            assert(!Kaffeine_ShadowNative(&f.ctx, OWN, KAFFEINE_SHADOW_CLASSIC) &&
                   Kaffeine_ClassicGuest(&f.ctx, g) == OWN && f.ctx.window_gadgets->gadget(&f.ctx, g) == OWN);
            g->Flags |= GFLG_SELECTED;
            const ULONG second = gt ? f.call(72, { f.port() }) : f.call_at(Fixture::execbase, 372, { f.port() });
            assert(second && second != msg && f.lng(second + 28) == OWN && f.lng(second + 20) == cls &&
                   (f.word(OWN + GD_FLAGS) & GFLG_SELECTED));
            for (ULONG held : { msg, second })
            {
                if (gt) f.call(78, { 0, held });
                else f.call_at(Fixture::execbase, 378, { 0, held });
            }
            assert(replied_messages.size() == 2 && replied_messages[0] == &im.ExecMessage &&
                   replied_messages[1] == &retired.ExecMessage && native_blocks.size() == 2);
            own_gadget(f, 0x4700);
            assert(f.add_one(0x4700) == 3 && !f.stopped() && !Kaffeine_ClassicGuest(&f.ctx, g) && !block_holding(g));
            assert(native_blocks.size() == 2);
        }
}

static void test_classic_queued_retirement()
{
    ClassicFixture f;
    own_gadget(f);
    assert(f.add_one(OWN) == 3);
    Gadget *old = f.copy(OWN);
    IntuiMessage im{};
    f.message(im, old);
    assert(f.remove_one(OWN) == 3 && native_blocks.size() == 2 && !Kaffeine_ClassicOn(&f.ctx, OWN));
    // The same guest gadget gets a distinct copy while its old message waits.
    assert(f.add_one(OWN) == 3 && !f.stopped());
    Gadget *fresh = f.copy(OWN);
    assert(fresh != old && native_blocks.size() == 4 && Kaffeine_ClassicGuest(&f.ctx, old) == OWN);
    const ULONG msg = f.call_at(Fixture::execbase, 372, { f.port() });
    assert(msg && f.lng(msg + 28) == OWN);
    f.call_at(Fixture::execbase, 378, { 0, msg });
    assert(f.remove_one(OWN) == 3 && !f.stopped()); // a remove sweeps both unreferenced copies
    assert(!f.ctx.classic && native_blocks.empty() && !Kaffeine_ClassicGuest(&f.ctx, old) &&
           !Kaffeine_ClassicGuest(&f.ctx, fresh));
    f.only_system(0);
}

static void test_classic_held_retirement()
{
    for (bool gt : { false, true })
    {
        ClassicFixture f;
        const auto a = own_chain(f, 2);
        assert(f.add(a[0]) == 3);
        Gadget *old = f.copy(a[0]);
        IntuiMessage im{};
        f.message(im, old, IDCMP_GADGETDOWN);
        const ULONG held = gt ? f.call(72, { f.port() }) : f.call_at(Fixture::execbase, 372, { f.port() });
        assert(held && !f.ports[0].mp_MsgList.lh_Head->ln_Succ);
        assert(f.remove_one(a[0]) == 3 && native_blocks.size() == 4 && Kaffeine_ClassicGuest(&f.ctx, old) == a[0]);
        own_gadget(f, 0x4700);
        assert(f.add_one(0x4700) == 4 && native_blocks.size() == 6 && block_holding(old));
        if (gt) f.call(78, { 0, held });
        else f.call_at(Fixture::execbase, 378, { 0, held });
        assert(block_holding(old) && native_blocks.size() == 6); // release is at the next successful add/remove
        assert(f.remove_one(0x4700) == 4 && !f.stopped() && native_blocks.size() == 2 &&
               !block_holding(old) && !Kaffeine_ClassicGuest(&f.ctx, old));
        assert(f.copy(a[1]));
    }
}

static void test_classic_close_lifetime()
{
    ClassicOpenFixture f;
    const auto a = own_chain(f);
    const UWORD shadows = f.ctx.num_shadows;
    const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
    assert(f.open(606, a[0]));
    auto *first = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, a[0], KAFFEINE_SHADOW_CLASSIC));
    Gadget *last = first->NextGadget->NextGadget;
    IntuiMessage queued{}, held{};
    queued.IDCMPWindow = held.IDCMPWindow = &f.window;
    queued.IAddress = last; queued.Class = IDCMP_GADGETUP;
    held.IAddress = first; held.Class = IDCMP_GADGETDOWN;
    queue_message(f.port, held);
    const ULONG port = f.lng(f.mw + M68K_WIN_USERPORT);
    const ULONG msg = f.call_at(Fixture::execbase, 372, { port });
    assert(msg && f.lng(msg + 28) == a[0]);
    queue_message(f.port, queued);
    assert(f.call_at(Fixture::intuition, 228, { f.mw, a[2] }) == 5 && native_blocks.size() == 6);
    assert(f.call_at(Fixture::intuition, 228, { f.mw, a[0] }) == 3 && native_blocks.size() == 6);
    // Both retired copies and the still attached middle gadget belong to this window.
    f.close();
    assert(!f.ctx.classic && native_blocks.empty() && f.ctx.num_shadows == shadows &&
           Kaffeine_HeapAvail(&f.ctx, 0) == heap && !f.system[2].NextGadget &&
           !Kaffeine_ClassicGuest(&f.ctx, first) && !Kaffeine_ClassicGuest(&f.ctx, last));
    assert(replied_messages.size() == 2 && replied_messages[0] == &held.ExecMessage &&
           replied_messages[1] == &queued.ExecMessage);
}

static void test_classic_cleanup_lifetime()
{
    ClassicFixture f;
    const ULONG gt = f.context();
    const auto a = own_chain(f);
    assert(f.add_one(a[0], 0) == 0 && f.add(gt, 1, 1) == 1 && f.add_one(a[1], 2) == 2 &&
           f.add_one(a[2], 0xFFFF, 1) == 3);
    IntuiMessage retired{};
    f.message(retired, f.copy(a[1]));
    assert(f.remove_one(a[1]) == 2 && Kaffeine_ClassicGuest(&f.ctx, static_cast<Gadget *>(retired.IAddress)) == a[1]);
    // End-of-program cleanup also owns copies prepared for an open never attempted.
    own_gadget(f, 0x4700);
    Gadget *pending = f.prepare(0x4700);
    assert(pending && !Kaffeine_ClassicOn(&f.ctx, 0x4700));
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(!f.ctx.classic && !f.ctx.gadgets && native_blocks.empty() && natives.empty() &&
           !Kaffeine_ClassicGuest(&f.ctx, pending));
    for (ULONG at : { a[0], a[1], a[2], 0x4700u })
        assert(!Kaffeine_ShadowNative(&f.ctx, at, KAFFEINE_SHADOW_CLASSIC));
    f.only_system(0); f.only_system(1);
    assert(closed_windows.empty()); // cleanup takes copies off windows left open
    empty_port(f.ports[0]);
}

static void test_classic_snapshot_on_reattach()
{
    ClassicFixture f;
    own_gadget(f, OWN, 0, GFLG_LABELSTRING);
    const ULONG text = f.s("Old label");
    m68k_write32(&f.ctx, OWN + GD_GADGETTEXT, text);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    assert(!std::strcmp(reinterpret_cast<char *>(g->GadgetText), "Old label"));
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + text), "New label");
    m68k_write16(&f.ctx, OWN + GD_LEFTEDGE, (UWORD)-45);
    assert(f.call_at(Fixture::intuition, 432, { OWN, f.win0, 0 }, { 1 }) == 0);
    assert(g->LeftEdge == 0 && !std::strcmp(reinterpret_cast<char *>(g->GadgetText), "Old label"));
    assert(f.remove_one(OWN) == 3 && native_blocks.empty());
    assert(f.add_one(OWN) == 3);
    g = f.copy(OWN);
    assert(g->LeftEdge == -45 && !std::strcmp(reinterpret_cast<char *>(g->GadgetText), "New label"));
}

/* StringInfo and StringExtend are 36-byte NDK records, constructed as
   guest bytes. Each slot keeps its three buffers separate from them. */
static constexpr ULONG STRINFO = 0x4E00, STREXT = 0x4F00, STRBUFFER = 0x5000;
static constexpr WORD STRMAX = 32;
static constexpr StringState STRING_INITIAL = { 1, 2, 3, 4, 5, -6 };
struct OwnString { ULONG info, extension, buffer, undo, work; };
static void guest_string_state(Fixture &f, ULONG info, const StringState &s)
{
    m68k_write16(&f.ctx, info + SI_BUFFERPOS, (UWORD)s.pos);
    m68k_write16(&f.ctx, info + SI_DISPPOS, (UWORD)s.disp);
    m68k_write16(&f.ctx, info + SI_UNDOPOS, (UWORD)s.undo);
    m68k_write16(&f.ctx, info + SI_NUMCHARS, (UWORD)s.chars);
    m68k_write16(&f.ctx, info + SI_DISPCOUNT, (UWORD)s.count);
    m68k_write32(&f.ctx, info + SI_LONGINT, (ULONG)s.number);
}
static StringState guest_string_state(Fixture &f, ULONG info)
{
    return { (WORD)f.word(info + SI_BUFFERPOS), (WORD)f.word(info + SI_DISPPOS),
             (WORD)f.word(info + SI_UNDOPOS), (WORD)f.word(info + SI_NUMCHARS),
             (WORD)f.word(info + SI_DISPCOUNT), (LONG)f.lng(info + SI_LONGINT) };
}
/* Convert an own_gadget or an element of own_chain, preserving its links. */
static OwnString own_string(Fixture &f, ULONG gadget = OWN, unsigned slot = 0, bool integer = false)
{
    assert(slot < 4);
    OwnString s{ STRINFO + slot * 64, STREXT + slot * 64, STRBUFFER + slot * 256,
                 STRBUFFER + slot * 256 + 64, STRBUFFER + slot * 256 + 128 };
    std::memset(f.memory.data() + s.info, 0, STRINGINFO_SIZE);
    std::memset(f.memory.data() + s.extension, 0, STRINGEXTEND_SIZE);
    std::memset(f.memory.data() + s.buffer, 'b', STRMAX);
    std::memcpy(f.memory.data() + s.buffer, integer ? "-123" : "text", 5);
    std::memset(f.memory.data() + s.undo, 'u', STRMAX);
    std::memset(f.memory.data() + s.work, 'w', STRMAX);
    m68k_write16(&f.ctx, gadget + GD_GADGETTYPE, GTYP_STRGADGET);
    m68k_write16(&f.ctx, gadget + GD_ACTIVATION, GACT_RELVERIFY | (integer ? GACT_LONGINT : 0));
    m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, s.info);
    m68k_write32(&f.ctx, s.info + SI_BUFFER, s.buffer);
    m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, s.undo);
    m68k_write16(&f.ctx, s.info + SI_MAXCHARS, STRMAX);
    m68k_write16(&f.ctx, s.info + SI_CLEFT, 7);
    m68k_write16(&f.ctx, s.info + SI_CTOP, 8);
    m68k_write32(&f.ctx, s.info + SI_EXTENSION, s.extension);
    guest_string_state(f, s.info, STRING_INITIAL);
    f.memory[s.extension + SE_PENS] = 0x91;
    f.memory[s.extension + SE_PENS + 1] = 0xA2;
    f.memory[s.extension + SE_ACTIVEPENS] = 0xB3;
    f.memory[s.extension + SE_ACTIVEPENS + 1] = 0xC4;
    m68k_write32(&f.ctx, s.extension + SE_INITIALMODES, 0x89ABCDEF);
    m68k_write32(&f.ctx, s.extension + SE_WORKBUFFER, s.work);
    for (unsigned i = 0; i < 4; ++i)
        m68k_write32(&f.ctx, s.extension + SE_RESERVED + i * 4, 0xDEAD0000 + i);
    return s;
}
static ULONG tail_string_buffer(Fixture &f, const OwnString &s, ULONG left, bool nul = true)
{
    assert(left && left <= f.ctx.mem_size - f.ctx.heap_end);
    const ULONG buffer = f.ctx.mem_size - left;
    std::memset(f.ctx.mem + buffer, 'x', left);
    if (nul) f.memory[f.ctx.mem_size - 1] = 0;
    m68k_write32(&f.ctx, s.info + SI_BUFFER, buffer);
    return buffer;
}
static void count_native_string_texts(Fixture &f)
{
    watch_strings = count_string_texts = true;
    string_allocation_end = f.memory.data() + f.memory.size();
}
static StringInfo *string_info(ClassicFixture &f, ULONG guest = OWN)
{
    auto *si = static_cast<StringInfo *>(f.copy(guest)->SpecialInfo);
    assert(si && block_holding(si) == block_holding(f.copy(guest)));
    return si;
}
static ULONG string_call(ClassicFixture &f, ULONG lvo, ULONG guest = OWN, ULONG window = ClassicFixture::win0)
{
    return f.call_at(Fixture::intuition, lvo, { guest, window, 0 }, { 1 });
}

static void test_classic_stringinfo_offsets()
{
    for (bool integer : { false, true })
    {
        Fixture f;
        own_gadget(f);
        const OwnString s = own_string(f, OWN, 0, integer);
        const StringState values{ -32768, 32767, -23456, -12345, -1, (LONG)0x80000000 };
        guest_string_state(f, s.info, values);
        m68k_write16(&f.ctx, s.info + SI_CLEFT, (UWORD)-3210);
        m68k_write16(&f.ctx, s.info + SI_CTOP, (UWORD)-2222);
        // Neither Extension nor AltKeyMap is followed without its bit.
        m68k_write32(&f.ctx, s.info + SI_EXTENSION, 0xFFFFFFFC);
        m68k_write32(&f.ctx, s.info + SI_ALTKEYMAP, 0xFFFFFFF8);
        const auto memory = f.memory;
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
        assert(si && block_holding(si) == c.block && !f.guest(si));
        assert(c.head->GadgetType == GTYP_STRGADGET &&
               c.head->Activation == (GACT_RELVERIFY | (integer ? GACT_LONGINT : 0)));
        assert(string_state(*si) == values && si->MaxChars == STRMAX &&
               si->CLeft == -3210 && si->CTop == -2222 && !si->Extension && !si->AltKeyMap);
        assert(si->Buffer == f.ctx.mem + s.buffer && si->UndoBuffer == f.ctx.mem + s.undo);
        assert(f.memory == memory && c.copy.objects == 2);
        si->Buffer[0] = 'X';
        f.memory[s.undo + 1] = 'Y';
        assert(f.memory[s.buffer] == 'X' && si->UndoBuffer[1] == 'Y');
    }
}

static void test_classic_stringextend_offsets()
{
    static_assert(GACT_BOOLEXTEND == GACT_STRINGEXTEND);
    for (bool integer : { false, true })
        for (unsigned bits : { 1u, 2u, 3u })
        {
            ClassicFixture f;
            own_gadget(f, OWN, 0, GFLG_GADGHNONE | (bits & 1 ? GFLG_STRINGEXTEND : 0));
            const OwnString s = own_string(f, OWN, 0, integer);
            const UWORD activation = f.word(OWN + GD_ACTIVATION) | (bits & 2 ? GACT_BOOLEXTEND : 0);
            m68k_write16(&f.ctx, OWN + GD_ACTIVATION, activation);
            TextFont font{};
            font.tf_YSize = 17; font.tf_XSize = 9;
            assert(!held_fixture_font);
            held_fixture_font = &font;
            font.tf_Accessors = 1;
            const unsigned closes_before = fixture_font_closes;
            const ULONG guest_font = Kaffeine_FontGuest(&f.ctx, &font);
            assert(guest_font && Kaffeine_FontNative(&f.ctx, guest_font) == &font);
            Kaffeine_FontOpened(&f.ctx, guest_font);
            m68k_write32(&f.ctx, s.extension + SE_FONT, guest_font);
            const auto memory = f.memory;
            const UWORD flags = f.word(OWN + GD_FLAGS);
            {
                Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
                auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
                StringExtend *se = si->Extension;
                assert(c.head->Flags == (flags | GFLG_STRINGEXTEND) && c.head->Activation == activation);
                assert(block_holding(si) == c.block && se && block_holding(se) == c.block);
                assert(string_state(*si) == STRING_INITIAL && si->MaxChars == STRMAX &&
                       si->CLeft == 7 && si->CTop == 8 && !si->AltKeyMap);
                assert(si->Buffer == f.ctx.mem + s.buffer && si->UndoBuffer == f.ctx.mem + s.undo);
                assert(se->Font == &font && !se->EditHook && se->WorkBuffer == (STRPTR)(f.ctx.mem + s.work));
                assert(se->Pens[0] == 0x91 && se->Pens[1] == 0xA2 &&
                       se->ActivePens[0] == 0xB3 && se->ActivePens[1] == 0xC4 &&
                       se->InitialModes == 0x89ABCDEF);
                for (ULONG reserved : se->Reserved) assert(!reserved);
                assert(c.copy.objects == 3 && f.memory == memory);
                se->WorkBuffer[0] = 'Z';
                assert(f.memory[s.work] == 'Z');
            }
            // In particular the shared BOOLEXTEND bit must not leave a string off.
            assert(f.add_one(OWN) == 3 && !f.stopped());
            assert((f.copy(OWN)->Flags & GFLG_STRINGEXTEND) && f.word(OWN + GD_FLAGS) == flags &&
                   string_info(f)->Extension->Font == &font);
            assert(f.remove_one(OWN) == 3 && native_blocks.empty());
            Kaffeine_FontClosed(&f.ctx, guest_font);
            assert(!Kaffeine_FontNative(&f.ctx, guest_font));
            assert(!held_fixture_font && !font.tf_Accessors && fixture_font_closes == closes_before + 1);
        }
}

static void test_classic_string_boundaries()
{
    enum Edge { INFO, EXTENSION, BUFFER, UNDO, WORK };
    for (Edge edge : { INFO, EXTENSION, BUFFER, UNDO, WORK })
    {
        Fixture f;
        own_gadget(f, OWN, 0, GFLG_GADGHNONE | GFLG_STRINGEXTEND);
        OwnString s = own_string(f);
        const ULONG bytes = edge == INFO ? STRINGINFO_SIZE : edge == EXTENSION ? STRINGEXTEND_SIZE : STRMAX;
        const ULONG at = f.ctx.mem_size - bytes;
        if (edge == INFO)
        {
            std::memcpy(f.memory.data() + at, f.memory.data() + s.info, bytes);
            s.info = at;
            m68k_write32(&f.ctx, OWN + GD_SPECIALINFO, at);
        }
        else if (edge == EXTENSION)
        {
            std::memcpy(f.memory.data() + at, f.memory.data() + s.extension, bytes);
            m68k_write32(&f.ctx, s.info + SI_EXTENSION, at);
        }
        else
        {
            std::memset(f.memory.data() + at, 'e', bytes);
            if (edge == BUFFER) f.memory.back() = 0;
            m68k_write32(&f.ctx, edge == WORK ? s.extension + SE_WORKBUFFER :
                                  s.info + (edge == BUFFER ? SI_BUFFER : SI_UNDOBUFFER), at);
        }
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
        assert(si && si->Extension && !si->Extension->Font && !si->Extension->EditHook);
        if (edge == BUFFER) assert(si->Buffer == f.ctx.mem + at && si->Buffer[STRMAX - 1] == 0);
        if (edge == UNDO) assert(si->UndoBuffer == f.ctx.mem + at);
        if (edge == WORK) assert(si->Extension->WorkBuffer == (STRPTR)(f.ctx.mem + at));
        if (edge == INFO || edge == EXTENSION)
        {
            // One byte later the record runs past the end of guest memory.
            std::memmove(f.memory.data() + at + 1, f.memory.data() + at, bytes - 1);
            m68k_write32(&f.ctx, edge == INFO ? OWN + GD_SPECIALINFO : s.info + SI_EXTENSION, at + 1);
            KaffeineCopy bad{};
            assert(!Kaffeine_CopyGadget(&f.ctx, &bad, OWN) && bad.why);
        }
    }
    for (WORD max : { (WORD)1, (WORD)32767 })
    {
        Fixture f(0x20000);
        own_gadget(f);
        const OwnString s = own_string(f);
        const ULONG buffer = f.ctx.mem_size - max;
        std::memset(f.ctx.mem + buffer, 'm', max);
        f.memory.back() = 0;
        m68k_write32(&f.ctx, s.info + SI_BUFFER, buffer);
        m68k_write16(&f.ctx, s.info + SI_MAXCHARS, max);
        m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, 0);
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
        assert(si->MaxChars == max && si->Buffer == f.ctx.mem + buffer && !si->Buffer[max - 1]);
    }
}

static void test_classic_string_optional_buffers_and_keymaps()
{
    for (bool extended : { false, true })
        for (bool undo : { false, true })
            for (bool work : { false, true })
                for (unsigned keymap : { 0u, 1u, 2u })
                {
                    ClassicFixture f;
                    const auto a = own_chain(f);
                    const OwnString s = own_string(f, a[1]);
                    if (extended) m68k_write16(&f.ctx, a[1] + GD_ACTIVATION, GACT_STRINGEXTEND);
                    if (!undo) m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, 0);
                    if (!work) m68k_write32(&f.ctx, s.extension + SE_WORKBUFFER, 0);
                    // Default keymap, ALTKEYMAP with NULL, or a custom keymap.
                    if (keymap) m68k_write16(&f.ctx, a[1] + GD_ACTIVATION,
                                           f.word(a[1] + GD_ACTIVATION) | GACT_ALTKEYMAP);
                    m68k_write32(&f.ctx, s.info + SI_ALTKEYMAP, keymap == 1 ? 0 : 0xFFFFFFFC);
                    const auto memory = f.memory;
                    const UWORD shadows = f.ctx.num_shadows;
                    const unsigned calls = native_calls, allocations = native_allocations;
                    assert(f.add(a[0]) == (keymap == 2 ? 0xFFFFu : 3u) && !f.stopped());
                    if (keymap == 2)
                    {
                        assert(!f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
                               f.ctx.num_shadows == shadows && native_calls == calls &&
                               native_allocations == allocations && f.memory == memory);
                        f.only_system(0);
                    }
                    else
                    {
                        StringInfo *si = string_info(f, a[1]);
                        assert(!si->AltKeyMap && si->UndoBuffer == (undo ? f.ctx.mem + s.undo : nullptr));
                        assert(!!si->Extension == extended);
                        if (extended)
                            assert(!si->Extension->Font && !si->Extension->EditHook &&
                                   si->Extension->WorkBuffer == (work ? (STRPTR)(f.ctx.mem + s.work) : nullptr));
                    }
                }
}

static void test_classic_string_text_count_boundaries()
{
    for (ULONG left : { 1u, (ULONG)STRMAX, 0x7FFDu, 0x7FFEu, 0x7FFFu, 0x8000u })
        for (bool nul : { false, true })
            for (bool integer : { false, true })
            {
                Fixture f(0x20000, true);
                own_gadget(f);
                const OwnString s = own_string(f, OWN, 0, integer);
                const WORD max = (WORD)std::min<ULONG>(STRMAX, left);
                m68k_write16(&f.ctx, s.info + SI_MAXCHARS, max);
                const ULONG buffer = tail_string_buffer(f, s, left, nul);
                const auto memory = f.memory;
                Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
                auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
                assert(si->Buffer == f.ctx.mem + buffer && si->MaxChars == max && f.memory == memory);
                assert(std::all_of(f.ctx.mem + f.ctx.mem_size, f.memory.data() + f.memory.size(),
                                   [](UBYTE b) { return b == 0; }));
            }
    Fixture f(0x20000, true);
    own_gadget(f, OWN, 0, GFLG_GADGHNONE | GFLG_STRINGEXTEND);
    const OwnString s = own_string(f);
    // Allocated guard bytes are not part of the guest buffer's MaxChars.
    for (ULONG field : { s.info + SI_BUFFER, s.info + SI_UNDOBUFFER, s.extension + SE_WORKBUFFER })
    {
        const ULONG saved = f.lng(field);
        m68k_write32(&f.ctx, field, f.ctx.mem_size);
        KaffeineCopy bad{};
        assert(!Kaffeine_CopyGadget(&f.ctx, &bad, OWN) && bad.why && native_blocks.empty());
        m68k_write32(&f.ctx, field, saved);
    }
}

static void test_classic_string_beyond_maxchars()
{
    for (bool integer : { false, true })
        for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnString s = own_string(f, OWN, 0, integer);
            std::memcpy(f.ctx.mem + s.buffer, "0123456789", 11);
            m68k_write16(&f.ctx, s.info + SI_MAXCHARS, 8);
            const auto memory = f.memory;
            {
                Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
                auto *si = static_cast<StringInfo *>(c.head->SpecialInfo);
                assert(si->MaxChars == 8 && si->Buffer == f.ctx.mem + s.buffer && f.memory == memory);
            }
            assert(f.add_one(OWN) == 3 && !f.stopped());
            count_native_string_texts(f);
            const unsigned calls = native_calls;
            assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
            assert(native_calls == calls + 1 && string_calls.size() == 1 &&
                   string_calls[0].gadget == f.copy(OWN) && string_calls[0].window == &f.windows[0] &&
                   string_info(f)->NumChars == 10 && f.word(s.info + SI_NUMCHARS) == 10 &&
                   string_info(f)->MaxChars == 8 && !f.memory[s.buffer + 10]);
        }
}

enum BadString
{
    INFO_NULL, INFO_PAST, INFO_SHORT, INFO_WRAP, MAX_ZERO, MAX_NEGATIVE, MAX_SIGNBIT,
    BUFFER_NULL, BUFFER_PAST, BUFFER_SHORT, BUFFER_WRAP,
    UNDO_PAST, UNDO_SHORT, UNDO_WRAP, EXT_NULL, EXT_PAST, EXT_SHORT, EXT_WRAP,
    EDIT_HOOK, UNKNOWN_FONT, WORK_PAST, WORK_SHORT, WORK_WRAP
};
static constexpr BadString bad_strings[] =
{
    INFO_NULL, INFO_PAST, INFO_SHORT, INFO_WRAP, MAX_ZERO, MAX_NEGATIVE, MAX_SIGNBIT,
    BUFFER_NULL, BUFFER_PAST, BUFFER_SHORT, BUFFER_WRAP,
    UNDO_PAST, UNDO_SHORT, UNDO_WRAP, EXT_NULL, EXT_PAST, EXT_SHORT, EXT_WRAP,
    EDIT_HOOK, UNKNOWN_FONT, WORK_PAST, WORK_SHORT, WORK_WRAP
};
static void bad_string(Fixture &f, ULONG gadget, const OwnString &s, BadString bad)
{
    m68k_write16(&f.ctx, gadget + GD_ACTIVATION, f.word(gadget + GD_ACTIVATION) | GACT_STRINGEXTEND);
    const ULONG end = f.ctx.mem_size;
    switch (bad)
    {
    case INFO_NULL: m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, 0); break;
    case INFO_PAST: m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, end); break;
    case INFO_SHORT: m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, end - STRINGINFO_SIZE + 1); break;
    case INFO_WRAP: m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, 0xFFFFFFF0); break;
    case MAX_ZERO: m68k_write16(&f.ctx, s.info + SI_MAXCHARS, 0); break;
    case MAX_NEGATIVE: m68k_write16(&f.ctx, s.info + SI_MAXCHARS, (UWORD)-1); break;
    case MAX_SIGNBIT: m68k_write16(&f.ctx, s.info + SI_MAXCHARS, 0x8000); break;
    case BUFFER_NULL: m68k_write32(&f.ctx, s.info + SI_BUFFER, 0); break;
    case BUFFER_PAST: m68k_write32(&f.ctx, s.info + SI_BUFFER, end); break;
    case BUFFER_SHORT: m68k_write32(&f.ctx, s.info + SI_BUFFER, end - STRMAX + 1); break;
    case BUFFER_WRAP: m68k_write32(&f.ctx, s.info + SI_BUFFER, 0xFFFFFFF0); break;
    case UNDO_PAST: m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, end); break;
    case UNDO_SHORT: m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, end - STRMAX + 1); break;
    case UNDO_WRAP: m68k_write32(&f.ctx, s.info + SI_UNDOBUFFER, 0xFFFFFFF0); break;
    case EXT_NULL: m68k_write32(&f.ctx, s.info + SI_EXTENSION, 0); break;
    case EXT_PAST: m68k_write32(&f.ctx, s.info + SI_EXTENSION, end); break;
    case EXT_SHORT: m68k_write32(&f.ctx, s.info + SI_EXTENSION, end - STRINGEXTEND_SIZE + 1); break;
    case EXT_WRAP: m68k_write32(&f.ctx, s.info + SI_EXTENSION, 0xFFFFFFF0); break;
    case EDIT_HOOK: m68k_write32(&f.ctx, s.extension + SE_EDITHOOK, 0x4C00); break;
    case UNKNOWN_FONT: m68k_write32(&f.ctx, s.extension + SE_FONT, 0x4C00); break;
    case WORK_PAST: m68k_write32(&f.ctx, s.extension + SE_WORKBUFFER, end); break;
    case WORK_SHORT: m68k_write32(&f.ctx, s.extension + SE_WORKBUFFER, end - STRMAX + 1); break;
    case WORK_WRAP: m68k_write32(&f.ctx, s.extension + SE_WORKBUFFER, 0xFFFFFFF0); break;
    }
}

static void test_classic_string_refusals()
{
    for (BadString bad : bad_strings)
        for (bool integer : { false, true })
            for (unsigned index : { 0u, 1u, 2u })
            {
                ClassicFixture f;
                const auto a = own_chain(f);
                const OwnString s = own_string(f, a[index], 0, integer);
                bad_string(f, a[index], s, bad);
                const unsigned calls = native_calls;
                const auto memory = f.memory;
                const UWORD shadows = f.ctx.num_shadows;
                const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
                assert(f.add(a[0]) == 20 && f.stopped());
                assert(native_calls == calls && !f.ctx.classic && !f.ctx.gadgets &&
                       native_blocks.empty() && native_sizes.empty() && f.ctx.num_shadows == shadows &&
                       Kaffeine_HeapAvail(&f.ctx, 0) == heap && f.memory == memory);
                for (ULONG guest : a)
                    assert(!Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_CLASSIC) &&
                           !Kaffeine_ClassicOn(&f.ctx, guest));
                f.only_system(0); f.only_system(1);
                // Repair only the string; the same run must have no stale marks.
                own_string(f, a[index], 0, integer);
                assert(f.add(a[0]) == 3 && !f.stopped());
                assert(f.remove(a[0]) == 3 && native_blocks.empty() && !f.ctx.classic);
            }
    for (BadString bad : bad_strings)
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnString s = own_string(f);
        bad_string(f, OWN, s, bad);
        const UWORD shadows = f.ctx.num_shadows;
        const unsigned calls = native_calls;
        const auto memory = f.memory;
        assert(f.add_one(OWN) == 20 && f.stopped() && native_calls == calls && f.memory == memory &&
               !f.ctx.classic && native_blocks.empty() && native_sizes.empty() && f.ctx.num_shadows == shadows);
        f.only_system(0);
    }
}

static void test_classic_string_open_refusals()
{
    for (ULONG lvo : { 606u, 204u })
        for (BadString bad : bad_strings)
        {
            ClassicOpenFixture f;
            const auto a = own_chain(f);
            const OwnString s = own_string(f, a[1], 0, lvo == 204);
            bad_string(f, a[1], s, bad);
            const unsigned calls = native_calls;
            const UWORD shadows = f.ctx.num_shadows;
            const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
            const auto memory = f.memory;
            assert(f.open(lvo, a[0]) == 20 && f.stopped() && !f.mw);
            assert(native_calls == calls && !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
                   f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap &&
                   !f.system[2].NextGadget && opened_tags.empty());
            assert(std::equal(memory.begin() + OWN, memory.begin() + OWN + 192, f.memory.begin() + OWN));
            // open() writes its arguments at tags or 0x4D00. The program's
            // string records and buffers must remain byte-for-byte intact.
            assert(std::equal(memory.begin() + STRINFO, memory.begin() + 0x5400, f.memory.begin() + STRINFO));
        }
}

static void test_classic_string_open_state()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        const OwnString text = own_string(f, a[0]);
        const OwnString integer = own_string(f, a[2], 1, true);
        watch_strings = change_strings = true;
        string_after = { -17, 19, -21, 23, 25, -1234567 };
        assert(f.open(lvo, a[0]) && !f.stopped());
        assert(string_calls.size() == 2);
        for (unsigned i = 0; i < 2; ++i)
        {
            const ULONG guest = i ? a[2] : a[0], info = i ? integer.info : text.info;
            Gadget *g = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_CLASSIC));
            assert(g && string_calls[i].gadget == g && string_calls[i].window == &f.window &&
                   std::string(string_calls[i].name) == "OpenWindowTagList" &&
                   string_calls[i].state == STRING_INITIAL);
            assert(guest_string_state(f, info) == string_after &&
                   string_state(*static_cast<StringInfo *>(g->SpecialInfo)) == string_after);
        }
        f.close();
        assert(native_blocks.empty() && !f.ctx.classic);
    }
}

static void test_classic_string_open_failure()
{
    for (ULONG lvo : { 606u, 204u })
        for (bool guest_fails : { false, true })
        {
            ClassicOpenFixture f;
            const auto a = own_chain(f);
            const OwnString text = own_string(f, a[0]);
            own_string(f, a[2], 1, true);
            m68k_write16(&f.ctx, a[0] + GD_ACTIVATION, GACT_STRINGEXTEND);
            const UWORD shadows = f.ctx.num_shadows;
            const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
            const auto memory = f.memory;
            if (guest_fails) f.exhaust_heap();
            assert(!f.open(lvo, a[0], !guest_fails) && !f.stopped() && !f.mw);
            assert(!f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
                   f.ctx.num_shadows == shadows && !f.system[2].NextGadget &&
                   closed_windows.size() == (guest_fails ? 1u : 0u));
            assert(std::equal(memory.begin() + OWN, memory.begin() + OWN + 192, f.memory.begin() + OWN));
            assert(std::equal(memory.begin() + STRINFO, memory.begin() + 0x5400, f.memory.begin() + STRINFO));
            f.release_heap();
            assert(Kaffeine_HeapAvail(&f.ctx, 0) == heap && f.str(text.buffer) == "text");
            // A dropped open must leave neither prepared records nor marks.
            assert(f.open(lvo, a[0]) && !f.stopped() && f.system[2].NextGadget);
            f.close();
            assert(!f.ctx.classic && native_blocks.empty() && f.ctx.num_shadows == shadows &&
                   Kaffeine_HeapAvail(&f.ctx, 0) == heap);
        }
}

static void test_classic_string_call_state()
{
    for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
        for (bool integer : { false, true })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnString s = own_string(f, OWN, 0, integer);
            assert(f.add_one(OWN) == 3);
            StringInfo *si = string_info(f);
            const StringState input{ -32768, 32767, -101, -102, -103, (LONG)0x80000000 };
            guest_string_state(f, s.info, input);
            watch_strings = change_strings = true;
            // Reverse all three imports during the native call. This also
            // checks that the agreed values were updated on the way in.
            string_after = { 1, 2, -201, -202, -203, -6 };
            const unsigned calls = native_calls;
            assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
            assert(native_calls == calls + 1 && string_calls.size() == 1);
            const StringCall &seen = string_calls[0];
            const StringState expected{ 0, STRING_INITIAL.chars, STRING_INITIAL.undo,
                                       STRING_INITIAL.chars, STRING_INITIAL.count, input.number };
            assert(seen.gadget == f.copy(OWN) && seen.window == &f.windows[0] && seen.state == expected);
            const char *name = lvo == 462 ? "ActivateGadget" : lvo == 186 ? "OnGadget" :
                               lvo == 174 ? "OffGadget" : "RefreshGList";
            assert(std::string(seen.name) == name);
            assert(string_state(*si) == string_after && guest_string_state(f, s.info) == string_after);
            if (lvo == 432 || lvo == 222)
                assert_gadget_call("RefreshGList", &f.windows[0], f.copy(OWN), lvo == 432 ? 1 : -1, 0);
            if (lvo == 462)
                assert(passed_object == f.copy(OWN) && passed_window == &f.windows[0] && !passed_requester);
            else if (lvo == 174 || lvo == 186)
                assert(changed_object == f.copy(OWN) && changed_window == &f.windows[0] && !changed_requester);
        }
}

static void change_shared_string_field(StringState &state, unsigned field)
{
    switch (field)
    {
    case 0: state.pos = 3; break;
    case 1: state.disp = 0; break;
    case 2: state.number = (LONG)0x87654321; break;
    default: assert(false);
    }
}
static void test_classic_string_independent_changes()
{
    for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
        for (unsigned program : { 0u, 1u, 2u })
            for (unsigned intuition : { 0u, 1u, 2u })
            {
                if (program == intuition) continue;
                ClassicFixture f;
                own_gadget(f);
                const OwnString s = own_string(f);
                assert(f.add_one(OWN) == 3);
                StringInfo *si = string_info(f);
                StringState input = STRING_INITIAL, output = STRING_INITIAL, merged = STRING_INITIAL;
                change_shared_string_field(input, program);
                change_shared_string_field(output, intuition);
                change_shared_string_field(merged, program);
                change_shared_string_field(merged, intuition);
                guest_string_state(f, s.info, input);
                string_state(*si, output);
                watch_strings = true;
                assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
                assert(string_calls.size() == 1 && string_calls[0].state == merged &&
                       string_state(*si) == merged && guest_string_state(f, s.info) == merged);
                // Repeating the call must not restore an obsolete snapshot.
                assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
                assert(string_calls.size() == 2 && string_calls[1].state == merged &&
                       guest_string_state(f, s.info) == merged);
            }
}

static void test_classic_string_message_state()
{
    for (bool gt : { false, true })
        for (ULONG cls : { (ULONG)IDCMP_GADGETUP, (ULONG)IDCMP_GADGETDOWN, (ULONG)IDCMP_MOUSEMOVE })
            for (unsigned program : { 0u, 1u, 2u })
            {
                ClassicFixture f;
                const auto a = own_chain(f);
                const OwnString text = own_string(f, a[0]);
                const OwnString integer = own_string(f, a[2], 1, true);
                assert(f.add(a[0]) == 3);
                StringInfo *si = string_info(f, a[0]), *other = string_info(f, a[2]);
                StringState input = STRING_INITIAL, output = STRING_INITIAL;
                change_shared_string_field(input, program);
                change_shared_string_field(output, (program + 1) % 3);
                output.undo = -14; output.chars = 15; output.count = -16;
                guest_string_state(f, text.info, input);
                string_state(*si, output);
                const StringState all{ -31, -32, -33, -34, -35, -2147483647 };
                string_state(*other, all);
                IntuiMessage im{};
                // A non-gadget message must still export every gadget on its window.
                f.message(im, cls == IDCMP_MOUSEMOVE ? nullptr : f.copy(a[0]), cls);
                const ULONG msg = gt ? f.call(72, { f.port() }) : f.call_at(Fixture::execbase, 372, { f.port() });
                assert(msg && !f.stopped() && f.lng(msg + 20) == cls &&
                       f.lng(msg + 28) == (cls == IDCMP_MOUSEMOVE ? 0 : a[0]));
                StringState merged = output;
                change_shared_string_field(merged, program);
                assert(guest_string_state(f, text.info) == merged &&
                       guest_string_state(f, integer.info) == all && string_state(*si) == output);
                // Delivery exports only; the pending program change enters on refresh.
                assert(string_call(f, 432, a[0]) == 0 && string_state(*si) == merged &&
                       guest_string_state(f, text.info) == merged);
                if (gt) assert(f.call(78, { 0, msg }) == 0);
                else assert(f.call_at(Fixture::execbase, 378, { 0, msg }) == 0);
                assert(replied_messages.back() == &im.ExecMessage);
            }
}

static void test_classic_string_text_count_calls()
{
    for (ULONG left : { 1u, (ULONG)STRMAX, 0x7FFDu, 0x7FFEu, 0x7FFFu, 0x8000u })
        for (bool nul : { false, true })
            for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
                for (UBYTE guard : { (UBYTE)0, (UBYTE)'x' })
                {
                    ClassicFixture f(0x20000, true);
                    own_gadget(f);
                    const OwnString s = own_string(f, OWN, 0, lvo == 462);
                    const WORD max = (WORD)std::min<ULONG>(STRMAX, left);
                    m68k_write16(&f.ctx, s.info + SI_MAXCHARS, max);
                    const ULONG buffer = tail_string_buffer(f, s, left);
                    assert(f.add_one(OWN) == 3 && !f.stopped());
                    StringInfo *si = string_info(f);
                    // The program may remove its NUL after copying. Also exercise
                    // a guard with no NUL, forcing the longest native count/write.
                    if (!nul) f.memory[f.ctx.mem_size - 1] = 'x';
                    std::fill(f.memory.begin() + f.ctx.mem_size, f.memory.end(), guard);
                    const unsigned calls = native_calls;
                    const auto blocks = native_blocks;
                    const auto sizes = native_sizes;
                    const UWORD shadows = f.ctx.num_shadows;
                    count_native_string_texts(f);
                    const ULONG result = string_call(f, lvo);
                    const WORD chars = (WORD)std::min<ULONG>(nul ? left - 1 : guard ? 0x7FFE : left, 0x7FFE);
                    assert(result == (lvo == 462 ? TRUE : 0) && !f.stopped() && native_calls == calls + 1 &&
                           string_calls.size() == 1 && string_calls[0].gadget == f.copy(OWN) &&
                           si->NumChars == chars && f.word(s.info + SI_NUMCHARS) == (UWORD)chars &&
                           si->DispCount == std::min<WORD>(chars, 7) &&
                           si->MaxChars == max && si->Buffer == f.ctx.mem + buffer && !si->Buffer[chars]);
                    assert(std::all_of(si->Buffer, si->Buffer + chars, [](UBYTE c) { return c == 'x'; }) &&
                           native_blocks == blocks && native_sizes == sizes && f.ctx.num_shadows == shadows &&
                           f.memory.back() == guard);
                }
}

static void test_classic_string_other_windows()
{
    for (ULONG lvo : { 186u, 174u })
        for (ULONG window : { ClassicFixture::win1, 0u })
        {
            ClassicFixture f(0x10000, true);
            own_gadget(f);
            const OwnString s = own_string(f);
            const ULONG buffer = tail_string_buffer(f, s, STRMAX);
            assert(f.add_one(OWN) == 3);
            StringInfo *si = string_info(f);
            f.memory[f.ctx.mem_size - 1] = 'x';
            assert(si->Buffer == f.ctx.mem + buffer);
            m68k_write16(&f.ctx, s.info + SI_BUFFERPOS, (UWORD)-77);
            const unsigned calls = native_calls;
            watch_strings = true;
            assert(string_call(f, lvo, OWN, window) == 0 && !f.stopped());
            assert(native_calls == calls && string_calls.empty() && enable_list_saw.empty() &&
                   si->BufferPos == 0 && f.word(s.info + SI_BUFFERPOS) == 0 &&
                   !!(f.copy(OWN)->Flags & GFLG_DISABLED) == (lvo == 174) &&
                   !!(f.word(OWN + GD_FLAGS) & GFLG_DISABLED) == (lvo == 174));
            // Wrong-window refresh and activation also have no native work.
            for (ULONG other : { 432u, 222u, 462u })
                assert(string_call(f, other, OWN, window) == 0 && !f.stopped() && native_calls == calls);
        }
}

static void test_classic_string_enable_window_state()
{
    for (ULONG lvo : { 186u, 174u })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        const OwnString first = own_string(f, a[0]);
        const OwnString last = own_string(f, a[2], 1, true);
        assert(f.add(a[0]) == 3);
        own_gadget(f, 0x4700);
        assert(f.add_one(0x4700) == 6); // a separate run on the same window
        own_gadget(f, 0x4740);
        const OwnString elsewhere = own_string(f, 0x4740, 2);
        assert(f.add_one(0x4740, 0xFFFF, 1) == 3);
        const StringState input1{ -11, -12, -13, -14, -15, -16 };
        const StringState input2{ -21, -22, -23, -24, -25, -26 };
        guest_string_state(f, first.info, input1);
        guest_string_state(f, last.info, input2);
        guest_string_state(f, elsewhere.info, input2);
        for (ULONG guest : { a[0], a[1], a[2], 0x4700u, 0x4740u })
            m68k_write16(&f.ctx, guest + GD_FLAGS, GFLG_GADGHNONE | GFLG_DISABLED);
        watch_strings = change_strings = true;
        string_after = { 101, 102, 103, 104, 105, 106 };
        const unsigned calls = native_calls;
        assert(string_call(f, lvo, a[1]) == 0 && !f.stopped() && native_calls == calls + 1);
        assert(string_calls.size() == 2 && enable_list_saw.size() == 7);
        for (unsigned i = 0; i < 2; ++i)
        {
            const StringState &input = i ? input2 : input1;
            const StringState expected{ 0, 0, STRING_INITIAL.undo,
                                       STRING_INITIAL.chars, STRING_INITIAL.count, input.number };
            assert(string_calls[i].gadget == f.copy(i ? a[2] : a[0]) &&
                   string_calls[i].window == &f.windows[0] && string_calls[i].state == expected);
        }
        // Capture all gadgets before On/Off changes the target's disabled bit.
        for (ULONG guest : { a[0], a[1], a[2], 0x4700u })
        {
            Gadget *g = f.copy(guest);
            const auto seen = std::find_if(enable_list_saw.begin(), enable_list_saw.end(),
                                           [g](const GadgetState &s) { return s.gadget == g; });
            assert(seen != enable_list_saw.end() && (seen->flags & GFLG_DISABLED));
        }
        assert(guest_string_state(f, first.info) == string_after && guest_string_state(f, last.info) == string_after);
        assert(string_state(*string_info(f, 0x4740)) == STRING_INITIAL &&
               !(f.copy(0x4740)->Flags & GFLG_DISABLED) && guest_string_state(f, elsewhere.info) == input2);
    }
}

static void test_classic_string_selected_imports()
{
    for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
        for (bool integer : { false, true })
            for (bool program_selects : { false, true })
            {
                ClassicFixture f;
                own_gadget(f);
                const OwnString s = own_string(f, OWN, 0, integer);
                assert(f.add_one(OWN) == 3);
                Gadget *g = f.copy(OWN);
                StringInfo *si = string_info(f);
                const StringState pending{ 3, 0, -11, 222, 223, (LONG)0x87654321 };
                guest_string_state(f, s.info, pending);
                if (!program_selects) g->Flags |= GFLG_SELECTED;
                watch_strings = true;
                for (bool disabled : { true, false })
                {
                    // Selection may come from the input handler or the
                    // shared guest Flags. Disabled must still pass both ways.
                    const UWORD selected = program_selects ? GFLG_SELECTED : f.word(OWN + GD_FLAGS) & GFLG_SELECTED;
                    m68k_write16(&f.ctx, OWN + GD_FLAGS,
                                 GFLG_GADGHNONE | selected | (disabled ? GFLG_DISABLED : 0));
                    string_calls.clear();
                    assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
                    assert(string_calls.size() == 1 && string_calls[0].state == STRING_INITIAL &&
                           (string_calls[0].flags & GFLG_SELECTED) &&
                           !!(string_calls[0].flags & GFLG_DISABLED) == disabled &&
                           string_state(*si) == STRING_INITIAL && guest_string_state(f, s.info) == pending);
                    const bool native_disabled = lvo == 174 || (lvo != 186 && disabled);
                    assert((g->Flags & GFLG_SELECTED) && (f.word(OWN + GD_FLAGS) & GFLG_SELECTED) &&
                           !!(g->Flags & GFLG_DISABLED) == native_disabled &&
                           !!(f.word(OWN + GD_FLAGS) & GFLG_DISABLED) == native_disabled);
                }
                if (program_selects)
                    m68k_write16(&f.ctx, OWN + GD_FLAGS, f.word(OWN + GD_FLAGS) & ~GFLG_SELECTED);
                else
                    g->Flags &= ~GFLG_SELECTED;
                string_calls.clear();
                assert(string_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
                const StringState imported{ pending.pos, pending.disp, STRING_INITIAL.undo,
                                            STRING_INITIAL.chars, STRING_INITIAL.count, pending.number };
                assert(string_calls.size() == 1 && !(string_calls[0].flags & GFLG_SELECTED) &&
                       string_calls[0].state == imported && string_state(*si) == imported &&
                       guest_string_state(f, s.info) == pending &&
                       !(f.word(OWN + GD_FLAGS) & GFLG_SELECTED));
            }
}

static void test_classic_string_bounded_imports()
{
    const std::array<WORD, 7> positions{ -32768, -1, 0, 5, 6, 7, 32767 };
    for (WORD chars : { (WORD)0, (WORD)6 })
        for (bool integer : { false, true })
            for (unsigned i = 0; i < positions.size(); ++i)
            {
                ClassicFixture f;
                own_gadget(f);
                const OwnString s = own_string(f, OWN, 0, integer);
                assert(f.add_one(OWN) == 3);
                StringInfo *si = string_info(f);
                // The new text and the last count deliberately disagree,
                // both after shortening and after lengthening the text.
                std::memcpy(f.ctx.mem + s.buffer, chars ? "Hello!" : "", chars + 1);
                const WORD stale = chars ? 0 : 6;
                si->NumChars = stale;
                const WORD pos = positions[i], disp = positions[positions.size() - i - 1];
                const StringState input{ pos, disp, -41, 32767, -43, -1234567 };
                guest_string_state(f, s.info, input);
                // Import is distinct from export here. In particular the
                // agreed input must retain the unbounded program value.
                Kaffeine_ClassicRefreshing(&f.ctx, &f.windows[0], f.copy(OWN), 1);
                const WORD bounded_pos = std::clamp<WORD>(pos, 0, chars);
                const WORD bounded_disp = std::clamp<WORD>(disp, 0, chars);
                const StringState native{ bounded_pos, bounded_disp, STRING_INITIAL.undo,
                                          stale, STRING_INITIAL.count, input.number };
                assert(string_state(*si) == native && guest_string_state(f, s.info) == input);
                f.sync(0);
                assert((WORD)f.word(s.info + SI_BUFFERPOS) == bounded_pos &&
                       (WORD)f.word(s.info + SI_DISPPOS) == bounded_disp &&
                       (LONG)f.lng(s.info + SI_LONGINT) == input.number &&
                       f.word(s.info + SI_NUMCHARS) == (UWORD)stale);
            }
}

static void test_classic_string_new_text_positions()
{
    for (ULONG lvo : { 432u, 222u })
        for (bool integer : { false, true })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnString s = own_string(f, OWN, 0, integer);
            f.memory[s.buffer] = 0;
            guest_string_state(f, s.info, { 0, 0, 0, 0, 0, -6 });
            count_native_string_texts(f);
            assert(f.add_one(OWN) == 3);
            StringInfo *si = string_info(f);
            assert(!si->NumChars && !f.word(s.info + SI_NUMCHARS));
            std::memcpy(f.ctx.mem + s.buffer, "Hello", 6);
            m68k_write16(&f.ctx, s.info + SI_BUFFERPOS, 5);
            m68k_write16(&f.ctx, s.info + SI_DISPPOS, 5);
            Kaffeine_ClassicRefreshing(&f.ctx, &f.windows[0], f.copy(OWN), 1);
            assert(si->BufferPos == 5 && si->DispPos == 5 && !si->NumChars);
            string_calls.clear();
            assert(string_call(f, lvo) == 0 && !f.stopped());
            assert(string_calls.size() == 1 && string_calls[0].state.pos == 5 &&
                   string_calls[0].state.disp == 5 && !string_calls[0].state.chars &&
                   si->BufferPos == 5 && si->DispPos == 5 && si->NumChars == 5 &&
                   f.word(s.info + SI_BUFFERPOS) == 5 && f.word(s.info + SI_DISPPOS) == 5 &&
                   f.word(s.info + SI_NUMCHARS) == 5);
        }
}

static void test_classic_string_guard_positions()
{
    for (ULONG left : { 1u, (ULONG)STRMAX, 0x7FFEu, 0x7FFFu })
        for (bool reverse : { false, true })
        {
            ClassicFixture f(0x20000, true);
            own_gadget(f);
            const OwnString s = own_string(f);
            m68k_write16(&f.ctx, s.info + SI_MAXCHARS, (UWORD)std::min<ULONG>(left, STRMAX));
            const ULONG buffer = tail_string_buffer(f, s, left, false);
            assert(f.add_one(OWN) == 3);
            StringInfo *si = string_info(f);
            const WORD chars = (WORD)std::min<ULONG>(left, 0x7FFE);
            assert(si->NumChars == STRING_INITIAL.chars && chars != si->NumChars &&
                   std::all_of(f.ctx.mem + buffer, f.ctx.mem + f.ctx.mem_size,
                               [](UBYTE b) { return b == 'x'; }) &&
                   std::all_of(f.ctx.mem + f.ctx.mem_size, f.memory.data() + f.memory.size(),
                               [](UBYTE b) { return b == 0; }));
            const WORD pos = reverse ? -32768 : 32767, disp = reverse ? 32767 : -32768;
            m68k_write16(&f.ctx, s.info + SI_BUFFERPOS, (UWORD)pos);
            m68k_write16(&f.ctx, s.info + SI_DISPPOS, (UWORD)disp);
            Kaffeine_ClassicRefreshing(&f.ctx, &f.windows[0], f.copy(OWN), 1);
            const WORD bounded_pos = reverse ? 0 : chars, bounded_disp = reverse ? chars : 0;
            assert(si->BufferPos == bounded_pos && si->DispPos == bounded_disp &&
                   si->NumChars == STRING_INITIAL.chars &&
                   (WORD)f.word(s.info + SI_BUFFERPOS) == pos && (WORD)f.word(s.info + SI_DISPPOS) == disp);
            f.sync(0);
            assert(f.word(s.info + SI_BUFFERPOS) == (UWORD)bounded_pos &&
                   f.word(s.info + SI_DISPPOS) == (UWORD)bounded_disp);
            count_native_string_texts(f);
            assert(string_call(f, 432) == 0 && !f.stopped() && string_calls.size() == 1 &&
                   string_calls[0].state.pos == bounded_pos && string_calls[0].state.disp == bounded_disp &&
                   si->NumChars == chars && f.word(s.info + SI_NUMCHARS) == (UWORD)chars);
        }
}

static void test_classic_string_initial_selection()
{
    for (unsigned kind : { 0u, 1u, 2u }) // Boolean, string, integer
        for (bool one : { false, true })
        {
            ClassicFixture f;
            const UWORD base = GFLG_GADGHNONE | GFLG_DISABLED, flags = base | GFLG_SELECTED;
            own_gadget(f, OWN, 0, flags);
            OwnString s{};
            if (kind) s = own_string(f, OWN, 0, kind == 2);
            const auto memory = f.memory;
            Gadget *prepared = f.prepare(OWN);
            assert(prepared && prepared->Flags == (kind ? base : flags) &&
                   f.word(OWN + GD_FLAGS) == flags && f.memory == memory);
            Kaffeine_ClassicDropped(&f.ctx);
            assert(!f.ctx.classic && native_blocks.empty() && f.memory == memory);
            watch_strings = true;
            assert((one ? f.add_one(OWN) : f.add(OWN)) == 3 && !f.stopped());
            assert(f.copy(OWN)->Flags == (kind ? base : flags) &&
                   f.word(OWN + GD_FLAGS) == (kind ? base : flags));
            if (kind)
            {
                assert(string_calls.size() == 1 && string_calls[0].flags == base);
                const StringState input{ 3, 4, STRING_INITIAL.undo, STRING_INITIAL.chars,
                                         STRING_INITIAL.count, -7654321 };
                guest_string_state(f, s.info, input);
                string_calls.clear();
                assert(string_call(f, 432) == 0 && !f.stopped() && string_calls.size() == 1 &&
                       string_calls[0].state == input && string_state(*string_info(f)) == input &&
                       guest_string_state(f, s.info) == input);
            }
            else assert(string_calls.empty());
        }
}

static void test_classic_string_remove_selection()
{
    for (bool one : { false, true })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        own_string(f, a[1]); own_string(f, a[2], 1, true);
        assert(f.add(a[0]) == 3 && !remove_deselects);
        std::array<Gadget *, 3> copies{};
        std::array<IntuiMessage, 3> messages{};
        for (unsigned i = 0; i < a.size(); ++i)
        {
            copies[i] = f.copy(a[i]);
            copies[i]->Flags |= GFLG_SELECTED | GFLG_DISABLED;
            f.message(messages[i], copies[i]); // retain copies so their final flags can be inspected
        }
        f.sync(0);
        for (ULONG guest : a) assert(f.word(guest + GD_FLAGS) & GFLG_SELECTED);
        const unsigned calls = native_calls;
        if (one)
            for (ULONG guest : a) assert(f.remove_one(guest) == 3 && !f.stopped());
        else assert(f.remove(a[0]) == 3 && !f.stopped());
        assert(native_calls == calls + (one ? 3 : 1) && native_blocks.size() == 6);
        for (unsigned i = 0; i < a.size(); ++i)
        {
            const UWORD flags = GFLG_GADGHNONE | GFLG_DISABLED | (i ? 0 : GFLG_SELECTED);
            assert(copies[i]->Flags == flags && f.word(a[i] + GD_FLAGS) == flags &&
                   !Kaffeine_ClassicOn(&f.ctx, a[i]) && Kaffeine_ClassicGuest(&f.ctx, copies[i]) == a[i]);
        }
        f.only_system(0);
        while (GetMsg(&f.ports[0])) {}
        own_gadget(f, 0x4700);
        assert(f.add_one(0x4700) == 3 && f.remove_one(0x4700) == 3 && !f.ctx.classic && native_blocks.empty());
    }
}

static void test_classic_string_close_selection()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        own_string(f, a[1]); own_string(f, a[2], 1, true);
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.open(lvo, a[0]) && !remove_deselects);
        for (ULONG guest : a)
        {
            auto *g = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_CLASSIC));
            assert(g);
            g->Flags |= GFLG_SELECTED | GFLG_DISABLED;
        }
        Kaffeine_ClassicSync(&f.ctx, &f.window, nullptr);
        for (ULONG guest : a) assert(f.word(guest + GD_FLAGS) & GFLG_SELECTED);
        f.close();
        for (unsigned i = 0; i < a.size(); ++i)
            assert(f.word(a[i] + GD_FLAGS) == (GFLG_GADGHNONE | GFLG_DISABLED | (i ? 0 : GFLG_SELECTED)));
        assert(!f.ctx.classic && native_blocks.empty() && !f.system[2].NextGadget &&
               f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
    }
}

static void test_classic_string_add_state()
{
    for (bool one : { false, true })
        for (bool integer : { false, true })
        {
            ClassicFixture f;
            const auto a = own_chain(f, 2);
            const OwnString first = own_string(f, a[0], 0, integer);
            const OwnString second = own_string(f, a[1], 1, !integer);
            std::memcpy(f.ctx.mem + first.buffer, "0123456789", 11);
            std::memcpy(f.ctx.mem + second.buffer, "second text", 12);
            m68k_write16(&f.ctx, first.info + SI_MAXCHARS, 8);
            count_native_string_texts(f);
            const unsigned calls = native_calls;
            assert((one ? f.add_one(a[0]) : f.add(a[0])) == 3 && !f.stopped() && native_calls == calls + 1);
            assert(string_calls.size() == (one ? 1u : 2u));
            for (unsigned i = 0; i < (one ? 1u : 2u); ++i)
            {
                const ULONG info = i ? second.info : first.info;
                assert(std::string(string_calls[i].name) == (one ? "AddGadget" : "AddGList") &&
                       string_calls[i].gadget == f.copy(a[i]) && string_calls[i].window == &f.windows[0] &&
                       string_calls[i].state == STRING_INITIAL);
                assert(f.word(info + SI_NUMCHARS) == (i ? 11 : 10) &&
                       f.word(info + SI_DISPCOUNT) == 7 && string_info(f, a[i])->DispCount == 7);
            }
            if (one)
                assert(guest_string_state(f, second.info) == STRING_INITIAL &&
                       !Kaffeine_ClassicOn(&f.ctx, a[1]));
        }
}

static void test_classic_string_setedithook()
{
    for (ULONG hook : { 0u, 0x4C00u, OWN, 0xFFFFu, 0x10000u, 0xFFFFFFF0u, 0xFFFFFFFFu })
    {
        ClassicFixture f;
        own_gadget(f);
        own_string(f);
        assert(f.add_one(OWN) == 3);
        int native_hook = 0;
        assert(Kaffeine_ShadowAdd(&f.ctx, 0x4C00, &native_hook, FILLER));
        const unsigned calls = native_calls;
        const auto memory = f.memory;
        const auto blocks = native_blocks;
        const UWORD shadows = f.ctx.num_shadows;
        const auto list = f.list(0);
        assert(f.call_at(Fixture::intuition, 492, { hook }) == 20 && f.stopped());
        assert(native_calls == calls && f.memory == memory && native_blocks == blocks &&
               f.ctx.num_shadows == shadows && f.list(0) == list);
    }
}

static void test_classic_string_remove_state()
{
    for (bool one : { false, true })
    {
        ClassicFixture f;
        const auto a = own_chain(f, 2);
        const OwnString text = own_string(f, a[0]);
        const OwnString integer = own_string(f, a[1], 1, true);
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.add(a[0]) == 3);
        const StringState out1{ -32768, 32767, -1, -2, -3, (LONG)0x80000000 };
        const StringState out2{ 21, 22, 23, 24, 25, 0x12345678 };
        string_state(*string_info(f, a[0]), out1);
        string_state(*string_info(f, a[1]), out2);
        assert((one ? f.remove_one(a[0]) : f.remove(a[0])) == 3 && !f.stopped());
        assert(guest_string_state(f, text.info) == out1 && !Kaffeine_ClassicOn(&f.ctx, a[0]) &&
               !Kaffeine_ShadowNative(&f.ctx, a[0], KAFFEINE_SHADOW_CLASSIC));
        if (one)
        {
            assert(guest_string_state(f, integer.info) == STRING_INITIAL && native_blocks.size() == 2);
            assert(f.remove_one(a[1]) == 3 && !f.stopped());
        }
        assert(guest_string_state(f, integer.info) == out2 && !f.ctx.classic &&
               native_blocks.empty() && native_sizes.empty() && f.ctx.num_shadows == shadows &&
               Kaffeine_HeapAvail(&f.ctx, 0) == heap && f.str(text.buffer) == "text" &&
               f.str(integer.buffer) == "-123");
        f.only_system(0);
    }
}

static void test_classic_string_retirement()
{
    for (bool gt : { false, true })
        for (bool held : { false, true })
        {
            ClassicFixture f;
            const auto a = own_chain(f, 2);
            const OwnString text = own_string(f, a[0]);
            own_string(f, a[1], 1, true);
            m68k_write16(&f.ctx, a[0] + GD_ACTIVATION, GACT_STRINGEXTEND);
            assert(f.add(a[0]) == 3);
            Gadget *old = f.copy(a[0]);
            StringInfo *si = string_info(f, a[0]);
            StringExtend *se = si->Extension;
            assert(se && block_holding(se) == block_holding(old));
            IntuiMessage im{};
            f.message(im, old);
            const ULONG port = f.port();
            ULONG msg = 0;
            if (held) msg = gt ? f.call(72, { port }) : f.call_at(Fixture::execbase, 372, { port });
            assert(!held || msg);
            const StringState out{ -41, -42, -43, -44, -45, -46 };
            string_state(*si, out);
            assert(f.remove_one(a[0]) == 3 && !f.stopped() && !Kaffeine_ClassicOn(&f.ctx, a[0]));
            assert(!Kaffeine_ShadowNative(&f.ctx, a[0], KAFFEINE_SHADOW_CLASSIC) &&
                   Kaffeine_ClassicGuest(&f.ctx, old) == a[0] && native_blocks.size() == 4 &&
                   block_holding(si) == block_holding(old) && block_holding(se) == block_holding(old) &&
                   guest_string_state(f, text.info) == out && si->Buffer == f.ctx.mem + text.buffer);
            // A fresh copy can coexist with the retired one named by a message.
            assert(f.add_one(a[0]) == 4 && !f.stopped());
            Gadget *fresh = f.copy(a[0]);
            assert(fresh != old && native_blocks.size() == 6 && string_state(*string_info(f, a[0])) == out);
            if (!held) msg = gt ? f.call(72, { port }) : f.call_at(Fixture::execbase, 372, { port });
            assert(msg && f.lng(msg + 28) == a[0] && block_holding(si) && block_holding(se));
            if (gt) assert(f.call(78, { 0, msg }) == 0);
            else assert(f.call_at(Fixture::execbase, 378, { 0, msg }) == 0);
            assert(native_blocks.size() == 6); // reply allows the next successful removal to sweep
            assert(f.remove_one(a[0]) == 4 && !f.stopped() && native_blocks.size() == 2 &&
                   !Kaffeine_ClassicGuest(&f.ctx, old) && !Kaffeine_ClassicGuest(&f.ctx, fresh) &&
                   !block_holding(si) && !block_holding(se));
            assert(f.remove_one(a[1]) == 3 && !f.ctx.classic && native_blocks.empty());
            assert(f.str(text.buffer) == "text");
            f.only_system(0);
        }
}

static void test_classic_string_close_lifetime()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        const OwnString text = own_string(f, a[0]);
        const OwnString integer = own_string(f, a[2], 1, true);
        m68k_write16(&f.ctx, a[0] + GD_FLAGS, GFLG_GADGHNONE | GFLG_STRINGEXTEND);
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.open(lvo, a[0]));
        auto *first = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, a[0], KAFFEINE_SHADOW_CLASSIC));
        auto *last = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, a[2], KAFFEINE_SHADOW_CLASSIC));
        IntuiMessage held{}, queued{};
        held.Class = IDCMP_GADGETDOWN; held.IAddress = first; held.IDCMPWindow = &f.window;
        queued.Class = IDCMP_GADGETUP; queued.IAddress = last; queued.IDCMPWindow = &f.window;
        queue_message(f.port, held);
        const ULONG port = f.lng(f.mw + M68K_WIN_USERPORT);
        const ULONG msg = f.call_at(Fixture::execbase, 372, { port });
        assert(msg && f.lng(msg + 28) == a[0]);
        queue_message(f.port, queued);
        assert(f.call_at(Fixture::intuition, 228, { f.mw, a[2] }) == 5 && native_blocks.size() == 6);
        const StringState out{ 51, 52, 53, 54, 55, -56 };
        string_state(*static_cast<StringInfo *>(first->SpecialInfo), out);
        // CloseWindow exports and frees an attached string and a retired integer.
        f.close();
        assert(guest_string_state(f, text.info) == out && guest_string_state(f, integer.info) == STRING_INITIAL &&
               !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
               f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap &&
               !f.system[2].NextGadget && !Kaffeine_ClassicGuest(&f.ctx, first) &&
               !Kaffeine_ClassicGuest(&f.ctx, last) && f.str(text.buffer) == "text");
        assert(replied_messages.size() == 2 && replied_messages[0] == &held.ExecMessage &&
               replied_messages[1] == &queued.ExecMessage);
    }
}

static void test_classic_string_cleanup_lifetime()
{
    ClassicFixture f;
    const auto a = own_chain(f, 2);
    const OwnString text = own_string(f, a[0]);
    own_string(f, a[1], 1, true);
    m68k_write16(&f.ctx, a[0] + GD_ACTIVATION, GACT_STRINGEXTEND);
    assert(f.add(a[0]) == 3);
    own_gadget(f, 0x4700);
    const OwnString other = own_string(f, 0x4700, 2);
    assert(f.add_one(0x4700, 0xFFFF, 1) == 3);
    IntuiMessage retired{};
    f.message(retired, f.copy(a[1]));
    assert(f.remove_one(a[1]) == 4 && native_blocks.size() == 6);
    own_gadget(f, 0x4740);
    own_string(f, 0x4740, 3);
    Gadget *pending = f.prepare(0x4740);
    assert(pending && !Kaffeine_ClassicOn(&f.ctx, 0x4740) && native_blocks.size() == 8);
    const StringState out{ -61, -62, -63, -64, -65, -66 };
    string_state(*string_info(f, a[0]), out);
    string_state(*string_info(f, 0x4700), out);
    Kaffeine_GadgetsCleanup(&f.ctx);
    assert(!f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
           guest_string_state(f, text.info) == out && guest_string_state(f, other.info) == out);
    for (ULONG guest : { a[0], a[1], 0x4700u, 0x4740u })
        assert(!Kaffeine_ShadowNative(&f.ctx, guest, KAFFEINE_SHADOW_CLASSIC) &&
               !Kaffeine_ClassicOn(&f.ctx, guest));
    assert(!Kaffeine_ClassicGuest(&f.ctx, pending) && closed_windows.empty() &&
           f.str(text.buffer) == "text" && f.str(other.buffer) == "text");
    f.only_system(0); f.only_system(1);
    empty_port(f.ports[0]);
}

static void test_classic_string_allocation_failures()
{
    for (bool one : { false, true })
        for (unsigned fail = 0; fail < (one ? 2u : 6u); ++fail)
        {
            ClassicFixture f;
            const auto a = own_chain(f);
            own_string(f, a[0]);
            own_string(f, a[2], 1, true);
            m68k_write16(&f.ctx, a[0] + GD_ACTIVATION, GACT_STRINGEXTEND);
            const unsigned calls = native_calls, allocations = native_allocations;
            const UWORD shadows = f.ctx.num_shadows;
            const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
            const auto memory = f.memory;
            allocations_before_failure = fail;
            assert((one ? f.add_one(a[0]) : f.add(a[0])) == 0xFFFF && !f.stopped());
            assert(native_calls == calls && native_allocations == allocations + fail && !f.ctx.classic &&
                   native_blocks.empty() && native_sizes.empty() && f.ctx.num_shadows == shadows &&
                   Kaffeine_HeapAvail(&f.ctx, 0) == heap && f.memory == memory);
            f.only_system(0);
            assert((one ? f.add_one(a[0]) : f.add(a[0])) == 3 && !f.stopped());
            assert((one ? f.remove_one(a[0]) : f.remove(a[0])) == 3 && native_blocks.empty());
        }
    for (ULONG lvo : { 606u, 204u })
        for (unsigned fail = 0; fail < 6; ++fail)
        {
            ClassicOpenFixture f;
            const auto a = own_chain(f);
            own_string(f, a[0]); own_string(f, a[2], 1, true);
            m68k_write16(&f.ctx, a[0] + GD_FLAGS, GFLG_GADGHNONE | GFLG_STRINGEXTEND);
            const UWORD shadows = f.ctx.num_shadows;
            const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
            allocations_before_failure = fail;
            assert(f.open(lvo, a[0]) && !f.stopped() && !f.ctx.classic &&
                   native_blocks.empty() && native_sizes.empty() && !f.system[2].NextGadget);
            for (const TagItem &tag : opened_tags) assert(tag.ti_Tag != WA_Gadgets);
            f.close();
            assert(f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
            assert(f.open(lvo, a[0]) && !f.stopped() && f.system[2].NextGadget);
            f.close();
            assert(!f.ctx.classic && native_blocks.empty() && f.ctx.num_shadows == shadows);
        }
    for (UWORD available : { (UWORD)0, (UWORD)1, (UWORD)2 })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        own_string(f, a[0]); own_string(f, a[2], 1, true);
        f.leave_shadows(available);
        const UWORD shadows = f.ctx.num_shadows;
        const unsigned calls = native_calls;
        const auto memory = f.memory;
        assert(f.add(a[0]) == 0xFFFF && !f.stopped() && native_calls == calls &&
               !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
               f.ctx.num_shadows == shadows && f.memory == memory);
        f.only_system(0);
    }
}

/* PropInfo and Image are NDK bytes, independently of the host layout.
   Distinct high/low bytes also expose swapped or misplaced UWORDs. */
static constexpr ULONG PROPINFO = 0x4B00;
static constexpr PropState PROP_INITIAL = {
    { AUTOKNOB | FREEHORIZ | 0xA400, 0x1234, 0xABCD, 0x2468, 0xFEDC,
      0x1357, 0x89AB, 0xCDEF, 0x9753, 0x8001, 0x7FFE },
    { -19, -23, 17, 2 }
};
static constexpr PropState PROP_OUTPUT = {
    { AUTOKNOB | FREEVERT | KNOBHIT, 0xFFFF, 0, 0x8000, 0x7FFF,
      0x1112, 0x2324, 0x3536, 0x4748, 0x595A, 0x6B6C },
    { -32768, 32767, 301, 109 }
};
struct OwnProp { ULONG info, knob; };
static PropState guest_prop_state(Fixture &f, const OwnProp &p)
{
    PropState s{};
    for (unsigned i = 0; i < s.info.size(); ++i) s.info[i] = f.word(p.info + 2 * i);
    if (p.knob)
        for (unsigned i = 0; i < s.place.size(); ++i) s.place[i] = (WORD)f.word(p.knob + 2 * i);
    return s;
}
static void guest_prop_state(Fixture &f, const OwnProp &p, const PropState &s)
{
    for (unsigned i = 0; i < s.info.size(); ++i) m68k_write16(&f.ctx, p.info + 2 * i, s.info[i]);
    if (p.knob)
        for (unsigned i = 0; i < s.place.size(); ++i) m68k_write16(&f.ctx, p.knob + 2 * i, (UWORD)s.place[i]);
}
/* Convert an existing own gadget, preserving its flags and links. */
static OwnProp own_prop(Fixture &f, ULONG gadget = OWN, unsigned slot = 0, bool automatic = true)
{
    assert(slot < 4);
    OwnProp p{ PROPINFO + slot * 64, IMAGE + slot * 64 };
    m68k_write16(&f.ctx, gadget + GD_GADGETTYPE, GTYP_PROPGADGET);
    m68k_write32(&f.ctx, gadget + GD_SPECIALINFO, p.info);
    m68k_write32(&f.ctx, gadget + GD_GADGETRENDER, p.knob);
    m68k_write32(&f.ctx, gadget + GD_SELECTRENDER, 0xFFFFFFF0); // Intuition never reads this
    own_image(f, p.knob);
    PropState s = PROP_INITIAL;
    if (!automatic) s.info[0] &= ~AUTOKNOB;
    guest_prop_state(f, p, s);
    return p;
}
static ULONG prop_call(ClassicFixture &f, ULONG lvo, ULONG gadget = OWN,
                       ULONG window = ClassicFixture::win0)
{
    return f.call_at(Fixture::intuition, lvo, { gadget, window, 0 }, { 1 });
}
static ULONG modify_call(Fixture &f, ULONG lvo, ULONG gadget, ULONG window,
                         ULONG num = 0x1234FFFE, ULONG requester = 0, ULONG flags = 0xCAFE0005)
{
    // Upper halves must not reach UWORD values or the signed WORD count.
    return f.call_at(Fixture::intuition, lvo, { gadget, window, requester },
                    { flags, 0xABCDFFFF, 0xDCBA0000, 0x12348000, 0x56787FFF, num });
}
static constexpr std::array<UWORD, 5> MODIFIED = { 5, 0xFFFF, 0, 0x8000, 0x7FFF };
static PropState modified_state(PropState s)
{
    std::copy(MODIFIED.begin(), MODIFIED.end(), s.info.begin());
    return s;
}
static void assert_prop_call(const PropCall &c, const char *name, Gadget *g, Window *w,
                              const PropState &s)
{
    assert(std::string(c.name) == name && c.gadget == g && c.window == w && c.state == s);
}

static void test_classic_prop_autoknob_copy()
{
    for (UWORD flags : { (UWORD)GFLG_GADGHIMAGE, (UWORD)(GFLG_GADGHIMAGE | GFLG_GADGIMAGE),
                         (UWORD)GFLG_GADGHNONE })
      for (unsigned unused = 0; unused < 4; ++unused)
    {
        Fixture f;
        own_gadget(f, OWN, 0, flags);
        const OwnProp p = own_prop(f);
        // These are deliberately unusable. AUTOKNOB needs only the header.
        if (unused == 1 || unused == 3) m68k_write16(&f.ctx, p.knob + IM_DEPTH, 0xFFFF);
        if (unused == 0 || unused == 3) m68k_write32(&f.ctx, p.knob + IM_IMAGEDATA, 0xFFFFFFF0);
        if (unused == 2 || unused == 3) m68k_write32(&f.ctx, p.knob + IM_NEXTIMAGE, 0xFFFFFFF0);
        const auto memory = f.memory;
        Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
        Gadget *g = c.head;
        assert(g->GadgetType == GTYP_PROPGADGET && g->Flags == flags && !g->SelectRender);
        assert(prop_state(g) == PROP_INITIAL && block_holding(g->SpecialInfo) == c.block);
        auto *im = static_cast<Image *>(g->GadgetRender);
        assert(im && block_holding(im) == c.block && !im->Depth && !im->ImageData &&
               !im->NextImage && !im->PlanePick && !im->PlaneOnOff && f.memory == memory);
    }
}

static void test_classic_prop_knob_chain_copy()
{
    Fixture f;
    own_gadget(f, OWN, 0, GFLG_GADGHIMAGE); // no GFLG_GADGIMAGE: the knob is still an Image
    const OwnProp p = own_prop(f, OWN, 0, false);
    own_image(f, p.knob, p.knob + IMAGE_SIZE);
    own_image(f, p.knob + IMAGE_SIZE);
    m68k_write16(&f.ctx, p.knob + IMAGE_SIZE + IM_LEFTEDGE, 32767);
    m68k_write16(&f.ctx, p.knob + IMAGE_SIZE + IM_TOPEDGE, 0x8000);
    m68k_write16(&f.ctx, p.knob + IMAGE_SIZE + IM_WIDTH, 31);
    m68k_write16(&f.ctx, p.knob + IMAGE_SIZE + IM_HEIGHT, 3);
    m68k_write16(&f.ctx, p.knob + IMAGE_SIZE + IM_DEPTH, 2);
    m68k_write32(&f.ctx, p.knob + IMAGE_SIZE + IM_IMAGEDATA, XY + 16);
    f.memory[p.knob + IMAGE_SIZE + IM_PLANEPICK] = 5;
    f.memory[p.knob + IMAGE_SIZE + IM_PLANEONOFF] = 0x82;
    for (unsigned i = 0; i < 40; ++i) f.memory[XY + i] = (UBYTE)(i * 7);
    const auto memory = f.memory;
    Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
    assert(!c.head->SelectRender && prop_state(c.head).info == guest_prop_state(f, p).info);
    Image *im = static_cast<Image *>(c.head->GadgetRender);
    for (ULONG guest : { p.knob, p.knob + IMAGE_SIZE })
    {
        assert(im && block_holding(im) == c.block && !f.guest(im));
        assert(im->LeftEdge == (WORD)f.word(guest) && im->TopEdge == (WORD)f.word(guest + 2) &&
               im->Width == (WORD)f.word(guest + IM_WIDTH) && im->Height == (WORD)f.word(guest + IM_HEIGHT) &&
               im->Depth == (WORD)f.word(guest + IM_DEPTH) &&
               im->PlanePick == f.memory[guest + IM_PLANEPICK] &&
               im->PlaneOnOff == f.memory[guest + IM_PLANEONOFF]);
        assert(im->ImageData == reinterpret_cast<UWORD *>(f.ctx.mem + f.lng(guest + IM_IMAGEDATA)));
        assert(f.guest(im->ImageData)); // the data stays in guest memory, including later edits
        const ULONG data = f.lng(guest + IM_IMAGEDATA);
        f.memory[data] ^= 0xFF;
        assert(reinterpret_cast<UBYTE *>(im->ImageData)[0] == f.memory[data]);
        f.memory[data] ^= 0xFF;
        im = im->NextImage;
    }
    assert(!im && f.memory == memory);
}

static void test_classic_prop_copy_boundaries()
{
    for (bool automatic : { false, true })
        for (bool no_knob : { false, true })
        {
            Fixture f;
            own_gadget(f);
            OwnProp p = own_prop(f, OWN, 0, automatic);
            const ULONG info = f.ctx.mem_size - PROPINFO_SIZE;
            std::memcpy(f.ctx.mem + info, f.ctx.mem + p.info, PROPINFO_SIZE);
            p.info = info;
            m68k_write32(&f.ctx, OWN + GD_SPECIALINFO, p.info);
            // Put Image exactly at the end in a separate copy pass, since
            // an Image and a PropInfo cannot occupy the same tail bytes.
            if (no_knob) { p.knob = 0; m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, 0); }
            {
                Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
                assert(prop_state(c.head) == guest_prop_state(f, p) && !c.head->SelectRender);
                auto *im = static_cast<Image *>(c.head->GadgetRender);
                assert(im && block_holding(im) == c.block);
                if (no_knob)
                    assert(!im->LeftEdge && !im->TopEdge && !im->Width && !im->Height && !im->Depth &&
                           !im->ImageData && !im->PlanePick && !im->PlaneOnOff && !im->NextImage);
            }
            if (!no_knob)
            {
                m68k_write32(&f.ctx, OWN + GD_SPECIALINFO, PROPINFO);
                const ULONG image = f.ctx.mem_size - IMAGE_SIZE;
                std::memcpy(f.ctx.mem + image, f.ctx.mem + p.knob, IMAGE_SIZE);
                p.info = PROPINFO; p.knob = image;
                m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, image);
                Copied<Gadget> c(f, OWN, Kaffeine_CopyGadget);
                assert(prop_state(c.head) == guest_prop_state(f, p));
            }
        }
}

enum class BadProp { NullInfo, InfoOutside, InfoShort, KnobOutside, KnobShort,
                     NextOutside, DataOutside, DataShort };
static constexpr BadProp bad_props[] = { BadProp::NullInfo, BadProp::InfoOutside, BadProp::InfoShort,
    BadProp::KnobOutside, BadProp::KnobShort, BadProp::NextOutside, BadProp::DataOutside, BadProp::DataShort };
static void bad_prop(Fixture &f, ULONG g, const OwnProp &p, BadProp bad)
{
    switch (bad)
    {
    case BadProp::NullInfo: m68k_write32(&f.ctx, g + GD_SPECIALINFO, 0); break;
    case BadProp::InfoOutside: m68k_write32(&f.ctx, g + GD_SPECIALINFO, 0xFFFFFFF0); break;
    case BadProp::InfoShort: m68k_write32(&f.ctx, g + GD_SPECIALINFO, f.ctx.mem_size - PROPINFO_SIZE + 1); break;
    case BadProp::KnobOutside: m68k_write32(&f.ctx, g + GD_GADGETRENDER, 0xFFFFFFF0); break;
    case BadProp::KnobShort: m68k_write32(&f.ctx, g + GD_GADGETRENDER, f.ctx.mem_size - IMAGE_SIZE + 1); break;
    case BadProp::NextOutside: m68k_write32(&f.ctx, p.knob + IM_NEXTIMAGE, 0xFFFFFFF0); break;
    case BadProp::DataOutside: m68k_write32(&f.ctx, p.knob + IM_IMAGEDATA, 0xFFFFFFF0); break;
    case BadProp::DataShort:
        // One plane of 17x2 needs eight bytes. Make it 17x1 (four),
        // with just three bytes left at an even address in odd-sized memory.
        m68k_write16(&f.ctx, p.knob + IM_HEIGHT, 1);
        m68k_write32(&f.ctx, p.knob + IM_IMAGEDATA, f.ctx.mem_size - 3);
        break;
    }
}
static bool bad_automatic(BadProp bad)
{
    return bad == BadProp::NextOutside || bad == BadProp::DataOutside || bad == BadProp::DataShort;
}
static void test_classic_prop_copy_refusals()
{
    for (bool automatic : { false, true })
        for (BadProp bad : bad_props)
        {
            if (automatic && bad_automatic(bad)) continue; // ignored AUTOKNOB fields, tested above
            Fixture f(0x10001);
            own_gadget(f);
            const OwnProp p = own_prop(f, OWN, 0, automatic);
            bad_prop(f, OWN, p, bad);
            const auto memory = f.memory;
            const unsigned allocations = native_allocations;
            KaffeineCopy c{};
            assert(!Kaffeine_CopyGadget(&f.ctx, &c, OWN) && c.why);
            assert(native_allocations == allocations && native_blocks.empty() && f.memory == memory);
            // Refusal is sticky; nothing can be added afterwards.
            assert(!Kaffeine_CopyGadget(&f.ctx, &c, OWN) && c.why && native_blocks.empty());
        }
}

static void test_classic_prop_run_refusals()
{
    for (bool automatic : { false, true })
        for (BadProp bad : bad_props)
        {
            if (automatic && bad_automatic(bad)) continue;
            ClassicFixture f(0x10001);
            const auto a = own_chain(f);
            const OwnProp p = own_prop(f, a[1], 0, automatic);
            bad_prop(f, a[1], p, bad);
            const auto memory = f.memory;
            const unsigned calls = native_calls;
            const UWORD shadows = f.ctx.num_shadows;
            const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
            assert(f.add(a[0]) == 20 && f.stopped());
            assert(native_calls == calls && !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
                   f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap && f.memory == memory);
            f.only_system(0);
            // Repair and retry: neither records, shadows nor pending marks survived.
            own_prop(f, a[1]);
            assert(f.add(a[0]) == 3 && !f.stopped() && f.copy(a[1]));
            assert(f.remove(a[0]) == 3 && !f.ctx.classic && native_blocks.empty());
        }
}

static void test_classic_prop_open_refusals()
{
    for (ULONG lvo : { 606u, 204u })
        for (bool automatic : { false, true })
            for (BadProp bad : bad_props)
            {
                if (automatic && (bad_automatic(bad))) continue;
                // The odd-sized data boundary is exercised by run_refusals.
                if (bad == BadProp::DataShort) continue;
                ClassicOpenFixture f;
                const auto a = own_chain(f);
                const OwnProp p = own_prop(f, a[1], 0, automatic);
                bad_prop(f, a[1], p, bad);
                const auto memory = f.memory;
                const unsigned calls = native_calls;
                const UWORD shadows = f.ctx.num_shadows;
                const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
                assert(f.open(lvo, a[0]) == 20 && f.stopped() && !f.mw);
                assert(native_calls == calls && !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
                       f.ctx.num_shadows == shadows && Kaffeine_HeapAvail(&f.ctx, 0) == heap &&
                       !f.system[2].NextGadget && opened_tags.empty());
                assert(std::equal(memory.begin() + OWN, memory.begin() + PROPINFO + PROPINFO_SIZE,
                                  f.memory.begin() + OWN));
            }
}

static void test_classic_prop_agreed_values()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp p = own_prop(f);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    assert(prop_state(g) == PROP_INITIAL && guest_prop_state(f, p) == PROP_INITIAL);
    PropState pending = PROP_OUTPUT;
    guest_prop_state(f, p, pending);
    const auto memory = f.memory;
    // Initial agreement includes every field: unchanged native values may
    // not erase any pending guest field, including output-only geometry.
    f.sync(0); f.sync(0);
    assert(f.memory == memory && prop_state(g) == PROP_INITIAL);
    watch_props = true;
    assert(prop_call(f, 432) == 0);
    PropState imported = PROP_INITIAL;
    std::copy_n(pending.info.begin(), 5, imported.info.begin());
    assert_prop_call(prop_calls.back(), "RefreshGList", g, &f.windows[0], imported);
    assert(guest_prop_state(f, p) == pending && prop_state(g) == imported);
    // Native reversals must be compared to the new agreement, not creation.
    prop_state(g, PROP_INITIAL);
    f.sync(0);
    PropState returned = pending;
    std::copy_n(PROP_INITIAL.info.begin(), 5, returned.info.begin());
    assert(guest_prop_state(f, p) == returned);
    assert(prop_call(f, 432) == 0 && prop_state(g) == PROP_INITIAL);
}

static void test_classic_prop_record_addresses()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp saved = own_prop(f);
    own_gadget(f, OWN + 64);
    const OwnProp replacement = own_prop(f, OWN + 64, 1);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    m68k_write32(&f.ctx, OWN + GD_SPECIALINFO, replacement.info);
    m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, replacement.knob);
    PropState input = PROP_INITIAL;
    input.info[1] = 0x5432;
    guest_prop_state(f, saved, input);
    assert(prop_call(f, 432) == 0 && prop_state(g) == input);
    prop_state(g, PROP_OUTPUT);
    f.sync(0);
    assert(guest_prop_state(f, saved) == PROP_OUTPUT && guest_prop_state(f, replacement) == PROP_INITIAL);
    assert(f.remove_one(OWN) == 3 && native_blocks.empty());
    // Reattachment takes the replacement addresses and starts a new agreement.
    assert(f.add_one(OWN) == 3 && prop_state(f.copy(OWN)) == PROP_INITIAL);
}

static void test_classic_prop_import_flags()
{
    for (unsigned program = 1; program < 16; ++program)
        for (unsigned intuition = 1; intuition < 16; ++intuition)
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            assert(f.add_one(OWN) == 3);
            Gadget *g = f.copy(OWN);
            PropState guest = PROP_INITIAL, native = PROP_INITIAL, merged = PROP_INITIAL;
            guest.info[0] ^= (UWORD)(1u << program);
            native.info[0] ^= (UWORD)(1u << intuition);
            merged.info[0] = guest.info[0] ^ (program == intuition ? 0 : (UWORD)(1u << intuition));
            guest_prop_state(f, p, guest); prop_state(g, native);
            watch_props = true;
            assert(prop_call(f, 432) == 0 && !f.stopped());
            assert_prop_call(prop_calls.back(), "RefreshGList", g, &f.windows[0], merged);
            assert(prop_state(g) == merged && guest_prop_state(f, p) == merged);
            assert(prop_call(f, 432) == 0 && prop_calls.back().state == merged);
        }
}

static void test_classic_prop_import_pots_and_bodies()
{
    for (unsigned program = 1; program <= 4; ++program)
        for (unsigned intuition = 1; intuition <= 4; ++intuition)
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            assert(f.add_one(OWN) == 3);
            Gadget *g = f.copy(OWN);
            PropState guest = PROP_INITIAL, native = PROP_INITIAL, merged = PROP_INITIAL;
            guest.info[program] = 0x00F0;
            native.info[intuition] = 0x0F00;
            merged.info[intuition] = native.info[intuition];
            merged.info[program] = guest.info[program]; // simultaneous whole-field changes: program wins on import
            guest_prop_state(f, p, guest); prop_state(g, native);
            watch_props = true;
            assert(prop_call(f, 432) == 0);
            assert(prop_calls.back().state == merged && prop_state(g) == merged && guest_prop_state(f, p) == merged);
            assert(prop_call(f, 432) == 0 && prop_calls.back().state == merged);
            // Reversing an imported whole value during the native call is exported.
            change_props = true; prop_after = PROP_INITIAL;
            assert(prop_call(f, 432) == 0 && guest_prop_state(f, p) == PROP_INITIAL);
        }
}

static void test_classic_prop_selected_imports()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp p = own_prop(f);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    g->Flags |= GFLG_SELECTED;
    PropState pending = PROP_INITIAL;
    std::copy_n(PROP_OUTPUT.info.begin(), 5, pending.info.begin());
    guest_prop_state(f, p, pending);
    watch_props = true;
    assert(prop_call(f, 432) == 0);
    assert(prop_calls.back().state == PROP_INITIAL && prop_state(g) == PROP_INITIAL &&
           (prop_calls.back().flags & GFLG_SELECTED) && guest_prop_state(f, p) == pending);
    // Exports still run during a drag, and preserve independent guest changes.
    auto *pi = static_cast<PropInfo *>(g->SpecialInfo);
    pi->Flags |= KNOBHIT;
    pi->VertPot = 0x3456;
    f.sync(0);
    pending.info[0] |= KNOBHIT; pending.info[2] = 0x3456;
    assert(guest_prop_state(f, p) == pending);
    g->Flags &= ~GFLG_SELECTED;
    assert(prop_call(f, 432) == 0 && prop_calls.back().state == pending &&
           prop_state(g) == pending && guest_prop_state(f, p) == pending);
    assert(!(f.word(OWN + GD_FLAGS) & GFLG_SELECTED));
}

static void test_classic_prop_export_flags()
{
    for (unsigned program = 1; program < 16; ++program)
        for (unsigned intuition = 1; intuition < 16; ++intuition)
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            assert(f.add_one(OWN) == 3);
            Gadget *g = f.copy(OWN);
            PropState guest = PROP_INITIAL, native = PROP_INITIAL, merged = PROP_INITIAL;
            guest.info[0] ^= (UWORD)(1u << program);
            native.info[0] ^= (UWORD)(1u << intuition);
            merged.info[0] = guest.info[0] ^ (program == intuition ? 0 : (UWORD)(1u << intuition));
            guest_prop_state(f, p, guest); prop_state(g, native);
            f.sync(0); // output only, including KNOBHIT (bit 8), in either direction
            assert(guest_prop_state(f, p) == merged && prop_state(g) == native);
            f.sync(0);
            assert(guest_prop_state(f, p) == merged);
            prop_state(g, PROP_INITIAL);
            f.sync(0);
            merged.info[0] ^= (UWORD)(1u << intuition);
            assert(guest_prop_state(f, p) == merged);
        }
}

static void test_classic_prop_export_fields()
{
    for (unsigned field = 1; field < 15; ++field)
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnProp p = own_prop(f);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        PropState guest = PROP_OUTPUT, native = PROP_INITIAL, merged = guest;
        // Leave just one native field changed; every other guest field is pending.
        if (field < 11) native.info[field] = merged.info[field] = 0xBA98;
        else native.place[field - 11] = merged.place[field - 11] = -12345;
        guest_prop_state(f, p, guest); prop_state(g, native);
        f.sync(0);
        assert(guest_prop_state(f, p) == merged && prop_state(g) == native);
        // The new agreed value must prevent an unchanged native field from
        // overwriting a subsequent guest edit, including output-only fields.
        guest_prop_state(f, p, guest);
        f.sync(0);
        assert(guest_prop_state(f, p) == guest);
        prop_state(g, PROP_INITIAL);
        f.sync(0);
        if (field < 11) guest.info[field] = PROP_INITIAL.info[field];
        else guest.place[field - 11] = PROP_INITIAL.place[field - 11];
        assert(guest_prop_state(f, p) == guest);
    }
}

static void test_classic_prop_open_state()
{
    for (ULONG lvo : { 606u, 204u })
    {
        ClassicOpenFixture f;
        const auto a = own_chain(f);
        const OwnProp p = own_prop(f, a[0]);
        const OwnProp tail = own_prop(f, a[2], 1);
        watch_props = change_props = true; prop_after = PROP_OUTPUT;
        assert(f.open(lvo, a[0]) && !f.stopped() && prop_calls.size() == 2);
        for (unsigned i = 0; i < 2; ++i)
        {
            auto *g = static_cast<Gadget *>(Kaffeine_ShadowNative(&f.ctx, a[i * 2], KAFFEINE_SHADOW_CLASSIC));
            assert_prop_call(prop_calls[i], "OpenWindowTagList", g, &f.window, PROP_INITIAL);
            assert(prop_state(g) == PROP_OUTPUT);
        }
        assert(guest_prop_state(f, p) == PROP_OUTPUT && guest_prop_state(f, tail) == PROP_OUTPUT);
        change_props = false;
        f.close();
        assert(native_blocks.empty() && !f.ctx.classic);
    }
}

static void test_classic_prop_add_state()
{
    for (bool one : { false, true })
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        const OwnProp p = own_prop(f, a[0]);
        const OwnProp tail = own_prop(f, a[2], 1);
        watch_props = change_props = true; prop_after = PROP_OUTPUT;
        assert((one ? f.add_one(a[0]) : f.add(a[0])) == 3 && !f.stopped());
        assert(prop_calls.size() == (one ? 1u : 2u));
        assert_prop_call(prop_calls[0], one ? "AddGadget" : "AddGList", f.copy(a[0]), &f.windows[0], PROP_INITIAL);
        assert(guest_prop_state(f, p) == PROP_OUTPUT && prop_state(f.copy(a[0])) == PROP_OUTPUT);
        assert(guest_prop_state(f, tail) == (one ? PROP_INITIAL : PROP_OUTPUT));
    }
}

static void test_classic_prop_call_state()
{
    for (ULONG lvo : { 432u, 222u, 186u, 174u, 462u })
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnProp p = own_prop(f);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        PropState input = PROP_INITIAL;
        std::copy_n(PROP_OUTPUT.info.begin(), 5, input.info.begin());
        guest_prop_state(f, p, input);
        // Revert the imported fields to their creation values natively,
        // while changing all geometry: both passes are observable.
        prop_after = PROP_OUTPUT;
        std::copy_n(PROP_INITIAL.info.begin(), 5, prop_after.info.begin());
        watch_props = change_props = true;
        const unsigned calls = native_calls;
        assert(prop_call(f, lvo) == (lvo == 462 ? TRUE : 0) && !f.stopped());
        assert(native_calls == calls + 1 && prop_calls.size() == 1);
        const char *name = lvo == 462 ? "ActivateGadget" : lvo == 186 ? "OnGadget" :
                           lvo == 174 ? "OffGadget" : "RefreshGList";
        assert_prop_call(prop_calls[0], name, g, &f.windows[0], input);
        assert(prop_state(g) == prop_after && guest_prop_state(f, p) == prop_after);
        if (lvo == 462)
            assert(passed_object == g && passed_window == &f.windows[0] && !passed_requester &&
                   (f.word(OWN + GD_ACTIVATION) & GACT_ACTIVEGADGET));
        else if (lvo == 186 || lvo == 174)
            assert(changed_object == g && changed_window == &f.windows[0] && !changed_requester &&
                   !!(f.word(OWN + GD_FLAGS) & GFLG_DISABLED) == (lvo == 174));
        else assert_gadget_call("RefreshGList", &f.windows[0], g, lvo == 432 ? 1 : -1, 0);
    }
}

static void test_classic_prop_message_state()
{
    for (bool gt : { false, true })
        for (ULONG cls : { (ULONG)IDCMP_GADGETUP, (ULONG)IDCMP_GADGETDOWN, (ULONG)IDCMP_MOUSEMOVE })
        {
            ClassicFixture f;
            own_gadget(f); own_gadget(f, OWN + 64);
            const OwnProp p = own_prop(f), other = own_prop(f, OWN + 64, 1);
            assert(f.add_one(OWN) == 3 && f.add_one(OWN + 64, 0xFFFF, 1) == 3);
            Gadget *g = f.copy(OWN), *gother = f.copy(OWN + 64);
            PropState pending = PROP_INITIAL, native = PROP_OUTPUT, merged = PROP_OUTPUT;
            pending.info[0] ^= FREEHORIZ; pending.info[1] = 0x4321;
            native.info[0] = PROP_INITIAL.info[0] | KNOBHIT; native.info[1] = PROP_INITIAL.info[1];
            merged.info[0] = pending.info[0] | KNOBHIT; merged.info[1] = pending.info[1];
            guest_prop_state(f, p, pending); prop_state(g, native); prop_state(gother, PROP_OUTPUT);
            IntuiMessage im{};
            f.message(im, cls == IDCMP_MOUSEMOVE ? nullptr : g, cls);
            const ULONG msg = gt ? f.call(72, { f.port() }) : f.call_at(Fixture::execbase, 372, { f.port() });
            assert(msg && !f.stopped() && f.lng(msg + 20) == cls &&
                   f.lng(msg + 28) == (cls == IDCMP_MOUSEMOVE ? 0 : OWN));
            assert(guest_prop_state(f, p) == merged && prop_state(g) == native &&
                   guest_prop_state(f, other) == PROP_INITIAL && prop_state(gother) == PROP_OUTPUT);
            assert(prop_call(f, 432) == 0 && prop_state(g) == merged);
            if (gt) assert(f.call(78, { 0, msg }) == 0);
            else assert(f.call_at(Fixture::execbase, 378, { 0, msg }) == 0);
            assert(replied_messages.back() == &im.ExecMessage);
        }
}

static void test_classic_prop_remove_state()
{
    for (bool one : { false, true })
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnProp p = own_prop(f);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        g->Flags |= GFLG_SELECTED;
        watch_props = change_props = remove_deselects = true; prop_after = PROP_OUTPUT;
        const unsigned calls = native_calls;
        const UWORD shadows = f.ctx.num_shadows;
        assert((one ? f.remove_one(OWN) : f.remove(OWN)) == 3 && !f.stopped());
        assert(native_calls == calls + 1 && prop_calls.size() == 1);
        assert_prop_call(prop_calls[0], "RemoveGList", g, &f.windows[0], PROP_INITIAL);
        assert(guest_prop_state(f, p) == PROP_OUTPUT && !(f.word(OWN + GD_FLAGS) & GFLG_SELECTED));
        assert((f.word(p.info + PI_FLAGS) & KNOBHIT) && !f.ctx.classic && native_blocks.empty() &&
               f.ctx.num_shadows == shadows - 1);
        f.only_system(0);
    }
}

static void test_classic_prop_initial_selection()
{
    for (bool one : { false, true })
    {
        ClassicFixture f;
        const UWORD base = GFLG_GADGHNONE | GFLG_DISABLED;
        own_gadget(f, OWN, 0, base | GFLG_SELECTED);
        const OwnProp p = own_prop(f);
        const auto memory = f.memory;
        Gadget *prepared = f.prepare(OWN);
        assert(prepared && prepared->Flags == base && prop_state(prepared) == PROP_INITIAL && f.memory == memory);
        Kaffeine_ClassicDropped(&f.ctx);
        assert(!f.ctx.classic && native_blocks.empty() && f.memory == memory);
        watch_props = true;
        assert((one ? f.add_one(OWN) : f.add(OWN)) == 3);
        assert(prop_calls.size() == 1 && prop_calls[0].flags == base &&
               f.copy(OWN)->Flags == base && f.word(OWN + GD_FLAGS) == base);
        PropState pending = PROP_INITIAL;
        std::copy_n(PROP_OUTPUT.info.begin(), 5, pending.info.begin());
        guest_prop_state(f, p, pending);
        assert(prop_call(f, 432) == 0 && prop_calls.back().state == pending);
    }
}

static void test_classic_modify_prop_window()
{
    for (ULONG lvo : { 156u, 468u })
        for (ULONG num : { 0u, 1u, 0x1234FFFEu, 0xABCDEFFFu })
          for (unsigned drawing_window : { 0u, 1u })
        {
            ClassicFixture f;
            const auto a = own_chain(f, 4);
            const OwnProp p = own_prop(f, a[0]), tail = own_prop(f, a[3], 1);
            const OwnString s = own_string(f, a[2]);
            own_gadget(f, OWN + 256);
            const OwnProp elsewhere = own_prop(f, OWN + 256, 2);
            assert(f.add(a[0]) == 3 && f.add_one(OWN + 256, 0xFFFF, 1) == 3);
            Gadget *g = f.copy(a[0]), *gtail = f.copy(a[3]), *gother = f.copy(OWN + 256);
            PropState pending = PROP_INITIAL;
            pending.info[0] ^= FREEVERT; pending.info[1] = 0x1000;
            guest_prop_state(f, p, pending); guest_prop_state(f, tail, pending);
            guest_prop_state(f, elsewhere, pending);
            m68k_write16(&f.ctx, a[1] + GD_FLAGS, GFLG_GADGHNONE | GFLG_DISABLED);
            const StringState text{ 3, 1, STRING_INITIAL.undo, STRING_INITIAL.chars, STRING_INITIAL.count, -987654 };
            guest_string_state(f, s.info, text);
            watch_props = change_props = watch_strings = true; prop_after = PROP_OUTPUT;
            const unsigned calls = native_calls;
            assert(modify_call(f, lvo, a[0], f.address(drawing_window), num, 0xFFFFFFF0) == 0 && !f.stopped());
            assert(native_calls == calls + 1 && modify_calls.size() == 1 && prop_calls.size() == 2);
            const ModifyPropCall &c = modify_calls[0];
            const char *name = lvo == 156 ? "ModifyProp" : "NewModifyProp";
            assert(std::string(c.name) == name && c.gadget == g && c.window == &f.windows[drawing_window] &&
                   !c.requester && c.values == MODIFIED && c.num == (lvo == 156 ? -1 : (WORD)num));
            assert_prop_call(prop_calls[0], name, g, &f.windows[drawing_window], pending);
            assert_prop_call(prop_calls[1], name, gtail, &f.windows[drawing_window], pending);
            assert(string_calls.size() == 1 && string_calls[0].gadget == f.copy(a[2]) && string_calls[0].state == text);
            const auto boolean = std::find_if(modify_list_saw.begin(), modify_list_saw.end(),
                                              [&](const GadgetState &v) { return v.gadget == f.copy(a[1]); });
            assert(boolean != modify_list_saw.end() && (boolean->flags & GFLG_DISABLED));
            assert(prop_state(g) == modified_state(PROP_OUTPUT) && guest_prop_state(f, p) == modified_state(PROP_OUTPUT));
            assert(prop_state(gtail) == PROP_OUTPUT && guest_prop_state(f, tail) == PROP_OUTPUT);
            assert(prop_state(gother) == PROP_INITIAL && guest_prop_state(f, elsewhere) == pending);
            change_props = false;
            // The post-call sync advanced the agreement; a later import is stable.
            assert(prop_call(f, 432) == 0 && prop_state(g) == modified_state(PROP_OUTPUT));
        }
}

static void test_classic_modify_prop_elsewhere()
{
    for (ULONG lvo : { 156u, 468u })
        for (bool removed : { false, true })
          for (unsigned drawing_window : { 0u, 1u })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            if (removed)
            {
                assert(f.add_one(OWN) == 3);
                assert(f.remove_one(OWN) == 3);
            }
            const unsigned calls = native_calls;
            const auto blocks = native_blocks;
            const UWORD shadows = f.ctx.num_shadows;
            const PropState result = modified_state(PROP_INITIAL);
            guest_prop_state(f, p, result);
            const auto expected = f.memory;
            guest_prop_state(f, p, PROP_INITIAL);
            assert(modify_call(f, lvo, OWN, f.address(drawing_window)) == 0 && !f.stopped());
            assert(native_calls == calls && modify_calls.empty() && f.memory == expected &&
                   native_blocks == blocks && f.ctx.num_shadows == shadows);
        }
}

static void test_classic_modify_prop_foreign_window()
{
    for (ULONG lvo : { 156u, 468u })
        for (ULONG w : { 0xFFFFFFF0u, Fixture::raw, 0x3B90u })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            Window foreign{};
            assert(Kaffeine_ShadowAdd(&f.ctx, 0x3B90, &foreign, FILLER));
            const auto memory = f.memory;
            const unsigned calls = native_calls;
            assert(modify_call(f, lvo, OWN, w) == 20 && f.stopped() && f.state.d[0] == 20);
            assert(native_calls == calls && modify_calls.empty() && !f.ctx.classic &&
                   native_blocks.empty() && f.memory == memory && guest_prop_state(f, p) == PROP_INITIAL);
            assert(!debug_lines.empty() && debug_lines.back().find("not a window of the program") != std::string::npos);
        }
}

static void test_classic_modify_prop_gadtools()
{
    for (ULONG lvo : { 156u, 468u })
        for (ULONG kind : { (ULONG)BUTTON_KIND, (ULONG)SLIDER_KIND, (ULONG)SCROLLER_KIND })
          for (unsigned drawing_window : { 0u, 1u })
        {
            ClassicFixture f;
            const ULONG c = f.context();
            f.newgad(10, 20, 120, 14, 0, 0, 1, 0, f.vi, 0);
            f.tagitems({});
            const ULONG gadget = f.create(kind, c);
            assert(gadget && !f.stopped());
            own_gadget(f);
            const OwnProp p = own_prop(f);
            assert(f.add_one(OWN) == 3);
            prop_state(f.copy(OWN), PROP_OUTPUT);
            Requester requester{};
            assert(Kaffeine_ShadowAdd(&f.ctx, 0x3BA0, &requester, FILLER));
            const auto memory = f.memory;
            const unsigned calls = native_calls;
            const auto blocks = native_blocks;
            assert(modify_call(f, lvo, gadget, f.address(drawing_window), 0x1234FFFE, 0x3BA0) == 0 && !f.stopped());
            assert(native_calls == calls + 1 && modify_calls.size() == 1 && f.memory == memory && native_blocks == blocks);
            const ModifyPropCall &seen = modify_calls[0];
            assert(std::string(seen.name) == (lvo == 156 ? "ModifyProp" : "NewModifyProp") &&
                   seen.gadget == native_gadget(f, gadget) && seen.window == &f.windows[drawing_window] && !seen.requester &&
                   seen.values == MODIFIED && seen.num == (lvo == 156 ? -1 : -2));
            assert(guest_prop_state(f, p) == PROP_INITIAL && prop_state(f.copy(OWN)) == PROP_OUTPUT);
        }
}

static void test_classic_modify_prop_ignored()
{
    for (ULONG lvo : { 156u, 468u })
        for (unsigned scenario = 0; scenario < 10; ++scenario)
        {
            ClassicFixture f(0x10001, true);
            own_gadget(f);
            const OwnProp p = own_prop(f);
            ULONG gadget = OWN;
            switch (scenario)
            {
            case 0: // Boolean with a valid, tempting SpecialInfo: never write it.
            case 1:
                m68k_write16(&f.ctx, OWN + GD_GADGETTYPE, GTYP_BOOLGADGET);
                m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, 0);
                if (scenario == 1) assert(f.add_one(OWN) == 3);
                break;
            case 2: gadget = 0; break;
            case 3: gadget = 0xFFFFFFF0; break;
            case 4: gadget = f.ctx.mem_size - 43; break;
            case 5: gadget = Fixture::raw; break;
            case 6: bad_prop(f, OWN, p, BadProp::NullInfo); break;
            case 7: bad_prop(f, OWN, p, BadProp::InfoOutside); break;
            case 8: bad_prop(f, OWN, p, BadProp::InfoShort); break;
            case 9: m68k_write16(&f.ctx, OWN + GD_GADGETTYPE, GTYP_CUSTOMGADGET); break;
            }
            const auto memory = f.memory;
            const unsigned calls = native_calls;
            const size_t lines = debug_lines.size();
            const auto blocks = native_blocks;
            assert(modify_call(f, lvo, gadget, f.win0) == 0 && !f.stopped());
            assert(native_calls == calls && modify_calls.empty() && f.memory == memory && native_blocks == blocks);
            assert(debug_lines.size() == lines + 1 &&
                   debug_lines.back().find(lvo == 156 ? "ModifyProp(" : "NewModifyProp(") != std::string::npos &&
                   debug_lines.back().find("not a proportional gadget of the program") != std::string::npos);
        }
}

static void test_classic_prop_allocation_failures()
{
    for (unsigned fail = 0; fail < 6; ++fail)
    {
        ClassicFixture f;
        const auto a = own_chain(f);
        own_prop(f, a[0]); own_prop(f, a[2], 1, false);
        const auto memory = f.memory;
        const unsigned calls = native_calls;
        const UWORD shadows = f.ctx.num_shadows;
        const ULONG heap = Kaffeine_HeapAvail(&f.ctx, 0);
        allocations_before_failure = fail;
        assert(f.add(a[0]) == 0xFFFF && !f.stopped());
        assert(native_calls == calls && !f.ctx.classic && native_blocks.empty() && native_sizes.empty() &&
               f.ctx.num_shadows == shadows && f.memory == memory && Kaffeine_HeapAvail(&f.ctx, 0) == heap);
        assert(f.add(a[0]) == 3 && f.remove(a[0]) == 3 && native_blocks.empty());
    }
}

static void test_classic_prop_empty_knob_state()
{
    for (bool automatic : { false, true })
    {
        ClassicFixture f;
        own_gadget(f);
        OwnProp p = own_prop(f, OWN, 0, automatic);
        p.knob = 0;
        m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, 0);
        const std::vector<UBYTE> zero(f.memory.begin(), f.memory.begin() + 64);
        const std::vector<UBYTE> unused(f.memory.begin() + IMAGE, f.memory.begin() + IMAGE + IMAGE_SIZE);
        PropState initial = guest_prop_state(f, p);
        watch_props = change_props = true;
        prop_after = PROP_OUTPUT;
        prop_after.info[0] = (prop_after.info[0] & ~AUTOKNOB) | (initial.info[0] & AUTOKNOB);
        assert(f.add_one(OWN) == 3 && !f.stopped() && prop_calls.size() == 1);
        Gadget *g = f.copy(OWN);
        assert_prop_call(prop_calls[0], "AddGadget", g, &f.windows[0], initial);
        auto *im = static_cast<Image *>(g->GadgetRender);
        assert(im && block_holding(im) == block_holding(g) && !im->Depth && !im->ImageData &&
               !im->PlanePick && !im->PlaneOnOff && !im->NextImage);
        for (unsigned pass = 0; pass < 3; ++pass)
        {
            prop_after.place[pass] = (WORD)(-1234 - pass);
            assert(prop_call(f, 432) == 0 && prop_state(g) == prop_after);
            PropState guest = prop_after;
            guest.place = {};
            assert(guest_prop_state(f, p) == guest && !f.lng(OWN + GD_GADGETRENDER));
            assert(std::equal(zero.begin(), zero.end(), f.memory.begin()));
            assert(std::equal(unused.begin(), unused.end(), f.memory.begin() + IMAGE));
        }
        assert(f.remove_one(OWN) == 3 && native_blocks.empty());
        assert(std::equal(zero.begin(), zero.end(), f.memory.begin()));
    }
}

static void test_classic_prop_autoknob_import()
{
    for (bool automatic : { false, true })
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnProp p = own_prop(f, OWN, 0, automatic);
        const PropState initial = guest_prop_state(f, p);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        const Image image = *static_cast<Image *>(g->GadgetRender);
        PropState input = initial, native = initial;
        input.info[0] ^= AUTOKNOB | FREEVERT;
        input.info[1] = 0x5432;
        native.info[0] |= KNOBHIT;
        guest_prop_state(f, p, input); prop_state(g, native);
        PropState merged = input;
        merged.info[0] |= KNOBHIT;
        PropState imported = merged;
        imported.info[0] = (imported.info[0] & ~AUTOKNOB) | (initial.info[0] & AUTOKNOB);
        watch_props = true;
        for (unsigned pass = 0; pass < 3; ++pass)
        {
            assert(prop_call(f, 432) == 0 && !f.stopped());
            assert_prop_call(prop_calls.back(), "RefreshGList", g, &f.windows[0], imported);
            assert(prop_state(g) == imported && guest_prop_state(f, p) == merged);
            auto *im = static_cast<Image *>(g->GadgetRender);
            assert(im->Width == image.Width && im->Height == image.Height && im->Depth == image.Depth &&
                   im->ImageData == image.ImageData && im->NextImage == image.NextImage);
        }
    }
}

static void test_classic_prop_autoknob_export()
{
    for (bool automatic : { false, true })
        for (bool guest_changes : { false, true })
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f, OWN, 0, automatic);
            const PropState initial = guest_prop_state(f, p);
            assert(f.add_one(OWN) == 3);
            Gadget *g = f.copy(OWN);
            PropState guest = initial, native = initial;
            if (guest_changes) guest.info[0] ^= AUTOKNOB;
            guest.info[1] = 0x6543;
            native.info[0] ^= AUTOKNOB;
            native.info[0] |= KNOBHIT;
            native.info[2] = 0x7654;
            guest_prop_state(f, p, guest); prop_state(g, native);
            guest.info[0] |= KNOBHIT; guest.info[2] = native.info[2];
            f.sync(0);
            assert(guest_prop_state(f, p) == guest && prop_state(g) == native);
            // A reversal of AUTOKNOB alone must not write anything to the program.
            native.info[0] ^= AUTOKNOB;
            prop_state(g, native);
            const auto memory = f.memory;
            f.sync(0); f.sync(0);
            assert(f.memory == memory && guest_prop_state(f, p) == guest);
        }
}

static void test_classic_prop_autoknob_reattach()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp p = own_prop(f, OWN, 0, false);
    own_image(f, p.knob, p.knob + IMAGE_SIZE);
    own_image(f, p.knob + IMAGE_SIZE);
    assert(f.add_one(OWN) == 3);
    for (bool automatic : { true, false })
    {
        Gadget *g = f.copy(OWN);
        m68k_write16(&f.ctx, p.info + PI_FLAGS,
                     automatic ? f.word(p.info) | AUTOKNOB : f.word(p.info) & ~AUTOKNOB);
        assert(prop_call(f, 432) == 0);
        assert(!!(prop_state(g).info[0] & AUTOKNOB) != automatic);
        auto *im = static_cast<Image *>(g->GadgetRender);
        assert(!!im->ImageData == automatic && !!im->NextImage == automatic);
        assert(f.remove_one(OWN) == 3 && native_blocks.empty());
        assert(f.add_one(OWN) == 3);
        g = f.copy(OWN);
        im = static_cast<Image *>(g->GadgetRender);
        assert(!!(prop_state(g).info[0] & AUTOKNOB) == automatic &&
               !!im->ImageData != automatic && !!im->NextImage != automatic);
        if (!automatic)
            assert(im->ImageData == reinterpret_cast<UWORD *>(f.ctx.mem + XY) && im->NextImage->ImageData == im->ImageData);
    }
}

static void test_classic_prop_import_critical_section()
{
    for (unsigned selection : { 0u, 1u, 2u }) // clear, selected at Forbid entry, already selected
    {
        ClassicFixture f;
        own_gadget(f);
        const OwnProp p = own_prop(f);
        assert(f.add_one(OWN) == 3);
        Gadget *g = f.copy(OWN);
        if (selection == 2) g->Flags |= GFLG_SELECTED;
        PropState input = PROP_INITIAL, later = PROP_INITIAL;
        std::copy_n(PROP_OUTPUT.info.begin(), 5, input.info.begin());
        later.info[0] ^= FREEVERT;
        for (unsigned i = 1; i < 5; ++i) later.info[i] ^= 0x5555;
        guest_prop_state(f, p, input);
        const unsigned before_forbid = forbid_calls, before_permit = permit_calls;
        unsigned entries = 0, exits = 0;
        {
            SchedulerProbe probe([&](bool entering) {
                assert(forbid_depth == 1);
                if (entering)
                {
                    assert(++entries == 1 && prop_state(g) == PROP_INITIAL);
                    // The bridge must already have read all five guest values.
                    guest_prop_state(f, p, later);
                    if (selection == 1) g->Flags |= GFLG_SELECTED;
                }
                else
                    assert(++exits == 1 && prop_state(g) == (selection ? PROP_INITIAL : input));
            });
            Kaffeine_ClassicRefreshing(&f.ctx, &f.windows[0], g, 1);
            assert(entries == 1 && exits == 1 && !forbid_depth);
        }
        assert(forbid_calls == before_forbid + 1 && permit_calls == before_permit + 1 &&
               prop_state(g) == (selection ? PROP_INITIAL : input) && guest_prop_state(f, p) == later);
    }
}

static void test_classic_prop_sample_critical_section()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp p = own_prop(f);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    PropState later = PROP_INITIAL;
    for (unsigned i = 0; i < later.info.size(); ++i) later.info[i] ^= 0x5550;
    later.place = { -112, -113, 52, 9 };
    const unsigned before_forbid = forbid_calls, before_permit = permit_calls;
    unsigned entries = 0, exits = 0;
    {
        SchedulerProbe probe([&](bool entering) {
            assert(forbid_depth == 1);
            if (entering)
            {
                assert(++entries == 1);
                prop_state(g, PROP_OUTPUT);
            }
            else
            {
                assert(++exits == 1 && prop_state(g) == PROP_OUTPUT && guest_prop_state(f, p) == PROP_INITIAL);
                // After the protected sample, the input handler moves both
                // structures again. Every exported field must use that sample.
                prop_state(g, later);
            }
        });
        f.sync(0);
        assert(entries == 1 && exits == 1 && !forbid_depth);
    }
    assert(forbid_calls == before_forbid + 1 && permit_calls == before_permit + 1 &&
           guest_prop_state(f, p) == PROP_OUTPUT && prop_state(g) == later);
    f.sync(0);
    assert(guest_prop_state(f, p) == later);
}

static void test_classic_prop_critical_nesting()
{
    ClassicFixture f;
    own_gadget(f);
    const OwnProp p = own_prop(f);
    assert(f.add_one(OWN) == 3);
    Gadget *g = f.copy(OWN);
    PropState input = PROP_INITIAL;
    input.info[1] = 0x2345;
    guest_prop_state(f, p, input);
    unsigned entries = 0, exits = 0;
    Forbid();
    {
        SchedulerProbe probe([&](bool entering) {
            assert(forbid_depth == 2);
            if (entering) ++entries;
            else ++exits;
        });
        Kaffeine_ClassicRefreshing(&f.ctx, &f.windows[0], g, 1);
        assert(forbid_depth == 1 && prop_state(g) == input);
        f.sync(0);
        assert(forbid_depth == 1 && entries == 2 && exits == 2);
    }
    Permit();
    assert(!forbid_depth && forbid_peak == 2 && forbid_calls == permit_calls);
}

static void test_classic_modify_prop_autoknob()
{
    for (ULONG lvo : { 156u, 468u })
        for (bool automatic : { false, true })
            for (unsigned drawing_window : { 0u, 1u })
            {
                ClassicFixture f;
                own_gadget(f);
                const OwnProp p = own_prop(f, OWN, 0, automatic);
                const PropState initial = guest_prop_state(f, p);
                assert(f.add_one(OWN) == 3);
                Gadget *g = f.copy(OWN);
                const Image image = *static_cast<Image *>(g->GadgetRender);
                const UWORD given_flags = FREEVERT | KNOBHIT | (automatic ? 0 : AUTOKNOB);
                auto values = MODIFIED;
                values[0] = (given_flags & ~AUTOKNOB) | (automatic ? AUTOKNOB : 0);
                watch_props = true;
                assert(modify_call(f, lvo, OWN, f.address(drawing_window), 0x1234FFFE, 0xFFFFFFF0,
                                   0xCAFE0000 | given_flags) == 0 && !f.stopped());
                assert(modify_calls.size() == 1 && modify_calls[0].gadget == g &&
                       modify_calls[0].window == &f.windows[drawing_window] && !modify_calls[0].requester &&
                       modify_calls[0].values == values && modify_calls[0].num == (lvo == 156 ? -1 : -2));
                PropState native = modified_state(initial), guest = native;
                native.info[0] = values[0]; guest.info[0] = given_flags;
                assert(prop_state(g) == native && guest_prop_state(f, p) == guest);
                for (unsigned pass = 0; pass < 2; ++pass)
                    assert(prop_call(f, 432) == 0 && prop_state(g) == native && guest_prop_state(f, p) == guest);
                auto *im = static_cast<Image *>(g->GadgetRender);
                assert(im->Width == image.Width && im->Height == image.Height && im->Depth == image.Depth &&
                       im->ImageData == image.ImageData && im->NextImage == image.NextImage);
            }
}

static void test_classic_modify_prop_selected_agreement()
{
    for (ULONG lvo : { 156u, 468u })
        for (bool automatic : { false, true })
            for (unsigned field = 0; field < 6; ++field) // each shared field, then all five
            {
                ClassicFixture f;
                own_gadget(f);
                const OwnProp p = own_prop(f, OWN, 0, automatic);
                const PropState initial = guest_prop_state(f, p);
                assert(f.add_one(OWN) == 3);
                Gadget *g = f.copy(OWN);
                g->Flags |= GFLG_SELECTED;
                f.sync(0);
                PropState pending = initial;
                for (unsigned i = 0; i < 5; ++i)
                    if (field == i || field == 5) pending.info[i] ^= i ? 0x5A5A : FREEVERT;
                guest_prop_state(f, p, pending);
                UWORD flags = initial.info[0];
                if (field == 0 || field == 5) flags ^= AUTOKNOB;
                watch_props = true;
                assert(f.call_at(Fixture::intuition, lvo, { OWN, f.win1, 0xFFFFFFF0 },
                                 { flags, initial.info[1], initial.info[2], initial.info[3], initial.info[4], 1 }) == 0);
                assert(!f.stopped() && prop_calls.size() == 1 && prop_calls[0].state == initial &&
                       (prop_calls[0].flags & GFLG_SELECTED) && prop_state(g) == initial);
                PropState agreed = initial;
                agreed.info[0] = flags;
                assert(guest_prop_state(f, p) == agreed); // even if native values equalled the old agreement
                g->Flags &= ~GFLG_SELECTED;
                assert(prop_call(f, 432) == 0 && prop_state(g) == initial && guest_prop_state(f, p) == agreed);
            }
}

static void test_classic_modify_prop_geometry_agreement()
{
    for (ULONG lvo : { 156u, 468u })
        for (bool copy_auto : { false, true })
            for (bool given_auto : { false, true })
                for (bool has_knob : { false, true })
                    for (unsigned field = 0; field < 11; ++field) // six calculated fields, four Image fields, then all
                    {
                        ClassicFixture f;
                        own_gadget(f);
                        OwnProp p = own_prop(f, OWN, 0, copy_auto);
                        if (!has_knob)
                        {
                            p.knob = 0;
                            m68k_write32(&f.ctx, OWN + GD_GADGETRENDER, 0);
                        }
                        assert(f.add_one(OWN) == 3);
                        Gadget *g = f.copy(OWN);
                        const PropState initial = prop_state(g);
                        PropState worked = PROP_OUTPUT;
                        worked.info[0] = (worked.info[0] & ~AUTOKNOB) | (copy_auto ? AUTOKNOB : 0);
                        if (!copy_auto)
                        {
                            // Intuition places a custom knob without resizing its checked data.
                            worked.place[2] = initial.place[2];
                            worked.place[3] = initial.place[3];
                        }
                        prop_state(g, worked);
                        f.sync(0); // advance every agreed value beyond the creation snapshot
                        PropState pending = guest_prop_state(f, p);
                        for (unsigned i = 0; i < 6; ++i)
                            if (field == i || field == 10) pending.info[5 + i] = 0;
                        for (unsigned i = 0; i < 4; ++i)
                            if (field == 6 + i || field == 10) pending.place[i] = 0;
                        guest_prop_state(f, p, pending);
                        const std::vector<UBYTE> zero(f.memory.begin(), f.memory.begin() + 64);
                        const UWORD flags = (worked.info[0] & ~AUTOKNOB) | (given_auto ? AUTOKNOB : 0);
                        watch_props = change_props = true;
                        prop_after = worked; // native rendering works out exactly the same geometry again
                        const unsigned calls = native_calls;
                        assert(f.call_at(Fixture::intuition, lvo, { OWN, f.win0, 0xFFFFFFF0 },
                                         { flags, worked.info[1], worked.info[2], worked.info[3], worked.info[4],
                                           0x12340001 }) == 0 && !f.stopped());
                        assert(native_calls == calls + 1 && prop_calls.size() == 1 && modify_calls.size() == 1);
                        assert_prop_call(prop_calls[0], lvo == 156 ? "ModifyProp" : "NewModifyProp",
                                         g, &f.windows[0], worked);
                        const ModifyPropCall &call = modify_calls[0];
                        assert(call.gadget == g && call.window == &f.windows[0] && !call.requester &&
                               call.values[0] == worked.info[0] && call.num == (lvo == 156 ? -1 : 1));
                        assert(prop_state(g) == worked);
                        PropState expected = pending;
                        expected.info = worked.info;
                        expected.info[0] = flags;
                        if (has_knob)
                        {
                            expected.place[0] = worked.place[0];
                            expected.place[1] = worked.place[1];
                            if (copy_auto && given_auto)
                            {
                                expected.place[2] = worked.place[2];
                                expected.place[3] = worked.place[3];
                            }
                        }
                        else expected.place = {};
                        assert(guest_prop_state(f, p) == expected);
                        assert(f.lng(OWN + GD_GADGETRENDER) == p.knob &&
                               std::equal(zero.begin(), zero.end(), f.memory.begin()));
                    }
}

static void test_classic_modify_prop_null_window()
{
    for (ULONG lvo : { 156u, 468u })
        for (unsigned kind = 0; kind < 9; ++kind)
        {
            ClassicFixture f;
            own_gadget(f);
            const OwnProp p = own_prop(f);
            ULONG gadget = OWN;
            Gadget *copy = nullptr;
            switch (kind)
            {
            case 0: break; // own proportional gadget on no window
            case 1:
                assert(f.add_one(OWN) == 3);
                copy = f.copy(OWN);
                prop_state(copy, PROP_OUTPUT);
                break;
            case 2: m68k_write16(&f.ctx, OWN + GD_GADGETTYPE, GTYP_BOOLGADGET); break;
            case 3: gadget = 0xFFFFFFF0; break;
            case 4: gadget = 0; break;
            case 5:
            case 6:
            case 7:
            {
                const ULONG context = f.context();
                f.newgad(10, 20, 120, 14, 0, 0, 1, 0, f.vi, 0);
                f.tagitems({});
                gadget = f.create(kind == 5 ? BUTTON_KIND : kind == 6 ? SLIDER_KIND : SCROLLER_KIND, context);
                assert(gadget && !f.stopped());
                break;
            }
            case 8: bad_prop(f, OWN, p, BadProp::InfoOutside); break;
            }
            const auto memory = f.memory;
            const auto lines = debug_lines;
            const auto blocks = native_blocks;
            const unsigned calls = native_calls, forbids = forbid_calls, permits = permit_calls;
            const UWORD shadows = f.ctx.num_shadows;
            assert(modify_call(f, lvo, gadget, 0, 0x1234FFFE, 0xFFFFFFF0) == 0 && f.ctx.running && !f.ctx.exit_code);
            assert(native_calls == calls && modify_calls.empty() && debug_lines == lines && f.memory == memory &&
                   native_blocks == blocks && f.ctx.num_shadows == shadows && forbid_calls == forbids &&
                   permit_calls == permits && !forbid_depth);
            if (copy) assert(prop_state(copy) == PROP_OUTPUT);
        }
}

static void test_classic_modify_prop_gadtools_foreign_window()
{
    for (ULONG lvo : { 156u, 468u })
        for (ULONG kind : { (ULONG)BUTTON_KIND, (ULONG)SLIDER_KIND, (ULONG)SCROLLER_KIND })
            for (ULONG window : { 0xFFFFFFF0u, Fixture::raw, 0x3B90u })
            {
                ClassicFixture f;
                const ULONG context = f.context();
                f.newgad(10, 20, 120, 14, 0, 0, 1, 0, f.vi, 0);
                f.tagitems({});
                const ULONG gadget = f.create(kind, context);
                Window foreign{};
                assert(gadget && Kaffeine_ShadowAdd(&f.ctx, 0x3B90, &foreign, FILLER));
                const auto memory = f.memory;
                const auto blocks = native_blocks;
                const unsigned calls = native_calls;
                const UWORD shadows = f.ctx.num_shadows;
                assert(modify_call(f, lvo, gadget, window) == 20 && f.stopped() && f.state.d[0] == 20);
                assert(native_calls == calls && modify_calls.empty() && f.memory == memory && native_blocks == blocks &&
                       f.ctx.num_shadows == shadows && !f.ctx.classic);
                assert(!debug_lines.empty() && debug_lines.back().find("not a window of the program") != std::string::npos);
            }
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool kept_only = argc == 2 && std::strcmp(argv[1], "--kept-links") == 0;
    if (argc > 1 && !kept_only)
    {
        std::fputs("usage: test_gadgets [--kept-links]\n", stderr);
        return 2;
    }
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "VisualInfo resolves a type-13 public Screen without a generated layout", test_public_screen_visualinfo },
        { "GadTools and Intuition gadget tables use their ABI slots", test_registration },
        { "CreateContext() gives a 68k context and a shadow", test_context },
        { "A button: NewGadget, tags, label and the 68k gadget", test_button },
        { "Checkbox, cycle, mx, palette, text, number and generic", test_simple_kinds },
        { "Slider and scroller with the gadgets before them", test_compound_kinds },
        { "Listview: copied Nodes, names, native geometry, batch and PropInfo", test_listview_creation },
        { "Listview: empty Lists, successor-only terminators, NULL and ~0", test_listview_lists },
        { "Listview: 32767 entries, 32768 refused and bounded rings", test_listview_limits },
        { "Listview: malformed Lists, hooks, widths and item sizes refused before GadTools", test_listview_refused },
        { "Listview: last creation tags, WORD dimensions and replay order", test_listview_tags },
        { "Listview: Disabled, Top and MakeVisible replayed after native creation", test_listview_replay },
        { "Listview: allocation, native creation, guest heap and shadow rollback", test_listview_failures },
        { "ShowSelected: earlier string, creation tag order, moved mirrors and MaxChars", test_listview_showsel_creation },
        { "ShowSelected: dimension checks and mirrors after native or bridge creation failures", test_listview_showsel_failures },
        { "ShowSelected: buttons, compound parts, context and non-gadget addresses refused", test_listview_showsel_refused },
        { "String and integer with a 68k StringInfo", test_string_kinds },
        { "Of a repeated tag only the one that GadTools uses", test_tags_used },
        { "68k input that CreateGadgetA() refuses, and formats as RawDoFmt() gives them", test_refused },
        { "Batches: previous gadgets, FreeGadgets() and chains cut off", test_batches },
        { "GadTools fails, no guest memory, no shadows", test_failures },
        { "GT_GetGadgetAttrsA(): the attributes that the classes give", test_get_kinds },
        { "GT_GetGadgetAttrsA(): texts, numbers, levels and strings", test_get_values },
        { "GT_GetGadgetAttrsA(): listview values and NDK to AROS tag numbers", test_listview_get },
        { "GT_GetGadgetAttrsA(): what counts, and what has no attributes", test_get_lists },
        { "GT_GetGadgetAttrsA(): what the bridge refuses before it writes", test_get_refused },
        { "GT_GetGadgetAttrsA(): storage anywhere, also in the list itself", test_get_aliasing },
        { "GT_GetGadgetAttrsA(): memory, foreign pointers and gadgets that are gone", test_get_lifetime },
        { "GT_GetGadgetAttrsA() brings the mirrors up to date", test_get_settles },
        { "Intuition calls that would reach a GadTools gadget", test_intuition_guards },
        { "GT_SetGadgetAttrsA(): every attribute of every kind", test_set_kinds },
        { "ShowSelected: Selected, Labels, detach, tag order and immediate string mirrors", test_listview_showsel_set },
        { "ShowSelected: refused frees reset doomed, listview-only and joint frees succeed", test_listview_showsel_free },
        { "ShowSelected: two listviews share a string and retain independent dependencies", test_listview_showsel_shared },
        { "GT_SetGadgetAttrsA(): listview WORD positioning and immediate scroller mirrors", test_listview_set },
        { "Listview: native zero-row geometry and distinct builder/class divisors", test_listview_geometry },
        { "GT_SetGadgetAttrsA(): fresh listview snapshots, detach, reattach and replacement lifetime", test_listview_labels },
        { "GT_SetGadgetAttrsA(): listview snapshot rollback on refusal or allocation failure", test_listview_staging },
        { "Listview: OnGadget, OffGadget and FreeGadgets propagate and release the batch", test_listview_enable_free },
        { "GT_SetGadgetAttrsA(): texts, formats, pens, palettes and refusals", test_set_conversions },
        { "GT_SetGadgetAttrsA(): gadgets, windows and lists", test_set_targets },
        { "CreateGadgetA(): cycles and colour tables as the classes need them", test_set_creation },
        { "GT_SetGadgetAttrsA(): palette counts, choices and table room", test_set_palettes },
        { "GT_SetGadgetAttrsA(): string room and the AROS integer formatter", test_set_strings },
        { "GT_SetGadgetAttrsA(): staged copies survive only a native call", test_set_staging },
        { "GT_SetGadgetAttrsA(): 512 attributes, malformed data and the full slider range", test_set_boundaries },
        { "OnGadget(), OffGadget() and RefreshGadgets()", test_intuition_enable_refresh },
        { "Native Intuition: positions, counts, retained removal tails and AddGadget overwrite", test_native_window_lists },
        { "Kept links: RemoveGList and RemoveGadget keep mirror, classic, system and end successors", test_kept_remove_successors },
        { "Kept links: CloseWindow preserves every program link and FreeGadgets reaches all batches through classic gadgets", test_kept_close_classic_separators },
        { "Kept links: failed active-gadget filtering refuses FreeGadgets before cuts, resets doomed and permits a retry", test_kept_free_filter_failure },
        { "Kept links: a held gadget outside the freed batches needs no filter and cannot block the free", test_kept_free_unrelated_held_gadget },
        { "Kept links: AddGList(-1) restores all batches removed one by one", test_kept_readd_removed_batches },
        { "Kept links: OpenWindow WA_Gadgets restores all batches removed one by one", test_kept_open_removed_batches },
        { "Kept links: AddGList refuses a kept successor still on a window before native calls", test_kept_add_attached_successor },
        { "Kept links: AddGList and OpenWindow stop their native lists at program cuts between batches", test_kept_boundary_cuts },
        { "Kept links: AddGList and OpenWindow follow guest-only links between off-window batches", test_kept_link_off_window_batches },
        { "Kept links: FreeGadgets frees every removed batch with one terminated native call per batch", test_kept_free_removed_batches },
        { "Kept links: FreeGadgets walks past the program's own guest gadgets", test_kept_free_through_own_gadgets },
        { "Kept links: FreeGadgets stops at the program's cut and leaves the suffix for later", test_kept_free_program_cut },
        { "Kept links: FreeGadgets refuses middle entries and cycles and resets all doomed flags", test_kept_free_refusals_reset },
        { "Kept links: FreeGadgets cuts native and guest survivor links independently and preserves others", test_kept_free_survivor_links },
        { "Kept links: FreeGadgets bounds own-gadget walks and checks guest memory boundaries", test_kept_free_guest_bounds },
        { "Windows: AddGList positions among system gadgets, layout and mirror links", test_window_add_positions },
        { "Windows: whole-batch counts, oversized counts and a mirrored successor", test_window_add_counts_and_links },
        { "Windows: AddGList refusals stop before Intuition and roll back every mark", test_window_add_refusals },
        { "Windows: NULL, zero counts and application-owned gadgets never reach Intuition", test_window_add_empty_and_own },
        { "Windows: AddGadget takes one-gadget batches and refuses a scroller with arrows", test_window_add_one },
        { "Windows: RemoveGList positions, WORD counts, mirrors and subsequent FreeGadgets", test_window_remove_lists },
        { "Windows: removal prefixes cut native tails, keep guest tails and require a program cut before free", test_window_remove_prefix_and_tail },
        { "Windows: RemoveGadget, split-batch refusals and gadgets absent from that window", test_window_remove_one_and_not_found },
        { "Windows: RefreshGList and RefreshGadgets only forward attached native gadgets", test_window_refresh },
        { "Windows: FreeGadgets resets every doomed flag and attached previous gadgets refuse creation", test_window_free_and_create_guards },
        { "Windows: CreateGadgetA refuses an attached ShowSelected string", test_window_showsel_create_guard },
        { "Windows: ShowSelected dependencies attach, stay and leave on the same window", test_window_showsel_placement },
        { "Windows: an unattached ShowSelected string cannot join a different window from its listview", test_window_showsel_other_window },
        { "Window hooks: prepare validates whole lists and leaves no marks on success or refusal", test_window_prepare_refusals },
        { "Window hooks: opened attaches Intuition's appended list and sync settles only that window", test_window_hooks_open_sync_close },
        { "Window hooks: closing detaches two separate runs and clears both tails", test_window_closing_separate_runs },
        { "Windows: cleanup removes all runs from open windows before freeing any gadget", test_window_cleanup_open_windows },
        { "Windows: a gadget of the bridge before an added run gets its 68k link", test_window_existing_predecessor_add },
        { "Windows: a gadget of the bridge before a removed run gets its 68k link", test_window_existing_predecessor_remove },
        { "Windows: a palette that its class cannot lay out stays off windows", test_window_palette_layout },
        { "Windows: a list never goes between the gadgets of one batch on the window", test_window_insert_inside_batch },
        { "Windows: RefreshGList and RefreshGadgets settle what rendering changed on that window", test_window_refresh_settles },
        { "Windows: the mirrors follow what RemoveGList deactivated", test_window_remove_deactivates },
        { "Windows: a RemoveGList that Intuition refuses leaves the gadgets on no window", test_window_remove_native_failure },
        { "Windows: FreeGadgets never walks from a list into the window's own gadgets", test_window_free_stops_at_window_gadgets },
        { "Windows: FreeGadgets makes GadTools forget an active gadget that goes", test_window_held_gadget_forgotten },
        { "Window hooks: sync settles the batch that a message names, also off the window", test_window_sync_message_gadget },
        { "Windows: setters draw and ActivateGadget activates only on the batch's own window", test_window_setters_and_activation },
        { "The calls of a later part, and the end of the program", test_later_and_end },
        { "Classic copies: 16-byte NDK Borders, signed fields and native XY WORDs", test_classic_border_offsets },
        { "Classic copies: zero and negative Count, XY ranges and exact Border boundary", test_classic_border_counts_and_bounds },
        { "Classic copies: 1024-element Border and Image chains, over-limit and cycles", test_classic_copy_chain_limits },
        { "Classic copies: exact 44/56-byte Gadget offsets, signed fields and numeric data", test_classic_gadget_offsets },
        { "Classic copies: render flags, SelectRender gating and all label bit patterns", test_classic_renders_and_labels },
        { "Classic windows: both open LVOs pass native WA_Gadgets and attach copies in order", test_classic_window_open },
        { "Classic windows: failed native open and guest Window allocation drop all copies", test_classic_window_open_failure },
        { "Classic runs: AddGList positions, native order, shadows and guest links", test_classic_add_positions },
        { "Classic runs: WORD counts, limited runs and AddGadget taking exactly one", test_classic_add_counts },
        { "Classic runs: unsupported kinds leave the entire list off; a requester is ignored", test_classic_unsupported_runs },
        { "Classic runs: never between the gadgets of one GadTools call on the window", test_classic_add_split },
        { "Classic gadgets: ActivateGadget passes the copy on its window to Intuition", test_classic_activate },
        { "Classic windows: unsupported lists allow opening without WA_Gadgets", test_classic_unsupported_window_open },
        { "Classic runs: malformed, mixed, duplicate, cyclic and attached gadgets stop with D0=20", test_classic_add_refusals },
        { "Classic runs: 4097 distinct gadgets stop with D0=20 before any copy or shadow", test_classic_add_run_limit },
        { "Classic runs: every AllocVec failure and shadow exhaustion roll back without leaks", test_classic_allocation_failures },
        { "Classic removal: positions, counts, retained native/guest tails and predecessor repair", test_classic_remove_runs },
        { "Classic removal: RemoveGadget, wrong windows, absent gadgets and zero counts", test_classic_remove_one_and_windows },
        { "Classic removal: both LVOs retain last links to own, GadTools, system gadgets or NULL", test_classic_removed_last_links },
        { "Classic links: GadTools before and after own runs, insertion and removal repair", test_classic_mixed_window_links },
        { "Classic removal: runs reaching system gadgets or GadTools are refused atomically", test_classic_remove_foreign_run },
        { "Classic flags: by-bit import and merge before both refresh LVOs", test_classic_refresh_flags },
        { "Classic flags: imported snapshots track native reversals during and after refresh", test_classic_imported_flags_snapshot },
        { "Classic flags: by-bit export before messages and ACTIVEGADGET output only", test_classic_message_flags },
        { "Classic flags: On/Off import, native calls on the owning window and flag-only changes elsewhere", test_classic_enable_flags },
        { "Classic flags: unattached guest Flags and the old shadowed native On/Off path", test_classic_unattached_and_shadow_enable },
        { "Classic messages: GADGETUP/DOWN IAddress for attached and retired copies, including GadTools", test_classic_message_addresses },
        { "Classic lifetime: queued messages retain old copies across re-add until a remove sweeps", test_classic_queued_retirement },
        { "Classic lifetime: held Exec and GadTools messages retain copies until the next sweep", test_classic_held_retirement },
        { "Classic lifetime: CloseWindow frees attached and retired copies and replies messages", test_classic_close_lifetime },
        { "Classic lifetime: program cleanup removes open-window copies and frees pending copies", test_classic_cleanup_lifetime },
        { "Classic snapshots: geometry and labels are copied again only on reattachment", test_classic_snapshot_on_reattach },
        { "Classic strings: NDK StringInfo offsets, signed fields and shared guest buffers", test_classic_stringinfo_offsets },
        { "Classic strings: NDK StringExtend offsets, fonts, pens and either extension bit", test_classic_stringextend_offsets },
        { "Classic strings: exact 36-byte records, buffer boundaries and MaxChars limits", test_classic_string_boundaries },
        { "Classic strings: optional buffers, default keymaps and custom keymap lists left off", test_classic_string_optional_buffers_and_keymaps },
        { "Classic strings: unterminated buffers copy at every former text boundary with guard bytes", test_classic_string_text_count_boundaries },
        { "Classic strings: text beyond MaxChars is copied, drawn and counted without truncation", test_classic_string_beyond_maxchars },
        { "Classic strings: every malformed record, buffer, hook and font refuses the whole run", test_classic_string_refusals },
        { "Classic strings: both open LVOs refuse malformed strings without copies or native calls", test_classic_string_open_refusals },
        { "Classic strings: both open LVOs export the state set while Intuition opens", test_classic_string_open_state },
        { "Classic strings: failed native opens and guest Window allocation drop all copies", test_classic_string_open_failure },
        { "Classic strings: both refreshes, On, Off and Activate import and export state", test_classic_string_call_state },
        { "Classic strings: selection suppresses StringInfo imports while shared Flags still pass", test_classic_string_selected_imports },
        { "Classic strings: cursor and display positions bound to the current text despite stale NumChars", test_classic_string_bounded_imports },
        { "Classic strings: text written into an empty gadget keeps positions at its end through refresh", test_classic_string_new_text_positions },
        { "Classic strings: imports count into the zero guard and respect the native text limit", test_classic_string_guard_positions },
        { "Classic strings: copies start unselected, export that state and accept pending imports", test_classic_string_initial_selection },
        { "Classic selection: both removal LVOs clear strings and integers but preserve Booleans", test_classic_string_remove_selection },
        { "Classic selection: CloseWindow clears strings and integers but preserves Booleans", test_classic_string_close_selection },
        { "Classic strings: AddGList and AddGadget export NumChars and DispCount after native drawing", test_classic_string_add_state },
        { "Classic strings: independent changes to all shared fields merge without stale snapshots", test_classic_string_independent_changes },
        { "Classic strings: Exec and GadTools messages export window state without importing", test_classic_string_message_state },
        { "Classic strings: every drawing LVO accepts unterminated text and stays inside the guard", test_classic_string_text_count_calls },
        { "Classic strings: On and Off with another window or NULL bound state without native calls", test_classic_string_other_windows },
        { "Classic strings: On and Off import every own gadget on the window before native work", test_classic_string_enable_window_state },
        { "Classic strings: SetEditHook at -492 stops with D0=20 for every kind of A0", test_classic_string_setedithook },
        { "Classic strings: both removal LVOs export state and free copies without messages", test_classic_string_remove_state },
        { "Classic strings: queued and held Exec or GadTools messages retain copies until sweeping", test_classic_string_retirement },
        { "Classic strings: CloseWindow exports state and frees attached and retired copies", test_classic_string_close_lifetime },
        { "Classic strings: program cleanup frees attached, retired and pending copies", test_classic_string_cleanup_lifetime },
        { "Classic strings: each AllocVec failure and exhausted shadows leave lists off without leaks", test_classic_string_allocation_failures },
        { "Classic props: exact PropInfo and AUTOKNOB header, ignored data, next and SelectRender", test_classic_prop_autoknob_copy },
        { "Classic props: custom knob Image chains and live guest data regardless of render flags", test_classic_prop_knob_chain_copy },
        { "Classic props: exact PropInfo and Image boundaries and empty native knobs", test_classic_prop_copy_boundaries },
        { "Classic props: invalid and one-byte-short records, knob chains and data refuse copying", test_classic_prop_copy_refusals },
        { "Classic props: every malformed run rolls back copies, shadows, heap and marks", test_classic_prop_run_refusals },
        { "Classic props: both open LVOs refuse malformed props before native work", test_classic_prop_open_refusals },
        { "Classic props: every initial and updated agreed value preserves pending guest edits", test_classic_prop_agreed_values },
        { "Classic props: records keep PropInfo and knob addresses until reattachment", test_classic_prop_record_addresses },
        { "Classic props: imports merge all 15 shared flag bits with independent native changes", test_classic_prop_import_flags },
        { "Classic props: pots and bodies import as wholes with independent and simultaneous changes", test_classic_prop_import_pots_and_bodies },
        { "Classic props: selection holds off imports, allows exports and preserves pending changes", test_classic_prop_selected_imports },
        { "Classic props: exports merge all 15 shared flag bits, KNOBHIT and native reversals", test_classic_prop_export_flags },
        { "Classic props: each pot, body, calculated field and knob coordinate exports only changes", test_classic_prop_export_fields },
        { "Classic props: both OpenWindow paths export native PropInfo and knob changes", test_classic_prop_open_state },
        { "Classic props: AddGList and AddGadget export native PropInfo and knob changes", test_classic_prop_add_state },
        { "Classic props: both refreshes, On, Off and Activate import before and export after native work", test_classic_prop_call_state },
        { "Classic props: Exec and GadTools gadget and mouse messages export only their window", test_classic_prop_message_state },
        { "Classic props: both removals export drag state and geometry and free the copies", test_classic_prop_remove_state },
        { "Classic props: copies start unselected and then accept pending imports", test_classic_prop_initial_selection },
        { "Classic ModifyProp: both drawing windows pass copy, narrowed values and count after imports on its own window", test_classic_modify_prop_window },
        { "Classic ModifyProp: unattached and removed gadgets store values with either program window", test_classic_modify_prop_elsewhere },
        { "Classic ModifyProp: foreign windows stop with rc 20 and D0 20 without writes", test_classic_modify_prop_foreign_window },
        { "Classic ModifyProp: GadTools forwards either program window and discards the requester", test_classic_modify_prop_gadtools },
        { "Classic ModifyProp: Boolean, custom, garbage and invalid PropInfo are ignored with a debug line", test_classic_modify_prop_ignored },
        { "Classic props: every allocation failure rolls back the complete run and permits retry", test_classic_prop_allocation_failures },
        { "Classic props: empty native Images never export coordinates to address zero or an unused knob", test_classic_prop_empty_knob_state },
        { "Classic props: imports retain the copy's AUTOKNOB and render while merging other changes", test_classic_prop_autoknob_import },
        { "Classic props: exports preserve guest AUTOKNOB even when native AUTOKNOB changes", test_classic_prop_autoknob_export },
        { "Classic props: AUTOKNOB changes rebuild the render only on reattachment", test_classic_prop_autoknob_reattach },
        { "Classic props: guest reads precede Forbid, selection is checked inside and all imports precede Permit", test_classic_prop_import_critical_section },
        { "Classic props: PropInfo and knob are sampled together inside Forbid before exporting after Permit", test_classic_prop_sample_critical_section },
        { "Classic props: nested Forbid and Permit restore the caller's nesting after each pass", test_classic_prop_critical_nesting },
        { "Classic ModifyProp: native calls keep the copy's AUTOKNOB and guest Flags keep the AUTOKNOB given", test_classic_modify_prop_autoknob },
        { "Classic ModifyProp: selected copies overwrite all pending guest values even when native values did not change", test_classic_modify_prop_selected_agreement },
        { "Classic ModifyProp: unchanged geometry rewrites all calculated fields and knob place, sizes only with AUTOKNOB on both sides", test_classic_modify_prop_geometry_agreement },
        { "Classic ModifyProp: NULL window is silent and changes nothing for every kind of gadget", test_classic_modify_prop_null_window },
        { "Classic ModifyProp: GadTools gadgets also refuse every foreign window with rc 20 and D0 20", test_classic_modify_prop_gadtools_foreign_window }
    };
    size_t groups = 0;
    for (const Test &test : tests)
    {
        if (kept_only && std::strncmp(test.name, "Kept links:", 11) != 0) continue;
        test.run();
        ++groups;
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("GadTools gadgets: %zu groups passed\n", groups);
    return 0;
}
