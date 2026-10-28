#ifndef KAFFEINE_TEST_NATIVE_DOSEXTENS_H
#define KAFFEINE_TEST_NATIVE_DOSEXTENS_H
#include <exec/tasks.h>
#include <dos/bptr.h>

/* The fields of a native Process that the shadow engine reads. */
struct MsgPort;
struct Process
{
    struct Task pr_Task;
    BPTR pr_CES;
    struct MsgPort *pr_ConsoleTask, *pr_FileSystemTask;
};
#endif
