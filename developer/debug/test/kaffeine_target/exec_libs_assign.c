/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Install the existing emu68test.library fixture only in SYS:Libs/m68k.
   Each spelling below loads a fresh disk library, then expunges it, so
   another spelling cannot pass by reusing an already loaded library. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include "m68ktest.h"

static struct Library *TestBase;
#define TestAdd(a,b) LP2(0x1e,LONG,TestAdd,LONG,a,d0,LONG,b,d1,,TestBase)
#define TestExpunge() LP0(0x3c,ULONG,TestExpunge,,TestBase)

static void load_and_expunge(const char *name)
{
    struct Library *again;
    TestBase=OpenLibrary((CONST_STRPTR)name,2);
    if(!check(TestBase!=NULL,name))return;
    check_long("disk library version",TestBase->lib_Version,2);
    check_long("disk library revision",TestBase->lib_Revision,5);
    check_str("disk library identity",TestBase->lib_Node.ln_Name,"emu68test.library");
    check_long("disk library guest function",TestAdd(19,23),42);
    again=OpenLibrary((CONST_STRPTR)"emu68test.library",2);
    check(again==TestBase,"bare name reuses the same library");
    if(again)CloseLibrary(again);
    check((TestExpunge()&LIBF_DELEXP)!=0,"request expunge at last close");
    CloseLibrary(TestBase);
    TestBase=NULL;
}

int main(void)
{
    static const char *const names[]={
        "LIBS:emu68test.library","libs:emu68test.library",
        "LiBs:emu68test.library","emu68test.library",
        "SYS:Libs/m68k/emu68test.library"
    };
    struct Library *native=OpenLibrary((CONST_STRPTR)"utility.library",37);
    struct Library *alias=OpenLibrary((CONST_STRPTR)"LIBS:utility.library",37);
    unsigned i;
    check(native!=NULL,"native library opens");
    check(native && alias==native,"LIBS native lookup remains canonical");
    if(alias)CloseLibrary(alias);
    if(native)CloseLibrary(native);
    for(i=0;i<sizeof(names)/sizeof(names[0]);i++)load_and_expunge(names[i]);
    alias=OpenLibrary((CONST_STRPTR)"LIBS:kaffeine-absent.library",0);
    check(alias==NULL,"missing LIBS leaf fails without a requester");
    if(alias)CloseLibrary(alias);
    alias=OpenLibrary((CONST_STRPTR)"LIBS:no-such-dir/emu68test.library",0);
    check(alias==NULL,"deeper LIBS path is exact after expunge");
    if(alias)CloseLibrary(alias);
    return test_done("exec_libs_assign");
}
