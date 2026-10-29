/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
*/
/* kaffeine_shadow.c — Generic struct shadow engine + generated layout tables */

#include <aros/debug.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stddef.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_shadow.h"
#include "kaffeine_cli.h"
#include "kaffeine_tasks.h"
#include "kaffeine_sched.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_handlers.h"

/* Keep registry ownership with the engine that creates and destroys shadows. */
ULONG shadow_register(struct KaffeineContext *ctx, ULONG m68k_addr, void *native, UWORD type)
{
    if (ctx->num_shadows < KAFFEINE_MAX_SHADOWS) {
        ctx->shadow_map[ctx->num_shadows].m68k_addr = m68k_addr;
        ctx->shadow_map[ctx->num_shadows].native_ptr = native;
        ctx->shadow_map[ctx->num_shadows].type = type;
        ctx->num_shadows++;
        return m68k_addr;
    }
    bug("[kaffeine] shadow_register: shadow table full (%u entries), cannot register 0x%lx\n",
        (unsigned)KAFFEINE_MAX_SHADOWS, (unsigned long)m68k_addr);
    return 0;
}

void *shadow_lookup(struct KaffeineContext *ctx, ULONG m68k_addr)
{
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == m68k_addr)
            return ctx->shadow_map[i].native_ptr;
    return NULL;
}

void shadow_remove(struct KaffeineContext *ctx, ULONG m68k_addr)
{
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == m68k_addr) {
            ctx->shadow_map[i] = ctx->shadow_map[--ctx->num_shadows];
            return;
        }
}

/* ── Generated layout tables (179 structs, 766 fields) ── */
#include "kaffeine_shadow_layouts.h"

/* ── Engine ── */

static void sync_fields(struct KaffeineContext *ctx,
                        const struct M68KFieldMap *fields,
                        ULONG m68k_base, void *native)
{
    for (const struct M68KFieldMap *f = fields; f->type != SF_END; f++)
    {
        UBYTE *src = (UBYTE *)native + f->native_off;
        switch (f->type) {
        case SF_BYTE:
            ctx->mem[m68k_base + f->m68k_off] = *src;
            break;
        case SF_WORD:
            m68k_write16(ctx, m68k_base + f->m68k_off, *(UWORD *)src);
            break;
        case SF_LONG:
            m68k_write32(ctx, m68k_base + f->m68k_off, *(ULONG *)src);
            break;
        case SF_PTR: {
            void *ptr = *(void **)src;
            if (!ptr) { m68k_write32(ctx, m68k_base + f->m68k_off, 0); break; }
            /* Check if this native pointer already has a shadow */
            ULONG existing = 0;
            for (UWORD i = 0; i < ctx->num_shadows; i++)
                if (ctx->shadow_map[i].native_ptr == ptr &&
                    ctx->shadow_map[i].type == f->shadow_type)
                    { existing = ctx->shadow_map[i].m68k_addr; break; }
            if (existing) {
                m68k_write32(ctx, m68k_base + f->m68k_off, existing);
            } else if (f->sub_layout) {
                /* Known struct type — create a proper sub-shadow */
                ULONG mp = Kaffeine_HeapAlloc(ctx, f->sub_layout->m68k_size, 0);
                if (mp) {
                    shadow_register(ctx, mp, ptr, f->shadow_type);
                    m68k_write32(ctx, m68k_base + f->m68k_off, mp);
                    sync_fields(ctx, f->sub_layout->fields, mp, ptr);
                } else {
                    m68k_write32(ctx, m68k_base + f->m68k_off, 0);
                }
            } else {
                /* Unknown struct — write 0 rather than an empty placeholder
                   that m68k code would dereference into garbage */
                m68k_write32(ctx, m68k_base + f->m68k_off, 0);
            }
            break;
        }
        }
    }
}

ULONG shadow_create(struct KaffeineContext *ctx,
                    const struct M68KStructLayout *layout,
                    void *native)
{
    ULONG m = Kaffeine_HeapAlloc(ctx, layout->m68k_size, 0);
    if (!m) return 0;
    if (!shadow_register(ctx, m, native, layout->shadow_type)) {
        Kaffeine_HeapFree(ctx, m, layout->m68k_size);
        return 0;
    }
    sync_fields(ctx, layout->fields, m, native);
    return m;
}

void shadow_sync(struct KaffeineContext *ctx,
                 const struct M68KStructLayout *layout,
                 ULONG m68k_addr, void *native)
{
    sync_fields(ctx, layout->fields, m68k_addr, native);
}

void shadow_destroy(struct KaffeineContext *ctx,
                    const struct M68KStructLayout *layout,
                    ULONG m68k_addr)
{
    shadow_remove(ctx, m68k_addr);
    Kaffeine_HeapFree(ctx, m68k_addr, layout->m68k_size);
}

/* ── Layout lookup by name ── */

const struct M68KStructLayout *shadow_find_layout(const char *name)
{
    for (int i = 0; shadow_gen_layouts[i].name; i++) {
        if (strcmp(shadow_gen_layouts[i].name, name) == 0)
            return &shadow_gen_layouts[i];
    }
    return NULL;
}

/* ── Convenience: create shadow by struct name ── */

ULONG shadow_create_by_name(struct KaffeineContext *ctx,
                            const char *struct_name, void *native)
{
    const struct M68KStructLayout *layout =
        !strcmp(struct_name, "FileLock") ? NULL : shadow_find_layout(struct_name);
    if (!layout) {
        D(bug("[kaffeine] shadow_create_by_name: unknown struct '%s'\n", struct_name));
        return 0;
    }
    return shadow_create(ctx, layout, native);
}


/* ── m68k→native struct translation ── */

/* Convert m68k TagItem list (8 bytes per entry) to native (16 bytes on 64-bit).
   String-valued tags are resolved to host pointers.
   Caller must free_native_taglist / FreeVec the returned array. Returns NULL on failure. */
struct TagItem *m68k_to_native_taglist(struct KaffeineContext *ctx, ULONG m68k_addr)
{
    if (!m68k_addr) return NULL;

    /* Count tags */
    int n = 0;
    ULONG p = m68k_addr;
    while (m68k_read32(ctx, p) != TAG_DONE) { n++; p += 8; }
    n++; /* include TAG_DONE */

    struct TagItem *ht = (struct TagItem *)AllocVec(n * sizeof(struct TagItem), MEMF_CLEAR);
    if (!ht) return NULL;

    p = m68k_addr;
    for (int i = 0; i < n; i++) {
        ht[i].ti_Tag = m68k_read32(ctx, p);
        ULONG v = m68k_read32(ctx, p + 4);
        /* Heuristic: tags with pointer values need host resolution */
        if (v >= 0x100 && v < ctx->mem_size)
            ht[i].ti_Data = (IPTR)m68k_to_host(ctx, v);
        else
            ht[i].ti_Data = (IPTR)v;
        p += 8;
    }
    return ht;
}

void free_native_taglist(struct TagItem *tags)
{
    if (tags) FreeVec(tags);
}

/* Translate an m68k struct to a native struct using a layout table.
   Allocates and returns a native struct. Caller must FreeMem it. */
void *m68k_to_native_struct(struct KaffeineContext *ctx,
                            const char *struct_name, ULONG m68k_addr)
{
    const struct M68KStructLayout *layout =
        !strcmp(struct_name, "FileLock") ? NULL : shadow_find_layout(struct_name);
    if (!layout || !m68k_addr) return NULL;

    /* Find native size from the gen table */
    ULONG native_size = 0;
    for (int i = 0; shadow_gen_layouts[i].name; i++) {
        if (strcmp(shadow_gen_layouts[i].name, struct_name) == 0) {
            /* We don't store native_size in the layout, estimate from last field */
            native_size = layout->m68k_size * 3; /* conservative overestimate */
            break;
        }
    }
    if (!native_size) return NULL;

    void *native = AllocMem(native_size, MEMF_CLEAR);
    if (!native) return NULL;

    /* Reverse sync: read m68k fields, write to native offsets */
    for (const struct M68KFieldMap *f = layout->fields; f->type != SF_END; f++) {
        UBYTE *dst = (UBYTE *)native + f->native_off;
        switch (f->type) {
        case SF_BYTE:
            *dst = ctx->mem[m68k_addr + f->m68k_off];
            break;
        case SF_WORD:
            *(UWORD *)dst = m68k_read16(ctx, m68k_addr + f->m68k_off);
            break;
        case SF_LONG:
            *(ULONG *)dst = m68k_read32(ctx, m68k_addr + f->m68k_off);
            break;
        case SF_PTR: {
            ULONG mptr = m68k_read32(ctx, m68k_addr + f->m68k_off);
            if (mptr)
                *(void **)dst = m68k_to_host_or_shadow(ctx, mptr);
            break;
        }
        }
    }
    return native;
}

/* ── ExecBase shadow init ── */

static BOOL task_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && size <= ctx->mem_size && addr <= ctx->mem_size - size;
}

ULONG shadow_task(struct KaffeineContext *ctx, void *native_task, BOOL current)
{
    struct Task *task = (struct Task *)native_task;
    const struct M68KStructLayout *layout = shadow_find_layout("Task");
    ULONG guest;
    UWORD i;

    if (!task || !layout)
        return 0;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].native_ptr == task && ctx->shadow_map[i].type == layout->shadow_type)
        {
            guest = ctx->shadow_map[i].m68k_addr;
            return task_range(ctx, guest, layout->m68k_size) ? guest : 0;
        }
    /* Existing identities remain usable when full. A new task fails before
       allocating, so no unregistered or unstable shadow can escape. */
    if (ctx->num_shadows == KAFFEINE_MAX_SHADOWS)
        return 0;
    guest = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_PROCESS, 0);
    if (!task_range(ctx, guest, M68K_SIZEOF_PROCESS))
        return 0;
    if (!shadow_register(ctx, guest, task, layout->shadow_type))
    {
        Kaffeine_HeapFree(ctx, guest, M68K_SIZEOF_PROCESS);
        return 0;
    }
    /* The heap can return dirty memory. Foreign tasks get zero Process fields,
       never this context's CLI, arguments or native streams. */
    memset(ctx->mem + guest, 0, M68K_SIZEOF_PROCESS);
    shadow_sync(ctx, layout, guest, task);
    if (current && ctx->stack_bottom && ctx->stack_bottom < ctx->stack_top &&
        ctx->stack_top <= ctx->mem_size && ctx->stack_top - ctx->stack_bottom >= 4 &&
        !((ctx->stack_bottom | ctx->stack_top) & 3))
    {
        /* The classic Process describes the stack used by guest code,
           independently of the native Shell/emulator process stack. Its
           BPTR StackBase names the high end, as dos/dosextens.h specifies. */
        m68k_write32(ctx, guest + M68K_TASK_SPREG, ctx->stack_top - 4);
        m68k_write32(ctx, guest + M68K_TASK_SPLOWER, ctx->stack_bottom);
        m68k_write32(ctx, guest + M68K_TASK_SPUPPER, ctx->stack_top);
        if (task->tc_Node.ln_Type == NT_PROCESS)
        {
            m68k_write32(ctx, guest + M68K_PR_STACKSIZE, ctx->stack_top - ctx->stack_bottom);
            m68k_write32(ctx, guest + M68K_PR_STACKBASE, ctx->stack_top >> 2);
        }
    }
    if (current && task->tc_Node.ln_Type == NT_PROCESS)
    {
        ULONG current_lock = 0, program_lock = 0;
        if (!Kaffeine_LockGetInstalled(ctx, KAFFEINE_LOCK_CURRENT, &current_lock) ||
            !Kaffeine_LockGetInstalled(ctx, KAFFEINE_LOCK_PROGRAM, &program_lock)) return 0;
        m68k_write32(ctx, guest + M68K_PR_CURRENTDIR, current_lock);
        m68k_write32(ctx, guest + M68K_PR_HOMEDIR, program_lock);
        m68k_write32(ctx, guest + M68K_PR_CLI, Kaffeine_GetCLI(ctx) >> 2);
        m68k_write32(ctx, guest + M68K_PR_ARGUMENTS, ctx->m68k_argptr);
        m68k_write32(ctx, guest + M68K_PR_CIS, Kaffeine_FileImport(ctx, Input(), FALSE));
        m68k_write32(ctx, guest + M68K_PR_COS, Kaffeine_FileImport(ctx, Output(), FALSE));
        /* The Shell sets the error stream for *>; libnix takes stderr from
           here and opens the console only when it is zero. */
        m68k_write32(ctx, guest + M68K_PR_CES, Kaffeine_FileImport(ctx, ((struct Process *)task)->pr_CES, FALSE));
        /* The console and filesystem tasks are guest ports of the native
           handlers, which take packets (kaffeine_handlers.c). */
        m68k_write32(ctx, guest + M68K_PR_CONSOLETASK,
                     Kaffeine_HandlerPort(ctx, ((struct Process *)task)->pr_ConsoleTask));
        m68k_write32(ctx, guest + M68K_PR_FILESYSTEMTASK,
                     Kaffeine_HandlerPort(ctx, ((struct Process *)task)->pr_FileSystemTask));
        /* pr_MsgPort is the program's packet port, an empty guest queue with
           the SIGB_DOS of the native process, which the bridge neither
           allocates nor frees. */
        ctx->mem[guest + M68K_PR_MSGPORT + 8] = NT_MSGPORT;
        ctx->mem[guest + M68K_PR_MSGPORT + 14] = PA_SIGNAL;
        ctx->mem[guest + M68K_PR_MSGPORT + 15] = SIGB_DOS;
        m68k_write32(ctx, guest + M68K_PR_MSGPORT + 16, guest);
        m68k_write32(ctx, guest + M68K_PR_MSGPORT + 20, guest + M68K_PR_MSGPORT + 24);
        m68k_write32(ctx, guest + M68K_PR_MSGPORT + 24, 0);
        m68k_write32(ctx, guest + M68K_PR_MSGPORT + 28, guest + M68K_PR_MSGPORT + 20);
    }
    return guest;
}

/* -294: FindTask(A1=name) -> D0=stable guest Task address. */
IPTR Kaffeine_FindTask(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name_addr = THUNK_A(1);
    const char *name;

    if (!name_addr)
    {
        /* The context can execute in a different native process from the one
           that prepared it. Its guest identity is always the ExecBase field. */
        if (!task_range(ctx, ctx->m68k_sysbase, M68K_EXECBASE_THISTASK + 4))
            return 0;
        return m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK);
    }
    if (!task_range(ctx, name_addr, 1) ||
        !memchr(ctx->mem + name_addr, 0, ctx->mem_size - name_addr))
        return 0;
    name = (const char *)ctx->mem + name_addr;
    /* The program's own AddTask() children come first; none is native. */
    if (ctx->sched)
    {
        ULONG child = ctx->task_ops->find_named(ctx, name);

        if (child)
            return child;
    }
    return shadow_task(ctx, FindTask(name), FALSE);
}

/* ExecBase embedded list offsets (m68k -> AROS) */
static const struct { UWORD m68k_off; UWORD native_off; const char *name; } execbase_lists[] = {
    { M68K_EXECBASE_MEMLIST,       624, "MemList" },
    { M68K_EXECBASE_RESOURCELIST,  656, "ResourceList" },
    { M68K_EXECBASE_DEVICELIST,    688, "DeviceList" },
    { M68K_EXECBASE_INTRLIST,      720, "IntrList" },
    { M68K_EXECBASE_LIBLIST,       752, "LibList" },
    { M68K_EXECBASE_TASKREADY,     816, "TaskReady" },
    { M68K_EXECBASE_TASKWAIT,      848, "TaskWait" },
    {   0,    0, NULL },
};

/* Classic programs also inspect MemList directly to determine installed
   Chip RAM. The unified guest heap is chip-accessible; expose its actual
   two possible intervals, excluding the custom-chip window and stack.
   These are capacity-only descriptors: native headers and allocator free
   links remain private, and AvailMem is the supported free-space query. */
static void memory_capacity(struct KaffeineContext *ctx, ULONG list)
{
    ULONG bounds[4], start = CUSTOM_BASE, end = CUSTOM_BASE + CUSTOM_SIZE;
    ULONG previous = list, node;
    unsigned i, count = 0;

    if (ctx->heap_start >= ctx->heap_end || ctx->heap_end > ctx->mem_size)
        return;
    if (!ctx->memory_info)
    {
        node = Kaffeine_HeapAlloc(ctx, KAFFEINE_MEMINFO_SIZE, MEMF_CLEAR);
        if (!node) return;
        ctx->memory_info = node;
    }
    if (!task_range(ctx, ctx->memory_info, KAFFEINE_MEMINFO_SIZE))
        return;
    if (start < ctx->heap_start) start = ctx->heap_start;
    if (end > ctx->heap_end) end = ctx->heap_end;
    if (start >= end) start = end = ctx->heap_end;
    bounds[0] = ctx->heap_start; bounds[1] = start;
    bounds[2] = end; bounds[3] = ctx->heap_end;
    memset(ctx->mem + ctx->memory_info, 0, KAFFEINE_MEMINFO_SIZE);
    memcpy(ctx->mem + ctx->memory_info + 64, "Kaffeine guest arena", 21);
    for (i = 0; i < 4; i += 2)
    {
        if (bounds[i] == bounds[i + 1]) continue;
        node = ctx->memory_info + 32 * count++;
        m68k_write32(ctx, previous, node);
        m68k_write32(ctx, node, list + 4);
        m68k_write32(ctx, node + 4, previous);
        ctx->mem[node + 8] = 10; /* classic NT_MEMORY */
        m68k_write32(ctx, node + 10, ctx->memory_info + 64);
        m68k_write16(ctx, node + 14, MEMF_PUBLIC | MEMF_CHIP);
        m68k_write32(ctx, node + 20, bounds[i]);
        m68k_write32(ctx, node + 24, bounds[i + 1]);
        previous = node;
    }
    m68k_write32(ctx, list + 8, previous);
}

ULONG shadow_init_execbase(struct KaffeineContext *ctx, ULONG m68k_base, void *native_sysbase)
{
    const struct M68KStructLayout *layout = shadow_find_layout("ExecBase");
    if (!layout || !task_range(ctx, m68k_base, layout->m68k_size)) return 0;

    if (!shadow_register(ctx, m68k_base, native_sysbase, layout->shadow_type)) return 0;
    sync_fields(ctx, layout->fields, m68k_base, native_sysbase);

    /* Override AttnFlags: the 68040 integer unit without an FPU, as Moira
       runs it (see M68K_ATTNFLAGS_68040) */
    m68k_write16(ctx, m68k_base + M68K_EXECBASE_ATTNFLAGS, M68K_ATTNFLAGS_68040);

    /* Generated pointer fields without a sub-layout are zeroed by sync_fields.
       Publish the same registered snapshot that named FindTask lookups reuse. */
    ctx->guest_exec_task = shadow_task(ctx, FindTask(NULL), TRUE);
    m68k_write32(ctx, m68k_base + M68K_EXECBASE_THISTASK, ctx->guest_exec_task);

    /* This snapshot starts outside any guest critical section, regardless
       of the native nesting while the snapshot is constructed. */
    ctx->mem[m68k_base + KAFFEINE_EXECBASE_IDNESTCNT] = 0xff;
    ctx->mem[m68k_base + KAFFEINE_EXECBASE_TDNESTCNT] = 0xff;

    /* Public guest objects cannot share links with native Exec lists. */
    {
        const UWORD offsets[] = { M68K_EXECBASE_SEMAPHORELIST, M68K_EXECBASE_PORTLIST };
        const UBYTE types[] = { NT_SIGNALSEM, NT_MSGPORT };
        unsigned i;

        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        {
            ULONG list = m68k_base + offsets[i];

            m68k_write32(ctx, list, list + 4);
            m68k_write32(ctx, list + 4, 0);
            m68k_write32(ctx, list + 8, list);
            ctx->mem[list + 12] = types[i];
            ctx->mem[list + 13] = 0;
        }
    }

    /* Library node fields */
    struct Library *lib = (struct Library *)native_sysbase;
    m68k_write16(ctx, m68k_base + M68K_EXECBASE_LIBVERSION, lib->lib_Version);
    m68k_write16(ctx, m68k_base + M68K_EXECBASE_LIBREVISION, lib->lib_Revision);

    /* The native nodes have no guest representation. Direct guest walks
       must see an empty Exec list, whose head points at its tail sentinel,
       rather than follow an untranslated NULL head through address zero.
       Retain each native list's identity for intercepted named lookups. */
    for (int i = 0; execbase_lists[i].name; i++) {
        struct List *native_list = (struct List *)((UBYTE *)native_sysbase + execbase_lists[i].native_off);
        ULONG list = m68k_base + execbase_lists[i].m68k_off;

        m68k_write32(ctx, list, list + 4);
        m68k_write32(ctx, list + 4, 0);
        m68k_write32(ctx, list + 8, list);
        ctx->mem[list + 12] = native_list->lh_Type;
        ctx->mem[list + 13] = 0;
        shadow_register(ctx, list, native_list, 17);
    }

    memory_capacity(ctx, m68k_base + M68K_EXECBASE_MEMLIST);

    return m68k_base;
}
