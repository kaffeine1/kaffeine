/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   IND_WRITEEVENT from 68k code, written the way amigentd 1.1.1 writes its
   input: keys with their dead-key history, pixel positions on a screen,
   clicks and relative moves, all inside a window of the fixture's own.
   Then the single-event semantics of the autodoc, the payloads that the
   bridge refuses, and cleanup. Keys other than a qualifier and every
   mouse event are sent only once a qualifier key has reached this window,
   so nothing lands in another one. */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
static struct IOStdReq *input;
static struct Window *w;
/* What the device replaces in place: a stale link and an old timestamp */
#define STALE ((struct InputEvent *)0xFFFFFFF0)
#define OLD_SECS 0x01020304
#define CONTROL 0x63

static struct
{
    BOOL key[256], moved, down, up;
    WORD mx, my, bx, by;
} seen;

static void event(struct InputEvent *ev, UBYTE class, UBYTE subclass, UWORD code, UWORD qualifier)
{
    memset(ev, 0, sizeof(*ev));
    ev->ie_NextEvent = STALE;
    ev->ie_Class = class;
    ev->ie_SubClass = subclass;
    ev->ie_Code = code;
    ev->ie_Qualifier = qualifier;
    ev->ie_TimeStamp.tv_secs = OLD_SECS;
}

static LONG send_event(struct InputEvent *ev, ULONG length)
{
    input->io_Command = IND_WRITEEVENT;
    input->io_Data = ev;
    input->io_Length = length;
    input->io_Flags = 0;
    return DoIO((struct IORequest *)input);
}

/* As the device leaves an event it wrote, and one it never saw */
static BOOL stamped(const struct InputEvent *ev)
{
    return !ev->ie_NextEvent && ev->ie_TimeStamp.tv_secs != OLD_SECS;
}
static BOOL untouched(const struct InputEvent *ev)
{
    return ev->ie_NextEvent == STALE && ev->ie_TimeStamp.tv_secs == OLD_SECS;
}

static void pump(void)
{
    struct IntuiMessage *m;
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort)))
    {
        if (m->Class == IDCMP_RAWKEY)
            seen.key[m->Code & 0xFF] = TRUE;
        else if (m->Class == IDCMP_MOUSEMOVE)
        {
            seen.moved = TRUE;
            seen.mx = m->MouseX;
            seen.my = m->MouseY;
        }
        else if (m->Class == IDCMP_MOUSEBUTTONS && m->Code == SELECTDOWN)
        {
            seen.down = TRUE;
            seen.bx = m->MouseX;
            seen.by = m->MouseY;
        }
        else if (m->Class == IDCMP_MOUSEBUTTONS && m->Code == SELECTUP)
            seen.up = TRUE;
        ReplyMsg((struct Message *)m);
    }
}

/* About two seconds at most for a flag that pump() sets */
static BOOL waited(BOOL *flag)
{
    int i;
    for (i = 0; i < 100; i++)
    {
        pump();
        if (*flag)
            return TRUE;
        Delay(1);
    }
    return FALSE;
}

static void settle(int ticks)
{
    int i;
    for (i = 0; i < ticks; i++)
    {
        pump();
        Delay(1);
    }
}

static void forget(void)
{
    settle(5);
    memset(&seen, 0, sizeof(seen));
}

static BOOL key(UBYTE code, UWORD qualifier, UBYTE prev, ULONG length)
{
    struct InputEvent ev;
    event(&ev, IECLASS_RAWKEY, 0, code, qualifier);
    ev.ie_Prev1DownCode = prev;
    return send_event(&ev, length) == 0 && stamped(&ev);
}

static BOOL pixel(struct Screen *screen, WORD x, WORD y)
{
    struct InputEvent ev;
    struct IEPointerPixel pp;
    pp.iepp_Screen = screen;
    pp.iepp_Position.X = x;
    pp.iepp_Position.Y = y;
    event(&ev, IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL, 0, 0);
    ev.ie_EventAddress = &pp;
    return send_event(&ev, sizeof(ev)) == 0 && stamped(&ev) && pp.iepp_Screen == screen &&
           pp.iepp_Position.X == x && pp.iepp_Position.Y == y;
}

static BOOL mouse(UWORD code, UWORD qualifier, WORD dx, WORD dy)
{
    struct InputEvent ev;
    event(&ev, IECLASS_RAWMOUSE, 0, code, IEQUALIFIER_RELATIVEMOUSE | qualifier);
    ev.ie_X = dx;
    ev.ie_Y = dy;
    return send_event(&ev, sizeof(ev)) == 0 && stamped(&ev);
}

int main(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct InputEvent ev, next;
    struct IEPointerPixel pp;
    struct Screen *first;
    UWORD junk[16];
    BOOL opened = FALSE, active;
    ULONG lock;

    input = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(*input)) : NULL;
    if (!check(input != NULL, "create the input request and its reply port"))
        goto done;
    opened = OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)input, 0) == 0;
    if (!check(opened, "open input.device unit zero"))
        goto done;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    if (IntuitionBase)
        w = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 240, WA_Height, 120,
                           WA_Activate, TRUE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
                           WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, TAG_DONE);
    if (!check(w != NULL, "open a window of its own"))
        goto done;
    settle(50);

    /* A qualifier key changes nothing wherever it lands */
    forget();
    active = check(key(CONTROL, IEQUALIFIER_CONTROL, 0, sizeof(ev)) && key(CONTROL | IECODE_UP_PREFIX, 0, 0, sizeof(ev)),
                   "a qualifier key down and up are written, each with a NULL link and a new timestamp");
    active = check(active && waited(&seen.key[CONTROL | IECODE_UP_PREFIX]) && seen.key[CONTROL],
                   "both reach the active window as IDCMP_RAWKEY");
    if (!active)
        goto refused;

    /* amigentd's key(): a function key with its dead-key history */
    forget();
    check(key(0x50, 0, CONTROL, sizeof(ev)) && key(0xD0, 0, CONTROL, sizeof(ev)) && waited(&seen.key[0xD0]) &&
          seen.key[0x50], "F1 down and up reach the window");

    /* One event per request: ie_NextEvent is never followed */
    forget();
    event(&ev, IECLASS_RAWKEY, 0, 0x51, 0);
    event(&next, IECLASS_RAWKEY, 0, 0xD1, 0);
    ev.ie_NextEvent = &next;
    check(send_event(&ev, 2 * sizeof(ev)) == 0 && waited(&seen.key[0x51]), "the first event of a chain reaches the window");
    settle(10);
    check(!seen.key[0xD1] && stamped(&ev) && untouched(&next),
          "the linked event is neither written nor touched, and the link is cleared");
    check(send_event(&next, sizeof(next)) == 0 && waited(&seen.key[0xD1]), "the release written on its own arrives");

    /* io_Length is not read */
    forget();
    check(key(CONTROL, IEQUALIFIER_CONTROL, 0, 0) && key(CONTROL | IECODE_UP_PREFIX, 0, 0, 0) &&
          waited(&seen.key[CONTROL | IECODE_UP_PREFIX]), "a zero io_Length still writes the event");

    /* A pixel position on the window's screen, then amigentd's move_to(),
       which reads IntuitionBase->FirstScreen under LockIBase(): the
       fixture expects its window on the front screen */
    forget();
    check(pixel(w->WScreen, w->LeftEdge + 30, w->TopEdge + 40) && waited(&seen.moved),
          "a pixel position on the window's screen moves the pointer");
    settle(10);
    check(seen.mx == 30 && seen.my == 40, "IDCMP_MOUSEMOVE reports the position inside the window");
    lock = LockIBase(0);
    first = IntuitionBase->FirstScreen;
    UnlockIBase(lock);
    forget();
    check(first && pixel(first, w->LeftEdge + 50, w->TopEdge + 60) && waited(&seen.moved),
          "a pixel position on IntuitionBase->FirstScreen moves the pointer");
    settle(10);
    check(seen.mx == 50 && seen.my == 60, "FirstScreen is the window's front screen");

    /* amigentd's set_buttons() and move_by(), inside the window */
    forget();
    check(mouse(IECODE_LBUTTON, IEQUALIFIER_LEFTBUTTON, 0, 0) && mouse(IECODE_LBUTTON | IECODE_UP_PREFIX, 0, 0, 0) &&
          waited(&seen.up), "a left click is written as two RAWMOUSE events");
    check(seen.down && seen.bx == 50 && seen.by == 60, "IDCMP_MOUSEBUTTONS reports the click where the pointer is");
    forget();
    check(mouse(IECODE_NOBUTTON, 0, 4, 4) && waited(&seen.moved), "a relative move moves the pointer");
    settle(10);
    check(seen.mx > 50 && seen.my > 60, "the relative move goes further into the window");

refused:
    /* The bridge's refusals: nothing reaches the device or the guest event */
    forget();
    memset(junk, 0, sizeof(junk));
    check(send_event((struct InputEvent *)((UBYTE *)junk + 1), sizeof(ev)) == IOERR_BADADDRESS,
          "an odd event address ends with IOERR_BADADDRESS");
    event(&ev, IECLASS_GADGETUP, 0, 0, 0);
    ev.ie_EventAddress = junk;
    check(send_event(&ev, sizeof(ev)) == IOERR_NOCMD && untouched(&ev),
          "a class whose position is an address ends with IOERR_NOCMD");
    event(&ev, IECLASS_NEWPOINTERPOS, IESUBCLASS_TABLET, 0, 0);
    ev.ie_EventAddress = junk;
    check(send_event(&ev, sizeof(ev)) == IOERR_NOCMD && untouched(&ev), "a tablet position ends with IOERR_NOCMD");
    pp.iepp_Screen = NULL;
    pp.iepp_Position.X = pp.iepp_Position.Y = 10;
    event(&ev, IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL, 0, 0);
    ev.ie_EventAddress = &pp;
    check(send_event(&ev, sizeof(ev)) == IOERR_BADADDRESS && untouched(&ev),
          "a pixel position without a screen ends with IOERR_BADADDRESS");
    pp.iepp_Screen = (struct Screen *)junk;
    check(send_event(&ev, sizeof(ev)) == IOERR_BADADDRESS && untouched(&ev),
          "a pixel position on memory that is no screen ends with IOERR_BADADDRESS");
    if (w)
    {
        settle(10);
        check(!seen.moved && !seen.down && !seen.up, "no refused event reaches the window");
    }

done:
    if (w)
    {
        pump();
        CloseWindow(w);
    }
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (opened)
        CloseDevice((struct IORequest *)input);
    if (input)
        DeleteIORequest((struct IORequest *)input);
    if (port)
        DeleteMsgPort(port);
    return test_done("exec_input_event");
}
