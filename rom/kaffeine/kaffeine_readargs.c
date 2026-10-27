/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    ReadArgs(), FreeArgs() and GetProgramName() for 68k callers: the
    arguments and the name that the Shell gave the command.

    The native ReadArgs() fills
    an array of IPTR with native pointers: strings, pointers to LONGs for
    /N, and NULL-terminated pointer arrays for /M. The 68k array holds
    32-bit guest values, so the results are copied into one block of guest
    memory per call, and the array receives guest pointers to them.

    The native function always gets a native RDArgs from AllocDosObject().
    A program's own RDArgs is a 68k structure in guest memory, from
    AllocDosObject() (Kaffeine_AllocRDArgs() below) or from the program
    itself: its character source, extended help and flags go to the native
    RDArgs, and the position in the source comes back. Without one, the
    result block starts with a cleared 68k RDArgs. Either way the 68k
    RDArgs is the handle that FreeArgs() looks up to free the native
    RDArgs and the guest block.
*/

#include <dos/rdargs.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_readargs.h"

#define MAX_ITEMS 256

/* struct RDArgs of the 68k */
#define M68K_RDA_CS_BUFFER  0
#define M68K_RDA_CS_LENGTH  4
#define M68K_RDA_CS_CURCHR  8
#define M68K_RDA_EXTHELP   24
#define M68K_RDA_FLAGS     28
#define M68K_SIZEOF_RDARGS 32

/* The type bits of rom/dos/readargs.c: options combine with OR there,
   and only these three values give a result that is not a string. */
#define TYPEMASK 0x07
#define ITEM_SWITCH 0x01
#define ITEM_TOGGLE 0x02
#define ITEM_NUMBER 0x03
#define ITEM_REST   0x04

struct Item
{
    UBYTE type;
    UBYTE multiple;
};

/* A value that ReadArgs() leaves in place when an argument is absent */
static const UBYTE untouched[MAX_ITEMS];

/* Read the template as the native ReadArgs() does: one item more than
   there are commas, and the letter after every '/' is an option. The
   native function fills exactly that many array entries. */
static LONG parse_template(const char *tmpl, struct Item *items)
{
    LONG count = 1, i = 0;
    const char *p;

    for (p = tmpl; *p; p++)
        if (*p == ',')
            count++;
    if (count > MAX_ITEMS)
        return -1;

    for (p = tmpl; *p; )
    {
        if (*p == ',')
            i++;
        if (*p++ == '/')
        {
            switch (*p & ~0x20)
            {
            case 'F': items[i].type |= ITEM_REST; break;
            case 'M': items[i].multiple = 1; break;
            case 'N': items[i].type |= ITEM_NUMBER; break;
            case 'S': items[i].type |= ITEM_SWITCH; break;
            case 'T': items[i].type |= ITEM_TOGGLE; break;
            }
        }
    }
    for (i = 0; i < count; i++)
        items[i].type &= TYPEMASK;
    return count;
}

/* Does [addr, addr + size) lie in guest memory? */
static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return addr && addr < ctx->mem_size && size <= ctx->mem_size - addr;
}

/* A string that ends in guest memory, or NULL */
static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    const char *s = (const char *)m68k_to_host(ctx, addr);

    return s && memchr(s, 0, ctx->mem_size - addr) ? s : NULL;
}

static ULONG str_size(const char *s)
{
    return (ULONG)strlen(s) + 1;
}

/* Guest bytes needed for one converted result, 4-byte aligned. */
static ULONG result_size(const struct Item *item, IPTR value)
{
    ULONG size = 0;

    if (item->multiple)
    {
        IPTR *vec = (IPTR *)value;
        ULONG n = 0;

        if (!vec)
            return 0;
        while (vec[n])
        {
            size += item->type == ITEM_NUMBER ? 4 : (str_size((const char *)vec[n]) + 3) & ~3UL;
            n++;
        }
        return size + (n + 1) * 4;
    }
    if (item->type == ITEM_NUMBER)
        return 4;
    if (item->type == ITEM_SWITCH || item->type == ITEM_TOGGLE)
        return 0;
    return (str_size((const char *)value) + 3) & ~3UL;
}

static ULONG put_string(struct KaffeineContext *ctx, ULONG *next, const char *s)
{
    ULONG addr = *next, size = str_size(s);

    memcpy(ctx->mem + addr, s, size);
    *next += (size + 3) & ~3UL;
    return addr;
}

static ULONG put_long(struct KaffeineContext *ctx, ULONG *next, LONG value)
{
    ULONG addr = *next;

    m68k_write32(ctx, addr, (ULONG)value);
    *next += 4;
    return addr;
}

/* Copy one result into the guest block and return the 68k array value. */
static ULONG convert(struct KaffeineContext *ctx, ULONG *next,
                     const struct Item *item, IPTR value)
{
    if (item->multiple)
    {
        IPTR *vec = (IPTR *)value;
        ULONG n = 0, i, guest_vec;

        if (!vec)
            return 0;
        while (vec[n])
            n++;
        guest_vec = *next;
        *next += (n + 1) * 4;
        for (i = 0; i < n; i++)
        {
            ULONG guest = item->type == ITEM_NUMBER
                ? put_long(ctx, next, *(LONG *)vec[i])
                : put_string(ctx, next, (const char *)vec[i]);
            m68k_write32(ctx, guest_vec + i * 4, guest);
        }
        m68k_write32(ctx, guest_vec + n * 4, 0);
        return guest_vec;
    }
    switch (item->type)
    {
    case ITEM_NUMBER:
        return put_long(ctx, next, *(LONG *)value);
    case ITEM_SWITCH:
        return value ? (ULONG)DOSTRUE : 0;
    case ITEM_TOGGLE: /* ReadArgs() inverted the caller's value */
        return (ULONG)value;
    default: /* a string, also for /F */
        return put_string(ctx, next, (const char *)value);
    }
}

/* Give the native RDArgs the program's character source, extended help
   and flags. RDA_Buffer is not passed on: the results are copied into
   guest memory anyway, and with RDAF_NOALLOC the native FreeArgs() frees
   the string buffer as its own. Nor are the flags that DOS keeps private. */
static BOOL to_native(struct KaffeineContext *ctx, ULONG rdargs, struct RDArgs *native)
{
    ULONG buffer = m68k_read32(ctx, rdargs + M68K_RDA_CS_BUFFER);
    LONG length = (LONG)m68k_read32(ctx, rdargs + M68K_RDA_CS_LENGTH);
    LONG current = (LONG)m68k_read32(ctx, rdargs + M68K_RDA_CS_CURCHR);

    if (buffer)
    {
        if (length < 0 || current < 0 || current > length ||
            !guest_range(ctx, buffer, (ULONG)length))
            return FALSE;
        native->RDA_Source.CS_Buffer = ctx->mem + buffer;
        native->RDA_Source.CS_Length = length;
        native->RDA_Source.CS_CurChr = current;
    }
    native->RDA_ExtHelp = (UBYTE *)guest_string(ctx, m68k_read32(ctx, rdargs + M68K_RDA_EXTHELP));
    native->RDA_Flags = (LONG)m68k_read32(ctx, rdargs + M68K_RDA_FLAGS) &
                        (RDAF_STDIN | RDAF_NOPROMPT);
    return TRUE;
}

static LONG find_slot(struct KaffeineContext *ctx, ULONG handle)
{
    LONG i;

    for (i = 0; i < KAFFEINE_MAX_RDARGS; i++)
        if (ctx->rdargs_map[i].native && ctx->rdargs_map[i].handle == handle)
            return i;
    return -1;
}

/* FreeArgs() of the native RDArgs, which this file allocated */
static void release(struct KaffeineContext *ctx, LONG slot, BOOL free_block)
{
    struct RDArgs *native = (struct RDArgs *)ctx->rdargs_map[slot].native;

    FreeArgs(native);
    FreeDosObject(DOS_RDARGS, native);
    if (free_block)
        Kaffeine_HeapFree(ctx, ctx->rdargs_map[slot].block, ctx->rdargs_map[slot].block_size);
    ctx->rdargs_map[slot].native = NULL;
    ctx->rdargs_map[slot].handle = 0;
}

/* -798: ReadArgs(D1=template, D2=array, D3=rdargs) -> D0=rdargs handle */
IPTR Kaffeine_ReadArgs(struct KaffeineContext *ctx, void *cpu)
{
    const char *tmpl = guest_string(ctx, THUNK_D(1));
    ULONG guest_array = THUNK_D(2), guest_rdargs = THUNK_D(3);
    struct Item *items = NULL;
    IPTR *array = NULL;
    struct RDArgs *native = NULL;
    UBYTE *source;
    BOOL parsed;
    LONG count, i, slot = -1;
    ULONG size, block, next, handle = 0;

    items = tmpl ? (struct Item *)AllocMem(MAX_ITEMS * sizeof(*items), MEMF_CLEAR) : NULL;
    count = items ? parse_template(tmpl, items) : -1;
    /* The native ReadArgs() would write past an array that does not fit */
    if (count < 0 || !guest_range(ctx, guest_array, (ULONG)count * 4) ||
        (guest_rdargs && !guest_range(ctx, guest_rdargs, M68K_SIZEOF_RDARGS)))
    {
        if (items)
            FreeMem(items, MAX_ITEMS * sizeof(*items));
        SetIoErr(tmpl && !items ? ERROR_NO_FREE_STORE : ERROR_BAD_TEMPLATE);
        return 0;
    }
    array = (IPTR *)AllocMem(count * sizeof(*array), MEMF_CLEAR);
    native = array ? (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL) : NULL;
    if (!native)
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        goto out;
    }
    if (guest_rdargs && !to_native(ctx, guest_rdargs, native))
    {
        SetIoErr(ERROR_BAD_TEMPLATE);
        goto out;
    }
    source = native->RDA_Source.CS_Buffer;

    /* /T inverts the caller's value; any other entry stays at the marker
       when its argument is absent, so the caller's default is kept. */
    for (i = 0; i < count; i++)
        array[i] = items[i].type == ITEM_TOGGLE && !items[i].multiple
            ? (IPTR)m68k_read32(ctx, guest_array + i * 4)
            : (IPTR)&untouched[i];

    parsed = ReadArgs((CONST_STRPTR)tmpl, (SIPTR *)array, native) != NULL;
    /* The position in the program's source, unless a '?' made the native
       function read a new line of its own */
    if (source && native->RDA_Source.CS_Buffer == source)
        m68k_write32(ctx, guest_rdargs + M68K_RDA_CS_CURCHR, (ULONG)native->RDA_Source.CS_CurChr);
    if (!parsed)
        goto out;

    /* Without a caller's RDArgs, the block starts with a cleared 68k
       RDArgs, and its address is the program's handle. */
    size = guest_rdargs ? 0 : M68K_SIZEOF_RDARGS;
    for (i = 0; i < count; i++)
        if (array[i] != (IPTR)&untouched[i])
            size += result_size(&items[i], array[i]);
    if (!size)
        size = 4;

    for (i = 0; slot < 0 && i < KAFFEINE_MAX_RDARGS; i++)
        if (!ctx->rdargs_map[i].native)
            slot = i;
    block = slot >= 0 ? Kaffeine_HeapAlloc(ctx, size, 0) : 0;
    if (!block)
    {
        FreeArgs(native);
        SetIoErr(ERROR_NO_FREE_STORE);
        goto out;
    }

    next = block;
    if (!guest_rdargs)
    {
        memset(ctx->mem + block, 0, M68K_SIZEOF_RDARGS);
        next += M68K_SIZEOF_RDARGS;
    }
    for (i = 0; i < count; i++)
        if (array[i] != (IPTR)&untouched[i])
            m68k_write32(ctx, guest_array + i * 4, convert(ctx, &next, &items[i], array[i]));

    handle = guest_rdargs ? guest_rdargs : block;
    ctx->rdargs_map[slot].handle = handle;
    ctx->rdargs_map[slot].native = native;
    ctx->rdargs_map[slot].block = block;
    ctx->rdargs_map[slot].block_size = size;
    native = NULL;

out:
    if (native)
        FreeDosObject(DOS_RDARGS, native);
    if (array)
        FreeMem(array, count * sizeof(*array));
    FreeMem(items, MAX_ITEMS * sizeof(*items));
    return handle;
}

/* -858: FreeArgs(D1=rdargs handle) */
IPTR Kaffeine_FreeArgs(struct KaffeineContext *ctx, void *cpu)
{
    ULONG handle = THUNK_D(1);
    LONG slot;

    /* Every RDArgs the program can hold came from Kaffeine_ReadArgs(), so
       an unknown handle has been freed already: FreeArgs() twice on the
       caller's own RDArgs is allowed, and does nothing the second time. */
    slot = handle ? find_slot(ctx, handle) : -1;
    if (slot >= 0)
        release(ctx, slot, TRUE);
    return 0;
}

ULONG Kaffeine_AllocRDArgs(struct KaffeineContext *ctx)
{
    ULONG rdargs = Kaffeine_HeapAlloc(ctx, M68K_SIZEOF_RDARGS, 0);

    if (rdargs)
        memset(ctx->mem + rdargs, 0, M68K_SIZEOF_RDARGS);
    else
        SetIoErr(ERROR_NO_FREE_STORE);
    return rdargs;
}

void Kaffeine_FreeRDArgs(struct KaffeineContext *ctx, ULONG rdargs)
{
    BOOL in_block = FALSE;
    LONG slot;

    if (!rdargs)
        return;
    /* The results the program did not release with FreeArgs() */
    while ((slot = find_slot(ctx, rdargs)) >= 0)
    {
        in_block |= ctx->rdargs_map[slot].block == rdargs;
        release(ctx, slot, TRUE);
    }
    /* An RDArgs that ReadArgs() made is part of its result block */
    if (!in_block)
        Kaffeine_HeapFree(ctx, rdargs, M68K_SIZEOF_RDARGS);
}

/* -576: GetProgramName(D1=buf, D2=len) -> D0=success. The name starts with
   the one RunHunk() took from the Shell, then follows the exposed guest CLI.
   As the autodoc describes, a name that does not fit with
   its terminator is cut, and the call fails with ERROR_LINE_TOO_LONG;
   without a name, the buffer gets an empty string and the call fails with
   ERROR_OBJECT_WRONG_TYPE. */
IPTR Kaffeine_GetProgramName(struct KaffeineContext *ctx, void *cpu)
{
    ULONG buf = THUNK_D(1), len = THUNK_D(2), size;
    const char *name = ctx->program_name;

    if ((LONG)len <= 0 || !guest_range(ctx, buf, len))
    {
        SetIoErr(ERROR_LINE_TOO_LONG);
        return DOSFALSE;
    }
    size = (ULONG)strnlen(name, sizeof(ctx->program_name) - 1);
    /* Once Cli() exposes a guest CLI, its BSTR is authoritative, including
       direct guest writes and SetProgramName(). Before that, retain the
       startup name used by callers without a CLI. */
    if (ctx->m68k_cli)
    {
        ULONG bptr, addr;

        if (!guest_range(ctx, ctx->m68k_cli, M68K_SIZEOF_CLI))
        {
            SetIoErr(ERROR_BAD_NUMBER);
            return DOSFALSE;
        }
        bptr = m68k_read32(ctx, ctx->m68k_cli + M68K_CLI_COMMANDNAME);
        if (!bptr || bptr > 0x3FFFFFFFUL || !guest_range(ctx, bptr << 2, 1))
        {
            SetIoErr(ERROR_BAD_NUMBER);
            return DOSFALSE;
        }
        addr = bptr << 2;
        size = ctx->mem[addr];
        if (!guest_range(ctx, addr, size + 1))
        {
            SetIoErr(ERROR_BAD_NUMBER);
            return DOSFALSE;
        }
        name = (const char *)ctx->mem + addr + 1;
    }
    if (!size)
    {
        ctx->mem[buf] = 0;
        SetIoErr(ERROR_OBJECT_WRONG_TYPE);
        return DOSFALSE;
    }
    if (size >= len)
    {
        memmove(ctx->mem + buf, name, len - 1);
        ctx->mem[buf + len - 1] = 0;
        SetIoErr(ERROR_LINE_TOO_LONG);
        return DOSFALSE;
    }
    memmove(ctx->mem + buf, name, size);
    ctx->mem[buf + size] = 0;
    return (IPTR)(ULONG)DOSTRUE;
}

void Kaffeine_ReadArgsCleanup(struct KaffeineContext *ctx)
{
    LONG i;

    /* The guest blocks go with the context */
    for (i = 0; i < KAFFEINE_MAX_RDARGS; i++)
        if (ctx->rdargs_map[i].native)
            release(ctx, i, FALSE);
}
