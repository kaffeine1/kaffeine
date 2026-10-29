/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original guard regression for classic InitRequester caller storage. */
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
int main(void) {
 struct { struct Requester req; UBYTE guard[192]; } b;
 unsigned i; BOOL zero=TRUE,guard=TRUE;
 IntuitionBase=(struct IntuitionBase*)OpenLibrary("intuition.library",0);
 if (!check(IntuitionBase!=NULL,"open intuition.library")) return test_done("gfx_requester_init");
 memset(&b,0xA5,sizeof(b)); InitRequester(&b.req);
 for(i=0;i<sizeof(b.req);i++) if(((UBYTE*)&b.req)[i]) zero=FALSE;
 for(i=0;i<sizeof(b.guard);i++) if(b.guard[i]!=0xA5) guard=FALSE;
 check_long("classic Requester size",sizeof(b.req),112);
 check(zero,"InitRequester clears the classic structure");
 check(guard,"InitRequester preserves following storage");
 CloseLibrary((struct Library*)IntuitionBase);
 return test_done("gfx_requester_init");
}
