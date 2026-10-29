/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Poison collaborators for older unit tests that do not send guest signals
   or do asynchronous I/O. Tests exercising them supply strong recording
   definitions instead. */
#include <stdlib.h>
struct Task;
struct MsgPort;
struct IORequest;
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
__attribute__((weak)) unsigned int SetSignal(unsigned int signals, unsigned int set)
{ (void)signals; (void)set; abort(); }
__attribute__((weak)) unsigned int Wait(unsigned int set)
{ (void)set; abort(); }
