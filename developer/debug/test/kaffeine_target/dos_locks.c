/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   FileLock fields that classic file managers read as well as DOS calls. */
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"

#define ROOT "RAM:kaffeine-locks"
#define FIRST ROOT "/first"
#define SECOND ROOT "/second"
static struct FileInfoBlock fib __attribute__((aligned(4)));

int main(void)
{
    BPTR root=0,first=0,second=0,copy=0,old,installed,fh;
    struct FileLock *a,*b,*c;
    struct DosList *volume;
    struct DevProc *dp;
    struct Process *process=(struct Process *)FindTask(NULL);
    char buffer[128];
    unsigned i;
    DeleteFile(FIRST "/data");DeleteFile(FIRST);DeleteFile(SECOND);DeleteFile(ROOT);
    root=CreateDir(ROOT);first=CreateDir(FIRST);second=CreateDir(SECOND);
    if(!check(root&&first&&second,"create three directory locks"))goto done;
    UnLock(root);UnLock(first);UnLock(second);
    root=Lock(ROOT,SHARED_LOCK);first=Lock(FIRST,SHARED_LOCK);second=Lock(SECOND,SHARED_LOCK);
    if(!check(root&&first&&second,"reopen directory locks in shared mode"))goto done;
    copy=DupLock(first);
    if(!check(copy!=0,"duplicate a directory lock"))goto done;
    check(SameLock(first,copy)==LOCK_SAME&&SameLock(first,second)==LOCK_SAME_VOLUME,
          "DOS distinguishes equal objects from equal volumes");
    a=BADDR(first);b=BADDR(second);c=BADDR(copy);
    check(a->fl_Key==c->fl_Key&&a->fl_Key!=b->fl_Key,
          "readable FileLock keys distinguish two directories and their duplicate");
    check(a->fl_Task&&a->fl_Task==b->fl_Task&&a->fl_Task==c->fl_Task,
          "FileLock handler identity is stable on one filesystem");
    check(a->fl_Volume&&a->fl_Volume==b->fl_Volume&&a->fl_Volume==c->fl_Volume,
          "FileLock volume BPTR is readable and shared");
    volume=BADDR(a->fl_Volume);
    check(volume&&volume->dol_Type==DLT_VOLUME&&volume->dol_Name,
          "volume view has a type and readable name BPTR");
    check(a->fl_Access==SHARED_LOCK&&b->fl_Access==SHARED_LOCK,
          "FileLock access describes the native lock mode");
    check(NameFromLock(first,(STRPTR)buffer,sizeof(buffer))&&strstr(buffer,"first"),
          "NameFromLock resolves the directory view");
    check(Examine(first,&fib)&&fib.fib_DirEntryType>0,
          "Examine resolves the directory view");
    old=CurrentDir(copy);copy=0;
    installed=process->pr_CurrentDir;
    check(installed&&SameLock(installed,first)==LOCK_SAME,
          "CurrentDir synchronizes the Process FileLock field");
    fh=Open("data",MODE_NEWFILE);
    check(fh&&Write(fh,"lock data\n",10)==10,"relative file write uses the installed directory");
    if(fh)Close(fh);
    copy=CurrentDir(old);
    check(process->pr_CurrentDir==old&&copy==installed,
          "CurrentDir restores the displaced process reference");
    UnLock(copy);copy=0;
    copy=DupLock(second);
    old=SetProgramDir(copy);copy=0;
    installed=GetProgramDir();
    check(installed&&process->pr_HomeDir==installed&&SameLock(installed,second)==LOCK_SAME,
          "SetProgramDir and GetProgramDir share the Process lock view");
    copy=SetProgramDir(old);
    check(process->pr_HomeDir==old&&copy==installed,"restore the displaced program directory reference");
    UnLock(copy);copy=0;
    copy=DupLock(first);
    if(check(copy&&AssignLock("KAFFEINELOCKS",copy),"assign consumes a duplicated directory reference"))copy=0;
    dp=GetDeviceProc("KAFFEINELOCKS:",NULL);
    check(dp&&dp->dvp_Port&&dp->dvp_Lock&&NameFromLock(dp->dvp_Lock,(STRPTR)buffer,sizeof(buffer)),
          "GetDeviceProc exposes a readable assign lock until FreeDeviceProc");
    if(dp)FreeDeviceProc(dp);
    AssignLock("KAFFEINELOCKS",0);
    if(copy)UnLock(copy);copy=0;
    copy=ParentDir(first);
    check(copy&&SameLock(copy,root)==LOCK_SAME,"ParentDir returns a readable owned lock");
    if(copy)UnLock(copy);copy=0;
    fh=Open(FIRST "/data",MODE_OLDFILE);
    if(fh)
    {
        copy=DupLockFromFH(fh);
        check(copy&&NameFromLock(copy,(STRPTR)buffer,sizeof(buffer)),"DupLockFromFH returns a typed lock");
        Close(fh);
        fh=copy?OpenFromLock(copy):0;
        if(fh)copy=0;
        check(fh&&Read(fh,buffer,10)==10&&!memcmp(buffer,"lock data\n",10),
              "OpenFromLock consumes its lock and preserves the file data");
        if(fh)Close(fh);
    }
    else check(FALSE,"open file for lock conversion");
    if(copy)UnLock(copy);copy=0;
    for(i=0;i<128;i++)
    {
        copy=Lock(FIRST,SHARED_LOCK);
        if(!copy||SameLock(copy,first)!=LOCK_SAME)break;
        UnLock(copy);copy=0;
    }
    check(i==128,"repeated publication and release preserve directory identity");
done:
    if(copy)UnLock(copy);
    if(second)UnLock(second);
    if(first)UnLock(first);
    if(root)UnLock(root);
    DeleteFile(FIRST "/data");DeleteFile(FIRST);DeleteFile(SECOND);DeleteFile(ROOT);
    return test_done("dos_locks");
}
