/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    asl.library of 68k code: the file requester as a program such as
    AmiPixelShop uses it for Open and Save As. AllocAslRequest() of an
    ASL_FileRequest gives a 68k FileRequester of the bridge, whose native
    request never leaves it; AslRequest() runs that request once; and
    FreeAslRequest() ends it. Every other vector stops the program.

    Only the root task of the program may call them, on a native task
    that is a DOS Process (AslRequest() locks and creates drawers in the
    calling Process), with no callback, broker or LayerInfo scope in
    progress. Tags are taken by their numbers, which the obsolete ASL_
    names share: the scalars, the strings and the Screen listed in
    classify(); the parent Window, hooks, multiple selection, patterns,
    sorting, fonts, locales and every other number stop the program
    before anything native runs.

    Native ASL keeps the Window, Screen, PubScreenName, TitleText,
    PositiveText and NegativeText it is given by pointer, from one call to
    the next, and ignores the four strings when they are NULL. So the bridge
    gives each call its own copies of those strings, keeps every copy until
    FreeAslRequest(), sixteen at most for a requester, and ends each tag
    list with Window NULL and the Screen: NULL in AllocAslRequest(), and in
    AslRequest() the native screen of a 68k Screen that the program opened
    or holds locked, checked again at that moment. Initial strings and
    scalars go to native ASL in the order given, call by call, as native
    ASL clones or stores them; nothing of an earlier call is replayed.

    A requester runs one AslRequest() only: native ASL keeps private string
    pointers from one request for the next (filereqhooks.c frees fr_Pattern
    without its private copy), so a second request on the same requester
    stops the program, whatever the first returned; AmiPixelShop makes one
    requester for each dialog. After AllocAslRequest() and after the
    request, whatever it returned, the 68k FileRequester is a copy of the
    public fields of the native one, as native ASL changes the strings,
    UserData and geometry also when the user cancels: its strings in the
    mirror's own buffers, a NULL one as 0, never a native pointer.

    Within the one request, native ASL itself frees fr_File and fr_Drawer
    before it clones the new names; if that clone fails for lack of memory
    the requester stays open with stale private pointers that its Restore
    menu item would read. That is native ASL under memory exhaustion, which
    the bridge cannot change.
*/

#include <aros/debug.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <libraries/asl.h>
#include <utility/tagitem.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_asl.h"
#include "kaffeine_layers.h"
#include "kaffeine_rastport.h"
#include "kaffeine_sched.h"

/* The 68k FileRequester (libraries/asl.h), 56 bytes */
#define G_FR_FILE      4
#define G_FR_DRAWER    8
#define G_FR_LEFTEDGE  22
#define G_FR_TOPEDGE   24
#define G_FR_WIDTH     26
#define G_FR_HEIGHT    28
#define G_FR_NUMARGS   32
#define G_FR_ARGLIST   36
#define G_FR_USERDATA  40
#define G_FR_PATTERN   52
/* the string buffers that follow it in the mirror */
#define G_BUF_FILE     56
#define G_BUF_DRAWER   (G_BUF_FILE + KAFFEINE_ASL_FILE_BYTES)
#define G_BUF_PATTERN  (G_BUF_DRAWER + KAFFEINE_ASL_DRAWER_BYTES)

#define ASL_TAG_STEPS   64      /* tag items, TAG_MORE and TAG_SKIP included */
#define ASL_TEXT_MAX    256     /* characters of a title, button or screen name */
#define ASL_COMMON      4       /* TitleText, PositiveText, NegativeText, PubScreenName */

static const ULONG common_tags[ASL_COMMON] = {
    ASLFR_TitleText, ASLFR_PositiveText, ASLFR_NegativeText, ASLFR_PubScreenName
};

/* One call's tags as native ASL gets them, built before anything native */
struct Staging
{
    struct TagItem out[ASL_TAG_STEPS + ASL_COMMON + 3];
    ULONG count;
    ULONG common[ASL_COMMON];   /* last non-NULL guest string of each, or 0 */
    BOOL screen_given;
    ULONG screen;               /* the last Screen tag */
};

static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* The root task of the program, on the native task that runs it, from
   trusted state only, as kaffeine_exec_io.c's root_caller(); that task a
   DOS Process, as native ASL needs one; and no callback, DOS broker or
   LayerInfo scope in progress. */
static BOOL root_caller(struct KaffeineContext *ctx, void *cpu)
{
    struct Task *self = FindTask(NULL);

    if (!ctx->running || !ctx->exec_task || self != ctx->exec_task ||
        self->tc_Node.ln_Type != NT_PROCESS ||
        ctx->cpu != (struct KaffeineCPU *)cpu || ctx->callback_depth ||
        ctx->broker_call || ctx->broker_owner || Kaffeine_LayerInfoLocked(ctx))
        return FALSE;
    return !ctx->sched || (ctx->task_ops && ctx->task_ops->is_root &&
                           ctx->task_ops->is_root(ctx, cpu));
}

/* A NUL-terminated guest string of at most max characters */
static BOOL guest_text(struct KaffeineContext *ctx, ULONG addr, ULONG max)
{
    ULONG n;

    if (!addr || addr >= ctx->mem_size)
        return FALSE;
    for (n = 0; n <= max && addr + n < ctx->mem_size; n++)
        if (!ctx->mem[addr + n])
            return TRUE;
    return FALSE;
}

/* The native screen of a 68k Screen that the program opened and has not
   closed, or holds by LockPubScreen(): the exact entry of the screen table
   and its typed shadow, never the guest bytes; NULL for anything else. */
static struct Screen *held_screen(struct KaffeineContext *ctx, ULONG guest)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_MAX_PUBSCREENS; i++)
    {
        struct Screen *native = ctx->pubscreens[i].native;

        if (ctx->pubscreens[i].guest != guest || !native)
            continue;
        if (ctx->pubscreens[i].owned)
            return Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_SCREEN) == native ?
                   native : NULL;
        if (ctx->pubscreens[i].locks && Kaffeine_ScreenNative(ctx, guest) == native)
            return native;
        return NULL;
    }
    return NULL;
}

static void forward(struct Staging *st, ULONG tag, IPTR data)
{
    st->out[st->count].ti_Tag = tag;
    st->out[st->count].ti_Data = data;
    st->count++;
}

/* One tag item: TRUE when it is taken, FALSE after stopping the program */
static BOOL classify(struct KaffeineContext *ctx, struct Staging *st, ULONG tag, ULONG data,
                     const char **why)
{
    unsigned i;

    for (i = 0; i < ASL_COMMON; i++)
        if (tag == common_tags[i])
        {
            /* native ASL ignores NULL and keeps the last other one */
            if (data && !guest_text(ctx, data, ASL_TEXT_MAX))
                return *why = "asl.library: a title, button text or screen name that is no "
                              "string of at most 256 characters", FALSE;
            if (data)
                st->common[i] = data;
            return TRUE;
        }
    switch (tag)
    {
        case ASLFR_InitialFile:
        case ASLFR_InitialDrawer:
            /* cloned by native ASL during the call; NULL is "" there */
            if (data && !guest_text(ctx, data, tag == ASLFR_InitialDrawer ?
                                    KAFFEINE_ASL_DRAWER_BYTES - 1 : KAFFEINE_ASL_FILE_BYTES - 1))
                return *why = "asl.library: an initial file or drawer that is no string "
                              "that fits its buffer", FALSE;
            forward(st, tag, data ? (IPTR)(ctx->mem + data) : 0);
            return TRUE;
        case ASLFR_InitialLeftEdge:
        case ASLFR_InitialTopEdge:
        case ASLFR_InitialWidth:
        case ASLFR_InitialHeight:
        case ASLFR_DoSaveMode:
        case ASLFR_DrawersOnly:
        case ASLFR_RejectIcons:
        case ASLFR_FilterDrawers:
        case ASLFR_InitialShowVolumes:
        case ASLFR_PopToFront:
        case ASLFR_Activate:
        case ASLFR_PrivateIDCMP:
        case ASLFR_UserData:
            /* scalars, as native ASL stores them; UserData is opaque */
            forward(st, tag, (IPTR)data);
            return TRUE;
        case ASLFR_DoMultiSelect:
        case ASLFR_DoPatterns:
        case ASLFR_Locale:
        case ASLFR_TextAttr:
            if (data)
                return *why = "asl.library: multiple selection, patterns, a Locale or a "
                              "TextAttr", FALSE;
            forward(st, tag, 0);
            return TRUE;
        case ASLFR_Screen:
            /* the bridge's own, checked when the list is complete */
            st->screen_given = TRUE;
            st->screen = data;
            return TRUE;
        default:
            return *why = "asl.library: a tag that the file requester bridge does not take", FALSE;
    }
}

/* A guest tag list into st, item by item, TAG_IGNORE, TAG_MORE and
   TAG_SKIP as NextTagItem() takes them; FALSE after stopping the program. */
static BOOL stage(struct KaffeineContext *ctx, ULONG list, struct Staging *st)
{
    ULONG p = list, steps, tag, data;
    const char *why = NULL;

    memset(st, 0, sizeof(*st));
    for (steps = 0; p; steps++)
    {
        if (steps == ASL_TAG_STEPS)
            return refuse(ctx, "asl.library: a tag list too long or in a cycle"), FALSE;
        if ((p & 1) || p >= ctx->mem_size || ctx->mem_size - p < 8)
            return refuse(ctx, "asl.library: a tag item outside guest memory"), FALSE;
        tag = m68k_read32(ctx, p);
        data = m68k_read32(ctx, p + 4);
        if (tag == TAG_DONE)
            break;
        if (tag == TAG_IGNORE)
            p += 8;
        else if (tag == TAG_MORE)
            p = data;
        else if (tag == TAG_SKIP)
        {
            if (data > (ctx->mem_size - p) / 8 - 1)
                return refuse(ctx, "asl.library: TAG_SKIP past guest memory"), FALSE;
            p += (data + 1) * 8;
        }
        else if (!classify(ctx, st, tag, data, &why))
            return refuse(ctx, why), FALSE;
        else
            p += 8;
    }
    return TRUE;
}

/* The native copies of the common strings of st, with *count of them, or
   FALSE without memory, holding none. */
static BOOL copy_common(struct KaffeineContext *ctx, struct Staging *st, char **copies, UWORD *count)
{
    unsigned i;

    *count = 0;
    for (i = 0; i < ASL_COMMON; i++)
    {
        ULONG len;

        if (!st->common[i])
            continue;
        len = strlen((const char *)ctx->mem + st->common[i]);
        if (!(copies[*count] = AllocVec(len + 1, MEMF_ANY)))
        {
            while (*count)
                FreeVec(copies[--*count]);
            return FALSE;
        }
        memcpy(copies[*count], ctx->mem + st->common[i], len + 1);
        forward(st, common_tags[i], (IPTR)copies[*count]);
        (*count)++;
    }
    return TRUE;
}

/* A native string into its mirror buffer and the 68k field that names it:
   0 for NULL; FALSE when it does not fit. */
static BOOL put_string(struct KaffeineContext *ctx, ULONG guest, ULONG field, ULONG buffer,
                       ULONG size, const char *s)
{
    ULONG len;

    if (!s)
    {
        m68k_write32(ctx, guest + field, 0);
        return TRUE;
    }
    len = strlen(s);
    if (len >= size)
        return FALSE;
    memcpy(ctx->mem + guest + buffer, s, len + 1);
    m68k_write32(ctx, guest + field, guest + buffer);
    return TRUE;
}

/* The public fields of the native request into its mirror; FALSE after
   stopping the program for a field that the mirror cannot show. */
static BOOL snapshot(struct KaffeineContext *ctx, ULONG guest, const struct FileRequester *fr)
{
    if (fr->fr_NumArgs || fr->fr_ArgList ||
        !put_string(ctx, guest, G_FR_FILE, G_BUF_FILE, KAFFEINE_ASL_FILE_BYTES, (const char *)fr->fr_File) ||
        !put_string(ctx, guest, G_FR_DRAWER, G_BUF_DRAWER, KAFFEINE_ASL_DRAWER_BYTES,
                    (const char *)fr->fr_Drawer) ||
        !put_string(ctx, guest, G_FR_PATTERN, G_BUF_PATTERN, KAFFEINE_ASL_PATTERN_BYTES,
                    (const char *)fr->fr_Pattern))
        return refuse(ctx, "asl.library: native results that the 68k FileRequester cannot show"),
               FALSE;
    m68k_write16(ctx, guest + G_FR_LEFTEDGE, (UWORD)fr->fr_LeftEdge);
    m68k_write16(ctx, guest + G_FR_TOPEDGE, (UWORD)fr->fr_TopEdge);
    m68k_write16(ctx, guest + G_FR_WIDTH, (UWORD)fr->fr_Width);
    m68k_write16(ctx, guest + G_FR_HEIGHT, (UWORD)fr->fr_Height);
    m68k_write32(ctx, guest + G_FR_NUMARGS, 0);
    m68k_write32(ctx, guest + G_FR_ARGLIST, 0);
    /* the bridge passed a 32-bit guest value */
    m68k_write32(ctx, guest + G_FR_USERDATA, (ULONG)(IPTR)fr->fr_UserData);
    return TRUE;
}

static struct KaffeineAslRequest *record_of(struct KaffeineContext *ctx, ULONG guest)
{
    unsigned i;

    for (i = 0; guest && i < KAFFEINE_MAX_ASL_REQUESTS; i++)
        if (ctx->asl_requests[i].guest == guest && ctx->asl_requests[i].native)
            return &ctx->asl_requests[i];
    return NULL;
}

static void forget(struct KaffeineContext *ctx, struct KaffeineAslRequest *r)
{
    struct KaffeineAslRequest gone = *r;
    UWORD i;

    memset(r, 0, sizeof(*r));
    FreeAslRequest(gone.native);
    for (i = 0; i < gone.retained; i++)
        FreeVec(gone.kept[i]);
    Kaffeine_MirrorRelease(ctx, gone.guest);
}

/* -48: AllocAslRequest(D0=reqType, A0=tagList) -> D0=requester */
static IPTR thunk_asl_AllocAslRequest(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineAslRequest *slot = NULL;
    struct Staging st;
    char *copies[ASL_COMMON];
    UWORD count, i;
    ULONG guest;
    APTR native;

    if (!root_caller(ctx, cpu))
        return refuse(ctx, "AllocAslRequest(): not the root task of the program");
    if (THUNK_D(0) != ASL_FileRequest)
        return refuse(ctx, "AllocAslRequest(): a font or screen mode requester");
    if (!stage(ctx, THUNK_A(0), &st))
        return 20;
    if (st.screen_given && st.screen && !held_screen(ctx, st.screen))
        return refuse(ctx, "AllocAslRequest(): a Screen that the program does not hold");
    for (i = 0; i < KAFFEINE_MAX_ASL_REQUESTS && !slot; i++)
        if (!ctx->asl_requests[i].guest && !ctx->asl_requests[i].native)
            slot = &ctx->asl_requests[i];
    if (!slot || !copy_common(ctx, &st, copies, &count))
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    if (!(guest = Kaffeine_MirrorAlloc(ctx, KAFFEINE_ASL_MIRROR_SIZE)))
        goto no_memory;
    /* no 68k Window, and no Screen between two calls of the program */
    forward(&st, ASLFR_Window, 0);
    forward(&st, ASLFR_Screen, 0);
    st.out[st.count].ti_Tag = TAG_DONE;
    if (!(native = AllocAslRequest(ASL_FileRequest, st.out)))
    {
        Kaffeine_MirrorAbort(ctx, guest);
        while (count)
            FreeVec(copies[--count]);
        return 0;
    }
    if (!snapshot(ctx, guest, native))
    {
        FreeAslRequest(native);
        Kaffeine_MirrorAbort(ctx, guest);
        while (count)
            FreeVec(copies[--count]);
        return 20;
    }
    slot->guest = guest;
    slot->native = native;
    slot->screen = st.screen_given ? st.screen : 0;
    slot->consumed = FALSE;
    slot->retained = count;
    for (i = 0; i < count; i++)
        slot->kept[i] = copies[i];
    return guest;
no_memory:
    while (count)
        FreeVec(copies[--count]);
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
}

/* -60: AslRequest(A0=requester, A1=tagList) -> D0=BOOL */
static IPTR thunk_asl_AslRequest(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineAslRequest *r;
    struct Staging st;
    char *copies[ASL_COMMON];
    struct Screen *screen = NULL;
    ULONG identity;
    UWORD count, i;
    BOOL result;

    if (!root_caller(ctx, cpu))
        return refuse(ctx, "AslRequest(): not the root task of the program");
    if (!(r = record_of(ctx, THUNK_A(0))))
        return refuse(ctx, "AslRequest(): not a file requester of the program");
    if (r->consumed)
        return refuse(ctx, "AslRequest(): a second request on one file requester");
    if (!stage(ctx, THUNK_A(1), &st))
        return 20;
    identity = st.screen_given ? st.screen : r->screen;
    if (identity && !(screen = held_screen(ctx, identity)))
        return refuse(ctx, "AslRequest(): a Screen that the program no longer holds");
    count = 0;
    for (i = 0; i < ASL_COMMON; i++)
        count += st.common[i] != 0;
    if (r->retained + count > KAFFEINE_ASL_RETAINED || !copy_common(ctx, &st, copies, &count))
    {
        SetIoErr(ERROR_NO_FREE_STORE);
        return FALSE;
    }
    /* from here the request is used and native ASL may keep the copies */
    for (i = 0; i < count; i++)
        r->kept[r->retained++] = copies[i];
    r->screen = identity;
    r->consumed = TRUE;
    forward(&st, ASLFR_Window, 0);
    forward(&st, ASLFR_Screen, (IPTR)screen);
    st.out[st.count].ti_Tag = TAG_DONE;
    result = AslRequest(r->native, st.out);
    if (!snapshot(ctx, r->guest, r->native))
        return 20;
    return result ? TRUE : FALSE;
}

/* -54: FreeAslRequest(A0=requester) */
static IPTR thunk_asl_FreeAslRequest(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineAslRequest *r;

    if (!root_caller(ctx, cpu))
        return refuse(ctx, "FreeAslRequest(): not the root task of the program");
    if (!THUNK_A(0))
        return 0;
    if (!(r = record_of(ctx, THUNK_A(0))))
        return refuse(ctx, "FreeAslRequest(): not a file requester of the program");
    forget(ctx, r);
    return 0;
}

/* The other vectors: a halt that names the call and its argument. */

/* -30: AllocFileRequest() */
static IPTR thunk_asl_AllocFileRequest(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "AllocFileRequest(): the old file requester calls are not bridged");
}

/* -36: FreeFileRequest(A0=fileReq) */
static IPTR thunk_asl_FreeFileRequest(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] FreeFileRequest(%08lx)\n", (unsigned long)THUNK_A(0));
    return refuse(ctx, "FreeFileRequest(): the old file requester calls are not bridged");
}

/* -42: RequestFile(A0=fileReq) */
static IPTR thunk_asl_RequestFile(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] RequestFile(%08lx)\n", (unsigned long)THUNK_A(0));
    return refuse(ctx, "RequestFile(): the old file requester calls are not bridged");
}

/* -78: AbortAslRequest(A0=requester) */
static IPTR thunk_asl_AbortAslRequest(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] AbortAslRequest(%08lx)\n", (unsigned long)THUNK_A(0));
    return refuse(ctx, "AbortAslRequest(): it would act on a request that another task runs");
}

/* -84: ActivateAslRequest(A0=requester) */
static IPTR thunk_asl_ActivateAslRequest(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] ActivateAslRequest(%08lx)\n", (unsigned long)THUNK_A(0));
    return refuse(ctx, "ActivateAslRequest(): it would act on a request that another task runs");
}

const struct M68KThunkEntry kaffeine_thunks_asl[] = {
    { 30, thunk_asl_AllocFileRequest },
    { 36, thunk_asl_FreeFileRequest },
    { 42, thunk_asl_RequestFile },
    { 48, thunk_asl_AllocAslRequest },
    { 54, thunk_asl_FreeAslRequest },
    { 60, thunk_asl_AslRequest },
    { 78, thunk_asl_AbortAslRequest },
    { 84, thunk_asl_ActivateAslRequest },
    { 0, NULL }
};
const ULONG kaffeine_thunks_asl_count =
    sizeof(kaffeine_thunks_asl) / sizeof(kaffeine_thunks_asl[0]) - 1;

void Kaffeine_AslCleanup(struct KaffeineContext *ctx)
{
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_ASL_REQUESTS; i++)
        if (ctx->asl_requests[i].native)
            forget(ctx, &ctx->asl_requests[i]);
}
