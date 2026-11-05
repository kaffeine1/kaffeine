/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Production lock module and heap with explicit native-reference oracles. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <proto/exec.h>
#include <proto/dos.h>
#include "kaffeine_locks.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include "kaffeine_sched.h"

static unsigned checks, failures, unlocks, duplicates, setters, comparisons;
static int allocation_failure = -1;
static bool duplicate_failure, duplicate_same, handler_failure;
static SIPTR last_error;
static Process process;
static Process *native_process = &process;
static std::map<APTR, ULONG> allocations;
struct NativeLock { FileLock lock = {}; unsigned references = 1; ULONG object; };
static std::vector<std::unique_ptr<NativeLock>> natives;
static std::map<BPTR, NativeLock *> native_map;
struct NativeVolume { DeviceList volume = {}; std::array<UBYTE, 257> name = {}; };
static std::array<NativeVolume, 40> volumes;
static MsgPort handler = {1}, other_handler = {2};
static DosList doslist_root;
static bool doslist_locked, doslist_attempt_failure, doslist_reader_busy, doslist_forbidden;
static ULONG doslist_native_flags;
static unsigned doslist_calls, doslist_attempts, doslist_delays, doslist_shared;

static void check(bool ok, const char *label)
{ ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", label); } }
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    assert(!doslist_locked); // No allocation or guest publication while DOS is locked.
    if (allocation_failure == 0) { allocation_failure = -1; return nullptr; }
    if (allocation_failure > 0) --allocation_failure;
    void *p = std::malloc(size);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xD7, size); allocations[p] = size; }
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{ assert(allocations.count(p) && allocations[p] == size); allocations.erase(p); std::free(p); }
extern "C" APTR AllocVec(ULONG n, ULONG f) { return AllocMem(n, f); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p, allocations.at(p)); }
extern "C" Task *FindTask(CONST_STRPTR n) { assert(!n); return &native_process->pr_Task; }
extern "C" SIPTR IoErr(void) { return last_error; }
extern "C" SIPTR SetIoErr(SIPTR e) { SIPTR old = last_error; last_error = e; return old; }
extern "C" DosList *LockDosList(ULONG f)
{
    if (f == (LDF_READ | LDF_VOLUMES)) return nullptr; // FileLock's volume metadata copy.
    assert(f == (LDF_WRITE | LDF_ALL | LDF_ENTRY) && !doslist_locked);
    doslist_locked = true; ++doslist_calls; return &doslist_root;
}
extern "C" DosList *AttemptLockDosList(ULONG f)
{
    ++doslist_attempts;
    if (doslist_attempt_failure) return nullptr;
    if (f & LDF_WRITE)
    {
        if (doslist_reader_busy) return nullptr;
        doslist_native_flags=f;
    }
    else
    {
        assert(f==(LDF_READ|LDF_ALL) && !doslist_locked);
        doslist_locked=true;doslist_native_flags=f;++doslist_shared;++doslist_calls;return &doslist_root;
    }
    return LockDosList(f);
}
extern "C" void UnLockDosList(ULONG f)
{
    if (f == (LDF_READ | LDF_VOLUMES)) return;
    assert(f == doslist_native_flags && doslist_locked && !doslist_forbidden);
    doslist_locked = false;
}
extern "C" void Forbid(void)
{ assert(doslist_locked && (doslist_native_flags&LDF_READ) && !doslist_forbidden);doslist_forbidden=true; }
extern "C" void Permit(void)
{ assert(doslist_locked && doslist_forbidden);doslist_forbidden=false; }
extern "C" void Delay(ULONG n)
{ assert(n==1 && !doslist_locked && !doslist_forbidden);++doslist_delays; }
extern "C" LONG Strnicmp(CONST_STRPTR a, CONST_STRPTR b, LONG n)
{
    // Default utility.library case table; native target tests also use its
    // installed locale patch rather than imposing the host's locale.
    auto lower = [](unsigned char c) {
        return ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) ? c + 0x20 : c;
    };
    while (n-- > 0) { int x = lower(*a++), y = lower(*b++); if (x != y || !x) return x-y; }
    return 0;
}
static BPTR acquire(unsigned volume = 0, ULONG object = 1)
{
    auto n = std::make_unique<NativeLock>();
    n->object = object;
    n->lock.fl_Link = reinterpret_cast<BPTR>(0xFEDCBA9876543210ULL);
    n->lock.fl_Key = 0xABCD000000000000ULL + natives.size();
    n->lock.fl_Access = -2;
    n->lock.fl_Task = &handler;
    n->lock.fl_Volume = MKBADDR(&volumes[volume].volume);
    BPTR p = MKBADDR(&n->lock);
    native_map[p] = n.get(); natives.push_back(std::move(n));
    return p;
}
extern "C" void UnLock(BPTR p)
{
    if (!p) return;
    assert(native_map.count(p) && native_map[p]->references);
    --native_map[p]->references; ++unlocks; last_error = 0xBAD;
}
extern "C" BPTR DupLock(BPTR p)
{
    ++duplicates;
    if (duplicate_failure) { last_error = 103; return nullptr; }
    assert(native_map.count(p) && native_map[p]->references);
    if (duplicate_same) { ++native_map[p]->references; return p; }
    auto *n = native_map[p];
    unsigned index = static_cast<unsigned>(static_cast<NativeVolume *>(BADDR(n->lock.fl_Volume)) - volumes.data());
    return acquire(index, n->object);
}
extern "C" LONG SameLock(BPTR a, BPTR b)
{
    ++comparisons;
    assert(native_map.count(a) && native_map[a]->references && native_map.count(b) && native_map[b]->references);
    const auto *x = native_map[a], *y = native_map[b];
    if (x->lock.fl_Volume != y->lock.fl_Volume) return LOCK_DIFFERENT;
    return x->object == y->object ? LOCK_SAME : LOCK_SAME_VOLUME;
}
extern "C" BPTR CurrentDir(BPTR p)
{ ++setters; BPTR old = native_process->pr_CurrentDir; native_process->pr_CurrentDir = p; return old; }
extern "C" BPTR GetProgramDir(void) { return native_process->pr_HomeDir; }
extern "C" BPTR SetProgramDir(BPTR p)
{ ++setters; BPTR old = native_process->pr_HomeDir; native_process->pr_HomeDir = p; return old; }
extern "C" ULONG Kaffeine_HandlerPort(KaffeineContext *ctx, MsgPort *p)
{
    assert(!doslist_locked);
    if (handler_failure) { SetIoErr(ERROR_NO_FREE_STORE); return 0; }
    ULONG guest = p == &handler ? 0x8000 : 0x8040;
    std::memset(ctx->mem + guest, 0, 34); ctx->mem[guest + 8] = 4; ctx->mem[guest + 14] = 2;
    m68k_write32(ctx, guest + 20, guest + 24); m68k_write32(ctx, guest + 28, guest + 20);
    return guest;
}
extern "C" IPTR Kaffeine_MessageFailure(KaffeineContext *ctx, const char *)
{ ctx->running = FALSE; ctx->exit_code = 20; return 20; }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }

struct Fixture
{
    KaffeineLibBase library = {};
    KaffeineContext *ctx;
    Fixture()
    {
        allocation_failure = -1; duplicate_failure = duplicate_same = handler_failure = false;
        unlocks = duplicates = setters = comparisons = 0; last_error = 901;
        assert(!doslist_locked && !doslist_forbidden); doslist_root = {};
        doslist_calls = doslist_attempts = doslist_delays = doslist_shared = 0;
        doslist_attempt_failure = doslist_reader_busy = false;
        natives.clear(); native_map.clear(); native_process = &process; process = {}; process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
        volumes = {};
        for (unsigned i = 0; i < volumes.size(); ++i)
        {
            auto &v = volumes[i]; v.volume.dl_Type = DLT_VOLUME; v.volume.dl_Task = &handler;
            v.volume.dl_VolumeDate = {LONG(i), 2, 3}; v.volume.dl_DiskType = 0x444F5301;
#ifdef AROS_FAST_BSTR
            std::memcpy(v.name.data(), "Work", 5);
#else
            v.name[0] = 4; std::memcpy(v.name.data() + 1, "Work", 4);
#endif
            v.volume.dl_Name = MKBADDR(v.name.data());
        }
        ctx = Kaffeine_CreateContext(&library); assert(ctx); ctx->running = TRUE;
    }
    ~Fixture()
    {
        allocation_failure = -1; Kaffeine_LockCleanup(ctx);
        assert(!ctx->locks);
        /* Simulate the independent process/scoped owners ending afterwards. */
        for (auto &n : natives) while (n->references) UnLock(MKBADDR(&n->lock));
        Kaffeine_DestroyContext(ctx);
        check(allocations.empty(), "lock/heap native allocations are balanced");
    }
    ULONG read(ULONG a) { return m68k_read32(ctx, a); }
    bool resolves(ULONG t, BPTR expected)
    { BPTR got = reinterpret_cast<BPTR>(1); return Kaffeine_LockResolve(ctx, t, &got) && got == expected; }
    void task()
    {
        ctx->guest_exec_task = 0x6000; ctx->m68k_sysbase = 0x2000;
        m68k_write32(ctx, 0x2000 + M68K_EXECBASE_THISTASK, ctx->guest_exec_task);
    }
};

static void snapshots_and_identity()
{
    Fixture f; BPTR a = acquire(), b = acquire(0, 1), c = acquire(0, 2);
    ULONG x = Kaffeine_LockAdopt(f.ctx, a), y = Kaffeine_LockAdopt(f.ctx, b), z = Kaffeine_LockAdopt(f.ctx, c);
    check(x && y && z && x != y && y != z && !(x >> 30), "distinct references have unique valid guest BPTRs");
    check(f.resolves(x, a), "resolver returns full-width private native BPTR");
    check(f.read(x*4) == 0 && f.read(x*4+4) == f.read(y*4+4) && f.read(x*4+4) != f.read(z*4+4), "SameLock equivalence gives numeric object cookies without exposing native keys");
    check(f.read(x*4+8) == ULONG(-2) && f.read(x*4+12) == 0x8000, "classic signed access and typed handler identity");
    ULONG v = f.read(x*4+16)*4, name = f.read(v+40)*4;
    check(v && f.read(y*4+16)*4 == v && f.read(v+4) == 2 && f.read(v+8) == 0x8000, "same-volume views share a safe classic DeviceList");
    check(!f.read(v) && !f.read(v+12) && !f.read(v+28) && !f.read(v+36) && f.ctx->mem[name] == 4 && !std::memcmp(f.ctx->mem+name+1,"Work",4), "volume chain, lock pointers and bounded BSTR stay guest-only");
    check(last_error == 901 && comparisons, "successful publication preserves native IoErr");
    BPTR resolved;
    check(Kaffeine_LockForHandler(f.ctx, x, &handler, &resolved) && resolved == a, "packet lock resolves only to its native handler");
    unsigned calls = unlocks;
    check(!Kaffeine_LockForHandler(f.ctx, x, &other_handler, &resolved) && !resolved && unlocks == calls, "wrong handler is rejected without consuming native lock");
    check(Kaffeine_LockForHandler(f.ctx, 0, &other_handler, &resolved) && !resolved, "zero packet lock remains valid");
    check(Kaffeine_LockProtected(f.ctx,x*4+19,1) && Kaffeine_LockProtected(f.ctx,name+259,1) && !Kaffeine_LockProtected(f.ctx,0x7000,4), "protection includes whole snapshot/slab storage");
    check(Kaffeine_LockRelease(f.ctx,x) && !f.resolves(x,a), "last owned release retires its token");
    ULONG later = Kaffeine_LockAdopt(f.ctx,acquire(0,3));
    check(later && later != x && Kaffeine_LockProtected(f.ctx,x*4,20), "retired guest addresses are never republished or writable ordinary storage");
}

static void scopes_and_refcounts()
{
    Fixture f; BPTR a = acquire(); duplicate_same = true;
    ULONG x = Kaffeine_LockAdopt(f.ctx,a), y = Kaffeine_LockAdopt(f.ctx,DupLock(a));
    check(x == y && native_map[a]->references == 2, "duplicate native identity coalesces with two owned refs");
    check(Kaffeine_LockRelease(f.ctx,x) && f.resolves(x,a) && native_map[a]->references == 1, "one release preserves another owned native reference");
    check(Kaffeine_LockBorrow(f.ctx,a,KAFFEINE_LOCK_MATCH,1) == x && Kaffeine_LockBorrow(f.ctx,a,KAFFEINE_LOCK_MATCH,1) == x, "same scoped borrow is idempotent");
    check(Kaffeine_LockBorrow(f.ctx,a,KAFFEINE_LOCK_WBARG,1) == x, "different scope kinds retain independent ownership");
    Kaffeine_LockScopeDrop(f.ctx,KAFFEINE_LOCK_MATCH,1);
    check(f.resolves(x,a), "scope retirement preserves other views and caller refs");
    Kaffeine_LockScopeDrop(f.ctx,KAFFEINE_LOCK_WBARG,1);
    check(Kaffeine_LockRelease(f.ctx,x) && native_map[a]->references == 0, "scope drops never unlock the caller-owned reference");
    BPTR b = acquire(); ULONG borrowed = Kaffeine_LockBorrow(f.ctx,b,KAFFEINE_LOCK_DEVPROC,22);
    unsigned before = unlocks; KaffeineLockTransfer lease = {};
    check(!Kaffeine_LockRelease(f.ctx,borrowed) && !Kaffeine_LockTransferBegin(f.ctx,borrowed,&lease) && unlocks == before, "borrow-only UnLock and transfer fail before native consumption");
    Kaffeine_LockScopeDrop(f.ctx,KAFFEINE_LOCK_DEVPROC,22);
    check(!f.resolves(borrowed,b) && native_map[b]->references == 1, "scope drop retires a borrowed view without unlocking its owner");
}

static void edited_and_invalid_views()
{
    for (unsigned off : {0U,4U,8U,12U,16U})
    {
        Fixture f; BPTR a = acquire(); ULONG t = Kaffeine_LockAdopt(f.ctx,a);
        f.ctx->mem[t*4+off] ^= 1; unsigned before = unlocks;
        check(!f.resolves(t,a) && !Kaffeine_LockRelease(f.ctx,t) && unlocks == before, "every edited FileLock field is rejected before native use");
    }
    Fixture f; BPTR a = acquire(); ULONG t = Kaffeine_LockAdopt(f.ctx,a), v = f.read(t*4+16)*4;
    ULONG name = f.read(v+40)*4; f.ctx->mem[name+1] ^= 1;
    check(!f.resolves(t,a), "edited volume name invalidates dependent lock views");
    for (ULONG bad : {1U,t+1,t|0xC0000000U,0xFFFFFFFFU,f.ctx->mem_size>>2})
        check(!f.resolves(bad,a), "forged/interior/wrapped/outside BPTR is rejected");
    check(f.resolves(0,nullptr) && Kaffeine_LockRelease(f.ctx,0), "zero lock has API-specific null semantics");
}

static void transfers_and_cleanup()
{
    Fixture f; BPTR a = acquire(); ULONG t = Kaffeine_LockAdopt(f.ctx,a); KaffeineLockTransfer lease = {};
    check(Kaffeine_LockTransferBegin(f.ctx,t,&lease) && lease.native == a, "transfer reserves one caller-owned reference");
    unsigned before = unlocks;
    check(!Kaffeine_LockRelease(f.ctx,t) && unlocks == before, "reserved transfer cannot be unlocked concurrently");
    Kaffeine_LockTransferEnd(f.ctx,&lease,FALSE);
    check(f.resolves(t,a) && Kaffeine_LockRelease(f.ctx,t), "native failure restores the reserved ref without allocation");
    BPTR b = acquire(); t = Kaffeine_LockAdopt(f.ctx,b);
    check(Kaffeine_LockTransferBegin(f.ctx,t,&lease), "second transfer begins");
    Kaffeine_LockTransferEnd(f.ctx,&lease,TRUE);
    check(!f.resolves(t,b) && native_map[b]->references == 1, "successful transfer retires view and leaves native ownership with recipient");
    Kaffeine_LockTransferEnd(f.ctx,&lease,FALSE);
    check(native_map[b]->references == 1, "finished lease cannot restore a consumed reference twice");
    BPTR caller = acquire(0,3), scoped = acquire(0,4), installed = acquire(0,5);
    duplicate_same = true;
    ULONG owned = Kaffeine_LockAdopt(f.ctx,caller), process_token;
    assert(owned && Kaffeine_LockAdopt(f.ctx,DupLock(caller)) == owned);
    assert(Kaffeine_LockBorrow(f.ctx,scoped,KAFFEINE_LOCK_WBARG,77));
    process.pr_CurrentDir = installed;
    assert(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&process_token));
    before = unlocks; SetIoErr(901); Kaffeine_LockCleanup(f.ctx);
    check(!f.ctx->locks && unlocks == before+2 && !native_map[caller]->references &&
          native_map[b]->references == 1 && native_map[scoped]->references == 1 &&
          native_map[installed]->references == 1,
          "cleanup releases every caller duplicate while retaining transferred, scoped and process ownership");
    check(last_error == 901, "cleanup preserves native IoErr across caller releases");
}

static void installed_lifetimes()
{
    Fixture f; f.task(); BPTR initial = acquire(0,1), replacement = acquire(0,2);
    process.pr_CurrentDir = initial; ULONG old, input = Kaffeine_LockAdopt(f.ctx,replacement), token;
    check(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&old) && f.read(0x6000+152) == old, "initial native CurrentDir is published in the Process snapshot");
    unsigned before = unlocks;
    check(!Kaffeine_LockRelease(f.ctx,old) && unlocks == before, "initial installed lock is not caller-owned");
    ULONG displaced, returned;
    check(Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,input,&displaced) && displaced == old && process.pr_CurrentDir == replacement && f.read(0x6000+152) == input, "CurrentDir swaps owned input with an owned displaced reference");
    check(Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,displaced,&returned) && returned == input && process.pr_CurrentDir == initial, "nested CurrentDir(CurrentDir(new)) restores original ownership");
    check(Kaffeine_LockRelease(f.ctx,returned) && native_map[replacement]->references == 0, "restored replacement may be unlocked exactly once");
    duplicate_same = true;
    ULONG duplicate = Kaffeine_LockAdopt(f.ctx,DupLock(initial));
    check(duplicate == old && Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,duplicate,&returned) && returned == old, "same-slot duplicate exchanges references without losing caller ownership");
    check(Kaffeine_LockRelease(f.ctx,returned) && native_map[initial]->references == 1, "same-slot returned duplicate can be released without unlocking process ownership");
    process.pr_HomeDir = acquire(0,3);
    check(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_PROGRAM,&token) && f.read(0x6000+188) == token, "initial ProgramDir has a typed synchronized view");
    Kaffeine_LockCleanup(f.ctx);
    check(native_map[initial]->references == 1 && native_map[process.pr_HomeDir]->references == 1, "cleanup leaves installed native process owners intact");
}

static void borrowed_install_and_failures()
{
    Fixture f; f.task(); BPTR borrowed = acquire();
    ULONG input = Kaffeine_LockBorrow(f.ctx,borrowed,KAFFEINE_LOCK_MATCH,19), old, installed;
    duplicate_failure = true;
    check(!Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,input,&old) && !process.pr_CurrentDir && native_map[borrowed]->references == 1, "failed native duplicate leaves process and scoped owner untouched");
    duplicate_failure = false;
    check(Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,input,&old) && !old && process.pr_CurrentDir != borrowed, "borrowed install acquires its own native duplicate");
    BPTR installed_native = process.pr_CurrentDir;
    check(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&installed) && installed != input && f.read(0x6000+152) == installed, "Process snapshot publishes the actual installed duplicate token");
    Kaffeine_LockScopeDrop(f.ctx,KAFFEINE_LOCK_MATCH,19);
    check(!f.resolves(input,borrowed) && f.resolves(installed,installed_native) && native_map[borrowed]->references == 1, "scope retirement does not invalidate or unlock installed duplicate");
    check(Kaffeine_LockSwapInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,0,&old) && old == installed && Kaffeine_LockRelease(f.ctx,old), "removing installed duplicate returns its caller-owned reference");
}

static void native_process_field_changes()
{
    Fixture f; f.task(); BPTR first = acquire(0,1); process.pr_CurrentDir = first;
    ULONG obsolete, current;
    assert(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&obsolete));
    BPTR replacement = acquire(0,2);
    /* An independent native owner performs a legal swap and releases its
       displaced reference before the guest asks for a fresh snapshot. */
    UnLock(CurrentDir(replacement));
    allocation_failure = 0;
    check(!Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&current) &&
          process.pr_CurrentDir == replacement && native_map[replacement]->references == 1,
          "failed refresh preserves an externally replaced native process field");
    allocation_failure = -1;
    check(!f.resolves(obsolete,first), "externally displaced installed-only token is retired before publication");
    check(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&current) && f.resolves(current,replacement) &&
          f.read(0x6000+152) == current,
          "retry publishes replacement without comparing the released native lock");
    duplicate_same = true;
    ULONG independent = Kaffeine_LockAdopt(f.ctx,DupLock(replacement));
    BPTR third = acquire(0,3);
    UnLock(CurrentDir(third));
    check(Kaffeine_LockGetInstalled(f.ctx,KAFFEINE_LOCK_CURRENT,&current) && f.resolves(independent,replacement),
          "external process replacement retains an independent caller reference");
    check(Kaffeine_LockRelease(f.ctx,independent) && !native_map[replacement]->references,
          "independent displaced reference remains releasable exactly once");
}

static void publication_failures_and_limits()
{
    for (int fail = 0; fail < 8; ++fail)
    {
        Fixture f; BPTR a = acquire(); allocation_failure = fail;
        ULONG t = Kaffeine_LockAdopt(f.ctx,a); allocation_failure = -1;
        check(t ? native_map[a]->references == 1 : native_map[a]->references == 0, "publication allocation failure balances the acquired native reference");
    }
    {
        Fixture f; BPTR a = acquire(); handler_failure = true;
        check(!Kaffeine_LockAdopt(f.ctx,a) && !native_map[a]->references, "handler publication failure also balances native acquisition");
    }
    {
        Fixture f; std::vector<ULONG> tokens;
        for (unsigned i=0;i<KAFFEINE_LOCK_LIMIT;++i) tokens.push_back(Kaffeine_LockAdopt(f.ctx,acquire(0,i)));
        BPTR overflow = acquire(0,1000);
        check(!Kaffeine_LockAdopt(f.ctx,overflow) && !native_map[overflow]->references && last_error == ERROR_NO_FREE_STORE, "active lock limit fails with balanced native ownership");
        check(Kaffeine_LockRelease(f.ctx,tokens[0]) && Kaffeine_LockAdopt(f.ctx,acquire(0,1001)), "retired private lock slots can be reused with fresh guest identities");
    }
    {
        Fixture f;
        for (unsigned i=0;i<80;++i)
        { ULONG t=Kaffeine_LockAdopt(f.ctx,acquire(0,i)); check(t && Kaffeine_LockRelease(f.ctx,t), "sequential volume lifetimes do not consume simultaneous volume capacity"); }
    }
    {
        Fixture f;
        for (unsigned i=0;i<KAFFEINE_LOCK_VOLUME_LIMIT;++i) assert(Kaffeine_LockAdopt(f.ctx,acquire(i,i)));
        BPTR overflow = acquire(KAFFEINE_LOCK_VOLUME_LIMIT,99);
        check(!Kaffeine_LockAdopt(f.ctx,overflow) && !native_map[overflow]->references, "simultaneous volume limit is bounded and balances reference");
    }
}

static void access_refresh()
{
    Fixture f; BPTR a=acquire(); ULONG t=Kaffeine_LockAdopt(f.ctx,a);
    native_map[a]->lock.fl_Access=-1;
    check(Kaffeine_LockRefreshAccess(f.ctx,t) && f.read(t*4+8)==ULONG(-1) && f.resolves(t,a) && last_error==901, "successful native access change refreshes signed access and expected view only");
    f.ctx->mem[t*4+4] ^= 1; auto before=std::array<UBYTE,20>{}; std::memcpy(before.data(),f.ctx->mem+t*4,20);
    check(!Kaffeine_LockRefreshAccess(f.ctx,t) && !std::memcmp(before.data(),f.ctx->mem+t*4,20), "refresh refuses an edited token without laundering its modified fields");
}

static void separate_process_owners()
{
    Fixture f;
    Process child = {};
    child.pr_Task.tc_Node.ln_Type = NT_PROCESS;
    process.pr_CurrentDir = acquire(0, 401);
    child.pr_CurrentDir = acquire(0, 402);
    ULONG startup = 0;
    check(Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &startup),
          "initial native directory can be captured before the root TCB exists");
    f.ctx->m68k_sysbase = 0x4000;
    auto view = [&](Process *native, ULONG guest) {
        native_process = native;
        f.ctx->guest_exec_task = guest;
        f.ctx->broker_call = native != &process;
        f.ctx->broker_owner = f.ctx->broker_call ? &native->pr_Task : nullptr;
        m68k_write32(f.ctx, 0x4000 + M68K_EXECBASE_THISTASK, guest);
    };
    ULONG root = 0, peer = 0, again = 0;
    view(&process, 0x6000);
    check(Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &root) && root == startup,
          "root TCB publication binds the initial native owner without changing its token");
    view(&child, 0x6100);
    check(Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &peer) && peer != root,
          "another DOS owner has independent installed state in one arena");
    check(f.resolves(root, process.pr_CurrentDir) && f.resolves(peer, child.pr_CurrentDir),
          "publishing a child directory does not retire the parent's token");
    ULONG caller = Kaffeine_LockAdopt(f.ctx, acquire(0, 403));
    unsigned before = unlocks;
    last_error = static_cast<SIPTR>(0x123456789ABCULL);
    Kaffeine_LockOwnerDrop(f.ctx, &child.pr_Task);
    check(unlocks == before && last_error == static_cast<SIPTR>(0x123456789ABCULL),
          "dropping installed metadata neither unlocks native DOS owners nor narrows IoErr");
    check(!f.resolves(peer, child.pr_CurrentDir), "dead owner's installed-only token is retired");
    f.ctx->running = TRUE;
    check(Kaffeine_LockRelease(f.ctx, caller), "caller-owned reference survives owner retirement");
    view(&process, 0x6000);
    check(Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &again) && again == root,
          "returning to the root reuses its original directory identity");
    view(&child, 0x6200);
    check(Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &again) && again != peer,
          "a retired native task address can acquire a fresh quarantined identity");
    Kaffeine_LockOwnerDrop(f.ctx, &child.pr_Task);
    f.ctx->broker_owner = &process.pr_Task;
    before = comparisons;
    check(!Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &again) &&
          !f.ctx->running && comparisons == before,
          "an RPC on an unexpected native task fails before comparing native locks");
    view(&process, 0x6000);
    f.ctx->running = TRUE;
}

static void bounded_process_owners()
{
    Fixture f;
    std::array<Process, KAFFEINE_LOCK_OWNER_LIMIT + 1> owners = {};
    f.ctx->m68k_sysbase = 0x4000;
    for (unsigned i = 0; i < owners.size(); ++i)
    {
        auto &p = owners[i]; p.pr_Task.tc_Node.ln_Type = NT_PROCESS;
        p.pr_CurrentDir = acquire(0, 500 + i); native_process = &p;
        f.ctx->guest_exec_task = 0x6000 + i * 0x100;
        f.ctx->broker_call = TRUE; f.ctx->broker_owner = &p.pr_Task;
        m68k_write32(f.ctx, 0x4000 + M68K_EXECBASE_THISTASK, f.ctx->guest_exec_task);
        ULONG token = 0;
        BOOL ok = Kaffeine_LockGetInstalled(f.ctx, KAFFEINE_LOCK_CURRENT, &token);
        check(i < KAFFEINE_LOCK_OWNER_LIMIT ? ok && token : !ok && !token && last_error == ERROR_NO_FREE_STORE,
              "installed owner table has a bounded capacity and atomic refusal");
    }
    for (auto &p : owners) Kaffeine_LockOwnerDrop(f.ctx, &p.pr_Task);
    native_process = &process;
    f.ctx->broker_call = FALSE; f.ctx->broker_owner = nullptr; f.ctx->guest_exec_task = 0;
}

/* Independent native lists exercise the production reader and real heap;
   no fabricated guest result is used as the expected native enumeration. */
struct DosListFixture : Fixture
{
    std::array<DosList, 8> entries = {};
    std::array<std::array<UBYTE, 304>, 8> names = {};
    std::array<UBYTE, 1025> path = {};
    DosListFixture()
    {
        task();
        const LONG types[] = {DLT_DEVICE, DLT_DIRECTORY, DLT_VOLUME, DLT_LATE,
                              DLT_NONBINDING, -1, 7, DLT_VOLUME};
        const char *texts[] = {"DH0", "SYS", "Work", "Late", "Path", "private", "invalid", "WORK"};
        std::memcpy(path.data(), "SYS:Tools", 10);
        for (unsigned i=0; i<entries.size(); ++i)
        {
            entries[i].dol_Type = types[i];
            entries[i].dol_Task = &handler;
            entries[i].dol_Lock = reinterpret_cast<BPTR>(0xFEDCBA9876543210ULL);
            set_name(i, texts[i]);
            if (i+1<entries.size()) entries[i].dol_Next = MKBADDR(&entries[i+1]);
            if (types[i] == DLT_VOLUME)
            {
                entries[i].dol_misc.dol_volume.dol_VolumeDate = {123, 4, 5};
                entries[i].dol_misc.dol_volume.dol_DiskType = 0x444F5301;
            }
            if (types[i] == DLT_LATE || types[i] == DLT_NONBINDING)
                entries[i].dol_misc.dol_assign.dol_AssignName = path.data();
        }
        entries[1].dol_misc.dol_assign.dol_AssignName = reinterpret_cast<UBYTE *>(1); // Wrong union arm must not be inspected.
        doslist_root.dol_Next = MKBADDR(entries.data());
    }
    void set_name(unsigned i, const char *text)
    {
        names[i].fill(0);
        size_t n=std::strlen(text);
        assert(n+2<names[i].size());
#ifdef AROS_FAST_BSTR
        std::memcpy(names[i].data(),text,n+1);
#else
        assert(n<=255); names[i][0]=UBYTE(n); std::memcpy(names[i].data()+1,text,n+1);
#endif
        entries[i].dol_Name=MKBADDR(names[i].data());
    }
    ULONG lock(ULONG flags=LDF_ALL|LDF_READ, bool attempt=false)
    { return Kaffeine_DosListLock(ctx,ctx->cpu,flags,attempt); }
    void unlock(ULONG flags=LDF_ALL|LDF_READ)
    { Kaffeine_DosListUnlock(ctx,ctx->cpu,flags); }
    ULONG next(ULONG g, ULONG flags=LDF_ALL)
    { return Kaffeine_DosListNext(ctx,ctx->cpu,g,flags); }
    ULONG find(ULONG g, const char *s, ULONG flags=LDF_ALL)
    { std::strcpy(reinterpret_cast<char *>(ctx->mem+0x7000),s); return Kaffeine_DosListFind(ctx,ctx->cpu,g,0x7000,flags); }
    std::vector<ULONG> walk(ULONG root, ULONG flags=LDF_ALL)
    {
        std::vector<ULONG> result;
        for (ULONG g=next(root,flags);g;g=next(g,flags)) { assert(result.size()<256);result.push_back(g); }
        return result;
    }
};

static void doslist_contract()
{
    DosListFixture f;
    for (ULONG flags : {0u, 3u, LDF_READ|0x80u})
        check(!f.lock(flags) && last_error==901 && !f.ctx->locks,"malformed flags leave IoErr and allocations untouched");
    check(!f.lock(LDF_WRITE|LDF_ALL) && last_error==ERROR_NOT_IMPLEMENTED,"write list lock is explicitly unsupported");
    last_error=901; f.unlock(); f.unlock(LDF_WRITE|LDF_ALL);
    check(!f.next(0) && f.ctx->running && last_error==901,"unchecked refusal/unmatched unlock path is harmless");
    ULONG root=f.lock(); auto nodes=f.walk(root);
    check(root && !(root&3) && nodes.size()==6 && doslist_calls==1 && !doslist_locked,"snapshot omits private/unknown types and releases native lock");
    const LONG types[]={0,1,2,3,4,2};
    ULONG linked=f.read(root)*4;
    bool layout=true;
    for (unsigned i=0;i<nodes.size();++i)
    {
        ULONG n=nodes[i], name=f.read(n+40)*4;
        layout &= n==linked && f.read(n+4)==ULONG(types[i]) && !f.read(n+12) && !(name&3);
        layout &= f.ctx->mem[name]>0 && f.ctx->mem[name+1+f.ctx->mem[name]]==0;
        linked=f.read(n)*4;
    }
    check(layout && !linked,"classic BPTR chain, APTR entries and terminated aligned BSTRs agree with native order");
    check(f.read(nodes[0]+8)==0x8000 && f.read(nodes[1]+8)==0x8000 && f.read(nodes[2]+8)==0x8000,"devices/assigns/volumes use the same typed handler identity");
    check(!f.read(nodes[3]+8) && !f.read(nodes[4]+8) && !std::strcmp(reinterpret_cast<char *>(f.ctx->mem+f.read(nodes[3]+16)),"SYS:Tools"),"late/nonbinding paths copied, never forged as mounted handlers");
    check(f.read(nodes[2]+16)==123 && f.read(nodes[2]+20)==4 && f.read(nodes[2]+24)==5 && f.read(nodes[2]+32)==0x444F5301 && !f.read(nodes[2]+28),"volume date/type scalars, with no native lock-list pointer");
    check(!f.read(nodes[0]+28) && !f.read(nodes[0]+32) && !f.read(nodes[0]+36) && !f.read(nodes[1]+16) && !f.read(nodes[1]+20),"untyped startup/segment/assign lock-list values are not exposed");
    check(f.walk(root,LDF_DEVICES).size()==1 && f.walk(root,LDF_ASSIGNS).size()==3 && f.walk(root,LDF_VOLUMES).size()==2 && f.walk(root,LDF_READ|LDF_WRITE|LDF_ENTRY).empty(),"all five native types have independent type filters");
    check(f.find(root,"sYs:",LDF_ASSIGNS)==nodes[1] && !f.find(root,"SY",LDF_ASSIGNS) && !f.find(root,"SYS",LDF_VOLUMES),"case/trailing colon match, exact length and type filter");
    check(f.find(root,"work")==nodes[2] && f.find(nodes[2],"Work")==nodes[5] && !f.find(nodes[5],"Work"),"Find continues after argument and handles duplicate volume names");
    ULONG lock_token=Kaffeine_LockAdopt(f.ctx,acquire());
    ULONG volume_name=f.read(f.read(lock_token*4+16)*4+40)*4+1;
    check(Kaffeine_DosListFind(f.ctx,f.ctx->cpu,root,volume_name,LDF_VOLUMES)==nodes[2] && Kaffeine_LockReadString(f.ctx,volume_name),"Find and DOS inputs accept the canonical volume name from a FileLock view");
    check(!f.find(0,"SYS") && !f.next(root+1) && !f.next(f.ctx->mem_size-1),"NULL/unaligned/foreign navigation refuses without native dereference");
    ULONG name=f.read(nodes[2]+40)*4+1;
    check(Kaffeine_LockProtected(f.ctx,root,44) && Kaffeine_LockProtected(f.ctx,name,5) && Kaffeine_LockReadString(f.ctx,name) && !std::strcmp(Kaffeine_LockReadString(f.ctx,name),"Work"),"canonical names are readable DOS inputs and not mutable output buffers");
    f.ctx->mem[name]='X';
    check(!Kaffeine_LockReadString(f.ctx,name),"edited read-only name is rejected before native DOS input");
    f.ctx->mem[name]='W';
    m68k_write32(f.ctx,nodes[0],0xffffffff); m68k_write32(f.ctx,nodes[0]+4,7); m68k_write32(f.ctx,nodes[0]+40,0xffffffff);
    check(f.next(nodes[0])==nodes[1] && f.find(root,"DH0")==nodes[0] && f.ctx->running,"navigation uses private identities despite edited node links/type/name");
    check(f.lock(LDF_VOLUMES|LDF_READ)==root && f.lock(LDF_ASSIGNS|LDF_READ,true)==root && f.lock(LDF_READ|LDF_ENTRY)==root && doslist_calls==1 && doslist_attempts==1,"mixed nested list bits use one snapshot without native calls");
    check(!f.lock(LDF_WRITE|LDF_ALL,true),"write attempt inside a read snapshot refuses");
    last_error=901; f.unlock(LDF_WRITE|LDF_ALL); f.unlock(); f.unlock(LDF_VOLUMES|LDF_READ); f.unlock(LDF_ASSIGNS|LDF_READ);
    check(f.lock(LDF_READ|LDF_DELETE)==root && doslist_calls==1,"refused write unlock does not consume read hold; ENTRY keeps snapshot");
    f.unlock(LDF_READ|LDF_ENTRY); f.unlock(LDF_READ|LDF_DELETE);
    check(!std::strcmp(reinterpret_cast<char *>(f.ctx->mem+name),"Work") && f.next(root)==nodes[0] && last_error==901,"final unlock retains names/navigation until next fresh lock and preserves IoErr");
    ULONG fresh=f.lock(LDF_READ);
    check(fresh && fresh!=root && doslist_calls==2 && f.next(root)==nodes[0],"fresh read-only lock retains previous walker generation");
    f.unlock(LDF_READ);
    fresh=f.lock(LDF_READ|LDF_ALL,true);
    check(fresh && doslist_attempts==3 && !f.next(root) && last_error==901,"fresh Attempt preserves IoErr and bounds retention to two generations");
    f.unlock();
    doslist_attempt_failure=true;
    check(!f.lock(LDF_READ|LDF_ALL,true) && f.next(fresh) && last_error==901 && !doslist_locked,"native Attempt refusal leaves retained snapshot and counters unchanged");
    doslist_attempt_failure=false;
    std::memset(f.ctx->mem+0x7000,'x',300);
    check(!Kaffeine_DosListFind(f.ctx,f.ctx->cpu,fresh,0x7000,LDF_ALL),"overlength or unterminated Find name is bounded");
    f.ctx->mem[f.ctx->mem_size-1]='x';
    check(!Kaffeine_DosListFind(f.ctx,f.ctx->cpu,fresh,f.ctx->mem_size-1,LDF_ALL),"Find at arena edge never crosses guest memory");
}

static void doslist_bounds_and_rollback()
{
    DosListFixture f;
    f.entries[7].dol_Task=nullptr;
    f.set_name(0,"\xC4");
    ULONG root=f.lock();auto nodes=f.walk(root);
    check(f.find(root,"\xE4")==nodes[0] && !f.read(nodes.back()+8),"native utility case semantics and unmounted volume preserved");
    f.unlock();
    root=f.lock();f.unlock(); // Reach the bounded two-generation steady state.
    /* AvailMem subtracts one header per free interval: the rotating pair
       can split a free interval, so neither that byte count nor the native
       metadata count must equal an intermediate contiguous arrangement.
       At task retirement every interval must coalesce back exactly. */
    f.ctx->doslist_task_removed(f.ctx,f.ctx->guest_exec_task);
    size_t baseline=allocations.size(), max_allocations=baseline;
    ULONG guest_free=Kaffeine_HeapAvail(f.ctx,0), min_free=guest_free;
    bool balanced=true;
    for (unsigned i=0;i<1000;++i)
    {
        ULONG g=f.lock();balanced &= g && f.walk(g).size()==6;f.unlock();
        if (allocations.size()>max_allocations) max_allocations=allocations.size();
        ULONG available=Kaffeine_HeapAvail(f.ctx,0);if(available<min_free)min_free=available;
        balanced &= !doslist_locked;
        if ((i+1)%100==0)
        {
            f.ctx->doslist_task_removed(f.ctx,f.ctx->guest_exec_task);
            balanced &= allocations.size()==baseline && Kaffeine_HeapAvail(f.ctx,0)==guest_free;
        }
    }
    std::printf("DosList memory oracle: baseline %zu, peak %zu native blocks; retained guest bytes <= %u; retirement restores %u bytes\n",
                baseline,max_allocations,guest_free-min_free,Kaffeine_HeapAvail(f.ctx,0));
    check(balanced,"1000 enumerations restore all native allocations and guest free memory at each task retirement");
    ULONG retained=f.lock();f.unlock();
    for (int fail=0;fail<4;++fail)
    {
        allocation_failure=fail;ULONG g=f.lock();allocation_failure=-1;
        if (!g) check(f.next(retained) && last_error==ERROR_NO_FREE_STORE && !doslist_locked,"allocation rollback retains old snapshot and releases all native locks");
        else {f.unlock();retained=g;}
    }
    handler_failure=true;
    check(!f.lock() && last_error==ERROR_NO_FREE_STORE && f.next(retained) && !doslist_locked,"handler mapping failure refuses whole snapshot without consuming old one");
    handler_failure=false;
    f.entries[7].dol_Next=MKBADDR(&f.entries[0]);
    check(!f.lock() && last_error==ERROR_NO_FREE_STORE && f.next(retained) && !doslist_locked,"cyclic native list refuses atomically and never loops");
    f.entries[7].dol_Next=nullptr;
    std::memset(f.path.data(),'x',1024);f.path[1024]=0;
    ULONG short_list=f.lock();
    check(f.walk(short_list).size()==4,"overlength late/nonbinding paths are omitted without truncation");f.unlock();
#ifdef AROS_FAST_BSTR
    f.names[0].fill('n');f.names[0][256]=0;
    ULONG no_device=f.lock();check(f.walk(no_device,LDF_DEVICES).empty(),"native FAST name over255 omitted rather than changed");f.unlock();
#endif
    f.names[0].fill(0); f.set_name(2,""); f.path[0]=0;
    ULONG empty=f.lock();check(f.walk(empty).size()==4,"empty native names are omitted; late NULL/empty path is bounded");f.unlock();
    root=f.lock();
    for (unsigned i=1;i<1024;++i) assert(f.lock()==root);
    check(!f.lock() && last_error==ERROR_NO_FREE_STORE,"bounded per-group nesting refuses overflow without wrapping");
    for (unsigned i=0;i<1024;++i)f.unlock();
    check(f.lock()!=root && f.ctx->running,"exactly balanced depth permits fresh snapshot");f.unlock();
    f.ctx->doslist_task_removed(f.ctx,f.ctx->guest_exec_task);
    check(!f.next(empty) && !Kaffeine_LockProtected(f.ctx,empty,44),"task retirement discards snapshot and its protected storage before identity reuse");
}

static void doslist_contention()
{
    DosListFixture f;
    doslist_reader_busy=true;
    ULONG root=f.lock();
#if !defined(__AROSEXEC_SMP__)
    check(root && doslist_shared==1 && !doslist_forbidden && !doslist_delays,"long native reader uses coherent shared copy without blocking context");
    f.unlock(LDF_ALL); // Native unlock accepts group bits with no mode bit.
    check(f.lock()!=root && doslist_shared==2,"mode-less unlock releases its hold and permits fresh enumeration");f.unlock();
    check(f.lock(LDF_ALL|LDF_READ,true) && doslist_shared==3 && !doslist_delays,"guest read Attempt coexists with native readers");f.unlock();
#else
    check(!root && last_error==ERROR_OBJECT_IN_USE && doslist_delays==24 && !doslist_shared,"SMP refuses bounded contention rather than using unsafe Forbid");
    doslist_reader_busy=false;root=f.lock();f.unlock();
#endif
    doslist_attempt_failure=true;unsigned delays=doslist_delays;
    check(!f.lock() && last_error==ERROR_OBJECT_IN_USE && doslist_delays-delays==24 && !doslist_locked && !doslist_forbidden,"persistent native contention has a finite 24-tick wait and no native lock leak");
    last_error=901;delays=doslist_delays;
    check(!f.lock(LDF_ALL|LDF_READ,true) && last_error==901 && doslist_delays==delays,"Attempt contention never waits or changes IoErr");
    doslist_attempt_failure=doslist_reader_busy=false;
}

static ULONG doslist_test_caller;
static ULONG doslist_caller(KaffeineContext *,void *) {return doslist_test_caller;}
static void doslist_task_isolation()
{
    DosListFixture f;
    KaffeineTaskOps ops={};ops.caller_task=doslist_caller;
    f.ctx->task_ops=&ops;f.ctx->sched=reinterpret_cast<KaffeineScheduler *>(1);
    doslist_test_caller=0x6000;ULONG a=f.lock();
    doslist_test_caller=0x6100;ULONG b=f.lock();
    check(a && b && a!=b && !f.next(a) && f.next(b),"private scheduler caller isolates snapshots between guest tasks");
    f.ctx->doslist_task_removed(f.ctx,0x6000);
    check(f.next(b) && !f.next(a),"removing another task keeps current snapshot alive");
    doslist_test_caller=0x6000;
    check(!f.next(a) && !f.find(a,"SYS"),"retired TCB identity cannot navigate old snapshot");
    unsigned calls=doslist_calls;
    f.ctx->task_ops=nullptr;
    check(!f.lock() && doslist_calls==calls && f.ctx->running,"missing trusted scheduler identity refuses before native list access");
    f.ctx->sched=nullptr;
}

static void doslist_capacity_and_names()
{
    DosListFixture f;
    std::array<char,256> long_name;long_name.fill('n');long_name[255]=0;
    f.set_name(0,long_name.data());
    f.entries[0].dol_misc.dol_handler.dol_Handler=f.entries[0].dol_Name;
    f.entries[0].dol_misc.dol_handler.dol_StackSize=32768;
    f.entries[0].dol_misc.dol_handler.dol_Priority=-3;
    ULONG root=f.lock(),node=f.next(root,LDF_DEVICES),name=f.read(node+40)*4,executable=f.read(node+16)*4;
    check(node && f.ctx->mem[name]==255 && !f.ctx->mem[name+256] && executable && f.ctx->mem[executable]==255 && !f.ctx->mem[executable+256],"255-byte names and device handler BSTR are preserved without overflow");
    check(f.read(node+20)==32768 && f.read(node+24)==ULONG(-3) && !f.read(node+28),"device stack/priority are scalars while startup stays private");
    f.unlock();
    std::vector<DosList> large(KAFFEINE_DOSLIST_LIMIT+1);
    for (size_t i=0;i<large.size();++i)
    {
        large[i].dol_Type=DLT_DEVICE;large[i].dol_Name=f.entries[0].dol_Name;
        if(i+1<large.size())large[i].dol_Next=MKBADDR(&large[i+1]);
    }
    doslist_root.dol_Next=MKBADDR(large.data());
    check(!f.lock() && last_error==ERROR_NO_FREE_STORE && f.next(root) && !doslist_locked,"entry capacity refuses complete publication and preserves retained snapshot");
    large[KAFFEINE_DOSLIST_LIMIT-1].dol_Next=nullptr;
    ULONG full=f.lock();
    check(f.walk(full).size()==KAFFEINE_DOSLIST_LIMIT,"exact supported capacity is fully enumerated");f.unlock();
    std::vector<DosList> invalid(1025);
    for(size_t i=0;i<invalid.size();++i){invalid[i].dol_Type=-1;if(i+1<invalid.size())invalid[i].dol_Next=MKBADDR(&invalid[i+1]);}
    doslist_root.dol_Next=MKBADDR(invalid.data());
    check(!f.lock() && last_error==ERROR_NO_FREE_STORE && f.next(full) && !doslist_locked,"walk bound also covers omitted/unknown native nodes");
    doslist_root.dol_Next=nullptr;
    ULONG empty=f.lock();check(empty && !f.next(empty) && !f.read(empty),"empty native list publishes valid empty pseudo-node");f.unlock();
}

int main()
{
    snapshots_and_identity(); scopes_and_refcounts(); edited_and_invalid_views();
    transfers_and_cleanup(); installed_lifetimes(); borrowed_install_and_failures();
    native_process_field_changes();
    publication_failures_and_limits(); access_refresh();
    separate_process_owners(); bounded_process_owners();
    doslist_contract(); doslist_bounds_and_rollback(); doslist_task_isolation();
    doslist_capacity_and_names();
    doslist_contention();
    std::printf("Typed guest lock regressions: %u/%u checks passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
