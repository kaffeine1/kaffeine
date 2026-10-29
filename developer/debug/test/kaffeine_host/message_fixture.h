/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include "kaffeine_thunks_dos.h"
#include "kaffeine_shadow.h"
#include <vector>
#include <climits>
#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "kaffeine_intern.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_handlers.h"
#include "kaffeine_messages.h"
#include "kaffeine_locks.h"
#define KAFFEINE_LOCK_UNIT_RECORD_PUBLICATION
#include "lock_boundary_unit.h"
#undef KAFFEINE_LOCK_UNIT_RECORD_PUBLICATION
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_window.h"
extern "C" IPTR kaffeine_dos_Open(KaffeineContext *, void *);
extern "C" IPTR kaffeine_dos_Read(KaffeineContext *, void *);
extern "C" IPTR kaffeine_dos_Write(KaffeineContext *, void *);
extern "C" IPTR kaffeine_dos_SetVBuf(KaffeineContext *, void *);
extern "C" IPTR message_AllocDosObject(KaffeineContext *, void *);

static unsigned checks, failures, generated_calls;
static void check(bool condition, const char *label)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}
static std::map<void *, ULONG> native_blocks;
static int allocation_countdown = -1;
static unsigned alloc_calls, free_calls;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++alloc_calls;
    if (allocation_countdown == 0) return nullptr;
    if (allocation_countdown > 0) --allocation_countdown;
    void *p = std::malloc(size ? size : 1);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xD9, size); native_blocks[p] = size; }
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    assert(native_blocks.count(p) && native_blocks[p] == size);
    ++free_calls;
    native_blocks.erase(p);
    std::free(p);
}
extern "C" APTR AllocVec(ULONG size, ULONG flags) { return AllocMem(size, flags); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p, native_blocks.at(p)); }
static Process native_process;
static Task &native_task = native_process.pr_Task;
static BPTR close_frees;
static ULONG allocated_signals, pending_signals, signalled_mask;
static unsigned signals, freed_signals, reads, writes, closes, seeks, buffer_calls, scalar_calls;
static BPTR native_input, native_output, open_result, seen_handle, seen_lock;
static LONG read_length, read_override = INT_MIN, close_result = -1;
static void *read_buffer;
static std::vector<UBYTE> read_override_bytes;
static LONG write_override = INT_MIN;
static std::vector<UBYTE> written_bytes;
static unsigned native_packets;
static MsgPort *packet_handler;
static LONG packet_action, packet_error;
static SIPTR packet_result;
static std::array<SIPTR, 5> packet_args;
static InfoData packet_info;
static SIPTR ioerr;
static bool signal_failure;
static std::map<BPTR, size_t> positions;
static FileHandle *fixture_handles;
static size_t fixture_handle_count;
static std::array<UBYTE, 208> native_buffer;
/* Native handler ports and DevProcs. The fake handler keeps the names, the
   DateStamp and the FileInfoBlock that a packet brings, and fills the
   FileInfoBlock and InfoData that it takes. */
static std::array<MsgPort, 40> native_ports;
static MsgPort *console_task, *filesys_task;
static std::string packet_name1, packet_name2;
static struct DateStamp packet_stamp;
static FileInfoBlock packet_fib, packet_fib_in;
static unsigned unlocks, devproc_frees, deviceprocs;
static BPTR unlocked;
static MsgPort *ram_port() { return &native_ports[0]; }
extern "C" BOOL UnLock(BPTR lock) { ++unlocks; unlocked = lock; return -1; }
extern "C" MsgPort *GetConsoleTask(void) { return console_task; }
extern "C" MsgPort *SetConsoleTask(MsgPort *p) { MsgPort *o = console_task; console_task = p; return o; }
extern "C" MsgPort *GetFileSysTask(void) { return filesys_task; }
extern "C" MsgPort *SetFileSysTask(MsgPort *p) { MsgPort *o = filesys_task; filesys_task = p; return o; }
/* Real native list nodes let the production guard establish membership
   without dereferencing expired DevProc pointers. Lock values remain
   numeric collaborators here; test_locks covers the production mapping. */
static DosList dos_list_head;
static std::array<DosList, 5> named_nodes;
static std::array<DosList, 40> port_nodes;
static AssignList multi_tail;
static std::vector<DosList *> live_dos_nodes;
static unsigned dos_list_depth, dos_list_locks, dos_list_unlocks, deviceproc_advances;
static unsigned duplicate_locks, duplicate_depth, lock_releases;
static ULONG released_token, adopted_token_override, borrowed_token_override;
static BPTR adopted_lock, borrowed_lock;
static bool duplicate_failure, returned_dead_node, returned_removed_lock;
static void reset_dos_nodes()
{
    dos_list_head={}; named_nodes={}; port_nodes={}; multi_tail={}; live_dos_nodes.clear();
    named_nodes[0].dol_Type=DLT_DEVICE; // RAM:
    named_nodes[1].dol_Type=DLT_DIRECTORY; // WORK:
    named_nodes[1].dol_Lock=reinterpret_cast<BPTR>(0x1234560);
    named_nodes[2].dol_Type=DLT_DIRECTORY; // MULTI:
    named_nodes[2].dol_Lock=reinterpret_cast<BPTR>(0x1000);
    multi_tail.al_Lock=reinterpret_cast<BPTR>(0x2000);
    named_nodes[2].dol_misc.dol_assign.dol_List=&multi_tail;
    named_nodes[3].dol_Type=DLT_DIRECTORY; // BIG: native-width lock
    named_nodes[3].dol_Lock=reinterpret_cast<BPTR>(0x100000000ULL);
    named_nodes[4].dol_Type=DLT_NONBINDING; // owns its temporary lock
    for(auto &n:named_nodes) live_dos_nodes.push_back(&n);
    for(auto &n:port_nodes) { n.dol_Type=DLT_DEVICE;live_dos_nodes.push_back(&n); }
    dos_list_depth=dos_list_locks=dos_list_unlocks=deviceproc_advances=0;
    duplicate_locks=duplicate_depth=lock_releases=0;duplicate_failure=returned_dead_node=returned_removed_lock=false;
    released_token=adopted_token_override=borrowed_token_override=0;
    adopted_lock=borrowed_lock=nullptr;
}
extern "C" DosList *LockDosList(ULONG flags)
{
    assert(flags==(LDF_ALL|LDF_READ));++dos_list_locks;++dos_list_depth;
    return &dos_list_head;
}
extern "C" void UnLockDosList(ULONG flags)
{ assert(flags==(LDF_ALL|LDF_READ)&&dos_list_depth);++dos_list_unlocks;--dos_list_depth; }
extern "C" DosList *NextDosEntry(DosList *node,ULONG flags)
{
    assert(dos_list_depth&&flags==LDF_ALL);
    if(node==&dos_list_head)return live_dos_nodes.empty()?nullptr:live_dos_nodes.front();
    auto pos=std::find(live_dos_nodes.begin(),live_dos_nodes.end(),node);
    assert(pos!=live_dos_nodes.end());
    return ++pos==live_dos_nodes.end()?nullptr:*pos;
}
extern "C" BPTR DupLock(BPTR lock)
{
    ++duplicate_locks;duplicate_depth=dos_list_depth;
    if(duplicate_failure) { ioerr=ERROR_OBJECT_IN_USE;return nullptr; }
    return lock;
}
extern "C" ULONG Kaffeine_LockAdopt(KaffeineContext *,BPTR lock)
{ adopted_lock=lock;return lock?(adopted_token_override?adopted_token_override:static_cast<ULONG>(reinterpret_cast<IPTR>(lock))):0; }
extern "C" ULONG Kaffeine_LockBorrow(KaffeineContext *,BPTR lock,KaffeineLockScope,ULONG)
{ borrowed_lock=lock;return lock?(borrowed_token_override?borrowed_token_override:static_cast<ULONG>(reinterpret_cast<IPTR>(lock))):0; }
extern "C" BOOL Kaffeine_LockRelease(KaffeineContext *,ULONG token)
{ ++lock_releases;released_token=token;return TRUE; }
/* DeviceProc() is deliberately poisoned: the bridge must retain an assign
   through GetDeviceProc() and guard its live node before duplication. */
extern "C" MsgPort *DeviceProc(CONST_STRPTR) { std::abort(); }
extern "C" DevProc *GetDeviceProc(CONST_STRPTR name,DevProc *dp)
{
    ++deviceprocs;
    if(dp)
    {
        ++deviceproc_advances;
        assert(dos_list_depth&&dp->dvp_DevNode==&named_nodes[2]);
        if(dp->dvp_Lock==reinterpret_cast<BPTR>(0x1000))
        { dp->dvp_Lock=reinterpret_cast<BPTR>(0x2000);return dp; }
        FreeMem(dp,sizeof(*dp));ioerr=ERROR_NO_MORE_ENTRIES;return nullptr;
    }
    DosList *node=nullptr;MsgPort *port=ram_port();
    if(name&&!std::strcmp(name,"RAM:"))node=&named_nodes[0];
    if(name&&!std::strcmp(name,"WORK:"))node=&named_nodes[1];
    if(name&&!std::strcmp(name,"MULTI:"))node=&named_nodes[2];
    if(name&&!std::strcmp(name,"BIG:"))node=&named_nodes[3];
    if(name&&!std::strcmp(name,"UNBOUND:"))node=&named_nodes[4];
    if(name&&!std::strncmp(name,"PORT",4))
    { unsigned n=static_cast<unsigned>(std::atoi(name+4));node=&port_nodes.at(n);port=&native_ports.at(n); }
    if(!node) { ioerr=ERROR_DEVICE_NOT_MOUNTED;return nullptr; }
    dp=static_cast<DevProc *>(AllocMem(sizeof(DevProc),MEMF_CLEAR));
    if(!dp) { ioerr=ERROR_NO_FREE_STORE;return nullptr; }
    dp->dvp_Port=port;dp->dvp_DevNode=node;dp->dvp_Lock=node->dol_Lock;
    if(node==&named_nodes[2])dp->dvp_Flags=DVPF_ASSIGN;
    if(node==&named_nodes[4]) { dp->dvp_Lock=reinterpret_cast<BPTR>(0x3450);dp->dvp_Flags=DVPF_UNLOCK; }
    if(returned_dead_node)dp->dvp_DevNode=reinterpret_cast<DosList *>(0x7777);
    if(returned_removed_lock)dp->dvp_Lock=reinterpret_cast<BPTR>(0xDEAD);
    ioerr=0;
    return dp;
}
extern "C" void FreeDeviceProc(DevProc *dp)
{
    if(dp)
    { ++devproc_frees;if(dp->dvp_Flags&DVPF_UNLOCK)UnLock(dp->dvp_Lock);FreeMem(dp,sizeof(*dp)); }
}
static constexpr char contents[] = "ABCDEFGHIJKL";
extern "C" BYTE AllocSignal(LONG requested)
{
    if (signal_failure) return -1;
    for (LONG i = 0; i < 32; ++i)
        if ((requested < 0 || requested == i) && !(allocated_signals & (1U << i)))
        { allocated_signals |= 1U << i; return static_cast<BYTE>(i); }
    return -1;
}
extern "C" void FreeSignal(LONG bit) { ++freed_signals; allocated_signals &= ~(1U << bit); }
extern "C" Task *FindTask(CONST_STRPTR name) { assert(!name); return &native_task; }
extern "C" void Signal(Task *task, ULONG mask)
{ assert(task == &native_task); ++signals; signalled_mask = mask; pending_signals |= mask; }
extern "C" void RawPutChar(UBYTE) { std::abort(); }
extern "C" void PutMsg(MsgPort *, Message *) { std::abort(); }
extern "C" Message *GetMsg(MsgPort *) { std::abort(); }
extern "C" Message *WaitPort(MsgPort *) { std::abort(); }
extern "C" void ReplyMsg(Message *) { std::abort(); }
extern "C" ULONG Kaffeine_GetCLI(KaffeineContext *ctx) { return ctx->m68k_cli=0x6500; }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" void *Kaffeine_ShadowNative(KaffeineContext *, ULONG, UWORD) { return nullptr; }
extern "C" ULONG Kaffeine_ShadowGuest(KaffeineContext *, const void *, UWORD) { std::abort(); }
extern "C" SIPTR IoErr(void) { return ioerr; }
extern "C" SIPTR SetIoErr(SIPTR error) { SIPTR old = ioerr; ioerr = error; return old; }
extern "C" BPTR Input(void) { return native_input; }
extern "C" BPTR Output(void) { return native_output; }
extern "C" BPTR Open(CONST_STRPTR name, LONG mode)
{ assert(!std::strcmp(name, "fixture")); assert(mode == 1005); ++scalar_calls; return open_result; }
extern "C" LONG Close(BPTR handle)
{
    ++closes; seen_handle = handle;
    if (handle == close_frees) { close_frees=nullptr; FreeMem(BADDR(handle),sizeof(FileHandle)); }
    return close_result;
}
extern "C" LONG Read(BPTR handle, APTR buffer, LONG length)
{
    ++reads; seen_handle = handle; read_buffer = buffer; read_length = length;
    assert(handle && length >= 0);
    ioerr = read_override == -1 ? -123 : 909;
    if (read_override != INT_MIN)
    {
        if (read_override >= 0 && !read_override_bytes.empty())
        {
            assert(read_override <= length && read_override_bytes.size() == static_cast<size_t>(read_override));
            std::memcpy(buffer, read_override_bytes.data(), read_override_bytes.size());
            positions[handle] += read_override_bytes.size();
        }
        return read_override;
    }
    size_t &position = positions[handle];
    size_t count = std::min(static_cast<size_t>(length), sizeof(contents) - 1 - position);
    std::memcpy(buffer, contents + position, count);
    position += count;
    return static_cast<LONG>(count);
}
extern "C" LONG Write(BPTR handle, const void *buffer, LONG length)
{
    ++writes; seen_handle = handle; read_buffer = const_cast<void *>(buffer); read_length = length;
    assert(handle && length >= 0);
    written_bytes.assign(static_cast<const UBYTE *>(buffer), static_cast<const UBYTE *>(buffer) + length);
    ioerr = write_override == -1 ? 211 : 909;
    return write_override == INT_MIN ? length : write_override;
}
static LONG seek_position(BPTR handle, LONG position, LONG mode)
{
    seen_handle = handle;
    LONG old = static_cast<LONG>(positions[handle]);
    LONG next = position + (mode == OFFSET_BEGINNING ? 0 :
        mode == OFFSET_CURRENT ? old : static_cast<LONG>(sizeof(contents) - 1));
    if (next < 0 || next > static_cast<LONG>(sizeof(contents) - 1))
    { ioerr = ERROR_SEEK_ERROR; return -1; }
    positions[handle] = static_cast<size_t>(next);
    return old;
}
extern "C" LONG Seek(BPTR handle, LONG position, LONG mode)
{
    ++seeks;
    LONG old = seek_position(handle, position, mode);
    if (old != -1) ioerr = 909;
    return old;
}
extern "C" SIPTR DoPkt(MsgPort *handler, LONG action, SIPTR a1, SIPTR a2, SIPTR a3, SIPTR a4, SIPTR a5)
{
    ++native_packets; packet_handler=handler; packet_action=action; packet_args={a1,a2,a3,a4,a5};
    assert(handler);
    if (action == ACTION_DISK_INFO)
        *static_cast<InfoData *>(BADDR(reinterpret_cast<BPTR>(a1))) = packet_info;
    if (action == ACTION_INFO)
        *static_cast<InfoData *>(BADDR(reinterpret_cast<BPTR>(a2))) = packet_info;
    auto text = [](SIPTR a) { return std::string(reinterpret_cast<const char *>(a)); };
    if (action == ACTION_LOCATE_OBJECT || action == ACTION_CREATE_DIR || action == ACTION_DELETE_OBJECT)
        packet_name1 = text(a2);
    if (action == ACTION_RENAME_OBJECT) { packet_name1 = text(a2); packet_name2 = text(a4); }
    if (action == ACTION_SET_PROTECT || action == ACTION_SET_DATE) packet_name1 = text(a3);
    if (action == ACTION_SET_COMMENT) { packet_name1 = text(a3); packet_name2 = text(a4); }
    if (action == ACTION_SET_DATE) packet_stamp = *reinterpret_cast<struct DateStamp *>(a4);
    if (action == ACTION_EXAMINE_OBJECT || action == ACTION_EXAMINE_NEXT)
    {
        auto *fib = static_cast<FileInfoBlock *>(BADDR(reinterpret_cast<BPTR>(a2)));
        packet_fib_in = *fib;
        if (packet_result) *fib = packet_fib;
    }
    if (action == ACTION_SEEK)
    {
        /* The handler moves its own position, found by fh_Arg1, and sets
           res2 to 0 on success as the AROS handlers do. */
        FileHandle *fh = std::find_if(fixture_handles, fixture_handles + fixture_handle_count,
                                      [a1](const FileHandle &n) { return n.fh_Args == a1; });
        assert(fh != fixture_handles + fixture_handle_count);
        LONG old = seek_position(MKBADDR(fh), static_cast<LONG>(a2), static_cast<LONG>(a3));
        if (old != -1) ioerr = 0;
        return old;
    }
    ioerr=packet_error;
    return packet_result;
}
extern "C" ULONG Kaffeine_AllocRDArgs(KaffeineContext *) { std::abort(); }
extern "C" void Kaffeine_FreeRDArgs(KaffeineContext *, ULONG) { std::abort(); }
extern "C" ULONG Kaffeine_ExAllAllocControl(KaffeineContext *) { std::abort(); }
extern "C" BOOL Kaffeine_ExAllFreeControl(KaffeineContext *, ULONG) { std::abort(); }
extern "C" BPTR SelectInput(BPTR handle)
{ BPTR old = native_input; native_input = handle; ++scalar_calls; return old; }
extern "C" BPTR SelectOutput(BPTR handle)
{ BPTR old = native_output; native_output = handle; ++scalar_calls; return old; }
static LONG buffered(BPTR handle)
{
    ++buffer_calls; seen_handle = handle;
    auto *f = static_cast<FileHandle *>(BADDR(handle));
    f->fh_Buf = MKBADDR(native_buffer.data()); f->fh_OrigBuf = f->fh_Buf;
    f->fh_BufSize = native_buffer.size(); f->fh_Pos = 1; f->fh_End = 12;
    std::memcpy(native_buffer.data(), contents, 12);
    return 'A';
}
extern "C" LONG FGetC(BPTR handle) { return buffered(handle); }
extern "C" LONG FPutC(BPTR handle, LONG ch) { buffered(handle); return ch; }
extern "C" LONG UnGetC(BPTR handle, LONG ch) { buffered(handle); return ch; }
extern "C" LONG Flush(BPTR handle)
{ ++buffer_calls; seen_handle = handle; auto *f=static_cast<FileHandle *>(BADDR(handle)); f->fh_Pos=f->fh_End=0; return -1; }
extern "C" LONG SetVBuf(BPTR handle, STRPTR buffer, LONG mode, LONG size)
{
    assert(!buffer); ++buffer_calls; seen_handle = handle;
    auto *f=static_cast<FileHandle *>(BADDR(handle));
    f->fh_Flags = mode == 2 ? 8 : 0;
    if (size != -1) { f->fh_Buf = nullptr; f->fh_BufSize=0; f->fh_Pos=f->fh_End=-1; }
    return 0;
}
extern "C" LONG WaitForChar(BPTR f, LONG t) { ++scalar_calls; seen_handle=f; return t; }
extern "C" LONG IsInteractive(BPTR f) { ++scalar_calls; seen_handle=f; return -1; }
extern "C" BPTR DupLockFromFH(BPTR f) { ++scalar_calls; seen_handle=f; return reinterpret_cast<BPTR>(0x123456); }
extern "C" BPTR ParentOfFH(BPTR f) { ++scalar_calls; seen_handle=f; return reinterpret_cast<BPTR>(0x234567); }
extern "C" BPTR OpenFromLock(BPTR lock) { ++scalar_calls; seen_lock=lock; return open_result; }
extern "C" LONG SetMode(BPTR f,LONG n) { ++scalar_calls;seen_handle=f;return n; }
extern "C" LONG SetFileSize(BPTR f,LONG n,LONG mode) { ++scalar_calls;seen_handle=f;return n+mode; }
extern "C" LONG LockRecord(BPTR f,ULONG,ULONG,ULONG,ULONG) { ++scalar_calls;seen_handle=f;return -1; }
extern "C" LONG UnLockRecord(BPTR f,ULONG,ULONG) { ++scalar_calls;seen_handle=f;return -1; }
extern "C" LONG ChangeMode(ULONG,BPTR f,LONG n) { ++scalar_calls;seen_handle=f;return n; }

static ULONG block_length, block_number;
static BPTR execute_input, execute_output;
static std::string printed;
extern "C" LONG FRead(BPTR f,APTR block,ULONG length,ULONG number)
{ buffered(f);block_length=length;block_number=number;std::memcpy(block,"ABC",std::min<ULONG>(3,length*number));return 1; }
extern "C" LONG FWrite(BPTR f,const void *,ULONG length,ULONG number)
{ buffered(f);block_length=length;block_number=number;return 1; }
extern "C" STRPTR FGets(BPTR f,STRPTR b,ULONG size)
{ buffered(f);assert(size>=5);std::memcpy(b,"ABC\n",5);return b; }
extern "C" LONG FPuts(BPTR f,CONST_STRPTR s) { buffered(f);printed+=s;return 0; }
extern "C" BOOL NameFromFH(BPTR f,STRPTR b,LONG size)
{ ++scalar_calls;seen_handle=f;assert(size>=8);std::strcpy(b,"fixture");return -1; }
extern "C" APTR AllocDosObject(ULONG type,const TagItem *)
{ assert(type==DOS_FIB);return AllocMem(sizeof(FileInfoBlock),MEMF_CLEAR); }
extern "C" void FreeDosObject(ULONG type,APTR p) { assert(type==DOS_FIB);FreeMem(p,sizeof(FileInfoBlock)); }
extern "C" BOOL ExamineFH(BPTR f,FileInfoBlock *b) { ++scalar_calls;seen_handle=f;b->fib_Size=12;return -1; }
extern "C" BOOL Execute(CONST_STRPTR,BPTR input,BPTR output)
{ ++scalar_calls;execute_input=input;execute_output=output;return -1; }
extern "C" LONG VFWritef(BPTR f,CONST_STRPTR,const IPTR *) { buffered(f);return 0; }
extern "C" APTR m68k_to_native_fmt_stream(KaffeineContext *,const char *,ULONG,ULONG *size) { *size=0;return nullptr; }
extern "C" LONG WriteChars(CONST_STRPTR b,ULONG n) { buffered(native_output);printed.append(b,n);return n; }
extern "C" LONG PutStr(CONST_STRPTR s) { buffered(native_output);printed+=s;return 0; }

struct CPUState { ULONG d[8], a[8]; UWORD sr; ULONG pc; };
static ULONG get_d(void *s,int n) { return static_cast<CPUState *>(s)->d[n]; }
static ULONG get_a(void *s,int n) { return static_cast<CPUState *>(s)->a[n]; }
static void set_d(void *s,int n,ULONG v) { static_cast<CPUState *>(s)->d[n]=v; }
static void set_a(void *s,int n,ULONG v) { static_cast<CPUState *>(s)->a[n]=v; }
static UWORD get_sr(void *s) { return static_cast<CPUState *>(s)->sr; }
static void set_sr(void *s,UWORD v) { static_cast<CPUState *>(s)->sr=v; }
static ULONG get_pc(void *s) { return static_cast<CPUState *>(s)->pc; }
static void set_pc(void *s,ULONG v) { static_cast<CPUState *>(s)->pc=v; }
static const KaffeineCPUOps ops={get_d,get_a,set_d,set_a,get_sr,set_sr,get_pc,set_pc,nullptr,nullptr};
static IPTR poison(KaffeineContext *,void *) { ++generated_calls; std::abort(); }
static const M68KThunkEntry exec_manual[]={
    KAFFEINE_EXEC_COMPAT_THUNKS,
    {666,Kaffeine_CreateMsgPort},{672,Kaffeine_DeleteMsgPort},
    {372,Kaffeine_GetMsg},{378,Kaffeine_ReplyMsg},{384,Kaffeine_WaitPort}
};
static const M68KThunkEntry dos_manual[]={KAFFEINE_DOS_FILE_THUNKS,KAFFEINE_DOS_HANDLER_THUNKS,
    {228,message_AllocDosObject},{234,kaffeine_dos_FreeDosObject},
    {240,kaffeine_dos_DoPkt},{246,kaffeine_dos_SendPkt},{252,kaffeine_dos_WaitPkt},
    {258,kaffeine_dos_ReplyPkt},{264,kaffeine_dos_AbortPkt},
    {30,kaffeine_dos_Open},{42,kaffeine_dos_Read},{48,kaffeine_dos_Write},{366,kaffeine_dos_SetVBuf},
    {324,kaffeine_dos_FRead},{330,kaffeine_dos_FWrite},{336,kaffeine_dos_FGets},{342,kaffeine_dos_FPuts},
    {348,kaffeine_dos_VFWritef},{354,kaffeine_dos_VFPrintf},{390,kaffeine_dos_ExamineFH},{408,kaffeine_dos_NameFromFH},
    {222,kaffeine_dos_Execute},{942,kaffeine_dos_WriteChars},{948,kaffeine_dos_PutStr},{954,kaffeine_dos_VPrintf}};
static const M68KThunkEntry exec_poison[]={
    {366,poison},{372,poison},{378,poison},{384,poison},{666,poison},{672,poison}};
static const M68KThunkEntry dos_poison[]={
    {228,poison},{234,poison},{240,poison},{246,poison},{252,poison},{258,poison},{264,poison},
    {30,poison},{36,poison},{42,poison},{48,poison},{54,poison},{60,poison},{66,poison},
    {204,poison},{216,poison},{270,poison},{276,poison},{282,poison},{288,poison},
    {294,poison},{300,poison},{306,poison},{312,poison},{318,poison},{360,poison},
    {366,poison},{372,poison},{378,poison},{384,poison},{426,poison},{450,poison},{456,poison},
    {324,poison},{330,poison},{336,poison},{342,poison},{348,poison},{354,poison},{390,poison},{408,poison},
    {222,poison},{942,poison},{948,poison},{954,poison},
    {174,poison},{510,poison},{516,poison},{522,poison},{528,poison},{642,poison},{648,poison}};
static KaffeineContext *make_context()
{
    auto *c=static_cast<KaffeineContext *>(AllocMem(sizeof(KaffeineContext),MEMF_CLEAR));
    assert(c);
    c->mem_size=2*1024*1024;
    c->mem=static_cast<UBYTE *>(AllocMem(c->mem_size+KAFFEINE_MEM_GUARD,MEMF_CLEAR));
    assert(c->mem);
    c->heap_start=0x10000;c->heap_end=c->mem_size-0x10000;
    assert(Kaffeine_HeapInit(c));
    return c;
}
struct Fixture
{
    KaffeineContext &ctx;
    CPUState state={};
    KaffeineCPU cpu={&ops,&state};
    std::array<FileHandle,40> native={};
    ExecBase sysbase={};
    static constexpr ULONG exec_base=0x2000,dos_base=0x4000,task=0x6000,stack=0x7000;
    static constexpr ULONG port=0x8000,reply=0x8100,msg=0x9000,buffer=0xA001;
    Fixture():ctx(*make_context())
    {
        allocated_signals=pending_signals=signals=freed_signals=signalled_mask=0;
        reads=writes=closes=seeks=buffer_calls=scalar_calls=0;
        read_override=INT_MIN; read_override_bytes.clear(); signal_failure=false; generated_calls=0; ioerr=137;
        write_override=INT_MIN; written_bytes.clear(); native_packets=0; packet_handler=nullptr;
        packet_action=0; packet_result=-1; packet_error=37; packet_args={}; packet_info={};
        close_frees=nullptr;native_task.tc_Node.ln_Type=NT_PROCESS;native_process.pr_CES=nullptr;
        native_process.pr_ConsoleTask=native_process.pr_FileSystemTask=nullptr;
        native_ports={};console_task=&native_ports[1];filesys_task=&native_ports[2];
        packet_name1.clear();packet_name2.clear();packet_stamp={};packet_fib={};packet_fib_in={};
        unlocks=devproc_frees=deviceprocs=0;unlocked=nullptr;reset_dos_nodes();
        for (auto &n:native) { n.fh_Pos=n.fh_End=-1; n.fh_Type=reinterpret_cast<MsgPort *>(0x123456789ABCDEF0ULL); n.fh_Args=0xFEDCBA9876543210ULL+16*(&n-native.data()); }
        fixture_handles=native.data();fixture_handle_count=native.size();
        native_input=MKBADDR(&native[0]);native_output=MKBADDR(&native[1]);open_result=MKBADDR(&native[2]);
        positions.clear();
        ctx.m68k_sysbase=exec_base;ctx.guest_exec_task=task;ctx.exec_task=&native_task;
        m68k_write32(&ctx,exec_base+276,task);
        ctx.num_libs=2;
        auto &e=ctx.libs[0];e.m68k_addr=exec_base;e.jt_start=exec_base-3072;
        e.thunks=exec_manual;e.num_thunks=std::size(exec_manual);e.gen_thunks=exec_poison;e.num_gen_thunks=std::size(exec_poison);
        auto &d=ctx.libs[1];d.m68k_addr=dos_base;d.jt_start=dos_base-3072;
        d.thunks=dos_manual;d.num_thunks=std::size(dos_manual);d.gen_thunks=dos_poison;d.num_gen_thunks=std::size(dos_poison);
        std::strcpy(reinterpret_cast<char *>(ctx.mem+0x6200),"fixture");
        manual_port(port);manual_port(reply);manual_port(process_port(),PA_SIGNAL,task,SIGB_DOS);
        message(msg);w(stack,0x1234ABCD);
    }
    ~Fixture()
    {
        allocation_countdown=-1;
        Kaffeine_FileCleanup(&ctx);
        Kaffeine_HandlerCleanup(&ctx);
        check(!dos_list_depth && dos_list_locks==dos_list_unlocks,"native DosList guards are balanced");
        for (auto &p:ctx.io_ports)
            if(p.guest) { state.a[0]=p.guest;Kaffeine_DeleteMsgPort(&ctx,&cpu); }
        Kaffeine_DestroyContext(&ctx);
    }
    void initialize_shadows()
    {
        ctx.num_shadows=0;ctx.guest_exec_task=0;
        sysbase.LibNode.lib_Version=45;
        assert(shadow_init_execbase(&ctx,exec_base,&sysbase)==exec_base);
        assert(ctx.guest_exec_task && ctx.guest_exec_task==r(exec_base+276));
    }
    void w(ULONG a,ULONG v) { m68k_write32(&ctx,a,v); }
    ULONG r(ULONG a) { return m68k_read32(&ctx,a); }
    ULONG process_port() { return ctx.guest_exec_task+M68K_PR_MSGPORT; }
    void manual_port(ULONG p,UBYTE flags=2,ULONG owner=0,UBYTE bit=0)
    {
        std::memset(ctx.mem+p,0,34);ctx.mem[p+8]=4;ctx.mem[p+14]=flags;ctx.mem[p+15]=bit;
        w(p+16,owner);w(p+20,p+24);w(p+28,p+20);
    }
    void message(ULONG m,ULONG rp=0,UWORD len=68)
    {
        std::memset(ctx.mem+m,0xC7,len);w(m,0);w(m+4,0);ctx.mem[m+8]=0;ctx.mem[m+9]=static_cast<UBYTE>(-7);
        w(m+10,0);w(m+14,rp);m68k_write16(&ctx,m+18,len);
    }
    ULONG call(bool dos,ULONG lvo,ULONG a0=0xFFFF0000,ULONG a1=0xFFFF0002,
               ULONG d1=0xD0000001,ULONG d2=0xD0000002,ULONG d3=0xD0000003,ULONG d4=0xD0000004,
               ULONG d5=0xD0000005,ULONG d6=0xD0000006,ULONG d7=0xD0000007)
    {
        for(unsigned i=0;i<8;++i){state.d[i]=0xD0000000+i;state.a[i]=0xA0000000+i;}
        state.sr=0xA715;state.pc=0x12345678;state.a[7]=stack;
        state.a[0]=a0;state.a[1]=a1;state.d[1]=d1;state.d[2]=d2;state.d[3]=d3;state.d[4]=d4;
        state.d[5]=d5;state.d[6]=d6;state.d[7]=d7;
        w(stack,0x1234ABCD);auto before=state;
        ctx.running=TRUE;ctx.exit_code=0;
        KaffeineDispatchResult result={};
        assert(Kaffeine_DispatchLibraryCall(&ctx,&cpu,(dos?dos_base:exec_base)-lvo,&result));
        check(result.return_pc==0x1234ABCD && state.a[7]==stack+4,"dispatcher stack and return PC");
        check(state.sr==before.sr && state.pc==before.pc,"SR and PC preserved");
        for(unsigned i=1;i<8;++i)
            if (!(dos && lvo==240 && i==1)) check(state.d[i]==before.d[i],"non-result data register preserved");
        for(unsigned i=0;i<7;++i) check(state.a[i]==before.a[i],"address register preserved");
        check(!generated_calls,"unsafe generated fallback was not reached");
        return state.d[0];
    }
    ULONG dos(ULONG lvo,ULONG d1=0,ULONG d2=0,ULONG d3=0,ULONG d4=0,ULONG d5=0,ULONG d6=0,ULONG d7=0)
    { return call(true,lvo,0xFFFF0000,0xFFFF0002,d1,d2,d3,d4,d5,d6,d7); }
    ULONG exec(ULONG lvo,ULONG a0=0,ULONG a1=0) { return call(false,lvo,a0,a1); }
    std::vector<UBYTE> bytes() { return {ctx.mem,ctx.mem+ctx.mem_size+KAFFEINE_MEM_GUARD}; }
    void rejected(ULONG destination=port,ULONG message=msg)
    {
        auto before=bytes();unsigned nr=reads,nw=writes,ns=signals;auto positions_before=positions;
        check(exec(366,destination,message)==20 && !ctx.running && ctx.exit_code==20,"invalid PutMsg stops with rc20");
        check(bytes()==before,"rejected PutMsg preserves every guest and guard byte");
        check(reads==nr && writes==nw && signals==ns && positions==positions_before,"rejected PutMsg has no I/O or signal side effect");
    }
    ULONG open() { return dos(30,0x6200,1005); }
    void packet(ULONG token,LONG length=5,ULONG out=buffer,ULONG m=msg,ULONG rp=reply)
    {
        message(m,0,68);ctx.mem[m+8]=5;w(m+10,m+20);w(m+20,m);w(m+24,rp);w(m+28,82);
        w(m+32,0);w(m+36,0);w(m+40,r(token*4+36));w(m+44,out);w(m+48,static_cast<ULONG>(length));
        for(ULONG a=52;a<68;a+=4) w(m+a,0);
    }
};
