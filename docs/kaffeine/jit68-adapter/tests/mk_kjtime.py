# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjtime, a 68k AmigaOS program as one hunk, assembled by this script. usage: mk_kjtime.py <out>

It opens dos.library, takes DateStamp(), runs a five-instruction loop N times (the sum of the squares of the low words of
N..1), takes DateStamp() again, and writes 36 bytes to Output(): the two stamps (three longwords each: days, minutes, ticks of
1/50 s), the sum, and "kjtime" with a line feed and a zero. It returns 12 when the sum is right, 10 when it is wrong,
5 without dos.library. The host reads the time the loop took from the two stamps.
"""
import struct, sys
N = 4000000
expected = 0
for i in range(1, N + 1):
    expected = (expected + (i & 0xFFFF) * (i & 0xFFFF)) & 0xFFFFFFFF
items = []                       # ('w', value) | ('pc', label) | ('b8', opcode, label) | ('label', name) | ('data', bytes)
W = lambda *ws: items.extend(('w', w) for w in ws)
L = lambda name: items.append(('label', name))
PC = lambda label: items.append(('pc', label))               # 16-bit displacement from this word
B8 = lambda op, label: items.append(('b8', op, label))       # Bcc.s
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x43FA); PC('dosname')         # lea dosname(pc),a1
W(0x7000)                        # moveq #0,d0
W(0x4EAE, 0xFDD8)                # jsr -552(a6)        OpenLibrary
W(0x4A80)                        # tst.l d0
B8(0x67, 'nodos')                # beq.s nodos
W(0x2A40)                        # movea.l d0,a5
W(0x2C4D)                        # movea.l a5,a6
W(0x41FA); PC('ds1')             # lea ds1(pc),a0
W(0x2208)                        # move.l a0,d1
W(0x4EAE, 0xFF40)                # jsr -192(a6)        DateStamp
W(0x7800)                        # moveq #0,d4
W(0x263C, N >> 16, N & 0xFFFF)   # move.l #N,d3
L('loop')
W(0x2203)                        # move.l d3,d1
W(0xC2C3)                        # mulu.w d3,d1
W(0xD881)                        # add.l d1,d4
W(0x5383)                        # subq.l #1,d3
B8(0x66, 'loop')                 # bne.s loop
W(0x41FA); PC('ds2')             # lea ds2(pc),a0
W(0x2208)                        # move.l a0,d1
W(0x4EAE, 0xFF40)                # jsr -192(a6)        DateStamp
W(0x41FA); PC('sum')             # lea sum(pc),a0
W(0x2084)                        # move.l d4,(a0)
W(0x4EAE, 0xFFC4)                # jsr -60(a6)         Output
W(0x2200)                        # move.l d0,d1
W(0x41FA); PC('ds1')             # lea ds1(pc),a0
W(0x2408)                        # move.l a0,d2
W(0x7624)                        # moveq #36,d3
W(0x4EAE, 0xFFD0)                # jsr -48(a6)         Write
W(0x224D)                        # movea.l a5,a1
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x4EAE, 0xFE62)                # jsr -414(a6)        CloseLibrary
W(0x0C84, expected >> 16, expected & 0xFFFF)   # cmpi.l #expected,d4
B8(0x66, 'bad')                  # bne.s bad
W(0x700C, 0x4E75)                # moveq #12,d0 ; rts
L('bad')
W(0x700A, 0x4E75)                # moveq #10,d0 ; rts
L('nodos')
W(0x7005, 0x4E75)                # moveq #5,d0 ; rts
L('dosname'); items.append(('data', b'dos.library\0'))
L('ds1'); items.append(('data', b'\0' * 12))
L('ds2'); items.append(('data', b'\0' * 12))
L('sum'); items.append(('data', b'\0' * 4))
items.append(('data', b'kjtime\n\0'))
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
        assert -128 <= d < 128 and d != 0 and d != -1, d
        body += bytes([it[1], d & 0xFF])
    elif it[0] == 'data':
        body += it[1]
    off += size(it)
assert labels['ds1'] % 2 == 0 and labels['ds2'] == labels['ds1'] + 12 and labels['sum'] == labels['ds1'] + 24
body += b'\0' * (-len(body) % 4)
longs = len(body) // 4
hunk = struct.pack('>6I', 0x3F3, 0, 1, 0, 0, longs) + struct.pack('>2I', 0x3E9, longs) + body + struct.pack('>I', 0x3F2)
open(sys.argv[1], 'wb').write(hunk)
print('kjtime: %d bytes, code and data %d bytes, N %d, expected sum %08x, labels %s' % (len(hunk), len(body), N, expected, {k: hex(v) for k, v in labels.items()}))
