/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    timer.device through Exec, the way C programs use it: CreateMsgPort(),
    CreateIORequest() of a timerequest, OpenDevice(), DoIO() with
    TR_GETSYSTIME and TR_ADDREQUEST, the time functions of TimerBase,
    CloseDevice() and the deletes, twice over, then units VBLANK, WAITUNTIL
    and ECLOCK, signals, and the errors that the bridge returns for what it
    does not support. The delays are measured with the timer itself. With
    the argument "sendio" the program only calls SendIO(), which asynchronous
    I/O needs and which must end the program with rc 20.
*/

#include <exec/types.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stddef.h>

#include "m68ktest.h"

struct Device *TimerBase;

/* The 68k layouts that the bridge converts. */
typedef char check_ioreq[sizeof(struct IORequest) == 32 ? 1 : -1];
typedef char check_timereq[sizeof(struct timerequest) == 40 ? 1 : -1];
typedef char check_trtime[offsetof(struct timerequest, tr_time) == 32 ? 1 : -1];
typedef char check_port[sizeof(struct MsgPort) == 34 ? 1 : -1];

static const char timer_name[] = "timer.device";

static BOOL list_empty(struct List *list)
{
    return list->lh_TailPred == (struct Node *)list;
}

/* The time from start to now in microseconds, from TimerBase. */
static ULONG elapsed_micro(const struct timeval *start)
{
    struct timeval now, from = *start;

    GetSysTime(&now);
    SubTime(&now, &from);
    if (now.tv_secs > 4000)
        return 0xFFFFFFFF;
    return now.tv_secs * 1000000 + now.tv_micro;
}

static struct timerequest *open_timer(struct MsgPort *port, ULONG unit, const char *what)
{
    struct timerequest *tr = CreateIORequest(port, sizeof(struct timerequest));

    if (!check(tr != NULL, "CreateIORequest() of a timerequest"))
        return NULL;
    if (!check_long(what, OpenDevice((CONST_STRPTR)timer_name, unit, &tr->tr_node, 0), 0))
    {
        DeleteIORequest(tr);
        return NULL;
    }
    return tr;
}

static void close_timer(struct timerequest *tr)
{
    CloseDevice(&tr->tr_node);
    check(tr->tr_node.io_Device == (struct Device *)-1, "CloseDevice() leaves io_Device -1");
    DeleteIORequest(tr);
}

static void microhz_round(int round)
{
    struct MsgPort *port = CreateMsgPort();
    struct timeval t1, t2, a, b;
    struct EClockVal e1, e2;
    struct timerequest *tr;
    ULONG rate, waited;

    Printf("round %ld\n", (LONG)round);
    if (!check(port != NULL, "CreateMsgPort()"))
        return;
    check(port->mp_Node.ln_Type == NT_MSGPORT && port->mp_Flags == PA_SIGNAL &&
          port->mp_SigBit >= 16 && port->mp_SigTask == FindTask(NULL) &&
          list_empty(&port->mp_MsgList),
          "CreateMsgPort() makes a signalling port of this task with an empty list");
    tr = open_timer(port, UNIT_MICROHZ, "OpenDevice(timer.device, UNIT_MICROHZ)");
    if (!tr)
    {
        DeleteMsgPort(port);
        return;
    }
    check(tr->tr_node.io_Message.mn_ReplyPort == port &&
          tr->tr_node.io_Message.mn_Length == sizeof(struct timerequest),
          "the request keeps its reply port and length");
    check(tr->tr_node.io_Device != NULL && tr->tr_node.io_Error == 0,
          "OpenDevice() gives the timer.device base");
    TimerBase = tr->tr_node.io_Device;

    tr->tr_node.io_Command = TR_GETSYSTIME;
    check_long("DoIO(TR_GETSYSTIME)", DoIO(&tr->tr_node), 0);
    t1 = tr->tr_time;
    check(t1.tv_secs > 0 && t1.tv_micro < 1000000, "TR_GETSYSTIME returns a normalized time");
    GetSysTime(&t2);
    check(CmpTime(&t2, &t1) <= 0, "GetSysTime() of TimerBase is not earlier than TR_GETSYSTIME");

    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = 0;
    tr->tr_time.tv_micro = 200000;
    check_long("DoIO(TR_ADDREQUEST, 0.2 s)", DoIO(&tr->tr_node), 0);
    waited = elapsed_micro(&t1);
    Printf("     waited %ld us\n", (LONG)waited);
    check(waited >= 200000 && waited < 10000000, "the delay lasts at least 0.2 s");
    check(tr->tr_time.tv_secs == 0 && tr->tr_time.tv_micro == 0,
          "the completed delay has both time fields cleared");
    check(CheckIO(&tr->tr_node) == &tr->tr_node, "CheckIO() of the completed request returns it");
    check_long("WaitIO() of the completed request", WaitIO(&tr->tr_node), 0);
    AbortIO(&tr->tr_node);
    check(tr->tr_node.io_Error == 0, "AbortIO() of a completed request changes nothing");

    a.tv_secs = 1; a.tv_micro = 900000; b.tv_secs = 2; b.tv_micro = 300000;
    AddTime(&a, &b);
    check(a.tv_secs == 4 && a.tv_micro == 200000, "AddTime(1.9 s, 2.3 s) is 4.2 s");
    a.tv_secs = 5; a.tv_micro = 100000;
    SubTime(&a, &b);
    check(a.tv_secs == 2 && a.tv_micro == 800000, "SubTime(5.1 s, 2.3 s) is 2.8 s");
    check(CmpTime(&a, &b) == -1 && CmpTime(&b, &a) == 1 && CmpTime(&a, &a) == 0,
          "CmpTime() is -1 when the first time is later, 1 when it is earlier");

    rate = ReadEClock(&e1);
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = 0;
    tr->tr_time.tv_micro = 50000;
    DoIO(&tr->tr_node);
    ReadEClock(&e2);
    Printf("     E-clock %ld Hz\n", (LONG)rate);
    check(rate > 0 && (e2.ev_hi > e1.ev_hi || (e2.ev_hi == e1.ev_hi && e2.ev_lo > e1.ev_lo)),
          "ReadEClock() gives a rate and a count that grows");

    tr->tr_node.io_Command = TR_SETSYSTIME;
    tr->tr_time = t1;
    check_long("TR_SETSYSTIME is refused", DoIO(&tr->tr_node), IOERR_NOCMD);
    check(tr->tr_node.io_Error == IOERR_NOCMD, "io_Error says IOERR_NOCMD");
    check(elapsed_micro(&t1) >= 250000, "the system clock was not set back");
    tr->tr_node.io_Command = CMD_READ;
    check_long("CMD_READ is not a timer command", DoIO(&tr->tr_node), IOERR_NOCMD);

    close_timer(tr);
    DeleteMsgPort(port);
}

static void other_units(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr;
    struct timeval start, target;
    ULONG waited;

    if (!check(port != NULL, "CreateMsgPort() for the other units"))
        return;

    tr = open_timer(port, UNIT_VBLANK, "OpenDevice(timer.device, UNIT_VBLANK)");
    if (tr)
    {
        check(tr->tr_node.io_Device == TimerBase, "every open gives the same timer.device base");
        GetSysTime(&start);
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 100000;
        check_long("DoIO(TR_ADDREQUEST, 0.1 s) on UNIT_VBLANK", DoIO(&tr->tr_node), 0);
        waited = elapsed_micro(&start);
        Printf("     waited %ld us\n", (LONG)waited);
        check(waited >= 80000 && waited < 10000000, "the VBLANK delay lasts about 0.1 s or more");
        close_timer(tr);
    }

    tr = open_timer(port, UNIT_WAITUNTIL, "OpenDevice(timer.device, UNIT_WAITUNTIL)");
    if (tr)
    {
        GetSysTime(&target);
        start = target;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 150000;
        AddTime(&tr->tr_time, &target);
        target = tr->tr_time;
        tr->tr_node.io_Command = TR_ADDREQUEST;
        check_long("DoIO(TR_ADDREQUEST) until 0.15 s from now", DoIO(&tr->tr_node), 0);
        GetSysTime(&start);
        check(CmpTime(&start, &target) <= 0, "UNIT_WAITUNTIL returns once that time has come");
        close_timer(tr);
    }

    tr = open_timer(port, UNIT_ECLOCK, "OpenDevice(timer.device, UNIT_ECLOCK)");
    if (tr)
    {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 1000;
        check_long("an EClock delay is refused, the AROS driver never replies to it",
                   DoIO(&tr->tr_node), IOERR_NOCMD);
        tr->tr_node.io_Command = TR_GETSYSTIME;
        check_long("TR_GETSYSTIME on UNIT_ECLOCK", DoIO(&tr->tr_node), 0);
        close_timer(tr);
    }

    tr = CreateIORequest(port, sizeof(struct timerequest));
    if (check(tr != NULL, "CreateIORequest() for a device without adapter"))
    {
        check_long("OpenDevice(serial.device) fails",
                   OpenDevice((CONST_STRPTR)"serial.device", 0, &tr->tr_node, 0), IOERR_OPENFAIL);
        check(tr->tr_node.io_Error == IOERR_OPENFAIL && tr->tr_node.io_Device == NULL,
              "the failed open leaves io_Error -1 and no device");
        DeleteIORequest(tr);
    }
    tr = CreateIORequest(port, sizeof(struct IORequest));
    if (check(tr != NULL, "CreateIORequest() of a plain IORequest"))
    {
        check_long("a request smaller than a timerequest cannot open timer.device",
                   OpenDevice((CONST_STRPTR)timer_name, UNIT_MICROHZ, &tr->tr_node, 0), IOERR_OPENFAIL);
        DeleteIORequest(tr);
    }
    DeleteMsgPort(port);
}

static void signals(void)
{
    struct Task *me = FindTask(NULL);
    /* A LONG: GCC 6.5 copies a BYTE into another register with move.b and
       later passes that register as a LONG without extending it. */
    LONG sig = AllocSignal(-1);
    ULONG mask;

    if (!check(sig >= 16, "AllocSignal(-1) gives a user signal"))
        return;
    mask = 1UL << sig;
    SetSignal(0, mask);
    Signal(me, mask);
    check((SetSignal(0, 0) & mask) != 0, "Signal(FindTask(NULL)) sets the signal");
    check((Wait(mask) & mask) != 0, "Wait() returns at once for the set signal");
    check((SetSignal(0, 0) & mask) == 0, "Wait() cleared it");
    check_long("AllocSignal() of a taken signal", AllocSignal(sig), -1);
    FreeSignal(sig);
    check_long("the freed signal can be allocated again", AllocSignal(sig), sig);
    FreeSignal(sig);
}

static void sendio(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ? open_timer(port, UNIT_MICROHZ, "OpenDevice() before SendIO()") : NULL;

    if (!tr)
        return;
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = 0;
    tr->tr_time.tv_micro = 10000;
    PutStr((CONST_STRPTR)"SendIO() must stop the program with rc 20\n");
    test_flush();
    SendIO(&tr->tr_node);
    check(FALSE, "SendIO() returned");
}

int main(int argc, char **argv)
{
    ULONG avail;

    if (argc > 1 && argv[1][0] == 's')
    {
        sendio();
        return test_done("exec_timer");
    }
    microhz_round(1);
    avail = AvailMem(MEMF_ANY);
    microhz_round(2);
    check_long("the second round returns all of its memory", AvailMem(MEMF_ANY), avail);
    other_units();
    signals();
    check_long("ports, requests and signals return all of their memory", AvailMem(MEMF_ANY), avail);
    return test_done("exec_timer");
}
