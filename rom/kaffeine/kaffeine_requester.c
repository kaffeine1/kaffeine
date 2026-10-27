/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    EasyRequests of 68k code. The bridge formats the text and the gadget
    labels of a 68k EasyStruct with its 68k arguments itself, with the
    checked RawDoFmt() of kaffeine_format.c, cuts the labels as AROS does,
    and gives native Intuition the text and every label as native %s
    arguments, so that nothing of the 68k argument stream reaches native
    code. A requester window that the program gets back is a 68k Window,
    for it to pass to SysReqHandler() and FreeSysRequest().
*/

#include "kaffeine_intern.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_format.h"
#include "kaffeine_rastport.h"
#include "kaffeine_window.h"

#include <aros/debug.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>

/* The 68k EasyStruct of the NDK */
enum
{
    G_ES_FLAGS = 4, G_ES_TITLE = 8, G_ES_TEXTFORMAT = 12, G_ES_GADGETFORMAT = 16,
    G_EASYSTRUCT_SIZE = 20
};

/* The most text, or labels, that one requester takes */
#define TEXT_MAX 0x100000

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* Stop the program with return code 20. The thunk returns it, so that it
   reaches D0, from which the CPU loop takes the exit code. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* A formatted 68k template, every character with the final NUL included,
   as AROS formats it into a buffer of its own. */
struct Formatted
{
    char *text;
    ULONG size, used;
};

static BOOL formatted_put(void *state, UBYTE ch)
{
    struct Formatted *out = state;

    if (out->used >= out->size)
        return FALSE;
    out->text[out->used++] = ch;
    return TRUE;
}

/* Format a 68k template with its 68k data stream: 1, with the place of the
   next data in *next; 0 for 68k input that the bridge refuses; -1 without
   the memory for it. */
static int format(struct KaffeineContext *ctx, ULONG fmt, ULONG args, struct Formatted *out,
                  ULONG *next)
{
    out->text = NULL;
    out->used = 0;
    if (!Kaffeine_FormatCount(ctx, fmt, args, &out->size, next))
        return 0;
    if (out->size > TEXT_MAX || !(out->text = AllocVec(out->size, MEMF_ANY)))
        return -1;
    if (!Kaffeine_FormatToPutNext(ctx, fmt, args, formatted_put, out, next) ||
        out->used != out->size)
    {
        FreeVec(out->text);
        out->text = NULL;
        return 0;
    }
    return 1;
}

/* The native window for a 68k one: 0 and 1 as they are, as Intuition takes
   them from its requester calls; FALSE for anything but a 68k Window. */
static BOOL native_window(struct KaffeineContext *ctx, ULONG guest, struct Window **w)
{
    if (guest <= 1)
    {
        *w = (struct Window *)(IPTR)guest;
        return TRUE;
    }
    *w = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_WINDOW);
    return *w != NULL;
}

static BOOL native_requester(struct KaffeineContext *ctx, ULONG guest, struct Window **w)
{
    if (guest <= 1)
    {
        *w = (struct Window *)(IPTR)guest;
        return TRUE;
    }
    *w = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_REQUESTER_WINDOW);
    return *w != NULL;
}

/* Build the requester of a 68k EasyStruct natively. Returns the native
   window, NULL when it cannot be made, also without memory for the text,
   or -1 for 68k input that the bridge refuses.

   The bridge formats the text and the gadget labels with the 68k arguments
   and cuts the labels as AROS does: one gadget more than the '|' in the
   gadget template, each label up to the next '|' of the formatted labels,
   past a NUL that an argument put there, the last one up to the end. Native
   Intuition gets the text and the labels as the arguments of "%s" and
   "%s|...|%s", one native pointer each, so that it makes the same gadgets
   with the same text. */
static struct Window *build(struct KaffeineContext *ctx, ULONG window, ULONG easy,
                            ULONG idcmp, ULONG args)
{
    struct Window *ref, *req = NULL;
    struct EasyStruct es;
    struct Formatted text, labels = { NULL, 0, 0 };
    ULONG title, labelfmt, next, count = 1, pos, i;
    char *gadgets = NULL;
    IPTR *argv = NULL;
    int ok;

    if (!native_window(ctx, window, &ref) || ref == (struct Window *)1 ||
        !guest_range(ctx, easy, G_EASYSTRUCT_SIZE))
        return (struct Window *)-1;
    title = m68k_read32(ctx, easy + G_ES_TITLE);
    if (title && (!guest_range(ctx, title, 1) ||
                  !memchr(ctx->mem + title, 0, ctx->mem_size - title)))
        return (struct Window *)-1;
    labelfmt = m68k_read32(ctx, easy + G_ES_GADGETFORMAT);
    ok = format(ctx, m68k_read32(ctx, easy + G_ES_TEXTFORMAT), args, &text, &next);
    if (ok > 0)
        ok = format(ctx, labelfmt, next, &labels, &next);
    if (ok > 0)
    {
        /* The template is a checked guest string now. */
        for (i = 0; ctx->mem[labelfmt + i]; i++)
            if (ctx->mem[labelfmt + i] == '|')
                count++;
        gadgets = AllocVec(3 * count, MEMF_ANY);
        argv = AllocVec((count + 1) * sizeof(IPTR), MEMF_ANY);
        if (!gadgets || !argv)
            ok = -1;
    }
    if (ok > 0)
    {
        argv[0] = (IPTR)text.text;
        for (i = 0, pos = 0; i < count; i++)
        {
            argv[i + 1] = (IPTR)(pos < labels.size ? labels.text + pos : "");
            if (i < count - 1)
            {
                while (pos < labels.size && labels.text[pos] != '|')
                    pos++;
                if (pos < labels.size)
                    labels.text[pos] = 0;
                pos++;
            }
            memcpy(gadgets + 3 * i, "%s|", 3);
        }
        gadgets[3 * count - 1] = 0;
        es.es_StructSize = sizeof(es);
        es.es_Flags = m68k_read32(ctx, easy + G_ES_FLAGS);
        es.es_Title = title ? (CONST_STRPTR)ctx->mem + title : NULL;
        es.es_TextFormat = "%s";
        es.es_GadgetFormat = gadgets;
        /* Intuition keeps copies of the text and the labels. */
        req = BuildEasyRequestArgs(ref, &es, idcmp, (RAWARG)argv);
    }
    else if (ok < 0)
        bug("[kaffeine] EasyRequest: no memory for the text and the gadgets\n");
    FreeVec(argv);
    FreeVec(gadgets);
    FreeVec(labels.text);
    FreeVec(text.text);
    return ok == 0 ? (struct Window *)-1 : req;
}

/* intuition -594: BuildEasyRequestArgs(A0=window, A1=easyStruct, D0=idcmp,
   A3=args) -> D0: the requester as a 68k Window, or 0. */
IPTR Kaffeine_BuildEasyRequestArgs(struct KaffeineContext *ctx, void *cpu)
{
    struct Window *req = build(ctx, THUNK_A(0), THUNK_A(1), THUNK_D(0), THUNK_A(3));
    ULONG guest;

    if (req == (struct Window *)-1)
        return refuse(ctx, "BuildEasyRequestArgs(): a window, EasyStruct, text or argument "
                           "that the bridge cannot take");
    if (!req || req == (struct Window *)1)
        return (IPTR)req;
    guest = Kaffeine_WindowCreate(ctx, req);
    if (guest && !Kaffeine_ShadowAdd(ctx, guest, req, KAFFEINE_SHADOW_REQUESTER_WINDOW))
    {
        Kaffeine_WindowDelete(ctx, guest);
        guest = 0;
    }
    if (!guest)
        FreeSysRequest(req);
    return guest;
}

/* intuition -600: SysReqHandler(A0=window, A1=idcmpPtr, D0=waitInput) -> D0 */
IPTR Kaffeine_SysReqHandler(struct KaffeineContext *ctx, void *cpu)
{
    ULONG ptr = THUNK_A(1), flags = 0;
    struct Window *req;
    LONG result;

    if (!native_requester(ctx, THUNK_A(0), &req))
        return refuse(ctx, "SysReqHandler(): not a requester of the program");
    if (ptr && !guest_range(ctx, ptr, 4))
        return refuse(ctx, "SysReqHandler(): the IDCMP flags are not in guest memory");
    if (ptr)
        flags = m68k_read32(ctx, ptr);
    /* waitInput is a BOOL, a WORD in D0, as AROS declares it */
    result = SysReqHandler(req, ptr ? &flags : NULL, (UWORD)THUNK_D(0) ? TRUE : FALSE);
    if (ptr)
        m68k_write32(ctx, ptr, flags);
    return (IPTR)result;
}

/* intuition -372: FreeSysRequest(A0=window) */
IPTR Kaffeine_FreeSysRequest(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    struct Window *req;

    if (!native_requester(ctx, guest, &req))
    {
        bug("[kaffeine] FreeSysRequest(%08lx): not a requester of the program\n",
            (unsigned long)guest);
        return 0;
    }
    if (guest > 1)
    {
        if (ctx->menu_closing)
            ctx->menu_closing(ctx, req);
        if (ctx->window_gadgets)
            ctx->window_gadgets->closing(ctx, req);
        Kaffeine_WindowDelete(ctx, guest);
    }
    FreeSysRequest(req);
    return 0;
}

/* intuition -588: EasyRequestArgs(A0=window, A1=easyStruct, A2=idcmpPtr,
   A3=args) -> D0. As AROS: the requester, handled until it gives a result,
   then freed. Its window never reaches the program. */
IPTR Kaffeine_EasyRequestArgs(struct KaffeineContext *ctx, void *cpu)
{
    ULONG ptr = THUNK_A(2), flags = 0;
    struct Window *req;
    LONG result;

    if (ptr && !guest_range(ctx, ptr, 4))
        return refuse(ctx, "EasyRequestArgs(): the IDCMP flags are not in guest memory");
    if (ptr)
        flags = m68k_read32(ctx, ptr);
    req = build(ctx, THUNK_A(0), THUNK_A(1), flags, THUNK_A(3));
    if (req == (struct Window *)-1)
        return refuse(ctx, "EasyRequestArgs(): a window, EasyStruct, text or argument "
                           "that the bridge cannot take");
    while ((result = SysReqHandler(req, ptr ? &flags : NULL, TRUE)) == -2)
        ;
    FreeSysRequest(req);
    if (ptr)
        m68k_write32(ctx, ptr, flags);
    return (IPTR)result;
}
