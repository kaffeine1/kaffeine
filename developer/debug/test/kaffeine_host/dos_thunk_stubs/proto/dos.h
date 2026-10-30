#pragma once
#include <exec/libraries.h>
#include <dos/bptr.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

struct InfoData;
struct DateTime;
struct CSource;
struct CommandLineInterface;

struct RootNode { ULONG rn_Flags; };
struct DosLibrary { struct Library lib; struct RootNode *dl_Root; };
struct TagItem;
struct Process;
struct MsgPort;
struct ExAllData;
struct ExAllControl;
struct DosPacket;
#ifndef __typedef_RAWARG
#define __typedef_RAWARG
typedef void *RAWARG;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct CommandLineInterface *Cli(void);
LONG Info(BPTR lock, struct InfoData *info);
BPTR CreateDir(CONST_STRPTR name);
LONG SetVBuf(BPTR file, STRPTR buffer, LONG type, LONG size);
BOOL SetFileDate(CONST_STRPTR name, const struct DateStamp *date);
LONG SplitName(CONST_STRPTR name, ULONG separator, STRPTR buf, LONG oldpos, LONG size);
LONG AssignLock(CONST_STRPTR name, BPTR lock);
BOOL AssignLate(CONST_STRPTR name, CONST_STRPTR path);
BOOL AssignPath(CONST_STRPTR name, CONST_STRPTR path);
BOOL AssignAdd(CONST_STRPTR name, BPTR lock);
LONG RemAssignList(CONST_STRPTR name, BPTR lock);
BOOL IsFileSystem(CONST_STRPTR name);
LONG AddBuffers(CONST_STRPTR name, LONG number);
LONG CompareDates(const struct DateStamp *a, const struct DateStamp *b);
BOOL DateToStr(struct DateTime *date);
BOOL StrToDate(struct DateTime *date);
LONG FindArg(CONST_STRPTR templ, CONST_STRPTR keyword);
LONG ReadItem(STRPTR buffer, LONG maxchars, struct CSource *input);
LONG WriteChars(CONST_STRPTR buffer, ULONG len);
BOOL SetOwner(STRPTR name, ULONG owner_info);
BPTR Input(void);
LONG UnGetC(BPTR file, LONG c);
void FreeDosObject(ULONG type, APTR object);
APTR AllocDosObject(ULONG type, const struct TagItem *tags);
SIPTR IoErr(void);
SIPTR SetIoErr(SIPTR code);
ULONG GetSegListInfo(BPTR, const struct TagItem *);
void UnLoadSeg(BPTR);
LONG StrToLong(CONST_STRPTR string, LONG *value);
struct DateStamp *DateStamp(struct DateStamp *date);
STRPTR FilePart(CONST_STRPTR path);
STRPTR PathPart(CONST_STRPTR path);
LONG FGetC(BPTR fh);
STRPTR FGets(BPTR fh, STRPTR buf, ULONG buflen);
LONG FRead(BPTR fh, APTR block, ULONG blocklen, ULONG number);
LONG FWrite(BPTR fh, const void *block, ULONG blocklen, ULONG number);
BOOL NameFromLock(BPTR lock, STRPTR buffer, LONG len);
BOOL NameFromFH(BPTR fh, STRPTR buffer, LONG len);
BOOL AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size);
BOOL SetVar(CONST_STRPTR name, CONST_STRPTR buffer, LONG size, LONG flags);
LONG GetVar(CONST_STRPTR name, STRPTR buffer, LONG size, LONG flags);
LONG DeleteVar(CONST_STRPTR name, ULONG flags);
BOOL Fault(LONG code, CONST_STRPTR header, STRPTR buffer, LONG len);
BOOL PrintFault(LONG code, CONST_STRPTR header);
LONG SetComment(CONST_STRPTR name, CONST_STRPTR comment);
LONG SetProtection(CONST_STRPTR name, ULONG protect);
LONG ParsePattern(CONST_STRPTR source, STRPTR dest, LONG size);
LONG ParsePatternNoCase(CONST_STRPTR source, STRPTR dest, LONG size);
BOOL MatchPattern(CONST_STRPTR pattern, CONST_STRPTR string);
BOOL MatchPatternNoCase(CONST_STRPTR pattern, CONST_STRPTR string);
BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib);
LONG Examine(BPTR lock, struct FileInfoBlock *fib);
LONG ExNext(BPTR lock, struct FileInfoBlock *fib);
BOOL ExAll(BPTR lock, struct ExAllData *buffer, LONG size, LONG data, struct ExAllControl *control);
void ExAllEnd(BPTR lock, struct ExAllData *buffer, LONG size, LONG data, struct ExAllControl *control);
SIPTR DoPkt(struct MsgPort *port, LONG action, SIPTR arg1, SIPTR arg2, SIPTR arg3, SIPTR arg4, SIPTR arg5);
void SendPkt(struct DosPacket *dp, struct MsgPort *port, struct MsgPort *replyport);
struct DosPacket *WaitPkt(void);
void ReplyPkt(struct DosPacket *dp, SIPTR res1, LONG res2);
void AbortPkt(struct MsgPort *port, struct DosPacket *pkt);
struct Process *CreateNewProc(const struct TagItem *tags);
struct MsgPort *CreateProc(CONST_STRPTR name, LONG pri, BPTR segList, LONG stackSize);
LONG SystemTagList(CONST_STRPTR command, const struct TagItem *tags);
STRPTR DosGetString(LONG code);
LONG VFPrintf(BPTR file, CONST_STRPTR format, RAWARG args);
LONG VFWritef(BPTR file, CONST_STRPTR format, const IPTR *args);
LONG Read(BPTR file, APTR buffer, LONG length);
LONG Write(BPTR file, const void *buffer, LONG length);
BPTR Open(CONST_STRPTR name, LONG accessMode);
BPTR Lock(CONST_STRPTR name, LONG accessMode);
BOOL DeleteFile(CONST_STRPTR name);
LONG Rename(CONST_STRPTR oldName, CONST_STRPTR newName);
LONG PutStr(CONST_STRPTR string);
LONG FPuts(BPTR file, CONST_STRPTR string);
LONG FPutC(BPTR file, LONG character);
BPTR Output(void);
BOOL Execute(CONST_STRPTR string, BPTR input, BPTR output);
LONG RunCommand(BPTR segList, ULONG stacksize, STRPTR argptr, ULONG argsize);
BPTR LoadSeg(CONST_STRPTR name);
BPTR NewLoadSeg(CONST_STRPTR file, const struct TagItem *tags);
struct AnchorPath;
LONG MatchFirst(CONST_STRPTR pat, struct AnchorPath *AP);
LONG MatchNext(struct AnchorPath *AP);
void MatchEnd(struct AnchorPath *AP);

#ifdef __cplusplus
}
#endif
