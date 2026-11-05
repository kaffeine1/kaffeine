/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Independent classic NP_Path ownership and real native Shell lookup. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"
static const char folder[]="RAM:Kaffeine-System-Path";
static const char program[]="RAM:Kaffeine-System-Path/PathEcho";
static const char output[]="RAM:Kaffeine-System-Path/output.txt";
static BOOL copy_command(void)
{
    BPTR in=Open((CONST_STRPTR)"C:Echo",MODE_OLDFILE),out=0;UBYTE bytes[4096];LONG n;BOOL ok=FALSE;
    if(!in)return FALSE;
    out=Open((CONST_STRPTR)program,MODE_NEWFILE);
    if(out){while((n=Read(in,bytes,sizeof(bytes)))>0)if(Write(out,bytes,n)!=n)break;ok=n==0;Close(out);}
    Close(in);return ok;
}
static BPTR *path(ULONG bytes)
{
    BPTR *node=AllocVec(bytes,MEMF_PUBLIC|MEMF_CLEAR);if(!node)return NULL;
    node[1]=Lock((CONST_STRPTR)folder,SHARED_LOCK);
    if(!node[1]){FreeVec(node);return NULL;}return node;
}
static void drop(BPTR *node){if(node){UnLock(node[1]);FreeVec(node);}}
static LONG run(BPTR *node,BPTR in,BPTR out,BOOL asynchronous,BOOL close_error)
{
    return SystemTags((CONST_STRPTR)"PathEcho PATH_OK",SYS_Input,in,SYS_Output,out,
      NP_Error,out,NP_CloseError,close_error,NP_Path,MKBADDR(node),SYS_Asynch,asynchronous,
      NP_StackSize,65536,NP_Priority,0,TAG_DONE);
}
int main(void)
{
    BPTR directory=0,in=0,out=0;BPTR *node=NULL;LONG rc,error;char bytes[128];
    directory=CreateDir((CONST_STRPTR)folder);
    if(!check(directory!=0,"create owned path fixture directory"))goto end;
    UnLock(directory);directory=0;
    if(!check(copy_command(),"copy native Echo under a name found only through NP_Path"))goto end;
    in=Open((CONST_STRPTR)"NIL:",MODE_OLDFILE);out=Open((CONST_STRPTR)output,MODE_NEWFILE);
    if(!check(in&&out,"open owned native Shell streams"))goto end;
    node=path(8);if(!check(node!=NULL,"allocate classic eight-byte path node and caller lock"))goto end;
    node[0]=MKBADDR(node);rc=run(node,in,out,FALSE,FALSE);error=IoErr();
    check(rc==-1&&error==ERROR_BAD_NUMBER,"cyclic NP_Path is refused before native Shell launch");
    node[0]=0;rc=run(node,in,out,TRUE,FALSE);error=IoErr();
    check(rc==-1&&error==ERROR_NOT_IMPLEMENTED,"asynchronous path ownership is explicitly refused");
    rc=run(node,in,out,FALSE,TRUE);error=IoErr();
    check(rc==-1&&error==ERROR_NOT_IMPLEMENTED,"stream close ownership is refused before native side effects");
    rc=run(node,in,out,FALSE,FALSE);if(rc>=0)node=NULL;
    check(rc==0,"same path node and lock remain usable after all preflight refusals");
    Close(out);out=0;out=Open((CONST_STRPTR)output,MODE_OLDFILE);memset(bytes,0,sizeof(bytes));
    check(out&&Read(out,bytes,sizeof(bytes)-1)>0&&strstr(bytes,"PATH_OK"),"native Shell finds PathEcho only in the translated classic path");
    if(out){Close(out);out=0;}
    node=path(12);out=Open((CONST_STRPTR)output,MODE_NEWFILE);
    if(check(node&&out,"prepare noncanonical node size")) {
      rc=run(node,in,out,FALSE,FALSE);error=IoErr();
      check(rc==-1&&error==ERROR_BAD_NUMBER,"interior or oversized path containers cannot be consumed");
    }
    drop(node);node=NULL;
    if(out){Close(out);out=0;}
    node=path(8);out=Open((CONST_STRPTR)output,MODE_NEWFILE);
    if(check(node&&out,"prepare another independent caller-owned path")) {
      rc=run(node,in,out,FALSE,FALSE);if(rc>=0)node=NULL;
      check(rc==0,"a second native Shell gets its own path and restored broker state");
    }
end:
    drop(node);if(in)Close(in);if(out)Close(out);if(directory)UnLock(directory);
    DeleteFile((CONST_STRPTR)output);DeleteFile((CONST_STRPTR)program);
    check(DeleteFile((CONST_STRPTR)folder),"remove only owned path fixture files and directory");
    return test_done("dos_system_path");
}
