# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjmix, a 68k AmigaOS program as one hunk, assembled by this script. usage: mk_kjmix.py <out>

Four small things one after the other, each checked by the program itself:
  1. a buffer of 1024 bytes on the stack filled and summed with two DBRA loops (byte moves, postincrement);
  2. a subroutine with MOVEM to and from the stack and a multiplication;
  3. TRAP #0 with a handler of the program in vector 32, which returns with RTE;
  4. exec Supervisor() with a function of the program, which returns with RTE.
It returns 12 when all four are right, otherwise the number of the first one that is wrong (1 to 4).
"""
import struct, sys
mem = sum((3 + 7 * i) & 0xFF for i in range(1024))
items = []
W = lambda *ws: items.extend(('w', w) for w in ws)
L = lambda name: items.append(('label', name))
PC = lambda label: items.append(('pc', label))               # 16-bit displacement from this word
B8 = lambda op, label: items.append(('b8', op, label))       # Bcc.s / BSR.s
W(0x2C78, 0x0004)                # movea.l 4.w,a6
W(0x4FEF, 0xFC00)                # lea -1024(sp),sp
W(0x204F)                        # movea.l sp,a0
W(0x303C, 0x03FF)                # move.w #1023,d0
W(0x7203)                        # moveq #3,d1
L('fill')
W(0x10C1)                        # move.b d1,(a0)+
W(0x5E01)                        # addq.b #7,d1
W(0x51C8); PC('fill')            # dbra d0,fill
W(0x204F)                        # movea.l sp,a0
W(0x303C, 0x03FF)                # move.w #1023,d0
W(0x7800)                        # moveq #0,d4
W(0x7200)                        # moveq #0,d1
L('sum')
W(0x1218)                        # move.b (a0)+,d1
W(0xD881)                        # add.l d1,d4
W(0x51C8); PC('sum')             # dbra d0,sum
W(0x4FEF, 0x0400)                # lea 1024(sp),sp
W(0x0C84, mem >> 16, mem & 0xFFFF)   # cmpi.l #mem,d4
B8(0x67, 'memok')                # beq.s memok
W(0x7001, 0x4E75)                # moveq #1,d0 ; rts
L('memok')
W(0x7405)                        # moveq #5,d2
W(0x7606)                        # moveq #6,d3
B8(0x61, 'sub')                  # bsr.s sub
W(0x0C80, 0x0000, 0x001E)        # cmpi.l #30,d0
B8(0x67, 'subok')                # beq.s subok
W(0x7002, 0x4E75)                # moveq #2,d0 ; rts
L('subok')
W(0x41FA); PC('handler')         # lea handler(pc),a0
W(0x2C38, 0x0080)                # move.l $80.w,d6
W(0x21C8, 0x0080)                # move.l a0,$80.w
W(0x7A00)                        # moveq #0,d5
W(0x4E40)                        # trap #0
W(0x21C6, 0x0080)                # move.l d6,$80.w
W(0x0C85, 0x0000, 0x004D)        # cmpi.l #77,d5
B8(0x67, 'trapok')               # beq.s trapok
W(0x7003, 0x4E75)                # moveq #3,d0 ; rts
L('trapok')
W(0x7A00)                        # moveq #0,d5
W(0x4BFA); PC('svfunc')          # lea svfunc(pc),a5
W(0x4EAE, 0xFFE2)                # jsr -30(a6)         Supervisor
W(0x0C85, 0x0000, 0x0058)        # cmpi.l #88,d5
B8(0x67, 'svok')                 # beq.s svok
W(0x7004, 0x4E75)                # moveq #4,d0 ; rts
L('svok')
W(0x700C, 0x4E75)                # moveq #12,d0 ; rts
L('sub')
W(0x48E7, 0x3000)                # movem.l d2-d3,-(sp)
W(0x2002)                        # move.l d2,d0
W(0xC0C3)                        # mulu.w d3,d0
W(0x4CDF, 0x000C)                # movem.l (sp)+,d2-d3
W(0x4E75)                        # rts
L('handler')
W(0x7A4D, 0x4E73)                # moveq #77,d5 ; rte
L('svfunc')
W(0x7A58, 0x4E73)                # moveq #88,d5 ; rte
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
print('kjmix: %d bytes, code %d bytes, memory sum %08x, labels %s' % (len(hunk), len(body), mem, {k: hex(v) for k, v in labels.items()}))
