/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Poison collaborators for older unit tests that do not send guest signals
   or do asynchronous I/O. Tests exercising them supply strong recording
   definitions instead. */
#include <stdlib.h>
struct Task;
struct MsgPort;
struct IORequest;
struct RastPort;
struct Image;
struct Border;
struct TextAttr;
struct TextFont;
/* No successful kernel proof is invented for older GUI-only fixtures.
   Focused native-recipe tests override both symbols with recording state. */
__attribute__((weak)) void *OpenResource(const char *name)
{ (void)name; return NULL; }
__attribute__((weak)) unsigned int Kaffeine_Host_KrnGetCPUCount(void *resource)
{ (void)resource; abort(); }
__attribute__((weak)) signed char AllocSignal(int requested)
{ (void)requested; abort(); }
__attribute__((weak)) void FreeSignal(int bit)
{ (void)bit; abort(); }
__attribute__((weak)) void *AllocVec(unsigned int size, unsigned int flags)
{ (void)size; (void)flags; abort(); }
__attribute__((weak)) void FreeVec(void *memory) { (void)memory; abort(); }
__attribute__((weak)) void DrawImage(struct RastPort *rp, struct Image *image, short x, short y)
{ (void)rp; (void)image; (void)x; (void)y; abort(); }
__attribute__((weak)) void DrawBorder(struct RastPort *rp, struct Border *border, short x, short y)
{ (void)rp; (void)border; (void)x; (void)y; abort(); }
__attribute__((weak)) void WaitBlit(void) { abort(); }
__attribute__((weak)) struct TextFont *OpenFont(const struct TextAttr *attr)
{ (void)attr; abort(); }
__attribute__((weak)) void Signal(struct Task *task, unsigned int bits)
{ (void)task; (void)bits; abort(); }
__attribute__((weak)) struct Task *FindTask(const char *name)
{ (void)name; abort(); }
__attribute__((weak)) void SendIO(struct IORequest *req)
{ (void)req; abort(); }
__attribute__((weak)) long AbortIO(struct IORequest *req)
{ (void)req; abort(); }
__attribute__((weak)) long WaitIO(struct IORequest *req)
{ (void)req; abort(); }
__attribute__((weak)) void *GetMsg(struct MsgPort *port)
{ (void)port; abort(); }
__attribute__((weak)) void DeleteMsgPort(struct MsgPort *port)
{ (void)port; abort(); }
__attribute__((weak)) unsigned int SetSignal(unsigned int signals, unsigned int set)
{ (void)signals; (void)set; abort(); }
__attribute__((weak)) unsigned int Wait(unsigned int set)
{ (void)set; abort(); }

__attribute__((weak)) void BltTemplate(unsigned char *source, short xs, short mod,
                                      struct RastPort *rp, short xd, short yd, short w, short h)
{ (void)source; (void)xs; (void)mod; (void)rp; (void)xd; (void)yd; (void)w; (void)h; abort(); }

#include "lock_boundary_unit.h"

__attribute__((weak)) BPTR DupLock(BPTR lock) { return lock; }
__attribute__((weak)) BOOL Kaffeine_LockRelease(struct KaffeineContext *c, ULONG token)
{ (void)c; (void)token; return TRUE; }
