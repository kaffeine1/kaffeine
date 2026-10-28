/* Copyright (C) 2026, The AROS Development Team. All rights reserved.

   Legacy DOS buffer and Task snapshot unit fixtures use numeric native tokens
   and a synthetic heap. Keep their collaborators at that boundary. The new
   test_exec_messages and test_dos_packets link the production association,
   queues, heap and dispatcher with recording native DOS collaborators.
*/
#include "kaffeine_filehandle.h"
#include "kaffeine_handlers.h"
ULONG Kaffeine_FileImport(struct KaffeineContext *ctx, BPTR native, BOOL owned)
{ (void)ctx; (void)owned; return (ULONG)(IPTR)native; }
BOOL Kaffeine_FileResolve(struct KaffeineContext *ctx, ULONG token, BPTR *out)
{ (void)ctx; *out = (BPTR)(IPTR)token; return TRUE; }
BOOL Kaffeine_FileBufferPrepare(struct KaffeineContext *ctx, ULONG token)
{ (void)ctx; (void)token; return TRUE; }
BOOL Kaffeine_FileBufferFinish(struct KaffeineContext *ctx, ULONG token)
{ (void)ctx; (void)token; return TRUE; }

BOOL Kaffeine_FileStreamPrepare(struct KaffeineContext *ctx, BOOL input, ULONG *token)
{ (void)ctx; (void)input; *token = 0; return TRUE; }
BOOL Kaffeine_FileAllPrepare(struct KaffeineContext *ctx) { (void)ctx; return TRUE; }
BOOL Kaffeine_FileAllFinish(struct KaffeineContext *ctx) { (void)ctx; return TRUE; }
ULONG Kaffeine_HandlerPort(struct KaffeineContext *ctx, struct MsgPort *native)
{ (void)ctx; return (ULONG)(IPTR)native; }
