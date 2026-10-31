#ifndef KAFFEINE_TEST_SCHED_PROTO_EXEC_H
#define KAFFEINE_TEST_SCHED_PROTO_EXEC_H
#include <exec/tasks.h>
#define SIGB_DOS 8
#define SIGF_DOS (1UL << SIGB_DOS)

/* The native Exec of the guest task scheduler, which test_guest_tasks.cpp
   defines and records: one native task runs every guest task. */
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocMem(ULONG size, ULONG flags);
void FreeMem(APTR memory, ULONG size);
ULONG SetSignal(ULONG newSignals, ULONG signalSet);
void Signal(struct Task *task, ULONG signalSet);
ULONG Wait(ULONG signalSet);
struct Task *FindTask(CONST_STRPTR name);
BYTE SetTaskPri(struct Task *task, LONG priority);
BYTE AllocSignal(LONG signalNum);
void FreeSignal(LONG signalNum);
#ifdef __cplusplus
}
#endif
#endif
