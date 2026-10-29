/* Copyright (C) 2026, The AROS Development Team. All rights reserved.

   Typed classic FileLock views. Native BPTRs and filesystem keys remain in
   private metadata; guest fields are checked snapshots, never native inputs.
*/
#include "kaffeine_locks.h"
#include "kaffeine_handlers.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdint.h>
#include <string.h>

#define LOCK_BYTES 20u
#define LOCK_SLOT_BYTES 24u
#define SLAB_BYTES 4096u
#define SLOTS_PER_SLAB (SLAB_BYTES / LOCK_SLOT_BYTES)
#define SLAB_LIMIT ((KAFFEINE_LOCK_IDENTITY_LIMIT + SLOTS_PER_SLAB - 1) / SLOTS_PER_SLAB)
#define VOLUME_BYTES 44u
#define VOLUME_NAME_BYTES 260u
#define VOLUME_STORAGE (VOLUME_BYTES + VOLUME_NAME_BYTES)

struct LockRef
{
    BPTR native;
    ULONG guest, key, caller, borrowed, installed, pending;
    int volume;
    UBYTE snapshot[LOCK_BYTES];
};

struct LockVolume
{
    BPTR native;
    ULONG guest, refs;
    UBYTE snapshot[VOLUME_STORAGE];
};

struct LockBorrow
{
    enum KaffeineLockScope kind;
    ULONG owner;
    unsigned slot;
};

struct LockInstalled
{
    BPTR native;
    ULONG token;
    BOOL known;
};

struct KaffeineLocks
{
    struct LockRef refs[KAFFEINE_LOCK_LIMIT];
    struct LockVolume volumes[KAFFEINE_LOCK_VOLUME_LIMIT];
    struct LockBorrow borrows[KAFFEINE_LOCK_BORROW_LIMIT];
    struct LockInstalled installed[2];
    ULONG slabs[SLAB_LIMIT], volume_storage[KAFFEINE_LOCK_IDENTITY_LIMIT], identities, key_serial;
    unsigned slab_count, volume_storage_count;
};

struct NativeVolume
{
    LONG days, minutes, ticks, disk_type;
    UBYTE length, name[255];
};

static BOOL arena(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    return ctx && ctx->mem && a && a < ctx->mem_size && n <= ctx->mem_size - a;
}

static BOOL context_ok(struct KaffeineContext *ctx)
{
    if (!ctx || !ctx->mem)
        return FALSE;
    if (ctx->guest_exec_task &&
        (!arena(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4) ||
         m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) != ctx->guest_exec_task))
    {
        Kaffeine_MessageFailure(ctx, "lock outside the program's process");
        return FALSE;
    }
    return TRUE;
}

static struct KaffeineLocks *state(struct KaffeineContext *ctx)
{
    if (!ctx->locks)
        ctx->locks = (struct KaffeineLocks *)AllocMem(sizeof(*ctx->locks), MEMF_PRIVATE | MEMF_CLEAR);
    if (!ctx->locks)
        SetIoErr(ERROR_NO_FREE_STORE);
    return ctx->locks;
}

static int find_ref(struct KaffeineLocks *s, ULONG token)
{
    unsigned i;
    if (!s || !token || (token >> 30)) return -1;
    for (i = 0; i < KAFFEINE_LOCK_LIMIT; ++i)
        if (s->refs[i].native && s->refs[i].guest == (token << 2)) return (int)i;
    return -1;
}

static BOOL volume_ok(struct KaffeineContext *ctx, struct LockVolume *v)
{
    return v->refs && arena(ctx, v->guest, VOLUME_STORAGE) &&
           !memcmp(ctx->mem + v->guest, v->snapshot, VOLUME_STORAGE);
}

static BOOL view_ok(struct KaffeineContext *ctx, struct LockRef *r)
{
    return r->native && arena(ctx, r->guest, LOCK_BYTES) &&
           !memcmp(ctx->mem + r->guest, r->snapshot, LOCK_BYTES) &&
           (r->volume < 0 || volume_ok(ctx, &ctx->locks->volumes[r->volume]));
}

static void retire(struct KaffeineLocks *s, unsigned slot)
{
    struct LockRef *r = &s->refs[slot];
    if (r->caller || r->borrowed || r->installed || r->pending) return;
    if (r->native && r->volume >= 0)
    {
        struct LockVolume *v = &s->volumes[r->volume];
        if (--v->refs == 0) memset(v, 0, sizeof(*v));
    }
    /* The guest bytes remain quarantined, but their identity is retired. */
    memset(r, 0, sizeof(*r));
}

static BOOL native_volume(BPTR native, struct NativeVolume *out)
{
    const struct DeviceList *v = (const struct DeviceList *)BADDR(native);
    const UBYTE *name;
    ULONG length = 0;
    BOOL ok = TRUE;
    memset(out, 0, sizeof(*out));
    /* Volume/name lifetime is protected while making a bounded copy. */
    LockDosList(LDF_VOLUMES | LDF_READ);
    if (v->dl_Type != DLT_VOLUME)
        ok = FALSE;
    else
    {
        out->days = v->dl_VolumeDate.ds_Days;
        out->minutes = v->dl_VolumeDate.ds_Minute;
        out->ticks = v->dl_VolumeDate.ds_Tick;
        out->disk_type = v->dl_DiskType;
        name = (const UBYTE *)BADDR(v->dl_Name);
        if (name)
        {
#ifdef AROS_FAST_BSTR
            while (length < 255 && name[length]) ++length;
            if (length == 255 && name[length]) ok = FALSE;
#else
            length = *name++;
#endif
            if (ok && length) memcpy(out->name, name, length);
            out->length = (UBYTE)length;
        }
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return ok;
}

/* A first publication is staged before its table entries become visible.
   Only a successful Adopt adds a native reference owned by this module. */
static ULONG publish(struct KaffeineContext *ctx, BPTR native, BOOL own,
                       enum KaffeineLockScope kind, ULONG owner, BOOL installed)
{
    struct KaffeineLocks *s;
    const struct FileLock *n;
    struct NativeVolume nv;
    struct LockRef temp;
    unsigned i, slot = KAFFEINE_LOCK_LIMIT, borrow = KAFFEINE_LOCK_BORROW_LIMIT;
    ULONG guest, handler, volume_guest = 0, new_slab = 0, key = 0;
    int volume = -1;
    BOOL new_volume = FALSE, added_slab = FALSE;
    SIPTR error = IoErr();

    if (!context_ok(ctx)) return 0;
    s = state(ctx);
    if (!s) return 0;
    if (owner)
    {
        for (i = 0; i < KAFFEINE_LOCK_BORROW_LIMIT; ++i)
            if (!s->borrows[i].owner && borrow == KAFFEINE_LOCK_BORROW_LIMIT) borrow = i;
    }
    for (i = 0; i < KAFFEINE_LOCK_LIMIT; ++i)
    {
        struct LockRef *r = &s->refs[i];
        if (!r->native) { if (slot == KAFFEINE_LOCK_LIMIT) slot = i; continue; }
        if (r->native == native)
        {
            if (!view_ok(ctx, r))
            {
                Kaffeine_MessageFailure(ctx, "modified lock snapshot");
                return 0;
            }
            if (own && r->caller == UINT32_MAX) goto capacity;
            if (owner)
            {
                unsigned j;
                for (j = 0; j < KAFFEINE_LOCK_BORROW_LIMIT; ++j)
                    if (s->borrows[j].owner == owner && s->borrows[j].kind == kind &&
                        s->borrows[j].slot == i) break;
                if (j == KAFFEINE_LOCK_BORROW_LIMIT)
                {
                    if (borrow == KAFFEINE_LOCK_BORROW_LIMIT) goto capacity;
                    s->borrows[borrow].owner = owner;
                    s->borrows[borrow].kind = kind;
                    s->borrows[borrow].slot = i;
                    ++r->borrowed;
                }
            }
            if (own) ++r->caller;
            if (installed) ++r->installed;
            SetIoErr(error);
            return r->guest >> 2;
        }
    }
    if (slot == KAFFEINE_LOCK_LIMIT || s->identities == KAFFEINE_LOCK_IDENTITY_LIMIT ||
        (owner && borrow == KAFFEINE_LOCK_BORROW_LIMIT)) goto capacity;
    n = (const struct FileLock *)BADDR(native);
    /* SameLock compares native live handles, never native or guest key bits. */
    for (i = 0; i < KAFFEINE_LOCK_LIMIT; ++i)
        if (s->refs[i].native)
        {
            LONG same = SameLock(native, s->refs[i].native);
            if (same == LOCK_SAME)
                key = s->refs[i].key;
            if ((same == LOCK_SAME || same == LOCK_SAME_VOLUME) && s->refs[i].volume >= 0)
                volume = s->refs[i].volume;
            if (key) break;
        }
    if (!key) key = s->key_serial + 1;
    if (n->fl_Volume)
    {
        if (volume < 0)
            for (i = 0; i < KAFFEINE_LOCK_VOLUME_LIMIT; ++i)
                if (s->volumes[i].refs && s->volumes[i].native == n->fl_Volume) { volume = (int)i; break; }
        if (volume >= 0)
        {
            if (!volume_ok(ctx, &s->volumes[volume]))
            {
                Kaffeine_MessageFailure(ctx, "modified volume snapshot");
                return 0;
            }
            volume_guest = s->volumes[volume].guest;
        }
        else
        {
            for (i = 0; i < KAFFEINE_LOCK_VOLUME_LIMIT && s->volumes[i].refs; ++i) ;
            if (i == KAFFEINE_LOCK_VOLUME_LIMIT ||
                s->volume_storage_count == KAFFEINE_LOCK_IDENTITY_LIMIT) goto capacity;
            volume = (int)i;
            if (!native_volume(n->fl_Volume, &nv))
            {
                SetIoErr(ERROR_OBJECT_WRONG_TYPE);
                return 0;
            }
            volume_guest = Kaffeine_MirrorAlloc(ctx, VOLUME_STORAGE);
            if (!volume_guest) goto capacity;
            new_volume = TRUE;
        }
    }
    else volume = -1;
    if (s->identities % SLOTS_PER_SLAB == 0)
    {
        new_slab = Kaffeine_MirrorAlloc(ctx, SLAB_BYTES);
        if (!new_slab) goto rollback;
        added_slab = TRUE;
    }
    guest = (added_slab ? new_slab : s->slabs[s->slab_count - 1]) +
            (s->identities % SLOTS_PER_SLAB) * LOCK_SLOT_BYTES;
    handler = n->fl_Task ? Kaffeine_HandlerPort(ctx, n->fl_Task) : 0;
    if (!n->fl_Task || !handler) goto rollback;
    if (new_volume)
    {
        struct LockVolume *v = &s->volumes[volume];
        memset(ctx->mem + volume_guest, 0, VOLUME_STORAGE);
        m68k_write32(ctx, volume_guest + 4, DLT_VOLUME);
        m68k_write32(ctx, volume_guest + 8, handler);
        m68k_write32(ctx, volume_guest + 16, (ULONG)nv.days);
        m68k_write32(ctx, volume_guest + 20, (ULONG)nv.minutes);
        m68k_write32(ctx, volume_guest + 24, (ULONG)nv.ticks);
        m68k_write32(ctx, volume_guest + 32, (ULONG)nv.disk_type);
        m68k_write32(ctx, volume_guest + 40, (volume_guest + VOLUME_BYTES) >> 2);
        ctx->mem[volume_guest + VOLUME_BYTES] = nv.length;
        memcpy(ctx->mem + volume_guest + VOLUME_BYTES + 1, nv.name, nv.length);
        v->guest = volume_guest;
        v->native = n->fl_Volume;
        memcpy(v->snapshot, ctx->mem + volume_guest, VOLUME_STORAGE);
        s->volume_storage[s->volume_storage_count++] = volume_guest;
    }
    memset(&temp, 0, sizeof(temp));
    temp.native = native; temp.guest = guest; temp.key = key; temp.volume = volume;
    temp.caller = own ? 1 : 0;
    temp.borrowed = owner ? 1 : 0;
    temp.installed = installed ? 1 : 0;
    memset(ctx->mem + guest, 0, LOCK_SLOT_BYTES);
    m68k_write32(ctx, guest + 4, key);
    m68k_write32(ctx, guest + 8, (ULONG)n->fl_Access);
    m68k_write32(ctx, guest + 12, handler);
    m68k_write32(ctx, guest + 16, volume_guest >> 2);
    memcpy(temp.snapshot, ctx->mem + guest, LOCK_BYTES);
    s->refs[slot] = temp;
    if (volume >= 0) ++s->volumes[volume].refs;
    if (owner)
    {
        s->borrows[borrow].kind = kind;
        s->borrows[borrow].owner = owner;
        s->borrows[borrow].slot = slot;
    }
    if (added_slab) s->slabs[s->slab_count++] = new_slab;
    ++s->identities;
    if (key > s->key_serial) s->key_serial = key;
    SetIoErr(error);
    return guest >> 2;

rollback:
    if (new_slab) Kaffeine_MirrorRelease(ctx, new_slab);
    if (new_volume) Kaffeine_MirrorRelease(ctx, volume_guest);
capacity:
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
}

ULONG Kaffeine_LockAdopt(struct KaffeineContext *ctx, BPTR native)
{
    ULONG token;
    SIPTR error;
    if (!native) return 0;
    token = publish(ctx, native, TRUE, KAFFEINE_LOCK_LEGACY, 0, FALSE);
    if (token) return token;
    error = IoErr();
    UnLock(native);
    SetIoErr(error);
    return 0;
}

ULONG Kaffeine_LockBorrow(struct KaffeineContext *ctx, BPTR native,
                          enum KaffeineLockScope kind, ULONG owner)
{
    if (!native) return 0;
    if (!owner || kind < KAFFEINE_LOCK_DEVPROC || kind > KAFFEINE_LOCK_LEGACY)
    {
        Kaffeine_MessageFailure(ctx, "lock borrow owner");
        return 0;
    }
    return publish(ctx, native, FALSE, kind, owner, FALSE);
}

void Kaffeine_LockScopeDrop(struct KaffeineContext *ctx, enum KaffeineLockScope kind, ULONG owner)
{
    struct KaffeineLocks *s = ctx->locks;
    unsigned i;
    if (!s || !owner) return;
    for (i = 0; i < KAFFEINE_LOCK_BORROW_LIMIT; ++i)
        if (s->borrows[i].owner == owner && s->borrows[i].kind == kind)
        {
            unsigned slot = s->borrows[i].slot;
            memset(&s->borrows[i], 0, sizeof(s->borrows[i]));
            --s->refs[slot].borrowed;
            retire(s, slot);
        }
}

BOOL Kaffeine_LockResolve(struct KaffeineContext *ctx, ULONG token, BPTR *native)
{
    int i;
    *native = (BPTR)0;
    if (!context_ok(ctx)) return FALSE;
    if (!token) return TRUE;
    i = find_ref(ctx->locks, token);
    if (i < 0 || !view_ok(ctx, &ctx->locks->refs[i]))
    {
        Kaffeine_MessageFailure(ctx, "invalid or modified lock");
        return FALSE;
    }
    *native = ctx->locks->refs[i].native;
    return TRUE;
}

BOOL Kaffeine_LockRelease(struct KaffeineContext *ctx, ULONG token)
{
    BPTR native;
    int i;
    SIPTR error;
    if (!Kaffeine_LockResolve(ctx, token, &native)) return FALSE;
    if (!token) return TRUE;
    i = find_ref(ctx->locks, token);
    if (!ctx->locks->refs[i].caller)
    {
        Kaffeine_MessageFailure(ctx, "UnLock of a borrowed or installed lock");
        return FALSE;
    }
    --ctx->locks->refs[i].caller;
    error = IoErr();
    UnLock(native);
    SetIoErr(error);
    retire(ctx->locks, (unsigned)i);
    return TRUE;
}

BOOL Kaffeine_LockForHandler(struct KaffeineContext *ctx, ULONG token, struct MsgPort *handler, BPTR *native)
{
    if (!Kaffeine_LockResolve(ctx, token, native)) return FALSE;
    if (*native && ((const struct FileLock *)BADDR(*native))->fl_Task != handler)
    {
        *native = (BPTR)0;
        Kaffeine_MessageFailure(ctx, "lock of another handler");
        return FALSE;
    }
    return TRUE;
}

BOOL Kaffeine_LockRefreshAccess(struct KaffeineContext *ctx, ULONG token)
{
    BPTR native;
    int i;
    SIPTR error = IoErr();
    if (!Kaffeine_LockResolve(ctx, token, &native)) return FALSE;
    if (!token) { SetIoErr(error); return TRUE; }
    i = find_ref(ctx->locks, token);
    m68k_write32(ctx, ctx->locks->refs[i].guest + 8,
                  (ULONG)((const struct FileLock *)BADDR(native))->fl_Access);
    memcpy(ctx->locks->refs[i].snapshot, ctx->mem + ctx->locks->refs[i].guest, LOCK_BYTES);
    SetIoErr(error);
    return TRUE;
}

BOOL Kaffeine_LockTransferBegin(struct KaffeineContext *ctx, ULONG token, struct KaffeineLockTransfer *lease)
{
    BPTR native;
    int i;
    memset(lease, 0, sizeof(*lease));
    if (!Kaffeine_LockResolve(ctx, token, &native)) return FALSE;
    if (!token) return TRUE;
    i = find_ref(ctx->locks, token);
    if (!ctx->locks->refs[i].caller)
    {
        Kaffeine_MessageFailure(ctx, "transfer of a borrowed or installed lock");
        return FALSE;
    }
    --ctx->locks->refs[i].caller;
    ++ctx->locks->refs[i].pending;
    lease->native = native; lease->token = token; lease->slot = (unsigned)i; lease->active = TRUE;
    return TRUE;
}

void Kaffeine_LockTransferEnd(struct KaffeineContext *ctx, struct KaffeineLockTransfer *lease, BOOL consumed)
{
    struct LockRef *r;
    if (!lease->active) return;
    if (!ctx->locks || lease->slot >= KAFFEINE_LOCK_LIMIT ||
        (r = &ctx->locks->refs[lease->slot])->guest >> 2 != lease->token ||
        r->native != lease->native || !r->pending)
    {
        Kaffeine_MessageFailure(ctx, "invalid lock transfer lease");
        return;
    }
    --r->pending;
    if (!consumed) ++r->caller;
    lease->active = FALSE;
    retire(ctx->locks, lease->slot);
}

static BOOL installed_native(struct KaffeineContext *ctx, enum KaffeineLockInstalled which, BPTR *native)
{
    struct Task *task;
    if (which != KAFFEINE_LOCK_CURRENT && which != KAFFEINE_LOCK_PROGRAM)
        return Kaffeine_MessageFailure(ctx, "lock process slot"), FALSE;
    task = FindTask(NULL);
    if (!task || task->tc_Node.ln_Type != NT_PROCESS)
        return Kaffeine_MessageFailure(ctx, "lock process field outside a process"), FALSE;
    *native = which == KAFFEINE_LOCK_CURRENT ? ((struct Process *)task)->pr_CurrentDir : GetProgramDir();
    return TRUE;
}

static BOOL process_field_ok(struct KaffeineContext *ctx, enum KaffeineLockInstalled which)
{
    ULONG offset = which == KAFFEINE_LOCK_CURRENT ? M68K_PR_CURRENTDIR : M68K_PR_HOMEDIR;
    if (ctx->guest_exec_task && !arena(ctx, ctx->guest_exec_task, offset + 4))
        return Kaffeine_MessageFailure(ctx, "lock process snapshot bounds"), FALSE;
    return TRUE;
}

static void process_field(struct KaffeineContext *ctx, enum KaffeineLockInstalled which, ULONG token)
{
    if (ctx->guest_exec_task)
        m68k_write32(ctx, ctx->guest_exec_task +
            (which == KAFFEINE_LOCK_CURRENT ? M68K_PR_CURRENTDIR : M68K_PR_HOMEDIR), token);
}

BOOL Kaffeine_LockGetInstalled(struct KaffeineContext *ctx, enum KaffeineLockInstalled which, ULONG *token)
{
    BPTR native;
    struct KaffeineLocks *s;
    struct LockInstalled *slot;
    ULONG published = 0;
    int old;
    SIPTR error = IoErr();
    *token = 0;
    if (!context_ok(ctx) || !installed_native(ctx, which, &native) || !process_field_ok(ctx, which)) return FALSE;
    if (!native && !ctx->locks) { process_field(ctx, which, 0); SetIoErr(error); return TRUE; }
    s = state(ctx);
    if (!s) return FALSE;
    slot = &s->installed[which];
    if (slot->known && slot->native == native)
    {
        if (native)
        {
            old = find_ref(s, slot->token);
            if (old < 0 || !view_ok(ctx, &s->refs[old]))
                return Kaffeine_MessageFailure(ctx, "modified installed lock"), FALSE;
        }
        *token = slot->token;
        process_field(ctx, which, *token);
        SetIoErr(error);
        return TRUE;
    }
    /* Native code can replace a process field without using this bridge.
       Its displaced reference may already have been unlocked. Drop that
       ownership before publish() compares live handles with SameLock().
       Independent caller/scoped/other-slot references keep their record. */
    old = find_ref(s, slot->token);
    if (slot->known && old >= 0)
    {
        --s->refs[old].installed;
        retire(s, (unsigned)old);
    }
    memset(slot, 0, sizeof(*slot));
    if (native)
    {
        published = publish(ctx, native, FALSE, KAFFEINE_LOCK_PROCESS, 0, TRUE);
        if (!published) return FALSE;
    }
    slot->known = TRUE; slot->native = native; slot->token = published;
    *token = published;
    process_field(ctx, which, published);
    SetIoErr(error);
    return TRUE;
}

BOOL Kaffeine_LockSwapInstalled(struct KaffeineContext *ctx, enum KaffeineLockInstalled which,
                                 ULONG token, ULONG *previous)
{
    struct KaffeineLocks *s;
    BPTR native, old_native, returned;
    ULONG old_token;
    int incoming, outgoing;
    BOOL duplicated = FALSE;
    SIPTR error = IoErr();
    *previous = 0;
    if (!Kaffeine_LockGetInstalled(ctx, which, &old_token) ||
        !Kaffeine_LockResolve(ctx, token, &native)) return FALSE;
    if (token == old_token) { *previous = old_token; return TRUE; }
    s = state(ctx);
    if (!s) return FALSE;
    incoming = find_ref(s, token);
    outgoing = find_ref(s, old_token);
    if (incoming >= 0 && !s->refs[incoming].caller)
    {
        BPTR copy;
        if (!s->refs[incoming].borrowed && !s->refs[incoming].installed)
            return Kaffeine_MessageFailure(ctx, "installing a lock owned by another process field"), FALSE;
        copy = DupLock(native);
        if (!copy) return FALSE;
        token = Kaffeine_LockAdopt(ctx, copy);
        if (!token) return FALSE;
        incoming = find_ref(s, token);
        native = s->refs[incoming].native;
        duplicated = TRUE;
    }
    if (outgoing >= 0 && s->refs[outgoing].caller == UINT32_MAX)
    {
        if (duplicated) Kaffeine_LockRelease(ctx, token);
        SetIoErr(ERROR_NO_FREE_STORE);
        return FALSE;
    }
    old_native = s->installed[which].native;
    returned = which == KAFFEINE_LOCK_CURRENT ? CurrentDir(native) : SetProgramDir(native);
    if (returned != old_native)
    {
        if (which == KAFFEINE_LOCK_CURRENT) CurrentDir(returned); else SetProgramDir(returned);
        if (duplicated) Kaffeine_LockRelease(ctx, token);
        return Kaffeine_MessageFailure(ctx, "native process lock changed during swap"), FALSE;
    }
    if (incoming >= 0) { --s->refs[incoming].caller; ++s->refs[incoming].installed; }
    if (outgoing >= 0) { --s->refs[outgoing].installed; ++s->refs[outgoing].caller; }
    s->installed[which].known = TRUE;
    s->installed[which].native = native;
    s->installed[which].token = token;
    *previous = old_token;
    process_field(ctx, which, token);
    SetIoErr(error);
    return TRUE;
}

static BOOL overlaps(ULONG a, ULONG n, ULONG b, ULONG size)
{
    return n && size && (a <= b ? b - a < n : a - b < size);
}

BOOL Kaffeine_LockProtected(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    struct KaffeineLocks *s = ctx->locks;
    unsigned i;
    if (!s) return FALSE;
    for (i = 0; i < s->slab_count; ++i)
        if (overlaps(a, n, s->slabs[i], SLAB_BYTES)) return TRUE;
    for (i = 0; i < s->volume_storage_count; ++i)
        if (overlaps(a, n, s->volume_storage[i], VOLUME_STORAGE)) return TRUE;
    return FALSE;
}

void Kaffeine_LockCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineLocks *s = ctx->locks;
    unsigned i;
    SIPTR error;
    if (!s) return;
    for (i = 0; i < KAFFEINE_LOCK_LIMIT; ++i)
        if (s->refs[i].pending)
        { Kaffeine_MessageFailure(ctx, "lock cleanup during native transfer"); return; }
    error = IoErr();
    for (i = 0; i < KAFFEINE_LOCK_LIMIT; ++i)
        while (s->refs[i].caller)
        { --s->refs[i].caller; UnLock(s->refs[i].native); }
    ctx->locks = NULL;
    for (i = 0; i < s->slab_count; ++i) Kaffeine_MirrorRelease(ctx, s->slabs[i]);
    for (i = 0; i < s->volume_storage_count; ++i) Kaffeine_MirrorRelease(ctx, s->volume_storage[i]);
    FreeMem(s, sizeof(*s));
    SetIoErr(error);
}
