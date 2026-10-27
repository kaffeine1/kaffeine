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
struct Task *FindTask(const char *);
#ifdef __cplusplus
}
#endif
