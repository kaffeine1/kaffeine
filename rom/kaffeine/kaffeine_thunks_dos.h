/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Private declarations and LVO registrations for DOS data, buffer, string,
    formatter, object and process thunks. Keep registrations shared with the host
    regression test.
*/
#ifndef KAFFEINE_THUNKS_DOS_H
#define KAFFEINE_THUNKS_DOS_H
#include "kaffeine_filehandle.h"

#include "kaffeine_thunks.h"

struct FileInfoBlock;

#ifdef __cplusplus
extern "C" {
#endif

IPTR kaffeine_dos_FreeDosObject(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_VFPrintf(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_VFWritef(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_StrToLong(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_DateStamp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FilePart(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_PathPart(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FGets(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FRead(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FWrite(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_NameFromLock(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_NameFromFH(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Read(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Write(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Open(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Lock(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_DeleteFile(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Rename(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_PutStr(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FPuts(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Execute(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_RunCommand(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_LoadSeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_NewLoadSeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_UnLoadSeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SegmentUnsupported(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CliInitNewcli(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CliInitRun(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_VPrintf(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_MatchFirst(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_MatchNext(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_MatchEnd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AddPart(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetVar(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_GetVar(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_DeleteVar(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Fault(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_PrintFault(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetComment(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetProtection(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ParsePattern(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ParsePatternNoCase(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_MatchPattern(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_MatchPatternNoCase(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ExamineFH(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Examine(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ExNext(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ExAll(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ExAllEnd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_DoPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SendPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_WaitPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ReplyPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AbortPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CreateNewProc(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CreateProc(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SystemTagList(struct KaffeineContext *ctx, void *cpu);

IPTR kaffeine_dos_Info(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CreateDir(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_Exit(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetVBuf(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetFileDate(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SplitName(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_GetArgStr(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetArgStr(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetCurrentDirName(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_GetCurrentDirName(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetProgramName(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetPrompt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_GetPrompt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AssignLock(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AssignLate(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AssignPath(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AssignAdd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_RemAssignList(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_IsFileSystem(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_AddBuffers(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_CompareDates(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_DateToStr(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_StrToDate(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_FindArg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_ReadItem(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_WriteChars(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_dos_SetOwner(struct KaffeineContext *ctx, void *cpu);

/* Write the public fields of a native FIB into a complete 260-byte guest
   FIB. Its reserved bytes remain untouched. FALSE leaves guest memory intact. */
BOOL Kaffeine_FIBToGuest(struct KaffeineContext *ctx, ULONG guest,
                      const struct FileInfoBlock *native);
/* Ends the matches a program left open, which hold native locks. */
void Kaffeine_MatchCleanup(struct KaffeineContext *ctx);
ULONG Kaffeine_ExAllAllocControl(struct KaffeineContext *ctx);
BOOL Kaffeine_ExAllFreeControl(struct KaffeineContext *ctx, ULONG control);
ULONG Kaffeine_AllocStdPacket(struct KaffeineContext *ctx);
void Kaffeine_FreeStdPacket(struct KaffeineContext *ctx, ULONG dp);
void Kaffeine_ExAllCleanup(struct KaffeineContext *ctx);

/* The base calls that used to pass unchecked guest pointers from
   kaffeine_thunks.c, in LVO order. */
#define KAFFEINE_DOS_BASE_THUNKS \
    {  30, kaffeine_dos_Open }, \
    {  42, kaffeine_dos_Read }, \
    {  48, kaffeine_dos_Write }, \
    {  72, kaffeine_dos_DeleteFile }, \
    {  78, kaffeine_dos_Rename }, \
    {  84, kaffeine_dos_Lock }, \
    { 150, kaffeine_dos_LoadSeg }, \
    { 222, kaffeine_dos_Execute }, \
    { 342, kaffeine_dos_FPuts }, \
    { 504, kaffeine_dos_RunCommand }, \
    { 768, kaffeine_dos_NewLoadSeg }, \
    { 156, kaffeine_dos_UnLoadSeg }, \
    { 774, kaffeine_dos_SegmentUnsupported }, \
    { 780, kaffeine_dos_SegmentUnsupported }, \
    { 786, kaffeine_dos_SegmentUnsupported }, \
    { 1176, kaffeine_dos_SegmentUnsupported }, \
    { 930, kaffeine_dos_CliInitNewcli }, \
    { 936, kaffeine_dos_CliInitRun }, \
    { 948, kaffeine_dos_PutStr }, \
    { 954, kaffeine_dos_VPrintf }

/* MatchFirst(), MatchNext() and MatchEnd() through a native AnchorPath. */
#define KAFFEINE_DOS_MATCH_THUNKS \
    { 822, kaffeine_dos_MatchFirst }, \
    { 828, kaffeine_dos_MatchNext }, \
    { 834, kaffeine_dos_MatchEnd }

#define KAFFEINE_DOS_FORMAT_OBJECT_THUNKS \
    { 234, kaffeine_dos_FreeDosObject }, \
    { 348, kaffeine_dos_VFWritef }, \
    { 354, kaffeine_dos_VFPrintf }

#define KAFFEINE_DOS_DATA_THUNKS \
    { 102, kaffeine_dos_Examine }, \
    { 108, kaffeine_dos_ExNext }, \
    { 114, kaffeine_dos_Info }, \
    { 120, kaffeine_dos_CreateDir }, \
    { 144, kaffeine_dos_Exit }, \
    { 180, kaffeine_dos_SetComment }, \
    { 186, kaffeine_dos_SetProtection }, \
    { 192, kaffeine_dos_DateStamp }, \
    { 324, kaffeine_dos_FRead }, \
    { 330, kaffeine_dos_FWrite }, \
    { 336, kaffeine_dos_FGets }, \
    { 366, kaffeine_dos_SetVBuf }, \
    { 390, kaffeine_dos_ExamineFH }, \
    { 396, kaffeine_dos_SetFileDate }, \
    { 402, kaffeine_dos_NameFromLock }, \
    { 408, kaffeine_dos_NameFromFH }, \
    { 414, kaffeine_dos_SplitName }, \
    { 432, kaffeine_dos_ExAll }, \
    { 468, kaffeine_dos_Fault }, \
    { 474, kaffeine_dos_PrintFault }, \
    { 534, kaffeine_dos_GetArgStr }, \
    { 540, kaffeine_dos_SetArgStr }, \
    { 558, kaffeine_dos_SetCurrentDirName }, \
    { 564, kaffeine_dos_GetCurrentDirName }, \
    { 570, kaffeine_dos_SetProgramName }, \
    { 582, kaffeine_dos_SetPrompt }, \
    { 588, kaffeine_dos_GetPrompt }, \
    { 612, kaffeine_dos_AssignLock }, \
    { 618, kaffeine_dos_AssignLate }, \
    { 624, kaffeine_dos_AssignPath }, \
    { 630, kaffeine_dos_AssignAdd }, \
    { 636, kaffeine_dos_RemAssignList }, \
    { 708, kaffeine_dos_IsFileSystem }, \
    { 732, kaffeine_dos_AddBuffers }, \
    { 738, kaffeine_dos_CompareDates }, \
    { 744, kaffeine_dos_DateToStr }, \
    { 750, kaffeine_dos_StrToDate }, \
    { 804, kaffeine_dos_FindArg }, \
    { 810, kaffeine_dos_ReadItem }, \
    { 816, kaffeine_dos_StrToLong }, \
    { 840, kaffeine_dos_ParsePattern }, \
    { 846, kaffeine_dos_MatchPattern }, \
    { 870, kaffeine_dos_FilePart }, \
    { 876, kaffeine_dos_PathPart }, \
    { 882, kaffeine_dos_AddPart }, \
    { 900, kaffeine_dos_SetVar }, \
    { 906, kaffeine_dos_GetVar }, \
    { 912, kaffeine_dos_DeleteVar }, \
    { 942, kaffeine_dos_WriteChars }, \
    { 966, kaffeine_dos_ParsePatternNoCase }, \
    { 972, kaffeine_dos_MatchPatternNoCase }, \
    { 990, kaffeine_dos_ExAllEnd }, \
    { 996, kaffeine_dos_SetOwner }

/* Process launch, packet refusal and SystemTagList at -606 */
#define KAFFEINE_DOS_LAUNCH_THUNKS \
    { 138, kaffeine_dos_CreateProc }, \
    { 240, kaffeine_dos_DoPkt }, \
    { 246, kaffeine_dos_SendPkt }, \
    { 252, kaffeine_dos_WaitPkt }, \
    { 258, kaffeine_dos_ReplyPkt }, \
    { 264, kaffeine_dos_AbortPkt }, \
    { 498, kaffeine_dos_CreateNewProc }, \
    { 606, kaffeine_dos_SystemTagList }

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_THUNKS_DOS_H */
