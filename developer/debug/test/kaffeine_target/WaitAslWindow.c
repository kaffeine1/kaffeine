/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Observe only the controlled ASL probe's immutable titles. No GUI action,
    callback, guest pointer or native pointer is passed between processes.
*/
#include <aros/config.h>
#if !defined(__AROS__) || !defined(__aarch64__)
#error This helper requires native AROS AArch64 headers and compiler.
#endif
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "asl_probe_protocol.h"

struct IntuitionBase *IntuitionBase;

struct Observation { int screens, windows, active, truncated; long v[AP_VALUES]; };

/* The expected strings are fixed and short. Never strlen() a foreign title.
   The candidate screen's titles stay immutable until this helper has exited. */
static int title_is(const UBYTE *got, const char *expected)
{
    unsigned i;
    if (!got) return 0;
    for (i = 0; i < AP_TITLE; ++i) {
        if (got[i] != (UBYTE)expected[i]) return 0;
        if (!expected[i]) return 1;
    }
    return 0;
}

static void observe(const char *screen_title, const char *window_title,
                    struct Observation *o)
{
    struct Screen *s;
    unsigned ns = 0;
    ULONG key;

    memset(o, 0, sizeof(*o));
    key = LockIBase(0); /* Zero is a valid key. No other locks are held. */
    for (s = IntuitionBase->FirstScreen; s && ns < 32; s = s->NextScreen, ++ns) {
        struct Window *w;
        unsigned nw = 0;
        if (!title_is(s->DefaultTitle, screen_title)) continue;
        ++o->screens;
        for (w = s->FirstWindow; w && nw < 64; w = w->NextWindow, ++nw) {
            if (!title_is(w->Title, window_title)) continue;
            ++o->windows;
            o->active = w->WScreen == s && IntuitionBase->ActiveScreen == s &&
                        IntuitionBase->ActiveWindow == w && (w->Flags & WFLG_CLOSEGADGET);
            o->v[AP_SL] = s->LeftEdge; o->v[AP_ST] = s->TopEdge;
            o->v[AP_SW] = s->Width; o->v[AP_SH] = s->Height;
            o->v[AP_WL] = w->LeftEdge; o->v[AP_WT] = w->TopEdge;
            o->v[AP_WW] = w->Width; o->v[AP_WH] = w->Height;
            o->v[AP_BL] = w->BorderLeft; o->v[AP_BT] = w->BorderTop;
            o->v[AP_BR] = w->BorderRight; o->v[AP_BB] = w->BorderBottom;
        }
        if (w) o->truncated = 1;
    }
    if (s) o->truncated = 1;
    UnlockIBase(key);
    /* All native pointers were local to this locked observation. */
}

static int valid(const struct Observation *o)
{
    return !o->truncated && o->screens == 1 && o->windows == 1 && o->active &&
           o->v[AP_SL] == 0 && o->v[AP_ST] == 0 &&
           o->v[AP_SW] == AP_SCREEN_W && o->v[AP_SH] == AP_SCREEN_H &&
           o->v[AP_WL] >= 0 && o->v[AP_WT] >= 0 &&
           o->v[AP_WW] > 0 && o->v[AP_WH] > 0 &&
           o->v[AP_WW] <= AP_SCREEN_W - o->v[AP_WL] &&
           o->v[AP_WH] <= AP_SCREEN_H - o->v[AP_WT] &&
           o->v[AP_BL] >= 0 && o->v[AP_BT] > 0 &&
           o->v[AP_BR] >= 0 && o->v[AP_BB] >= 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    int okay;
    if (!f) return 0;
    okay = fputs(text, f) >= 0;
    if (fclose(f)) okay = 0;
    return okay;
}

/* Freshness was established by the probe; nevertheless refuse an existing
   final file rather than treating it as evidence of this execution. */
static int absent(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    LONG error = lock ? 0 : IoErr();
    if (lock) UnLock(lock);
    return !lock && error == ERROR_OBJECT_NOT_FOUND;
}

/* A final marker appears whole, by Rename() of its own temporary file. */
static int publish(const char *marker, const char *temporary)
{
    return absent(marker) && absent(temporary) && write_text(temporary, "ready\n") &&
           Rename((CONST_STRPTR)temporary, (CONST_STRPTR)marker);
}

int main(int argc, char **argv)
{
    struct Observation o, previous, again;
    char screen_title[AP_TITLE], window_title[AP_TITLE], path[AP_PATH];
    char shot[AP_PATH], shot_temporary[AP_PATH], keys[AP_PATH], keys_temporary[AP_PATH], text[512];
    const char *name;
    int n, poll, seen = 0, rc = 20, length;

    if (argc != 3 || ap_case(argv[1]) < 0 ||
        (strcmp(argv[2], "1") && strcmp(argv[2], "2"))) return 20;
    name = argv[1]; n = argv[2][0] - '0';
    if (!ap_title(screen_title, name, "screen", n) ||
        !ap_title(window_title, name, "requester", n) ||
        !ap_path(shot, name, "mouse", n) || !ap_path(shot_temporary, name, "mouse-tmp", n) ||
        !ap_path(keys, name, "keys", n) || !ap_path(keys_temporary, name, "keys-tmp", n)) return 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!IntuitionBase) return 20;
    memset(&previous, 0, sizeof(previous));
    for (poll = 0; poll < AP_POLLS; ++poll) {
        observe(screen_title, window_title, &o);
        if (o.truncated || o.screens > 1 || o.windows > 1) goto out;
        if (valid(&o) && valid(&previous) && !memcmp(o.v, previous.v, sizeof(o.v))) { seen = 1; break; }
        previous = o;
        Delay(AP_POLL_TICKS);
    }
    if (!seen) goto out;
    length = snprintf(text, sizeof(text),
        "visible %s %d %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n", name, n,
        o.v[0],o.v[1],o.v[2],o.v[3],o.v[4],o.v[5],o.v[6],o.v[7],o.v[8],o.v[9],o.v[10],o.v[11]);
    if (length < 0 || (unsigned)length >= sizeof(text) ||
        !ap_path(path, name, "observed", n) || !absent(path) || !write_text(path, text)) goto out;
    if (fputs(text, stdout) < 0 || fflush(stdout)) goto out;
    /* First the screenshot of the open requester, then its key, once the
       same requester is observed again unchanged; no pointer is kept over
       the wait. */
    if (!publish(shot, shot_temporary)) goto out;
    if (printf("shot %s %d\n", name, n) < 0 || fflush(stdout)) goto out;
    Delay(AP_SHOT_TICKS);
    observe(screen_title, window_title, &again);
    if (!valid(&again) || memcmp(again.v, o.v, sizeof(o.v))) goto out;
    if (!publish(keys, keys_temporary)) goto out;
    if (printf("keys %s %d\n", name, n) < 0 || fflush(stdout)) goto out;
    for (poll = 0; poll < AP_POLLS; ++poll) {
        observe(screen_title, window_title, &o);
        if (o.truncated || o.screens != 1 || o.windows > 1) goto out;
        if (!o.windows) {
            if (!ap_path(path, name, "closed", n) || !absent(path) || !write_text(path, "closed\n")) goto out;
            if (printf("closed %s %d\n", name, n) < 0) goto out;
            rc = fflush(stdout) ? 20 : 0;
            break;
        }
        Delay(AP_POLL_TICKS);
    }
out:
    if (rc) fprintf(stderr, "FAIL native ASL observer phase, seen=%d\n", seen);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
