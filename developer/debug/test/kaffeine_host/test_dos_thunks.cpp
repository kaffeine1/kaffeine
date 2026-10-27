/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Execute the production DOS thunks through the real dispatcher. String
 * parsing and FGets() use the native AROS sources, and VFPrintf() and
 * VPrintf() the production RawDoFmt formatter; time, filesystem calls, the
 * characters behind FGetC() and the VFWritef() format-stream converter are
 * controlled collaborators.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include <dos/dostags.h>
#include <dos/dosasl.h>
#include <dos/var.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <dos/stdio.h>
#include <utility/tagitem.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks_dos.h"
#include "kaffeine_cli.h"
#include "kaffeine_offsets.h"

extern "C" {
#include "kaffeine_shadow.h"
}

/* Use the actual native string routines without recreating their parsers.
   The library macros become plain functions; wrappers below record calls. */
extern "C" {
#define DOS_INTERN_H
#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(type, fn, a1, bt, bn, o, lib) type fn(a1)
#define AROS_LH2I(type, fn, a1, a2, bt, bn, o, lib) type fn(a1, a2)
#define AROS_LH3(type, fn, a1, a2, a3, bt, bn, o, lib) type fn(a1, a2, a3)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define StrToLong native_StrToLong
#define FilePart native_FilePart
#define PathPart native_PathPart
#define FGets native_FGets
#include "../../../../rom/dos/strtolong.c"
#include "../../../../rom/dos/filepart.c"
#include "../../../../rom/dos/pathpart.c"
#include "../../../../rom/dos/fgets.c"
#undef StrToLong
#undef FilePart
#undef PathPart
#undef FGets
#undef AROS_LIBFUNC_INIT
#undef AROS_LIBFUNC_EXIT
#undef AROS_LH3
#undef AROS_LH2I
#undef AROS_LH1
#undef AROS_LHA
#undef DOS_INTERN_H
}

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *message)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0;
    ULONG pc = 0;
};

static ULONG get_d(void *state, int reg) { return static_cast<TestCPU *>(state)->d[reg]; }
static ULONG get_a(void *state, int reg) { return static_cast<TestCPU *>(state)->a[reg]; }
static void set_d(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->d[reg] = value; }
static void set_a(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->a[reg] = value; }
static UWORD get_sr(void *state) { return static_cast<TestCPU *>(state)->sr; }
static void set_sr(void *state, UWORD value) { static_cast<TestCPU *>(state)->sr = value; }
static ULONG get_pc(void *state) { return static_cast<TestCPU *>(state)->pc; }
static void set_pc(void *state, ULONG value) { static_cast<TestCPU *>(state)->pc = value; }
static void execute(void *) { std::abort(); }

static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

/* These are the same registrations included by the production DOS table. */
static const M68KThunkEntry dos_thunks[] =
{
    KAFFEINE_DOS_FORMAT_OBJECT_THUNKS,
    KAFFEINE_DOS_DATA_THUNKS,
    KAFFEINE_DOS_LAUNCH_THUNKS,
    KAFFEINE_DOS_BASE_THUNKS,
    KAFFEINE_DOS_MATCH_THUNKS
};

static IPTR unexpected_generated(KaffeineContext *, void *);

/* Literal LVOs, independent of the production registrations. If a manual
   entry moves or disappears, the dispatcher must not silently use a stub. */
static const M68KThunkEntry generated_thunks[] =
{
    { 114, unexpected_generated },
    { 120, unexpected_generated },
    { 144, unexpected_generated },
    { 366, unexpected_generated },
    { 396, unexpected_generated },
    { 414, unexpected_generated },
    { 534, unexpected_generated },
    { 540, unexpected_generated },
    { 558, unexpected_generated },
    { 564, unexpected_generated },
    { 570, unexpected_generated },
    { 582, unexpected_generated },
    { 588, unexpected_generated },
    { 612, unexpected_generated },
    { 618, unexpected_generated },
    { 624, unexpected_generated },
    { 630, unexpected_generated },
    { 636, unexpected_generated },
    { 708, unexpected_generated },
    { 732, unexpected_generated },
    { 738, unexpected_generated },
    { 744, unexpected_generated },
    { 750, unexpected_generated },
    { 804, unexpected_generated },
    { 810, unexpected_generated },
    { 942, unexpected_generated },
    { 996, unexpected_generated },

    { 102, unexpected_generated }, { 108, unexpected_generated },
    { 138, unexpected_generated }, { 192, unexpected_generated },
    { 234, unexpected_generated }, { 240, unexpected_generated },
    { 246, unexpected_generated }, { 252, unexpected_generated },
    { 258, unexpected_generated }, { 264, unexpected_generated },
    { 336, unexpected_generated },
    { 348, unexpected_generated }, { 354, unexpected_generated },
    { 390, unexpected_generated }, { 432, unexpected_generated },
    { 498, unexpected_generated }, { 606, unexpected_generated },
    { 816, unexpected_generated }, { 870, unexpected_generated },
    { 876, unexpected_generated }, { 990, unexpected_generated },
    { 324, unexpected_generated }, { 330, unexpected_generated },
    { 402, unexpected_generated }, { 408, unexpected_generated },
    { 882, unexpected_generated }, { 900, unexpected_generated },
    { 906, unexpected_generated }, { 912, unexpected_generated },
    { 468, unexpected_generated }, { 474, unexpected_generated },
    { 180, unexpected_generated }, { 186, unexpected_generated },
    { 840, unexpected_generated }, { 966, unexpected_generated },
    { 846, unexpected_generated }, { 972, unexpected_generated },

    { 30, unexpected_generated }, { 42, unexpected_generated },
    { 48, unexpected_generated }, { 72, unexpected_generated },
    { 78, unexpected_generated }, { 84, unexpected_generated },
    { 150, unexpected_generated }, { 222, unexpected_generated },
    { 342, unexpected_generated }, { 504, unexpected_generated },
    { 768, unexpected_generated }, { 930, unexpected_generated },
    { 936, unexpected_generated }, { 948, unexpected_generated },
    { 954, unexpected_generated },

    { 822, unexpected_generated }, { 828, unexpected_generated },
    { 834, unexpected_generated }
};

enum BufferCall
{
    read_blocks, write_blocks, name_lock, name_fh, add_part,
    set_var, get_var, delete_var, fault_text, print_fault,
    set_comment, set_protect, parse_case, parse_nocase,
    match_case, match_nocase, buffer_call_count
};

enum Batch2Call
{
    info_call, create_dir_call, setvbuf_call, setdate_call, split_call,
    assign_lock_call, assign_late_call, assign_path_call, assign_add_call,
    rem_assign_call, isfs_call, addbuffers_call, compare_call,
    datetostr_call, strtodate_call, findarg_call, readitem_call,
    writechars_call, setowner_call, batch2_count
};

struct Batch2Calls
{
    unsigned count[batch2_count] = {};
    LONG result = DOSTRUE;
    SIPTR error = 0x2468;
    BPTR handle = nullptr;
    STRPTR buffer = nullptr;
    LONG type = 0, size = 0;
    ULONG scalar = 0;
    CONST_STRPTR name = nullptr, extra = nullptr;
    std::string name_copy, extra_copy, bytes;
    InfoData info = { -3, 0x01020304, 82, 1234567, 765432, 512,
                      0x444F5301, reinterpret_cast<BPTR>(0x123456789ABCDEF0ULL), -1 };
    InfoData *info_output = nullptr;
    struct DateStamp first = {}, second = {};
    DateTime date = {};
    CSource source = {};
    bool source_null = false;
    bool cli_present = true, heap_fails = false;
    unsigned heap_calls = 0;
};

/* The base calls moved from kaffeine_thunks.c. Their stubs copy only bytes
   inside the current arena, so a comparison with the old unchecked thunks
   records the native calls without touching memory outside it. */
enum BaseCall
{
    open_call, lock_call, delete_call, rename_call, read_call, write_call,
    loadseg_call, newloadseg_call, execute_call, fputs_call, putstr_call,
    runcommand_call, fputc_call, base_count
};

struct BaseCalls
{
    unsigned count[base_count] = {};
    const UBYTE *mem = nullptr;
    ULONG mem_size = 0;
    BPTR handle = nullptr, input = nullptr, output = nullptr;
    const void *pointer = nullptr, *second = nullptr;
    std::string text, second_text;
    LONG length = 0, mode = 0;
    ULONG stack = 0, argsize = 0;
    const struct TagItem *tags = nullptr;
    LONG result = 0x1234;
    SIPTR error = 0x1357;
    std::string printed;
    BPTR printed_to = nullptr;
    size_t fputc_fails_at = static_cast<size_t>(-1);
    BPTR output_handle = reinterpret_cast<BPTR>(static_cast<IPTR>(0x0CCDDEE0));
    unsigned output_calls = 0;
};

/* The native matcher of the MatchFirst() tests: a base AChain for the
   directory, then one entry of names per call. APF_DODIR on the "dir"
   entry enters it with a child AChain and the inner names, and leaving it
   reports "dir" again with APF_DIDDIR, as AROS does. */
struct MatchCalls
{
    unsigned first = 0, next = 0, end = 0;
    bool ap_in_arena = false;
    const void *pattern_pointer = nullptr;
    std::string pattern;
    const AnchorPath *ap = nullptr;
    LONG strlen_in = -1, breakbits_in = 0, foundbreak_in = 0;
    int flags_in = -1;
    std::string buf_in;
    std::vector<std::string> names = { "a.txt", "dir", "b.txt" };
    std::vector<std::string> inner = { "c.txt" };
    size_t index = 0, inner_index = 0;
    bool build_fails = false;
    LONG signals = 0;
    std::vector<AChain *> live;
    unsigned achain_allocs = 0, achain_frees = 0, achain_fails_at = 0;
    ULONG achain_next = 0x8000;
    std::vector<ULONG> achain_live;
    bool achain_double_free = false;
};

struct Calls
{
    Batch2Calls dos2;
    BaseCalls base;
    MatchCalls match;

    KaffeineContext *converter_context = nullptr;
    const char *converter_format = nullptr;
    ULONG converter_guest_args = 0;
    unsigned converter_calls = 0;
    APTR stream = nullptr;
    ULONG stream_size = 0;
    unsigned writef_calls = 0;
    BPTR file = nullptr;
    const char *native_format = nullptr;
    bool native_args_match = false;
    unsigned free_mem_calls = 0;
    bool freed_converted_stream = false;
    unsigned alloc_mem_calls = 0;
    APTR last_alloc = nullptr;
    ULONG last_alloc_size = 0;
    bool freed_last_alloc = false;
    unsigned free_object_calls = 0;
    ULONG object_type = 0;
    APTR object = nullptr;
    unsigned free_rdargs_calls = 0;
    ULONG rdargs = 0;
    unsigned generated_calls = 0;
    unsigned strtolong_calls = 0;
    CONST_STRPTR string = nullptr;
    LONG *long_output = nullptr;
    unsigned datestamp_calls = 0;
    struct DateStamp date_value = { 0x01020304, 1234, 2999 };
    struct DateStamp *date_output = nullptr;
    bool date_succeeds = true;
    unsigned filepart_calls = 0;
    CONST_STRPTR path = nullptr;
    unsigned pathpart_calls = 0;
    CONST_STRPTR pathpart_path = nullptr;
    bool pathpart_override = false;
    STRPTR pathpart_result = nullptr;
    unsigned fgets_calls = 0;
    BPTR fgets_file = nullptr;
    STRPTR fgets_buf = nullptr;
    ULONG fgets_buflen = 0;
    STRPTR fgets_result_override = nullptr;
    unsigned fgetc_calls = 0;
    BPTR fgetc_file = nullptr;
    const char *input = "";
    size_t input_pos = 0;
    size_t input_error_at = static_cast<size_t>(-1);
    SIPTR input_error = 0;
    unsigned allocate_object_calls = 0;
    ULONG allocate_type = 0;
    bool allocate_without_tags = false;
    bool allocation_fails = false;
    struct FileInfoBlock *allocated_fib = nullptr;
    unsigned examinefh_calls = 0;
    struct FileInfoBlock fib_value = {};
    bool examine_received_temporary = false;
    BOOL examine_result = -1;
    SIPTR examine_ioerr = 0x1234;
    SIPTR ioerr = 0x4567;
    bool freed_temporary_fib = false;
    unsigned examine_calls = 0;
    unsigned exnext_calls = 0;
    BPTR lock = nullptr;
    IPTR exnext_key = 0;
    LONG exnext_type = 0;
    LONG lock_result = -1;
    SIPTR lock_ioerr = 0x2345;
    unsigned createnewproc_calls = 0;
    unsigned createproc_calls = 0;
    unsigned exall_calls = 0;
    unsigned exallend_calls = 0;
    unsigned dopkt_calls = 0;
    unsigned sendpkt_calls = 0;
    unsigned waitpkt_calls = 0;
    unsigned replypkt_calls = 0;
    unsigned abortpkt_calls = 0;
    unsigned system_taglist_calls = 0;
    CONST_STRPTR system_command = nullptr;
    bool system_had_tags = false;
    std::vector<TagItem> captured_tags;
    LONG system_result = 20;
    unsigned taglist_converter_calls = 0;
    ULONG taglist_guest = 0;
    bool taglist_converts = false;
    struct TagItem *converted_tags = nullptr;
    unsigned alloc_vec_calls = 0;
    bool alloc_vec_fails = false;
    struct TagItem *allocated_tags = nullptr;
    std::vector<void *> vec_live;
    ULONG last_vec_size = 0;
    bool vec_double_free = false;
    unsigned free_vec_calls = 0;
    bool freed_converted_tags = false;
    unsigned buffer_calls[buffer_call_count] = {};
    BPTR buffer_handle = nullptr;
    const void *buffer_input = nullptr;
    void *buffer_output = nullptr;
    CONST_STRPTR buffer_name = nullptr;
    CONST_STRPTR buffer_extra = nullptr;
    ULONG blocklen = 0, number = 0, buffer_flags = 0;
    LONG buffer_size = 0, buffer_code = 0;
    LONG buffer_result = 1;
    SIPTR buffer_ioerr = 0x3344;
    bool buffer_failure = false;
    bool variable_defined = true;
    bool fault_unknown = false;
    std::vector<UBYTE> payload;
    std::vector<UBYTE> variable = { 'a', 'b', 'c', 'd', 'e', 'f', '\n', 'X', 'Y', 'Z' };
};

static Calls calls;

static IPTR unexpected_generated(KaffeineContext *, void *)
{
    ++calls.generated_calls;
    return 0xBAD0BAD0;
}

extern "C" LONG StrToLong(CONST_STRPTR string, LONG *value)
{
    ++calls.strtolong_calls;
    calls.string = string;
    calls.long_output = value;
    return native_StrToLong(string, value);
}

extern "C" struct DateStamp *DateStamp(struct DateStamp *date)
{
    ++calls.datestamp_calls;
    calls.date_output = date;
    *date = calls.date_value;
    return calls.date_succeeds ? date : nullptr;
}

extern "C" STRPTR FilePart(CONST_STRPTR path)
{
    ++calls.filepart_calls;
    calls.path = path;
    return native_FilePart(path);
}

extern "C" STRPTR PathPart(CONST_STRPTR path)
{
    ++calls.pathpart_calls;
    calls.pathpart_path = path;
    return calls.pathpart_override ? calls.pathpart_result : native_PathPart(path);
}

/* The character source behind the native FGets(): the test's input string,
   then EOF with IoErr() 0, or an error code at a chosen position. */
extern "C" LONG FGetC(BPTR fh)
{
    ++calls.fgetc_calls;
    calls.fgetc_file = fh;
    if (calls.input_pos == calls.input_error_at)
    {
        calls.ioerr = calls.input_error;
        return -1;
    }
    if (!calls.input[calls.input_pos])
    {
        calls.ioerr = 0;
        return -1;
    }
    return static_cast<UBYTE>(calls.input[calls.input_pos++]);
}

extern "C" STRPTR FGets(BPTR fh, STRPTR buf, ULONG buflen)
{
    ++calls.fgets_calls;
    calls.fgets_file = fh;
    calls.fgets_buf = buf;
    calls.fgets_buflen = buflen;
    STRPTR result = native_FGets(fh, buf, buflen);
    return calls.fgets_result_override ? calls.fgets_result_override : result;
}

extern "C" APTR AllocDosObject(ULONG type, const struct TagItem *tags)
{
    ++calls.allocate_object_calls;
    calls.allocate_type = type;
    calls.allocate_without_tags = tags == nullptr;
    if (calls.allocation_fails)
        return nullptr;
    calls.allocated_fib = static_cast<struct FileInfoBlock *>(
        std::calloc(1, sizeof(struct FileInfoBlock)));
    if (!calls.allocated_fib)
        std::abort();
    return calls.allocated_fib;
}

extern "C" BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    ++calls.examinefh_calls;
    calls.file = file;
    calls.examine_received_temporary = fib == calls.allocated_fib;
    /* Write the native output even on failure. Guest memory must then be
       preserved rather than receive a partial filesystem result. */
    *fib = calls.fib_value;
    calls.ioerr = calls.examine_ioerr;
    return calls.examine_result;
}

/* Examine() and ExNext() fill the native FIB like ExamineFH() above. */
static LONG examine_native(BPTR lock, struct FileInfoBlock *fib)
{
    calls.lock = lock;
    calls.examine_received_temporary = fib == calls.allocated_fib;
    *fib = calls.fib_value;
    calls.ioerr = calls.lock_ioerr;
    return calls.lock_result;
}

extern "C" LONG Examine(BPTR lock, struct FileInfoBlock *fib)
{
    ++calls.examine_calls;
    return examine_native(lock, fib);
}

extern "C" LONG ExNext(BPTR lock, struct FileInfoBlock *fib)
{
    ++calls.exnext_calls;
    calls.exnext_key = fib->fib_DiskKey;
    calls.exnext_type = fib->fib_DirEntryType;
    return examine_native(lock, fib);
}

/* Only reached if a launch thunk handed guest code to native DOS */
extern "C" struct Process *CreateNewProc(const struct TagItem *)
{
    ++calls.createnewproc_calls;
    return reinterpret_cast<struct Process *>(&calls);
}

extern "C" struct MsgPort *CreateProc(CONST_STRPTR, LONG, BPTR, LONG)
{
    ++calls.createproc_calls;
    return reinterpret_cast<struct MsgPort *>(&calls);
}

extern "C" BOOL ExAll(BPTR, struct ExAllData *, LONG, LONG, struct ExAllControl *)
{
    ++calls.exall_calls;
    return -1;
}

extern "C" void ExAllEnd(BPTR, struct ExAllData *, LONG, LONG, struct ExAllControl *)
{
    ++calls.exallend_calls;
}

extern "C" SIPTR DoPkt(struct MsgPort *, LONG, SIPTR, SIPTR, SIPTR, SIPTR, SIPTR)
{
    ++calls.dopkt_calls;
    return -1;
}

extern "C" void SendPkt(struct DosPacket *, struct MsgPort *, struct MsgPort *)
{
    ++calls.sendpkt_calls;
}

extern "C" struct DosPacket *WaitPkt(void)
{
    ++calls.waitpkt_calls;
    return reinterpret_cast<struct DosPacket *>(&calls);
}

extern "C" void ReplyPkt(struct DosPacket *, SIPTR, LONG)
{
    ++calls.replypkt_calls;
}

extern "C" void AbortPkt(struct MsgPort *, struct DosPacket *)
{
    ++calls.abortpkt_calls;
}

/* SystemTagList must not use the generic heuristic tag converter.
   NewLoadSeg() still does, and its tests ask for a converted list. */
extern "C" struct TagItem *m68k_to_native_taglist(KaffeineContext *, ULONG tags)
{
    ++calls.taglist_converter_calls;
    calls.taglist_guest = tags;
    if (!calls.taglist_converts || !tags)
        return nullptr;
    calls.converted_tags = static_cast<struct TagItem *>(AllocVec(sizeof(struct TagItem), 0));
    return calls.converted_tags;
}

extern "C" APTR AllocVec(ULONG byteSize, ULONG)
{
    ++calls.alloc_vec_calls;
    if (calls.alloc_vec_fails)
        return nullptr;
    calls.allocated_tags = static_cast<struct TagItem *>(std::calloc(1, byteSize));
    if (!calls.allocated_tags)
        std::abort();
    calls.vec_live.push_back(calls.allocated_tags);
    calls.last_vec_size = byteSize;
    return calls.allocated_tags;
}

extern "C" void FreeVec(APTR memory)
{
    ++calls.free_vec_calls;
    calls.freed_converted_tags = memory && memory == calls.allocated_tags;
    if (memory)
    {
        bool live = false;
        for (size_t i = 0; i < calls.vec_live.size() && !live; ++i)
            if (calls.vec_live[i] == memory)
            {
                calls.vec_live.erase(calls.vec_live.begin() + static_cast<long>(i));
                live = true;
            }
        calls.vec_double_free |= !live;
    }
    std::free(memory);
    calls.allocated_tags = nullptr;
}

extern "C" LONG SystemTagList(CONST_STRPTR command, const struct TagItem *tags)
{
    ++calls.system_taglist_calls;
    calls.system_command = command;
    calls.system_had_tags = tags != nullptr;
    calls.captured_tags.clear();
    if (tags)
    {
        for (const struct TagItem *ti = tags;; ++ti)
        {
            calls.captured_tags.push_back(*ti);
            if (ti->ti_Tag == TAG_DONE)
                break;
        }
    }
    return calls.system_result;
}

extern "C" SIPTR IoErr(void) { return calls.ioerr; }

extern "C" SIPTR SetIoErr(SIPTR code)
{
    SIPTR previous = calls.ioerr;
    calls.ioerr = code;
    return previous;
}

extern "C" BOOL dos_native_AddPart(STRPTR, CONST_STRPTR, ULONG);
extern "C" BOOL dos_native_Fault(LONG, CONST_STRPTR, STRPTR, LONG);
extern "C" LONG dos_native_ParsePattern(CONST_STRPTR, STRPTR, LONG, BOOL);
extern "C" BOOL dos_native_MatchPattern(CONST_STRPTR, CONST_STRPTR, BOOL);

extern "C" LONG FRead(BPTR fh, APTR block, ULONG blocklen, ULONG number)
{
    ++calls.buffer_calls[read_blocks];
    calls.buffer_handle = fh;
    calls.buffer_output = block;
    calls.blocklen = blocklen;
    calls.number = number;
    if (!calls.buffer_failure)
        std::memset(block, 0x6D, blocklen * number);
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? -1 : calls.buffer_result;
}

extern "C" LONG FWrite(BPTR fh, const void *block, ULONG blocklen, ULONG number)
{
    ++calls.buffer_calls[write_blocks];
    calls.buffer_handle = fh;
    calls.buffer_input = block;
    calls.blocklen = blocklen;
    calls.number = number;
    const auto *p = static_cast<const UBYTE *>(block);
    calls.payload.assign(p, p + blocklen * number);
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? -1 : calls.buffer_result;
}

static BOOL name_from(BufferCall call, BPTR handle, STRPTR buffer, LONG len)
{
    ++calls.buffer_calls[call];
    calls.buffer_handle = handle;
    calls.buffer_output = buffer;
    calls.buffer_size = len;
    const char *name = handle ? "RAM:test" : "SYS:";
    if (calls.buffer_failure || std::strlen(name) >= static_cast<ULONG>(len))
    {
        calls.ioerr = ERROR_LINE_TOO_LONG;
        return 0;
    }
    std::strcpy(buffer, name);
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_result;
}

extern "C" BOOL NameFromLock(BPTR lock, STRPTR buffer, LONG len)
{
    return name_from(name_lock, lock, buffer, len);
}

extern "C" BOOL NameFromFH(BPTR fh, STRPTR buffer, LONG len)
{
    return name_from(name_fh, fh, buffer, len);
}

extern "C" BOOL AddPart(STRPTR dir, CONST_STRPTR file, ULONG size)
{
    ++calls.buffer_calls[add_part];
    calls.buffer_output = dir;
    calls.buffer_extra = file;
    calls.buffer_size = static_cast<LONG>(size);
    return dos_native_AddPart(dir, file, size);
}

extern "C" BOOL SetVar(CONST_STRPTR name, CONST_STRPTR buffer, LONG size, LONG flags)
{
    ++calls.buffer_calls[set_var];
    calls.buffer_name = name;
    calls.buffer_input = buffer;
    calls.buffer_size = size;
    calls.buffer_flags = flags;
    if (!calls.buffer_failure)
    {
        size_t length = size == -1 ? std::strlen(buffer) : static_cast<ULONG>(size);
        const auto *p = reinterpret_cast<const UBYTE *>(buffer);
        calls.variable.assign(p, p + length);
    }
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? 0 : calls.buffer_result;
}

/* Controlled variable contents with the native truncation and newline
   rules. Keep its text lookahead observable under ASan at the arena end. */
extern "C" LONG GetVar(CONST_STRPTR name, STRPTR buffer, LONG size, LONG flags)
{
    ++calls.buffer_calls[get_var];
    calls.buffer_name = name;
    calls.buffer_output = buffer;
    calls.buffer_size = size;
    calls.buffer_flags = flags;
    if (!calls.variable_defined || calls.buffer_failure)
    {
        calls.ioerr = ERROR_OBJECT_NOT_FOUND;
        return -1;
    }
    LONG count = static_cast<LONG>(calls.variable.size());
    if (count > size)
        count = size;
    if (count)
        std::memcpy(buffer, calls.variable.data(), count);
    LONG written = count;
    if (!(flags & GVF_BINARY_VAR))
    {
        written = 0;
        while (buffer[written] != '\n' && written < count)
            ++written;
        if (written == size)
            --written;
        buffer[written] = 0;
    }
    else if (!(flags & GVF_DONT_NULL_TERM))
    {
        if (written == size)
            --written;
        buffer[written] = 0;
    }
    calls.ioerr = calls.variable.size();
    return written;
}

extern "C" LONG DeleteVar(CONST_STRPTR name, ULONG flags)
{
    ++calls.buffer_calls[delete_var];
    calls.buffer_name = name;
    calls.buffer_flags = flags;
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? 0 : calls.buffer_result;
}

extern "C" STRPTR DosGetString(LONG)
{
    static char message[] = "controlled DOS error";
    return calls.fault_unknown ? nullptr : message;
}

extern "C" BOOL Fault(LONG code, CONST_STRPTR header, STRPTR buffer, LONG len)
{
    ++calls.buffer_calls[fault_text];
    calls.buffer_code = code;
    calls.buffer_extra = header;
    calls.buffer_output = buffer;
    calls.buffer_size = len;
    return dos_native_Fault(code, header, buffer, len);
}

extern "C" BOOL PrintFault(LONG code, CONST_STRPTR header)
{
    ++calls.buffer_calls[print_fault];
    calls.buffer_code = code;
    calls.buffer_extra = header;
    calls.ioerr = code;
    return calls.buffer_failure ? 0 : calls.buffer_result;
}

extern "C" LONG SetComment(CONST_STRPTR name, CONST_STRPTR comment)
{
    ++calls.buffer_calls[set_comment];
    calls.buffer_name = name;
    calls.buffer_extra = comment;
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? 0 : calls.buffer_result;
}

extern "C" LONG SetProtection(CONST_STRPTR name, ULONG protect)
{
    ++calls.buffer_calls[set_protect];
    calls.buffer_name = name;
    calls.buffer_flags = protect;
    calls.ioerr = calls.buffer_ioerr;
    return calls.buffer_failure ? 0 : calls.buffer_result;
}

static LONG parse(BufferCall call, CONST_STRPTR source, STRPTR dest, LONG size)
{
    ++calls.buffer_calls[call];
    calls.buffer_name = source;
    calls.buffer_output = dest;
    calls.buffer_size = size;
    return dos_native_ParsePattern(source, dest, size, call == parse_case);
}
extern "C" LONG ParsePattern(CONST_STRPTR source, STRPTR dest, LONG size)
{
    return parse(parse_case, source, dest, size);
}
extern "C" LONG ParsePatternNoCase(CONST_STRPTR source, STRPTR dest, LONG size)
{
    return parse(parse_nocase, source, dest, size);
}
static BOOL match(BufferCall call, CONST_STRPTR pattern, CONST_STRPTR string)
{
    ++calls.buffer_calls[call];
    calls.buffer_name = pattern;
    calls.buffer_extra = string;
    return dos_native_MatchPattern(pattern, string, call == match_case);
}
extern "C" BOOL MatchPattern(CONST_STRPTR pattern, CONST_STRPTR string)
{
    return match(match_case, pattern, string);
}
extern "C" BOOL MatchPatternNoCase(CONST_STRPTR pattern, CONST_STRPTR string)
{
    return match(match_nocase, pattern, string);
}
extern "C" ULONG ToUpper(ULONG c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}
extern "C" APTR AllocMem(ULONG size, ULONG)
{
    ++calls.alloc_mem_calls;
    calls.last_alloc = std::calloc(1, size);
    calls.last_alloc_size = size;
    return calls.last_alloc;
}

extern "C" APTR m68k_to_native_fmt_stream(KaffeineContext *ctx,
    const char *format, ULONG guest_args, ULONG *size)
{
    ++calls.converter_calls;
    calls.converter_context = ctx;
    calls.converter_format = format;
    calls.converter_guest_args = guest_args;
    *size = 0;
    if (!guest_args)
        return nullptr;

    /* A sentinel buffer checks handoff and ownership without reproducing
       the format-stream converter in the test. */
    calls.stream_size = 24;
    calls.stream = std::calloc(1, calls.stream_size);
    if (!calls.stream)
        std::abort();
    *size = calls.stream_size;
    return calls.stream;
}

extern "C" LONG VFWritef(BPTR file, CONST_STRPTR format, const IPTR *args)
{
    ++calls.writef_calls;
    calls.file = file;
    calls.native_format = format;
    calls.native_args_match = args == calls.stream;
    return 17;
}

extern "C" void FreeMem(APTR memory, ULONG size)
{
    ++calls.free_mem_calls;
    calls.freed_converted_stream = memory == calls.stream && size == calls.stream_size;
    calls.freed_last_alloc = memory && memory == calls.last_alloc && size == calls.last_alloc_size;
    std::free(memory);
    if (memory == calls.stream)
        calls.stream = nullptr;
    if (memory == calls.last_alloc)
        calls.last_alloc = nullptr;
}

static bool base_inside(const void *pointer, size_t size)
{
    uintptr_t p = reinterpret_cast<uintptr_t>(pointer);
    uintptr_t m = reinterpret_cast<uintptr_t>(calls.base.mem);
    return calls.base.mem && p >= m && p - m < calls.base.mem_size &&
        size <= calls.base.mem_size - (p - m);
}

/* A bounded copy: a string that leaves the arena is marked, not read. */
static std::string base_string(const void *pointer)
{
    if (!base_inside(pointer, 1))
        return "<outside>";
    size_t room = calls.base.mem_size -
        (reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(calls.base.mem));
    const char *s = static_cast<const char *>(pointer);
    size_t length = strnlen(s, room);
    return length == room ? "<unterminated>" : std::string(s, length);
}

static LONG base_record(BaseCall call, const void *pointer, const void *second = nullptr)
{
    ++calls.base.count[call];
    calls.base.pointer = pointer;
    calls.base.text = pointer ? base_string(pointer) : "";
    calls.base.second = second;
    calls.base.second_text = second ? base_string(second) : "";
    calls.ioerr = calls.base.error;
    return calls.base.result;
}

static BPTR base_handle(ULONG value)
{
    return reinterpret_cast<BPTR>(static_cast<IPTR>(value));
}

extern "C" BPTR Open(CONST_STRPTR name, LONG mode)
{
    calls.base.mode = mode;
    return base_handle(static_cast<ULONG>(base_record(open_call, name)));
}

extern "C" BPTR Lock(CONST_STRPTR name, LONG mode)
{
    calls.base.mode = mode;
    return base_handle(static_cast<ULONG>(base_record(lock_call, name)));
}

extern "C" BOOL DeleteFile(CONST_STRPTR name)
{
    return base_record(delete_call, name);
}

extern "C" LONG Rename(CONST_STRPTR from, CONST_STRPTR to)
{
    return base_record(rename_call, from, to);
}

extern "C" LONG Read(BPTR file, APTR buffer, LONG length)
{
    LONG result = base_record(read_call, buffer);
    calls.base.handle = file;
    calls.base.length = length;
    calls.base.text.clear();
    if (length > 0 && base_inside(buffer, static_cast<size_t>(length)))
        std::memset(buffer, 0x5C, static_cast<size_t>(length));
    return result;
}

extern "C" LONG Write(BPTR file, const void *buffer, LONG length)
{
    LONG result = base_record(write_call, buffer);
    calls.base.handle = file;
    calls.base.length = length;
    if (length > 0 && base_inside(buffer, static_cast<size_t>(length)))
        calls.base.text.assign(static_cast<const char *>(buffer), static_cast<size_t>(length));
    else
        calls.base.text = "<outside>";
    return result;
}

extern "C" BPTR LoadSeg(CONST_STRPTR name)
{
    return base_handle(static_cast<ULONG>(base_record(loadseg_call, name)));
}

extern "C" BPTR NewLoadSeg(CONST_STRPTR name, const struct TagItem *tags)
{
    calls.base.tags = tags;
    return base_handle(static_cast<ULONG>(base_record(newloadseg_call, name)));
}

extern "C" BOOL Execute(CONST_STRPTR command, BPTR input, BPTR output)
{
    calls.base.input = input;
    calls.base.output = output;
    return base_record(execute_call, command);
}

extern "C" LONG FPuts(BPTR file, CONST_STRPTR string)
{
    calls.base.handle = file;
    return base_record(fputs_call, string);
}

extern "C" LONG PutStr(CONST_STRPTR string)
{
    return base_record(putstr_call, string);
}

extern "C" LONG RunCommand(BPTR seglist, ULONG stack, STRPTR args, ULONG size)
{
    calls.base.handle = seglist;
    calls.base.stack = stack;
    calls.base.argsize = size;
    return base_record(runcommand_call, args);
}

/* As FWriteChars() in AROS, a NULL handle fails without setting IoErr. */
extern "C" LONG FPutC(BPTR file, LONG character)
{
    ++calls.base.count[fputc_call];
    calls.base.printed_to = file;
    if (!file || calls.base.printed.size() >= calls.base.fputc_fails_at)
        return EOF;
    calls.base.printed.push_back(static_cast<char>(character));
    return character;
}

extern "C" BPTR Output(void)
{
    ++calls.base.output_calls;
    return calls.base.output_handle;
}

/* The DOS formatter writes into a native buffer, never to the serial port. */
extern "C" void RawPutChar(UBYTE)
{
    std::abort();
}

static AChain *match_chain(ULONG lock)
{
    auto *ac = static_cast<AChain *>(std::calloc(1, sizeof(AChain) + 8));
    if (!ac)
        std::abort();
    ac->an_Lock = base_handle(lock);
    ac->an_Flags = DDF_PatternBit | DDF_ExaminedBit;
    calls.match.live.push_back(ac);
    return ac;
}

static void match_free(AChain *ac)
{
    auto &live = calls.match.live;
    for (size_t i = 0; i < live.size(); ++i)
        if (live[i] == ac)
        {
            live.erase(live.begin() + static_cast<long>(i));
            break;
        }
    std::free(ac);
}

/* One directory entry, with the path that AddPart() builds, or
   ERROR_LINE_TOO_LONG and an empty buffer when it does not fit. */
static LONG match_result(AnchorPath *ap, const std::string &name, LONG type)
{
    AChain *ac = ap->ap_Current;

    std::memset(&ac->an_Info, 0, sizeof(ac->an_Info));
    std::memcpy(ac->an_Info.fib_FileName, name.c_str(), name.size() + 1);
    ac->an_Info.fib_DirEntryType = type;
    ac->an_Info.fib_Size = static_cast<LONG>(name.size()) * 100;
    ap->ap_Info = ac->an_Info;
    if (ap->ap_Strlen)
    {
        std::string path = (ac == ap->ap_Base ? "RAM:dir/" : "RAM:dir/dir/") + name;
        ap->ap_Buf[0] = 0;
        if (path.size() >= static_cast<size_t>(ap->ap_Strlen))
            return ERROR_LINE_TOO_LONG;
        std::memcpy(ap->ap_Buf, path.c_str(), path.size() + 1);
    }
    return 0;
}

static LONG match_step(AnchorPath *ap)
{
    auto &m = calls.match;
    AChain *base = ap->ap_Base;

    ap->ap_Flags &= ~APF_DIDDIR;
    if ((ap->ap_Flags & APF_DODIR) && ap->ap_Current == base &&
        base->an_Info.fib_DirEntryType > 0)
    {
        AChain *child = match_chain(0x00C3D4E0);
        child->an_Parent = base;
        base->an_Child = child;
        ap->ap_Current = child;
        m.inner_index = 0;
    }
    ap->ap_Flags &= ~APF_DODIR;
    if (ap->ap_BreakBits & m.signals)
    {
        ap->ap_FoundBreak = ap->ap_BreakBits & m.signals;
        return ERROR_BREAK;
    }
    if (ap->ap_Current != base)
    {
        if (m.inner_index < m.inner.size())
            return match_result(ap, m.inner[m.inner_index++], -3);
        match_free(ap->ap_Current);
        base->an_Child = nullptr;
        ap->ap_Current = base;
        ap->ap_Flags |= APF_DIDDIR;
        return match_result(ap, "dir", 2);
    }
    if (m.index < m.names.size())
    {
        const std::string &name = m.names[m.index++];
        return match_result(ap, name, name == "dir" ? 2 : -3);
    }
    return ERROR_NO_MORE_ENTRIES;
}

/* The old thunks passed the 68k AnchorPath itself, or NULL for an address
   outside guest memory: record it, never touch it. */
static bool match_record(AnchorPath *ap)
{
    auto &m = calls.match;

    m.ap = ap;
    if (!ap || base_inside(ap, 1))
    {
        m.ap_in_arena = true;
        return false;
    }
    m.strlen_in = ap->ap_Strlen;
    m.breakbits_in = ap->ap_BreakBits;
    m.foundbreak_in = ap->ap_FoundBreak;
    m.flags_in = static_cast<UBYTE>(ap->ap_Flags);
    m.buf_in.assign(reinterpret_cast<const char *>(ap->ap_Buf),
                    ap->ap_Strlen > 0 ? static_cast<size_t>(ap->ap_Strlen) : 0);
    return true;
}

extern "C" LONG MatchFirst(CONST_STRPTR pat, AnchorPath *ap)
{
    auto &m = calls.match;
    LONG error = ERROR_OBJECT_NOT_FOUND;

    ++m.first;
    m.pattern_pointer = pat;
    m.pattern = pat ? base_string(pat) : "";
    if (!match_record(ap))
        return ERROR_BAD_NUMBER;
    ap->ap_Flags = 0;
    ap->ap_Base = ap->ap_Current = nullptr;
    if (!m.build_fails)
    {
        ap->ap_Base = ap->ap_Current = match_chain(0x00A1B2C0);
        ap->ap_Flags |= APF_ITSWILD;
        error = match_step(ap);
    }
    calls.ioerr = error;
    return error;
}

extern "C" LONG MatchNext(AnchorPath *ap)
{
    LONG error;

    ++calls.match.next;
    if (!match_record(ap))
        return ERROR_BAD_NUMBER;
    error = match_step(ap);
    calls.ioerr = error;
    return error;
}

extern "C" void MatchEnd(AnchorPath *ap)
{
    ++calls.match.end;
    calls.match.ap = ap;
    if (!ap || base_inside(ap, 1))
    {
        calls.match.ap_in_arena = true;
        return;
    }
    for (AChain *ac = ap->ap_Base; ac;)
    {
        AChain *child = ac->an_Child;
        match_free(ac);
        ac = child;
    }
    ap->ap_Base = ap->ap_Current = nullptr;
}

/* Bridge-owned guest blocks from 0x8000 on, cleared like MEMF_CLEAR. */
extern "C" ULONG Kaffeine_AChainAlloc(KaffeineContext *ctx, ULONG size)
{
    auto &m = calls.match;
    ULONG addr = m.achain_next;

    if (++m.achain_allocs == m.achain_fails_at || size > ctx->mem_size ||
        addr > ctx->mem_size - size)
        return 0;
    m.achain_next = addr + ((size + 7) & ~7U);
    std::memset(ctx->mem + addr, 0, size);
    m.achain_live.push_back(addr);
    return addr;
}

extern "C" void Kaffeine_AChainFree(KaffeineContext *, ULONG addr)
{
    auto &m = calls.match;

    if (!addr)
        return;
    ++m.achain_frees;
    for (size_t i = 0; i < m.achain_live.size(); ++i)
        if (m.achain_live[i] == addr)
        {
            m.achain_live.erase(m.achain_live.begin() + static_cast<long>(i));
            return;
        }
    m.achain_double_free = true;
}

extern "C" void FreeDosObject(ULONG type, APTR object)
{
    ++calls.free_object_calls;
    calls.object_type = type;
    calls.object = object;
    if (object && object == calls.allocated_fib)
    {
        calls.freed_temporary_fib = true;
        std::free(calls.allocated_fib);
        calls.allocated_fib = nullptr;
        /* Make error preservation observable across DOS cleanup. */
        calls.ioerr = 0x7777;
    }
}

/* kaffeine_readargs.c frees the 68k RDArgs of AllocDosObject() */
extern "C" void Kaffeine_FreeRDArgs(KaffeineContext *, ULONG rdargs)
{
    ++calls.free_rdargs_calls;
    calls.rdargs = rdargs;
}

extern "C" LONG dos_native_CompareDates(const struct DateStamp *, const struct DateStamp *);
extern "C" BOOL dos_native_DateToStr(DateTime *);
extern "C" BOOL dos_native_StrToDate(DateTime *);
extern "C" LONG dos_native_FindArg(CONST_STRPTR, CONST_STRPTR);
extern "C" LONG dos_native_ReadItem(STRPTR, LONG, CSource *);
extern "C" LONG dos_native_SplitName(CONST_STRPTR, ULONG, STRPTR, LONG, LONG);

static void record2(Batch2Call call, CONST_STRPTR name = nullptr, CONST_STRPTR extra = nullptr)
{
    ++calls.dos2.count[call];
    calls.dos2.name = name;
    calls.dos2.extra = extra;
    calls.dos2.name_copy = name ? name : "";
    calls.dos2.extra_copy = extra ? extra : "";
}

static LONG result2()
{
    calls.ioerr = calls.dos2.error;
    return calls.dos2.result;
}

extern "C" CommandLineInterface *Cli(void)
{
    return calls.dos2.cli_present ? reinterpret_cast<CommandLineInterface *>(0x12345678ABCDEF00ULL) : nullptr;
}

extern "C" ULONG Kaffeine_HeapAlloc(KaffeineContext *ctx, ULONG size, ULONG)
{
    ++calls.dos2.heap_calls;
    if (calls.dos2.heap_fails || size > ctx->mem_size - 0x900)
        return 0;
    return 0x900;
}

extern "C" LONG Info(BPTR lock, InfoData *info)
{
    record2(info_call);
    calls.dos2.handle = lock;
    calls.dos2.info_output = info;
    *info = calls.dos2.info;
    return result2();
}

extern "C" BPTR CreateDir(CONST_STRPTR name)
{
    record2(create_dir_call, name);
    return result2() ? reinterpret_cast<BPTR>(0x13572468UL) : nullptr;
}

extern "C" LONG SetVBuf(BPTR file, STRPTR buffer, LONG type, LONG size)
{
    record2(setvbuf_call);
    calls.dos2.handle = file;
    calls.dos2.buffer = buffer;
    calls.dos2.type = type;
    calls.dos2.size = size;
    calls.ioerr = calls.dos2.error;
    return calls.dos2.result == DOSTRUE ? 0 : -1;
}

extern "C" BOOL SetFileDate(CONST_STRPTR name, const struct DateStamp *date)
{
    record2(setdate_call, name);
    calls.dos2.first = *date;
    return result2();
}

extern "C" LONG SplitName(CONST_STRPTR name, ULONG sep, STRPTR out, LONG pos, LONG size)
{
    record2(split_call, name);
    calls.dos2.scalar = sep;
    calls.dos2.type = pos;
    calls.dos2.size = size;
    return dos_native_SplitName(name, sep, out, pos, size);
}

extern "C" LONG AssignLock(CONST_STRPTR name, BPTR lock)
{
    record2(assign_lock_call, name);
    calls.dos2.handle = lock;
    return result2();
}

extern "C" BOOL AssignLate(CONST_STRPTR name, CONST_STRPTR path)
{
    record2(assign_late_call, name, path);
    /* Reproduce the native strlen(path)+2 CopyMem read under ASan. */
    calls.dos2.bytes.assign(path, std::strlen(path) + 2);
    return result2();
}

extern "C" BOOL AssignPath(CONST_STRPTR name, CONST_STRPTR path)
{
    record2(assign_path_call, name, path);
    calls.dos2.bytes.assign(path, std::strlen(path) + 2);
    return result2();
}

extern "C" BOOL AssignAdd(CONST_STRPTR name, BPTR lock)
{
    record2(assign_add_call, name);
    calls.dos2.handle = lock;
    return result2();
}

extern "C" LONG RemAssignList(CONST_STRPTR name, BPTR lock)
{
    record2(rem_assign_call, name);
    calls.dos2.handle = lock;
    return result2();
}

extern "C" BOOL IsFileSystem(CONST_STRPTR name)
{
    record2(isfs_call, name);
    return result2();
}

extern "C" LONG AddBuffers(CONST_STRPTR name, LONG number)
{
    record2(addbuffers_call, name);
    calls.dos2.size = number;
    return result2();
}

extern "C" LONG CompareDates(const struct DateStamp *a, const struct DateStamp *b)
{
    record2(compare_call);
    calls.dos2.first = *a;
    calls.dos2.second = *b;
    return dos_native_CompareDates(a, b);
}

extern "C" BOOL DateToStr(DateTime *date)
{
    record2(datetostr_call);
    calls.dos2.date = *date;
    return dos_native_DateToStr(date);
}

extern "C" BOOL StrToDate(DateTime *date)
{
    record2(strtodate_call);
    calls.dos2.date = *date;
    return dos_native_StrToDate(date);
}

extern "C" LONG FindArg(CONST_STRPTR templ, CONST_STRPTR keyword)
{
    record2(findarg_call, templ, keyword);
    return dos_native_FindArg(templ, keyword);
}

extern "C" LONG ReadItem(STRPTR buffer, LONG size, CSource *source)
{
    record2(readitem_call);
    calls.dos2.source_null = !source;
    if (source)
        calls.dos2.source = *source;
    return dos_native_ReadItem(buffer, size, source);
}

extern "C" LONG WriteChars(CONST_STRPTR buffer, ULONG size)
{
    record2(writechars_call);
    calls.dos2.bytes.assign(buffer, size);
    calls.dos2.scalar = size;
    return result2() ? static_cast<LONG>(size) : -1;
}

extern "C" BOOL SetOwner(STRPTR name, ULONG owner)
{
    record2(setowner_call, name);
    calls.dos2.scalar = owner;
    return result2();
}

extern "C" BPTR Input(void) { return reinterpret_cast<BPTR>(0x33445566UL); }
extern "C" LONG UnGetC(BPTR file, LONG)
{
    check(file == Input(), "native ReadItem unreads Input handle");
    if (calls.input_pos)
        --calls.input_pos;
    return 0;
}

extern "C" LONG ToLower(LONG c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

extern "C" LONG Strnicmp(CONST_STRPTR a, CONST_STRPTR b, LONG n)
{
    for (LONG i = 0; i < n; ++i)
    {
        LONG diff = ToLower(static_cast<UBYTE>(a[i])) - ToLower(static_cast<UBYTE>(b[i]));
        if (diff || !a[i])
            return diff;
    }
    return 0;
}

struct Fixture
{
    static constexpr ULONG format_addr = 0x200;
    static constexpr ULONG args_addr = 0x300;
    static constexpr ULONG file_handle = 0x10203040;
    static constexpr ULONG return_pc = 0x76543210;
    KaffeineContext ctx = {};
    UBYTE memory[4096] = {};
    std::vector<UBYTE> isolated;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    explicit Fixture(const char *format = "")
    {
        calls = {};
        ctx.mem = memory;
        ctx.mem_size = sizeof(memory);
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = 0x1000;
        lib.jt_start = 0x800;
        std::strcpy(lib.name, "dos.library");
        lib.thunks = dos_thunks;
        lib.num_thunks = sizeof(dos_thunks) / sizeof(dos_thunks[0]);
        lib.gen_thunks = generated_thunks;
        lib.num_gen_thunks = sizeof(generated_thunks) / sizeof(generated_thunks[0]);
        std::strcpy(reinterpret_cast<char *>(memory + format_addr), format);
        std::strcpy(reinterpret_cast<char *>(memory + 0x280), "wrong address register");
        state.a[7] = 0x80;
        m68k_write32(&ctx, state.a[7], return_pc);
        state.d[0] = 0xDEADBEEF;
        state.d[1] = file_handle;
        state.d[2] = format_addr;
        state.d[3] = args_addr;
        state.a[1] = 0x11111111;
        state.a[2] = 0x280;
        state.a[3] = 0x380;
    }

    ~Fixture()
    {
        /* A failed ownership assertion must not leak into the next case. */
        if (calls.allocated_fib)
            std::free(calls.allocated_fib);
        for (void *memory : calls.vec_live)
            std::free(memory);
        if (calls.last_alloc)
            std::free(calls.last_alloc);
        for (AChain *ac : calls.match.live)
            std::free(ac);
    }

    /* A separate allocation makes ASan's redzone coincide with mem_size. */
    void isolate_arena(ULONG size = sizeof(memory))
    {
        isolated.assign(memory, memory + sizeof(memory));
        isolated.resize(size, 0);
        ctx.mem = isolated.data();
        ctx.mem_size = size;
        for (unsigned i = 4; i < 8; ++i)
            state.d[i] = 0xD0000000 + i;
        for (unsigned i = 0; i < 7; ++i)
            state.a[i] = 0xA0000000 + i;
        state.sr = 0x201F;
    }

    void dispatch(ULONG lvo)
    {
        TestCPU before = state;
        KaffeineDispatchResult result = {};
        check(Kaffeine_DispatchLibraryCall(&ctx, &cpu, 0x1000 - lvo, &result),
            "DOS LVO is dispatched");
        check(state.a[7] == 0x84 && result.return_pc == return_pc && !result.supervisor,
            "dispatch consumes exactly one guest return address");
        check(calls.generated_calls == 0, "manual DOS thunk wins over generated fallback");
        check(std::memcmp(state.d + 1, before.d + 1, 7 * sizeof(ULONG)) == 0 &&
            std::memcmp(state.a, before.a, 7 * sizeof(ULONG)) == 0 && state.sr == before.sr,
            "DOS data calls preserve guest input registers and status");
    }
};

/* 68k RawDoFmt rules: a word for %d and %c unless 'l' asks for a long. */
static void test_vfprintf()
{
    Fixture f("%d|%ld|%s|%c\n");
    m68k_write16(&f.ctx, Fixture::args_addr, 0xFFFF);
    m68k_write32(&f.ctx, Fixture::args_addr + 2, 123456);
    m68k_write32(&f.ctx, Fixture::args_addr + 6, 0x340);
    m68k_write16(&f.ctx, Fixture::args_addr + 10, 'Z');
    std::strcpy(reinterpret_cast<char *>(f.memory + 0x340), "text");
    f.dispatch(354);
    const std::string expected = "-1|123456|text|Z\n";
    check(calls.base.printed == expected && f.state.d[0] == expected.size(),
        "VFPrintf renders the 68k data stream and returns the bytes written");
    check(calls.base.printed_to == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)) &&
        calls.base.count[fputc_call] == expected.size(),
        "VFPrintf writes each character to the D1 handle with FPutC");
    check(calls.converter_calls == 0 && calls.writef_calls == 0,
        "VFPrintf uses neither the format-stream converter nor VFWritef");
    check(calls.alloc_mem_calls == 0 && calls.free_mem_calls == 0,
        "VFPrintf sends the text out without a native buffer");
}

static void test_vfwritef()
{
    Fixture f("literal text\n");
    f.state.d[3] = 0;
    f.dispatch(348);
    check(calls.writef_calls == 1 && calls.base.count[fputc_call] == 0,
        "LVO -348 calls VFWritef, not the VFPrintf formatter");
    check(calls.file == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)) &&
        calls.native_format == reinterpret_cast<char *>(f.memory + Fixture::format_addr),
        "VFWritef receives the D1 handle and translated D2 format");
    check(calls.converter_calls == 1 && calls.converter_guest_args == 0 &&
        calls.native_args_match && calls.free_mem_calls == 0,
        "VFWritef preserves the no-argument path without freeing a null stream");
    check(f.state.d[0] == 0, "VFWritef retains the existing zero return behavior");
}

static void test_free_dos_object()
{
    Fixture f;
    UBYTE wanted_object[64] = {};
    UBYTE decoy_object[64] = {};
    f.ctx.num_shadows = 2;
    f.ctx.shadow_map[0].m68k_addr = 0x400;
    f.ctx.shadow_map[0].native_ptr = wanted_object;
    f.ctx.shadow_map[1].m68k_addr = 0x500;
    f.ctx.shadow_map[1].native_ptr = decoy_object;
    f.state.d[1] = DOS_FIB;
    f.state.d[2] = 0x400;
    f.state.a[2] = 0x500;
    f.dispatch(234);
    check(calls.free_object_calls == 1 && calls.object_type == 2,
        "FreeDosObject receives the D1 object type");
    check(calls.object == wanted_object && calls.object != decoy_object,
        "FreeDosObject resolves the D2 shadow, ignoring the distinct A2 shadow");
    check(f.state.d[0] == 0, "FreeDosObject returns zero in D0");
    check(calls.free_rdargs_calls == 0, "FreeDosObject of a DOS_FIB is not an RDArgs");
}

/* AllocDosObject() gives a 68k RDArgs in guest memory, which only the
   ReadArgs code can free; the native FreeDosObject() must not see it. */
static void test_free_rdargs()
{
    Fixture f;
    UBYTE decoy_object[64] = {};
    f.ctx.num_shadows = 1;
    f.ctx.shadow_map[0].m68k_addr = 0x500;
    f.ctx.shadow_map[0].native_ptr = decoy_object;
    f.state.d[1] = DOS_RDARGS;
    f.state.d[2] = 0x600;
    f.state.a[2] = 0x500;
    f.dispatch(234);
    check(calls.free_rdargs_calls == 1 && calls.rdargs == 0x600,
        "FreeDosObject(DOS_RDARGS) frees the 68k RDArgs from D2");
    check(calls.free_object_calls == 0,
        "FreeDosObject(DOS_RDARGS) does not call the native FreeDosObject");
    check(f.state.d[0] == 0, "FreeDosObject(DOS_RDARGS) returns zero in D0");
}

static bool unchanged_outside(const Fixture &f, const UBYTE *before,
                              ULONG start, ULONG size)
{
    return std::memcmp(f.memory, before, start) == 0 &&
        std::memcmp(f.memory + start + size, before + start + size,
                    sizeof(f.memory) - start - size) == 0;
}

static void test_strtolong()
{
    const struct
    {
        const char *text;
        LONG count;
        UBYTE value[4];
    } cases[] =
    {
        { "16909060!", 8, { 0x01, 0x02, 0x03, 0x04 } },
        { " \t-123tail", 6, { 0xFF, 0xFF, 0xFF, 0x85 } },
        { "+0007", 5, { 0, 0, 0, 7 } },
        { "0", 1, { 0, 0, 0, 0 } },
        { "12 34", 2, { 0, 0, 0, 12 } },
        { "19088743", 8, { 0x01, 0x23, 0x45, 0x67 } },
        { "-19088743", 9, { 0xFE, 0xDC, 0xBA, 0x99 } },
        { " \tfoo", -1, { 0, 0, 0, 0 } },
        { "", -1, { 0, 0, 0, 0 } },
        { "+", -1, { 0, 0, 0, 0 } }
    };

    for (const auto &item : cases)
    {
        Fixture f(item.text);
        constexpr ULONG output = 0x402; /* only word aligned, legal on 68k */
        std::memset(f.memory + output - 2, 0xCD, 8);
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = output;
        f.dispatch(816);

        check(calls.strtolong_calls == 1 &&
            calls.string == reinterpret_cast<char *>(f.memory + Fixture::format_addr),
            "StrToLong receives the D1 guest string");
        check(calls.long_output != reinterpret_cast<LONG *>(f.memory + output),
            "StrToLong receives an aligned native LONG, not the guest output");
        check(f.state.d[0] == static_cast<ULONG>(item.count),
            "StrToLong preserves the native consumed count, including whitespace and sign");
        check(std::memcmp(f.memory + output, item.value, 4) == 0,
            "StrToLong writes exactly the four big-endian result bytes, including failure zero");
        check(unchanged_outside(f, before, output, 4),
            "StrToLong leaves input and output canaries untouched");
    }

    Fixture f("16909060");
    LONG decoy = 0x76543210;
    f.ctx.num_shadows = 1;
    f.ctx.shadow_map[0].m68k_addr = 0x400;
    f.ctx.shadow_map[0].native_ptr = &decoy;
    f.state.d[1] = Fixture::format_addr;
    f.state.d[2] = 0x400;
    f.dispatch(816);
    const UBYTE expected[] = { 1, 2, 3, 4 };
    check(std::memcmp(f.memory + 0x400, expected, sizeof(expected)) == 0 &&
        decoy == 0x76543210,
        "StrToLong always fills the guest LONG even if the address has a shadow");
}

static void test_datestamp()
{
    const UBYTE expected[] = { 1, 2, 3, 4, 0, 0, 4, 0xD2, 0, 0, 0x0B, 0xB7 };
    for (bool unset : { false, true })
    {
        Fixture f;
        constexpr ULONG output = 0x402;
        struct DateStamp shadow = { -1, -2, -3 };
        f.ctx.num_shadows = 1;
        f.ctx.shadow_map[0].m68k_addr = output;
        f.ctx.shadow_map[0].native_ptr = &shadow;
        if (unset)
            calls.date_value = {};
        std::memset(f.memory + output - 2, 0xCD, 16);
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[1] = output;
        f.dispatch(192);

        const UBYTE zero[12] = {};
        check(calls.datestamp_calls == 1 &&
            calls.date_output != reinterpret_cast<struct DateStamp *>(f.memory + output) &&
            calls.date_output != &shadow,
            "DateStamp writes through a native temporary independent of guest and shadow");
        check(f.state.d[0] == output, "DateStamp returns the original guest pointer in D0");
        check(std::memcmp(f.memory + output, unset ? zero : expected, 12) == 0,
            "DateStamp stores all three big-endian LONGs, including unset date");
        check(unchanged_outside(f, before, output, 12) &&
            shadow.ds_Days == -1 && shadow.ds_Minute == -2 && shadow.ds_Tick == -3,
            "DateStamp leaves the shadow and surrounding guest bytes alone");
    }
}

static void test_filepart()
{
    const struct { const char *text; ULONG offset; } cases[] =
    {
        { "RAM:dir/file", 8 }, { "RAM:file", 4 }, { "file", 0 },
        { "", 0 }, { "RAM:", 4 }, { "dir/", 4 },
        { "//file", 2 }, { "dir/#?", 4 }
    };
    for (const auto &item : cases)
    {
        Fixture f(item.text);
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[1] = Fixture::format_addr;
        f.dispatch(870);

        ULONG expected = Fixture::format_addr + item.offset;
        check(calls.filepart_calls == 1 &&
            calls.path == reinterpret_cast<char *>(f.memory + Fixture::format_addr),
            "FilePart receives the D1 guest string");
        check(f.state.d[0] == expected,
            "FilePart returns the guest interior pointer, including same pointer and terminal NUL");
        check(std::memcmp(f.memory, before, sizeof(before)) == 0 && f.ctx.num_shadows == 0,
            "FilePart changes neither guest string nor shadow registrations");
        bool aliases_input = false;
        if (f.state.d[0] < sizeof(f.memory))
        {
            f.memory[f.state.d[0]] = 'X';
            aliases_input = f.memory[expected] == 'X';
        }
        check(aliases_input, "writing through FilePart result aliases the original guest buffer");
    }
}

/* Expected offsets follow rom/dos/pathpart.c: leading '/' belong to the
   path, a later '/' is itself the result, and after a ':' the result is
   the next character. */
static void test_pathpart()
{
    const struct { const char *text; ULONG offset; } cases[] =
    {
        { "RAM:dir/file", 7 }, { "RAM:file", 4 }, { "file", 0 },
        { "", 0 }, { "RAM:", 4 }, { "dir/", 3 }, { "//file", 2 },
        { "/", 1 }, { "a:b:c", 4 }, { "RAM:a/b/c", 7 }, { "RAM:/x", 4 },
        { "dir/#?", 3 }
    };
    for (const auto &item : cases)
    {
        Fixture f(item.text);
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[1] = Fixture::format_addr;
        f.dispatch(876);

        ULONG expected = Fixture::format_addr + item.offset;
        const char *host = reinterpret_cast<char *>(f.memory + Fixture::format_addr);
        check(calls.pathpart_calls == 1 && calls.pathpart_path == reinterpret_cast<CONST_STRPTR>(host),
            "PathPart receives the D1 guest string in place");
        check(f.state.d[0] == expected,
            "PathPart returns the guest address of the separator, the start or the terminator");
        check(f.state.d[0] != static_cast<ULONG>(reinterpret_cast<uintptr_t>(host + item.offset)),
            "PathPart does not return the low half of a host pointer");
        check(std::memcmp(f.memory, before, sizeof(before)) == 0 && f.ctx.num_shadows == 0,
            "PathPart changes neither guest string nor shadow registrations");
        bool aliases_input = false;
        if (f.state.d[0] < sizeof(f.memory))
        {
            f.memory[f.state.d[0]] = 'X';
            aliases_input = f.memory[expected] == 'X';
        }
        check(aliases_input, "writing through PathPart result aliases the original guest buffer");
    }
}

/* A native result that does not lie in the validated string is never
   handed to the guest; the terminator itself still is. */
static void test_pathpart_native_results()
{
    const struct { bool null; int offset; ULONG d0; } cases[] =
    {
        { true, 0, 0 }, { false, -1, 0 }, { false, 13, 0 },
        { false, 12, Fixture::format_addr + 12 }
    };
    for (const auto &item : cases)
    {
        Fixture f("RAM:dir/file");
        STRPTR host = reinterpret_cast<STRPTR>(f.memory + Fixture::format_addr);
        calls.pathpart_override = true;
        calls.pathpart_result = item.null ? nullptr : host + item.offset;
        f.state.d[1] = Fixture::format_addr;
        f.dispatch(876);
        check(f.state.d[0] == item.d0,
            "PathPart hands back only a native result inside the guest string");
    }
}

static const SIPTR untouched_ioerr = 0x4567;
static const SIPTR read_error = 0x1F2E;
static const size_t no_error = static_cast<size_t>(-1);

struct FGetsCase
{
    const char *what;
    const char *input;
    ULONG buf;
    ULONG buflen;
    bool succeeds;
    const char *text;   /* characters before the terminator, or nullptr */
    unsigned reads;
    SIPTR ioerr;
    size_t error_at;
};

/* Guard bytes cover everything above 0x300, so any write outside the line
   and its terminator changes the comparison. */
static void check_fgets(const FGetsCase &item)
{
    Fixture f;
    std::memset(f.memory + 0x300, 0xA5, sizeof(f.memory) - 0x300);
    calls.input = item.input;
    calls.input_error_at = item.error_at;
    calls.input_error = read_error;
    f.state.d[2] = item.buf;
    f.state.d[3] = item.buflen;
    UBYTE expected[sizeof(f.memory)];
    std::memcpy(expected, f.memory, sizeof(expected));
    if (item.text)
        std::memcpy(expected + item.buf, item.text, std::strlen(item.text) + 1);
    f.dispatch(336);

    std::string what(item.what);
    check(f.state.d[0] == (item.succeeds ? item.buf : 0),
        (what + ": D0 is the D2 guest buffer, or NULL").c_str());
    check(calls.fgets_calls == 1 &&
        calls.fgets_buf == reinterpret_cast<STRPTR>(f.memory + item.buf) &&
        calls.fgets_buflen == item.buflen &&
        calls.fgets_file == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)) &&
        calls.fgetc_file == calls.fgets_file,
        (what + ": native FGets gets the D1 handle, the D2 buffer in place and D3").c_str());
    check(calls.fgetc_calls == item.reads, (what + ": characters read").c_str());
    check(IoErr() == item.ioerr, (what + ": IoErr() comes from the native call").c_str());
    check(std::memcmp(f.memory, expected, sizeof(expected)) == 0,
        (what + ": only the line and its terminator change guest memory").c_str());
}

/* Expected results follow rom/dos/fgets.c, which decrements buflen and
   then reads at least one character before testing it. */
static void test_fgets()
{
    const FGetsCase cases[] =
    {
        { "FGets reads a line with its newline", "first line\nrest", 0x400, 32,
          true, "first line\n", 11, untouched_ioerr, no_error },
        { "FGets stops after buflen - 1 characters", "abcdefgh\n", 0x400, 5,
          true, "abcd", 4, untouched_ioerr, no_error },
        { "FGets fills buflen - 1 characters without reading on", "abc", 0x400, 4,
          true, "abc", 3, untouched_ioerr, no_error },
        { "FGets returns NULL when the first read is EOF", "", 0x400, 32,
          false, nullptr, 1, 0, no_error },
        { "FGets returns a last line without a newline", "tail", 0x400, 32,
          true, "tail", 5, 0, no_error },
        { "FGets returns an empty line", "\nnext", 0x400, 32,
          true, "\n", 1, untouched_ioerr, no_error },
        { "FGets with buflen 1 stores one character, as rom/dos/fgets.c does", "xy", 0x400, 1,
          true, "x", 1, untouched_ioerr, no_error },
        { "FGets with buflen 2 stores one character", "xy", 0x400, 2,
          true, "x", 1, untouched_ioerr, no_error },
        { "FGets keeps the IoErr() of a failed first read", "abc", 0x400, 32,
          false, nullptr, 1, read_error, 0 },
        { "FGets returns the characters before a read error", "abcdef", 0x400, 32,
          true, "ab", 3, read_error, 2 },
    };
    for (const FGetsCase &item : cases)
        check_fgets(item);

    Fixture f;
    calls.input = "line\n";
    calls.fgets_result_override = reinterpret_cast<STRPTR>(f.memory + 0x401);
    f.state.d[2] = 0x400;
    f.state.d[3] = 32;
    f.dispatch(336);
    check(f.state.d[0] == 0 && calls.fgets_calls == 1,
        "FGets never hands back a native result other than its buffer");
}

static void test_fgets_bounds()
{
    const ULONG end = 4096;
    const FGetsCase cases[] =
    {
        { "FGets fills a buffer that ends at the end of guest memory", "1234567890", end - 8, 8,
          true, "1234567", 7, untouched_ioerr, no_error },
        { "FGets with buflen 1 writes the last two bytes of guest memory", "xy", end - 2, 1,
          true, "x", 1, untouched_ioerr, no_error },
    };
    for (const FGetsCase &item : cases)
        check_fgets(item);

    const struct { const char *what; ULONG buf; ULONG buflen; } refused[] =
    {
        { "FGets refuses buflen 0, which rom/dos/fgets.c does not bound", 0x400, 0 },
        { "FGets refuses buflen 2^31", 0x400, 0x80000000 },
        { "FGets refuses buflen 0xFFFFFFFF", 0x400, 0xFFFFFFFF },
        { "FGets refuses a buffer that crosses the end of guest memory", end - 8, 9 },
        { "FGets refuses buflen 1 in the last byte, where it writes two", end - 1, 1 },
        { "FGets refuses a NULL buffer", 0, 8 },
        { "FGets refuses a buffer outside guest memory", end, 8 },
        { "FGets refuses a range that wraps around 2^32", 0xFFFFFFF8, 16 },
    };
    for (const auto &item : refused)
    {
        Fixture f;
        std::memset(f.memory + 0x300, 0xA5, sizeof(f.memory) - 0x300);
        calls.input = "never read\n";
        f.state.d[2] = item.buf;
        f.state.d[3] = item.buflen;
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.dispatch(336);
        std::string what(item.what);
        check(f.state.d[0] == 0 && IoErr() == ERROR_BAD_NUMBER,
            (what + ": NULL with ERROR_BAD_NUMBER").c_str());
        check(calls.fgets_calls == 0 && calls.fgetc_calls == 0 && calls.input_pos == 0,
            (what + ": nothing is read from the stream").c_str());
        check(std::memcmp(f.memory, before, sizeof(before)) == 0,
            (what + ": guest memory is unchanged").c_str());
    }
}

static void set_native_fib()
{
    auto &fib = calls.fib_value;
    fib.fib_DiskKey = static_cast<IPTR>(0x1122334489ABCDEFULL);
    fib.fib_DirEntryType = -3;
    fib.fib_Protection = 0x13579BDF;
    fib.fib_EntryType = -7;
    fib.fib_Size = 0x01020304;
    fib.fib_NumBlocks = 0x06070809;
    fib.fib_Date = { 0x10203040, 0x50607080, 0x12345678 };
    fib.fib_OwnerUID = 0x1234;
    fib.fib_OwnerGID = 0xABCD;
    std::memset(fib.fib_FileName, 0x51, sizeof(fib.fib_FileName));
    std::strcpy(reinterpret_cast<char *>(fib.fib_FileName), "native-result.bin");
    std::memset(fib.fib_Comment, 0x73, sizeof(fib.fib_Comment));
    std::strcpy(reinterpret_cast<char *>(fib.fib_Comment), "native comment");
    std::memset(fib.fib_Reserved, 0xE7, sizeof(fib.fib_Reserved));
}

static void check_guest_fib(const Fixture &f, ULONG output, const UBYTE *before,
                            const std::string &name = "ExamineFH")
{
    /* Independent classic offsets and literal bytes, not the converter's
       constants or native offsetof values. */
    const struct { ULONG offset; UBYTE value[4]; } fields[] =
    {
        { 0,   { 0x89, 0xAB, 0xCD, 0xEF } },
        { 4,   { 0xFF, 0xFF, 0xFF, 0xFD } },
        { 116, { 0x13, 0x57, 0x9B, 0xDF } },
        { 120, { 0xFF, 0xFF, 0xFF, 0xF9 } },
        { 124, { 1, 2, 3, 4 } },
        { 128, { 6, 7, 8, 9 } },
        { 132, { 0x10, 0x20, 0x30, 0x40 } },
        { 136, { 0x50, 0x60, 0x70, 0x80 } },
        { 140, { 0x12, 0x34, 0x56, 0x78 } },
        { 224, { 0x12, 0x34, 0xAB, 0xCD } }
    };
    bool numeric_match = true;
    for (const auto &field : fields)
        numeric_match &= std::memcmp(f.memory + output + field.offset, field.value, 4) == 0;
    check(numeric_match, (name + " serializes native integers at classic offsets in big endian").c_str());
    check(std::memcmp(f.memory + output + 8, calls.fib_value.fib_FileName, 108) == 0 &&
        std::memcmp(f.memory + output + 144, calls.fib_value.fib_Comment, 80) == 0,
        (name + " copies fixed-length name and comment bytes without BSTR conversion").c_str());
    check(std::memcmp(f.memory + output + 228, before + output + 228, 32) == 0,
        (name + " preserves guest reserved bytes instead of exposing native private data").c_str());
    check(unchanged_outside(f, before, output, 260),
        (name + " does not overwrite either FIB canary").c_str());
}

static void test_examinefh()
{
    static_assert(sizeof(IPTR) > 4 && offsetof(struct FileInfoBlock, fib_FileName) != 8,
        "This regression needs the native 64-bit FIB layout to differ from the guest");
    for (bool with_shadow : { false, true })
    {
        Fixture f;
        constexpr ULONG output = 0x500;
        set_native_fib();
        struct FileInfoBlock shadow;
        std::memset(&shadow, 0x39, sizeof(shadow));
        UBYTE original_shadow[sizeof(shadow)];
        std::memcpy(original_shadow, &shadow, sizeof(shadow));
        if (with_shadow)
        {
            f.ctx.num_shadows = 1;
            f.ctx.shadow_map[0].m68k_addr = output;
            f.ctx.shadow_map[0].native_ptr = &shadow;
        }
        std::memset(f.memory + output - 8, 0xCD, 276);
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[2] = output;
        f.dispatch(390);

        check(calls.examinefh_calls == 1 &&
            calls.file == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)),
            "ExamineFH receives the D1 opaque file handle without address shifting");
        check(calls.allocate_object_calls == 1 && calls.allocate_type == DOS_FIB &&
            calls.allocate_without_tags && calls.examine_received_temporary,
            "ExamineFH receives a native FIB allocated by AllocDosObject");
        check(f.state.d[0] == 0xFFFFFFFF,
            "ExamineFH preserves the exact native success value in guest D0");
        check(calls.free_object_calls == 1 && calls.object_type == DOS_FIB &&
            calls.freed_temporary_fib && calls.allocated_fib == nullptr,
            "ExamineFH frees its temporary FIB exactly once");
        check(IoErr() == calls.examine_ioerr,
            "ExamineFH preserves the filesystem IoErr across DOS cleanup");
        check(std::memcmp(&shadow, original_shadow, sizeof(shadow)) == 0,
            "ExamineFH leaves a registered native shadow untouched");
        check_guest_fib(f, output, before);
    }
}

static void test_output_boundaries()
{
    {
        Fixture f("16909060");
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = sizeof(f.memory) - 4;
        f.dispatch(816);
        const UBYTE expected[] = { 1, 2, 3, 4 };
        check(f.state.d[0] == 8 &&
            std::memcmp(f.memory + sizeof(f.memory) - 4, expected, 4) == 0,
            "StrToLong accepts a LONG ending exactly at the guest memory boundary");
    }
    {
        Fixture f;
        f.state.d[1] = sizeof(f.memory) - 12;
        f.dispatch(192);
        const UBYTE expected[] = { 1, 2, 3, 4, 0, 0, 4, 0xD2, 0, 0, 0x0B, 0xB7 };
        check(f.state.d[0] == sizeof(f.memory) - 12 &&
            std::memcmp(f.memory + sizeof(f.memory) - 12, expected, 12) == 0,
            "DateStamp accepts a complete structure at the guest memory boundary");
    }
    for (const char *path : { "", "RAM:f" })
    {
        Fixture f;
        ULONG start = sizeof(f.memory) - std::strlen(path) - 1;
        std::strcpy(reinterpret_cast<char *>(f.memory + start), path);
        f.state.d[1] = start;
        f.dispatch(870);
        check(f.state.d[0] == start + (*path ? 4 : 0) && calls.filepart_calls == 1,
            "FilePart accepts a terminator in the last byte of guest memory");
    }
    for (const char *path : { "", "RAM:d/f" })
    {
        Fixture f;
        ULONG start = sizeof(f.memory) - std::strlen(path) - 1;
        std::strcpy(reinterpret_cast<char *>(f.memory + start), path);
        f.state.d[1] = start;
        f.dispatch(876);
        check(f.state.d[0] == start + (*path ? 5 : 0) && calls.pathpart_calls == 1,
            "PathPart accepts a terminator in the last byte of guest memory");
    }
    {
        Fixture f;
        ULONG output = sizeof(f.memory) - 260;
        set_native_fib();
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.state.d[2] = output;
        f.dispatch(390);
        check(f.state.d[0] == 0xFFFFFFFF && calls.examinefh_calls == 1,
            "ExamineFH accepts a full FIB ending exactly at the guest memory boundary");
        check_guest_fib(f, output, before);
    }
}

static void test_invalid_outputs()
{
    const struct { ULONG lvo; ULONG size; } functions[] =
        { { 816, 4 }, { 192, 12 }, { 390, 260 } };
    for (const auto &function : functions)
    {
        const ULONG invalid[] = { 0, 4096, 4096 - function.size + 1, 0xFFFFFFFF };
        for (ULONG output : invalid)
        {
            Fixture f("123");
            UBYTE before[sizeof(f.memory)];
            std::memcpy(before, f.memory, sizeof(before));
            if (function.lvo == 816)
                f.state.d[1] = Fixture::format_addr;
            f.state.d[function.lvo == 192 ? 1 : 2] = output;
            f.dispatch(function.lvo);
            check(f.state.d[0] == (function.lvo == 816 ? 0xFFFFFFFF : 0),
                "invalid DOS destination returns the function's failure result");
            check(calls.strtolong_calls == 0 && calls.datestamp_calls == 0 &&
                calls.examinefh_calls == 0 && calls.allocate_object_calls == 0,
                "invalid DOS destination never reaches a native writer or allocates a FIB");
            check(std::memcmp(f.memory, before, sizeof(before)) == 0,
                "invalid or wrapping destination leaves the whole guest arena unchanged");
            if (function.lvo == 390)
                check(IoErr() == ERROR_BAD_NUMBER, "invalid ExamineFH buffer reports a bad argument");
        }
    }
}

static void test_invalid_strings()
{
    for (ULONG lvo : { 816U, 870U, 876U })
    {
        const ULONG invalid[] = { 0, 4096, 0xFFFFFFFF, 4095 };
        for (ULONG input : invalid)
        {
            Fixture f;
            f.memory[4095] = '7'; /* unterminated string at the end of memory */
            f.state.d[1] = input;
            f.state.d[2] = 0x400;
            UBYTE before[sizeof(f.memory)];
            std::memcpy(before, f.memory, sizeof(before));
            f.dispatch(lvo);
            check(f.state.d[0] == (lvo == 816 ? 0xFFFFFFFF : 0),
                "invalid DOS string returns failure without reading outside guest memory");
            check(calls.strtolong_calls == 0 && calls.filepart_calls == 0 &&
                calls.pathpart_calls == 0,
                "invalid or unterminated guest string never reaches the native parser");
            check(std::memcmp(f.memory, before, sizeof(before)) == 0,
                "invalid string leaves guest output and the entire arena unchanged");
        }
    }
}

static void test_examinefh_failures()
{
    for (int failure_kind : { 0, 1, 2 })
    {
        Fixture f;
        set_native_fib();
        f.state.d[2] = 0x500;
        std::memset(f.memory + 0x500, 0xCD, 260);
        if (failure_kind == 0)
            f.state.d[1] = 0;
        else if (failure_kind == 1)
            calls.allocation_fails = true;
        else
            calls.examine_result = 0;
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.dispatch(390);

        check(f.state.d[0] == 0 && std::memcmp(f.memory, before, sizeof(before)) == 0,
            "failed ExamineFH leaves the guest FIB and all surrounding bytes intact");
        if (failure_kind == 0)
        {
            check(calls.allocate_object_calls == 0 && calls.examinefh_calls == 0 &&
                IoErr() == ERROR_BAD_NUMBER,
                "null ExamineFH handle is rejected before the native dereference");
        }
        else if (failure_kind == 1)
        {
            check(calls.allocate_object_calls == 1 && calls.examinefh_calls == 0 &&
                calls.free_object_calls == 0 && IoErr() == ERROR_NO_FREE_STORE,
                "FIB allocation failure reports no memory without invoking or freeing a handler result");
        }
        else
        {
            check(calls.examinefh_calls == 1 && calls.freed_temporary_fib &&
                calls.free_object_calls == 1 && IoErr() == calls.examine_ioerr,
                "filesystem failure frees the native FIB and preserves the original IoErr");
        }
    }
}

static void test_datestamp_failure()
{
    Fixture f;
    f.state.d[1] = 0x400;
    calls.date_succeeds = false;
    UBYTE before[sizeof(f.memory)];
    std::memcpy(before, f.memory, sizeof(before));
    f.dispatch(192);
    check(calls.datestamp_calls == 1 && f.state.d[0] == 0 &&
        std::memcmp(f.memory, before, sizeof(before)) == 0,
        "native DateStamp failure does not publish the temporary output");
}

/* Examine() and ExNext() use the same native temporary FIB as ExamineFH();
   ExNext() starts from the key and type of the entry in the guest FIB. */
static void test_examine_lock()
{
    for (ULONG lvo : { 102U, 108U })
    {
        const std::string name = lvo == 102 ? "Examine" : "ExNext";
        for (ULONG output : { 0x500U, 4096U - 260U })
        {
            Fixture f;
            set_native_fib();
            std::memset(f.memory + output - 8, 0xCD, output == 0x500 ? 276 : 268);
            const UBYTE previous[] = { 0x01, 0x02, 0x03, 0x04, 0xFF, 0xFF, 0xFF, 0xFE };
            std::memcpy(f.memory + output, previous, sizeof(previous));
            UBYTE before[sizeof(f.memory)];
            std::memcpy(before, f.memory, sizeof(before));
            f.state.d[2] = output;
            f.dispatch(lvo);

            check((lvo == 102 ? calls.examine_calls : calls.exnext_calls) == 1 &&
                calls.examine_calls + calls.exnext_calls == 1 &&
                calls.lock == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)),
                (name + " calls the native function once with the D1 lock").c_str());
            check(calls.allocate_object_calls == 1 && calls.allocate_type == DOS_FIB &&
                calls.examine_received_temporary,
                (name + " receives a native FIB allocated by AllocDosObject").c_str());
            if (lvo == 108)
                check(calls.exnext_key == 0x01020304 && calls.exnext_type == -2,
                    "ExNext starts from the key and type in the guest FIB");
            check(f.state.d[0] == 0xFFFFFFFF,
                (name + " preserves the native success value in guest D0").c_str());
            check(calls.free_object_calls == 1 && calls.freed_temporary_fib,
                (name + " frees its temporary FIB exactly once").c_str());
            check(IoErr() == calls.lock_ioerr,
                (name + " preserves the filesystem IoErr across DOS cleanup").c_str());
            check_guest_fib(f, output, before, name);
        }
    }
}

/* The end of a directory: ExNext() fails and the guest FIB stays as it was. */
static void test_exnext_end()
{
    Fixture f;
    set_native_fib();
    calls.lock_result = 0;
    calls.lock_ioerr = ERROR_NO_MORE_ENTRIES;
    std::memset(f.memory + 0x500, 0xCD, 260);
    UBYTE before[sizeof(f.memory)];
    std::memcpy(before, f.memory, sizeof(before));
    f.state.d[2] = 0x500;
    f.dispatch(108);
    check(f.state.d[0] == 0 && IoErr() == ERROR_NO_MORE_ENTRIES &&
        calls.freed_temporary_fib && std::memcmp(f.memory, before, sizeof(before)) == 0,
        "ExNext at the end of a directory keeps ERROR_NO_MORE_ENTRIES and the guest FIB");
}

/* A FIB that does not lie wholly in guest memory is refused before the
   native call, so ExNext() cannot move the directory on and lose an entry. */
static void test_examine_lock_invalid()
{
    for (ULONG lvo : { 102U, 108U })
        for (ULONG output : { 0U, 4096U, 4096U - 259U, 0xFFFFFFFFU })
        {
            Fixture f;
            UBYTE before[sizeof(f.memory)];
            std::memcpy(before, f.memory, sizeof(before));
            f.state.d[2] = output;
            f.dispatch(lvo);
            check(f.state.d[0] == 0 && IoErr() == ERROR_BAD_NUMBER,
                "an incomplete guest FIB fails with ERROR_BAD_NUMBER");
            check(calls.examine_calls == 0 && calls.exnext_calls == 0 &&
                calls.allocate_object_calls == 0,
                "an incomplete guest FIB never reaches Examine or ExNext");
            check(std::memcmp(f.memory, before, sizeof(before)) == 0,
                "an incomplete guest FIB leaves the whole guest arena unchanged");
        }
}

/* A new process would run 68k code natively, so CreateNewProc() and
   CreateProc() fail with ERROR_NOT_IMPLEMENTED and call no native DOS. */
static void test_process_launch()
{
    for (ULONG lvo : { 498U, 138U })
    {
        Fixture f;
        /* NP_Entry with guest code, NP_StackSize 4096, TAG_DONE */
        const UBYTE tags[] =
        {
            0x80, 0x00, 0x03, 0xEB, 0x00, 0x00, 0x06, 0x00,
            0x80, 0x00, 0x03, 0xF3, 0x00, 0x00, 0x10, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        std::memcpy(f.memory + 0x400, tags, sizeof(tags));
        f.state.d[1] = 0x400;
        f.state.d[2] = 0;
        f.state.d[3] = 0x600 >> 2;
        f.state.d[4] = 4096;
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        f.dispatch(lvo);
        const std::string name = lvo == 498 ? "CreateNewProc" : "CreateProc";
        check(f.state.d[0] == 0 && IoErr() == ERROR_NOT_IMPLEMENTED,
            (name + " fails with ERROR_NOT_IMPLEMENTED").c_str());
        check(calls.createnewproc_calls == 0 && calls.createproc_calls == 0 &&
            calls.allocate_object_calls == 0,
            (name + " never hands guest code to native DOS").c_str());
        check(std::memcmp(f.memory, before, sizeof(before)) == 0 && f.ctx.num_shadows == 0,
            (name + " leaves guest memory and the shadows alone").c_str());
    }
}

/* ExAll() writes 64-bit native ExAllData and uses an ExAllControl; DoPkt(),
   SendPkt(), WaitPkt(), ReplyPkt() and AbortPkt() use 68k DosPacket and
   MsgPort structures. Earlier manual thunks also read A2/A5 or A1..A3 instead
   of D1..D5. Verify that all seven LVOs refuse or no-op without calling
   native DOS or touching guest/shadow objects placed at either the D or A
   register addresses. */
static void test_exall_and_packets_refused()
{
    const struct { ULONG lvo; const char *name; bool sets_ioerr; } cases[] =
    {
        { 432, "ExAll", true },
        { 990, "ExAllEnd", false },
        { 240, "DoPkt", true },
        { 246, "SendPkt", true },
        { 252, "WaitPkt", true },
        { 258, "ReplyPkt", true },
        { 264, "AbortPkt", true }
    };
    for (const auto &item : cases)
    {
        Fixture f;
        UBYTE d_shadow[64], a_shadow[64];
        std::memset(d_shadow, 0x5A, sizeof(d_shadow));
        std::memset(a_shadow, 0xA5, sizeof(a_shadow));
        f.ctx.num_shadows = 2;
        f.ctx.shadow_map[0].m68k_addr = 0x400;
        f.ctx.shadow_map[0].native_ptr = d_shadow;
        f.ctx.shadow_map[1].m68k_addr = 0x500;
        f.ctx.shadow_map[1].native_ptr = a_shadow;
        f.state.d[1] = 0x400;
        f.state.d[2] = 0x400;
        f.state.d[3] = 256;
        f.state.d[4] = 1;
        f.state.d[5] = 0x480;
        f.state.a[1] = 0x500;
        f.state.a[2] = 0x500;
        f.state.a[3] = 0x500;
        f.state.a[5] = 0x580;
        UBYTE before[sizeof(f.memory)];
        std::memcpy(before, f.memory, sizeof(before));
        const SIPTR initial_ioerr = IoErr();

        f.dispatch(item.lvo);

        check(f.state.d[0] == 0,
            (std::string(item.name) + " returns zero in D0").c_str());
        check(IoErr() == (item.sets_ioerr ? ERROR_NOT_IMPLEMENTED : initial_ioerr),
            (std::string(item.name) + " sets or preserves IoErr as expected").c_str());
        check(calls.exall_calls == 0 && calls.exallend_calls == 0 &&
            calls.dopkt_calls == 0 && calls.sendpkt_calls == 0 &&
            calls.waitpkt_calls == 0 && calls.replypkt_calls == 0 &&
            calls.abortpkt_calls == 0,
            (std::string(item.name) + " never calls native DOS").c_str());
        check(std::memcmp(f.memory, before, sizeof(before)) == 0,
            (std::string(item.name) + " leaves guest memory untouched").c_str());
    }
}

static void write_tag(KaffeineContext *ctx, ULONG addr, ULONG tag, ULONG data)
{
    m68k_write32(ctx, addr, tag);
    m68k_write32(ctx, addr + 4, data);
}

/* SystemTagList() is at LVO -606 (slot 101 in dos_lib.fd), taking command in
   D1 and tags in D2. Verify that the old -604 slot is no longer registered,
   that -606 reads D1/D2 rather than A1/A2, and that tags are converted with
   tag-aware rules so scalar values such as NP_StackSize 8192 reach the native
   call unchanged instead of being turned into host pointers. */
static void test_system_taglist_lvo()
{
    {
        Fixture f("Echo ok");
        KaffeineDispatchResult result = {};
        check(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 604, &result),
            "old -604 slot is handled as an unimplemented vector");
        check(f.state.d[0] == 0 && calls.system_taglist_calls == 0 &&
            calls.taglist_converter_calls == 0,
            "old -604 slot no longer reaches SystemTagList");
    }
    {
        Fixture f("Echo ok");
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = 0;
        f.state.a[1] = 0x280;
        f.state.a[2] = 0x380;
        f.dispatch(606);
        check(calls.system_taglist_calls == 1 &&
            calls.system_command == reinterpret_cast<char *>(f.memory + Fixture::format_addr),
            "SystemTagList at -606 reads the command string from D1, not A1");
        check(!calls.system_had_tags && calls.alloc_vec_calls == 0 &&
            calls.free_vec_calls == 0 && calls.taglist_converter_calls == 0,
            "SystemTagList with null D2 passes a null tag pointer without allocating");
        check(f.state.d[0] == static_cast<ULONG>(calls.system_result),
            "SystemTagList at -606 returns the native return code in D0");
    }
    {
        Fixture f;
        /* Use a 16 KB arena so 8192 (0x2000) falls inside [0x100, mem_size)
           where the generic heuristic tag converter would corrupt it. */
        std::vector<UBYTE> big_mem(16384, 0);
        f.ctx.mem = big_mem.data();
        f.ctx.mem_size = static_cast<ULONG>(big_mem.size());
        m68k_write32(&f.ctx, f.state.a[7], Fixture::return_pc);
        std::strcpy(reinterpret_cast<char *>(big_mem.data() + Fixture::format_addr), "Echo stack");
        std::strcpy(reinterpret_cast<char *>(big_mem.data() + 0x280), "wrong A1");
        std::strcpy(reinterpret_cast<char *>(big_mem.data() + 0x600), "MyShell");
        std::strcpy(reinterpret_cast<char *>(big_mem.data() + 0x640), "Worker");

        /* First list at 0x300:
           - TAG_IGNORE (skipped)
           - NP_StackSize = 8192
           - TAG_SKIP = 1 (skips the following decoy tag)
           - 0x80009999 = 0xDEAD (skipped by TAG_SKIP)
           - SYS_Input = 0x400 (BPTR inside guest memory range, kept as-is)
           - SYS_Asynch = 1
           - NP_Priority = -5
           - TAG_MORE -> 0x500
           Second list at 0x500:
           - SYS_CustomShell = 0x600 (translated to host string)
           - NP_Name = 0x640 (translated to host string)
           - TAG_DONE */
        write_tag(&f.ctx, 0x300, TAG_IGNORE, 0x200);
        write_tag(&f.ctx, 0x308, NP_StackSize, 8192);
        write_tag(&f.ctx, 0x310, TAG_SKIP, 1);
        write_tag(&f.ctx, 0x318, 0x80009999U, 0xDEAD);
        write_tag(&f.ctx, 0x320, SYS_Input, 0x400);
        write_tag(&f.ctx, 0x328, SYS_Asynch, 1);
        write_tag(&f.ctx, 0x330, NP_Priority, static_cast<ULONG>(-5));
        write_tag(&f.ctx, 0x338, TAG_MORE, 0x500);

        write_tag(&f.ctx, 0x500, SYS_CustomShell, 0x600);
        write_tag(&f.ctx, 0x508, NP_Name, 0x640);
        write_tag(&f.ctx, 0x510, TAG_DONE, 0);

        /* Put an invalid tag at A2 (0x380) to prove D2 is read instead of A2. */
        write_tag(&f.ctx, 0x380, 0x80009999U, 0);
        write_tag(&f.ctx, 0x388, TAG_DONE, 0);

        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = 0x300;
        f.state.a[1] = 0x280;
        f.state.a[2] = 0x380;
        f.dispatch(606);

        check(calls.taglist_converter_calls == 0,
            "SystemTagList never calls the heuristic m68k_to_native_taglist");
        check(calls.system_taglist_calls == 1 && calls.system_had_tags &&
            calls.alloc_vec_calls == 1 && calls.free_vec_calls == 1 &&
            calls.freed_converted_tags,
            "SystemTagList allocates, passes and frees its native tag array");
        check(calls.captured_tags.size() == 7,
            "SystemTagList flattens TAG_IGNORE, TAG_SKIP and TAG_MORE into six items plus TAG_DONE");
        if (calls.captured_tags.size() == 7)
        {
            check(calls.captured_tags[0].ti_Tag == NP_StackSize &&
                calls.captured_tags[0].ti_Data == 8192,
                "NP_StackSize 8192 reaches the native SystemTagList call as 8192");
            check(calls.captured_tags[1].ti_Tag == SYS_Input &&
                calls.captured_tags[1].ti_Data == 0x400,
                "SYS_Input BPTR is passed unchanged to native SystemTagList");
            check(calls.captured_tags[2].ti_Tag == SYS_Asynch &&
                calls.captured_tags[2].ti_Data == 1,
                "SYS_Asynch scalar is passed unchanged to native SystemTagList");
            check(calls.captured_tags[3].ti_Tag == NP_Priority &&
                static_cast<SIPTR>(calls.captured_tags[3].ti_Data) == -5,
                "NP_Priority signed scalar is sign-extended for native SystemTagList");
            check(calls.captured_tags[4].ti_Tag == SYS_CustomShell &&
                calls.captured_tags[4].ti_Data ==
                    reinterpret_cast<IPTR>(big_mem.data() + 0x600),
                "SYS_CustomShell string pointer is translated to host memory");
            check(calls.captured_tags[5].ti_Tag == NP_Name &&
                calls.captured_tags[5].ti_Data ==
                    reinterpret_cast<IPTR>(big_mem.data() + 0x640),
                "NP_Name string pointer is translated to host memory");
            check(calls.captured_tags[6].ti_Tag == TAG_DONE &&
                calls.captured_tags[6].ti_Data == 0,
                "translated SystemTagList tag array ends with TAG_DONE");
        }
    }
}

static void test_system_taglist_invalid()
{
    /* Unsupported tags must fail with -1 and ERROR_NOT_IMPLEMENTED without
       calling native SystemTagList. */
    for (ULONG bad_tag : { static_cast<ULONG>(NP_Entry),
                           static_cast<ULONG>(SYS_Dummy + 11),
                           0x80009999U })
    {
        Fixture f("Echo ok");
        write_tag(&f.ctx, 0x300, bad_tag, 0x200);
        write_tag(&f.ctx, 0x308, TAG_DONE, 0);
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = 0x300;
        f.dispatch(606);
        check(f.state.d[0] == 0xFFFFFFFFU && IoErr() == ERROR_NOT_IMPLEMENTED &&
            calls.system_taglist_calls == 0 && calls.alloc_vec_calls == 0,
            "SystemTagList rejects unsupported tags with -1 and ERROR_NOT_IMPLEMENTED");
    }

    /* Invalid or unterminated command strings must fail with -1 and ERROR_BAD_NUMBER. */
    for (ULONG bad_cmd : { 0U, 4095U, 4096U, 0xFFFFFFFFU })
    {
        Fixture f;
        f.memory[4095] = 'X';
        f.state.d[1] = bad_cmd;
        f.state.d[2] = 0;
        f.dispatch(606);
        check(f.state.d[0] == 0xFFFFFFFFU && IoErr() == ERROR_BAD_NUMBER &&
            calls.system_taglist_calls == 0,
            "SystemTagList rejects null, out-of-bounds or unterminated command strings");
    }

    /* Invalid or unterminated string tag data must fail with -1 and ERROR_BAD_NUMBER. */
    for (ULONG bad_str : { 4095U, 4096U, 0xFFFFFFFFU })
    {
        Fixture f("Echo ok");
        f.memory[4095] = 'X';
        write_tag(&f.ctx, 0x300, NP_Name, bad_str);
        write_tag(&f.ctx, 0x308, TAG_DONE, 0);
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = 0x300;
        f.dispatch(606);
        check(f.state.d[0] == 0xFFFFFFFFU && IoErr() == ERROR_BAD_NUMBER &&
            calls.system_taglist_calls == 0 && calls.alloc_vec_calls == 0,
            "SystemTagList rejects out-of-bounds or unterminated tag strings");
    }

    /* Out-of-bounds tag lists or TAG_MORE targets must fail with -1 and ERROR_BAD_NUMBER. */
    for (ULONG bad_list : { 4096U - 7U, 4096U, 0xFFFFFFFFU })
    {
        Fixture f("Echo ok");
        f.state.d[1] = Fixture::format_addr;
        f.state.d[2] = bad_list;
        f.dispatch(606);
        check(f.state.d[0] == 0xFFFFFFFFU && IoErr() == ERROR_BAD_NUMBER &&
            calls.system_taglist_calls == 0,
            "SystemTagList rejects out-of-bounds tag list addresses");
    }
}

struct BufferVector
{
    ULONG lvo;
    BufferCall call;
    LONG failure;
    unsigned buffer_reg;
    unsigned size_reg;
    std::vector<unsigned> strings;
};

/* Literal ABI values, independent of the production registration macros. */
static const BufferVector buffer_vectors[] =
{
    { 324, read_blocks, -1, 2, 3, {} },
    { 330, write_blocks, -1, 2, 3, {} },
    { 402, name_lock, 0, 2, 3, {} },
    { 408, name_fh, 0, 2, 3, {} },
    { 882, add_part, 0, 1, 3, { 1, 2 } },
    { 900, set_var, 0, 2, 3, { 1 } },
    { 906, get_var, -1, 2, 3, { 1 } },
    { 912, delete_var, 0, 0, 0, { 1 } },
    { 468, fault_text, 0, 3, 4, { 2 } },
    { 474, print_fault, 0, 0, 0, { 2 } },
    { 180, set_comment, 0, 0, 0, { 1, 2 } },
    { 186, set_protect, 0, 0, 0, { 1 } },
    { 840, parse_case, -1, 2, 3, { 1 } },
    { 966, parse_nocase, -1, 2, 3, { 1 } },
    { 846, match_case, 0, 0, 0, { 1, 2 } },
    { 972, match_nocase, 0, 0, 0, { 1, 2 } }
};

static constexpr ULONG buffer_addr = 0x600, name_addr = 0x500, extra_addr = 0x580;

static char *guest(Fixture &f, ULONG addr)
{
    return reinterpret_cast<char *>(f.ctx.mem + addr);
}

static void prepare_buffer(Fixture &f, const BufferVector &v)
{
    f.isolate_arena();
    std::memset(f.ctx.mem + buffer_addr, 0xA5, 128);
    std::strcpy(guest(f, name_addr), "RAM:path");
    std::strcpy(guest(f, extra_addr), "file");
    f.state.d[1] = name_addr;
    f.state.d[2] = extra_addr;
    f.state.d[3] = 32;
    if (v.buffer_reg)
        f.state.d[v.buffer_reg] = buffer_addr;
    switch (v.call)
    {
    case read_blocks:
    case write_blocks:
        f.state.d[1] = Fixture::file_handle;
        f.state.d[3] = 4;
        f.state.d[4] = 3;
        break;
    case name_lock:
    case name_fh:
        f.state.d[1] = Fixture::file_handle;
        break;
    case add_part:
        std::strcpy(guest(f, buffer_addr), "RAM:dir");
        break;
    case set_var:
        f.state.d[3] = 8;
        f.state.d[4] = GVF_LOCAL_ONLY | GVF_BINARY_VAR;
        break;
    case get_var:
        f.state.d[3] = 4;
        f.state.d[4] = GVF_LOCAL_ONLY;
        break;
    case delete_var:
        f.state.d[2] = GVF_LOCAL_ONLY | LV_ALIAS;
        break;
    case fault_text:
        f.state.d[1] = ERROR_OBJECT_NOT_FOUND;
        f.state.d[2] = extra_addr;
        f.state.d[4] = 32;
        break;
    case print_fault:
        f.state.d[1] = ERROR_OBJECT_NOT_FOUND;
        break;
    case set_protect:
        f.state.d[2] = 0x87654321;
        break;
    case parse_case:
    case parse_nocase:
        std::strcpy(guest(f, name_addr), "#?");
        f.state.d[3] = 6;
        break;
    case match_case:
    case match_nocase:
        f.ctx.mem[name_addr] = P_ANY;
        f.ctx.mem[name_addr + 1] = 0;
        break;
    default:
        break;
    }
}

static unsigned buffer_native_calls()
{
    unsigned count = 0;
    for (unsigned n : calls.buffer_calls)
        count += n;
    return count;
}

static void refuse_buffer(Fixture &f, ULONG lvo, LONG result, SIPTR error = ERROR_BAD_NUMBER)
{
    std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
    f.dispatch(lvo);
    check(f.state.d[0] == static_cast<ULONG>(result) && calls.ioerr == error,
        "refused buffer call returns the API failure value and expected IoErr");
    check(buffer_native_calls() == 0, "refused buffers never reach native DOS");
    check(std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
        "refused buffer call leaves the entire guest arena unchanged");
}

static void test_buffer_dispatch()
{
    for (const auto &v : buffer_vectors)
    {
        Fixture f;
        prepare_buffer(f, v);
        f.dispatch(v.lvo);
        check(calls.buffer_calls[v.call] == 1 && buffer_native_calls() == 1,
            "literal LVO reaches exactly its native DOS collaborator");
        if (v.buffer_reg)
        {
            const void *pointer = v.call == write_blocks || v.call == set_var ?
                calls.buffer_input : calls.buffer_output;
            check(pointer == f.ctx.mem + buffer_addr,
                "native byte buffer aliases the guest buffer without a copy");
            check(f.ctx.mem[buffer_addr - 1] == 0 && f.ctx.mem[buffer_addr + 32] == 0xA5,
                "native writes preserve the outer buffer canaries");
        }
        if (v.call == read_blocks || v.call == write_blocks)
        {
            check(calls.buffer_handle == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)) &&
                calls.blocklen == 4 && calls.number == 3,
                "block I/O passes D1 unchanged as BPTR and D3/D4 as unsigned counts");
            check(f.state.d[0] == 1 && calls.ioerr == calls.buffer_ioerr,
                "block I/O returns the native short block count and IoErr");
            if (v.call == read_blocks)
                check(f.ctx.mem[buffer_addr] == 0x6D && f.ctx.mem[buffer_addr + 11] == 0x6D &&
                    f.ctx.mem[buffer_addr + 12] == 0xA5, "FRead writes exactly the checked range");
            else
                check(calls.payload == std::vector<UBYTE>(12, 0xA5), "FWrite reads exactly the checked range");
        }
        else if (v.call == name_lock || v.call == name_fh)
            check(calls.buffer_handle == reinterpret_cast<BPTR>(static_cast<IPTR>(Fixture::file_handle)) &&
                calls.buffer_size == 32 && std::strcmp(guest(f, buffer_addr), "RAM:test") == 0 &&
                f.state.d[0] == 1, "NameFromLock/FH keep the native handle, capacity and result");
        else if (v.call == add_part)
            check(f.state.d[0] == static_cast<ULONG>(-1) &&
                std::strcmp(guest(f, buffer_addr), "RAM:dir/file") == 0 &&
                calls.buffer_extra == guest(f, extra_addr), "AddPart appends using the original guest strings");
        else if (v.call == set_var)
            check(calls.buffer_name == guest(f, name_addr) && calls.buffer_size == 8 &&
                calls.variable == std::vector<UBYTE>(8, 0xA5) &&
                calls.buffer_flags == (GVF_LOCAL_ONLY | GVF_BINARY_VAR) && f.state.d[0] == 1,
                "SetVar explicit size reads binary bytes without searching for NUL");
        else if (v.call == get_var)
            check(calls.buffer_name == guest(f, name_addr) && calls.buffer_size == 4 &&
                calls.buffer_flags == GVF_LOCAL_ONLY && f.state.d[0] == 3 && calls.ioerr == 10 &&
                std::strcmp(guest(f, buffer_addr), "abc") == 0 && f.ctx.mem[buffer_addr + 4] == 0xA5,
                "GetVar truncates, terminates and returns bytes copied with full length in IoErr");
        else if (v.call == delete_var || v.call == set_comment || v.call == set_protect)
        {
            check(calls.buffer_name == guest(f, name_addr) && f.state.d[0] == 1 &&
                calls.ioerr == calls.buffer_ioerr, "named DOS calls preserve guest string, result and IoErr");
            if (v.call == set_comment)
                check(calls.buffer_extra == guest(f, extra_addr), "SetComment passes the D2 guest comment");
            else
                check(calls.buffer_flags == (v.call == delete_var ? (GVF_LOCAL_ONLY | LV_ALIAS) : 0x87654321U),
                    "DeleteVar flags and SetProtection mask remain scalars from D2");
        }
        else if (v.call == fault_text)
            check(calls.buffer_code == ERROR_OBJECT_NOT_FOUND && calls.buffer_extra == guest(f, extra_addr) &&
                calls.buffer_size == 32 && std::strcmp(guest(f, buffer_addr), "file: controlled DOS error") == 0 &&
                f.state.d[0] == 32 - std::strlen(guest(f, buffer_addr)),
                "Fault returns native spare capacity, not the message length claimed by its comment");
        else if (v.call == print_fault)
            check(calls.buffer_code == ERROR_OBJECT_NOT_FOUND && calls.buffer_extra == guest(f, extra_addr) &&
                f.state.d[0] == 1 && calls.ioerr == ERROR_OBJECT_NOT_FOUND,
                "PrintFault keeps the native result and sets IoErr to the error code");
        else if (v.call == parse_case || v.call == parse_nocase)
            check(calls.buffer_name == guest(f, name_addr) && calls.buffer_size == 6 &&
                f.state.d[0] == 1 && f.ctx.mem[buffer_addr] == P_ANY && f.ctx.mem[buffer_addr + 1] == 0,
                "ParsePattern variants produce native NUL-terminated token bytes");
        else
            check(calls.buffer_name == guest(f, name_addr) && calls.buffer_extra == guest(f, extra_addr) &&
                f.state.d[0] == 1 && calls.ioerr == 0 && calls.free_vec_calls == 1,
                "MatchPattern variants consume original guest tokens and release their validation stack");
    }
}

static void test_buffer_bad_addresses()
{
    for (const auto &v : buffer_vectors)
    {
        std::vector<unsigned> pointers = v.strings;
        if (v.buffer_reg)
        {
            bool listed = false;
            for (unsigned reg : pointers)
                listed |= reg == v.buffer_reg;
            if (!listed)
                pointers.push_back(v.buffer_reg);
        }
        for (unsigned reg : pointers)
        {
            for (ULONG bad : { 0U, 4096U, 4097U, 0x80000000U, 0xFFFFFFFEU, 0xFFFFFFFFU })
            {
                if (!bad && reg == 2 && (v.call == fault_text || v.call == print_fault))
                    continue; /* The two nullable headers are tested below. */
                Fixture f;
                prepare_buffer(f, v);
                f.state.d[reg] = bad;
                refuse_buffer(f, v.lvo, v.failure);
            }
        }
        for (unsigned reg : v.strings)
        {
            Fixture f;
            prepare_buffer(f, v);
            std::memset(f.ctx.mem + f.ctx.mem_size - 8, 'x', 8);
            f.state.d[reg] = f.ctx.mem_size - 8;
            if (v.call == add_part && reg == 1)
                f.state.d[3] = 8;
            refuse_buffer(f, v.lvo, v.failure);
        }
    }
}

static void test_buffer_edges_and_lengths()
{
    for (const auto &v : buffer_vectors)
    {
        if (!v.buffer_reg)
            continue;
        for (ULONG past : { 0U, 1U })
        {
            Fixture f;
            prepare_buffer(f, v);
            if (v.call == get_var)
                f.state.d[4] = GVF_LOCAL_ONLY | GVF_BINARY_VAR | GVF_DONT_NULL_TERM;
            ULONG length = v.call == read_blocks || v.call == write_blocks ? 12 : f.state.d[v.size_reg];
            ULONG address = f.ctx.mem_size - length + past;
            if (v.call == add_part)
                std::strcpy(guest(f, address), "RAM:dir");
            f.state.d[v.buffer_reg] = address;
            if (past)
                refuse_buffer(f, v.lvo, v.failure);
            else
            {
                f.dispatch(v.lvo);
                check(calls.buffer_calls[v.call] == 1 && buffer_native_calls() == 1,
                    "a complete buffer ending exactly at mem_size reaches native DOS");
            }
        }
        for (ULONG huge : { 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFEU, 0xFFFFFFFFU })
        {
            if (v.call == set_var && huge == 0xFFFFFFFFU)
                continue; /* Its special string-size value is tested below. */
            Fixture f;
            prepare_buffer(f, v);
            f.state.d[v.size_reg] = huge;
            refuse_buffer(f, v.lvo, v.failure);
        }
        Fixture f;
        prepare_buffer(f, v);
        f.state.d[v.size_reg] = 0;
        if (v.call == read_blocks || v.call == write_blocks)
        {
            f.state.d[1] = 0;
            f.state.d[2] = 0xFFFFFFFF;
            std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
            f.dispatch(v.lvo);
            check(f.state.d[0] == 0 && calls.ioerr == 0 && buffer_native_calls() == 0 &&
                std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
                "zero block size is a no-op without touching the pointer or handle");
        }
        else if (v.call == set_var)
        {
            f.state.d[2] = f.ctx.mem_size - 1;
            f.ctx.mem[f.ctx.mem_size - 1] = 0xAB;
            f.dispatch(v.lvo);
            check(calls.buffer_calls[set_var] == 1 && calls.variable.empty() &&
                f.ctx.mem[f.ctx.mem_size - 1] == 0xAB,
                "SetVar size zero sets an empty value without reading the buffer");
        }
        else
        {
            f.state.d[v.buffer_reg] = 0xFFFFFFFF;
            refuse_buffer(f, v.lvo, v.call == get_var ? 0 : v.failure);
        }
    }
    for (ULONG lvo : { 324U, 330U })
    {
        for (auto factors : { std::pair<ULONG, ULONG>{ 0x80000000U, 2U },
                              { 0xFFFFFFFFU, 2U }, { 0x10000U, 0x10000U } })
        {
            Fixture f;
            prepare_buffer(f, buffer_vectors[lvo == 324 ? 0 : 1]);
            f.state.d[3] = factors.first;
            f.state.d[4] = factors.second;
            refuse_buffer(f, lvo, -1);
        }
        Fixture f;
        prepare_buffer(f, buffer_vectors[lvo == 324 ? 0 : 1]);
        f.state.d[1] = 0;
        f.state.d[2] = 0xFFFFFFFF;
        f.state.d[3] = 0xFFFFFFFF;
        f.state.d[4] = 0;
        f.dispatch(lvo);
        check(f.state.d[0] == 0 && calls.ioerr == 0 && buffer_native_calls() == 0,
            "zero block count is a no-op even with a huge block size");
    }
    for (unsigned index : { 0U, 1U, 3U })
    {
        Fixture f;
        const auto &v = buffer_vectors[index];
        prepare_buffer(f, v);
        f.state.d[1] = 0;
        refuse_buffer(f, v.lvo, v.failure);
    }
    Fixture f;
    prepare_buffer(f, buffer_vectors[2]);
    f.state.d[1] = 0;
    f.dispatch(402);
    check(calls.buffer_calls[name_lock] == 1 && calls.buffer_handle == nullptr &&
        std::strcmp(guest(f, buffer_addr), "SYS:") == 0, "NameFromLock accepts the native zero-lock convention");
}

static void test_buffer_strings_and_failures()
{
    for (const auto &v : buffer_vectors)
    {
        for (unsigned reg : v.strings)
        {
            /* A single NUL at the final byte is a valid empty string. */
            Fixture f;
            prepare_buffer(f, v);
            f.state.d[reg] = f.ctx.mem_size - 1;
            f.ctx.mem[f.ctx.mem_size - 1] = 0;
            if (v.call == add_part && reg == 1)
            {
                f.state.d[3] = 1;
                std::strcpy(guest(f, extra_addr), "");
            }
            f.dispatch(v.lvo);
            check(calls.buffer_calls[v.call] == 1, "string NUL at the arena's last byte is accepted");
        }
        if (v.call == read_blocks || v.call == write_blocks || v.call == name_lock ||
            v.call == name_fh || v.call == set_var || v.call == get_var || v.call == delete_var ||
            v.call == print_fault || v.call == set_comment || v.call == set_protect)
        {
            Fixture f;
            prepare_buffer(f, v);
            calls.buffer_failure = true;
            std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
            f.dispatch(v.lvo);
            LONG want = v.call == read_blocks || v.call == write_blocks || v.call == get_var ? -1 : 0;
            SIPTR error = v.call == name_lock || v.call == name_fh ? ERROR_LINE_TOO_LONG :
                v.call == get_var ? ERROR_OBJECT_NOT_FOUND :
                v.call == print_fault ? ERROR_OBJECT_NOT_FOUND : calls.buffer_ioerr;
            check(calls.buffer_calls[v.call] == 1 && f.state.d[0] == static_cast<ULONG>(want) &&
                calls.ioerr == error && std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
                "native failures and IoErr propagate without guest writes");
        }
    }
    for (ULONG flags : { 0U, static_cast<ULONG>(GVF_LOCAL_ONLY | LV_ALIAS),
                        static_cast<ULONG>(GVF_GLOBAL_ONLY | GVF_SAVE_VAR | GVF_BINARY_VAR | GVF_DONT_NULL_TERM) })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[5]);
        f.state.d[2] = f.ctx.mem_size - 4;
        std::memcpy(guest(f, f.state.d[2]), "a\nb", 4);
        f.state.d[3] = 0xFFFFFFFF;
        f.state.d[4] = flags;
        f.dispatch(900);
        check(calls.variable == std::vector<UBYTE>({ 'a', '\n', 'b' }) &&
            calls.buffer_size == -1 && calls.buffer_flags == flags,
            "SetVar -1 accepts a string ending at mem_size and passes all type/scope flags");
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[5]);
        f.state.d[2] = f.ctx.mem_size - 4;
        std::memset(guest(f, f.state.d[2]), 'x', 4);
        f.state.d[3] = 0xFFFFFFFF;
        refuse_buffer(f, 900, 0);
    }
    for (ULONG flags : { 0U, static_cast<ULONG>(GVF_LOCAL_ONLY | LV_ALIAS),
                        static_cast<ULONG>(GVF_GLOBAL_ONLY | GVF_BINARY_VAR),
                        static_cast<ULONG>(GVF_LOCAL_ONLY | GVF_BINARY_VAR | GVF_DONT_NULL_TERM) })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[6]);
        f.state.d[4] = flags;
        f.dispatch(906);
        bool raw = (flags & GVF_BINARY_VAR) && (flags & GVF_DONT_NULL_TERM);
        check(calls.buffer_flags == flags && f.state.d[0] == (raw ? 4U : 3U) && calls.ioerr == 10 &&
            std::memcmp(guest(f, buffer_addr), raw ? "abcd" : "abc\0", 4) == 0,
            "GetVar preserves native text/binary truncation and scope/type flags");
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[6]);
        f.state.d[3] = 32;
        f.dispatch(906);
        check(f.state.d[0] == 6 && calls.ioerr == 10 && std::strcmp(guest(f, buffer_addr), "abcdef") == 0 &&
            std::memcmp(guest(f, buffer_addr + 7), "XYZ", 3) == 0,
            "GetVar text copies the value before replacing its first newline with NUL");
    }
    for (ULONG spare : { 0U, 1U })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[6]);
        f.state.d[2] = f.ctx.mem_size - 4 - spare;
        if (!spare)
            refuse_buffer(f, 906, -1);
        else
        {
            f.dispatch(906);
            check(f.state.d[0] == 3 && calls.buffer_calls[get_var] == 1,
                "GetVar text accepts a full buffer only with its native lookahead byte in the arena");
        }
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[6]);
        f.state.d[2] = name_addr + 1;
        f.state.d[4] = GVF_GLOBAL_ONLY | GVF_BINARY_VAR | GVF_DONT_NULL_TERM;
        refuse_buffer(f, 906, -1);
    }
}

static void test_addpart_and_fault_edges()
{
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[8]);
        f.state.d[2] = f.ctx.mem_size - 33;
        f.state.d[3] = f.ctx.mem_size - 32;
        std::strcpy(guest(f, f.state.d[2]), "A");
        refuse_buffer(f, 468, 0);
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[4]);
        f.state.d[3] = 8;
        std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
        f.dispatch(882);
        check(f.state.d[0] == 0 && calls.ioerr == ERROR_LINE_TOO_LONG && calls.buffer_calls[add_part] == 1 &&
            std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
            "AddPart native no-fit failure leaves dirname intact with ERROR_LINE_TOO_LONG");
    }
    for (const char *path : { "SYS:other", ":root" })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[4]);
        std::strcpy(guest(f, extra_addr), path);
        f.dispatch(882);
        check(std::strcmp(guest(f, buffer_addr), path[0] == ':' ? "RAM:root" : "SYS:other") == 0,
            "AddPart delegates absolute and root-relative paths to native DOS");
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[4]);
        f.state.d[2] = buffer_addr + 4;
        refuse_buffer(f, 882, 0);
    }
    for (ULONG lvo : { 468U, 474U })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[lvo == 468 ? 8 : 9]);
        f.state.d[2] = 0;
        f.dispatch(lvo);
        check(calls.buffer_extra == nullptr && buffer_native_calls() == 1,
            "Fault and PrintFault accept NULL headers");
    }
    for (ULONG capacity : { 1U, 3U })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[8]);
        f.state.d[3] = f.ctx.mem_size - capacity;
        f.state.d[4] = 1;
        if (capacity == 1)
            refuse_buffer(f, 468, 0);
        else
        {
            f.dispatch(468);
            check(calls.buffer_calls[fault_text] == 1 && f.state.d[0] == static_cast<ULONG>(-1) &&
                std::strcmp(guest(f, f.state.d[3]), ": ") == 0,
                "Fault len 1 with a header needs three bytes and returns its native negative result");
        }
    }
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[8]);
        f.state.d[1] = 0;
        f.state.d[2] = 0;
        f.state.d[3] = f.ctx.mem_size - 1;
        f.state.d[4] = 1;
        f.ctx.mem[f.ctx.mem_size - 1] = 0xAA;
        f.dispatch(468);
        check(f.state.d[0] == 0 && f.ctx.mem[f.ctx.mem_size - 1] == 0 && buffer_native_calls() == 1,
            "Fault code zero writes only its terminator and returns zero");
    }
    for (ULONG capacity : { 1U, 2U })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[8]);
        f.state.d[1] = static_cast<ULONG>(-123);
        f.state.d[2] = 0;
        f.state.d[3] = f.ctx.mem_size - capacity;
        f.state.d[4] = 1;
        calls.fault_unknown = true;
        if (capacity == 1)
            refuse_buffer(f, 468, 0);
        else
        {
            f.dispatch(468);
            check(f.state.d[0] == 0 && std::strcmp(guest(f, f.state.d[3]), "-") == 0,
                "Fault reserves the unknown negative code's unbounded minus sign too");
        }
    }
    for (ULONG lvo : { 468U, 474U })
    {
        Fixture f;
        prepare_buffer(f, buffer_vectors[lvo == 468 ? 8 : 9]);
        f.state.d[1] = 0x80000000;
        refuse_buffer(f, lvo, 0);
    }
}

static void test_pattern_buffers()
{
    for (unsigned index : { 12U, 13U })
    {
        const auto &v = buffer_vectors[index];
        {
            Fixture f;
            prepare_buffer(f, v);
            f.state.d[3] = 5;
            refuse_buffer(f, v.lvo, -1, ERROR_BUFFER_OVERFLOW);
        }
        {
            Fixture f;
            prepare_buffer(f, v);
            f.state.d[2] = name_addr;
            refuse_buffer(f, v.lvo, -1);
        }
        for (const char *source : { "#|a", "~|a", "a|b", "[a'", "[", "(a", "a)" })
        {
            Fixture f;
            prepare_buffer(f, v);
            f.state.d[1] = f.ctx.mem_size - static_cast<ULONG>(std::strlen(source)) - 1;
            std::strcpy(guest(f, f.state.d[1]), source);
            f.state.d[3] = 64;
            refuse_buffer(f, v.lvo, -1, ERROR_BAD_TEMPLATE);
        }
        {
            Fixture f;
            prepare_buffer(f, v);
            std::strcpy(guest(f, name_addr), "#");
            f.dispatch(v.lvo);
            check(calls.buffer_calls[v.call] == 1 && f.state.d[0] == static_cast<ULONG>(-1) &&
                calls.ioerr == ERROR_BAD_TEMPLATE, "native ParsePattern syntax errors retain -1 and IoErr");
        }
    }
    for (ULONG lvo : { 846U, 972U })
    {
        for (const auto &tokens : std::vector<std::vector<UBYTE>>{
                { P_REPBEG, 0 }, { P_REPEND, 0 }, { P_NOT, 0 }, { P_NOTEND, 0 },
                { P_ORSTART, 0 }, { P_ORNEXT, 0 }, { P_OREND, 0 },
                { P_CLASS, 'a', 0 }, { P_NOTCLASS, 'a', 0 },
                { P_ORSTART, P_REPBEG, P_OREND, P_REPEND, 0 },
                { P_REPBEG, P_CLASS, P_REPBEG, P_CLASS, P_REPEND, 0 },
                { P_REPBEG, P_CLASS, P_REPEND, P_CLASS, P_REPEND, 0 },
                { P_ORSTART, P_CLASS, P_ORSTART, P_CLASS, P_OREND, 0 },
                { P_ORSTART, P_CLASS, P_ORNEXT, P_CLASS, P_OREND, 0 },
                { P_ORSTART, P_CLASS, P_OREND, P_CLASS, P_OREND, 0 },
                { P_NOT, P_CLASS, P_NOT, P_CLASS, P_NOTEND, 0 },
                { P_NOT, P_CLASS, P_NOTEND, P_CLASS, P_NOTEND, 0 } })
        {
            Fixture f;
            prepare_buffer(f, buffer_vectors[lvo == 846 ? 14 : 15]);
            ULONG addr = f.ctx.mem_size - static_cast<ULONG>(tokens.size());
            std::memcpy(f.ctx.mem + addr, tokens.data(), tokens.size());
            f.state.d[1] = addr;
            refuse_buffer(f, lvo, 0, ERROR_BAD_TEMPLATE);
            check(calls.free_vec_calls == 1, "invalid compiled pattern releases its validation stack");
        }
        Fixture f;
        prepare_buffer(f, buffer_vectors[lvo == 846 ? 14 : 15]);
        calls.alloc_vec_fails = true;
        refuse_buffer(f, lvo, 0, ERROR_NO_FREE_STORE);
    }
    /* Compare the wrapper with actual native tokens and matching results,
       including nested groups, repetitions, negations, ranges and quotes. */
    for (const char *source : { "", "#?", "?", "a", "(a|b)", "#(ab)", "~(ab)", "[a-z]",
                               "[~a]", "[a-]", "[]]", "[']]", "((a|b)|c)", "'#literal", "*", "abc'" })
    {
        for (bool nocase : { false, true })
        {
            for (const char *text : { "", "a", "ab", "ABC", "]" })
            {
                Fixture f;
                const auto &v = buffer_vectors[nocase ? 13 : 12];
                prepare_buffer(f, v);
                std::strcpy(guest(f, name_addr), source);
                f.state.d[3] = 128;
                char expected[128] = {};
                LONG parsed = dos_native_ParsePattern(source, expected, sizeof(expected), !nocase);
                check(parsed >= 0, "native pattern sample parses successfully");
                f.dispatch(v.lvo);
                check(f.state.d[0] == static_cast<ULONG>(parsed) &&
                    std::strcmp(guest(f, buffer_addr), expected) == 0,
                    "ParsePattern wrapper preserves exact native tokens and wildcard result");
                BOOL matched = dos_native_MatchPattern(expected, text, !nocase);
                f.state.a[7] = 0x80;
                f.state.d[1] = buffer_addr;
                f.state.d[2] = extra_addr;
                std::strcpy(guest(f, extra_addr), text);
                f.dispatch(nocase ? 972 : 846);
                check(calls.buffer_calls[nocase ? match_nocase : match_case] == 1 &&
                    f.state.d[0] == static_cast<ULONG>(matched),
                    "token validator accepts native output and preserves native match/no-match results");
            }
        }
    }
}

static ULONG call2(Fixture &f, ULONG lvo, std::initializer_list<ULONG> args = {})
{
    unsigned reg = 1;
    f.state.a[7] = 0x80;
    for (ULONG arg : args)
        f.state.d[reg++] = arg;
    f.dispatch(lvo);
    return f.state.d[0];
}

static unsigned native_calls2()
{
    unsigned count = 0;
    for (unsigned n : calls.dos2.count)
        count += n;
    return count;
}

static std::vector<UBYTE> snapshot2(const Fixture &f)
{
    return { f.ctx.mem, f.ctx.mem + f.ctx.mem_size };
}

static void refused2(Fixture &f, ULONG lvo, std::initializer_list<ULONG> args,
                      ULONG result, SIPTR error = ERROR_BAD_NUMBER)
{
    auto before = snapshot2(f);
    unsigned count = native_calls2();
    check(call2(f, lvo, args) == result && calls.ioerr == error,
          "DOS batch 2 refuses invalid input with its failure result and IoErr");
    check(native_calls2() == count && snapshot2(f) == before,
          "DOS batch 2 refusal makes no native call and changes no guest bytes");
}

static void put_date2(Fixture &f, ULONG addr, LONG days = 380, LONG mins = 754, LONG ticks = 1150)
{
    m68k_write32(&f.ctx, addr, static_cast<ULONG>(days));
    m68k_write32(&f.ctx, addr + 4, static_cast<ULONG>(mins));
    m68k_write32(&f.ctx, addr + 8, static_cast<ULONG>(ticks));
}

static void put_datetime2(Fixture &f, ULONG addr, ULONG day, ULONG date, ULONG time)
{
    put_date2(f, addr);
    f.ctx.mem[addr + 12] = FORMAT_INT;
    f.ctx.mem[addr + 13] = DTF_FUTURE;
    m68k_write32(&f.ctx, addr + 14, day);
    m68k_write32(&f.ctx, addr + 18, date);
    m68k_write32(&f.ctx, addr + 22, time);
}

static void put_csource2(Fixture &f, ULONG addr, ULONG buffer, LONG len, LONG pos)
{
    m68k_write32(&f.ctx, addr, buffer);
    m68k_write32(&f.ctx, addr + 4, static_cast<ULONG>(len));
    m68k_write32(&f.ctx, addr + 8, static_cast<ULONG>(pos));
}

static void test_dos2_named_calls()
{
    struct Named { ULONG lvo; Batch2Call call; };
    const Named cases[] = {
        { 120, create_dir_call }, { 612, assign_lock_call },
        { 630, assign_add_call }, { 636, rem_assign_call },
        { 708, isfs_call }, { 732, addbuffers_call }, { 996, setowner_call }
    };
    for (auto c : cases)
    {
        for (bool edge : { false, true })
        {
            Fixture f;
            f.isolate_arena();
            ULONG addr = edge ? f.ctx.mem_size - 5 : 0x500;
            std::memcpy(f.ctx.mem + addr, "RAM:", 5);
            ULONG scalar = c.lvo == 732 ? 0xFFFFFFFDu : 0x89ABCDEFu;
            check(call2(f, c.lvo, { addr, scalar }) ==
                    (c.lvo == 120 ? 0x13572468u : 0xFFFFFFFFu),
                  "named DOS call returns the controlled native result");
            check(calls.dos2.count[c.call] == 1 && calls.dos2.name ==
                    reinterpret_cast<char *>(f.ctx.mem + addr) && calls.dos2.name_copy == "RAM:" &&
                    calls.ioerr == calls.dos2.error,
                  "named DOS call translates D1 and preserves native IoErr");
            if (c.lvo == 612 || c.lvo == 630 || c.lvo == 636)
                check(calls.dos2.handle == reinterpret_cast<BPTR>(static_cast<IPTR>(scalar)),
                      "assign lock is an opaque D2 BPTR, not a shifted guest address");
            if (c.lvo == 732)
                check(calls.dos2.size == -3, "AddBuffers sign extends its buffer count");
            if (c.lvo == 996)
                check(calls.dos2.scalar == 0x89ABCDEFu, "SetOwner preserves UID/GID scalar bits");
            calls.dos2.result = 0;
            check(call2(f, c.lvo, { addr, scalar }) == 0 && calls.ioerr == calls.dos2.error,
                  "named DOS native failure preserves result and IoErr");
        }
        for (ULONG bad : { 0u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu, 4095u })
        {
            Fixture f;
            f.isolate_arena();
            f.ctx.mem[4095] = 'X';
            refused2(f, c.lvo, { bad, 0x10203040 }, 0);
        }
    }
    for (ULONG lvo : { 612u, 636u })
    {
        Fixture f("RAM:");
        check(call2(f, lvo, { 0x200, 0 }) == 0xFFFFFFFFu && calls.dos2.handle == nullptr,
              "AssignLock removes with zero; RemAssignList retains zero-lock semantics");
    }
    Fixture add("RAM:");
    refused2(add, 630, { 0x200, 0 }, 0);

    for (ULONG lvo : { 618u, 624u })
    {
        Fixture f("TEST");
        f.isolate_arena();
        std::memcpy(f.ctx.mem + 4091, "RAM:", 5);
        check(call2(f, lvo, { 0x200, 4091 }) == 0xFFFFFFFFu,
              "deferred/path assign accepts path ending at final arena byte");
        check(calls.dos2.extra_copy == "RAM:" && calls.dos2.bytes == std::string("RAM:\0\0", 6) &&
              calls.dos2.extra != reinterpret_cast<char *>(f.ctx.mem + 4091),
              "assign path scratch supplies the native one-byte read past NUL");
        check(calls.alloc_vec_calls == 1 && calls.free_vec_calls == 1 &&
              calls.allocated_tags == nullptr && calls.ioerr == calls.dos2.error,
              "assign scratch is released while preserving native IoErr");
        calls.dos2.result = 0;
        check(call2(f, lvo, { 0x200, 4091 }) == 0 && calls.ioerr == calls.dos2.error,
              "assign native failure releases scratch and preserves its error");
        for (unsigned arg : { 1u, 2u })
            for (ULONG bad : { 0u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu, 4095u })
            {
                f.ctx.mem[4095] = 'X';
                refused2(f, lvo, { arg == 1 ? bad : 0x200u, arg == 2 ? bad : 0x200u }, 0);
            }
        calls.alloc_vec_fails = true;
        refused2(f, lvo, { 0x200, 0x200 }, 0, ERROR_NO_FREE_STORE);
    }
}

static void test_dos2_info_dates()
{
    for (ULONG addr : { 0x502u, 4060u })
    {
        Fixture f;
        f.isolate_arena();
        auto before = snapshot2(f);
        check(call2(f, 114, { 0, addr }) == 0xFFFFFFFFu && calls.dos2.handle == nullptr,
              "Info accepts zero lock and uses a native temporary");
        const ULONG expected[] = { 0xFFFFFFFD, 0x01020304, 82, 1234567, 765432, 512,
                                   0x444F5301, 0, 0xFFFFFFFF };
        for (unsigned i = 0; i < 9; ++i)
            check(m68k_read32(&f.ctx, addr + 4 * i) == expected[i],
                  "Info converts every InfoData longword and suppresses native VolumeNode");
        check(reinterpret_cast<UBYTE *>(calls.dos2.info_output) != f.ctx.mem + addr &&
              std::memcmp(f.ctx.mem + addr, "\xFF\xFF\xFF\xFD\x01\x02\x03\x04", 8) == 0,
              "Info uses the native layout and writes big-endian guest fields");
        bool canaries = true;
        for (ULONG i = 0; i < f.ctx.mem_size; ++i)
            if (i < addr || i - addr >= 36)
                canaries &= f.ctx.mem[i] == before[i];
        check(canaries, "Info writes exactly the 36-byte guest structure");
        calls.dos2.result = 0;
        before = snapshot2(f);
        check(call2(f, 114, { 0x10203040, addr }) == 0 && snapshot2(f) == before &&
              calls.ioerr == calls.dos2.error,
              "Info native failure leaves guest output intact and preserves IoErr");
    }
    for (ULONG bad : { 0u, 4061u, 4096u, 0xFFFFFFFEu, 0xFFFFFFFFu })
    {
        Fixture f;
        f.isolate_arena();
        refused2(f, 114, { 0, bad }, 0);
        refused2(f, 396, { 0x200, bad == 4061 ? 4085u : bad }, 0);
        refused2(f, 738, { 0x300, bad == 4061 ? 4085u : bad }, 0);
        refused2(f, 738, { bad == 4061 ? 4085u : bad, 0x300 }, 0);
    }
    for (ULONG addr : { 0x502u, 4084u })
    {
        Fixture f("RAM:date");
        f.isolate_arena();
        put_date2(f, addr, -123, 0x01020304, -456);
        auto before = snapshot2(f);
        check(call2(f, 396, { 0x200, addr }) == 0xFFFFFFFFu && calls.dos2.first.ds_Days == -123 &&
              calls.dos2.first.ds_Minute == 0x01020304 && calls.dos2.first.ds_Tick == -456 &&
              snapshot2(f) == before && calls.ioerr == calls.dos2.error,
              "SetFileDate converts all signed DateStamp fields without changing input");
        calls.dos2.result = 0;
        check(call2(f, 396, { 0x200, addr }) == 0 && calls.ioerr == calls.dos2.error,
              "SetFileDate preserves native failure and IoErr");
        for (ULONG bad : { 0u, 4096u, 0xFFFFFFFFu, 4095u })
        {
            f.ctx.mem[4095] = 'X';
            refused2(f, 396, { bad, 0x300 }, 0);
        }
    }
    const struct DateStamp pairs[][2] = {
        { { 1, 2, 3 }, { 1, 2, 3 } }, { { 1, 2, 3 }, { 4, 2, 3 } },
        { { 1, 9, 3 }, { 1, 2, 3 } }, { { 1, 2, 9 }, { 1, 2, 3 } },
        { { INT32_MIN, 0, 0 }, { INT32_MAX, 0, 0 } },
        { { 1, INT32_MAX, 0 }, { 1, INT32_MIN, 0 } },
        { { 1, 2, INT32_MIN }, { 1, 2, INT32_MAX } }
    };
    for (auto &pair : pairs)
    {
        Fixture f;
        f.isolate_arena();
        put_date2(f, 0x500, pair[0].ds_Days, pair[0].ds_Minute, pair[0].ds_Tick);
        put_date2(f, 4084, pair[1].ds_Days, pair[1].ds_Minute, pair[1].ds_Tick);
        auto before = snapshot2(f);
        int64_t diff = static_cast<int64_t>(pair[1].ds_Days) - pair[0].ds_Days;
        if (!diff) diff = static_cast<int64_t>(pair[1].ds_Minute) - pair[0].ds_Minute;
        if (!diff) diff = static_cast<int64_t>(pair[1].ds_Tick) - pair[0].ds_Tick;
        bool overflow = diff < INT32_MIN || diff > INT32_MAX;
        LONG expected = overflow ? (diff < 0 ? -1 : 1) : static_cast<LONG>(diff);
        SIPTR error = calls.ioerr;
        check(call2(f, 738, { 0x500, 4084 }) == static_cast<ULONG>(expected) &&
              calls.dos2.count[compare_call] == (overflow ? 0u : 1u) && snapshot2(f) == before &&
              calls.ioerr == error,
              "CompareDates keeps reverse ordering and avoids native signed overflow");
        if (!overflow)
            check(std::memcmp(&calls.dos2.first, &pair[0], sizeof(pair[0])) == 0 &&
                  std::memcmp(&calls.dos2.second, &pair[1], sizeof(pair[1])) == 0,
                  "CompareDates receives two native DateStamp values");
    }
}

static void test_dos2_datetime()
{
    for (ULONG mask = 0; mask < 8; ++mask)
    {
        Fixture f;
        f.isolate_arena();
        ULONG ptrs[] = { mask & 1 ? 0x500u : 0u, mask & 2 ? 0x600u : 0u, mask & 4 ? 4080u : 0u };
        put_datetime2(f, 4070 - 32, ptrs[0], ptrs[1], ptrs[2]);
        char day[16] = {}, date[16] = {}, time[16] = {};
        DateTime expected = { { 380, 754, 1150 }, FORMAT_INT, DTF_FUTURE,
                              ptrs[0] ? day : nullptr, ptrs[1] ? date : nullptr, ptrs[2] ? time : nullptr };
        check(dos_native_DateToStr(&expected) != 0, "native date oracle accepts fixture stamp");
        auto before = snapshot2(f);
        check(call2(f, 744, { 4038 }) == 0xFFFFFFFFu && calls.dos2.count[datetostr_call] == 1,
              "DateToStr accepts every combination of three nullable output pointers");
        for (unsigned i = 0; i < 3; ++i)
        {
            if (ptrs[i])
                check(std::strcmp(reinterpret_cast<char *>(f.ctx.mem + ptrs[i]),
                                  i == 0 ? day : i == 1 ? date : time) == 0,
                      "DateToStr converts each 68k pointer at offsets 14/18/22");
            check(m68k_read32(&f.ctx, 4038 + 14 + i * 4) == ptrs[i],
                  "DateTime pointers remain 32-bit guest addresses");
        }
        check(std::memcmp(f.ctx.mem + 4038, before.data() + 4038, 26) == 0 &&
              calls.dos2.date.dat_Format == FORMAT_INT && calls.dos2.date.dat_Flags == DTF_FUTURE,
              "DateToStr preserves the 26-byte layout including byte format and flags");
        put_date2(f, 4038, 100, 200, 300);
        expected.dat_Stamp = { 100, 200, 300 };
        BOOL native_result = dos_native_StrToDate(&expected);
        check(call2(f, 750, { 4038 }) == static_cast<ULONG>(native_result) &&
              m68k_read32(&f.ctx, 4038) == static_cast<ULONG>(expected.dat_Stamp.ds_Days) &&
              m68k_read32(&f.ctx, 4042) == static_cast<ULONG>(expected.dat_Stamp.ds_Minute) &&
              m68k_read32(&f.ctx, 4046) == static_cast<ULONG>(expected.dat_Stamp.ds_Tick),
              "StrToDate copies the native stamp back and preserves components with NULL inputs");
    }
    for (UBYTE format : { 0, 1, 2, 3, 4, 255 })
    {
        Fixture f;
        f.isolate_arena();
        put_datetime2(f, 0x500, 0x600, 0x700, 0x800);
        f.ctx.mem[0x50C] = format;
        f.ctx.mem[0x50D] = 0;
        char day[16] = {}, date[16] = {}, time[16] = {};
        DateTime expected = { { 380, 754, 1150 }, format, 0, day, date, time };
        BOOL result = dos_native_DateToStr(&expected);
        check(call2(f, 744, { 0x500 }) == static_cast<ULONG>(result) &&
              calls.dos2.date.dat_Format == format &&
              std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 0x600), day) == 0 &&
              std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 0x700), date) == 0 &&
              std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 0x800), time) == 0,
              "DateToStr preserves all format bytes and unknown-format native fallback");
        put_date2(f, 0x500, 0, 0, 0);
        check(call2(f, 750, { 0x500 }) == 0xFFFFFFFFu && m68k_read32(&f.ctx, 0x500) == 380 &&
              m68k_read32(&f.ctx, 0x504) == 754 && m68k_read32(&f.ctx, 0x508) == 1150,
              "StrToDate preflight preserves all four native date grammars and fallback format");
    }
    for (UBYTE flags : { 0, 2 })
    {
        Fixture f;
        f.isolate_arena();
        std::strcpy(reinterpret_cast<char *>(f.ctx.mem + 0x600), "Tuesday");
        put_datetime2(f, 0x500, 0, 0x600, 0);
        f.ctx.mem[0x50D] = flags;
        DateTime expected = { { 380, 754, 1150 }, FORMAT_INT, flags, nullptr,
                              reinterpret_cast<char *>(f.ctx.mem + 0x600), nullptr };
        BOOL result = dos_native_StrToDate(&expected);
        check(call2(f, 750, { 0x500 }) == static_cast<ULONG>(result) &&
              m68k_read32(&f.ctx, 0x500) == static_cast<ULONG>(expected.dat_Stamp.ds_Days),
              "StrToDate preserves relative weekday strings and FUTURE flag semantics");
    }
    for (ULONG lvo : { 744u, 750u })
    {
        for (ULONG addr : { 0u, 4071u, 4096u, 0xFFFFFFFEu, 0xFFFFFFFFu })
        {
            Fixture f;
            f.isolate_arena();
            refused2(f, lvo, { addr }, 0);
        }
        Fixture edge;
        edge.isolate_arena();
        put_datetime2(edge, 4070, 0, 0, 0);
        check(call2(edge, lvo, { 4070 }) == 0xFFFFFFFFu,
              "complete DateTime may end exactly at arena boundary");
        for (unsigned field = 0; field < 3; ++field)
        {
            for (ULONG bad : { 4081u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu })
            {
                Fixture f;
                f.isolate_arena();
                std::memset(f.ctx.mem + 4081, 'X', 15);
                ULONG ptrs[3] = {};
                ptrs[field] = bad;
                put_datetime2(f, 0x500, ptrs[0], ptrs[1], ptrs[2]);
                refused2(f, lvo, { 0x500 }, 0);
            }
        }
    }
    for (unsigned field = 0; field < 3; ++field)
    {
        Fixture edge;
        edge.isolate_arena();
        ULONG ptrs[3] = {};
        ptrs[field] = 4080;
        put_datetime2(edge, 0x500, ptrs[0], ptrs[1], ptrs[2]);
        check(call2(edge, 744, { 0x500 }) == 0xFFFFFFFFu && edge.ctx.mem[4080] != 0,
              "each DateToStr LEN_DATSTRING buffer may end exactly at guest memory boundary");
    }
    Fixture edge_input;
    edge_input.isolate_arena();
    std::memcpy(edge_input.ctx.mem + 4087, "12:34:23", 9);
    put_datetime2(edge_input, 0x500, 4095, 0, 4087);
    check(call2(edge_input, 750, { 0x500 }) == 0xFFFFFFFFu &&
          m68k_read32(&edge_input.ctx, 0x504) == 754 && m68k_read32(&edge_input.ctx, 0x508) == 1150,
          "StrToDate accepts input NUL at last byte, including ignored day string");
    Fixture partial;
    partial.isolate_arena();
    std::strcpy(reinterpret_cast<char *>(partial.ctx.mem + 0x600), "79-01-16");
    std::strcpy(reinterpret_cast<char *>(partial.ctx.mem + 0x700), "99:00:00");
    put_datetime2(partial, 0x500, 0, 0x600, 0x700);
    put_date2(partial, 0x500, 999, 111, 222);
    SIPTR error = calls.ioerr;
    check(call2(partial, 750, { 0x500 }) == 0 && m68k_read32(&partial.ctx, 0x500) == 380 &&
          m68k_read32(&partial.ctx, 0x504) == 111 && m68k_read32(&partial.ctx, 0x508) == 222 &&
          calls.ioerr == error,
          "StrToDate failure after date conversion copies its partial stamp and preserves IoErr");
    for (const char *bad : { "79-00-01", "79-13-01", "2147483647-1-1", "79-1-2147483647", "5885500-1-1" })
    {
        Fixture f;
        f.isolate_arena();
        std::strcpy(reinterpret_cast<char *>(f.ctx.mem + 0x600), bad);
        put_datetime2(f, 0x500, 0, 0x600, 0);
        refused2(f, 750, { 0x500 }, 0);
    }
    Fixture long_input;
    long_input.isolate_arena();
    std::strcpy(reinterpret_cast<char *>(long_input.ctx.mem + 0x600), "1979-01-16 trailing 2147483647");
    std::strcpy(reinterpret_cast<char *>(long_input.ctx.mem + 0x700), "12:34:23 trailing 2147483647");
    put_datetime2(long_input, 0x500, 0, 0x600, 0x700);
    check(call2(long_input, 750, { 0x500 }) == 0xFFFFFFFFu &&
          m68k_read32(&long_input.ctx, 0x500) == 380 &&
          m68k_read32(&long_input.ctx, 0x504) == 754 &&
          m68k_read32(&long_input.ctx, 0x508) == 1150,
          "StrToDate accepts inputs longer than LEN_DATSTRING and native ignored suffixes");
    Fixture huge_time;
    huge_time.isolate_arena();
    std::strcpy(reinterpret_cast<char *>(huge_time.ctx.mem + 0x600), "2147483647:00");
    put_datetime2(huge_time, 0x500, 0, 0, 0x600);
    refused2(huge_time, 750, { 0x500 }, 0);
    Fixture overlap;
    overlap.isolate_arena();
    put_datetime2(overlap, 0x500, 0x500, 0, 0);
    refused2(overlap, 744, { 0x500 }, 0);
    put_datetime2(overlap, 0x500, 0x600, 0x608, 0);
    refused2(overlap, 744, { 0x500 }, 0);
    Fixture invalid_stamp;
    invalid_stamp.isolate_arena();
    put_datetime2(invalid_stamp, 0x500, 0x600, 0x700, 0x800);
    put_date2(invalid_stamp, 0x500, -1, 0, 0);
    auto before = snapshot2(invalid_stamp);
    error = calls.ioerr;
    check(call2(invalid_stamp, 744, { 0x500 }) == 0 && snapshot2(invalid_stamp) == before &&
          calls.dos2.count[datetostr_call] == 1 && calls.ioerr == error,
          "DateToStr native invalid-stamp failure leaves every guest byte intact");
}

static void test_dos2_split_find()
{
    for (LONG pos : { 0, 4, 8 })
        for (LONG size : { 1, 2, 4, 16 })
        {
            Fixture f("one/two/");
            f.isolate_arena();
            char expected[16] = {};
            LONG result = dos_native_SplitName("one/two/", '/', expected, pos, size);
            check(call2(f, 414, { 0x200, '/', 4096u - static_cast<ULONG>(size),
                        static_cast<ULONG>(pos), static_cast<ULONG>(size) }) == static_cast<ULONG>(result) &&
                  std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 4096 - size), expected) == 0,
                  "SplitName preserves native positions, truncation, size one and exact-end buffer");
            check(calls.dos2.scalar == '/' && calls.dos2.type == pos && calls.dos2.size == size,
                  "SplitName uses D1 through D5 in NDK order");
        }
    Fixture f("one/two/");
    f.isolate_arena();
    for (ULONG bad : { 0u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu })
    {
        refused2(f, 414, { bad, '/', 0x500, 0, 8 }, 0xFFFFFFFFu);
        refused2(f, 414, { 0x200, '/', bad, 0, 8 }, 0xFFFFFFFFu);
        refused2(f, 804, { bad, 0x200 }, 0xFFFFFFFFu);
        refused2(f, 804, { 0x200, bad }, 0xFFFFFFFFu);
    }
    for (ULONG bad : { 0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
        refused2(f, 414, { 0x200, '/', 0x500, 0, bad }, 0xFFFFFFFFu);
    for (ULONG bad : { 9u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
        refused2(f, 414, { 0x200, '/', 0x500, bad, 8 }, 0xFFFFFFFFu);
    refused2(f, 414, { 0x200, '/', 4089, 0, 8 }, 0xFFFFFFFFu);
    refused2(f, 414, { 0x200, '/', 0x201, 0, 8 }, 0xFFFFFFFFu);
    std::memcpy(f.ctx.mem + 4087, "one/two/", 9);
    check(call2(f, 414, { 4087, '/', 0x500, 0, 16 }) == 4 &&
          std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 0x500), "one") == 0,
          "SplitName accepts source terminator at arena edge");
    f.ctx.mem[4095] = 'X';
    refused2(f, 414, { 4087, '/', 0x500, 0, 16 }, 0xFFFFFFFFu);
    refused2(f, 804, { 4087, 0x200 }, 0xFFFFFFFFu);
    refused2(f, 804, { 0x200, 4087 }, 0xFFFFFFFFu);
    Fixture template_edge;
    template_edge.isolate_arena();
    std::memcpy(template_edge.ctx.mem + 4092, "Q/S", 4);
    std::strcpy(reinterpret_cast<char *>(template_edge.ctx.mem + 0x500), "q");
    check(call2(template_edge, 804, { 4092, 0x500 }) == 0,
          "FindArg accepts the template NUL at the guest memory boundary");
    Fixture reversed("q");
    std::strcpy(reinterpret_cast<char *>(reversed.ctx.mem + 0x500), "FROM/A,TO/A,QUIET=Q/S");
    check(call2(reversed, 804, { 0x200, 0x500 }) == 0xFFFFFFFFu && calls.dos2.name_copy == "q",
          "FindArg never guesses or swaps template and keyword from their contents");
    const char *templ = "FROM/A,TO/A,QUIET=Q/S";
    for (const char *key : { "from", "TO", "q", "QUIET", "missing", "" })
    {
        Fixture find(templ);
        find.isolate_arena();
        ULONG addr = 4096 - static_cast<ULONG>(std::strlen(key)) - 1;
        std::strcpy(reinterpret_cast<char *>(find.ctx.mem + addr), key);
        LONG expected = dos_native_FindArg(templ, key);
        check(call2(find, 804, { 0x200, addr }) == static_cast<ULONG>(expected) &&
              calls.dos2.name_copy == templ && calls.dos2.extra_copy == key,
              "FindArg uses template D1 and keyword D2, including aliases and missing keyword");
    }
}

static void test_dos2_readitem()
{
    const char *sources[] = { "word", "one two\n", "\"a*n**b\"", "=", "\n", "", "\"unterminated" };
    for (const char *text : sources)
        for (LONG size : { 1, 2, 16 })
        {
            Fixture f;
            f.isolate_arena();
            ULONG len = static_cast<ULONG>(std::strlen(text)), start = 4096 - len;
            std::memcpy(f.ctx.mem + start, text, len);
            put_csource2(f, 0x500, start, len, 0);
            char expected[16] = {};
            CSource source = { const_cast<char *>(text), static_cast<LONG>(len), 0 };
            LONG result = dos_native_ReadItem(expected, size, &source);
            SIPTR error = calls.ioerr;
            check(call2(f, 810, { 0x600, static_cast<ULONG>(size), 0x500 }) == static_cast<ULONG>(result) &&
                  std::strcmp(reinterpret_cast<char *>(f.ctx.mem + 0x600), expected) == 0 &&
                  m68k_read32(&f.ctx, 0x508) == static_cast<ULONG>(source.CS_CurChr),
                  "ReadItem converts CSource both ways and preserves AROS quoting/EOF quirks");
            check(m68k_read32(&f.ctx, 0x500) == start && m68k_read32(&f.ctx, 0x504) == len &&
                  calls.ioerr == error && calls.dos2.count[readitem_call] == 1,
                  "ReadItem only writes CS_CurChr and preserves native IoErr");
            if (len)
                check(calls.dos2.source.CS_Buffer == reinterpret_cast<char *>(f.ctx.mem + start) &&
                      calls.dos2.source.CS_Length == static_cast<LONG>(len),
                      "ReadItem accepts a counted source ending at last byte without a NUL");
        }
    Fixture edge;
    edge.isolate_arena();
    std::memcpy(edge.ctx.mem + 0x600, "word\n", 5);
    put_csource2(edge, 4084, 0x600, 5, 0);
    check(call2(edge, 810, { 0x500, 16, 4084 }) == ITEM_UNQUOTED &&
          m68k_read32(&edge.ctx, 4092) == 4,
          "ReadItem accepts CSource ending exactly at arena boundary");
    put_csource2(edge, 0x500, 0x600, 5, 0);
    check(call2(edge, 810, { 4080, 16, 0x500 }) == ITEM_UNQUOTED,
          "ReadItem accepts an output ending exactly at arena boundary");
    Fixture stream;
    stream.isolate_arena();
    calls.input = "one two\n";
    check(call2(stream, 810, { 0x500, 16, 0 }) == ITEM_UNQUOTED && calls.dos2.source_null &&
          std::strcmp(reinterpret_cast<char *>(stream.ctx.mem + 0x500), "one") == 0 &&
          calls.fgetc_file == Input(), "ReadItem NULL source reads native Input");
    for (ULONG bad : { 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu })
    {
        Fixture f;
        f.isolate_arena();
        refused2(f, 810, { bad, 16, 0 }, 0xFFFFFFFFu);
        refused2(f, 810, { 0x500, 16, bad }, 0xFFFFFFFFu);
    }
    Fixture bad;
    bad.isolate_arena();
    refused2(bad, 810, { 0x500, 16, 4085 }, 0xFFFFFFFFu);
    refused2(bad, 810, { 4081, 16, 0 }, 0xFFFFFFFFu);
    for (ULONG size : { 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
        refused2(bad, 810, { 0x500, size, 0 }, 0xFFFFFFFFu);
    const LONG invalid[][2] = { { -1, 0 }, { 1, -1 }, { 1, 2 }, { INT32_MAX, 0 } };
    for (auto &p : invalid)
    {
        put_csource2(bad, 0x500, 0x600, p[0], p[1]);
        refused2(bad, 810, { 0x700, 16, 0x500 }, 0xFFFFFFFFu);
    }
    for (ULONG source : { 0u, 4093u, 0xFFFFFFFFu, 0xFFFFFFFEu })
    {
        put_csource2(bad, 0x500, source, 4, 0);
        refused2(bad, 810, { 0x700, 16, 0x500 }, 0xFFFFFFFFu);
    }
    put_csource2(bad, 0x500, 0x600, 4, 0);
    refused2(bad, 810, { 0x504, 16, 0x500 }, 0xFFFFFFFFu);
    refused2(bad, 810, { 0x600, 16, 0x500 }, 0xFFFFFFFFu);
    auto before = snapshot2(bad);
    SIPTR error = calls.ioerr;
    check(call2(bad, 810, { 0, 0xFFFFFFFFu, 0xFFFFFFFFu }) == ITEM_NOTHING &&
          snapshot2(bad) == before && calls.ioerr == error,
          "ReadItem NULL output returns ITEM_NOTHING without inspecting other arguments");
    bad.ctx.mem[4095] = 'X';
    check(call2(bad, 810, { 4095, 0, 0xFFFFFFFFu }) == ITEM_NOTHING && bad.ctx.mem[4095] == 0 &&
          calls.ioerr == error, "ReadItem zero maxchars still writes one NUL without reading source");
    put_csource2(bad, 0x500, 0, 0, 0);
    check(call2(bad, 810, { 4095, 1, 0x500 }) == ITEM_NOTHING && bad.ctx.mem[4095] == 0 &&
          m68k_read32(&bad.ctx, 0x508) == 0,
          "ReadItem zero-length source may have NULL buffer and consumes no characters");
}

static void test_dos2_vbuf_writechars()
{
    for (LONG size : { -1, 0, 207, 208, 256 })
        for (ULONG ptr : { 0u, 0x500u })
        {
            Fixture f;
            f.isolate_arena();
            check(call2(f, 366, { 0x10203040, ptr, BUF_FULL, static_cast<ULONG>(size) }) == 0 &&
                  calls.dos2.count[setvbuf_call] == 1 && calls.dos2.buffer == nullptr &&
                  calls.dos2.handle == reinterpret_cast<BPTR>(0x10203040UL) &&
                  calls.dos2.type == BUF_FULL && calls.dos2.size == size && calls.ioerr == calls.dos2.error,
                  "SetVBuf preserves mode/size but always requests a DOS-owned native buffer");
            calls.dos2.result = 0;
            check(call2(f, 366, { 0x10203040, ptr, BUF_NONE, static_cast<ULONG>(size) }) == 0xFFFFFFFFu &&
                  calls.ioerr == calls.dos2.error, "SetVBuf preserves native EOF failure and IoErr");
        }
    Fixture f;
    f.isolate_arena();
    check(call2(f, 366, { 1, 3888, BUF_LINE, 0 }) == 0,
          "SetVBuf accepts exactly 208 guest bytes at the arena edge even for size zero");
    for (ULONG buffer : { 3889u, 3892u, 0x501u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFCu })
        refused2(f, 366, { 1, buffer, BUF_FULL, 208 }, 0xFFFFFFFFu);
    for (ULONG size : { 0xFFFFFFFEu, 0x80000000u, 0x7FFFFFFFu, 4097u })
        refused2(f, 366, { 1, 0, BUF_FULL, size }, 0xFFFFFFFFu);
    for (ULONG type : { 3u, 0xFFFFFFFFu, 0x80000000u })
        refused2(f, 366, { 1, 0x500, type, 208 }, 0xFFFFFFFFu);
    refused2(f, 366, { 0, 0, BUF_FULL, 208 }, 0xFFFFFFFFu);
    check(call2(f, 366, { 1, 0xFFFFFFFFu, BUF_LINE, 0xFFFFFFFFu }) == 0,
          "SetVBuf size -1 ignores the unused guest buffer");
    const char bytes[] = { 'A', 0, 'B', '\n' };
    std::memcpy(f.ctx.mem + 4092, bytes, 4);
    check(call2(f, 942, { 4092, 4 }) == 4 && calls.dos2.bytes == std::string(bytes, 4) &&
          calls.dos2.scalar == 4 && calls.ioerr == calls.dos2.error,
          "WriteChars accepts counted binary bytes ending at the arena edge");
    calls.dos2.result = 0;
    check(call2(f, 942, { 4092, 4 }) == 0xFFFFFFFFu && calls.ioerr == calls.dos2.error,
          "WriteChars preserves native EOF failure and IoErr");
    for (ULONG buffer : { 0u, 4093u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu })
        refused2(f, 942, { buffer, 4 }, 0xFFFFFFFFu);
    for (ULONG size : { 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
        refused2(f, 942, { 0x500, size }, 0xFFFFFFFFu);
    auto before = snapshot2(f);
    unsigned count = native_calls2();
    SIPTR error = calls.ioerr;
    check(call2(f, 942, { 0xFFFFFFFFu, 0 }) == 0 && snapshot2(f) == before &&
          native_calls2() == count && calls.ioerr == error,
          "WriteChars zero length ignores buffer and preserves IoErr without native output");
}

static void setup_arguments2(Fixture &f)
{
    f.ctx.m68k_sysbase = 0x400;
    m68k_write32(&f.ctx, 0x400 + 276, 0x600);
    m68k_write32(&f.ctx, 0x600 + 204, 0x200);
}

static void test_dos2_arguments_cli()
{
    Fixture f("original\n");
    f.isolate_arena();
    setup_arguments2(f);
    f.ctx.m68k_argptr = 0x200;
    f.ctx.m68k_argsize = 9;
    std::memcpy(f.ctx.mem + 4092, "new", 4);
    check(call2(f, 534) == 0x200, "GetArgStr returns guest ThisTask pr_Arguments");
    check(call2(f, 540, { 4092 }) == 0x200 && call2(f, 534) == 4092 &&
          m68k_read32(&f.ctx, 0x600 + 204) == 4092 && f.ctx.m68k_argptr == 0x200 &&
          f.ctx.m68k_argsize == 9, "SetArgStr changes only guest Process arguments, preserving startup registers");
    check(call2(f, 540, { 0 }) == 4092 && call2(f, 534) == 0,
          "SetArgStr accepts NULL and GetArgStr returns NULL without a native pointer");
    m68k_write32(&f.ctx, 0x600 + 204, 0x200);
    for (ULONG bad : { 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu, 4095u })
    {
        f.ctx.mem[4095] = 'X';
        refused2(f, 540, { bad }, 0);
        m68k_write32(&f.ctx, 0x600 + 204, bad);
        refused2(f, 534, {}, 0);
        refused2(f, 540, { 0x200 }, 0);
        m68k_write32(&f.ctx, 0x600 + 204, 0x200);
    }
    for (ULONG bad : { 0u, 3889u, 4096u, 0xFFFFFFFEu })
    {
        m68k_write32(&f.ctx, 0x400 + 276, bad);
        refused2(f, 534, {}, 0);
        refused2(f, 540, { 0x200 }, 0);
    }
    setup_arguments2(f);
    for (ULONG bad : { 0u, 3817u, 4096u, 0xFFFFFFFEu })
    {
        f.ctx.m68k_sysbase = bad;
        refused2(f, 534, {}, 0);
        refused2(f, 540, { 0x200 }, 0);
    }
    setup_arguments2(f);
    m68k_write32(&f.ctx, 0x400 + 276, 3888);
    m68k_write32(&f.ctx, 4092, 0x200);
    check(call2(f, 534) == 0x200, "GetArgStr accepts its Process field ending at the arena boundary");
    setup_arguments2(f);
    f.ctx.m68k_sysbase = 3816;
    m68k_write32(&f.ctx, 4092, 0x600);
    check(call2(f, 534) == 0x200, "GetArgStr accepts its ExecBase field ending at the arena boundary");
    for (auto pair : { std::pair<ULONG, ULONG>(558, 564), { 582, 588 }, { 570, 0 } })
    {
        Fixture cli("guest name");
        cli.isolate_arena();
        std::strcpy(cli.ctx.program_name, "initial");
        check(call2(cli, pair.first, { 0x200 }) == 0xFFFFFFFFu,
              "CLI setter changes the guest snapshot");
        ULONG offset = pair.first == 558 ? 4 : pair.first == 582 ? 24 : 16;
        ULONG bstr = m68k_read32(&cli.ctx, cli.ctx.m68k_cli + offset) << 2;
        check(cli.ctx.mem[bstr] == 10 && std::memcmp(cli.ctx.mem + bstr + 1, "guest name", 10) == 0,
              "CLI setter writes the NDK BSTR field and length");
        if (!pair.second)
            check(std::strcmp(cli.ctx.program_name, "guest name") == 0,
                  "SetProgramName also updates the cached startup-name fallback");
        else
        {
            check(call2(cli, pair.second, { 4080, 16 }) == 0xFFFFFFFFu &&
                  std::strcmp(reinterpret_cast<char *>(cli.ctx.mem + 4080), "guest name") == 0,
                  "CLI getter reads its guest BSTR into a buffer ending at the arena edge");
            check(call2(cli, pair.second, { 4085, 11 }) == 0 && calls.ioerr == ERROR_LINE_TOO_LONG &&
                  std::strcmp(reinterpret_cast<char *>(cli.ctx.mem + 4085), "guest name") == 0,
                  "CLI getter preserves AROS exact-fit failure and writes the terminated name");
            check(call2(cli, pair.second, { 4095, 1 }) == 0 && cli.ctx.mem[4095] == 0 &&
                  calls.ioerr == ERROR_LINE_TOO_LONG, "CLI one-byte buffer receives only NUL");
            for (ULONG size : { 0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
                refused2(cli, pair.second, { 0x500, size }, 0);
            for (ULONG buffer : { 0u, 4081u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu })
                refused2(cli, pair.second, { buffer, 16 }, 0);
            /* A BSTR needs its counted bytes, not a C terminator. */
            cli.ctx.mem[4092] = 3;
            std::memcpy(cli.ctx.mem + 4093, "end", 3);
            m68k_write32(&cli.ctx, cli.ctx.m68k_cli + offset, 4092 >> 2);
            check(call2(cli, pair.second, { 0x500, 16 }) == 0xFFFFFFFFu &&
                  std::strcmp(reinterpret_cast<char *>(cli.ctx.mem + 0x500), "end") == 0,
                  "CLI getter honors direct guest BSTR changes at the memory boundary");
            for (ULONG bad : { 0u, 0x40000000u, 0xFFFFFFFFu, 1024u })
            {
                m68k_write32(&cli.ctx, cli.ctx.m68k_cli + offset, bad);
                refused2(cli, pair.second, { 0x500, 16 }, 0);
            }
            cli.ctx.mem[4092] = 4;
            m68k_write32(&cli.ctx, cli.ctx.m68k_cli + offset, 4092 >> 2);
            refused2(cli, pair.second, { 0x500, 16 }, 0);
            calls.dos2.cli_present = false;
            check(call2(cli, pair.first, { 0x200 }) == 0xFFFFFFFFu && calls.dos2.heap_calls == 1,
                  "cached guest CLI survives execution in a native child without CLI");
        }
        std::memcpy(cli.ctx.mem + 4092, "end", 4);
        check(call2(cli, pair.first, { 4092 }) == 0xFFFFFFFFu,
              "CLI setters accept the source C string ending at the arena boundary");
        std::memset(cli.ctx.mem + 0x500, 'N', 255);
        cli.ctx.mem[0x5FF] = 0;
        check(call2(cli, pair.first, { 0x500 }) == 0xFFFFFFFFu && cli.ctx.mem[bstr] == 255,
              "CLI setters have capacity for the full 255-byte BSTR");
        cli.ctx.mem[0x5FF] = 'N';
        cli.ctx.mem[0x600] = 0;
        refused2(cli, pair.first, { 0x500 }, 0, ERROR_LINE_TOO_LONG);
        for (ULONG bad : { 0u, 4096u, 0xFFFFFFFFu, 0xFFFFFFFEu, 4095u })
        {
            cli.ctx.mem[4095] = 'X';
            refused2(cli, pair.first, { bad }, 0);
        }
    }
    for (ULONG lvo : { 558u, 570u, 582u })
    {
        Fixture absent("guest");
        calls.dos2.cli_present = false;
        refused2(absent, lvo, { 0x200 }, 0, ERROR_OBJECT_WRONG_TYPE);
        calls.dos2.cli_present = true;
        calls.dos2.heap_fails = true;
        refused2(absent, lvo, { 0x200 }, 0, ERROR_NO_FREE_STORE);
        absent.ctx.m68k_cli = 0xFFFFFFFEu;
        refused2(absent, lvo, { 0x200 }, 0);
    }
    for (ULONG lvo : { 564u, 588u })
    {
        Fixture absent;
        calls.dos2.cli_present = false;
        absent.ctx.mem[4095] = 'X';
        check(call2(absent, lvo, { 4095, 1 }) == 0 && absent.ctx.mem[4095] == 0 &&
              calls.ioerr == ERROR_OBJECT_WRONG_TYPE,
              "without a guest CLI the getter empties valid output and fails");
    }
}

static Fixture *exit_fixture2;
static unsigned exit_steps2;

static void execute_exit2(void *)
{
    ++exit_steps2;
    exit_fixture2->dispatch(144);
    /* The baseline comparison has only the poisoned fallback. Bound that
       comparison too, so a missing stop reports a failure rather than hanging. */
    if (calls.generated_calls)
        exit_fixture2->ctx.running = FALSE;
}

static void test_dos2_exit()
{
    for (ULONG code : { 0u, 7u, 20u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu })
    {
        Fixture f;
        f.isolate_arena();
        f.state.d[1] = code;
        KaffeineCPUOps ops = cpu_ops;
        ops.execute = execute_exit2;
        f.cpu.ops = &ops;
        exit_fixture2 = &f;
        exit_steps2 = 0;
        auto before = snapshot2(f);
        LONG result = Kaffeine_RunCPU(&f.ctx, &f.cpu);
        check(static_cast<ULONG>(result) == code && f.state.d[0] == code &&
              static_cast<ULONG>(f.ctx.exit_code) == code && !f.ctx.running && exit_steps2 == 1,
              "Exit stops the real run loop and reports the signed D1 code through D0");
        check(snapshot2(f) == before && native_calls2() == 0,
              "Exit returns to native cleanup without native Exit or guest memory changes");
    }
    exit_fixture2 = nullptr;
}

struct BaseVector
{
    ULONG lvo;
    BaseCall call;
    LONG failure;
    std::vector<unsigned> strings;
};

/* Literal ABI values, independent of KAFFEINE_DOS_BASE_THUNKS. */
static const BaseVector base_vectors[] =
{
    { 30, open_call, 0, { 1 } },
    { 84, lock_call, 0, { 1 } },
    { 72, delete_call, 0, { 1 } },
    { 78, rename_call, 0, { 1, 2 } },
    { 150, loadseg_call, 0, { 1 } },
    { 768, newloadseg_call, 0, { 1 } },
    { 222, execute_call, 0, { 1 } },
    { 342, fputs_call, -1, { 2 } },
    { 948, putstr_call, -1, { 1 } },
    { 504, runcommand_call, -1, { 3 } }
};

static constexpr ULONG base_name = 0x500, base_extra = 0x580, base_buffer = 0x600,
    base_tags = 0x700, base_format = 0x780, base_args = 0x7C0;

static void prepare_base(Fixture &f)
{
    f.isolate_arena();
    calls.base.mem = f.ctx.mem;
    calls.base.mem_size = f.ctx.mem_size;
    std::strcpy(guest(f, base_name), "RAM:old");
    std::strcpy(guest(f, base_extra), "RAM:new");
    std::memset(f.ctx.mem + base_buffer, 0xA5, 64);
}

static void prepare_base(Fixture &f, const BaseVector &v)
{
    prepare_base(f);
    f.state.d[1] = base_name;
    f.state.d[2] = base_extra;
    switch (v.call)
    {
    case open_call:
        f.state.d[2] = 1006; /* MODE_NEWFILE */
        break;
    case lock_call:
        f.state.d[2] = 0xFFFFFFFE; /* SHARED_LOCK */
        break;
    case newloadseg_call:
        f.state.d[2] = base_tags;
        calls.taglist_converts = true;
        break;
    case execute_call:
        f.state.d[2] = 0x00223344;
        f.state.d[3] = 0x00556677;
        break;
    case fputs_call:
        f.state.d[1] = Fixture::file_handle;
        f.state.d[2] = base_name;
        break;
    case runcommand_call:
        std::strcpy(guest(f, base_extra), "one two\n");
        f.state.d[1] = 0x00334455;
        f.state.d[2] = 8192;
        f.state.d[3] = base_extra;
        f.state.d[4] = 8;
        break;
    default:
        break;
    }
}

/* Output() has no side effect, so VPrintf() may ask for it before checking. */
static unsigned base_native_calls()
{
    unsigned count = calls.taglist_converter_calls;
    for (unsigned n : calls.base.count)
        count += n;
    return count;
}

static void refuse_base(Fixture &f, ULONG lvo, LONG result, SIPTR error = ERROR_BAD_NUMBER)
{
    std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
    f.dispatch(lvo);
    check(f.state.d[0] == static_cast<ULONG>(result) && calls.ioerr == error,
        "refused base DOS call returns its failure value and IoErr");
    check(base_native_calls() == 0 && calls.alloc_mem_calls == 0 && calls.converter_calls == 0,
        "refused base DOS call reaches no native code and allocates nothing");
    check(std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
        "refused base DOS call leaves the entire guest arena unchanged");
}

static void test_base_valid()
{
    for (const auto &v : base_vectors)
    {
        Fixture f;
        prepare_base(f, v);
        ULONG first = f.state.d[v.strings[0]];
        std::string text = guest(f, first);
        f.dispatch(v.lvo);
        check(calls.base.count[v.call] == 1 &&
            base_native_calls() == (v.call == newloadseg_call ? 2U : 1U),
            "base DOS LVO reaches exactly its native call");
        check(calls.base.pointer == f.ctx.mem + first && calls.base.text == text,
            "the native call receives the checked guest string in place");
        check(f.state.d[0] == 0x1234 && calls.ioerr == calls.base.error,
            "the native result and IoErr reach the guest unchanged");
        switch (v.call)
        {
        case open_call:
        case lock_call:
            check(calls.base.mode == static_cast<LONG>(f.state.d[2]),
                "Open and Lock pass the D2 mode as a signed LONG");
            break;
        case rename_call:
            check(calls.base.second == f.ctx.mem + base_extra && calls.base.second_text == "RAM:new",
                "Rename passes the checked D2 name in place");
            break;
        case newloadseg_call:
            check(calls.taglist_guest == base_tags && calls.base.tags == calls.converted_tags &&
                calls.base.tags && calls.free_vec_calls == 1 && calls.freed_converted_tags,
                "NewLoadSeg converts the D2 tags, passes them on and frees them");
            break;
        case execute_call:
            check(calls.base.input == base_handle(0x00223344) &&
                calls.base.output == base_handle(0x00556677),
                "Execute passes the D2 input and D3 output handles");
            break;
        case fputs_call:
            check(calls.base.handle == base_handle(Fixture::file_handle),
                "FPuts passes the D1 handle");
            break;
        case runcommand_call:
            check(calls.base.handle == base_handle(0x00334455) && calls.base.stack == 8192 &&
                calls.base.argsize == 8, "RunCommand passes the seglist, stack and D4 size");
            break;
        default:
            break;
        }
    }
}

static void test_base_strings_refused()
{
    for (const auto &v : base_vectors)
    {
        for (unsigned reg : v.strings)
        {
            for (ULONG bad : { 0U, 4096U, 4097U, 0x80000000U, 0xFFFFFFFFU })
            {
                Fixture f;
                prepare_base(f, v);
                f.state.d[reg] = bad;
                refuse_base(f, v.lvo, v.failure);
            }
            Fixture f;
            prepare_base(f, v);
            std::memset(f.ctx.mem + f.ctx.mem_size - 8, 'x', 8);
            f.state.d[reg] = f.ctx.mem_size - 8;
            refuse_base(f, v.lvo, v.failure);
        }
    }
}

static void test_base_read_write()
{
    for (ULONG lvo : { 42U, 48U })
    {
        BaseCall call = lvo == 42 ? read_call : write_call;
        {
            Fixture f;
            prepare_base(f);
            f.state.d[2] = base_buffer;
            f.state.d[3] = 16;
            f.dispatch(lvo);
            check(calls.base.count[call] == 1 && base_native_calls() == 1 &&
                calls.base.handle == base_handle(Fixture::file_handle) &&
                calls.base.pointer == f.ctx.mem + base_buffer && calls.base.length == 16 &&
                f.state.d[0] == 0x1234 && calls.ioerr == calls.base.error,
                "Read and Write pass the D1 handle, the guest buffer in place and the D3 length");
            if (call == read_call)
                check(f.ctx.mem[base_buffer] == 0x5C && f.ctx.mem[base_buffer + 15] == 0x5C &&
                    f.ctx.mem[base_buffer + 16] == 0xA5 && f.ctx.mem[base_buffer - 1] == 0,
                    "Read fills exactly the checked range");
            else
                check(calls.base.text == std::string(16, '\xA5'), "Write reads exactly the checked range");
        }
        {
            Fixture f;
            prepare_base(f);
            f.state.d[2] = f.ctx.mem_size - 8;
            f.state.d[3] = 8;
            f.dispatch(lvo);
            check(calls.base.count[call] == 1 && calls.base.length == 8 &&
                calls.base.pointer == f.ctx.mem + f.ctx.mem_size - 8,
                "a buffer that ends exactly at the end of guest memory is accepted");
        }
        {
            Fixture f;
            prepare_base(f);
            f.state.d[1] = 0;
            f.state.d[2] = base_buffer;
            f.state.d[3] = 4;
            f.dispatch(lvo);
            check(calls.base.count[call] == 1 && calls.base.handle == nullptr,
                "a NULL handle reaches native DOS, which reports it");
        }
        for (ULONG pointer : { 0U, base_buffer, 4096U, 0xFFFFFFFFU })
        {
            Fixture f;
            prepare_base(f);
            f.state.d[2] = pointer;
            f.state.d[3] = 0;
            std::vector<UBYTE> before(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
            f.dispatch(lvo);
            const UBYTE *inside = pointer == base_buffer ? f.ctx.mem + base_buffer : f.ctx.mem;
            check(calls.base.count[call] == 1 && base_native_calls() == 1 &&
                calls.base.length == 0 && calls.base.pointer == inside &&
                calls.base.handle == base_handle(Fixture::file_handle) &&
                f.state.d[0] == 0x1234 && calls.ioerr == calls.base.error,
                "a zero length reaches DOS with the handle and a pointer inside guest memory");
            check(std::memcmp(before.data(), f.ctx.mem, before.size()) == 0,
                "a zero length moves no guest byte");
        }
        {
            Fixture f;
            prepare_base(f);
            f.state.d[1] = 0;
            f.state.d[2] = 0;
            f.state.d[3] = 0;
            f.dispatch(lvo);
            check(calls.base.count[call] == 1 && calls.base.handle == nullptr && calls.base.length == 0,
                "a zero length with a NULL handle still reaches DOS, which refuses the handle");
        }
        const struct { ULONG buffer, length; } bad[] =
        {
            { 0, 4 }, { 4096, 1 }, { 4097, 1 }, { 0x80000000, 4 }, { 0xFFFFFFFF, 1 },
            { 4096 - 8, 9 }, { 1, 4096 }, { 0x100, 0x7FFFFFFF },
            { base_buffer, 0x80000000 }, { base_buffer, 0xFFFFFFFF }
        };
        for (const auto &b : bad)
        {
            Fixture f;
            prepare_base(f);
            f.state.d[2] = b.buffer;
            f.state.d[3] = b.length;
            refuse_base(f, lvo, -1);
        }
    }
}

static void test_base_runcommand()
{
    const BaseVector v = { 504, runcommand_call, -1, { 3 } };
    {
        Fixture f;
        prepare_base(f, v);
        f.state.d[3] = 0;
        f.state.d[4] = 0;
        f.dispatch(504);
        check(calls.base.count[runcommand_call] == 1 && calls.base.pointer == nullptr &&
            calls.base.argsize == 0, "RunCommand without arguments passes NULL and zero");
    }
    for (ULONG size : { 0U, 64U })
    {
        Fixture f;
        prepare_base(f, v);
        f.state.d[4] = size;
        f.dispatch(504);
        check(calls.base.count[runcommand_call] == 1 && calls.base.pointer == f.ctx.mem + base_extra &&
            calls.base.argsize == size,
            "RunCommand accepts any size whose bytes are guest memory");
    }
    {
        Fixture f;
        prepare_base(f, v);
        std::strcpy(guest(f, f.ctx.mem_size - 8), "abc");
        f.state.d[3] = f.ctx.mem_size - 8;
        f.state.d[4] = 9;
        refuse_base(f, 504, -1);
    }
    {
        Fixture f;
        prepare_base(f, v);
        f.state.d[4] = 0xFFFFFFFF;
        refuse_base(f, 504, -1);
    }
}

static void test_base_cli_init()
{
    for (ULONG lvo : { 930U, 936U })
    {
        for (ULONG packet : { 0U, base_buffer })
        {
            Fixture f;
            prepare_base(f);
            f.state.a[0] = packet;
            refuse_base(f, lvo, 0, ERROR_NOT_IMPLEMENTED);
        }
    }
}

static void prepare_print(Fixture &f, ULONG lvo, const char *format)
{
    prepare_base(f);
    std::strcpy(guest(f, base_format), format);
    unsigned reg = lvo == 354 ? 2 : 1;
    if (lvo == 354)
        f.state.d[1] = Fixture::file_handle;
    f.state.d[reg] = base_format;
    f.state.d[reg + 1] = base_args;
}

static void test_base_print()
{
    for (ULONG lvo : { 354U, 954U })
    {
        BPTR handle = lvo == 354 ? base_handle(Fixture::file_handle) : BaseCalls().output_handle;
        unsigned reg = lvo == 354 ? 2 : 1;
        {
            Fixture f;
            prepare_print(f, lvo, "%s=%lx%%\n");
            m68k_write32(&f.ctx, base_args, base_name);
            m68k_write32(&f.ctx, base_args + 4, 0xBEEF);
            f.dispatch(lvo);
            check(calls.base.printed == "RAM:old=BEEF%\n" && f.state.d[0] == 14 &&
                calls.base.printed_to == handle,
                "VFPrintf and VPrintf render on the bridge and write to their handle");
            check(calls.base.output_calls == (lvo == 954 ? 1U : 0U),
                "only VPrintf asks for Output()");
            check(calls.alloc_mem_calls == 0 && calls.free_mem_calls == 0,
                "the text goes out without a native buffer");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "a%cb");
            m68k_write16(&f.ctx, base_args, 0);
            f.dispatch(lvo);
            check(calls.base.printed == "ab" && f.state.d[0] == 2,
                "a zero character is neither written nor counted");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "abcd");
            calls.base.fputc_fails_at = 2;
            SIPTR error = calls.ioerr;
            f.dispatch(lvo);
            check(f.state.d[0] == 0xFFFFFFFF && calls.base.count[fputc_call] == 3 &&
                calls.base.printed == "ab" && calls.ioerr == error,
                "the first failed write returns -1, stops the output and keeps IoErr");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "abc");
            if (lvo == 354)
                f.state.d[1] = 0;
            else
                calls.base.output_handle = nullptr;
            f.dispatch(lvo);
            check(f.state.d[0] == 0xFFFFFFFF && calls.base.count[fputc_call] == 1 &&
                calls.base.printed_to == nullptr,
                "a NULL handle reaches FPutC, which refuses it");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "");
            if (lvo == 354)
                f.state.d[1] = 0;
            else
                calls.base.output_handle = nullptr;
            f.dispatch(lvo);
            check(f.state.d[0] == 0 && calls.base.count[fputc_call] == 0,
                "an empty text returns 0 without writing, as in AROS");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "%60000ld");
            m68k_write32(&f.ctx, base_args, 7);
            f.dispatch(lvo);
            check(f.state.d[0] == 60000 && calls.base.printed.size() == 60000 &&
                calls.base.printed.back() == '7' && calls.alloc_mem_calls == 0,
                "a wide guest field goes out character by character, without native memory");
        }
        {
            Fixture f;
            prepare_print(f, lvo, "%60000ld");
            m68k_write32(&f.ctx, base_args, 7);
            calls.base.fputc_fails_at = 2;
            f.dispatch(lvo);
            check(f.state.d[0] == 0xFFFFFFFF && calls.base.count[fputc_call] == 3,
                "a failed write in a wide field stops the writes but not the formatting");
        }
        for (ULONG bad : { 0U, 4096U, 0xFFFFFFFFU, 4096U - 8U })
        {
            Fixture f;
            prepare_print(f, lvo, "abc");
            std::memset(f.ctx.mem + f.ctx.mem_size - 8, 'x', 8);
            f.state.d[reg] = bad;
            refuse_base(f, lvo, -1);
        }
        const struct { const char *format; ULONG args, first; } bad_args[] =
        {
            { "%s", base_args, 4096 },
            { "%s", base_args, 4096 - 8 },
            { "%ld", base_args + 1, 7 },
            { "%ld", 4096 - 2, 7 },
            { "%d", 0, 7 }
        };
        for (const auto &b : bad_args)
        {
            Fixture f;
            prepare_print(f, lvo, b.format);
            std::memset(f.ctx.mem + f.ctx.mem_size - 8, 'x', 8);
            if (b.args && b.args <= 4096 - 4)
                m68k_write32(&f.ctx, b.args, b.first);
            f.state.d[reg + 1] = b.args;
            refuse_base(f, lvo, -1);
        }
    }
}

static void test_base_vfwritef()
{
    for (ULONG bad : { 0U, 4096U, 0xFFFFFFFFU, 4096U - 8U })
    {
        Fixture f;
        prepare_base(f);
        std::memset(f.ctx.mem + f.ctx.mem_size - 8, 'x', 8);
        f.state.d[2] = bad;
        refuse_base(f, 348, 0);
        check(calls.writef_calls == 0, "VFWritef never reaches native code with an invalid format");
    }
}

static void test_dos_base()
{
    test_base_valid();
    test_base_strings_refused();
    test_base_read_write();
    test_base_runcommand();
    test_base_cli_init();
    test_base_print();
    test_base_vfwritef();
}

static constexpr ULONG match_pattern = 0x500, match_ap = 0x1000, match_size = 0x10000;

/* A guest AnchorPath with ap_Strlen bytes of 'Q' and a canary after them. */
static void put_anchor(Fixture &f, ULONG ap, UWORD len, ULONG breakbits = 0)
{
    std::memset(f.ctx.mem + ap, 0, M68K_AP_BUF);
    m68k_write32(&f.ctx, ap + M68K_AP_BREAKBITS, breakbits);
    m68k_write16(&f.ctx, ap + M68K_AP_STRLEN, len);
    if (len < 0x8000)
    {
        ULONG room = f.ctx.mem_size - (ap + M68K_AP_BUF);
        std::memset(f.ctx.mem + ap + M68K_AP_BUF, 'Q', len < room ? len : room);
        if (len < room)
            f.ctx.mem[ap + M68K_AP_BUF + len] = 0xA5;
    }
}

static void prepare_match(Fixture &f, UWORD len = 64)
{
    f.isolate_arena(match_size);
    calls.base.mem = f.ctx.mem;
    calls.base.mem_size = f.ctx.mem_size;
    std::strcpy(guest(f, match_pattern), "RAM:dir/#?");
    put_anchor(f, match_ap, len);
}

static ULONG match_first(Fixture &f, ULONG ap = match_ap, ULONG pattern = match_pattern)
{
    return call2(f, 822, { pattern, ap });
}

static ULONG match_next(Fixture &f, ULONG ap = match_ap)
{
    return call2(f, 828, { ap });
}

static void match_end(Fixture &f, ULONG ap = match_ap)
{
    call2(f, 834, { ap });
}

static ULONG read32(const Fixture &f, ULONG addr)
{
    return m68k_read32(const_cast<KaffeineContext *>(&f.ctx), addr);
}

static std::string guest_name(Fixture &f, ULONG fib)
{
    return guest(f, fib + M68K_FIB_FILENAME);
}

/* Everything a match left behind is released, once. */
static bool match_released()
{
    const auto &m = calls.match;
    return m.live.empty() && m.achain_live.empty() && calls.vec_live.empty() &&
        !m.achain_double_free && !calls.vec_double_free;
}

static void test_match_first()
{
    Fixture f;
    prepare_match(f);
    m68k_write32(&f.ctx, match_ap + M68K_AP_BREAKBITS, 1U << 12);
    f.ctx.mem[match_ap + M68K_AP_FLAGS] = 0x40;
    check(match_first(f) == 0 && calls.ioerr == 0, "MatchFirst returns the native result and IoErr");
    const auto &m = calls.match;
    check(m.first == 1 && m.pattern_pointer == f.ctx.mem + match_pattern && m.pattern == "RAM:dir/#?",
        "MatchFirst passes the checked guest pattern in place");
    check(m.ap && !base_inside(m.ap, 1) && m.strlen_in == 64 && m.breakbits_in == (1 << 12),
        "native DOS gets a native AnchorPath with the guest ap_Strlen and ap_BreakBits");
    check(m.flags_in == 0 && read32(f, match_ap + M68K_AP_FLAGS) >> 24 == APF_ITSWILD,
        "ap_Flags starts at zero as in AROS and the native flags come back");
    check(m.buf_in == std::string(64, 'Q') && calls.last_vec_size == sizeof(AnchorPath) + 64,
        "the native buffer holds the guest bytes and has room for ap_Strlen bytes");
    ULONG base = read32(f, match_ap + M68K_AP_BASE), current = read32(f, match_ap + M68K_AP_LAST);
    check(base && base == current && base >= 0x8000 && m.achain_live.size() == 2,
        "ap_Base and ap_Current point to one guest copy of the only AChain");
    check(read32(f, current + M68K_AN_LOCK) == 0x00A1B2C0 && read32(f, current + M68K_AN_CHILD) == 0 &&
        read32(f, current + M68K_AN_PARENT) == 0 &&
        f.ctx.mem[current + M68K_AN_FLAGS] == (DDF_PatternBit | DDF_ExaminedBit) &&
        f.ctx.mem[current + M68K_AN_STRING] == 0,
        "the AChain copy holds the native lock, no links, the flags and an empty an_String");
    check(guest_name(f, current + M68K_AN_INFO) == "a.txt" &&
        read32(f, current + M68K_AN_INFO + M68K_FIB_DIRENTRYTYPE) == 0xFFFFFFFD,
        "the AChain copy holds the converted FIB of the entry");
    check(guest_name(f, match_ap + M68K_AP_INFO) == "a.txt" &&
        read32(f, match_ap + M68K_AP_INFO + M68K_FIB_SIZE) == 500,
        "ap_Info is the converted FIB of the first match");
    check(std::strcmp(guest(f, match_ap + M68K_AP_BUF), "RAM:dir/a.txt") == 0 &&
        f.ctx.mem[match_ap + M68K_AP_BUF + 14] == 'Q' && f.ctx.mem[match_ap + M68K_AP_BUF + 63] == 'Q' &&
        f.ctx.mem[match_ap + M68K_AP_BUF + 64] == 0xA5,
        "ap_Buf gets the path, the bytes after it stay, and nothing past ap_Strlen changes");
    check(read32(f, match_ap + M68K_AP_FOUNDBREAK) == 0, "ap_FoundBreak comes back");
    Kaffeine_MatchCleanup(&f.ctx);
    check(m.end == 1 && match_released(), "the context cleanup ends an open match and frees it");
}

static void test_match_next_and_dirs()
{
    Fixture f;
    prepare_match(f);
    match_first(f);
    ULONG first_copy = read32(f, match_ap + M68K_AP_BASE);
    check(match_next(f) == 0 && guest_name(f, match_ap + M68K_AP_INFO) == "dir" &&
        read32(f, match_ap + M68K_AP_INFO + M68K_FIB_DIRENTRYTYPE) == 2,
        "MatchNext returns the next entry");
    f.ctx.mem[match_ap + M68K_AP_FLAGS] |= APF_DODIR;
    check(match_next(f) == 0 && (calls.match.flags_in & APF_DODIR),
        "APF_DODIR set by the program reaches native DOS");
    ULONG base = read32(f, match_ap + M68K_AP_BASE), current = read32(f, match_ap + M68K_AP_LAST);
    check(base == first_copy && current && current != base &&
        read32(f, base + M68K_AN_CHILD) == current && read32(f, current + M68K_AN_PARENT) == base &&
        read32(f, base + M68K_AN_PARENT) == 0 && read32(f, current + M68K_AN_CHILD) == 0,
        "inside a directory ap_Base and ap_Current are two linked copies");
    check(read32(f, base + M68K_AN_LOCK) == 0x00A1B2C0 && read32(f, current + M68K_AN_LOCK) == 0x00C3D4E0,
        "each copy holds the lock of its own AChain");
    check(guest_name(f, match_ap + M68K_AP_INFO) == "c.txt" &&
        std::strcmp(guest(f, match_ap + M68K_AP_BUF), "RAM:dir/dir/c.txt") == 0 &&
        !(f.ctx.mem[match_ap + M68K_AP_FLAGS] & APF_DODIR),
        "the entry inside comes back and native DOS clears APF_DODIR");
    check(match_next(f) == 0 && (f.ctx.mem[match_ap + M68K_AP_FLAGS] & APF_DIDDIR) &&
        guest_name(f, match_ap + M68K_AP_INFO) == "dir" &&
        read32(f, match_ap + M68K_AP_BASE) == read32(f, match_ap + M68K_AP_LAST) &&
        read32(f, base + M68K_AN_CHILD) == 0,
        "leaving the directory reports it again with APF_DIDDIR and one AChain");
    check(match_next(f) == 0 && guest_name(f, match_ap + M68K_AP_INFO) == "b.txt",
        "MatchNext goes on after the directory");
    std::strcpy(guest(f, match_ap + M68K_AP_INFO + M68K_FIB_COMMENT), "kept");
    check(match_next(f) == ERROR_NO_MORE_ENTRIES && calls.ioerr == ERROR_NO_MORE_ENTRIES,
        "the end of the match returns ERROR_NO_MORE_ENTRIES");
    check(std::strcmp(guest(f, match_ap + M68K_AP_INFO + M68K_FIB_COMMENT), "kept") == 0,
        "ap_Info is left alone when native DOS does not write it");
    unsigned ends = calls.match.end;
    match_end(f);
    check(calls.match.end == ends + 1 && read32(f, match_ap + M68K_AP_BASE) == 0 &&
        read32(f, match_ap + M68K_AP_LAST) == 0 && match_released(),
        "MatchEnd ends the native match, clears both pointers and frees everything");
    match_end(f);
    check(calls.match.end == ends + 1 && read32(f, match_ap + M68K_AP_BASE) == 0,
        "a second MatchEnd has nothing to end");
    unsigned nexts = calls.match.next;
    check(match_next(f) == ERROR_BAD_NUMBER && calls.ioerr == ERROR_BAD_NUMBER &&
        calls.match.next == nexts,
        "MatchNext without an open match is refused");
}

static void refused_match(Fixture &f, ULONG lvo, std::initializer_list<ULONG> args,
                          ULONG result, SIPTR error = ERROR_BAD_NUMBER)
{
    auto before = snapshot2(f);
    unsigned first = calls.match.first, next = calls.match.next, allocs = calls.alloc_vec_calls;
    check(call2(f, lvo, args) == result && calls.ioerr == error,
        "a refused match returns its error code and sets IoErr");
    check(calls.match.first == first && calls.match.next == next && calls.alloc_vec_calls == allocs,
        "a refused match reaches no native code and allocates nothing");
    check(snapshot2(f) == before, "a refused match leaves the guest arena unchanged");
}

static void test_match_refusals()
{
    for (ULONG bad : { 0U, match_size, match_size + 1, 0xFFFFFFFFU, match_size - 8 })
    {
        Fixture f;
        prepare_match(f);
        std::memset(f.ctx.mem + match_size - 8, 'x', 8);
        refused_match(f, 822, { bad, match_ap }, ERROR_BAD_NUMBER);
    }
    for (ULONG bad : { 0U, match_size, match_size - M68K_AP_BUF + 1, 0xFFFFFFFFU })
    {
        Fixture f;
        prepare_match(f);
        refused_match(f, 822, { match_pattern, bad }, ERROR_BAD_NUMBER);
    }
    for (UWORD len : { UWORD(0xFFFF), UWORD(0x8000), UWORD(65) })
    {
        Fixture f;
        prepare_match(f);
        ULONG ap = match_size - M68K_AP_BUF - 64;
        put_anchor(f, ap, len);
        refused_match(f, 822, { match_pattern, ap }, ERROR_BAD_NUMBER);
    }
    {
        Fixture f;
        prepare_match(f);
        refused_match(f, 828, { match_ap }, ERROR_BAD_NUMBER);
        refused_match(f, 828, { 0 }, ERROR_BAD_NUMBER);
    }
    {
        Fixture f;
        prepare_match(f);
        match_first(f);
        m68k_write16(&f.ctx, match_ap + M68K_AP_STRLEN, 65);
        refused_match(f, 828, { match_ap }, ERROR_BAD_NUMBER);
        m68k_write16(&f.ctx, match_ap + M68K_AP_STRLEN, 0xFFFF);
        refused_match(f, 828, { match_ap }, ERROR_BAD_NUMBER);
        m68k_write16(&f.ctx, match_ap + M68K_AP_STRLEN, 16);
        check(match_next(f) == 0 && calls.match.strlen_in == 16,
            "a smaller ap_Strlen still works after a refused call");
        Kaffeine_MatchCleanup(&f.ctx);
        check(match_released(), "the refused calls left the match intact");
    }
    {
        Fixture f;
        prepare_match(f);
        match_first(f);
        put_anchor(f, match_ap + 0x100, 64);
        refused_match(f, 822, { match_pattern, match_ap + 0x100 }, ERROR_BAD_NUMBER);
        ULONG next_to = match_ap + M68K_AP_BUF + 64;
        put_anchor(f, next_to, 64);
        check(match_first(f, next_to) == 0, "an AnchorPath right after an open one is accepted");
        Kaffeine_MatchCleanup(&f.ctx);
        check(calls.match.end == 2 && match_released(), "the cleanup ends both matches");
    }
}

static void test_match_resources()
{
    {
        Fixture f;
        prepare_match(f);
        ULONG ap = match_ap;
        for (unsigned i = 0; i < KAFFEINE_MAX_ANCHORS; ++i, ap += 0x200)
        {
            put_anchor(f, ap, 64);
            match_first(f, ap);
        }
        put_anchor(f, ap, 64);
        m68k_write32(&f.ctx, ap + M68K_AP_BASE, 0x12345678);
        unsigned first = calls.match.first;
        check(match_first(f, ap) == ERROR_NO_FREE_STORE && calls.ioerr == ERROR_NO_FREE_STORE &&
            calls.match.first == first && read32(f, ap + M68K_AP_BASE) == 0,
            "a seventeenth open match fails with ERROR_NO_FREE_STORE after clearing ap_Base");
        Kaffeine_MatchCleanup(&f.ctx);
        check(calls.match.end == KAFFEINE_MAX_ANCHORS && match_released(),
            "the cleanup ends all sixteen matches");
    }
    for (unsigned fail = 0; fail < 3; ++fail)
    {
        Fixture f;
        prepare_match(f);
        if (fail == 0)
            calls.alloc_vec_fails = true;
        else
            calls.match.achain_fails_at = fail;
        check(match_first(f) == ERROR_NO_FREE_STORE && calls.ioerr == ERROR_NO_FREE_STORE &&
            calls.match.first == 0 && match_released(),
            "allocation failures return ERROR_NO_FREE_STORE and free what was allocated");
        calls.alloc_vec_fails = false;
        calls.match.achain_fails_at = 0;
        check(match_first(f) == 0, "the AnchorPath works once memory is available");
        Kaffeine_MatchCleanup(&f.ctx);
        check(match_released(), "nothing leaks after an allocation failure");
    }
}

static void test_match_lifecycle()
{
    {
        Fixture f;
        prepare_match(f);
        calls.match.build_fails = true;
        check(match_first(f) == ERROR_OBJECT_NOT_FOUND && calls.ioerr == ERROR_OBJECT_NOT_FOUND,
            "a failed MatchFirst keeps the native error and IoErr");
        check(calls.match.end == 1 && match_released() && read32(f, match_ap + M68K_AP_BASE) == 0 &&
            read32(f, match_ap + M68K_AP_LAST) == 0,
            "without a native chain the match is released at once");
        check(match_next(f) == ERROR_BAD_NUMBER, "MatchNext after it is refused");
    }
    {
        Fixture f;
        prepare_match(f);
        calls.match.names.clear();
        check(match_first(f) == ERROR_NO_MORE_ENTRIES && read32(f, match_ap + M68K_AP_BASE) != 0 &&
            calls.match.end == 0,
            "a match without entries keeps its chain until MatchEnd, as in AROS");
        match_end(f);
        check(calls.match.end == 1 && match_released(), "MatchEnd releases it");
    }
    {
        Fixture f;
        prepare_match(f);
        match_first(f);
        calls.match.index = 0;
        check(match_first(f) == 0 && calls.match.end == 1 && calls.match.first == 2,
            "a second MatchFirst on an open AnchorPath ends the first match");
        Kaffeine_MatchCleanup(&f.ctx);
        check(calls.match.end == 2 && match_released(), "only the second match was still open");
    }
    {
        Fixture f;
        prepare_match(f, 0);
        f.ctx.mem[match_ap + M68K_AP_BUF] = 0x5A;
        check(match_first(f) == 0 && calls.match.strlen_in == 0 &&
            calls.last_vec_size == sizeof(AnchorPath) && f.ctx.mem[match_ap + M68K_AP_BUF] == 0x5A,
            "with ap_Strlen 0 no buffer is used or written");
        Kaffeine_MatchCleanup(&f.ctx);
    }
    {
        Fixture f;
        prepare_match(f, 8);
        check(match_first(f) == ERROR_LINE_TOO_LONG && guest_name(f, match_ap + M68K_AP_INFO) == "a.txt" &&
            f.ctx.mem[match_ap + M68K_AP_BUF] == 0 && f.ctx.mem[match_ap + M68K_AP_BUF + 8] == 0xA5,
            "a path too long for ap_Buf fails as AddPart does, with ap_Info written");
        Kaffeine_MatchCleanup(&f.ctx);
    }
    {
        Fixture f;
        prepare_match(f);
        m68k_write32(&f.ctx, match_ap + M68K_AP_BREAKBITS, 1U << 12);
        match_first(f);
        calls.match.signals = 1 << 12;
        check(match_next(f) == ERROR_BREAK && read32(f, match_ap + M68K_AP_FOUNDBREAK) == (1U << 12),
            "a break signal returns ERROR_BREAK with ap_FoundBreak");
        Kaffeine_MatchCleanup(&f.ctx);
        check(match_released(), "the broken match is released by the cleanup");
    }
}

static void test_dos_match()
{
    test_match_first();
    test_match_next_and_dirs();
    test_match_refusals();
    test_match_resources();
    test_match_lifecycle();
}

static void test_dos_batch2()
{
    test_dos2_named_calls();
    test_dos2_info_dates();
    test_dos2_datetime();
    test_dos2_split_find();
    test_dos2_readitem();
    test_dos2_vbuf_writechars();
    test_dos2_arguments_cli();
    test_dos2_exit();
}

int main(int argc, char **argv)
{
    bool valid_only = argc == 2 && std::strcmp(argv[1], "--valid-data-only") == 0;
    if (argc == 2 && std::strcmp(argv[1], "--dos2-only") == 0)
    {
        test_dos_batch2();
        std::printf("DOS batch 2 regressions: %u/%u checks passed\n", checks - failures, checks);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--match-only") == 0)
    {
        test_dos_match();
        std::printf("DOS match regressions: %u/%u checks passed\n", checks - failures, checks);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--base-only") == 0)
    {
        test_dos_base();
        std::printf("DOS base call regressions: %u/%u checks passed\n", checks - failures, checks);
        return failures ? 1 : 0;
    }
    if (argc > 1 && !valid_only)
        return 2;
    if (!valid_only)
    {
        test_dos_batch2();
        test_dos_base();
        test_dos_match();
    }
    test_vfprintf();
    test_vfwritef();
    test_free_dos_object();
    test_free_rdargs();
    test_strtolong();
    test_datestamp();
    test_filepart();
    test_pathpart();
    test_pathpart_native_results();
    test_fgets();
    test_examinefh();
    test_examine_lock();
    test_exnext_end();
    test_process_launch();
    test_exall_and_packets_refused();
    test_system_taglist_lvo();
    test_buffer_dispatch();
    /* The valid-only mode can demonstrate regressions against the old raw
       generated thunks without deliberately handing them invalid pointers. */
    if (!valid_only)
    {
        test_output_boundaries();
        test_invalid_outputs();
        test_invalid_strings();
        test_fgets_bounds();
        test_examinefh_failures();
        test_datestamp_failure();
        test_examine_lock_invalid();
        test_system_taglist_invalid();
        test_buffer_bad_addresses();
        test_buffer_edges_and_lengths();
        test_buffer_strings_and_failures();
        test_addpart_and_fault_edges();
        test_pattern_buffers();
    }
    std::printf("DOS thunk regressions: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
