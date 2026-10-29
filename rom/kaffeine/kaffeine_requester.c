/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    EasyRequests of 68k code. The bridge formats the text and the gadget
    labels of a 68k EasyStruct with its 68k arguments itself, with the
    checked RawDoFmt() of kaffeine_format.c, cuts the labels as AROS does,
    and gives native Intuition the text and every label as native %s
    arguments, so that nothing of the 68k argument stream reaches native
    code. A requester window that the program gets back is a 68k Window,
    for it to pass to SysReqHandler() and FreeSysRequest().

    Classic requesters of Request() go on regular windows of the program
    only. Intuition never gets the 68k Requester: the bridge gives it a
    native Requester of its own, with native copies of the IntuiText,
    Border and Image chains (kaffeine_copy.c), which stay until
    EndRequest() or the window closes, as Intuition draws them again when
    the requester's layer needs it. Requester gadgets, PREDRAWN bitmaps,
    double-menu requesters and every flag but POINTREL, NOISYREQ,
    SIMPLEREQ, USEREQIMAGE and NOREQBACKFILL stop the program. The 68k
    Requesters of a window are linked through OlderRequest from
    Window.FirstRequest, newest first, as Intuition links its own, and
    ReqCount counts them; the bridge keeps that order in its records and
    never reads it back from guest memory, nor follows the native list,
    which may hold requesters that are not the program's. ReqLayer is the
    68k Layer that the layer bridge gives for the native requester layer
    (kaffeine_layers.c), through ctx->layer_ops, with a typed 68k RastPort
    of its own as rp; no native layer reaches guest memory. Without the
    layer bridge Request() gives FALSE before the native call, and when
    the bridge cannot make them the native requester ends again and the
    program gets FALSE: an active requester never has ReqLayer 0. Both go
    before the native EndRequest() deletes the layer. Request() and
    EndRequest() wait for Intuition's input handler, which locks the
    window's layers: while the program holds a layer lock of a window or
    screen they stop it instead.
*/

#include "kaffeine_intern.h"
#include "kaffeine_copy.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_format.h"
#include "kaffeine_layers.h"
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

/* The 68k Requester of the NDK, and the fields of the 68k Window that
   requesters use */
enum
{
    G_RQ_OLDERREQUEST = 0, G_RQ_LEFTEDGE = 4, G_RQ_TOPEDGE = 6, G_RQ_WIDTH = 8,
    G_RQ_HEIGHT = 10, G_RQ_RELLEFT = 12, G_RQ_RELTOP = 14, G_RQ_REQGADGET = 16,
    G_RQ_REQBORDER = 20, G_RQ_REQTEXT = 24, G_RQ_FLAGS = 28, G_RQ_BACKFILL = 30,
    G_RQ_REQLAYER = 32, G_RQ_RWINDOW = 72, G_RQ_REQIMAGE = 76,
    G_REQUESTER_SIZE = 112,
    G_WD_FIRSTREQUEST = 36, G_WD_DMREQUEST = 40, G_WD_REQCOUNT = 44
};

/* The flags of a Requester that the program may set for Request() */
#define REQ_INPUT_FLAGS (POINTREL | NOISYREQ | SIMPLEREQ | USEREQIMAGE | NOREQBACKFILL)
#define REQ_STATUS_FLAGS (REQOFFWINDOW | REQACTIVE | SYSREQUEST | DEFERREFRESH)

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

/* ── Requesters of Request() ─────────────────────────────────────── */

/* The record of an active 68k Requester, or -1 */
static int request_find(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_MAX_REQUESTS; i++)
        if (ctx->requests[i].guest == guest)
            return i;
    return -1;
}

/* Whether the 112 bytes at guest share a byte with an active Requester */
static BOOL request_overlaps(struct KaffeineContext *ctx, ULONG guest)
{
    ULONG other;
    int i;

    for (i = 0; i < KAFFEINE_MAX_REQUESTS; i++)
        if ((other = ctx->requests[i].guest) &&
            guest < other + G_REQUESTER_SIZE && other < guest + G_REQUESTER_SIZE)
            return TRUE;
    return FALSE;
}

/* The record of the same window that names record ri as its older one, or
   -1 */
static int request_newer(struct KaffeineContext *ctx, int ri)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_REQUESTS; i++)
        if (i != ri && ctx->requests[i].guest &&
            ctx->requests[i].window == ctx->requests[ri].window &&
            ctx->requests[i].older == ctx->requests[ri].guest)
            return i;
    return -1;
}

/* The newest active requester of a 68k Window, which no other one of it
   names as older; any of it should the links ever disagree, so that
   teardown still finds every one; -1 for none */
static int request_newest(struct KaffeineContext *ctx, ULONG window)
{
    int i, any = -1;

    for (i = 0; window && i < KAFFEINE_MAX_REQUESTS; i++)
        if (ctx->requests[i].guest && ctx->requests[i].window == window)
        {
            if (request_newer(ctx, i) < 0)
                return i;
            any = i;
        }
    return any;
}

/* Window.FirstRequest and Window.ReqCount of a 68k Window as the records
   have them */
static void request_publish(struct KaffeineContext *ctx, ULONG window)
{
    int i, newest = request_newest(ctx, window);
    UWORD count = 0;

    for (i = 0; i < KAFFEINE_MAX_REQUESTS; i++)
        if (ctx->requests[i].guest && ctx->requests[i].window == window)
            count++;
    if (!guest_range(ctx, window, G_WD_REQCOUNT + 2))
        return;
    m68k_write32(ctx, window + G_WD_FIRSTREQUEST, newest >= 0 ? ctx->requests[newest].guest : 0);
    m68k_write16(ctx, window + G_WD_REQCOUNT, count);
}

/* The native window of a regular 68k Window of the program; NULL for 0,
   for an EasyRequest window and for anything else */
static struct Window *request_window(struct KaffeineContext *ctx, ULONG guest)
{
    if (!guest || Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_REQUESTER_WINDOW))
        return NULL;
    return Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_WINDOW);
}

/* Whether the program holds a lock of a window or screen layer, which the
   input handler would wait for while the program waits for it */
static BOOL request_locked(struct KaffeineContext *ctx)
{
    return ctx->layer_ops && ctx->layer_ops->has_locks && ctx->layer_ops->has_locks(ctx);
}

/* One run of the copies of the texts, the borders and, for USEREQIMAGE,
   the image of the 68k Requester at guest; the second run fills them into
   req, the first gets NULL. */
static void request_copy(struct KaffeineContext *ctx, struct KaffeineCopy *c, ULONG guest,
                         UWORD flags, struct Requester *req)
{
    struct IntuiText *text = Kaffeine_CopyTexts(ctx, c, m68k_read32(ctx, guest + G_RQ_REQTEXT));
    struct Border *border = Kaffeine_CopyBorders(ctx, c, m68k_read32(ctx, guest + G_RQ_REQBORDER));
    struct Image *image = NULL;

    if (flags & USEREQIMAGE)
        image = Kaffeine_CopyImages(ctx, c, m68k_read32(ctx, guest + G_RQ_REQIMAGE));
    if (req)
    {
        req->ReqText = text;
        req->ReqBorder = border;
        req->ReqImage = image;
    }
}

/* The native side of record ri goes: the record leaves the table first,
   then the 68k Layer and RastPort of the requester layer, while the native
   layer still exists, then Intuition ends the native requester, then its
   copies are freed. The 68k Requester is not touched. */
static void request_end(struct KaffeineContext *ctx, int ri)
{
    struct Requester *req = ctx->requests[ri].native;
    struct Window *w = ctx->requests[ri].native_window;
    struct Layer *layer = ctx->requests[ri].native_layer;
    APTR block = ctx->requests[ri].block;
    int newer = request_newer(ctx, ri);

    if (newer >= 0)
        ctx->requests[newer].older = ctx->requests[ri].older;
    memset(&ctx->requests[ri], 0, sizeof(ctx->requests[ri]));
    /* nothing once the layer bridge has gone at the end */
    if (layer && ctx->layer_ops && ctx->layer_ops->request_closing)
        ctx->layer_ops->request_closing(ctx, layer);
    if (req && w)
        EndRequest(req, w);
    FreeVec(block);
    FreeVec(req);
}

/* Before a 68k Window goes, after its layer locks went back: every
   requester of the program there ends natively, newest first. The 68k
   Requesters may be freed by now and are never touched; the 68k Window
   still is the bridge's. */
static void requests_closing(struct KaffeineContext *ctx, ULONG window, struct Window *w)
{
    int ri;

    (void)w;
    while ((ri = request_newest(ctx, window)) >= 0)
        request_end(ctx, ri);
    request_publish(ctx, window);
}

/* InitRequester() initializes caller storage, not a native requester.
   The classic structure is 112 bytes; native pointers make the AArch64
   structure larger. Never pass this guest buffer to native InitRequester.
   Storage of a requester that is still active stops the program, as
   clearing it would unlink the bridge's records from guest memory. */
IPTR Kaffeine_InitRequester(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    if (!guest)
        return 0;
    if (!guest_range(ctx, guest, G_REQUESTER_SIZE))
        return refuse(ctx, "InitRequester(): requester outside guest memory");
    if (request_overlaps(ctx, guest))
        return refuse(ctx, "InitRequester(): a requester that is still active");
    memset(ctx->mem + guest, 0, G_REQUESTER_SIZE);
    return 0;
}

/* intuition -240: Request(A0=requester, A1=window) -> D0. NULL for either
   gives FALSE, as in AROS. Without room, memory, the layer bridge, a
   native requester or a 68k Layer and RastPort for its layer the program
   gets FALSE and nothing of the call stays; a size that leaves no room in
   the window is Intuition's to refuse. */
IPTR Kaffeine_Request(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), window = THUNK_A(1), guest_layer = 0;
    struct KaffeineCopy c;
    struct Requester *req;
    struct Window *w;
    struct Layer *layer;
    APTR block = NULL;
    UWORD flags;
    int ri, newest;

    if (!guest || !window)
        return FALSE;
    if (!(w = request_window(ctx, window)))
        return refuse(ctx, "Request(): not a regular window of the program");
    if (!guest_range(ctx, guest, G_REQUESTER_SIZE))
        return refuse(ctx, "Request(): requester outside guest memory");
    if (request_overlaps(ctx, guest))
        return refuse(ctx, "Request(): a requester that is already active");
    flags = m68k_read16(ctx, guest + G_RQ_FLAGS);
    if (flags & ~(REQ_INPUT_FLAGS | REQ_STATUS_FLAGS))
        return refuse(ctx, "Request(): requester flags that the bridge cannot take");
    flags &= REQ_INPUT_FLAGS;
    if (m68k_read32(ctx, guest + G_RQ_REQGADGET))
        return refuse(ctx, "Request(): requester gadgets");
    memset(&c, 0, sizeof(c));
    request_copy(ctx, &c, guest, flags, NULL);
    if (c.why)
    {
        bug("[kaffeine] Request(): %s\n", c.why);
        return refuse(ctx, "Request(): requester texts, borders or image that the bridge "
                           "cannot take");
    }
    if (request_locked(ctx))
        return refuse(ctx, "Request(): while holding a layer lock");
    for (ri = 0; ri < KAFFEINE_MAX_REQUESTS && ctx->requests[ri].guest; ri++)
        ;
    if (ri == KAFFEINE_MAX_REQUESTS)
    {
        bug("[kaffeine] Request(): too many active requesters\n");
        return FALSE;
    }
    /* ReqLayer needs a 68k Layer: no native requester without the bridge
       that makes it */
    if (!ctx->layer_ops || !ctx->layer_ops->request_opened || !ctx->layer_ops->request_closing)
    {
        bug("[kaffeine] Request(): no layer bridge for the requester's layer\n");
        return FALSE;
    }
    if (!(req = AllocVec(sizeof(*req), MEMF_ANY | MEMF_CLEAR)) ||
        (c.used && !(block = AllocVec(c.used, MEMF_ANY | MEMF_CLEAR))))
    {
        bug("[kaffeine] Request(): no memory for the requester\n");
        FreeVec(req);
        return FALSE;
    }
    /* Nothing to copy takes no block: every chain is empty then. */
    if (block)
    {
        Kaffeine_CopyFill(&c, block);
        request_copy(ctx, &c, guest, flags, req);
        if (c.why)
        {
            FreeVec(block);
            FreeVec(req);
            bug("[kaffeine] Request(): %s\n", c.why);
            return refuse(ctx, "Request(): requester texts, borders or image that the bridge "
                               "cannot take");
        }
    }
    req->LeftEdge = (WORD)m68k_read16(ctx, guest + G_RQ_LEFTEDGE);
    req->TopEdge = (WORD)m68k_read16(ctx, guest + G_RQ_TOPEDGE);
    req->Width = (WORD)m68k_read16(ctx, guest + G_RQ_WIDTH);
    req->Height = (WORD)m68k_read16(ctx, guest + G_RQ_HEIGHT);
    req->RelLeft = (WORD)m68k_read16(ctx, guest + G_RQ_RELLEFT);
    req->RelTop = (WORD)m68k_read16(ctx, guest + G_RQ_RELTOP);
    req->Flags = flags;
    req->BackFill = ctx->mem[guest + G_RQ_BACKFILL];
    if (!Request(req, w))
    {
        FreeVec(block);
        FreeVec(req);
        return FALSE;
    }
    /* The layer that Intuition made gets its 68k Layer and RastPort, or
       the requester ends again, before anything of it is published. */
    layer = req->ReqLayer;
    if (!layer || !(guest_layer = ctx->layer_ops->request_opened(ctx, w, layer)))
    {
        bug("[kaffeine] Request(): no 68k Layer for the requester's layer\n");
        EndRequest(req, w);
        FreeVec(block);
        FreeVec(req);
        return FALSE;
    }
    newest = request_newest(ctx, window);
    ctx->requests[ri].guest = guest;
    ctx->requests[ri].window = window;
    ctx->requests[ri].older = newest >= 0 ? ctx->requests[newest].guest : 0;
    ctx->requests[ri].layer = guest_layer;
    ctx->requests[ri].native_window = w;
    ctx->requests[ri].native = req;
    ctx->requests[ri].native_layer = layer;
    ctx->requests[ri].block = block;
    ctx->requests_closing = requests_closing;
    /* the place that Intuition gave it, inside the window's borders */
    m68k_write32(ctx, guest + G_RQ_OLDERREQUEST, ctx->requests[ri].older);
    m68k_write16(ctx, guest + G_RQ_LEFTEDGE, (UWORD)req->LeftEdge);
    m68k_write16(ctx, guest + G_RQ_TOPEDGE, (UWORD)req->TopEdge);
    m68k_write16(ctx, guest + G_RQ_FLAGS, flags | REQACTIVE);
    m68k_write32(ctx, guest + G_RQ_REQLAYER, guest_layer);
    m68k_write32(ctx, guest + G_RQ_RWINDOW, window);
    request_publish(ctx, window);
    return TRUE;
}

/* intuition -120: EndRequest(A0=requester, A1=window). Other requesters
   of the window stay, in their order. A requester that is not active on
   that window, NULL included, does nothing, without a native call. */
IPTR Kaffeine_EndRequest(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0), window = THUNK_A(1), older, newer_guest = 0;
    int ri = request_find(ctx, guest), newer;

    if (ri < 0 || ctx->requests[ri].window != window ||
        request_window(ctx, window) != ctx->requests[ri].native_window)
    {
        if (guest)
            bug("[kaffeine] EndRequest(%08lx, %08lx): not an active requester of the window\n",
                (unsigned long)guest, (unsigned long)window);
        return 0;
    }
    if (request_locked(ctx))
        return refuse(ctx, "EndRequest(): while holding a layer lock");
    older = ctx->requests[ri].older;
    if ((newer = request_newer(ctx, ri)) >= 0)
        newer_guest = ctx->requests[newer].guest;
    request_end(ctx, ri);
    /* the newer one names the older one, as Intuition unlinks it */
    if (newer_guest && guest_range(ctx, newer_guest, G_REQUESTER_SIZE))
        m68k_write32(ctx, newer_guest + G_RQ_OLDERREQUEST, older);
    if (guest_range(ctx, guest, G_REQUESTER_SIZE))
    {
        m68k_write32(ctx, guest + G_RQ_OLDERREQUEST, 0);
        m68k_write16(ctx, guest + G_RQ_FLAGS,
                     m68k_read16(ctx, guest + G_RQ_FLAGS) & ~(UWORD)REQACTIVE);
        m68k_write32(ctx, guest + G_RQ_REQLAYER, 0);
    }
    request_publish(ctx, window);
    return 0;
}

/* Only an already clear double-menu requester is supported. The bridge
   never installs one. AROS SetDMRequest rejects NULL before it reaches its
   clearing branch, so don't make that native call for this no-op. */
IPTR Kaffeine_ClearDMRequest(struct KaffeineContext *ctx, void *cpu)
{
    ULONG window = THUNK_A(0);
    struct Window *w = request_window(ctx, window);
    if (!w)
        return refuse(ctx, "ClearDMRequest(): not a regular window of the program");
    if (w->DMRequest)
        return refuse(ctx, "ClearDMRequest(): an unmanaged double-menu requester");
    m68k_write32(ctx, window + G_WD_DMREQUEST, 0);
    return TRUE;
}

IPTR Kaffeine_SetDMRequest(struct KaffeineContext *ctx, void *cpu)
{
    if (THUNK_A(1))
        return refuse(ctx, "SetDMRequest(): double-menu requesters");
    return Kaffeine_ClearDMRequest(ctx, cpu);
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
        if (request_locked(ctx))
        {
            refuse(ctx, "BuildEasyRequestArgs(): while holding a layer lock");
            ok = 0;
        }
        else
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
        Kaffeine_WindowPrepareClose(ctx, guest);
        if (request_locked(ctx))
            return refuse(ctx, "FreeSysRequest(): while holding a layer lock");
        if (ctx->requests_closing)
            ctx->requests_closing(ctx, guest, req);
        if (ctx->menu_closing)
            ctx->menu_closing(ctx, req);
        if (ctx->window_gadgets)
            ctx->window_gadgets->closing(ctx, req);
        Kaffeine_WindowDeletePrepared(ctx, guest);
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
