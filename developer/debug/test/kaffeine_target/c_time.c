/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The time functions of libnix, which reach timer.device through DOSBase
    without opening it: clock() and its constructor read the E-clock
    through the io_Device of dl_TimeReq, and sleep(), usleep() and
    nanosleep() copy dl_TimeReq onto the stack and call DoIO() with
    TR_ADDREQUEST on the copy. time() goes through DateStamp().

    The program waits for short times and checks that clock() moves at
    least half as far, with no upper bound, because an emulator may run the
    guest slower than the clock. It prints no times, so its output is the
    same on every run.
*/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <dos/dosextens.h>
#include <devices/timer.h>

/* libnix has nanosleep() but no prototype for it */
int nanosleep(const struct timespec *req, struct timespec *rem);

extern struct DosLibrary *DOSBase;

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

int main(void)
{
    struct timespec ts = { 0, 300000000 }, rem = { 5, 5 };
    clock_t c0, c1;
    time_t t0, t1;

    check(DOSBase->dl_lib.lib_Version >= 36, "dos.library is version 36 or later");
    check(DOSBase->dl_TimeReq != NULL && DOSBase->dl_TimeReq->tr_node.io_Device != NULL,
          "dl_TimeReq names timer.device");

    t0 = time(NULL);
    check(t0 != (time_t)-1, "time() answers");
    c0 = clock();
    check(usleep(200000) == 0, "usleep(200000) returns 0");
    c1 = clock();
    check(c1 - c0 >= CLOCKS_PER_SEC / 10, "clock() moves at least 0.1 s over usleep(200000)");

    c0 = clock();
    check(nanosleep(&ts, &rem) == 0 && rem.tv_sec == 0 && rem.tv_nsec == 0,
          "nanosleep() of 0.3 s returns 0 and clears the remainder");
    c1 = clock();
    check(c1 - c0 >= CLOCKS_PER_SEC * 15 / 100, "clock() moves at least 0.15 s over nanosleep()");

    c0 = clock();
    sleep(1);
    c1 = clock();
    check(c1 - c0 >= CLOCKS_PER_SEC / 2, "clock() moves at least 0.5 s over sleep(1)");
    t1 = time(NULL);
    check(t1 >= t0 + 1, "time() moves at least one second over the three waits");

    printf("c_time: %d checks, %d failed\n", checks, failures);
    return failures ? 10 : 0;
}
