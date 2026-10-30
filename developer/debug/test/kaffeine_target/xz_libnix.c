/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The POSIX calls of xz 5.8.3 that libnix lacks, for its m68k build. xz
    catches signals with sigaction() and blocks them with sigprocmask()
    while it updates its state; libnix has signal() and raises SIGINT itself
    at Ctrl-C, so sigaction() sets the handler through signal() and
    sigprocmask() has nothing to block. signal() takes the signals of ISO C
    only, and the others that xz catches, SIGHUP and SIGPIPE among them,
    never come on AmigaOS, so sigaction() takes them without a handler. xz
    copies the owner and the permissions of a file to the file it writes;
    AmigaOS keeps no owners, and its protection bits are not those of POSIX,
    so fchown() and fchmod() leave the file as it is. The fcntl() of libnix
    fails every call, and xz reads the flags of standard input, standard
    output and its self-pipe with F_GETFL and sets O_NONBLOCK with F_SETFL:
    here the first gives no flags and the second does nothing, so the files
    block as libnix has them. xz waits with poll() only when a read() or
    write() gives EAGAIN, which they then never do, so poll() finds every
    descriptor ready.
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    void (*old)(int) = act ? signal(sig, act->sa_handler) : SIG_DFL;

    if (old == SIG_ERR)
        old = SIG_DFL;
    if (oact)
    {
        oact->sa_handler = old;
        sigemptyset(&oact->sa_mask);
        oact->sa_flags = 0;
    }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    (void)how;
    (void)set;
    if (oset)
        sigemptyset(oset);
    return 0;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    unsigned long i;

    (void)timeout;
    for (i = 0; i < nfds; i++)
        fds[i].revents = fds[i].events;
    return (int)nfds;
}

int fcntl(int fd, int cmd, ...)
{
    (void)fd;
    if (cmd == F_GETFL || cmd == F_SETFL)
        return 0;
    errno = EINVAL;
    return -1;
}
