#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""Compile public entry points verbatim with poisoned native collaborators.

The complete window, Workbench and I/O modules retain their own independent
unit suites. This executable isolates the guest boundary and uses the actual
queue, handle association, heap and dispatcher, with no copied adapter logic.
"""
from pathlib import Path
import re
import sys
core, output = map(Path, sys.argv[1:])
def function(filename, name):
    source = (core / filename).read_text()
    match = re.search(r'^(?:static )?[^\n{};]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise SystemExit('missing production body: ' + name)
    start, end, depth = match.start(), match.end(), 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[start:end]
text = '''/* Generated from production. Native IDCMP and App paths are poisoned. */
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
#include "kaffeine_messages.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_thunks_dos.h"
#include "kaffeine_readargs.h"
#include "kaffeine_sched.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_format.h"
#include "kaffeine_shadow.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include <proto/dos.h>
#include <dos/stdio.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <intuition/intuition.h>
#include <aros/debug.h>
#include <stdlib.h>
#include <string.h>
enum { G_MP_MSGLIST=20, G_LH_TAIL=4, G_LH_TAILPRED=8, G_LN_SUCC=0, G_LN_PRED=4,
       G_MP_FLAGS=14, G_MP_SIGBIT=15, G_MP_SIGTASK=16, G_LN_TYPE=8 };
static void *message_native(struct KaffeineContext *c, ULONG a, UWORD t) { (void)c;(void)a;(void)t;return NULL; }
static void message_unshow(struct KaffeineContext *c, ULONG a) { (void)c;(void)a;abort(); }
static ULONG message_guest(struct KaffeineContext *c, struct IntuiMessage *m, UWORD t) { (void)c;(void)m;(void)t;abort(); }
static void message_take(struct KaffeineContext *c, struct IntuiMessage *m) { (void)c;(void)m;abort(); }
static void message_reply(struct KaffeineContext *c, struct IntuiMessage *m) { (void)c;(void)m;abort(); }
static BOOL Kaffeine_AppPortFill(struct KaffeineContext *c, ULONG p, BOOL w) { (void)c;(void)p;(void)w;return FALSE; }
static BOOL Kaffeine_AppReplyMsg(struct KaffeineContext *c, ULONG m) { (void)c;(void)m;return FALSE; }
static void Kaffeine_AppPortGone(struct KaffeineContext *c, ULONG p) { (void)c;(void)p; }
static IPTR refuse(struct KaffeineContext *c, const char *s) { return Kaffeine_MessageFailure(c,s); }
'''
text += '#define GUEST_FIB_SIZE 260\n'
# The DOS and Exec I/O files each define guest_range. Their definitions have
# the same range contract in this supported subset; keep the DOS definition.
text += function('kaffeine_thunks_dos.c','guest_range') + '\n'
for name in ['guest_string','bad_dos_buffer','transfer_buffer','kaffeine_dos_Open','kaffeine_dos_Read','kaffeine_dos_Write','kaffeine_dos_SetVBuf',
             'kaffeine_dos_FRead','kaffeine_dos_FWrite','fgets_write_size','kaffeine_dos_FGets','kaffeine_dos_FPuts','kaffeine_dos_NameFromFH',
             'Kaffeine_FIBToGuest','kaffeine_dos_ExamineFH','kaffeine_dos_Execute','kaffeine_dos_VFWritef','kaffeine_dos_WriteChars','kaffeine_dos_PutStr']:
    text += function('kaffeine_thunks_dos.c',name) + '\n'
# Keep the packet constants, private helpers and public bodies together.
source = (core / 'kaffeine_thunks_dos.c').read_text()
start = source.index('#define GUEST_MSG_SIZE ')
end = source.index('/* Walk a guest TagItem chain', start)
text += source[start:end]
text += function('kaffeine_thunks.c','thunk_dos_AllocDosObject') + '\n'
text += function('kaffeine_thunks_dos.c','kaffeine_dos_FreeDosObject') + '\n'
text += """
IPTR message_AllocDosObject(struct KaffeineContext *ctx, void *cpu)
{ return thunk_dos_AllocDosObject(ctx, cpu); }
"""
start = (core / 'kaffeine_thunks_dos.c').read_text().index('struct PrintOutput\n')
source = (core / 'kaffeine_thunks_dos.c').read_text()
text += source[start:source.index('IPTR kaffeine_dos_VFWritef(', start)]
for name in ['guest_task','Kaffeine_IORefuse','port_entry','Kaffeine_CreateMsgPort','Kaffeine_DeleteMsgPort']:
    text += function('kaffeine_exec_io.c',name) + '\n'
for name in ['guest_first','Kaffeine_GetMsg','Kaffeine_WaitPort','Kaffeine_ReplyMsg']:
    text += function('kaffeine_window.c',name) + '\n'
output.write_text(text)
