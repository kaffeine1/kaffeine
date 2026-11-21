# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjtask, a 68k AmigaOS program as one hunk, assembled by this script. usage: mk_kjtask.py <out>

Three things that make the library run 68k code on more than one CPU of the program, each checked by the program:
  1. exec Cause() with a software interrupt of the program, which adds 1 to a cell;
  2. dos CreateNewProc() with NP_Entry: a process of the program adds the squares of the low words of 100000..1 and
     stores the sum, while the program waits two seconds in Delay();
  3. exec AddIntServer() for the vertical blank with a server that adds 1 to a cell, while the program runs a loop of
     twenty million instructions; then RemIntServer().
It writes 24 bytes to Output(): the three cells and the sum of its own loop (four longwords), then "kjtask", a line
feed and a zero. It returns 12 when everything is right; otherwise 1 (the software interrupt did not run once),
2 (no process), 3 (the process did not store its sum), 4 (no vertical blank reached the server), 10 (the loop's sum is
wrong), 5 (no dos.library).
"""
import struct, sys
N = 4000000
low2 = lambda n: sum((i & 0xFFFF) * (i & 0xFFFF) for i in range(1, n + 1)) & 0xFFFFFFFF
loopsum, childsum = low2(N), low2(100000)
items = []
W = lambda *ws: items.extend(('w', w) for w in ws)
L = lambda name: items.append(('label', name))
PC = lambda label: items.append(('pc', label))               # 16-bit displacement from this word
B8 = lambda op, label: items.append(('b8', op, label))       # Bcc.s
D = lambda data: items.append(('data', data))
LOOP = lambda name: (L(name), W(0x2203, 0xC2C3, 0xD881, 0x5383), B8(0x66, name))   # move.l d3,d1; mulu.w d3,d1; add.l d1,d4; subq.l #1,d3; bne.s
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x43FA); PC('dosname')         # lea dosname(pc),a1
W(0x7000)                        # moveq #0,d0
W(0x4EAE, 0xFDD8)                # jsr -552(a6)        OpenLibrary
W(0x4A80)                        # tst.l d0
W(0x6700); PC('nodos')           # beq.w nodos
W(0x2A40)                        # movea.l d0,a5
# 1. the software interrupt
W(0x43FA); PC('swi')             # lea swi(pc),a1
W(0x41FA); PC('cell_swi')        # lea cell_swi(pc),a0
W(0x2348, 0x000E)                # move.l a0,14(a1)    is_Data
W(0x41FA); PC('swicode')         # lea swicode(pc),a0
W(0x2348, 0x0012)                # move.l a0,18(a1)    is_Code
W(0x4EAE, 0xFF4C)                # jsr -180(a6)        Cause
W(0x41FA); PC('cell_swi')        # lea cell_swi(pc),a0
W(0x0C90, 0x0000, 0x0001)        # cmpi.l #1,(a0)
B8(0x67, 'swiok')                # beq.s swiok
W(0x7001, 0x4E75)                # moveq #1,d0 ; rts
L('swiok')
# 2. the process
W(0x41FA); PC('tags')            # lea tags(pc),a0
W(0x43FA); PC('child')           # lea child(pc),a1
W(0x2149, 0x0004)                # move.l a1,4(a0)     ti_Data of NP_Entry
W(0x2208)                        # move.l a0,d1
W(0x2C4D)                        # movea.l a5,a6
W(0x4EAE, 0xFE0E)                # jsr -498(a6)        CreateNewProc
W(0x4A80)                        # tst.l d0
B8(0x66, 'procok')               # bne.s procok
W(0x7002, 0x4E75)                # moveq #2,d0 ; rts
L('procok')
W(0x7264)                        # moveq #100,d1
W(0x4EAE, 0xFF3A)                # jsr -198(a6)        Delay
W(0x41FA); PC('cell_child')      # lea cell_child(pc),a0
W(0x0C90, childsum >> 16, childsum & 0xFFFF)   # cmpi.l #childsum,(a0)
B8(0x67, 'childok')              # beq.s childok
W(0x7003, 0x4E75)                # moveq #3,d0 ; rts
L('childok')
# 3. the vertical blank server
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x43FA); PC('vbi')             # lea vbi(pc),a1
W(0x41FA); PC('cell_vbl')        # lea cell_vbl(pc),a0
W(0x2348, 0x000E)                # move.l a0,14(a1)
W(0x41FA); PC('vblcode')         # lea vblcode(pc),a0
W(0x2348, 0x0012)                # move.l a0,18(a1)
W(0x7005)                        # moveq #5,d0         INTB_VERTB
W(0x4EAE, 0xFF58)                # jsr -168(a6)        AddIntServer
W(0x7800)                        # moveq #0,d4
W(0x263C, N >> 16, N & 0xFFFF)   # move.l #N,d3
LOOP('loop')
W(0x43FA); PC('vbi')             # lea vbi(pc),a1
W(0x7005)                        # moveq #5,d0
W(0x4EAE, 0xFF52)                # jsr -174(a6)        RemIntServer
W(0x41FA); PC('cell_sum')        # lea cell_sum(pc),a0
W(0x2084)                        # move.l d4,(a0)
W(0x2C4D)                        # movea.l a5,a6
W(0x4EAE, 0xFFC4)                # jsr -60(a6)         Output
W(0x2200)                        # move.l d0,d1
W(0x41FA); PC('cell_swi')        # lea cell_swi(pc),a0
W(0x2408)                        # move.l a0,d2
W(0x7618)                        # moveq #24,d3
W(0x4EAE, 0xFFD0)                # jsr -48(a6)         Write
W(0x224D)                        # movea.l a5,a1
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x4EAE, 0xFE62)                # jsr -414(a6)        CloseLibrary
W(0x41FA); PC('cell_vbl')        # lea cell_vbl(pc),a0
W(0x4A90)                        # tst.l (a0)
B8(0x66, 'vblok')                # bne.s vblok
W(0x7004, 0x4E75)                # moveq #4,d0 ; rts
L('vblok')
W(0x0C84, loopsum >> 16, loopsum & 0xFFFF)     # cmpi.l #loopsum,d4
B8(0x67, 'allok')                # beq.s allok
W(0x700A, 0x4E75)                # moveq #10,d0 ; rts
L('allok')
W(0x700C, 0x4E75)                # moveq #12,d0 ; rts
L('nodos')
W(0x7005, 0x4E75)                # moveq #5,d0 ; rts
L('swicode')                     # the software interrupt: A1 is is_Data
W(0x5291, 0x4E75)                # addq.l #1,(a1) ; rts
L('child')                       # the process
W(0x41FA); PC('cell_child')      # lea cell_child(pc),a0
W(0x7800)                        # moveq #0,d4
W(0x263C, 0x0001, 0x86A0)        # move.l #100000,d3
LOOP('cloop')
W(0x2084)                        # move.l d4,(a0)
W(0x7000, 0x4E75)                # moveq #0,d0 ; rts
L('vblcode')                     # the vertical blank server: A1 is is_Data; Z set on return
W(0x5291)                        # addq.l #1,(a1)
W(0x7000, 0x4E75)                # moveq #0,d0 ; rts
L('dosname'); D(b'dos.library\0')
node = b'\0' * 8 + b'\x02\0' + b'\0' * 12        # struct Interrupt: ln_Type NT_INTERRUPT, is_Data and is_Code filled by the program
L('swi'); D(node)
L('vbi'); D(node)
L('tags'); D(struct.pack('>4I', 0x800003EB, 0, 0, 0))    # NP_Entry, (the process); TAG_DONE
L('cell_swi'); D(b'\0' * 4)
L('cell_child'); D(b'\0' * 4)
L('cell_vbl'); D(b'\0' * 4)
L('cell_sum'); D(b'\0' * 4)
D(b'kjtask\n\0')
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
        body += struct.pack('>h', labels[it[1]] - off)
    elif it[0] == 'b8':
        d = labels[it[2]] - (off + 2)
        assert -128 <= d < 128 and d != 0 and d != -1, (it, d)
        body += bytes([it[1], d & 0xFF])
    elif it[0] == 'data':
        body += it[1]
    off += size(it)
assert all(labels[n] % 2 == 0 for n in ('swi', 'vbi', 'tags', 'cell_swi')) and len(node) == 22
assert labels['cell_child'] == labels['cell_swi'] + 4 and labels['cell_sum'] == labels['cell_swi'] + 12
body += b'\0' * (-len(body) % 4)
longs = len(body) // 4
hunk = struct.pack('>6I', 0x3F3, 0, 1, 0, 0, longs) + struct.pack('>2I', 0x3E9, longs) + body + struct.pack('>I', 0x3F2)
open(sys.argv[1], 'wb').write(hunk)
print('kjtask: %d bytes, code and data %d bytes, loop sum %08x, process sum %08x' % (len(hunk), len(body), loopsum, childsum))
print('labels', {k: hex(v) for k, v in labels.items()})
