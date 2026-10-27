/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    IDCMP messages of a window, the way C programs take them. A window
    opened active with IDCMP flags gets IDCMP_ACTIVEWINDOW and then
    IDCMP_INTUITICKS; SizeWindow() and MoveWindow() bring IDCMP_NEWSIZE and
    IDCMP_CHANGEWINDOW, after which the Window has its new size and place.
    A program can Wait() for the signal of the IDCMP port. The messages come
    through WaitPort() and GetMsg() and go back through
    ReplyMsg(), then through GT_GetIMsg() and GT_ReplyIMsg() of
    gadtools.library, also after WaitPort(), and none of them keeps guest
    memory. CloseWindow() takes back the messages that the program has not
    replied. ModifyIDCMP() gives a window opened without flags its IDCMP
    port and takes it away. No input is needed: Intuition sends all of these
    by itself.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

/* The 68k layout that programs read. */
typedef char check_imsg[sizeof(struct IntuiMessage) == 52 ? 1 : -1];

#define FLAGS (IDCMP_ACTIVEWINDOW | IDCMP_INTUITICKS | IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW)

struct Seen
{
    LONG active, ticks, newsize, change, other, wrong_window;
    ULONG seconds, micros;
    BOOL backwards;
};

static void note(struct Window *win, struct IntuiMessage *msg, struct Seen *seen)
{
    ULONG class = msg->Class;

    if (msg->IDCMPWindow != win)
        seen->wrong_window++;
    if (class == IDCMP_ACTIVEWINDOW)
        seen->active++;
    else if (class == IDCMP_INTUITICKS)
        seen->ticks++;
    else if (class == IDCMP_NEWSIZE)
        seen->newsize++;
    else if (class == IDCMP_CHANGEWINDOW)
        seen->change++;
    else
        seen->other++;
    if (msg->Seconds < seen->seconds ||
        (msg->Seconds == seen->seconds && msg->Micros < seen->micros))
        seen->backwards = TRUE;
    seen->seconds = msg->Seconds;
    seen->micros = msg->Micros;
}

/* Take messages with GetMsg() until done() or about 5 seconds. */
static void take(struct Window *win, struct Seen *seen, BOOL (*done)(struct Seen *), BOOL gadtools)
{
    LONG waited;

    for (waited = 0; waited < 250 && !done(seen); waited++)
    {
        struct IntuiMessage *msg;

        while (!done(seen) &&
               (msg = gadtools ? GT_GetIMsg(win->UserPort)
                               : (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            note(win, msg, seen);
            if (gadtools)
                GT_ReplyIMsg(msg);
            else
                ReplyMsg(&msg->ExecMessage);
        }
        if (!done(seen))
            Delay(1);
    }
}

/* Take what has come, without waiting. */
static void drain(struct Window *win, struct Seen *seen)
{
    struct IntuiMessage *msg;

    while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
    {
        note(win, msg, seen);
        ReplyMsg(&msg->ExecMessage);
    }
}

static BOOL active_and_ticks(struct Seen *seen) { return seen->active && seen->ticks >= 5; }
static BOOL resized(struct Seen *seen) { return seen->newsize > 0; }
static BOOL moved(struct Seen *seen) { return seen->change > 0; }
static BOOL twenty_ticks(struct Seen *seen) { return seen->ticks >= 20; }

static void messages(struct Window *win)
{
    struct Seen seen = { 0 };
    struct IntuiMessage *msg;
    WORD width, height, left;
    ULONG avail;
    LONG i;

    take(win, &seen, active_and_ticks, FALSE);
    check(seen.active > 0, "IDCMP_ACTIVEWINDOW for a window opened active");
    check(seen.ticks >= 5, "IDCMP_INTUITICKS keep coming");
    check(!seen.wrong_window && !seen.other, "every message names the window and has a class it asked for");
    check(!seen.backwards && seen.seconds > 0, "the times of the messages go forward");

    /* The signal of the IDCMP port comes with the next tick. A signal left
       by a message already taken can end the first Wait() with no message,
       as on any Amiga, so wait until one comes. */
    for (i = 0, msg = NULL; i < 5 && !msg; i++)
    {
        Wait(1UL << win->UserPort->mp_SigBit);
        msg = (struct IntuiMessage *)GetMsg(win->UserPort);
    }
    check(msg != NULL, "Wait() for the signal of the IDCMP port brings a message");
    if (msg)
        ReplyMsg(&msg->ExecMessage);

    /* WaitPort() returns the message that GetMsg() then takes. */
    msg = (struct IntuiMessage *)WaitPort(win->UserPort);
    check(msg != NULL && msg == (struct IntuiMessage *)GetMsg(win->UserPort),
          "WaitPort() returns the message that GetMsg() takes");
    if (msg)
        ReplyMsg(&msg->ExecMessage);

    width = win->Width;
    height = win->Height;
    SizeWindow(win, 30, 20);
    take(win, &seen, resized, FALSE);
    check(seen.newsize > 0, "IDCMP_NEWSIZE after SizeWindow()");
    check(win->Width == width + 30 && win->Height == height + 20,
          "the Window has its new size with the message");
    /* the IDCMP_CHANGEWINDOW of the resize, if any, before the move */
    for (i = 0; i < 10; i++)
    {
        Delay(1);
        drain(win, &seen);
    }

    left = win->LeftEdge;
    seen.change = 0;
    MoveWindow(win, 10, 5);
    take(win, &seen, moved, FALSE);
    check(seen.change > 0, "IDCMP_CHANGEWINDOW after MoveWindow()");
    check(win->LeftEdge == left + 10, "the Window has its new place with the message");

    /* Twenty messages in a row, through GetMsg() and then GadTools */
    avail = AvailMem(MEMF_ANY);
    seen.ticks = 0;
    take(win, &seen, twenty_ticks, FALSE);
    check(seen.ticks >= 20, "twenty IDCMP_INTUITICKS through GetMsg()");
    check_long("GetMsg() and ReplyMsg() keep no guest memory", AvailMem(MEMF_ANY), avail);
    if (GadToolsBase)
    {
        seen.ticks = 0;
        seen.wrong_window = 0;
        take(win, &seen, twenty_ticks, TRUE);
        check(seen.ticks >= 20, "twenty IDCMP_INTUITICKS through GT_GetIMsg()");
        check(!seen.wrong_window, "GT_GetIMsg() messages name the window");
        check_long("GT_GetIMsg() and GT_ReplyIMsg() keep no guest memory", AvailMem(MEMF_ANY), avail);

        /* WaitPort() first, as many programs do, then GT_GetIMsg() */
        seen.ticks = 0;
        for (i = 0; i < 20; i++)
        {
            WaitPort(win->UserPort);
            while ((msg = GT_GetIMsg(win->UserPort)))
            {
                note(win, msg, &seen);
                GT_ReplyIMsg(msg);
            }
        }
        check(seen.ticks >= 20, "twenty IDCMP_INTUITICKS through WaitPort() and GT_GetIMsg()");
        check_long("WaitPort() before GT_GetIMsg() keeps no guest memory", AvailMem(MEMF_ANY), avail);
    }
}

/* A message taken with GetMsg(), one with GT_GetIMsg() and one that
   WaitPort() shows, none of them replied, as a program that closes its
   window on IDCMP_CLOSEWINDOW before replying leaves them. Intuition sends
   no IDCMP_INTUITICKS while one is not replied, so the other two come from
   SizeWindow() and MoveWindow(). */
static void unreplied(struct Window *win)
{
    struct IntuiMessage *taken = NULL, *copy = NULL, *shown;
    LONG i;

    for (i = 0; i < 250 && !taken; i++)
        if (!(taken = (struct IntuiMessage *)GetMsg(win->UserPort)))
            Delay(1);
    SizeWindow(win, -30, -20);
    for (i = 0; GadToolsBase && i < 250 && !copy; i++)
        if (!(copy = GT_GetIMsg(win->UserPort)))
            Delay(1);
    MoveWindow(win, -10, -5);
    shown = (struct IntuiMessage *)WaitPort(win->UserPort);
    check(taken && (copy || !GadToolsBase) && shown,
          "messages left unreplied for CloseWindow()");
}

static void modify(void)
{
    struct Window *win = OpenWindowTags(NULL,
                                        WA_Left, 400, WA_Top, 300,
                                        WA_InnerWidth, 160, WA_InnerHeight, 60,
                                        WA_Title, (ULONG)"gfx_idcmp ModifyIDCMP",
                                        WA_DragBar, TRUE, TAG_DONE);

    if (!check(win != NULL, "OpenWindowTags() without IDCMP flags"))
        return;
    check(win->UserPort == NULL && win->IDCMPFlags == 0, "no IDCMP port without flags");
    check(ModifyIDCMP(win, IDCMP_INTUITICKS), "ModifyIDCMP(IDCMP_INTUITICKS)");
    check(win->UserPort != NULL && win->IDCMPFlags == IDCMP_INTUITICKS,
          "ModifyIDCMP() gives the window its IDCMP port");
    check(ModifyIDCMP(win, 0), "ModifyIDCMP(0)");
    check(win->UserPort == NULL && win->IDCMPFlags == 0,
          "ModifyIDCMP(0) takes the IDCMP port away");
    CloseWindow(win);
}

int main(void)
{
    struct Window *win;
    ULONG avail;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 37");
    if (IntuitionBase)
    {
        /* A first window shows the bridge the screen font, which it keeps. */
        win = OpenWindowTags(NULL, WA_Left, 60, WA_Top, 60, WA_InnerWidth, 100, WA_InnerHeight, 40,
                             WA_Title, (ULONG)"gfx_idcmp", TAG_DONE);
        if (win)
            CloseWindow(win);
        avail = AvailMem(MEMF_ANY);
        win = OpenWindowTags(NULL,
                             WA_Left, 60, WA_Top, 60, WA_InnerWidth, 240, WA_InnerHeight, 100,
                             WA_Title, (ULONG)"gfx_idcmp", WA_DragBar, TRUE,
                             WA_SizeGadget, TRUE, WA_MaxWidth, -1, WA_MaxHeight, -1,
                             WA_Activate, TRUE, WA_IDCMP, FLAGS, TAG_DONE);
        if (check(win != NULL, "OpenWindowTags() active with IDCMP flags"))
        {
            check(win->UserPort != NULL && win->IDCMPFlags == FLAGS,
                  "the window has its IDCMP port and flags");
            messages(win);
            unreplied(win);
            CloseWindow(win);
        }
        modify();
        check_long("the windows and their messages return all of their memory",
                   AvailMem(MEMF_ANY), avail);
    }
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_idcmp");
}
