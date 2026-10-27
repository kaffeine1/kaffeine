/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
    kaffeine.library internals — full containment memory model
*/
#ifndef KAFFEINE_INTERN_H
#define KAFFEINE_INTERN_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/bptr.h>

struct KaffeineLibBase
{
    struct Library          lib;
    struct ExecBase        *sysBase;
    struct DosLibrary      *dosBase;
};

#define KAFFEINE_MEM_SIZE        (32 * 1024 * 1024)
/* Zero bytes past the guest memory, which the guest cannot reach: native
   code that counts a text of the program as far as its NUL, as Intuition
   counts the Buffer of a string gadget up to 32766 bytes and writes at its
   end, stops within them however the program left its text. */
#define KAFFEINE_MEM_GUARD       0x8000
#define KAFFEINE_VECTORS_BASE    0x000000
#define KAFFEINE_VECTORS_SIZE    0x000400
#define KAFFEINE_LIBBASE_REGION  0x000400
#define KAFFEINE_LIBBASE_SIZE    0x010000
#define KAFFEINE_HEAP_BASE       0x010400
#define KAFFEINE_STACK_SIZE      (64 * 1024)

struct M68KHeapBlock
{
    ULONG size;
    ULONG next;
};

struct M68KThunkEntry;
struct M68KHeapAllocation;
struct KaffeineCPU;
struct IORequest;
struct Message;
struct MsgPort;
struct Task;
struct Device;
struct TextFont;
struct DiskObject;
struct Window;
struct IntuiMessage;
struct Screen;
struct KaffeineMenuStrip;
struct KaffeineGadgetBatch;
struct KaffeineWindowGadgets;
struct KaffeineClassic;
struct KaffeineScreenKeep;

#define KAFFEINE_MAX_ICONS 32
#define KAFFEINE_MAX_MONITORS 128
struct MonitorSpec;
struct KaffeineIcon
{
    ULONG guest, size;
    struct DiskObject *native;
};

/* A port from CreateMsgPort() and the native signal bit behind it. */
#define KAFFEINE_MAX_IO_PORTS 16
struct KaffeineIOPort
{
    ULONG   guest;              /* 68k MsgPort, 0 for a free entry */
    BYTE    sigbit;
};

/* A 68k IORequest from CreateIORequest() or open on a device. While it is
   open, a native request of the native size and its own reply port stand
   behind it, and only kaffeine_exec_io.c passes them to native Exec. */
#define KAFFEINE_MAX_IO_REQUESTS 16
enum KaffeineIODeviceKind
{
    KAFFEINE_IO_NONE, KAFFEINE_IO_TIMER, KAFFEINE_IO_CONSOLE
};
struct KaffeineIORequest
{
    ULONG   guest;              /* 68k IORequest, 0 for a free entry */
    ULONG   capacity;           /* bytes from CreateIORequest(), 0 for guest storage */
    struct IORequest *native;   /* while open */
    struct MsgPort   *reply;
    ULONG   unit;
    enum KaffeineIODeviceKind kind; /* native state, never taken from guest fields */
};

#define KAFFEINE_MAX_LIBS        32
#define KAFFEINE_MAX_LVO         512

struct M68KFakeLibBase
{
    ULONG   m68k_addr;          /* library base address in m68k space */
    ULONG   jt_start;           /* first byte of jump table (lowest addr) */
    UWORD   lib_id;
    UWORD   num_vectors;
    UWORD   pos_size;
    const struct M68KThunkEntry *thunks;
    ULONG   num_thunks;
    const struct M68KThunkEntry *gen_thunks;
    ULONG   num_gen_thunks;
    char    name[32];           /* library name for OpenLibrary matching */
};

/* Map library names to fake base addresses for OpenLibrary */
#define KAFFEINE_MAX_LIBMAP 16
struct M68KLibMapEntry
{
    char    name[32];
    ULONG   m68k_base;          /* fake base in containment space */
};

/* Active native file associations and quarantined guest identities. The active
   table is reusable; identities are never reused during a guest execution. */
#define KAFFEINE_MAX_FILES 32
#define KAFFEINE_FILE_SIZE 52
#define KAFFEINE_FILE_SLOT_SIZE 88
#define KAFFEINE_FILE_SLAB_SIZE 4096
struct KaffeineFileSlab
{
    struct KaffeineFileSlab *next;
    ULONG guest, used;
};
#define KAFFEINE_FILE_BUFFER_LIMIT 65536
struct KaffeineFile
{
    ULONG guest, handler, key;
    BPTR native;
    BOOL owned;
    ULONG mirror;
    UBYTE snapshot[KAFFEINE_FILE_SIZE];
};
#define KAFFEINE_MAX_MESSAGE_PORTS 64

struct KaffeineContext
{
    UBYTE  *mem;
    ULONG   mem_size;

    ULONG   heap_start;
    ULONG   heap_end;
    ULONG   heap_free;
    /* Native interval AVL tree and ordered free list. */
    struct M68KHeapAllocation *heap_allocations;
    struct M68KHeapAllocation *heap_first_free;

    ULONG   entry_point;
    ULONG   seg_start;

    ULONG   stack_top;
    ULONG   stack_bottom;

    struct M68KFakeLibBase libs[KAFFEINE_MAX_LIBS];
    UWORD   num_libs;

    /* Library name -> fake base mapping (for m68k OpenLibrary) */
    struct M68KLibMapEntry libmap[KAFFEINE_MAX_LIBMAP];
    UWORD   num_libmap;

    /* 68k libraries from disk that the program opened, kept to its end
       (kaffeine_m68klib.c): base, guest seglist and name, in slots that
       stay where they are while init, Open() and Close() run; a slot is
       free with no name and taken with no base while its library is made */
    #define KAFFEINE_MAX_M68KLIBS 32
    struct { ULONG base; ULONG seglist; char name[32]; } m68klibs[KAFFEINE_MAX_M68KLIBS];
    UWORD   num_m68klibs;

    /* workbench.library (kaffeine_workbench.c): the native ports behind
       each 68k port that App objects send to (native is the one that
       Workbench gets), the App objects by the guest token that the program
       got for each, with the native copies that they keep, and the 68k
       copies of the AppMessages that the program took and has not replied,
       with the carrier of each (native) */
    #define KAFFEINE_MAX_APP_PORTS 8
    struct { ULONG guest; struct MsgPort *native; } app_ports[KAFFEINE_MAX_APP_PORTS];
    #define KAFFEINE_MAX_APP_OBJECTS 32
    struct { ULONG guest; UBYTE kind; UBYTE port; APTR native; APTR copy; } app_objects[KAFFEINE_MAX_APP_OBJECTS];
    #define KAFFEINE_MAX_APP_MESSAGES 32
    struct { ULONG guest; UBYTE port; struct Message *native; } app_messages[KAFFEINE_MAX_APP_MESSAGES];

    struct KaffeineLibBase *emuBase;

    ULONG   m68k_argptr;
    ULONG   m68k_argsize;
    ULONG   m68k_sysbase;

    /* CLI snapshot shared by Cli(), ThisTask and FindTask(NULL). */
    ULONG   m68k_cli;

    BOOL    running;
    LONG    exit_code;
    UWORD   callback_depth;
    BOOL    callback_failed; /* Suppress pending backend accesses after a stop. */
    /* The CPU in Kaffeine_RunCPU(), NULL outside it: the loaders of 68k
       code have no CPU handle to tell its backend that code changed. */
    struct KaffeineCPU *cpu;
    /* Shadow struct mapping: m68k addr <-> native ptr */
    #define KAFFEINE_MAX_SHADOWS 1024
    struct { ULONG m68k_addr; void *native_ptr; UWORD type; } shadow_map[KAFFEINE_MAX_SHADOWS];
    UWORD num_shadows;

    /* Run by the dispatcher right after a thunk returns, then cleared: the
       68k RastPorts that the thunk used get the native fields back. */
    void (*after_thunk)(struct KaffeineContext *ctx);
    #define KAFFEINE_MAX_SETTLE 2
    ULONG settle_rastports[KAFFEINE_MAX_SETTLE];

    /* 68k TextFonts (kaffeine_rastport.c): the fonts of windows and screens
       stay, and a font of the program goes once it is closed and no 68k
       RastPort shows it */
    #define KAFFEINE_MAX_FONTS 32
    struct {
        ULONG guest, allocation_size;
        struct TextFont *native;
        ULONG opens;
        BOOL pinned, retained;
    } fonts[KAFFEINE_MAX_FONTS];

    /* Menu strips of the program, GadTools ones with their 68k mirrors and
       native copies of those that it built (kaffeine_menus.c); menu_sync
       brings the program's side of a window's strip up to date before a
       menu message reaches the program, menu_closing takes the strip off
       a window before CloseWindow() and FreeSysRequest() */
    struct KaffeineMenuStrip *menus;
    void (*menu_sync)(struct KaffeineContext *ctx, struct IntuiMessage *im);
    void (*menu_closing)(struct KaffeineContext *ctx, struct Window *w);

    /* GadTools gadgets of the program and their 68k mirrors, a batch for
       each call that made some, and what windows do with them, set with
       the first batch or as the program starts (kaffeine_gadgets.c), and
       the native copies of the gadgets that it built itself on windows
       (kaffeine_classic.c) */
    struct KaffeineGadgetBatch *gadgets;
    const struct KaffeineWindowGadgets *window_gadgets;
    struct KaffeineClassic *classic;

    /* Screens of the program (kaffeine_screens.c): one 68k Screen for each
       native screen, with its 68k TextAttr, kept while the program holds a
       lock on it, has a window there or holds the list of public screens,
       and for a screen that it opened (owned) until CloseScreen() closes
       it; keep is the native copy of the title and the font that Intuition
       points to while such a screen is open. screen_release, set with the
       first one, lets the window bridge say that a 68k Window on that
       screen has gone. */
    #define KAFFEINE_MAX_PUBSCREENS 16
    struct {
        struct Screen *native; ULONG guest; ULONG font; UWORD locks;
        BOOL owned; struct KaffeineScreenKeep *keep;
    } pubscreens[KAFFEINE_MAX_PUBSCREENS];
    void (*screen_release)(struct KaffeineContext *ctx, struct Screen *screen);
    /* VisualInfo keeps DrawInfo/Screen alive. Borrowed screens also hold
       a native visitor lock until their VisualInfo references are freed. */
    #define KAFFEINE_MAX_VISUALINFOS 32
    struct {
        APTR native; struct Screen *screen; ULONG guest; BOOL public_lock;
    } visualinfos[KAFFEINE_MAX_VISUALINFOS];
    /* the native fonts of those screens, held open for the program, whose
       68k TextFonts stay, as many as there are 68k TextFonts; closed at
       its end */
    struct TextFont *pubscreen_fonts[KAFFEINE_MAX_FONTS];
    /* the 68k copy of the list of public screens while the program holds
       it, its nodes, and the nested locks of the list */
    ULONG pubscreen_list;
    UWORD pubscreen_list_nodes;
    UWORD pubscreen_list_locks;

    /* Program name for GetProgramName */
    char    program_name[256];

    /* ReadArgs() results copied into guest memory, released by FreeArgs() */
    #define KAFFEINE_MAX_RDARGS 8
    struct { ULONG handle; void *native; ULONG block; ULONG block_size; } rdargs_map[KAFFEINE_MAX_RDARGS];

    /* OpenLocale() guest copies and GetLocaleStr() string blocks */
    #define KAFFEINE_MAX_LOCALES 8
    #define KAFFEINE_LOCALE_MAXSTRMSG 71
    struct
    {
        ULONG guest_addr;
        void *native_locale;
        ULONG str_addrs[KAFFEINE_LOCALE_MAXSTRMSG];
        UWORD active_calls;
        BOOL close_pending; /* A callback closed a locale still in native use. */
    } locale_map[KAFFEINE_MAX_LOCALES];

    /* MatchFirst() AnchorPaths: a native AnchorPath for each guest one,
       guest copies of its first and current AChain, and the native
       capacity of ap_Buf */
    #define KAFFEINE_MAX_ANCHORS 16
    struct { ULONG guest; void *native; ULONG base; ULONG current; UWORD capacity; } anchor_map[KAFFEINE_MAX_ANCHORS];

    /* Exec I/O of the guest: the native task that runs it, which owns the
       native signal bits the guest allocated, its ports and requests, and
       guest Device identities and the native TimerBase behind timer.device */
    struct Task *exec_task;
    struct KaffeineFile files[KAFFEINE_MAX_FILES];
    struct KaffeineFileSlab *file_slabs;
    ULONG file_serial;
    ULONG packet_trace_count;
    ULONG guest_exec_task;
    IPTR (*file_packet)(struct KaffeineContext *, ULONG, ULONG);
    ULONG message_ports[KAFFEINE_MAX_MESSAGE_PORTS];
    struct KaffeineIcon icons[KAFFEINE_MAX_ICONS];
    /* Borrowed display records: snapshots live until context destruction.
       Their native identities are never registered as generic shadows. */
    struct { const struct MonitorSpec *native; ULONG guest, size; }
        monitors[KAFFEINE_MAX_MONITORS];
    ULONG   guest_signals;
    struct KaffeineIOPort io_ports[KAFFEINE_MAX_IO_PORTS];
    struct KaffeineIORequest io_requests[KAFFEINE_MAX_IO_REQUESTS];
    ULONG   timer_base;
    struct Device *native_timer;
    ULONG   console_base;      /* shared guest Device, outside the library namespace */

    /* Supervisor() redirect: user function address, 0 = none */
    ULONG   sv_redirect;

    /* Custom chip beam position counter (PAL: 313 lines × 227 clocks) */
    UWORD   beam_v;     /* vertical position 0-312 */
    UBYTE   beam_h;     /* horizontal position 0-226 */
    UBYTE   beam_lof;   /* long frame flag (toggled each frame) */
};

static inline void *m68k_to_host(struct KaffeineContext *ctx, ULONG addr)
{
    if (addr == 0 || addr >= ctx->mem_size) return NULL;
    return ctx->mem + addr;
}

/* Shadow-aware pointer resolution: shadow lookup first, then containment */
static inline void *m68k_to_host_or_shadow(struct KaffeineContext *ctx, ULONG addr)
{
    if (addr == 0) return NULL;
    /* Check shadow table first */
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr)
            return ctx->shadow_map[i].native_ptr;
    /* Fall back to containment memory */
    if (addr >= ctx->mem_size) return NULL;
    return ctx->mem + addr;
}

static inline ULONG host_to_m68k(struct KaffeineContext *ctx, void *ptr)
{
    if (ptr == NULL) return 0;
    return (ULONG)((UBYTE *)ptr - ctx->mem);
}

/* Whether seglist is that of a 68k library from disk (kaffeine_m68klib.c),
   whose hunks are guest memory that native DOS must never get */
static inline BOOL Kaffeine_IsM68KSegList(struct KaffeineContext *ctx, ULONG seglist)
{
    UWORD i;

    for (i = 0; seglist && i < KAFFEINE_MAX_M68KLIBS; i++)
        if (ctx->m68klibs[i].name[0] && ctx->m68klibs[i].seglist == seglist)
            return TRUE;
    return FALSE;
}

/* Check the whole access before forming a host pointer. Guest ULONG
   addresses can be near 2^32, so adding the width before checking can wrap. */
static inline UWORD m68k_read16(struct KaffeineContext *ctx, ULONG addr)
{
    if (addr >= ctx->mem_size || ctx->mem_size - addr < 2) return 0;
    UBYTE *p = ctx->mem + addr;
    return (UWORD)((p[0] << 8) | p[1]);
}

static inline ULONG m68k_read32(struct KaffeineContext *ctx, ULONG addr)
{
    if (addr >= ctx->mem_size || ctx->mem_size - addr < 4) return 0;
    UBYTE *p = ctx->mem + addr;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}

static inline void m68k_write16(struct KaffeineContext *ctx, ULONG addr, UWORD val)
{
    if (addr >= ctx->mem_size || ctx->mem_size - addr < 2) return;
    UBYTE *p = ctx->mem + addr;
    p[0] = (UBYTE)(val >> 8);
    p[1] = (UBYTE)(val);
}

static inline void m68k_write32(struct KaffeineContext *ctx, ULONG addr, ULONG val)
{
    if (addr >= ctx->mem_size || ctx->mem_size - addr < 4) return;
    UBYTE *p = ctx->mem + addr;
    p[0] = (UBYTE)(val >> 24);
    p[1] = (UBYTE)(val >> 16);
    p[2] = (UBYTE)(val >> 8);
    p[3] = (UBYTE)(val);
}

#ifdef __cplusplus
extern "C" {
#endif

struct KaffeineContext *Kaffeine_CreateContext(struct KaffeineLibBase *base);
void   Kaffeine_DestroyContext(struct KaffeineContext *ctx);
LONG   Kaffeine_Execute(struct KaffeineContext *ctx);
ULONG  Kaffeine_HeapAlloc(struct KaffeineContext *ctx, ULONG size, ULONG requirements);
ULONG  Kaffeine_HeapAllocAbs(struct KaffeineContext *ctx, ULONG size, ULONG addr);
ULONG  Kaffeine_HeapAllocEntry(struct KaffeineContext *ctx, ULONG entry);
void   Kaffeine_HeapFreeEntry(struct KaffeineContext *ctx, ULONG entry);
BOOL   Kaffeine_HeapInit(struct KaffeineContext *ctx);
void   Kaffeine_HeapFree(struct KaffeineContext *ctx, ULONG addr, ULONG size);
ULONG  Kaffeine_HeapAvail(struct KaffeineContext *ctx, ULONG requirements);
/* Utility allocations: exact base, native ownership and requested capacity. */
ULONG  Kaffeine_TagItemsAlloc(struct KaffeineContext *ctx, ULONG count);
ULONG  Kaffeine_TagItemsCapacity(struct KaffeineContext *ctx, ULONG addr);
void   Kaffeine_TagItemsFree(struct KaffeineContext *ctx, ULONG addr);
/* Locale allocations: exact base and native ownership. */
ULONG  Kaffeine_LocaleAlloc(struct KaffeineContext *ctx, ULONG size);
void   Kaffeine_LocaleFree(struct KaffeineContext *ctx, ULONG addr);
/* AChain copies of MatchFirst(): exact base and native ownership. */
ULONG  Kaffeine_AChainAlloc(struct KaffeineContext *ctx, ULONG size);
void   Kaffeine_AChainFree(struct KaffeineContext *ctx, ULONG addr);
/* 68k mirrors of GadTools menus and gadgets, VisualInfo handles, the 68k
   Screens of locked public screens and the copy of their list: exact
   base, cleared, and native ownership. */
ULONG  Kaffeine_MirrorAlloc(struct KaffeineContext *ctx, ULONG size);
void   Kaffeine_MirrorRelease(struct KaffeineContext *ctx, ULONG addr);
ULONG  Kaffeine_PoolCreate(struct KaffeineContext *ctx, ULONG requirements,
                         ULONG puddle_size, ULONG threshold_size);
void   Kaffeine_PoolDelete(struct KaffeineContext *ctx, ULONG pool);
ULONG  Kaffeine_PoolAlloc(struct KaffeineContext *ctx, ULONG pool, ULONG size);
void   Kaffeine_PoolFree(struct KaffeineContext *ctx, ULONG pool, ULONG addr, ULONG size);
struct HunkRelocTable;
ULONG  Kaffeine_LoadHunks(struct KaffeineContext *ctx, BPTR segList,
                         const struct HunkRelocTable *relocs);
ULONG  Kaffeine_LoadHunksFromMemory(struct KaffeineContext *ctx, UBYTE *data, LONG size);
/* The hunks of a 68k library loaded by DOS, copied as Kaffeine_LoadHunks()
   does but with the [size][next] headers of a seglist, whose guest BPTR
   it returns, or 0 also for more than KAFFEINE_MAX_LIBRARY_HUNKS hunks;
   the entry point of the program stays. */
#define KAFFEINE_MAX_LIBRARY_HUNKS 1000
ULONG  Kaffeine_LoadLibraryHunks(struct KaffeineContext *ctx, BPTR segList,
                                const struct HunkRelocTable *relocs);
/* OpenLibrary() of a 68k library from disk: the result of its Open()
   with version, or 0, also when the library is older than version, whose
   Expunge() an open library then gets, as on AmigaOS 3.2.3
   (kaffeine_m68klib.c). With the program stopped, return from the thunk
   with the live D0. */
ULONG  Kaffeine_OpenM68KLibrary(struct KaffeineContext *ctx, void *cpu, const char *name,
                               ULONG version);
/* CloseLibrary() of base: Close() of the 68k library and TRUE, or FALSE
   when base is none, that of a library or one whose Close() lies in the
   hunks of a library, as the base that Open() makes for each opener */
BOOL   Kaffeine_CloseM68KLibrary(struct KaffeineContext *ctx, void *cpu, ULONG base);
ULONG  Kaffeine_SetupLibBase(struct KaffeineContext *ctx, UWORD lib_id,
                             const char *name, UWORD num_vectors,
                             const struct M68KThunkEntry *thunks, ULONG num_thunks,
                             const struct M68KThunkEntry *gen_thunks, ULONG num_gen_thunks);

/* Lookup fake base by library name (used by OpenLibrary thunk) */
ULONG  Kaffeine_FindLibBase(struct KaffeineContext *ctx, const char *name);
ULONG  Kaffeine_SetupDeviceBase(struct KaffeineContext *ctx, const char *name,
                               UWORD num_vectors,
                               const struct M68KThunkEntry *thunks, ULONG num_thunks);

/* Open/register a library by name, return fake base (no cpu needed) */
ULONG  Kaffeine_OpenLibrary(struct KaffeineContext *ctx, const char *name);

/* Scan containment memory for C startup auto-open tables and fill them */
void   Kaffeine_AutoOpenLibs(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
