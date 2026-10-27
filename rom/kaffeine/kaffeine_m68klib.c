/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    68k libraries from disk, made as Exec makes a disk library. LoadSeg()
    loads the file and keeps the record of its relocations, with which the
    hunks are copied into guest memory as a seglist there; the resident
    tag is the one in those hunks; for RTF_AUTOINIT the library is the one
    of MakeLibrary(): the jump table from a table of function pointers, or
    of WORD offsets from the table when it starts with -1, the data area
    cleared and set up by its InitStruct() table, then the node type, name,
    version and IdString of the resident tag, as InitResident() sets them,
    and the init function called with the base in D0, the seglist in A0
    and SysBase in A6, which gives the library. Each OpenLibrary() then calls the Open() of the
    library with the version asked for, after checking lib_Version, and
    each CloseLibrary() its Close(), also for a base that Open() made for
    its opener. A version newer than that of an open library calls its
    Expunge(), as AmigaOS 3.2.3 does, which sets LIBF_DELEXP for the last
    close. The libraries stay to the end of the program, whose guest
    memory goes with them, unless Close() gives back the seglist after an
    expunge. Init, Open() and Close() are 68k code that can open and close
    other libraries, so each library keeps its slot in the table while
    they run, and the name is a copy that the thunk made before them.
*/

#include <stdio.h>
#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/doshunks.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_callback.h"

#define NT_LIBRARY    9
/* Longer tables than these are none that a library means. */
#define MAX_VECTORS   1024
#define MAX_INITS     4096
#define MAX_HUNKS     KAFFEINE_MAX_LIBRARY_HUNKS

static BOOL in_guest(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* LoadSeg() without requesters: without the LIBS68K: assign, DOS would
   ask for that volume, and a program started through Run would wait on a
   requester that nobody answers. */
static BPTR load(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR window = NULL;
    BPTR seg;

    if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS)
    {
        window = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
    }
    seg = LoadSeg(path);
    if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        me->pr_WindowPtr = window;
    return seg;
}

/* The guest hunks of a seglist that Kaffeine_LoadLibraryHunks() made go
   back to the heap. */
static void free_seglist(struct KaffeineContext *ctx, ULONG seglist)
{
    ULONG n;

    for (n = 0; seglist && n < MAX_HUNKS; n++)
    {
        ULONG block = (seglist << 2) - 4, next = m68k_read32(ctx, seglist << 2);

        Kaffeine_HeapFree(ctx, block, m68k_read32(ctx, block));
        seglist = next;
    }
}

/* Whether addr lies in the hunks of seglist */
static BOOL in_hunks(struct KaffeineContext *ctx, ULONG seglist, ULONG addr)
{
    ULONG n;

    for (n = 0; seglist && n < MAX_HUNKS; n++, seglist = m68k_read32(ctx, seglist << 2))
    {
        ULONG data = (seglist << 2) + 4;

        if (addr >= data && addr - data < m68k_read32(ctx, (seglist << 2) - 4) - 8)
            return TRUE;
    }
    return FALSE;
}

/* The resident tag in the hunks of seglist: a RTC_MATCHWORD whose
   rt_MatchTag points at it, as Exec looks for it */
static ULONG find_resident(struct KaffeineContext *ctx, ULONG seglist)
{
    ULONG n, x;

    for (n = 0; seglist && n < MAX_HUNKS; n++, seglist = m68k_read32(ctx, seglist << 2))
    {
        ULONG data = (seglist << 2) + 4;
        ULONG size = m68k_read32(ctx, (seglist << 2) - 4) - 8;

        for (x = data; size >= M68K_SIZEOF_RESIDENT && x <= data + size - M68K_SIZEOF_RESIDENT;
             x += 2)
            if (m68k_read16(ctx, x) == M68K_RTC_MATCHWORD &&
                m68k_read32(ctx, x + M68K_RT_MATCHTAG) == x)
                return x;
    }
    return 0;
}

/* InitStruct() of the table at it onto the size bytes at base, which its
   writes must stay within: commands of copies (0), repeats (1), copies at
   a byte offset (2) or at a 24-bit one (3), of LONGs, WORDs or bytes, up
   to a command byte of 0, at most MAX_INITS of them, WORDs and LONGs from
   and to even addresses and each command on an even address, as Exec
   reads the table; an offset is taken as the table gives it, also an odd
   one for WORDs and LONGs, as Exec takes it. */
static BOOL init_struct(struct KaffeineContext *ctx, ULONG it, ULONG base, ULONG size)
{
    ULONG dst = base, n;

    for (n = 0;; n++)
    {
        ULONG cmd, kind, width, count, offset = 0, value = 0, i;

        if (!in_guest(ctx, it, 1))
            return FALSE;
        if (!(cmd = ctx->mem[it]))
            return TRUE;
        if (n == MAX_INITS)
            return FALSE;
        kind = cmd >> 6 & 3;
        width = (cmd >> 4 & 3) == 0 ? 4 : (cmd >> 4 & 3) == 1 ? 2 : (cmd >> 4 & 3) == 2 ? 1 : 0;
        count = (cmd & 15) + 1;
        /* the fourth size is none of AmigaOS */
        if (!width)
            return FALSE;
        if (kind == 2)
        {
            if (!in_guest(ctx, it, 2))
                return FALSE;
            offset = ctx->mem[it + 1];
            it += 2;
        }
        else if (kind == 3)
        {
            if (!in_guest(ctx, it, 4))
                return FALSE;
            offset = m68k_read32(ctx, it) & 0xFFFFFF;
            it += 4;
        }
        else
            it++;
        if (width > 1)
        {
            it = (it + 1) & ~1u;
            dst = (dst + 1) & ~1u;
        }
        if (kind >= 2)
            dst = base + offset;
        if (dst - base > size || count * width > size - (dst - base) ||
            !in_guest(ctx, it, kind == 1 ? width : count * width))
            return FALSE;
        for (i = 0; i < count; i++)
        {
            if (kind != 1 || i == 0)
            {
                value = width == 4 ? m68k_read32(ctx, it) :
                        width == 2 ? m68k_read16(ctx, it) : ctx->mem[it];
                it += width;
            }
            if (width == 4)
                m68k_write32(ctx, dst, value);
            else if (width == 2)
                m68k_write16(ctx, dst, (UWORD)value);
            else
                ctx->mem[dst] = (UBYTE)value;
            dst += width;
        }
        it = (it + 1) & ~1u;
    }
}

/* MakeLibrary() of the RTF_AUTOINIT table of the resident tag at rt, as
   InitResident() makes it: the library, or 0 for tables that it cannot be
   made from, or with the program stopped */
static ULONG make_library(struct KaffeineContext *ctx, void *cpu, ULONG rt, ULONG seglist)
{
    struct KaffeineGuestCall call;
    ULONG init = m68k_read32(ctx, rt + M68K_RT_INIT);
    ULONG data_size, table, data_init, init_func, count, neg, block, base, i;
    BOOL words;

    if (!in_guest(ctx, init, 16))
        return 0;
    data_size = m68k_read32(ctx, init + M68K_AUTOINIT_DATASIZE);
    table = m68k_read32(ctx, init + M68K_AUTOINIT_FUNCTABLE);
    data_init = m68k_read32(ctx, init + M68K_AUTOINIT_DATAINIT);
    init_func = m68k_read32(ctx, init + M68K_AUTOINIT_INITFUNC);
    /* lib_PosSize is a UWORD */
    if (!table || !in_guest(ctx, table, 2) || data_size > 0xFFFF)
        return 0;
    if (data_size < M68K_SIZEOF_LIBRARY)
        data_size = M68K_SIZEOF_LIBRARY;

    /* WORD offsets from the table after a -1, else function pointers,
       up to a -1 of their size; each function even and in guest memory */
    words = m68k_read16(ctx, table) == 0xFFFF;
    for (count = 0;; count++)
    {
        ULONG at = words ? table + 2 + 2 * count : table + 4 * count, fn;

        if (count > MAX_VECTORS || !in_guest(ctx, at, words ? 2 : 4))
            return 0;
        if (words ? m68k_read16(ctx, at) == 0xFFFF : m68k_read32(ctx, at) == 0xFFFFFFFF)
            break;
        fn = words ? table + (ULONG)(LONG)(WORD)m68k_read16(ctx, at) : m68k_read32(ctx, at);
        if (fn && ((fn & 1) || !in_guest(ctx, fn, 2)))
            return 0;
    }

    neg = (count * M68K_JT_SLOT_SIZE + 3) & ~3u;
    if (!(block = Kaffeine_HeapAlloc(ctx, neg + data_size, MEMF_PUBLIC | MEMF_CLEAR)))
        return 0;
    base = block + neg;
    for (i = 0; i < count; i++)
    {
        ULONG slot = base - (i + 1) * M68K_JT_SLOT_SIZE;
        ULONG fn = words ? table + (ULONG)(LONG)(WORD)m68k_read16(ctx, table + 2 + 2 * i)
                         : m68k_read32(ctx, table + 4 * i);

        m68k_write16(ctx, slot, M68K_OP_JMP_ABS_L);
        m68k_write32(ctx, slot + 2, fn);
    }
    /* The program runs while OpenLibrary() makes the library: the jump
       table is new guest code in heap memory that it may have used. */
    Kaffeine_FlushCode(cpu, block, neg);
    m68k_write16(ctx, base + M68K_LIB_NEGSIZE, (UWORD)neg);
    m68k_write16(ctx, base + M68K_LIB_POSSIZE, (UWORD)data_size);
    if (data_init && !init_struct(ctx, data_init, base, data_size))
    {
        bug("[kaffeine] m68klib: an InitStruct() table that the bridge refuses\n");
        Kaffeine_HeapFree(ctx, block, neg + data_size);
        return 0;
    }
    /* the node, version and IdString of the resident tag, which
       InitResident() sets after MakeLibrary() and before the init */
    ctx->mem[base + M68K_LIB_LNTYPE] = ctx->mem[rt + M68K_RT_TYPE];
    m68k_write32(ctx, base + M68K_LIB_LNNAME, m68k_read32(ctx, rt + M68K_RT_NAME));
    ctx->mem[base + M68K_LIB_FLAGS] = M68K_LIBF_SUMUSED | M68K_LIBF_CHANGED;
    m68k_write16(ctx, base + M68K_LIB_VERSION, ctx->mem[rt + M68K_RT_VERSION]);
    m68k_write32(ctx, base + M68K_LIB_IDSTRING, m68k_read32(ctx, rt + M68K_RT_IDSTRING));
    if (!init_func)
        return base;

    memset(&call, 0, sizeof(call));
    call.d[0] = base;
    call.a[0] = seglist;
    call.a[6] = ctx->m68k_sysbase;
    call.d_mask = 1u << 0;
    call.a_mask = (1u << 0) | (1u << 6);
    if (!Kaffeine_CallGuest(ctx, cpu, init_func, &call))
        return 0;
    /* the library that the init function gives, which keeps its memory
       also when it gives none */
    return call.d[0];
}

/* The library of name, loaded and made, or 0 */
static ULONG load_library(struct KaffeineContext *ctx, void *cpu, const char *name, ULONG *seglist)
{
    static const char *const dirs[] = { "LIBS68K:", "LIBS:m68k/", "LIBS:" };
    char path[256];
    IPTR relocs = 0;
    struct TagItem tags[] = { { GSLI_68KRelocs, (IPTR)&relocs }, { TAG_DONE, 0 } };
    BPTR seg = BNULL;
    ULONG rt, base, i;

    /* a name with a path is that file alone, as ramlib takes it */
    if (strchr(name, ':') || strchr(name, '/'))
        seg = load(name);
    for (i = 0; !seg && !strchr(name, ':') && !strchr(name, '/') && i < 3; i++)
    {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        seg = load(path);
    }
    if (!seg)
        return 0;
    /* a native library has no record of 68k relocations */
    if (!GetSegListInfo(seg, tags) || !relocs)
    {
        bug("[kaffeine] m68klib: %s is no 68k library\n", name);
        UnLoadSeg(seg);
        return 0;
    }
    *seglist = Kaffeine_LoadLibraryHunks(ctx, seg, (const struct HunkRelocTable *)relocs);
    UnLoadSeg(seg);
    if (!*seglist)
    {
        bug("[kaffeine] m68klib: no guest memory for the hunks of %s\n", name);
        return 0;
    }

    if (!(rt = find_resident(ctx, *seglist)))
        bug("[kaffeine] m68klib: no resident tag in %s\n", name);
    else if (!(ctx->mem[rt + M68K_RT_FLAGS] & M68K_RTF_AUTOINIT) ||
             ctx->mem[rt + M68K_RT_TYPE] != NT_LIBRARY)
        bug("[kaffeine] m68klib: %s is no RTF_AUTOINIT library, which the bridge does not make yet\n",
            name);
    else if ((base = make_library(ctx, cpu, rt, *seglist)))
        return base;
    else if (ctx->running)
        bug("[kaffeine] m68klib: %s could not be made\n", name);
    if (ctx->running)
        free_seglist(ctx, *seglist);
    return 0;
}

/* The Close() or Expunge() at lvo of the library in slot i, through base:
   FALSE with the program stopped. The library goes when the call gives
   its seglist back while the slot still has it, since the 68k code can
   close other libraries, and this one again. */
static BOOL call_library(struct KaffeineContext *ctx, void *cpu, int i, ULONG base, ULONG lvo)
{
    struct KaffeineGuestCall call;
    ULONG library = ctx->m68klibs[i].base, seglist = ctx->m68klibs[i].seglist;

    memset(&call, 0, sizeof(call));
    call.a[6] = base;
    call.a_mask = 1u << 6;
    if (!Kaffeine_CallGuest(ctx, cpu, base - lvo, &call))
        return FALSE;
    if (call.d[0] && call.d[0] == seglist && ctx->m68klibs[i].base == library &&
        ctx->m68klibs[i].seglist == seglist)
    {
        free_seglist(ctx, seglist);
        memset(&ctx->m68klibs[i], 0, sizeof(ctx->m68klibs[i]));
        ctx->num_m68klibs--;
    }
    return TRUE;
}

/* The slot of the library of base: the one made with it, else the one in
   whose hunks the Close() of base lies, as for the base that the Open() of
   a library makes for each opener; -1 for none */
static int owner(struct KaffeineContext *ctx, ULONG base)
{
    ULONG slot = base - 2 * M68K_JT_SLOT_SIZE, close = 0;
    int i;

    for (i = 0; base && i < KAFFEINE_MAX_M68KLIBS; i++)
        if (ctx->m68klibs[i].base == base)
            return i;
    if (m68k_read16(ctx, slot) == M68K_OP_JMP_ABS_L)
        close = m68k_read32(ctx, slot + 2);
    for (i = 0; close && i < KAFFEINE_MAX_M68KLIBS; i++)
        if (ctx->m68klibs[i].base && in_hunks(ctx, ctx->m68klibs[i].seglist, close))
            return i;
    return -1;
}

ULONG Kaffeine_OpenM68KLibrary(struct KaffeineContext *ctx, void *cpu, const char *name,
                              ULONG version)
{
    struct KaffeineGuestCall call;
    const char *file = name, *c;
    ULONG base;
    int i;

    /* the table has the name without its path, as Exec finds a library */
    for (c = name; *c; c++)
        if (*c == ':' || *c == '/')
            file = c + 1;
    if (!*file || strlen(file) >= sizeof(ctx->m68klibs[0].name))
        return 0;
    for (i = 0; i < KAFFEINE_MAX_M68KLIBS && strcmp(ctx->m68klibs[i].name, file); i++)
        ;
    if (i < KAFFEINE_MAX_M68KLIBS)
    {
        /* none for its own init yet, as Exec adds a library after it */
        if (!(base = ctx->m68klibs[i].base))
            return 0;
    }
    else
    {
        /* the slot is taken before init, which can open other libraries */
        for (i = 0; i < KAFFEINE_MAX_M68KLIBS && ctx->m68klibs[i].name[0]; i++)
            ;
        if (i == KAFFEINE_MAX_M68KLIBS)
        {
            bug("[kaffeine] m68klib: no slot for %s, the bridge keeps %d libraries\n", file,
                KAFFEINE_MAX_M68KLIBS);
            return 0;
        }
        strcpy(ctx->m68klibs[i].name, file);
        ctx->num_m68klibs++;
        if (!(base = load_library(ctx, cpu, name, &ctx->m68klibs[i].seglist)))
        {
            memset(&ctx->m68klibs[i], 0, sizeof(ctx->m68klibs[i]));
            ctx->num_m68klibs--;
            return 0;
        }
        ctx->m68klibs[i].base = base;
    }

    if (m68k_read16(ctx, base + M68K_LIB_VERSION) < version)
    {
        bug("[kaffeine] m68klib: %s is version %u, older than %lu\n", file,
            m68k_read16(ctx, base + M68K_LIB_VERSION), (unsigned long)version);
        /* the Expunge() of an open library, as AmigaOS 3.2.3 calls it for
           a newer version; nothing for a closed one */
        if (m68k_read16(ctx, base + M68K_LIB_OPENCNT))
            call_library(ctx, cpu, i, base, 3 * M68K_JT_SLOT_SIZE);
        return 0;
    }
    memset(&call, 0, sizeof(call));
    call.d[0] = version;
    call.a[6] = base;
    call.d_mask = 1u << 0;
    call.a_mask = 1u << 6;
    if (!Kaffeine_CallGuest(ctx, cpu, base - 1 * M68K_JT_SLOT_SIZE, &call))
        return 0;
    return call.d[0];
}

BOOL Kaffeine_CloseM68KLibrary(struct KaffeineContext *ctx, void *cpu, ULONG base)
{
    int i = owner(ctx, base);

    if (i < 0)
        return FALSE;
    call_library(ctx, cpu, i, base, 2 * M68K_JT_SLOT_SIZE);
    return TRUE;
}
