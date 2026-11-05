/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Typed classic FileLock views. Native BPTRs and filesystem keys remain in
   private metadata; guest fields are checked snapshots, never native inputs.
*/
#include "kaffeine_locks.h"
#include "kaffeine_handlers.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include "kaffeine_sched.h"
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LOCK_BYTES 20u
#define LOCK_SLOT_BYTES 24u
#define SLAB_BYTES 4096u
#define SLOTS_PER_SLAB (SLAB_BYTES / LOCK_SLOT_BYTES)
#define SLAB_LIMIT ((KAFFEINE_LOCK_IDENTITY_LIMIT + SLOTS_PER_SLAB - 1) / SLOTS_PER_SLAB)
#define VOLUME_BYTES 44u
#define VOLUME_NAME_BYTES 260u
#define VOLUME_STORAGE (VOLUME_BYTES + VOLUME_NAME_BYTES)
#define DOSLIST_BYTES 44u
#define DOSLIST_DEPTH 1024u
#define DOSLIST_WALK_LIMIT 1024u
#define DOSLIST_ATTEMPTS 25u
/* Also serializes native late-assign promotion, which uses a READ lock. */
#define DOSLIST_NATIVE_FLAGS (LDF_ALL | LDF_ENTRY | LDF_WRITE)

struct DosEntryCopy
{
    struct MsgPort *port;
    LONG type, days, minutes, ticks, disk_type, stack, priority;
    UBYTE name[257], handler[257], path[1024];
    ULONG name_len, handler_len, path_len;
};

struct DosListScratch
{
    /* One extra scratch entry lets omissions precede the capacity check. */
    struct DosEntryCopy entries[KAFFEINE_DOSLIST_LIMIT + 1];
};

struct DosEntryView
{
    LONG type;
    ULONG name, handler, path, name_len, handler_len, path_len;
};

struct DosSnapshot
{
    ULONG guest, size, count;
    struct DosEntryView entries[KAFFEINE_DOSLIST_LIMIT];
    /* Canonical classic bytes follow this private native header. */
};

struct DosOwner
{
    ULONG task, all, entry, deletion;
    struct DosSnapshot *snapshot;
    struct DosSnapshot *previous;
};

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

struct LockOwner
{
    struct Task *native;
    ULONG guest;
    struct LockInstalled installed[2];
};

struct KaffeineLocks
{
    struct LockRef refs[KAFFEINE_LOCK_LIMIT];
    struct LockVolume volumes[KAFFEINE_LOCK_VOLUME_LIMIT];
    struct LockBorrow borrows[KAFFEINE_LOCK_BORROW_LIMIT];
    struct LockOwner owners[KAFFEINE_LOCK_OWNER_LIMIT];
    ULONG slabs[SLAB_LIMIT], volume_storage[KAFFEINE_LOCK_IDENTITY_LIMIT], identities, key_serial;
    unsigned slab_count, volume_storage_count;
    struct DosOwner dosowners[KAFFEINE_LOCK_OWNER_LIMIT];
};

static void doslist_drop(struct KaffeineContext *, struct DosOwner *);

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
    if (ctx->broker_call && FindTask(NULL) != ctx->broker_owner)
        return Kaffeine_MessageFailure(ctx, "lock outside the active DOS owner"), FALSE;
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

/* The scheduler publishes the current Process view before a DOS RPC. Native
   installed references belong to that Process's broker, while caller/scoped
   lock tokens remain shared throughout this guest domain. */
static struct LockOwner *lock_owner(struct KaffeineContext *ctx, struct KaffeineLocks *s)
{
    struct Task *task = FindTask(NULL);
    unsigned i, spare = KAFFEINE_LOCK_OWNER_LIMIT;
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
    {
        struct LockOwner *owner = &s->owners[i];
        if (!owner->native)
        {
            if (spare == KAFFEINE_LOCK_OWNER_LIMIT) spare = i;
            continue;
        }
        if (owner->native == task && owner->guest == ctx->guest_exec_task) return owner;
        /* The first current/home snapshots are built before shadow_task()
           publishes its guest TCB. Bind that same native owner's startup
           record once the trusted Process view is available. */
        if (owner->native == task && !owner->guest)
        {
            owner->guest = ctx->guest_exec_task;
            return owner;
        }
        if (owner->native == task || (ctx->guest_exec_task && owner->guest == ctx->guest_exec_task))
            return Kaffeine_MessageFailure(ctx, "lock owner of another process"), NULL;
    }
    if (spare == KAFFEINE_LOCK_OWNER_LIMIT)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return NULL;
    }
    s->owners[spare].native = task;
    s->owners[spare].guest = ctx->guest_exec_task;
    return &s->owners[spare];
}

BOOL Kaffeine_LockGetInstalled(struct KaffeineContext *ctx, enum KaffeineLockInstalled which, ULONG *token)
{
    BPTR native;
    struct KaffeineLocks *s;
    struct LockOwner *owner;
    struct LockInstalled *slot;
    ULONG published = 0;
    int old;
    SIPTR error = IoErr();
    *token = 0;
    if (!context_ok(ctx) || !installed_native(ctx, which, &native) || !process_field_ok(ctx, which)) return FALSE;
    if (!native && !ctx->locks) { process_field(ctx, which, 0); SetIoErr(error); return TRUE; }
    s = state(ctx);
    if (!s) return FALSE;
    owner = lock_owner(ctx, s);
    if (!owner) return FALSE;
    slot = &owner->installed[which];
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
    struct LockOwner *owner;
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
    owner = lock_owner(ctx, s);
    if (!owner) return FALSE;
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
    old_native = owner->installed[which].native;
    returned = which == KAFFEINE_LOCK_CURRENT ? CurrentDir(native) : SetProgramDir(native);
    if (returned != old_native)
    {
        if (which == KAFFEINE_LOCK_CURRENT) CurrentDir(returned); else SetProgramDir(returned);
        if (duplicated) Kaffeine_LockRelease(ctx, token);
        return Kaffeine_MessageFailure(ctx, "native process lock changed during swap"), FALSE;
    }
    if (incoming >= 0) { --s->refs[incoming].caller; ++s->refs[incoming].installed; }
    if (outgoing >= 0) { --s->refs[outgoing].installed; ++s->refs[outgoing].caller; }
    owner->installed[which].known = TRUE;
    owner->installed[which].native = native;
    owner->installed[which].token = token;
    *previous = old_token;
    process_field(ctx, which, token);
    SetIoErr(error);
    return TRUE;
}

/* Called in the native owner before DOS releases its current/home locks.
   Only borrowed installed references go; caller/scoped references survive. */
void Kaffeine_LockOwnerDrop(struct KaffeineContext *ctx, struct Task *task)
{
    struct KaffeineLocks *s = ctx ? ctx->locks : NULL;
    SIPTR error = IoErr();
    unsigned i, which;
    if (!s || !task) return;
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
        if (s->owners[i].native == task)
        {
            for (which = 0; which < 2; ++which)
            {
                struct LockInstalled *slot = &s->owners[i].installed[which];
                int ref = slot->known ? find_ref(s, slot->token) : -1;
                if (ref >= 0 && s->refs[ref].installed)
                {
                    --s->refs[ref].installed;
                    retire(s, (unsigned)ref);
                }
            }
            memset(&s->owners[i], 0, sizeof(s->owners[i]));
        }
    SetIoErr(error);
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
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
    {
        struct DosSnapshot *current = s->dosowners[i].snapshot, *previous = s->dosowners[i].previous;
        if ((current && overlaps(a, n, current->guest, current->size)) ||
            (previous && overlaps(a, n, previous->guest, previous->size))) return TRUE;
    }
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
    ctx->doslist_task_removed = NULL;
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i) doslist_drop(ctx, &s->dosowners[i]);
    for (i = 0; i < s->slab_count; ++i) Kaffeine_MirrorRelease(ctx, s->slabs[i]);
    for (i = 0; i < s->volume_storage_count; ++i) Kaffeine_MirrorRelease(ctx, s->volume_storage[i]);
    FreeMem(s, sizeof(*s));
    SetIoErr(error);
}

/* DosList has a BPTR chain but all public entry points return APTRs.
   Native list pointers never cross this boundary. A copy needs either a
   short exclusive lease or, on a uniprocessor, READ plus Forbid. Never
   wait indefinitely for readers which may own an insert-volume requester. */
static ULONG doslist_caller(struct KaffeineContext *ctx, void *cpu)
{
    if (!ctx || !ctx->mem || cpu != ctx->cpu ||
        (ctx->broker_call && FindTask(NULL) != ctx->broker_owner)) return 0;
    if (ctx->sched)
        return ctx->task_ops && ctx->task_ops->caller_task ?
            ctx->task_ops->caller_task(ctx, cpu) : 0;
    return ctx->guest_exec_task &&
           arena(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4) &&
           m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) == ctx->guest_exec_task ?
           ctx->guest_exec_task : 0;
}

static struct DosOwner *doslist_owner(struct KaffeineContext *ctx, void *cpu, BOOL create)
{
    ULONG task = doslist_caller(ctx, cpu);
    struct KaffeineLocks *s;
    struct DosOwner *free_owner = NULL;
    unsigned i;
    if (!task) return NULL;
    s = create ? state(ctx) : ctx->locks;
    if (!s) return NULL;
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
    {
        if (s->dosowners[i].task == task) return &s->dosowners[i];
        if (!s->dosowners[i].task && !free_owner) free_owner = &s->dosowners[i];
    }
    if (create && free_owner) { free_owner->task = task; return free_owner; }
    if (create) SetIoErr(ERROR_NO_FREE_STORE);
    return NULL;
}

static void doslist_release(struct KaffeineContext *ctx, struct DosSnapshot *snapshot)
{
    if (snapshot)
    {
        Kaffeine_MirrorRelease(ctx, snapshot->guest);
        FreeMem(snapshot, sizeof(*snapshot) + snapshot->size);
    }
}

static void doslist_drop(struct KaffeineContext *ctx, struct DosOwner *owner)
{
    struct DosSnapshot *current = owner->snapshot, *previous = owner->previous;
    memset(owner, 0, sizeof(*owner));
    doslist_release(ctx, current);
    doslist_release(ctx, previous);
}

static void doslist_task_removed(struct KaffeineContext *ctx, ULONG task)
{
    unsigned i;
    SIPTR error = IoErr();
    if (ctx->locks)
        for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
            if (ctx->locks->dosowners[i].task == task)
                doslist_drop(ctx, &ctx->locks->dosowners[i]);
    SetIoErr(error);
}

static BOOL doslist_flags(ULONG flags)
{
    ULONG mode = flags & (LDF_READ | LDF_WRITE);
    return (mode == LDF_READ || mode == LDF_WRITE) &&
           !(flags & ~(LDF_READ | LDF_WRITE | LDF_ALL | LDF_ENTRY | LDF_DELETE));
}

static ULONG doslist_bstr(BSTR bstr, UBYTE *out)
{
    const UBYTE *p = (const UBYTE *)BADDR(bstr);
    ULONG n = 0;
    if (!p) return 0;
#ifdef AROS_FAST_BSTR
    while (n < 256 && p[n]) ++n;
    if (n > 255) return 0;
#else
    n = *p++;
#endif
    if (n) memcpy(out, p, n);
    out[n] = 0;
    return n;
}

static BOOL doslist_copy(struct DosListScratch *scratch, struct DosList *root, ULONG *count)
{
    const struct DosList *entry = (const struct DosList *)BADDR(root->dol_Next);
    ULONG visited = 0;
    *count = 0;
    while (entry)
    {
        LONG type;
        struct DosEntryCopy *copy;
        /* Also bounds cycles of omitted/private nodes, in linear time. */
        if (visited++ == DOSLIST_WALK_LIMIT) return FALSE;
        type = entry->dol_Type;
        if (type < DLT_DEVICE || type > DLT_NONBINDING) goto next;
        copy = &scratch->entries[*count];
        memset(copy, 0, offsetof(struct DosEntryCopy, name));
        copy->handler_len = copy->path_len = 0;
        copy->name_len = doslist_bstr(entry->dol_Name, copy->name);
        if (!copy->name_len) goto next;
        copy->type = type;
        if (type == DLT_LATE || type == DLT_NONBINDING)
        {
            const UBYTE *path = entry->dol_misc.dol_assign.dol_AssignName;
            if (path)
            {
                while (copy->path_len < sizeof(copy->path) && path[copy->path_len]) ++copy->path_len;
                if (copy->path_len == sizeof(copy->path)) goto next;
                memcpy(copy->path, path, copy->path_len + 1);
            }
        }
        else
        {
            copy->port = entry->dol_Task;
            if (type == DLT_VOLUME)
            {
                copy->days = entry->dol_misc.dol_volume.dol_VolumeDate.ds_Days;
                copy->minutes = entry->dol_misc.dol_volume.dol_VolumeDate.ds_Minute;
                copy->ticks = entry->dol_misc.dol_volume.dol_VolumeDate.ds_Tick;
                copy->disk_type = entry->dol_misc.dol_volume.dol_DiskType;
            }
            else if (type == DLT_DEVICE)
            {
                copy->handler_len = doslist_bstr(entry->dol_misc.dol_handler.dol_Handler, copy->handler);
                copy->stack = entry->dol_misc.dol_handler.dol_StackSize;
                copy->priority = entry->dol_misc.dol_handler.dol_Priority;
            }
        }
        if (*count == KAFFEINE_DOSLIST_LIMIT) return FALSE;
        ++*count;
next:
        entry = (const struct DosList *)BADDR(entry->dol_Next);
    }
    return TRUE;
}

static ULONG doslist_align(ULONG n) { return (n + 3u) & ~3u; }

static ULONG doslist_string(struct KaffeineContext *ctx, ULONG base, ULONG *offset,
                           const UBYTE *text, ULONG length, BOOL bstr)
{
    ULONG at = *offset;
    if (bstr) ctx->mem[base + at++] = (UBYTE)length;
    memcpy(ctx->mem + base + at, text, length);
    ctx->mem[base + at + length] = 0;
    *offset = doslist_align(at + length + 1);
    return at;
}

static struct DosSnapshot *doslist_snapshot(struct KaffeineContext *ctx, BOOL attempt)
{
    SIPTR error = IoErr();
    struct DosListScratch *scratch = AllocMem(sizeof(*scratch), MEMF_PRIVATE | MEMF_CLEAR);
    struct DosSnapshot *snapshot = NULL;
    struct DosList *native;
    ULONG count = 0, size, i, offset, native_flags = DOSLIST_NATIVE_FLAGS, attempts;
    BOOL existing_ports[KAFFEINE_MAX_HANDLERS];
    IPTR (*old_packet)(struct KaffeineContext *, ULONG, ULONG) = ctx->handler_packet;
    BOOL copied;
    if (!scratch) { SetIoErr(ERROR_NO_FREE_STORE); return NULL; }
    for (i = 0; i < KAFFEINE_MAX_HANDLERS; ++i) existing_ports[i] = ctx->handlers[i].guest != 0;
    native = NULL;
    for (attempts = 0; attempts < (attempt ? 1u : DOSLIST_ATTEMPTS); ++attempts)
    {
        native_flags = DOSLIST_NATIVE_FLAGS;
        native = AttemptLockDosList(native_flags);
#if !defined(__AROSEXEC_SMP__)
        /* A long-lived READ holder (e.g. an insert-volume requester) must
           not stall the remote context. On a uniprocessor, shared READ
           plus Forbid makes the bounded copy coherent without waiting for
           readers. SMP requires the exclusive attempt for that guarantee. */
        if (!native)
        {
            native_flags = LDF_ALL | LDF_READ;
            native = AttemptLockDosList(native_flags);
        }
#endif
        if (native) break;
        if (!attempt && attempts + 1 < DOSLIST_ATTEMPTS) Delay(1);
    }
    if (!native)
    {
        FreeMem(scratch, sizeof(*scratch));
        SetIoErr(attempt ? error : ERROR_OBJECT_IN_USE);
        return NULL;
    }
#if !defined(__AROSEXEC_SMP__)
    if (native_flags & LDF_READ) Forbid();
#endif
    copied = doslist_copy(scratch, native, &count);
#if !defined(__AROSEXEC_SMP__)
    if (native_flags & LDF_READ) Permit();
#endif
    UnLockDosList(native_flags);
    if (!copied) goto capacity;
    size = DOSLIST_BYTES * (count + 1);
    for (i = 0; i < count; ++i)
    {
        struct DosEntryCopy *copy = &scratch->entries[i];
        size += doslist_align(copy->name_len + 2);
        if (copy->handler_len) size += doslist_align(copy->handler_len + 2);
        if (copy->path_len) size += doslist_align(copy->path_len + 1);
    }
    snapshot = AllocMem(sizeof(*snapshot) + size, MEMF_PRIVATE | MEMF_CLEAR);
    if (!snapshot) goto capacity;
    snapshot->size = size;
    snapshot->count = count;
    snapshot->guest = Kaffeine_MirrorAlloc(ctx, size);
    if (!snapshot->guest) goto capacity;
    if (snapshot->guest & 3u) goto capacity;
    memset(ctx->mem + snapshot->guest, 0, size);
    m68k_write32(ctx, snapshot->guest, count ? (snapshot->guest + DOSLIST_BYTES) >> 2 : 0);
    offset = DOSLIST_BYTES * (count + 1);
    for (i = 0; i < count; ++i)
    {
        struct DosEntryCopy *copy = &scratch->entries[i];
        struct DosEntryView *view = &snapshot->entries[i];
        ULONG node = snapshot->guest + DOSLIST_BYTES * (i + 1);
        ULONG port = copy->port ? Kaffeine_HandlerPort(ctx, copy->port) : 0;
        if (copy->port && !port) goto capacity;
        view->type = copy->type;
        view->name_len = copy->name_len;
        view->name = doslist_string(ctx, snapshot->guest, &offset, copy->name, copy->name_len, TRUE);
        m68k_write32(ctx, node, i + 1 < count ? (node + DOSLIST_BYTES) >> 2 : 0);
        m68k_write32(ctx, node + 4, copy->type);
        m68k_write32(ctx, node + 8, port);
        m68k_write32(ctx, node + 40, (snapshot->guest + view->name - 1) >> 2);
        if (copy->type == DLT_VOLUME)
        {
            m68k_write32(ctx, node + 16, copy->days);
            m68k_write32(ctx, node + 20, copy->minutes);
            m68k_write32(ctx, node + 24, copy->ticks);
            m68k_write32(ctx, node + 32, copy->disk_type);
        }
        else if (copy->type == DLT_DEVICE)
        {
            m68k_write32(ctx, node + 20, copy->stack);
            m68k_write32(ctx, node + 24, copy->priority);
            if (copy->handler_len)
            {
                view->handler_len = copy->handler_len;
                view->handler = doslist_string(ctx, snapshot->guest, &offset, copy->handler, copy->handler_len, TRUE);
                m68k_write32(ctx, node + 16, (snapshot->guest + view->handler - 1) >> 2);
            }
        }
        else if (copy->path_len)
        {
            view->path_len = copy->path_len;
            view->path = doslist_string(ctx, snapshot->guest, &offset, copy->path, copy->path_len, FALSE);
            m68k_write32(ctx, node + 16, snapshot->guest + view->path);
        }
    }
    memcpy(snapshot + 1, ctx->mem + snapshot->guest, size);
    FreeMem(scratch, sizeof(*scratch));
    return snapshot;
capacity:
    /* New handler views have not escaped this synchronous publication.
       Roll back their allocations too; pre-existing identities survive. */
    for (i = 0; i < KAFFEINE_MAX_HANDLERS; ++i)
        if (!existing_ports[i] && ctx->handlers[i].guest)
        {
            Kaffeine_MirrorAbort(ctx, ctx->handlers[i].guest);
            ctx->handlers[i].guest = 0;
            ctx->handlers[i].native = NULL;
        }
    ctx->handler_packet = old_packet;
    if (snapshot)
    {
        if (snapshot->guest) Kaffeine_MirrorAbort(ctx, snapshot->guest);
        FreeMem(snapshot, sizeof(*snapshot) + snapshot->size);
    }
    FreeMem(scratch, sizeof(*scratch));
    SetIoErr(ERROR_NO_FREE_STORE);
    return NULL;
}

ULONG Kaffeine_DosListLock(struct KaffeineContext *ctx, void *cpu, ULONG flags, BOOL attempt)
{
    SIPTR error = IoErr();
    struct DosOwner *owner;
    struct DosSnapshot *snapshot;
    if (!doslist_flags(flags)) return 0;
    if (flags & LDF_WRITE) { SetIoErr(ERROR_NOT_IMPLEMENTED); return 0; }
    owner = doslist_owner(ctx, cpu, TRUE);
    if (!owner) return 0;
    ctx->doslist_task_removed = doslist_task_removed;
    if (((flags & LDF_ALL) && owner->all == DOSLIST_DEPTH) ||
        ((flags & LDF_ENTRY) && owner->entry == DOSLIST_DEPTH) ||
        ((flags & LDF_DELETE) && owner->deletion == DOSLIST_DEPTH))
    { SetIoErr(ERROR_NO_FREE_STORE); return 0; }
    if (!owner->all && !owner->entry && !owner->deletion)
    {
        snapshot = doslist_snapshot(ctx, attempt);
        if (!snapshot)
        {
            if (!owner->snapshot) memset(owner, 0, sizeof(*owner));
            return 0;
        }
        /* Keep one previous generation for a READ-alone walker interrupted
           by a library helper's fresh lock. Older generations are bounded,
           not a growing publication quarantine. */
        {
            struct DosSnapshot *obsolete = owner->previous;
            owner->previous = NULL;
            doslist_release(ctx, obsolete);
        }
        owner->previous = owner->snapshot;
        owner->snapshot = snapshot;
    }
    if (flags & LDF_ALL) ++owner->all;
    if (flags & LDF_ENTRY) ++owner->entry;
    if (flags & LDF_DELETE) ++owner->deletion;
    SetIoErr(error);
    return owner->snapshot->guest;
}

void Kaffeine_DosListUnlock(struct KaffeineContext *ctx, void *cpu, ULONG flags)
{
    struct DosOwner *owner;
    /* Native UnLock uses group bits, including when mode bits are absent. */
    if (flags & LDF_WRITE) return;
    owner = doslist_owner(ctx, cpu, FALSE);
    if (!owner) return;
    if ((flags & LDF_ALL) && owner->all) --owner->all;
    if ((flags & LDF_ENTRY) && owner->entry) --owner->entry;
    if ((flags & LDF_DELETE) && owner->deletion) --owner->deletion;
}

static BOOL doslist_start(struct DosSnapshot *snapshot, ULONG entry, ULONG *index)
{
    ULONG delta;
    if (!snapshot || !entry || entry < snapshot->guest) return FALSE;
    delta = entry - snapshot->guest;
    if (delta % DOSLIST_BYTES || delta / DOSLIST_BYTES > snapshot->count) return FALSE;
    *index = delta / DOSLIST_BYTES;
    return TRUE;
}

static BOOL doslist_matches(LONG type, ULONG flags)
{
    return flags & (type == DLT_DEVICE ? LDF_DEVICES : type == DLT_VOLUME ? LDF_VOLUMES : LDF_ASSIGNS);
}

static struct DosSnapshot *doslist_lookup(struct DosOwner *owner, ULONG entry, ULONG *index)
{
    if (!owner) return NULL;
    if (doslist_start(owner->snapshot, entry, index)) return owner->snapshot;
    if (doslist_start(owner->previous, entry, index)) return owner->previous;
    return NULL;
}

ULONG Kaffeine_DosListNext(struct KaffeineContext *ctx, void *cpu, ULONG entry, ULONG flags)
{
    struct DosOwner *owner = doslist_owner(ctx, cpu, FALSE);
    struct DosSnapshot *snapshot;
    ULONG i;
    snapshot = doslist_lookup(owner, entry, &i);
    if (!snapshot) return 0;
    for (; i < snapshot->count; ++i)
        if (doslist_matches(snapshot->entries[i].type, flags))
            return snapshot->guest + (i + 1) * DOSLIST_BYTES;
    return 0;
}

ULONG Kaffeine_DosListFind(struct KaffeineContext *ctx, void *cpu, ULONG entry, ULONG name, ULONG flags)
{
    struct DosOwner *owner = doslist_owner(ctx, cpu, FALSE);
    struct DosSnapshot *snapshot;
    const char *text, *end;
    ULONG i, limit, length;
    snapshot = doslist_lookup(owner, entry, &i);
    if (!snapshot || !arena(ctx, name, 1)) return 0;
    text = Kaffeine_LockReadString(ctx, name);
    if (text)
    {
        limit = (ULONG)strlen(text) + 1;
        if (limit > 257) limit = 257;
    }
    else
    {
        if (Kaffeine_LockProtected(ctx, name, 1)) return 0;
        text = (const char *)ctx->mem + name;
        limit = ctx->mem_size - name;
        if (limit > 257) limit = 257;
    }
    end = memchr(text, 0, limit);
    if (!end) return 0;
    length = (ULONG)(end - text);
    if (length && text[length - 1] == ':') --length;
    for (; i < snapshot->count; ++i)
    {
        struct DosEntryView *view = &snapshot->entries[i];
        const char *candidate = (const char *)(snapshot + 1) + view->name;
        if (doslist_matches(view->type, flags) && view->name_len == length &&
            !Strnicmp(text, candidate, (LONG)length))
            return snapshot->guest + (i + 1) * DOSLIST_BYTES;
    }
    return 0;
}

const char *Kaffeine_LockReadString(struct KaffeineContext *ctx, ULONG address)
{
    unsigned i, j, k, generation;
    if (!ctx || !ctx->locks) return NULL;
    for (i = 0; i < KAFFEINE_LOCK_VOLUME_LIMIT; ++i)
    {
        const struct LockVolume *volume = &ctx->locks->volumes[i];
        ULONG start = volume->guest + VOLUME_BYTES + 1, size;
        if (!volume->refs || address < start || address - start > volume->snapshot[VOLUME_BYTES]) continue;
        size = volume->snapshot[VOLUME_BYTES] - (address - start) + 1;
        if (arena(ctx, address, size) && !memcmp(ctx->mem + address,
                volume->snapshot + VOLUME_BYTES + 1 + (address - start), size))
            return (const char *)volume->snapshot + VOLUME_BYTES + 1 + (address - start);
    }
    for (i = 0; i < KAFFEINE_LOCK_OWNER_LIMIT; ++i)
    for (generation = 0; generation < 2; ++generation)
    {
        struct DosSnapshot *snapshot = generation ? ctx->locks->dosowners[i].previous : ctx->locks->dosowners[i].snapshot;
        ULONG relative;
        if (!snapshot || address < snapshot->guest || address - snapshot->guest >= snapshot->size) continue;
        relative = address - snapshot->guest;
        for (j = 0; j < snapshot->count; ++j)
        {
            const struct DosEntryView *view = &snapshot->entries[j];
            ULONG offsets[3] = {view->name, view->handler, view->path};
            ULONG lengths[3] = {view->name_len, view->handler_len, view->path_len};
            for (k = 0; k < 3; ++k)
                if (offsets[k] && relative >= offsets[k] && relative - offsets[k] <= lengths[k])
                {
                    const UBYTE *canonical = (const UBYTE *)(snapshot + 1) + relative;
                    ULONG size = lengths[k] - (relative - offsets[k]) + 1;
                    return arena(ctx, address, size) && !memcmp(ctx->mem + address, canonical, size) ?
                        (const char *)canonical : NULL;
                }
        }
    }
    return NULL;
}
