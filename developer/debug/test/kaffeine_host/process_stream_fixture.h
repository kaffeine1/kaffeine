/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Verbatim production Init/Drop, real FileRecords/Close and heap. The native
   exit collaborator saves identities before Init and closes those saved
   values using live flags, as DosEntry does, never the later selections. */
#pragma once

static constexpr ULONG stream_close_all=PRF_CLOSEINPUT|PRF_CLOSEOUTPUT|PRF_CLOSEERROR;
static constexpr ULONG stream_flags[]={PRF_CLOSEINPUT,PRF_CLOSEOUTPUT,PRF_CLOSEERROR};

struct StreamOwner
{
    Fixture &f;
    Process process={};
    std::array<BPTR,3> saved;
    bool dropped=false;
    StreamOwner(Fixture &fixture,std::array<BPTR,3> startup,ULONG flags=stream_close_all)
        :f(fixture),saved(startup)
    {
        process.pr_Task.tc_Node.ln_Type=NT_PROCESS;
        process.pr_CIS=saved[0];process.pr_COS=saved[1];process.pr_CES=saved[2];
        process.pr_Flags=flags;
        process_native_streams=true;
    }
    template<class Job> auto job(Job fn)
    {
        auto *old_task=native_caller,*old_owner=f.ctx.broker_owner;
        BOOL old_call=f.ctx.broker_call;
        native_caller=&process.pr_Task;f.ctx.broker_call=TRUE;f.ctx.broker_owner=native_caller;
        auto result=fn();
        f.ctx.broker_call=old_call;f.ctx.broker_owner=old_owner;native_caller=old_task;
        return result;
    }
    IPTR init() { return job([&] { return Kaffeine_ProcessInitGuestDos(&f.ctx,&f.cpu); }); }
    void drop()
    {
        SIPTR error=ioerr;
        check(job([&] { return Kaffeine_ProcessDropGuestDos(&f.ctx,nullptr); })==1 && ioerr==error,
              "real Drop retires this owner's captured aliases and preserves full-width IoErr");
        dropped=true;
    }
    void native_exit(bool free_native=false)
    {
        check(dropped,"native exit follows synchronous alias retirement ACK");
        for(unsigned i=0;i<saved.size();++i)
            if(saved[i] && (process.pr_Flags&stream_flags[i]))
            {
                if(free_native)close_frees=saved[i];
                Close(saved[i]);
            }
    }
};

static KaffeineFile *stream_record(Fixture &f,BPTR native)
{
    for(auto &record:f.ctx.files)if(record.native==native)return &record;
    return nullptr;
}
static BPTR stream_native(Fixture &f)
{
    auto *handle=static_cast<FileHandle *>(AllocMem(sizeof(FileHandle),MEMF_CLEAR));
    assert(handle);*handle=f.native[2];return MKBADDR(handle);
}
static void stream_sweep(Fixture &f)
{
    check(Kaffeine_FileAllPrepare(&f.ctx) && Kaffeine_FileAllFinish(&f.ctx),
          "real all-file preparation/snapshot never traverses a native-freed startup file");
}

static void selected_replacements_survive_exit()
{
    for(bool duplicate:{false,true})
    {
        Fixture f;
        BPTR first=stream_native(f);
        std::array<BPTR,3> originals={first,duplicate?first:stream_native(f),duplicate?first:stream_native(f)};
        StreamOwner owner(f,originals);
        const SIPTR wide_error=static_cast<SIPTR>(0x1234567887654321ULL);
        ioerr=wide_error;
        check(owner.init()==1 && ioerr==wide_error && f.ctx.process_file_close,
              "production Init captures native originals before importing borrowed aliases");
        auto *capture=f.ctx.process_streams;unsigned before_allocs=alloc_calls;
        check(!owner.init() && f.ctx.process_streams==capture && alloc_calls==before_allocs,
              "repeat Init never recaptures replacement selections");
        BPTR replacement=MKBADDR(&f.native[10]);
        ULONG token=Kaffeine_FileImport(&f.ctx,replacement,TRUE);
        owner.job([&] {
            f.dos(294,token);f.dos(300,token);owner.process.pr_CES=replacement;return 1;
        });
        check(owner.process.pr_CIS==replacement && owner.process.pr_COS==replacement &&
              stream_record(f,replacement)->owned,
              "selected guest-owned replacement retains its original owned FileRecord");
        /* The paused driver can also select an imported child's original. */
        native_process.pr_CIS=native_process.pr_COS=native_process.pr_CES=first;
        ioerr=wide_error;owner.drop();
        check(!native_process.pr_CIS && !native_process.pr_COS && !native_process.pr_CES &&
              !f.ctx.process_streams && !f.ctx.process_file_close,
              "Drop detaches driver aliases and resets the hook after the final capture");
        for(BPTR original:originals)check(!stream_record(f,original),
              "only captured originals scheduled for native close have lost their aliases");
        check(stream_record(f,replacement) && stream_record(f,replacement)->owned && !closes,
              "Drop does not close or forget the selected replacement");
        owner.native_exit(true);
        check(closes==(duplicate?1U:3U),"native exit closes each distinct saved original exactly once");
        stream_sweep(f);
        check(f.dos(42,token,f.buffer,1)==1 && seen_handle==replacement,
              "the replacement remains usable after original native storage was freed");
        Kaffeine_FileCleanup(&f.ctx);
        check(closes==(duplicate?2U:4U),"Context cleanup closes the retained owned replacement once");
    }
}

static void explicitly_closed_startup_reuse()
{
    for(unsigned caller=0;caller<3;++caller)
        for(bool native_success:{false,true})
        {
            Fixture f;
            BPTR original=MKBADDR(&f.native[12]),replacement=MKBADDR(&f.native[13]);
            StreamOwner a(f,{original,original,original}),b(f,{original,original,original});
            check(a.init()==1 && b.init()==1,"two live captured owners share one original native identity");
            ULONG old_token=stream_record(f,original)->guest>>2;
            ULONG selected=Kaffeine_FileImport(&f.ctx,replacement,TRUE);
            a.job([&] { f.dos(294,selected);f.dos(300,selected);a.process.pr_CES=replacement;return 1; });
            native_process.pr_CIS=native_process.pr_COS=native_process.pr_CES=original;
            close_result=native_success?-1:0;
            auto close_original=[&] { return f.dos(36,old_token); };
            ULONG result=caller==0?close_original():(caller==1?a.job(close_original):b.job(close_original));
            check(result==static_cast<ULONG>(close_result) && closes==1 &&
                  !(a.process.pr_Flags&stream_close_all) && !(b.process.pr_Flags&stream_close_all),
                  "explicit Close disarms every matching owner/slot even when native Close reports failure");
            check(!stream_record(f,original) && !native_process.pr_CIS && !native_process.pr_COS &&
                  !native_process.pr_CES && !b.process.pr_CIS && !b.process.pr_COS && !b.process.pr_CES,
                  "Close retires the old token and detaches other live owners before native reuse");
            /* Deterministic native allocator reuse of the very same BPTR. */
            f.native[12]=f.native[14];
            ULONG fresh=Kaffeine_FileImport(&f.ctx,original,TRUE);
            check(fresh && fresh!=old_token && stream_record(f,original)->owned,
                  "same native BPTR imports as a fresh quarantined owned identity");
            a.drop();a.native_exit();
            check(f.ctx.process_streams && f.ctx.process_file_close,
                  "first owner exit keeps the remaining owner's capture and close hook live");
            b.drop();b.native_exit();
            check(closes==1 && stream_record(f,original) && stream_record(f,replacement) &&
                  stream_record(f,original)->owned && stream_record(f,replacement)->owned &&
                  !f.ctx.process_streams && !f.ctx.process_file_close,
                  "both exits preserve replacement and reused owned files without a second native close");
            stream_sweep(f);
            Kaffeine_FileCleanup(&f.ctx);
            check(closes==3,"only final owned cleanup closes the replacement and fresh reused handle");
            close_result=-1;
        }
}

static void partial_and_unarmed_startup()
{
    {
        Fixture f;
        std::array<BPTR,3> originals={stream_native(f),stream_native(f),stream_native(f)};
        static_cast<FileHandle *>(BADDR(originals[1]))->fh_BufSize=KAFFEINE_FILE_BUFFER_LIMIT+1;
        static_cast<FileHandle *>(BADDR(originals[1]))->fh_Buf=MKBADDR(native_buffer.data());
        StreamOwner owner(f,originals);
        check(!owner.init() && f.ctx.process_streams && stream_record(f,originals[0]) &&
              !stream_record(f,originals[1]) && !stream_record(f,originals[2]),
              "partial import failure retains the startup capture and only its published aliases");
        owner.drop();owner.drop();owner.native_exit(true);
        check(closes==3 && !f.ctx.process_streams && !f.ctx.process_file_close,
              "repeated Drop after partial Init is safe before native original cleanup");
        f.ctx.running=TRUE;f.ctx.callback_failed=FALSE;f.ctx.exit_code=0;stream_sweep(f);
    }
    {
        Fixture f;StreamOwner owner(f,{nullptr,nullptr,nullptr});
        allocation_countdown=0;
        check(!owner.init() && !f.ctx.process_streams && !f.ctx.process_file_close,
              "capture allocation failure imports nothing and installs no close hook");
        allocation_countdown=-1;owner.drop();owner.native_exit();
        check(!closes,"Drop without Init and NULL originals has no stream-close effects");
    }
    {
        Fixture f;BPTR original=MKBADDR(&f.native[15]);
        ULONG token=Kaffeine_FileImport(&f.ctx,original,TRUE);
        StreamOwner owner(f,{original,original,nullptr},0);
        check(owner.init()==1 && stream_record(f,original)->owned,
              "startup import deduplicates an existing owned file without changing its ownership");
        owner.drop();owner.native_exit();
        check(!closes && stream_record(f,original) && stream_record(f,original)->owned &&
              f.dos(42,token,f.buffer,1)==1,
              "unarmed originals remain registered because native DosEntry will not close them");
        Kaffeine_FileCleanup(&f.ctx);check(closes==1,"unarmed owned original remains Context-owned");
    }
    {
        Fixture f;BPTR original=MKBADDR(&f.native[16]);
        StreamOwner owner(f,{original,original,nullptr},PRF_CLOSEOUTPUT);
        check(owner.init()==1,"one live native close flag may own a duplicate original");
        owner.drop();owner.native_exit();
        check(closes==1 && !stream_record(f,original),
              "Drop honors the live output flag rather than assuming the first captured slot owns close");
    }
}

static void unsafe_startup_close_refused()
{
    Fixture f;BPTR original=MKBADDR(&f.native[17]);
    StreamOwner owner(f,{original,original,original});
    check(owner.init()==1,"unsafe caller test owns a real startup capture");
    ULONG token=stream_record(f,original)->guest>>2;
    Task intruder={};intruder.tc_Node.ln_Type=NT_PROCESS;
    native_caller=&intruder;
    unsigned effects=scalar_calls;
    check(f.dos(36,token)==20 && !closes && scalar_calls==effects && stream_record(f,original) &&
          (owner.process.pr_Flags&stream_close_all)==stream_close_all,
          "uncontrolled native caller cannot mutate close flags, selections or native file ownership");
    native_caller=nullptr;f.ctx.running=TRUE;f.ctx.callback_failed=FALSE;f.ctx.exit_code=0;
    owner.drop();owner.native_exit();
    check(closes==1 && !f.ctx.process_streams && !f.ctx.process_file_close,
          "refused Close retains ownership until the real broker retires it once");
}

static void captured_startup_streams()
{
    selected_replacements_survive_exit();
    explicitly_closed_startup_reuse();
    partial_and_unarmed_startup();
    unsafe_startup_close_refused();
    process_native_streams=false;
}
