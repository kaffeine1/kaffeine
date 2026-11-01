/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    One ASL FileRequest on an owned 1024 by 768 truecolor screen, paired
    with the native WaitAslWindow observer and host keyboard input.
*/
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/asl.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <inline/macros.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "asl_probe_protocol.h"
#include "m68ktest.h"

#if !defined(__mc68000__) && !defined(__m68k__)
#error This probe requires the classic m68k AmigaOS compiler and NDK.
#endif

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *AslBase;
static struct Library *P96Base;

typedef char classic_fr_size[sizeof(struct FileRequester) == 56 ? 1 : -1];
typedef char classic_fr_file[offsetof(struct FileRequester, fr_File) == 4 ? 1 : -1];
typedef char classic_fr_drawer[offsetof(struct FileRequester, fr_Drawer) == 8 ? 1 : -1];
typedef char classic_fr_left[offsetof(struct FileRequester, fr_LeftEdge) == 22 ? 1 : -1];
typedef char classic_fr_top[offsetof(struct FileRequester, fr_TopEdge) == 24 ? 1 : -1];
typedef char classic_fr_width[offsetof(struct FileRequester, fr_Width) == 26 ? 1 : -1];
typedef char classic_fr_height[offsetof(struct FileRequester, fr_Height) == 28 ? 1 : -1];
typedef char classic_fr_numargs[offsetof(struct FileRequester, fr_NumArgs) == 32 ? 1 : -1];
typedef char classic_fr_arglist[offsetof(struct FileRequester, fr_ArgList) == 36 ? 1 : -1];
typedef char classic_fr_userdata[offsetof(struct FileRequester, fr_UserData) == 40 ? 1 : -1];
typedef char classic_fr_pattern[offsetof(struct FileRequester, fr_Pattern) == 52 ? 1 : -1];
typedef char classic_bool[sizeof(BOOL) == 2 ? 1 : -1];

/* Same NDK LP register declaration as the existing gfx_p96 target probe. */
#define p96BestModeIDTagList(tags) \
    LP1(0x3c, ULONG, p96BestModeIDTagList, struct TagItem *, tags, a0, , P96Base)
#define P96BIDTAG_NominalWidth (TAG_USER + 99)
#define P96BIDTAG_NominalHeight (TAG_USER + 100)
#define P96BIDTAG_Depth (TAG_USER + 101)

static BOOL fresh(const char *path)
{
    if (DeleteFile((CONST_STRPTR)path)) return TRUE;
    return IoErr() == ERROR_OBJECT_NOT_FOUND;
}

static BOOL file_text(const char *path, char *text, LONG size)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ), f;
    LONG got, more;
    char extra;
    if (!lock) return FALSE;
    f = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!f) { UnLock(lock); return FALSE; }
    got = Read(f, text, size - 1);
    more = got >= 0 ? Read(f, &extra, 1) : -1;
    Close(f); UnLock(lock);
    if (got < 0 || more != 0) return FALSE;
    text[got] = 0;
    return TRUE;
}

static BOOL exact(const char *name, const char *suffix, int n, const char *expected)
{
    char path[AP_PATH], text[512];
    return ap_path(path, name, suffix, n) && file_text(path, text, sizeof(text)) && !strcmp(text, expected);
}

/* A marker counts only if it is exact and its temporary was renamed away. */
static BOOL published(const char *name, const char *suffix, const char *temporary, int n)
{
    char path[AP_PATH];
    BPTR lock;
    if (!exact(name, suffix, n, "ready\n") || !ap_path(path, name, temporary, n)) return FALSE;
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock) { UnLock(lock); return FALSE; }
    return IoErr() == ERROR_OBJECT_NOT_FOUND;
}

static BOOL await_helper(const char *name, int n)
{
    char path[AP_PATH], text[512];
    LONG poll;
    if (!ap_path(path, name, "helper-done", n)) return FALSE;
    for (poll = 0; poll < AP_POLLS + 100; ++poll) {
        if (file_text(path, text, sizeof(text)) && !strcmp(text, "done\n")) return TRUE;
        Delay(AP_POLL_TICKS);
    }
    return FALSE;
}

/* This failed fixture must not close the screen or retire its titles while
   an observer may still be alive. The separately owned VM timeout ends it.
   This is never an accepted execution or a substitute for helper closure. */
static void retain_until_outer_timeout(void)
{
    PutStr("FAIL retaining screen until the outer owned VM timeout\n"); test_flush();
    for (;;) Delay(50);
}

static BOOL observation(const char *name, int n, LONG value[AP_VALUES])
{
    char path[AP_PATH], text[512], prefix[80], *p;
    int i;
    if (!ap_path(path, name, "observed", n) || !file_text(path, text, sizeof(text))) return FALSE;
    snprintf(prefix, sizeof(prefix), "visible %s %d ", name, n);
    if (strncmp(text, prefix, strlen(prefix))) return FALSE;
    p = text + strlen(prefix);
    for (i = 0; i < AP_VALUES; ++i) {
        LONG v = 0; int digits = 0, negative = *p == '-';
        if (negative) ++p;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 6) return FALSE;
            v = v * 10 + *p++ - '0';
        }
        if (!digits || *p != (i + 1 == AP_VALUES ? '\n' : ' ')) return FALSE;
        ++p; value[i] = negative ? -v : v;
    }
    return !*p && value[AP_SL] == 0 && value[AP_ST] == 0 &&
           value[AP_SW] == AP_SCREEN_W && value[AP_SH] == AP_SCREEN_H &&
           value[AP_WL] >= 0 && value[AP_WT] >= 0 && value[AP_WW] > 0 && value[AP_WH] > 0 &&
           value[AP_WW] <= AP_SCREEN_W - value[AP_WL] &&
           value[AP_WH] <= AP_SCREEN_H - value[AP_WT];
}

static BOOL ram_drawer(const char *drawer)
{
    BPTR a, b;
    BOOL okay;
    if (!drawer) return FALSE;
    a = Lock((CONST_STRPTR)drawer, ACCESS_READ);
    b = Lock((CONST_STRPTR)"RAM:", ACCESS_READ);
    okay = a && b && SameLock(a, b) == LOCK_SAME;
    if (a) UnLock(a);
    if (b) UnLock(b);
    return okay;
}

static BOOL record_result(const char *name, int n, BOOL result, const struct FileRequester *fr)
{
    char path[AP_PATH], text[2048];
    BPTR f;
    LONG length, wrote;
    BOOL closed;
    if (!ap_path(path, name, "result", n)) return FALSE;
    length = snprintf(text, sizeof(text),
        "result %s %d %ld %ld %ld %ld %ld %lu %ld %lu\nfile %s\ndrawer %s\n",
        name, n, (long)(result != FALSE), (long)fr->fr_LeftEdge, (long)fr->fr_TopEdge,
        (long)fr->fr_Width, (long)fr->fr_Height, (unsigned long)(ULONG)fr->fr_UserData,
        (long)fr->fr_NumArgs, (unsigned long)(ULONG)fr->fr_ArgList,
        fr->fr_File ? (const char *)fr->fr_File : "<NULL>",
        fr->fr_Drawer ? (const char *)fr->fr_Drawer : "<NULL>");
    if (length < 0 || (ULONG)length >= sizeof(text)) return FALSE;
    f = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!f) return FALSE;
    wrote = Write(f, text, length);
    closed = Close(f);
    return wrote == length && closed;
}

static BOOL drain_window(struct Window *window, BOOL draw)
{
    struct IntuiMessage *msg;
    BOOL refresh = FALSE;
    while ((msg = (struct IntuiMessage *)GetMsg(window->UserPort))) {
        ULONG cls = msg->Class;
        ReplyMsg((struct Message *)msg);
        if (cls == IDCMP_REFRESHWINDOW) {
            BeginRefresh(window);
            if (draw) { SetAPen(window->RPort, 1); RectFill(window->RPort, 12, 24, 100, 60); }
            EndRefresh(window, TRUE);
            refresh = TRUE;
        }
    }
    return refresh;
}

int main(int argc, char **argv)
{
    struct Screen *screen = NULL;
    struct Window *window = NULL;
    struct FileRequester *fr = NULL;
    struct TagItem mode_tags[] = {{P96BIDTAG_NominalWidth, AP_SCREEN_W},
        {P96BIDTAG_NominalHeight, AP_SCREEN_H}, {P96BIDTAG_Depth, AP_SCREEN_DEPTH}, {TAG_DONE, 0}};
    const char *name, *suffixes[] = {"keys", "mouse", "keys-tmp", "mouse-tmp", "observed", "closed", "result",
        "helper-out", "helper-err", "helper-rc", "helper-done"};
    char program[256], path[AP_PATH], command[256];
    char screen_title[AP_TITLE], window_title[AP_TITLE], request_title[AP_TITLE];
    char *base;
    BPTR lock;
    ULONG mode;
    LONG values[AP_VALUES], poll;
    int accept, n = 0, i;
    BOOL result, done = FALSE, helper_attempted = FALSE, refreshed = FALSE;

    name = argc == 2 && !strcmp(argv[1], "accept") ? "asl_accept" :
           argc == 2 && !strcmp(argv[1], "cancel") ? "asl_cancel" : NULL;
    if (!check(name != NULL, "explicit accept or cancel case")) return test_done("asl_file_probe");
    accept = ap_case(name);
    if (!check(GetProgramName((STRPTR)program, sizeof(program)), "program identity available")) goto out;
    base = program;
    for (i = 0; program[i]; ++i) if (program[i] == ':' || program[i] == '/') base = program + i + 1;
    if (!check(!strcmp(base, name), "fixed program name matches case")) goto out;
    snprintf(path, sizeof(path), "SYS:EMU68/%s-shell-rc.txt", name);
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock) { n = 2; UnLock(lock); }
    else { if (!check(IoErr() == ERROR_OBJECT_NOT_FOUND, "Shell or Run execution identity established")) goto out; n = 1; }
    /* One check in either execution, to keep the expected output identical. */
    if (n == 2) check(TRUE, "Shell or Run execution identity established");
    for (i = 0; i < (int)(sizeof(suffixes) / sizeof(suffixes[0])); ++i)
        if (!ap_path(path, name, suffixes[i], n) || !fresh(path)) break;
    if (!check(i == (int)(sizeof(suffixes) / sizeof(suffixes[0])), "exact execution witnesses are fresh")) goto out;
    if (!ap_title(screen_title, name, "screen", n) || !ap_title(window_title, name, "normal", n) ||
        !ap_title(request_title, name, "requester", n)) goto out;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 39);
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    if (!check(GfxBase && IntuitionBase && AslBase && P96Base, "required library bases opened")) goto out;
    mode = p96BestModeIDTagList(mode_tags);
    if (!check(mode != (ULONG)INVALID_ID, "truecolor mode available")) goto out;
    screen = OpenScreenTags(NULL, SA_DisplayID, mode, SA_Left, 0, SA_Top, 0,
        SA_Width, AP_SCREEN_W, SA_Height, AP_SCREEN_H, SA_Depth, AP_SCREEN_DEPTH,
        SA_Type, CUSTOMSCREEN, SA_Title, (ULONG)screen_title, TAG_DONE);
    if (!check(screen && screen->Width == AP_SCREEN_W && screen->Height == AP_SCREEN_H &&
        GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH) == AP_SCREEN_DEPTH,
        "owned 1024 by 768 screen reports depth 24")) goto out;
    window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen, WA_Left, 12, WA_Top, 18,
        WA_Width, 250, WA_Height, 110, WA_Title, (ULONG)window_title,
        WA_Flags, WFLG_DRAGBAR | WFLG_SIMPLE_REFRESH | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_REFRESHWINDOW | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW, TAG_DONE);
    if (!check(window && window->WScreen == screen && window->UserPort, "ordinary owned-screen window opened")) goto out;
    SetAPen(window->RPort, 1); RectFill(window->RPort, 12, 24, 100, 60);
    drain_window(window, TRUE);
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Screen, (ULONG)screen,
        ASLFR_TitleText, (ULONG)request_title, ASLFR_DoSaveMode, TRUE,
        ASLFR_RejectIcons, TRUE, ASLFR_InitialDrawer, (ULONG)"RAM:",
        ASLFR_InitialFile, (ULONG)"probe.txt", ASLFR_InitialWidth, AP_REQUEST_W,
        ASLFR_InitialHeight, AP_REQUEST_H, ASLFR_InitialLeftEdge, AP_REQUEST_LEFT,
        ASLFR_InitialTopEdge, AP_REQUEST_TOP, ASLFR_UserData, AP_USERDATA, TAG_DONE);
    if (!check(fr != NULL, "typed FileRequester allocated")) goto out;
    if (!check(fr->fr_File && !strcmp(fr->fr_File, "probe.txt") && fr->fr_Drawer &&
        !strcmp(fr->fr_Drawer, "RAM:"), "initial native strings mirrored")) goto out;
    if (!check(fr->fr_NumArgs == 0 && !fr->fr_ArgList && (ULONG)fr->fr_UserData == AP_USERDATA,
        "initial scalar outputs are opaque and bounded")) goto out;
    snprintf(command, sizeof(command), "Run >NIL: <NIL: Execute SYS:EMU68/%s-helper-%d", name, n);
    helper_attempted = TRUE;
    if (!check(SystemTagList((CONST_STRPTR)command, NULL) == 0, "native helper script launched")) goto out;
    result = AslRequestTags(fr, TAG_DONE); /* The only Request on this handle. */
    done = await_helper(name, n);
    if (!check(done, "native helper completed before screen retirement")) retain_until_outer_timeout();
    check(exact(name, "helper-rc", n, "rc 0\n"), "actual native helper RC is zero");
    check(exact(name, "helper-err", n, ""), "native helper stderr is empty");
    check(published(name, "mouse", "mouse-tmp", n), "fresh exact screenshot marker exists");
    check(published(name, "keys", "keys-tmp", n), "fresh exact key marker exists");
    check(exact(name, "closed", n, "closed\n"), "native observer saw requester disappear");
    if (!check(observation(name, n, values), "native opened requester geometry observed")) goto out;
    check_long("AslRequest BOOL", result != FALSE, accept);
    check(fr->fr_File && !strcmp(fr->fr_File, "probe.txt"), "returned file is probe.txt");
    check(ram_drawer(fr->fr_Drawer), "returned drawer denotes RAM");
    check(fr->fr_LeftEdge == values[AP_WL] && fr->fr_TopEdge == values[AP_WT] &&
        fr->fr_Width == values[AP_WW] && fr->fr_Height == values[AP_WH], "returned geometry matches native observed window");
    check(fr->fr_NumArgs == 0 && !fr->fr_ArgList && (ULONG)fr->fr_UserData == AP_USERDATA,
        "returned scalar outputs remain bounded");
    check(record_result(name, n, result, fr), "actual native result fields recorded");
    FreeAslRequest(fr); fr = NULL;
    ActivateWindow(window);
    for (poll = 0; poll < 100 && !refreshed; ++poll) {
        refreshed = drain_window(window, TRUE);
        if (!refreshed) Delay(AP_POLL_TICKS);
    }
    check(refreshed, "ordinary window received post-request refresh");
    SetAPen(window->RPort, 1); RectFill(window->RPort, 12, 24, 110, 65);
    check(window->WScreen == screen, "ordinary window drawing calls completed");
out:
    if (helper_attempted && !done) {
        done = await_helper(name, n);
        if (!done) retain_until_outer_timeout();
    }
    if (fr) FreeAslRequest(fr);
    if (window) { drain_window(window, TRUE); CloseWindow(window); }
    if (screen) check(CloseScreen(screen), "owned screen closed after helper");
    if (P96Base) CloseLibrary(P96Base);
    if (AslBase) CloseLibrary(AslBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    return test_done(name ? name : "asl_file_probe");
}
