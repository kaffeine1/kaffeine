#pragma once
#include <exec/io.h>
#define UNIT_MICROHZ 0
#define UNIT_VBLANK 1
#define UNIT_ECLOCK 2
#define UNIT_WAITUNTIL 3
#define UNIT_WAITECLOCK 4
#define TR_ADDREQUEST 9
#define TR_GETSYSTIME 10
#define TR_SETSYSTIME 11
/* The host's sys/time.h has its own struct timeval: the AROS one, two
   ULONGs, takes another tag in these tests. */
#define timeval kaffeine_test_timeval
struct timeval
{
    ULONG tv_secs;
    ULONG tv_micro;
};
struct EClockVal
{
    ULONG ev_hi;
    ULONG ev_lo;
};
struct timerequest
{
    struct IORequest tr_node;
    struct timeval tr_time;
};
