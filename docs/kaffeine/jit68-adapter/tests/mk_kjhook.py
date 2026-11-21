# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjhook, a 68k AmigaOS program as one hunk, assembled by this script. usage: mk_kjhook.py <out>

A BOOPSI class written in 68k code, called back by intuition.library. The program makes a class under rootclass with
MakeClass() and writes its own dispatcher into it, then calls NewObjectA(), GetAttr(), SetAttrsA() and DisposeObject()
on an object of that class and FreeClass(). Each of the four calls makes intuition call the dispatcher once: OM_NEW,
OM_GET, OM_SET, OM_DISPOSE. The dispatcher counts the method, adds the squares of the low words of 1000..1 to a sum
(a loop of 5000 instructions), answers OM_GET for its attribute with a value and 1, keeps the tag value of OM_SET and
returns 1, and passes OM_NEW, OM_DISPOSE and anything else to its superclass.

It writes 52 bytes to Output(): the counters of OM_NEW, OM_DISPOSE, OM_SET, OM_GET and of any other method, the value
GetAttr() stored, the value OM_SET saw, the sum, the results of GetAttr(), SetAttrsA() and FreeClass() (eleven
longwords), then "kjhook", a line feed and a zero. It returns 12 when everything is right; otherwise 1 (no class),
2 (no object), 3 (a counter), 4 (the value GetAttr() stored), 6 (the value OM_SET saw), 7 (the sum), 8 (a result),
5 (a library did not open).
"""
import struct, sys
ATTR, GOT, SEEN, LOOP = 0x8F4A0001, 0x00004B4A, 0x0000BEEF, 1000
total = (4 * sum((i & 0xFFFF) * (i & 0xFFFF) for i in range(1, LOOP + 1))) & 0xFFFFFFFF
items, count, seg = [], {}, ['main']
def I(*parts):                   # one instruction: words, ('pc', label) or ('b8', opcode, label)
    count[seg[0]] = count.get(seg[0], 0) + 1
    for p in parts:
        items.append(('w', p & 0xFFFF) if isinstance(p, int) else p)
L = lambda name: items.append(('label', name))
D = lambda data: items.append(('data', data))
PC = lambda label: ('pc', label)
B8 = lambda op, label: ('b8', op, label)
long = lambda v: ((v >> 16) & 0xFFFF, v & 0xFFFF)
def fail(label, code):
    old = seg[0]; seg[0] = 'unused'
    L(label); I(0x7000 | code); I(0x4E75)
    seg[0] = old

I(0x2C78, 0x0004)                # movea.l 4.w,a6
I(0x43FA, PC('intname'))         # lea intname(pc),a1
I(0x7024)                        # moveq #36,d0
I(0x4EAE, 0xFDD8)                # jsr -552(a6)        OpenLibrary
I(0x4A80)                        # tst.l d0
I(0x6700, PC('nolib'))           # beq.w nolib
I(0x2A40)                        # movea.l d0,a5       IntuitionBase
I(0x43FA, PC('dosname'))         # lea dosname(pc),a1
I(0x7000)                        # moveq #0,d0
I(0x4EAE, 0xFDD8)                # jsr -552(a6)        OpenLibrary
I(0x4A80)                        # tst.l d0
I(0x6700, PC('nolib'))           # beq.w nolib
I(0x41FA, PC('dosbase'))         # lea dosbase(pc),a0
I(0x2080)                        # move.l d0,(a0)
I(0x2C4D)                        # movea.l a5,a6
I(0x91C8)                        # suba.l a0,a0        classID NULL
I(0x43FA, PC('rootname'))        # lea rootname(pc),a1
I(0x95CA)                        # suba.l a2,a2        superClassPtr NULL
I(0x7004)                        # moveq #4,d0         instance size
I(0x7200)                        # moveq #0,d1
I(0x4EAE, 0xFD5A)                # jsr -678(a6)        MakeClass
I(0x4A80)                        # tst.l d0
I(B8(0x66, 'classok'))           # bne.s classok
fail('noclass', 1)
L('classok')
I(0x2840)                        # movea.l d0,a4       the class
I(0x41FA, PC('dispatcher'))      # lea dispatcher(pc),a0
I(0x2948, 0x0008)                # move.l a0,8(a4)     cl_Dispatcher.h_Entry
I(0x204C)                        # movea.l a4,a0
I(0x93C9)                        # suba.l a1,a1
I(0x95CA)                        # suba.l a2,a2
I(0x4EAE, 0xFD84)                # jsr -636(a6)        NewObjectA
I(0x4A80)                        # tst.l d0
I(B8(0x66, 'objok'))             # bne.s objok
fail('noobj', 2)
L('objok')
I(0x2640)                        # movea.l d0,a3       the object
I(0x203C, *long(ATTR))           # move.l #ATTR,d0
I(0x204B)                        # movea.l a3,a0
I(0x43FA, PC('got'))             # lea got(pc),a1
I(0x4EAE, 0xFD72)                # jsr -654(a6)        GetAttr
I(0x41FA, PC('r_get'))           # lea r_get(pc),a0
I(0x2080)                        # move.l d0,(a0)
I(0x204B)                        # movea.l a3,a0
I(0x43FA, PC('tags'))            # lea tags(pc),a1
I(0x4EAE, 0xFD78)                # jsr -648(a6)        SetAttrsA
I(0x41FA, PC('r_set'))           # lea r_set(pc),a0
I(0x2080)                        # move.l d0,(a0)
I(0x204B)                        # movea.l a3,a0
I(0x4EAE, 0xFD7E)                # jsr -642(a6)        DisposeObject
I(0x204C)                        # movea.l a4,a0
I(0x4EAE, 0xFD36)                # jsr -714(a6)        FreeClass
I(0x41FA, PC('r_free'))          # lea r_free(pc),a0
I(0x2080)                        # move.l d0,(a0)
I(0x2C7A, PC('dosbase'))         # movea.l dosbase(pc),a6
I(0x4EAE, 0xFFC4)                # jsr -60(a6)         Output
I(0x2200)                        # move.l d0,d1
I(0x41FA, PC('cells'))           # lea cells(pc),a0
I(0x2408)                        # move.l a0,d2
I(0x7634)                        # moveq #52,d3
I(0x4EAE, 0xFFD0)                # jsr -48(a6)         Write
I(0x2C78, 0x0004)                # movea.l 4.w,a6
I(0x227A, PC('dosbase'))         # movea.l dosbase(pc),a1
I(0x4EAE, 0xFE62)                # jsr -414(a6)        CloseLibrary
I(0x224D)                        # movea.l a5,a1
I(0x4EAE, 0xFE62)                # jsr -414(a6)        CloseLibrary
I(0x41FA, PC('cells'))           # lea cells(pc),a0
for k in range(4):
    I(0x0C98, 0x0000, 0x0001)    # cmpi.l #1,(a0)+     OM_NEW, OM_DISPOSE, OM_SET, OM_GET once each
    I(B8(0x66, 'bad3'))          # bne.s bad3
I(0x4A98)                        # tst.l (a0)+         no other method
I(B8(0x66, 'bad3'))              # bne.s bad3
I(0x0C98, *long(GOT))            # cmpi.l #GOT,(a0)+
I(B8(0x66, 'bad4'))              # bne.s bad4
I(0x0C98, *long(SEEN))           # cmpi.l #SEEN,(a0)+
I(B8(0x66, 'bad6'))              # bne.s bad6
I(0x0C98, *long(total))          # cmpi.l #total,(a0)+
I(B8(0x66, 'bad7'))              # bne.s bad7
I(0x0C98, 0x0000, 0x0001)        # cmpi.l #1,(a0)+     GetAttr returned 1
I(B8(0x66, 'bad8'))              # bne.s bad8
I(0x0C98, 0x0000, 0x0001)        # cmpi.l #1,(a0)+     SetAttrsA returned 1
I(B8(0x66, 'bad8'))              # bne.s bad8
I(0x4A90)                        # tst.l (a0)          FreeClass
I(B8(0x67, 'bad8'))              # beq.s bad8
I(0x700C)                        # moveq #12,d0
I(0x4E75)                        # rts
fail('bad3', 3); fail('bad4', 4); fail('bad6', 6); fail('bad7', 7); fail('bad8', 8); fail('nolib', 5)

seg[0] = 'disp'                  # the dispatcher: A0 class, A1 message, A2 object
L('dispatcher')
I(0x2011)                        # move.l (a1),d0      MethodID
I(0x47FA, PC('cells'))           # lea cells(pc),a3
I(0x2200)                        # move.l d0,d1
I(0x0481, 0x0000, 0x0101)        # subi.l #$101,d1     OM_NEW 0, OM_DISPOSE 1, OM_SET 2, OM_GET 3
I(0x0C81, 0x0000, 0x0003)        # cmpi.l #3,d1
I(B8(0x63, 'known'))             # bls.s known
seg[0] = 'other'
I(0x7204)                        # moveq #4,d1         any other method
seg[0] = 'disp'
L('known')
I(0xE589)                        # lsl.l #2,d1
I(0x52B3, 0x1800)                # addq.l #1,(0,a3,d1.l)
I(0x263C, *long(LOOP))           # move.l #LOOP,d3
I(0x7800)                        # moveq #0,d4
seg[0] = 'loop'
L('dloop')
I(0x2203)                        # move.l d3,d1
I(0xC2C3)                        # mulu.w d3,d1
I(0xD881)                        # add.l d1,d4
I(0x5383)                        # subq.l #1,d3
I(B8(0x66, 'dloop'))             # bne.s dloop
seg[0] = 'disp'
I(0xD9AB, 0x001C)                # add.l d4,28(a3)     the sum
I(0x0C80, 0x0000, 0x0104)        # cmpi.l #OM_GET,d0
I(B8(0x66, 'notget'))            # bne.s notget
seg[0] = 'get'
I(0x0CA9, *long(ATTR), 0x0004)   # cmpi.l #ATTR,4(a1)  opg_AttrID
I(B8(0x66, 'tosuper'))           # bne.s tosuper
I(0x2069, 0x0008)                # movea.l 8(a1),a0    opg_Storage
I(0x20BC, *long(GOT))            # move.l #GOT,(a0)
I(0x7001)                        # moveq #1,d0
I(0x4E75)                        # rts
seg[0] = 'notget'
L('notget')
I(0x0C80, 0x0000, 0x0103)        # cmpi.l #OM_SET,d0
I(B8(0x66, 'tosuper'))           # bne.s tosuper
seg[0] = 'set'
I(0x2069, 0x0004)                # movea.l 4(a1),a0    ops_AttrList
I(0x2768, 0x0004, 0x0018)        # move.l 4(a0),24(a3) the first tag's value
I(0x7001)                        # moveq #1,d0
I(0x4E75)                        # rts
seg[0] = 'super'
L('tosuper')
I(0x2068, 0x0018)                # movea.l 24(a0),a0   cl_Super
I(0x2868, 0x0008)                # movea.l 8(a0),a4    its h_Entry
I(0x4ED4)                        # jmp (a4)            with A1 and A2 as they came

L('intname'); D(b'intuition.library\0')
L('dosname'); D(b'dos.library\0')
L('rootname'); D(b'rootclass\0')
L('dosbase'); D(b'\0' * 4)
L('tags'); D(struct.pack('>4I', ATTR, SEEN, 0, 0))
L('cells'); D(b'\0' * 20)        # OM_NEW, OM_DISPOSE, OM_SET, OM_GET, other
L('got'); D(b'\0' * 4)
D(b'\0' * 8)                     # the value OM_SET saw, the sum
L('r_get'); D(b'\0' * 4)
L('r_set'); D(b'\0' * 4)
L('r_free'); D(b'\0' * 4)
D(b'kjhook\n\0')
size = lambda it: 0 if it[0] == 'label' else len(it[1]) if it[0] == 'data' else 2
labels, off = {}, 0
for it in items:
    if it[0] == 'label':
        labels[it[1]] = off
    off += size(it)
body, off = b'', 0
for it in items:
    if it[0] == 'w':
        body += struct.pack('>H', it[1])
    elif it[0] == 'pc':
        d = labels[it[1]] - off
        assert -32768 <= d < 32768
        body += struct.pack('>h', d)
    elif it[0] == 'b8':
        d = labels[it[2]] - (off + 2)
        assert -128 <= d < 128 and d != 0 and d != -1, (it, d)
        body += bytes([it[1], d & 0xFF])
    elif it[0] == 'data':
        body += it[1]
    off += size(it)
assert all(labels[n] % 2 == 0 for n in ('dispatcher', 'dosbase', 'tags', 'cells', 'got', 'r_get'))
assert labels['got'] == labels['cells'] + 20 and labels['r_get'] == labels['cells'] + 32 and labels['r_free'] == labels['cells'] + 40
body += b'\0' * (-len(body) % 4)
longs = len(body) // 4
hunk = struct.pack('>6I', 0x3F3, 0, 1, 0, 0, longs) + struct.pack('>2I', 0x3E9, longs) + body + struct.pack('>I', 0x3F2)
open(sys.argv[1], 'wb').write(hunk)
# instruction counts of a run in which everything is right
common = count['disp'] + LOOP * count['loop'] + 2          # the two instructions of the OM_GET test are 'disp'
calls = {'OM_NEW': common + count['notget'] + count['super'], 'OM_DISPOSE': common + count['notget'] + count['super'],
         'OM_SET': common + count['notget'] + count['set'], 'OM_GET': common + count['get']}
calls = {k: v - 2 for k, v in calls.items()}                # 'disp' already has the two
program = count['main'] + sum(calls.values())
library_calls_main, superclass_calls = 12, 2
interpreter = 1 + 3 * library_calls_main + 4 * 1 + 2 * superclass_calls
engine = program - (1 + library_calls_main) - 4
print('kjhook: %d bytes, code and data %d bytes, sum %08x' % (len(hunk), len(body), total))
print('instructions: main %d, per dispatcher call %s, program total %d' % (count['main'], calls, program))
print('predicted with the engine: engine %d instructions, interpreter %d' % (engine, interpreter))
