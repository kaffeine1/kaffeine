/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
    kaffeine.library internals — full containment memory model
*/
#ifndef KAFFEINE_INTERN_H
#define KAFFEINE_INTERN_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <dos/bptr.h>

struct KaffeineLibBase
{
    struct Library          lib;
    struct ExecBase        *sysBase;
    struct DosLibrary      *dosBase;
    struct SignalSemaphore segment_ids_lock;
    ULONG segment_next_token;
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
/* Two classic capacity descriptors and their shared guest name. These
   describe the containment heap, not native Exec memory or free lists. */
#define KAFFEINE_MEMINFO_SIZE    96

struct M68KHeapBlock
{
    ULONG size;
    ULONG next;
};

struct M68KThunkEntry;
struct M68KHeapAllocation;
struct KaffeineCPU;
struct KaffeineScheduler;
struct KaffeineTaskOps;
struct KaffeineCPUFactory;
struct IORequest;
struct Message;
struct MsgPort;
struct DevProc;
struct Task;
struct Device;
struct TextFont;
struct DiskObject;
struct Window;
struct Requester;
struct Layer;
struct IntuiMessage;
struct Screen;
struct DrawInfo;
struct KaffeineMenuStrip;
struct KaffeineGadgetBatch;
struct KaffeineWindowGadgets;
struct KaffeineClassic;
struct KaffeineScreenKeep;
struct KaffeineLayers;
struct KaffeineLocks;
struct KaffeineSegment;
struct KaffeineDomainModule;
struct KaffeineRemember;
struct KaffeineVideoExtra;
struct KaffeineIDCMPPort;
struct KaffeineLayerOps;

/* Completed slices of the outermost Kaffeine_RunCPU() between two calls of
   ctx->gui_poll */
#define KAFFEINE_GUI_POLL_SLICES 4096

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
   open, a native request of the native size stands behind it, and only
   kaffeine_exec_io.c passes it to native Exec. Originals and 40-byte copies
   sent with SendIO() all reply to ctx->io_reply_port. Each copy has a
   native open of its own on the same unit; a native address identifies
   each completion on the shared private FIFO. */
#define KAFFEINE_MAX_IO_REQUESTS 16
enum KaffeineIODeviceKind
{
    KAFFEINE_IO_NONE, KAFFEINE_IO_TIMER, KAFFEINE_IO_CONSOLE,
    KAFFEINE_IO_KEYBOARD, KAFFEINE_IO_INPUT
};
struct KaffeineIORequest
{
    ULONG   guest;              /* 68k IORequest, 0 for a free entry */
    ULONG   capacity;           /* bytes from CreateIORequest(), 0 for guest storage */
    struct IORequest *native;   /* while open */
    struct MsgPort   *reply;     /* non-owning shared-port alias for an original */
    ULONG   unit;
    enum KaffeineIODeviceKind kind; /* native state, never taken from guest fields */
    ULONG   open_flags;         /* OpenDevice() D1, which copies open with */
    ULONG   port;               /* guest mn_ReplyPort captured by SendIO() */
    ULONG   owner;              /* trusted guest task captured by SendIO(), 0 before sending */
    UBYTE   pending;            /* the native request is at the device */
    UBYTE   replied;            /* native idle, guest results final: the node may be on port */
    UBYTE   waiting;            /* the owner waits in WaitIO(): 1 in the call, 2 blocked */
    UBYTE   copy;               /* a copy record; reply is unused */
    UBYTE   orphan;             /* storage freed while pending: reaped, never written */
    UBYTE   stack_message;      /* original unowned timer sent with mn_Length == 0 */
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
    BOOL    dos;                /* dos.library: no call of a CreateProc() child reaches it */
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
    struct KaffeineVideoExtra *video_extras;
    ULONG video_extra_count; /* includes retired identities until arena destruction */
    BOOL video_closing;
    void (*video_screen_retire)(struct KaffeineContext *, struct Screen *);
    BOOL (*video_protected)(struct KaffeineContext *, ULONG, ULONG);
    ULONG (*video_extra_guest)(struct KaffeineContext *, struct Screen *, ULONG);
    void (*video_destroy)(struct KaffeineContext *);
    struct KaffeineRemember *remembers;
    ULONG remember_count; /* includes retired node identities until arena destruction */
    BOOL remember_closing;
    void (*remember_destroy)(struct KaffeineContext *);
    struct KaffeineDomainModule *domain_modules;
    ULONG domain_module_count;
    BOOL domain_modules_closing, domain_cleanup_running;
    void (*domain_cleanup)(struct KaffeineContext *);
    void (*domain_destroy)(struct KaffeineContext *);
    struct KaffeineSegment *segments;
    ULONG segment_count; /* live, loaned and transfer entries plus pending LoadBegin */
    void (*segments_cleanup)(struct KaffeineContext *);
    BOOL segments_closing, segments_cleanup_running;
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
       with the carrier of each (native). The AppIcon of an icon.library
       DiskObject of the program keeps a DupDiskObjectA() clone of its
       native object (clone), freed through icon_base, a reference to
       icon.library of its own, after Workbench has let the AppIcon go. */
    #define KAFFEINE_MAX_APP_PORTS 8
    struct { ULONG guest; struct MsgPort *native; } app_ports[KAFFEINE_MAX_APP_PORTS];
    #define KAFFEINE_MAX_APP_OBJECTS 32
    struct { ULONG guest; UBYTE kind; UBYTE port; APTR native; APTR copy;
             struct DiskObject *clone; struct Library *icon_base; } app_objects[KAFFEINE_MAX_APP_OBJECTS];
    #define KAFFEINE_MAX_APP_MESSAGES 32
    struct { ULONG guest; UBYTE port; struct Message *native; } app_messages[KAFFEINE_MAX_APP_MESSAGES];

    struct KaffeineLibBase *emuBase;

    /* The native driver keeps this domain until every independent guest
       Process has ended. Only its owner invokes the completion callback. */
    void (*root_complete)(struct KaffeineContext *, LONG);
    APTR root_completion_data;
    BOOL broker_call; /* selected DOS thunk is executing on its owner worker */
    struct Task *broker_owner; /* valid only while that private RPC runs */
    ULONG root_streams[3]; /* caller-borrowed streams retired before early return */
    struct KaffeineDomainStartRecovery *domain_start_recovery;
    BOOL (*domain_drain)(struct KaffeineContext *); /* FALSE waits one native tick */

    ULONG   m68k_argptr;
    ULONG   m68k_argsize;
    ULONG   m68k_sysbase;
    ULONG   memory_info; /* owned, read-only MemList capacity snapshots */

    /* CLI snapshot shared by Cli(), ThisTask and FindTask(NULL). */
    ULONG   m68k_cli;

    BOOL    running;
    LONG    exit_code;
    UWORD   callback_depth;
    BOOL    callback_failed; /* Suppress pending backend accesses after a stop. */
    /* The CPU in Kaffeine_RunCPU(), NULL outside it: the loaders of 68k
       code have no CPU handle to tell its backend that code changed. */
    struct KaffeineCPU *cpu;
    /* JIT CPUs share the arena, but own separate translation caches.
       A flush by one CPU invalidates the others at their next slice. */
    unsigned long long jit_generation;
    /* Guest tasks (kaffeine_sched.c): the scheduler while the program has
       AddTask() or CreateProc() children, else NULL; its entry points,
       without which AddTask() stops the program and CreateProc() fails;
       and the backend's CPU factory while its
       Kaffeine_Execute() runs. Children are never native Exec tasks. */
    struct KaffeineScheduler *sched;
    const struct KaffeineTaskOps *task_ops;
    const struct KaffeineCPUFactory *cpu_factory;
    /* Optional: the outermost Kaffeine_RunCPU() calls it after every
       KAFFEINE_GUI_POLL_SLICES completed slices, between slices, where no
       thunk and no guest callback runs, also without guest tasks. It
       brings native GUI snapshots up to date (kaffeine_layers.c). */
    void (*gui_poll)(struct KaffeineContext *ctx);
    ULONG gui_poll_slices;
    /* Asynchronous Exec I/O (kaffeine_exec_io.c), set by its first SendIO(),
       hooks so that the queues, the heap, the scheduler and the Exec thunks
       link without it. io_poll moves native completions to their guest
       requests and ports, FALSE when that stopped the guest. io_wait, for
       an empty guest port without guest tasks, waits natively and returns
       TRUE when a request in flight replies to it, else FALSE. io_release
       decides about guest memory that the heap frees: FALSE keeps it
       allocated and changes nothing; commit FALSE only asks. io_signals are
       the private native reply-port bits for their entire lifetime,
       never the guest's. */
    BOOL (*io_poll)(struct KaffeineContext *ctx);
    BOOL (*io_wait)(struct KaffeineContext *ctx, ULONG port);
    BOOL (*io_release)(struct KaffeineContext *ctx, ULONG addr, ULONG size, BOOL commit);
    void (*io_task_removed)(struct KaffeineContext *ctx, ULONG task);
    ULONG io_signals;
    /* Window-owned peers of ordinary guest ports. Native Intuition sees
       only these private inboxes, with the driver's task and a validated
       guest-owned signal. Retire before port storage, signals or owners go. */
    struct KaffeineIDCMPPort *idcmp_ports;
    void (*idcmp_port_gone)(struct KaffeineContext *, ULONG);
    void (*idcmp_signal_gone)(struct KaffeineContext *, BYTE);
    void (*idcmp_task_gone)(struct KaffeineContext *, ULONG);
    BOOL (*idcmp_release)(struct KaffeineContext *, ULONG, ULONG, BOOL);
    void (*idcmp_cleanup)(struct KaffeineContext *);
    /* Shadow struct mapping: m68k addr <-> native ptr */
    #define KAFFEINE_MAX_SHADOWS 1024
    struct { ULONG m68k_addr; void *native_ptr; UWORD type;
             ULONG idcmp_window; /* immutable owner of an IDCMP mirror, else 0 */
    } shadow_map[KAFFEINE_MAX_SHADOWS];
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

    /* Active requesters of Request() of the program (kaffeine_requester.c):
       the 68k Requester and 68k Window of each, the older 68k Requester of
       the same window as the bridge linked it, and the bridge's native
       copy that Intuition has instead, with the block of the copies of its
       texts, borders and image, and the native layer of the requester
       with the 68k Layer that the layer bridge gave for it (ReqLayer); a
       free slot has no guest. requests_closing, set with the first one,
       ends those of a window natively after its layer locks went back and
       before its menus and gadgets go, never touching the 68k Requesters,
       which may be gone by then. */
    #define KAFFEINE_MAX_REQUESTS 32
    struct {
        ULONG guest, window, older, layer;
        struct Window *native_window;
        struct Requester *native;
        struct Layer *native_layer;
        APTR block;
    } requests[KAFFEINE_MAX_REQUESTS];
    void (*requests_closing)(struct KaffeineContext *ctx, ULONG window, struct Window *w);

    /* Layers (kaffeine_layers.c): the 68k snapshots of native layers, and
       what the window and screen bridges tell them; both NULL without the
       module. */
    struct KaffeineLayers *layers;
    struct KaffeineLocks *locks;
    BPTR legacy_device_lock; /* private duplicate backing DeviceProc IoErr */
    const struct KaffeineLayerOps *layer_ops;
    /* BltBitMap() calls of the program in progress (kaffeine_layers.c),
       also without the layer bridge: while any runs, no LayerInfo of the
       program is disposed of and no screen of the program closes, so that
       every native BitMap that a blit got outlives it. */
    UWORD bitmap_blits;
    /* Regions of NewRegion() of the program (kaffeine_regions.c), made
       with the first one; NULL before. */
    struct KaffeineRegions *regions;

    /* Screens of the program (kaffeine_screens.c): one 68k Screen for each
       native screen, with its 68k TextAttr, kept while the program holds a
       lock on it, has a window there or holds the list of public screens,
       and for a screen that it opened (owned) until CloseScreen() closes
       it; keep is the native copy of the title and the font that Intuition
       points to while such a screen is open. screen_release, set with the
       first one, lets the window bridge say that a 68k Window on that
       screen has gone. pen_refs counts the ObtainBestPenA() results of the
       program on the screen's ColorMap that it has not released, by pen
       (kaffeine_palette.c), pens their sum; while any is left the entry
       stays, and a screen that the program did not open also keeps one
       native visitor lock of the bridge (pen_lock). pen_leases is the sum
       over all screens. */
    #define KAFFEINE_MAX_PUBSCREENS 16
    #define KAFFEINE_MAX_SCREEN_PENS 256
    #define KAFFEINE_MAX_PEN_LEASES 4096
    struct {
        struct Screen *native; ULONG guest; ULONG font; ULONG colormap; UWORD locks;
        BOOL owned; struct KaffeineScreenKeep *keep;
        UWORD pen_refs[KAFFEINE_MAX_SCREEN_PENS]; UWORD pens; BOOL pen_lock;
    } pubscreens[KAFFEINE_MAX_PUBSCREENS];
    UWORD pen_leases;
    void (*screen_release)(struct KaffeineContext *ctx, struct Screen *screen);
    /* VisualInfo keeps DrawInfo/Screen alive. Borrowed screens also hold
       a native visitor lock until their VisualInfo references are freed. */
    #define KAFFEINE_MAX_VISUALINFOS 32
    struct {
        APTR native; struct Screen *screen; ULONG guest; BOOL public_lock;
    } visualinfos[KAFFEINE_MAX_VISUALINFOS];
    /* 68k copies of GetScreenDrawInfo() (kaffeine_screens.c): one for each
       native DrawInfo of a screen of the program, with the gets that the
       program has not freed (refs). Each keeps the 68k Screen of its screen,
       and a borrowed one also holds a native visitor lock (public_lock),
       until its last free or the end of the program. */
    #define KAFFEINE_MAX_DRAWINFOS 16
    struct {
        struct Screen *screen; struct DrawInfo *native; ULONG guest; UWORD refs;
        BOOL public_lock;
    } drawinfos[KAFFEINE_MAX_DRAWINFOS];
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

    /* ExAll() scans: a native FileInfoBlock for each guest ExAllControl,
       which holds the scan position and an entry that did not fit yet.
       owned: the control came from AllocDosObject(); active: its
       MatchFunc is running */
    #define KAFFEINE_MAX_EXALL 8
    struct { ULONG guest; struct FileInfoBlock *fib; BOOL owned, started, pending, ended, active; } exall_map[KAFFEINE_MAX_EXALL];

    /* StandardPackets that AllocDosObject(DOS_STDPKT) gave the program, by
       the guest address of their Message */
    #define KAFFEINE_MAX_STDPKTS 32
    ULONG stdpkt_map[KAFFEINE_MAX_STDPKTS];

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
    /* Guest ports of native DOS handlers, one for each native port, and
       the DevProcs of GetDeviceProc() (kaffeine_handlers.c) */
    #define KAFFEINE_MAX_HANDLERS 32
    struct { ULONG guest; struct MsgPort *native; } handlers[KAFFEINE_MAX_HANDLERS];
    #define KAFFEINE_MAX_DEVPROCS 16
    struct { ULONG guest; struct DevProc *native; BPTR view_lock; } devprocs[KAFFEINE_MAX_DEVPROCS];
    IPTR (*handler_packet)(struct KaffeineContext *, ULONG, ULONG);
    ULONG message_ports[KAFFEINE_MAX_MESSAGE_PORTS];
    struct KaffeineIcon icons[KAFFEINE_MAX_ICONS];
    /* Borrowed display records: snapshots live until context destruction.
       Their native identities are never registered as generic shadows. */
    struct { const struct MonitorSpec *native; ULONG guest, size; }
        monitors[KAFFEINE_MAX_MONITORS];
    ULONG   guest_signals;
    struct KaffeineIOPort io_ports[KAFFEINE_MAX_IO_PORTS];
    struct KaffeineIORequest io_requests[KAFFEINE_MAX_IO_REQUESTS];
    struct MsgPort *io_reply_port;     /* one native port/signal, while any native IO request exists */
    ULONG   timer_base;
    struct Device *native_timer;
    ULONG   console_base;      /* shared guest Device, outside the library namespace */
    ULONG   keyboard_base;     /* guest Devices of unit 0, with no native pointer */
    ULONG   input_base;

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
/* AllocEntry() of a bridge-owned specification: count pairs of requirements
   and length in native memory. The guest MemList, or bit 31 on failure,
   which leaves nothing allocated; Kaffeine_HeapFreeEntry() frees it. */
ULONG  Kaffeine_HeapAllocList(struct KaffeineContext *ctx, const ULONG *spec, UWORD count);
void   Kaffeine_HeapFreeEntry(struct KaffeineContext *ctx, ULONG entry);
BOOL   Kaffeine_HeapInit(struct KaffeineContext *ctx);
void   Kaffeine_HeapFree(struct KaffeineContext *ctx, ULONG addr, ULONG size);
/* Free exactly the ordinary, unpooled allocation at addr of the requested
   size; FALSE with no effect for anything else, also while I/O keeps it. */
BOOL   Kaffeine_HeapFreeExact(struct KaffeineContext *ctx, ULONG addr, ULONG size);
/* Whether the part of a guest range inside the heap lies only in payloads
   of the program's allocations (AllocMem, AllocAbs, AllocEntry, pooled),
   never in free, reserved, pool, mirror or other bridge-owned intervals or
   their headers. Memory outside the heap (hunks, stack) is not its own. */
BOOL   Kaffeine_HeapGuestRange(struct KaffeineContext *ctx, ULONG addr, ULONG size);
ULONG  Kaffeine_HeapAvail(struct KaffeineContext *ctx, ULONG requirements);
/* Utility allocations: exact base, native ownership and requested capacity. */
ULONG  Kaffeine_TagItemsAlloc(struct KaffeineContext *ctx, ULONG count);
ULONG  Kaffeine_TagItemsCapacity(struct KaffeineContext *ctx, ULONG addr);
BOOL   Kaffeine_TagItemsRange(struct KaffeineContext *ctx, ULONG addr, ULONG size);
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
BOOL   Kaffeine_MirrorAbort(struct KaffeineContext *ctx, ULONG addr);
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
ULONG  Kaffeine_SetupLibBaseSized(struct KaffeineContext *, UWORD lib_id,
                                  const char *name, UWORD vectors, UWORD positive_size,
                                  const struct M68KThunkEntry *, ULONG,
                                  const struct M68KThunkEntry *, ULONG);
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
