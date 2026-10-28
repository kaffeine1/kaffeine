/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_FILEHANDLE_H
#define KAFFEINE_FILEHANDLE_H
#include "kaffeine_intern.h"
#include <dos/dos.h>
#ifdef __cplusplus
extern "C" {
#endif
ULONG Kaffeine_FileImport(struct KaffeineContext *, BPTR, BOOL);
BOOL Kaffeine_FileResolve(struct KaffeineContext *, ULONG, BPTR *);
BOOL Kaffeine_FileBufferPrepare(struct KaffeineContext *, ULONG);
BOOL Kaffeine_FileBufferFinish(struct KaffeineContext *, ULONG);
BOOL Kaffeine_FileStreamPrepare(struct KaffeineContext *, BOOL, ULONG *);
BOOL Kaffeine_FileAllPrepare(struct KaffeineContext *);
BOOL Kaffeine_FileAllFinish(struct KaffeineContext *);
void Kaffeine_FileCleanup(struct KaffeineContext *);
IPTR Kaffeine_FilePacket(struct KaffeineContext *, ULONG, ULONG);
BOOL Kaffeine_FilePort(struct KaffeineContext *, ULONG, struct MsgPort **);
/* Packets to the synthetic handler ports of files and of handlers */
struct KaffeineQueue;
struct InfoData;
BOOL Kaffeine_PacketMessage(struct KaffeineContext *, ULONG, struct KaffeineQueue *);
BOOL Kaffeine_PacketPort(struct KaffeineContext *, ULONG);
BOOL Kaffeine_PacketTask(struct KaffeineContext *);
BOOL Kaffeine_PacketRange(struct KaffeineContext *, ULONG, const struct KaffeineQueue *, ULONG, ULONG);
void Kaffeine_PacketReply(struct KaffeineContext *, ULONG, ULONG, const struct KaffeineQueue *, LONG, LONG);
void Kaffeine_InfoToGuest(struct KaffeineContext *, ULONG, const struct InfoData *);
IPTR kaffeine_dos_Input(struct KaffeineContext *, void *);
IPTR kaffeine_dos_Output(struct KaffeineContext *, void *);
IPTR kaffeine_dos_Close(struct KaffeineContext *, void *);
IPTR kaffeine_dos_Seek(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SelectInput(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SelectOutput(struct KaffeineContext *, void *);
IPTR kaffeine_dos_FGetC(struct KaffeineContext *, void *);
IPTR kaffeine_dos_FPutC(struct KaffeineContext *, void *);
IPTR kaffeine_dos_UnGetC(struct KaffeineContext *, void *);
IPTR kaffeine_dos_Flush(struct KaffeineContext *, void *);
IPTR kaffeine_dos_WaitForChar(struct KaffeineContext *, void *);
IPTR kaffeine_dos_IsInteractive(struct KaffeineContext *, void *);
IPTR kaffeine_dos_DupLockFromFH(struct KaffeineContext *, void *);
IPTR kaffeine_dos_OpenFromLock(struct KaffeineContext *, void *);
IPTR kaffeine_dos_ParentOfFH(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SetMode(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SetFileSize(struct KaffeineContext *, void *);
IPTR kaffeine_dos_LockRecord(struct KaffeineContext *, void *);
IPTR kaffeine_dos_UnLockRecord(struct KaffeineContext *, void *);
IPTR kaffeine_dos_LockRecords(struct KaffeineContext *, void *);
IPTR kaffeine_dos_UnLockRecords(struct KaffeineContext *, void *);
IPTR kaffeine_dos_ChangeMode(struct KaffeineContext *, void *);
#define KAFFEINE_DOS_FILE_THUNKS \
    {36, kaffeine_dos_Close}, {54, kaffeine_dos_Input}, {60, kaffeine_dos_Output}, \
    {66, kaffeine_dos_Seek}, {204, kaffeine_dos_WaitForChar}, {216, kaffeine_dos_IsInteractive}, \
    {270, kaffeine_dos_LockRecord}, {276, kaffeine_dos_LockRecords}, \
    {282, kaffeine_dos_UnLockRecord}, {288, kaffeine_dos_UnLockRecords}, \
    {294, kaffeine_dos_SelectInput}, {300, kaffeine_dos_SelectOutput}, \
    {306, kaffeine_dos_FGetC}, {312, kaffeine_dos_FPutC}, {318, kaffeine_dos_UnGetC}, \
    {360, kaffeine_dos_Flush}, {372, kaffeine_dos_DupLockFromFH}, \
    {378, kaffeine_dos_OpenFromLock}, {384, kaffeine_dos_ParentOfFH}, \
    {426, kaffeine_dos_SetMode}, {450, kaffeine_dos_ChangeMode}, {456, kaffeine_dos_SetFileSize}
#ifdef __cplusplus
}
#endif
#endif
