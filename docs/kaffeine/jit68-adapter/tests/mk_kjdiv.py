# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjdiv, a 68k AmigaOS program as one hunk, assembled by this script. usage: mk_kjdiv.py <out>

Three divisions, each checked by the program:
  1. DIVU.W (A0),D0 with A0 at the custom chip register POTGOR ($DFF016), which m68kemu.library answers with $FF00:
     the divisor is read through the library, so an engine declines the instruction and the interpreter executes it;
  2. DIVU.W (A1),D1 with the divisor in the program's own memory: an engine divides;
  3. DIVS.W ($DFF016).L,D2: the same register through an absolute address, signed.
It returns 12 when all three results are right, otherwise the number of the first wrong one.
"""
import struct, sys
POTGOR = 0xFF00
def divu(n, d):
    q, r = divmod(n, d)
    assert q < 0x10000
    return (r << 16) | q
def divs(n, d):                  # 68k: the quotient is truncated toward zero, the remainder has the dividend's sign
    q = abs(n) // abs(d) * (1 if (n < 0) == (d < 0) else -1)
    r = n - q * d
    assert -32768 <= q < 32768
    return ((r & 0xFFFF) << 16) | (q & 0xFFFF)
n1, n2, d2, n3 = 0x00FE0100, 0x00123456, 0x1234, -25600
r1, r2, r3 = divu(n1, POTGOR), divu(n2, d2), divs(n3, POTGOR - 0x10000)
items = []
W = lambda *ws: items.extend(('w', w & 0xFFFF) for w in ws)
L = lambda name: items.append(('label', name))
PC = lambda label: items.append(('pc', label))
B8 = lambda op, label: items.append(('b8', op, label))
long = lambda v: ((v >> 16) & 0xFFFF, v & 0xFFFF)
W(0x207C, 0x00DF, 0xF016)        # movea.l #$00DFF016,a0
W(0x203C, *long(n1))             # move.l #n1,d0
W(0x80D0)                        # divu.w (a0),d0
W(0x0C80, *long(r1))             # cmpi.l #r1,d0
B8(0x66, 'bad1')                 # bne.s bad1
W(0x43FA); PC('divisor')         # lea divisor(pc),a1
W(0x223C, *long(n2))             # move.l #n2,d1
W(0x82D1)                        # divu.w (a1),d1
W(0x0C81, *long(r2))             # cmpi.l #r2,d1
B8(0x66, 'bad2')                 # bne.s bad2
W(0x243C, *long(n3))             # move.l #n3,d2
W(0x85F9, 0x00DF, 0xF016)        # divs.w ($00DFF016).l,d2
W(0x0C82, *long(r3))             # cmpi.l #r3,d2
B8(0x66, 'bad3')                 # bne.s bad3
W(0x700C, 0x4E75)                # moveq #12,d0 ; rts
L('bad1'); W(0x7001, 0x4E75)     # moveq #1,d0 ; rts
L('bad2'); W(0x7002, 0x4E75)     # moveq #2,d0 ; rts
L('bad3'); W(0x7003, 0x4E75)     # moveq #3,d0 ; rts
L('divisor'); W(d2)
size = lambda it: 0 if it[0] == 'label' else 2
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
        assert -128 <= d < 128 and d != 0 and d != -1, d
        body += bytes([it[1], d & 0xFF])
    off += size(it)
body += b'\0' * (-len(body) % 4)
longs = len(body) // 4
hunk = struct.pack('>6I', 0x3F3, 0, 1, 0, 0, longs) + struct.pack('>2I', 0x3E9, longs) + body + struct.pack('>I', 0x3F2)
open(sys.argv[1], 'wb').write(hunk)
print('kjdiv: %d bytes, code %d bytes, results %08x %08x %08x' % (len(hunk), len(body), r1, r2, r3))
