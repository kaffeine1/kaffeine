#pragma once
#include <stdlib.h>
#include <string.h>
#define SIGB_DOS 8
#define SIGF_DOS (1UL << SIGB_DOS)
#define AllocMem(size, flags) calloc(1, size)
#define FreeMem(ptr, size) free(ptr)
#define AllocVec(size, flags) calloc(1, size)
#define FreeVec(ptr) free(ptr)
#define CopyMem(src, dst, n) memcpy(dst, src, n)
/* one task on the host */
#define Forbid() ((void)0)
#define Permit() ((void)0)

/* Defined by each test that links the Exec compatibility handlers. */
struct MsgPort;
struct Message;
#ifdef __cplusplus
extern "C"
#endif
void PutMsg(struct MsgPort *port, struct Message *message);
struct Task;
#ifdef __cplusplus
extern "C" {
#endif
void Signal(struct Task *, unsigned int);
/* Older GUI fixtures have no kernel resource. Their weak native
   collaborator refuses recipe installation before allocating a wake bit. */
void *OpenResource(const char *);
signed char AllocSignal(int);
void FreeSignal(int);
unsigned int SetSignal(unsigned int, unsigned int);
struct Task *FindTask(const char *);
struct SignalSemaphore;
void InitSemaphore(struct SignalSemaphore *);
void ObtainSemaphore(struct SignalSemaphore *);
void ReleaseSemaphore(struct SignalSemaphore *);
#ifdef __cplusplus
}
#endif
