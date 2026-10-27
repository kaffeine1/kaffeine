/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    icon.library DiskObjects as metadata-only 68k snapshots. The program gets
    a guest copy of the classic DiskObject with its strings and ToolTypes;
    the native object stays private behind the owner table and every guest
    pointer field that would refer to native data is zero.
*/
#include "kaffeine_intern.h"
#include "kaffeine_icon.h"
#include <aros/debug.h>
#include <exec/memory.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/icon.h>
#include <string.h>

#define ICON_STRING_MAX   4096    /* bytes including the NUL */
#define ICON_TOOLTYPE_MAX 128     /* entries before the terminator */
#define ICON_SNAPSHOT_MAX 65536
#define ICON_HEADER       80      /* classic 78 bytes, padded for the array */

/* Classic 68k DiskObject layout (workbench/workbench.h, intuition.h) */
#define DO_MAGIC          0
#define DO_VERSION        2
#define DO_GADGET         4
#define GG_LEFTEDGE       8
#define GG_TOPEDGE        10
#define GG_WIDTH          12
#define GG_HEIGHT         14
#define GG_FLAGS          16
#define GG_ACTIVATION     18
#define GG_GADGETTYPE     20
#define GG_MUTUALEXCLUDE  34
#define GG_GADGETID       42
#define DO_TYPE           48
#define DO_DEFAULTTOOL    50
#define DO_TOOLTYPES      54
#define DO_CURRENTX       58
#define DO_CURRENTY       62
#define DO_DRAWERDATA     66
#define DO_TOOLWINDOW     70
#define DO_STACKSIZE      74

static IPTR stop(struct KaffeineContext *ctx, const char *reason)
{
    bug("[kaffeine] icon boundary: %s, stopping (20)\n", reason);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* A NUL-terminated guest string of at most ICON_STRING_MAX bytes, wholly
   inside guest memory; its length goes to *len. */
static BOOL guest_string(struct KaffeineContext *ctx, ULONG addr, ULONG *len)
{
    ULONG n, limit;
    if (!ctx->mem || !addr || addr >= ctx->mem_size) return FALSE;
    limit = ctx->mem_size - addr;
    if (limit > ICON_STRING_MAX) limit = ICON_STRING_MAX;
    for (n = 0; n < limit; ++n)
        if (!ctx->mem[addr + n])
        {
            *len = n;
            return TRUE;
        }
    return FALSE;
}

/* Length of a trusted native string, bounded like a guest one. */
static BOOL native_string(CONST_STRPTR s, ULONG *len)
{
    ULONG n = 0;
    while (n < ICON_STRING_MAX && s[n]) ++n;
    if (n == ICON_STRING_MAX) return FALSE;
    *len = n;
    return TRUE;
}

static BOOL grow(ULONG *size, ULONG add)
{
    if (add > ICON_SNAPSHOT_MAX || *size > ICON_SNAPSHOT_MAX - add) return FALSE;
    *size += add;
    return TRUE;
}

static BOOL measure(CONST_STRPTR s, ULONG *size, ULONG *len)
{
    return native_string(s, len) && grow(size, *len + 1);
}

/* Copy a measured native string to guest offset *at; its guest address. */
static ULONG place(struct KaffeineContext *ctx, ULONG guest, ULONG *at, CONST_STRPTR s, ULONG len)
{
    ULONG addr = guest + *at;
    memcpy(ctx->mem + addr, s, len + 1);
    *at += len + 1;
    return addr;
}

/* Publish a guest snapshot of a newly acquired native DiskObject. On any
   failure the native object is freed once and the program gets 0. */
static IPTR publish(struct KaffeineContext *ctx, struct DiskObject *dob)
{
    struct KaffeineIcon *slot = NULL;
    ULONG size = ICON_HEADER, count = 0, guest, at, array = 0, i;
    ULONG default_len = 0, window_len = 0, type_lens[ICON_TOOLTYPE_MAX];
    if (!dob) return 0;
    for (i = 0; i < KAFFEINE_MAX_ICONS && !slot; ++i)
        if (!ctx->icons[i].guest && !ctx->icons[i].native) slot = &ctx->icons[i];
    if (!slot) goto fail;
    if (dob->do_DefaultTool && !measure(dob->do_DefaultTool, &size, &default_len)) goto fail;
    if (dob->do_ToolWindow && !measure(dob->do_ToolWindow, &size, &window_len)) goto fail;
    if (dob->do_ToolTypes)
    {
        while (count < ICON_TOOLTYPE_MAX && dob->do_ToolTypes[count]) ++count;
        if (dob->do_ToolTypes[count] || !grow(&size, (count + 1) * 4)) goto fail;
        for (i = 0; i < count; ++i)
            if (!measure(dob->do_ToolTypes[i], &size, &type_lens[i])) goto fail;
    }
    guest = Kaffeine_HeapAlloc(ctx, size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!guest) goto fail;
    if (guest >= ctx->mem_size || ctx->mem_size - guest < size)
    {
        Kaffeine_HeapFree(ctx, guest, size);
        goto fail;
    }

    m68k_write16(ctx, guest + DO_MAGIC, dob->do_Magic);
    m68k_write16(ctx, guest + DO_VERSION, dob->do_Version);
    m68k_write32(ctx, guest + DO_GADGET, 0);
    m68k_write16(ctx, guest + GG_LEFTEDGE, (UWORD)dob->do_Gadget.LeftEdge);
    m68k_write16(ctx, guest + GG_TOPEDGE, (UWORD)dob->do_Gadget.TopEdge);
    m68k_write16(ctx, guest + GG_WIDTH, (UWORD)dob->do_Gadget.Width);
    m68k_write16(ctx, guest + GG_HEIGHT, (UWORD)dob->do_Gadget.Height);
    m68k_write16(ctx, guest + GG_FLAGS, dob->do_Gadget.Flags);
    m68k_write16(ctx, guest + GG_ACTIVATION, dob->do_Gadget.Activation);
    m68k_write16(ctx, guest + GG_GADGETTYPE, dob->do_Gadget.GadgetType);
    m68k_write32(ctx, guest + GG_MUTUALEXCLUDE, (ULONG)dob->do_Gadget.MutualExclude);
    m68k_write16(ctx, guest + GG_GADGETID, dob->do_Gadget.GadgetID);
    ctx->mem[guest + DO_TYPE] = dob->do_Type;
    m68k_write32(ctx, guest + DO_CURRENTX, (ULONG)dob->do_CurrentX);
    m68k_write32(ctx, guest + DO_CURRENTY, (ULONG)dob->do_CurrentY);
    m68k_write32(ctx, guest + DO_DRAWERDATA, 0);
    m68k_write32(ctx, guest + DO_STACKSIZE, (ULONG)dob->do_StackSize);

    at = ICON_HEADER;
    if (dob->do_ToolTypes)
    {
        array = guest + at;
        at += (count + 1) * 4;
    }
    m68k_write32(ctx, guest + DO_DEFAULTTOOL,
                 dob->do_DefaultTool ? place(ctx, guest, &at, dob->do_DefaultTool, default_len) : 0);
    m68k_write32(ctx, guest + DO_TOOLWINDOW,
                 dob->do_ToolWindow ? place(ctx, guest, &at, dob->do_ToolWindow, window_len) : 0);
    m68k_write32(ctx, guest + DO_TOOLTYPES, array);
    for (i = 0; array && i < count; ++i)
        m68k_write32(ctx, array + i * 4, place(ctx, guest, &at, dob->do_ToolTypes[i], type_lens[i]));
    if (array) m68k_write32(ctx, array + count * 4, 0);

    slot->guest = guest;
    slot->size = size;
    slot->native = dob;
    return guest;
fail:
    FreeDiskObject(dob);
    return 0;
}

/* An optional guest name: NULL stays NULL, anything else must be valid. */
static BOOL guest_name(struct KaffeineContext *ctx, ULONG addr, CONST_STRPTR *name)
{
    ULONG len;
    *name = NULL;
    if (!addr) return TRUE;
    if (!guest_string(ctx, addr, &len)) return FALSE;
    *name = (CONST_STRPTR)(ctx->mem + addr);
    return TRUE;
}

IPTR kaffeine_icon_GetDiskObject(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR name;
    if (!guest_name(ctx, THUNK_A(0), &name)) return stop(ctx, "GetDiskObject name");
    return publish(ctx, GetDiskObject(name));
}

IPTR kaffeine_icon_GetDiskObjectNew(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR name;
    if (!guest_name(ctx, THUNK_A(0), &name)) return stop(ctx, "GetDiskObjectNew name");
    return publish(ctx, GetDiskObjectNew(name));
}

IPTR kaffeine_icon_GetDefDiskObject(struct KaffeineContext *ctx, void *cpu)
{
    return publish(ctx, GetDefDiskObject((LONG)THUNK_D(0)));
}

IPTR kaffeine_icon_NewDiskObject(struct KaffeineContext *ctx, void *cpu)
{
    return publish(ctx, NewDiskObject((LONG)THUNK_D(0)));
}

/* Only the exact guest address of a live snapshot is freed; the stored
   native object is released, never anything read from guest memory. */
IPTR kaffeine_icon_FreeDiskObject(struct KaffeineContext *ctx, void *cpu)
{
    ULONG addr = THUNK_A(0), guest, size;
    struct DiskObject *native;
    unsigned i;
    if (!addr) return 0;
    for (i = 0; i < KAFFEINE_MAX_ICONS; ++i)
    {
        struct KaffeineIcon *icon = &ctx->icons[i];
        if (icon->guest != addr || !icon->native) continue;
        guest = icon->guest;
        size = icon->size;
        native = icon->native;
        icon->guest = 0;
        icon->size = 0;
        icon->native = NULL;
        FreeDiskObject(native);
        Kaffeine_HeapFree(ctx, guest, size);
        return 0;
    }
    return stop(ctx, "FreeDiskObject of a DiskObject the program does not own");
}

/* The real FindToolType() runs on validated guest strings; its result goes
   back only as an address within one of them. */
IPTR kaffeine_icon_FindToolType(struct KaffeineContext *ctx, void *cpu)
{
    ULONG array = THUNK_A(0), name = THUNK_A(1), addrs[ICON_TOOLTYPE_MAX], lens[ICON_TOOLTYPE_MAX];
    ULONG count = 0, entry, len, i;
    STRPTR types[ICON_TOOLTYPE_MAX + 1];
    UBYTE *result;
    if (!array || !name) return 0;
    if (!ctx->mem || (array & 1)) return stop(ctx, "FindToolType array");
    for (;;)
    {
        if (array >= ctx->mem_size || (ctx->mem_size - array) / 4 <= count)
            return stop(ctx, "FindToolType array outside guest memory");
        entry = m68k_read32(ctx, array + count * 4);
        if (!entry) break;
        if (count == ICON_TOOLTYPE_MAX) return stop(ctx, "FindToolType array too long");
        if (!guest_string(ctx, entry, &lens[count])) return stop(ctx, "FindToolType entry");
        addrs[count] = entry;
        types[count] = (STRPTR)(ctx->mem + entry);
        ++count;
    }
    types[count] = NULL;
    if (!guest_string(ctx, name, &len)) return stop(ctx, "FindToolType name");
    result = FindToolType(types, (STRPTR)(ctx->mem + name));
    if (!result) return 0;
    for (i = 0; i < count; ++i)
        if ((IPTR)result >= (IPTR)types[i] && (IPTR)result - (IPTR)types[i] <= lens[i])
            return addrs[i] + (ULONG)((IPTR)result - (IPTR)types[i]);
    return stop(ctx, "FindToolType result outside the tool types");
}

IPTR kaffeine_icon_MatchToolValue(struct KaffeineContext *ctx, void *cpu)
{
    ULONG type = THUNK_A(0), value = THUNK_A(1), len;
    if (!guest_string(ctx, type, &len) || !guest_string(ctx, value, &len))
        return stop(ctx, "MatchToolValue string");
    return (IPTR)MatchToolValue(ctx->mem + type, ctx->mem + value);
}

IPTR kaffeine_icon_Unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return stop(ctx, "unsupported icon.library call");
}

void Kaffeine_IconsCleanup(struct KaffeineContext *ctx)
{
    unsigned i;
    for (i = 0; i < KAFFEINE_MAX_ICONS; ++i)
    {
        struct KaffeineIcon icon = ctx->icons[i];
        ctx->icons[i].guest = 0;
        ctx->icons[i].size = 0;
        ctx->icons[i].native = NULL;
        if (icon.native) FreeDiskObject(icon.native);
        if (icon.guest) Kaffeine_HeapFree(ctx, icon.guest, icon.size);
    }
}

const struct M68KThunkEntry kaffeine_thunks_icon_manual[] = {
    KAFFEINE_ICON_THUNKS
};
const ULONG kaffeine_thunks_icon_manual_count =
    sizeof(kaffeine_thunks_icon_manual) / sizeof(kaffeine_thunks_icon_manual[0]);
