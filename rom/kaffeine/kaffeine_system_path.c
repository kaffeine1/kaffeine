/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Synchronous SystemTagList path ownership. Native nodes/locks are private;
   classic nodes are reserved until launch succeeds or rollback restores them. */
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdint.h>
#include "kaffeine_system_path.h"
#include "kaffeine_locks.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_messages.h"
#define PATH_LIMIT 64
#define TAG_LIMIT 256
struct PathCall {
    ULONG nodes[PATH_LIMIT], count;
    struct KaffeineLockTransfer locks[PATH_LIMIT];
    BPTR native_path;
    char *strings[TAG_LIMIT+1];ULONG string_count;
};
static char *copy_string(struct KaffeineContext *ctx, const char *text, ULONG maximum)
{
    uintptr_t offset=(uintptr_t)text-(uintptr_t)ctx->mem;ULONG i,limit;char *out;
    if(offset>=ctx->mem_size)return NULL;
    limit=ctx->mem_size-(ULONG)offset;if(limit>maximum)limit=maximum;
    out=AllocVec(limit+1,MEMF_PUBLIC);if(!out)return NULL;
    for(i=0;i<limit;++i){out[i]=text[i];if(!out[i])return out;}
    FreeVec(out);return NULL;
}
static BOOL copy_path(struct KaffeineContext *ctx, struct PathCall *call, ULONG bptr, SIPTR *error)
{
    BPTR *tail=&call->native_path;ULONG i,addr,next,token;BPTR *node;
    while(bptr) {
        if(bptr>0x3fffffffUL || call->count==PATH_LIMIT){*error=ERROR_BAD_NUMBER;return FALSE;}
        addr=bptr<<2;
        for(i=0;i<call->count;++i)if(call->nodes[i]==addr){*error=ERROR_BAD_NUMBER;return FALSE;}
        if(!Kaffeine_HeapReserveExact(ctx,addr,8))
        {*error=ERROR_BAD_NUMBER;return FALSE;}
        i=call->count++;call->nodes[i]=addr;
        next=m68k_read32(ctx,addr);token=m68k_read32(ctx,addr+4);
        if(!token || !Kaffeine_LockTransferBegin(ctx,token,&call->locks[i]))
        {*error=ERROR_INVALID_LOCK;return FALSE;}
        node=AllocVec(2*sizeof(BPTR),MEMF_PUBLIC|MEMF_CLEAR);
        if(!node){*error=ERROR_NO_FREE_STORE;return FALSE;}
        node[1]=DupLock(call->locks[i].native);
        if(!node[1]){*error=IoErr();FreeVec(node);return FALSE;}
        *tail=MKBADDR(node);tail=&node[0];bptr=next;
    }
    return TRUE;
}
static void cleanup(struct KaffeineContext *ctx, struct PathCall *call, BOOL consume)
{
    ULONG i;BPTR p=call->native_path;
    while(p){BPTR *node=BADDR(p);BPTR next=node[0];if(node[1])UnLock(node[1]);FreeVec(node);p=next;}
    for(i=0;i<call->count;++i) {
        BPTR native=call->locks[i].native;BOOL active=call->locks[i].active;
        Kaffeine_LockTransferEnd(ctx,&call->locks[i],consume);
        if(consume && active)UnLock(native);
        if(!Kaffeine_HeapFinishExact(ctx,call->nodes[i],8,consume))
            Kaffeine_MessageFailure(ctx,"SystemTagList path allocation ownership changed");
    }
    for(i=0;i<call->string_count;++i)FreeVec(call->strings[i]);
}
LONG Kaffeine_SystemPathRun(struct KaffeineContext *ctx,const char *command,struct TagItem *tags,SIPTR *error)
{
    struct PathCall *call;struct Process *process=NULL;struct CommandLineInterface *cli=NULL,*temporary=NULL;
    BPTR saved_cli=0,saved_path=0;ULONG i,head=0,paths=0;LONG rc=-1;BOOL installed=FALSE,invoked=FALSE;
    call=AllocVec(sizeof(*call),MEMF_PUBLIC|MEMF_CLEAR);
    if(!call){*error=ERROR_NO_FREE_STORE;return -1;}
    *error=ERROR_BAD_NUMBER;
    for(i=0;i<TAG_LIMIT && tags[i].ti_Tag!=TAG_DONE;++i) {
        ULONG tag=tags[i].ti_Tag;
        if(tag==NP_Path){head=(ULONG)tags[i].ti_Data;++paths;tags[i].ti_Tag=TAG_IGNORE;}
        if((tag==SYS_Asynch && tags[i].ti_Data) || tag==SYS_CustomShell ||
           (tag==SYS_UserShell && !tags[i].ti_Data)) {*error=ERROR_NOT_IMPLEMENTED;goto end;}
        if(tag==NP_Name || tag==NP_CommandName) {
            char *copy=copy_string(ctx,(const char *)tags[i].ti_Data,4096);
            if(!copy){*error=ERROR_NO_FREE_STORE;goto end;}
            call->strings[call->string_count++]=copy;tags[i].ti_Data=(IPTR)copy;
        }
    }
    if(i==TAG_LIMIT || paths!=1)goto end;
    call->strings[call->string_count]=copy_string(ctx,command,65536);
    if(!call->strings[call->string_count]){*error=ERROR_NO_FREE_STORE;goto end;}
    command=call->strings[call->string_count++];
    if(!copy_path(ctx,call,head,error))goto end;
    process=(struct Process *)FindTask(NULL);
    if(!process || process->pr_Task.tc_Node.ln_Type!=NT_PROCESS)goto end;
    saved_cli=process->pr_CLI;cli=BADDR(saved_cli);
    if(!cli) {
        temporary=AllocDosObject(DOS_CLI,NULL);
        if(!temporary){*error=ERROR_NO_FREE_STORE;goto end;}
        cli=temporary;process->pr_CLI=MKBADDR(cli);
    }
    /* AROS synchronous SystemTagList creates a process without NP_Cli;
       CliInitRun copies its parent's command path and ignores NP_Path.
       Stage the private path on this call's own broker, then restore it.
       No classic node or lock reaches native DOS, even on failed launch. */
    saved_path=cli->cli_CommandDir;cli->cli_CommandDir=call->native_path;installed=TRUE;
    if(!Kaffeine_FileAllPrepare(ctx)){*error=IoErr();goto end;}
    invoked=TRUE;rc=SystemTagList(command,tags);*error=IoErr();
end:
    if(installed)cli->cli_CommandDir=saved_path;
    if(temporary){process->pr_CLI=saved_cli;FreeDosObject(DOS_CLI,temporary);}
    cleanup(ctx,call,invoked && rc>=0);FreeVec(call);
    if(!ctx->running && ctx->exit_code==20)return 20;
    return rc;
}
