/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
    kaffeine.library — native thunk implementations for exec and dos
*/
#include <exec/types.h>
#include <exec/memory.h>
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/rdargs.h>
#include <dos/dosextens.h>
#include <string.h>

#include <proto/intuition.h>
#include <proto/graphics.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_shadow.h"
#include "kaffeine_thunks.h"
#include "kaffeine_format.h"
#include "kaffeine_lists.h"
#include "kaffeine_memheader.h"
#include "kaffeine_thunks_memory.h"
#include "kaffeine_thunks_dos.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_thunks_utility.h"
#include "kaffeine_thunks_locale.h"
#include "kaffeine_thunks_mathieee.h"
#include "kaffeine_thunks_mathffp.h"
#include "kaffeine_readargs.h"
#include "kaffeine_cli.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_thunks_gfx.h"
#include "kaffeine_display.h"
#include "kaffeine_thunks_diskfont.h"
#include "kaffeine_rastport.h"
#include "kaffeine_window.h"
#include "kaffeine_menus.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_screens.h"
#include "kaffeine_tasks.h"
#include "kaffeine_sched.h"
#include "kaffeine_workbench.h"
#include "kaffeine_icon.h"
#include "kaffeine_layers.h"
#include "kaffeine_regions.h"

extern const struct M68KThunkEntry kaffeine_thunks_intuition_manual[];
extern const ULONG kaffeine_thunks_intuition_manual_count;
extern const struct M68KThunkEntry kaffeine_thunks_gadtools_manual[];
extern const ULONG kaffeine_thunks_gadtools_manual_count;
extern const struct M68KThunkEntry kaffeine_thunks_graphics_manual[];
extern const ULONG kaffeine_thunks_graphics_manual_count;
extern const struct M68KThunkEntry kaffeine_thunks_cybergraphics_manual[];
extern const ULONG kaffeine_thunks_cybergraphics_manual_count;

const struct M68KThunkEntry kaffeine_thunks_mathffp[] = {
    KAFFEINE_MATHFFP_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_mathffp_count =
    sizeof(kaffeine_thunks_mathffp) / sizeof(kaffeine_thunks_mathffp[0]) - 1;

/* ── exec.library thunks ─────────────────────────────────────────── */

/* Open a library by name and return its fake base (no cpu needed) */
ULONG Kaffeine_OpenLibrary(struct KaffeineContext *ctx, const char *name)
{
    if (!name) return 0;

    /* Exec identity is the file part; keep its exact case and leave the
       caller's full path intact for the 68k disk loader. */
    const char *file = name;
    for (const char *p = name; *p; p++)
        if (*p == ':' || *p == '/')
            file = p + 1;
    if (!*file) return 0;
    name = file;

    ULONG base = Kaffeine_FindLibBase(ctx, name);
    if (base) return base;

    for (ULONG i = 0; i < kaffeine_all_gen_libs_count; i++)
    {
        if (strcmp(name, kaffeine_all_gen_libs[i].name) == 0)
        {
            const struct M68KThunkEntry *manual = NULL;
            ULONG manual_count = 0;
            if (strcmp(name, "intuition.library") == 0) {
                manual = kaffeine_thunks_intuition_manual;
                manual_count = kaffeine_thunks_intuition_manual_count;
            } else if (strcmp(name, "graphics.library") == 0) {
                manual = kaffeine_thunks_graphics_manual;
                manual_count = kaffeine_thunks_graphics_manual_count;
            } else if (strcmp(name, "cybergraphics.library") == 0) {
                manual = kaffeine_thunks_cybergraphics_manual;
                manual_count = kaffeine_thunks_cybergraphics_manual_count;
            } else if (strcmp(name, "diskfont.library") == 0) {
                manual = kaffeine_thunks_diskfont_manual;
                manual_count = kaffeine_thunks_diskfont_manual_count;
            } else if (strcmp(name, "utility.library") == 0) {
                manual = kaffeine_thunks_utility_manual;
                manual_count = kaffeine_thunks_utility_manual_count;
            } else if (strcmp(name, "locale.library") == 0) {
                manual = kaffeine_thunks_locale_manual;
                manual_count = kaffeine_thunks_locale_manual_count;
            } else if (strcmp(name, "gadtools.library") == 0) {
                manual = kaffeine_thunks_gadtools_manual;
                manual_count = kaffeine_thunks_gadtools_manual_count;
            } else if (strcmp(name, "icon.library") == 0) {
                manual = kaffeine_thunks_icon_manual;
                manual_count = kaffeine_thunks_icon_manual_count;
            } else if (strcmp(name, "layers.library") == 0) {
                manual = kaffeine_thunks_layers_manual;
                manual_count = kaffeine_thunks_layers_manual_count;
            }
            return Kaffeine_SetupLibBase(ctx, ctx->num_libs, kaffeine_all_gen_libs[i].name,
                                        KAFFEINE_MAX_LVO,
                                        manual, manual_count,
                                        kaffeine_all_gen_libs[i].thunks,
                                        kaffeine_all_gen_libs[i].count);
        }
    }
    if (strcmp(name, "mathffp.library") == 0)
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, "mathffp.library",
                                    KAFFEINE_MAX_LVO,
                                    kaffeine_thunks_mathffp,
                                    kaffeine_thunks_mathffp_count,
                                    NULL, 0);
    if (strcmp(name, "workbench.library") == 0)
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, "workbench.library",
                                    KAFFEINE_MAX_LVO,
                                    kaffeine_thunks_workbench,
                                    kaffeine_thunks_workbench_count,
                                    NULL, 0);
    if (strcmp(name, "mathieeedoubbas.library") == 0)
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, "mathieeedoubbas.library",
                                    KAFFEINE_MAX_LVO,
                                    kaffeine_thunks_mathieeedoubbas,
                                    kaffeine_thunks_mathieeedoubbas_count,
                                    NULL, 0);
    if (strcmp(name, "mathieeedoubtrans.library") == 0)
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, "mathieeedoubtrans.library",
                                    KAFFEINE_MAX_LVO,
                                    kaffeine_thunks_mathieeedoubtrans,
                                    kaffeine_thunks_mathieeedoubtrans_count,
                                    NULL, 0);
    if (strcmp(name, "mathieeesingbas.library") == 0)
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, "mathieeesingbas.library",
                                    KAFFEINE_MAX_LVO,
                                    kaffeine_thunks_mathieeesingbas,
                                    kaffeine_thunks_mathieeesingbas_count,
                                    NULL, 0);
    return 0;
}

/* OpenLibrary() and OldOpenLibrary(): a native library through its thunks,
   whose version the bridge does not check, else a 68k library from disk
   through its Open() (kaffeine_m68klib.c). The name is a copy of at most
   255 bytes, as a DOS path, since the init of a 68k library can change
   the guest one. The trace names every library with the version asked
   for and how it was answered, so that the trace of an application lists
   what it needs. */
static IPTR open_library(struct KaffeineContext *ctx, void *cpu, ULONG version)
{
    ULONG at = THUNK_A(1), base, n;
    char name[256];

    for (n = 0; at && at + n < ctx->mem_size && n < sizeof(name); n++)
        if (!(name[n] = (char)ctx->mem[at + n]))
            break;
    if (!at || at + n >= ctx->mem_size || n == sizeof(name))
    {
        if (at)
            bug("[kaffeine] OpenLibrary: no name of at most 255 bytes at 0x%lx\n",
                (unsigned long)at);
        return 0;
    }
    if ((base = Kaffeine_OpenLibrary(ctx, name)))
    {
        bug("[kaffeine] OpenLibrary %s %lu: native\n", name, (unsigned long)version);
        return (IPTR)base;
    }
    /* no native library: a 68k one from disk */
    base = Kaffeine_OpenM68KLibrary(ctx, cpu, name, version);
    if (!ctx->running)
        return (IPTR)ctx->exit_code;
    if (base)
    {
        bug("[kaffeine] OpenLibrary %s %lu: 68k at 0x%lx\n", name, (unsigned long)version,
            (unsigned long)base);
        return (IPTR)base;
    }
    bug("[kaffeine] OpenLibrary %s %lu: not found\n", name, (unsigned long)version);
    return 0;
}

/* -552: OpenLibrary(A1=name, D0=version) -> D0=library. Classic callers
   may set D0.W alone: the version is the low word, as AmigaOS 3.2.3 takes
   it and AROS for m68k does (rom/exec/openlibrary.c). */
static IPTR thunk_exec_OpenLibrary(struct KaffeineContext *ctx, void *cpu)
{
    return open_library(ctx, cpu, (UWORD)THUNK_D(0));
}

/* -408: OldOpenLibrary(A1=name) -> D0=library */
static IPTR thunk_exec_OldOpenLibrary(struct KaffeineContext *ctx, void *cpu)
{
    return open_library(ctx, cpu, 0);
}

/* -414: CloseLibrary(A1=library): the Close() of a 68k library from disk;
   nothing for the base of a native library */
static IPTR thunk_exec_CloseLibrary(struct KaffeineContext *ctx, void *cpu)
{
    if (THUNK_A(1) && Kaffeine_CloseM68KLibrary(ctx, cpu, THUNK_A(1)) && !ctx->running)
        return (IPTR)ctx->exit_code;
    return 0;
}

/* -120/-126: Disable()/Enable() only affect guest interrupt nesting. */
static IPTR thunk_exec_Disable(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_TaskDisable(ctx, cpu);
}

static IPTR thunk_exec_Enable(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_TaskEnable(ctx, cpu);
}

/* -132: Forbid(), -138: Permit(). No native effect: they count in the guest
   ExecBase TDNestCnt, which keeps guest tasks from switching. */
static IPTR thunk_exec_Forbid(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_TaskForbid(ctx, cpu);
}

static IPTR thunk_exec_Permit(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_TaskPermit(ctx, cpu);
}

/* -300: SetTaskPri(task=a1, priority=d0). Validate the root identity even
   before the first AddTask; guest records never become native Tasks. */
static IPTR thunk_exec_SetTaskPri(struct KaffeineContext *ctx, void *cpu)
{
    if (ctx->task_ops)
        return ctx->task_ops->set_task_pri(ctx, cpu);
    Kaffeine_StopGuest(ctx, cpu, 20);
    return 20;
}

/* -318: Wait(D0=signalSet) -> D0. With AddTask() children the other guest
   tasks run while one waits; otherwise the native task waits. */
static IPTR thunk_exec_Wait(struct KaffeineContext *ctx, void *cpu)
{
    if (ctx->sched)
        return ctx->task_ops->wait(ctx, cpu);
    return (IPTR)Wait(THUNK_D(0));
}

/* -534: TypeOfMem(A1=address) -> D0=attributes */
static IPTR thunk_exec_TypeOfMem(struct KaffeineContext *ctx, void *cpu)
{
    return MEMF_PUBLIC; /* everything is public in containment */
}


/*
 * Build a native RawDoFmt() datastream from the m68k one.
 *
 * RawDoFmt() fetches each argument at the width its specifier implies -
 * a WORD for %d, a LONG for %ld and a pointer for %s - and advances the
 * stream by exactly that much (see fetch_mem_arg() in rom/exec/rawdofmt.c).
 * An array of uniformly sized slots is therefore not a datastream: on
 * 64-bit every pointer slot is twice the size of the LONG the reader
 * expects, and on 32-bit a WORD is read from a slot twice its size, so
 * the reader loses sync with the second argument onwards.
 *
 * The size a slot takes is the same on both ABIs for %d (2) and %ld (4),
 * and follows the pointer for %s, so packing by specifier is correct for
 * 32-bit and 64-bit alike.
 *
 * Returns the stream, and its size through out_size for FreeMem().
 */
APTR m68k_to_native_fmt_stream(struct KaffeineContext *ctx, const char *fmt, ULONG m68k_args, ULONG *out_size)
{
    *out_size = 0;

    if (!fmt || !m68k_args)
        return NULL;

    int nargs = 0;
    for (const char *f = fmt; *f; f++)
        if (*f == '%' && f[1] && f[1] != '%') nargs++;
    if (!nargs)
        return NULL;

    /* A pointer is the widest slot any one argument can take */
    ULONG size = nargs * sizeof(APTR);
    UBYTE *stream = (UBYTE *)AllocMem(size, MEMF_CLEAR);
    if (!stream)
        return NULL;

    UBYTE *src = (UBYTE *)m68k_to_host(ctx, m68k_args);
    UBYTE *dst = stream;
    int ai = 0;

    for (const char *f = fmt; *f && ai < nargs; f++) {
        if (*f != '%') continue;
        f++;
        if (*f == '%') continue;
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '0' || (*f >= '1' && *f <= '9') || *f == '.') f++;
        int is_long = 0;
        if (*f == 'l') { is_long = 1; f++; }

        if (*f == 's' || *f == 'b') {
            /* m68k passes a 32bit address - hand over the host one */
            ULONG p = ((ULONG)src[0]<<24)|((ULONG)src[1]<<16)|((ULONG)src[2]<<8)|src[3];
            src += 4;
            APTR hostptr = (*f == 's') ? m68k_to_host(ctx, p) : (APTR)(IPTR)p;
            CopyMem(&hostptr, dst, sizeof(APTR));
            dst += sizeof(APTR);
        } else if (*f == 'd' || *f == 'u' || *f == 'x' || *f == 'c') {
            if (is_long) {
                ULONG v = ((ULONG)src[0]<<24)|((ULONG)src[1]<<16)|((ULONG)src[2]<<8)|src[3];
                src += 4;
                CopyMem(&v, dst, sizeof(ULONG));
                dst += sizeof(ULONG);
            } else {
                UWORD v = (UWORD)((src[0]<<8)|src[1]);
                src += 2;
                CopyMem(&v, dst, sizeof(UWORD));
                dst += sizeof(UWORD);
            }
        } else {
            ULONG v = ((ULONG)src[0]<<24)|((ULONG)src[1]<<16)|((ULONG)src[2]<<8)|src[3];
            src += 4;
            CopyMem(&v, dst, sizeof(ULONG));
            dst += sizeof(ULONG);
        }
        ai++;
    }

    *out_size = size;
    return stream;
}








/* exec -306: SetSignal(D0=newSignals, D1=signalSet) -> D0=oldSignals, of
   the current guest task */
static IPTR thunk_exec_SetSignal(struct KaffeineContext *ctx, void *cpu)
{
    if (ctx->sched)
        return ctx->task_ops->set_signal(ctx, cpu);
    return (IPTR)SetSignal(THUNK_D(0), THUNK_D(1));
}

/* FindName on an explicitly registered native list, never on guest bytes. */
IPTR Kaffeine_FindNativeListName(struct KaffeineContext *ctx,
                              void *native_list, const char *name)
{
    struct List *list = (struct List *)native_list;
    if (!list || !name) return 0;
    struct Node *node = FindName(list, name);
    if (!node) return 0;
    /* For libraries/devices, return our fake base if we have one,
       otherwise return 0 — a shadow struct would crash if used as a base */
    if (node->ln_Type == NT_LIBRARY || node->ln_Type == NT_DEVICE)
    {
        ULONG base = Kaffeine_FindLibBase(ctx, name);
        return base ? (IPTR)base : 0;
    }
    return shadow_create_by_name(ctx, "Node", node);
}
/* exec -30: Supervisor(A5=userFunc) -> D0
   The user function runs in supervisor mode and typically does:
     MOVE.W SR,D0
     RTE
   We redirect execution to the user function after the LINEA/RTE return.
   willExecute detects sv_redirect and adjusts the exception frame target. */
static IPTR thunk_exec_Supervisor(struct KaffeineContext *ctx, void *cpu)
{
    ULONG userFunc = THUNK_A(5);
    bug("[kaffeine] Supervisor: userFunc=0x%lx\n", (unsigned long)userFunc);
    ctx->sv_redirect = userFunc;
    return 0; /* D0 will be set by the user function */
}

const struct M68KThunkEntry kaffeine_thunks_exec[] = {
    KAFFEINE_EXEC_LIST_THUNKS
    KAFFEINE_EXEC_MEMHEADER_THUNKS
    KAFFEINE_EXEC_MEMORY_THUNKS
    { 30, thunk_exec_Supervisor },
    { 552, thunk_exec_OpenLibrary },
    { 408, thunk_exec_OldOpenLibrary },
    { 414, thunk_exec_CloseLibrary },
    KAFFEINE_EXEC_TASK_THUNKS,
    { 120, thunk_exec_Disable },
    { 126, thunk_exec_Enable },
    { 132, thunk_exec_Forbid },
    { 138, thunk_exec_Permit },
    { 300, thunk_exec_SetTaskPri },
    { 318, thunk_exec_Wait },
    { 534, thunk_exec_TypeOfMem },
    KAFFEINE_EXEC_COMPAT_THUNKS,
    KAFFEINE_EXEC_IO_THUNKS,
    KAFFEINE_EXEC_IDCMP_THUNKS,
    { 306, thunk_exec_SetSignal },
    { 522, thunk_exec_RawDoFmt },
    { 0, NULL }
};
const ULONG kaffeine_thunks_exec_count =
    sizeof(kaffeine_thunks_exec) / sizeof(kaffeine_thunks_exec[0]) - 1;

/* ── dos.library thunks ──────────────────────────────────────────── */

/* -60: Output() -> D0=filehandle */


/* -36: Close(D1=file) -> D0=success */


/* -66: Seek(D1=file, D2=position, D3=offset) -> D0=oldPosition */


/* -132: IoErr() -> D0=result */
static IPTR thunk_dos_IoErr(struct KaffeineContext *ctx, void *cpu)
{
    return (IPTR)IoErr();
}

/* -462: SetIoErr(D1=result) -> D0=previous */
static IPTR thunk_dos_SetIoErr(struct KaffeineContext *ctx, void *cpu)
{
    return (IPTR)SetIoErr((LONG)THUNK_D(1));
}

/* -90: UnLock(D1=lock) */
static IPTR thunk_dos_UnLock(struct KaffeineContext *ctx, void *cpu)
{
    UnLock((BPTR)THUNK_D(1));
    return 0;
}

/* -198: Delay(D1=timeout) */
static IPTR thunk_dos_Delay(struct KaffeineContext *ctx, void *cpu)
{
    Delay((ULONG)THUNK_D(1));
    return 0;
}


/* -126: CurrentDir(D1=lock) -> D0=oldLock */
static IPTR thunk_dos_CurrentDir(struct KaffeineContext *ctx, void *cpu)
{
    return (IPTR)CurrentDir((BPTR)THUNK_D(1));
}


/* -54: Input() -> D0=filehandle */


/* ── DOS: Object Alloc ── */

static IPTR thunk_dos_AllocDosObject(struct KaffeineContext *ctx, void *cpu)
{
    ULONG type = THUNK_D(1);
    /* A blank native FileHandle cannot be exposed or filled by guest code. */
    if (type == DOS_FILEHANDLE)
    {
        SetIoErr(ERROR_NOT_IMPLEMENTED);
        return 0;
    }
    /* The program fills in its RDArgs itself, so it gets a 68k one */
    if (type == DOS_RDARGS)
        return Kaffeine_AllocRDArgs(ctx);
    /* ExAll() keeps its native state on the bridge */
    if (type == DOS_EXALLCONTROL)
        return Kaffeine_ExAllAllocControl(ctx);
    /* Packets travel through guest queues, so they live in guest memory */
    if (type == DOS_STDPKT)
        return Kaffeine_AllocStdPacket(ctx);
    struct TagItem *tags = m68k_to_native_taglist(ctx, THUNK_D(2));
    APTR obj = AllocDosObject(type, tags);
    if (tags) FreeVec(tags);
    if (!obj) return 0;
    if (type == DOS_FIB) return shadow_create_by_name(ctx, "FileInfoBlock", obj);
    return (IPTR)obj;
}

/* ── DOS: Segments ── */

/* -156: UnLoadSeg(D1=seglist): native DOS for its own seglists; that of a
   68k library from disk is guest memory, which goes after its expunge */
static IPTR thunk_dos_UnLoadSeg(struct KaffeineContext *ctx, void *cpu)
{
    if (!Kaffeine_IsM68KSegList(ctx, THUNK_D(1)))
        UnLoadSeg((BPTR)THUNK_D(1));
    return 0;
}

const struct M68KThunkEntry kaffeine_thunks_dos[] = {
    { 228, thunk_dos_AllocDosObject },
    KAFFEINE_DOS_FORMAT_OBJECT_THUNKS,
    KAFFEINE_DOS_DATA_THUNKS,
    KAFFEINE_DOS_CLI_THUNKS,
    KAFFEINE_DOS_LAUNCH_THUNKS,
    { 156, thunk_dos_UnLoadSeg },
    { 132, thunk_dos_IoErr },
    { 462, thunk_dos_SetIoErr },
    {  90, thunk_dos_UnLock },
    { 198, thunk_dos_Delay },
    { 126, thunk_dos_CurrentDir },
    KAFFEINE_DOS_READARGS_THUNKS,
    KAFFEINE_DOS_FILE_THUNKS,
    KAFFEINE_DOS_BASE_THUNKS,
    KAFFEINE_DOS_MATCH_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_dos_count =
    sizeof(kaffeine_thunks_dos) / sizeof(kaffeine_thunks_dos[0]) - 1;



/* ── IntuiText m68k→native translation ── */
static void translate_intuitext(struct KaffeineContext *ctx, ULONG m68k_it,
                                struct IntuiText *native, int max_chain)
{
    native->FrontPen  = ctx->mem[m68k_it + M68K_IT_FRONTPEN];
    native->BackPen   = ctx->mem[m68k_it + M68K_IT_BACKPEN];
    native->DrawMode  = ctx->mem[m68k_it + M68K_IT_DRAWMODE];
    native->LeftEdge  = (WORD)m68k_read16(ctx, m68k_it + M68K_IT_LEFTEDGE);
    native->TopEdge   = (WORD)m68k_read16(ctx, m68k_it + M68K_IT_TOPEDGE);
    native->ITextFont = NULL;
    ULONG m68k_text = m68k_read32(ctx, m68k_it + M68K_IT_ITEXT);
    native->IText     = m68k_text ? (STRPTR)m68k_to_host(ctx, m68k_text) : NULL;
    ULONG m68k_next = m68k_read32(ctx, m68k_it + M68K_IT_NEXTTEXT);
    if (m68k_next && max_chain > 1) {
        struct IntuiText *next = (struct IntuiText *)AllocMem(sizeof(struct IntuiText), MEMF_CLEAR);
        if (next) { translate_intuitext(ctx, m68k_next, next, max_chain - 1); native->NextText = next; }
    } else
        native->NextText = NULL;
}

static void free_intuitext_chain(struct IntuiText *it)
{
    struct IntuiText *n;
    for (n = it->NextText; n; ) { struct IntuiText *tmp = n->NextText; FreeMem(n, sizeof(struct IntuiText)); n = tmp; }
}

/* intuition -348: AutoRequest(A0=win, A1=body, A2=posText, A3=negText, D0-D3) */
static IPTR thunk_intuition_AutoRequest(struct KaffeineContext *ctx, void *cpu)
{
    struct Window *win = (struct Window *)shadow_lookup(ctx, THUNK_A(0));
    ULONG m_body = THUNK_A(1), m_pos = THUNK_A(2), m_neg = THUNK_A(3);
    struct IntuiText body, pos, neg;
    memset(&body, 0, sizeof(body)); memset(&pos, 0, sizeof(pos)); memset(&neg, 0, sizeof(neg));
    if (m_body) translate_intuitext(ctx, m_body, &body, 8);
    if (m_pos) translate_intuitext(ctx, m_pos, &pos, 1);
    if (m_neg) translate_intuitext(ctx, m_neg, &neg, 1);
    BOOL result = AutoRequest(win, m_body ? &body : NULL, m_pos ? &pos : NULL, m_neg ? &neg : NULL,
                              THUNK_D(0), THUNK_D(1), THUNK_D(2), THUNK_D(3));
    if (m_body) free_intuitext_chain(&body);
    return (IPTR)result;
}

/* ── Intuition manual thunks (shadow struct) ── */
const struct M68KThunkEntry kaffeine_thunks_intuition_manual[] = {
    { 348, thunk_intuition_AutoRequest },
    KAFFEINE_INTUITION_COMPAT_THUNKS,
    KAFFEINE_INTUITION_WINDOW_THUNKS,
    KAFFEINE_INTUITION_REQUEST_THUNKS,
    KAFFEINE_INTUITION_MENU_THUNKS,
    KAFFEINE_INTUITION_GADGET_THUNKS,
    KAFFEINE_INTUITION_PUBSCREEN_THUNKS,
    KAFFEINE_INTUITION_SCREEN_THUNKS,
    KAFFEINE_INTUITION_LAYER_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_intuition_manual_count =
    sizeof(kaffeine_thunks_intuition_manual) / sizeof(kaffeine_thunks_intuition_manual[0]) - 1;

/* ── GadTools manual thunks: IDCMP messages (kaffeine_window.c), menus
   (kaffeine_menus.c) and gadgets (kaffeine_gadgets.c) ── */
const struct M68KThunkEntry kaffeine_thunks_gadtools_manual[] = {
    KAFFEINE_GADTOOLS_IDCMP_THUNKS,
    KAFFEINE_GADTOOLS_MENU_THUNKS,
    KAFFEINE_GADTOOLS_GADGET_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_gadtools_manual_count =
    sizeof(kaffeine_thunks_gadtools_manual) / sizeof(kaffeine_thunks_gadtools_manual[0]) - 1;

/* graphics -192: LoadRGB4(A0=vp, A1=colors, D0=count) */
static IPTR thunk_gfx_LoadRGB4(struct KaffeineContext *ctx, void *cpu)
{
    struct ViewPort *vp = (struct ViewPort *)m68k_to_host_or_shadow(ctx, THUNK_A(0));
    ULONG m68k_colors = THUNK_A(1);
    LONG count = (LONG)THUNK_D(0);
    if (!vp || !m68k_colors || count <= 0) return 0;
    /* Byte-swap big-endian m68k UWORDs to native */
    UWORD *native_colors = (UWORD *)AllocMem(count * sizeof(UWORD), 0);
    if (!native_colors) return 0;
    for (LONG i = 0; i < count; i++)
        native_colors[i] = m68k_read16(ctx, m68k_colors + i * 2);
    LoadRGB4(vp, native_colors, count);
    FreeMem(native_colors, count * sizeof(UWORD));
    return 0;
}

/* ── Graphics manual thunks (shadow RastPort) ── */
const struct M68KThunkEntry kaffeine_thunks_graphics_manual[] = {
    KAFFEINE_GRAPHICS_COMPAT_THUNKS,
    KAFFEINE_GRAPHICS_RASTPORT_THUNKS,
    KAFFEINE_GRAPHICS_DISPLAY_THUNKS,
    KAFFEINE_GRAPHICS_LAYER_THUNKS,
    KAFFEINE_GRAPHICS_REGION_THUNKS,
    { 192, thunk_gfx_LoadRGB4 },
    { 0, NULL }
};
const ULONG kaffeine_thunks_graphics_manual_count =
    sizeof(kaffeine_thunks_graphics_manual) / sizeof(kaffeine_thunks_graphics_manual[0]) - 1;


/* Bitmap and native-lock-handle calls without a classic converter. */
const struct M68KThunkEntry kaffeine_thunks_cybergraphics_manual[] = {
    KAFFEINE_CYBERGRAPHICS_BITMAP_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_cybergraphics_manual_count =
    sizeof(kaffeine_thunks_cybergraphics_manual) / sizeof(kaffeine_thunks_cybergraphics_manual[0]) - 1;
