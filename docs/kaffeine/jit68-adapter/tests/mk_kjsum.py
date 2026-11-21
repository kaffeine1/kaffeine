# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write kjsum, a 68k AmigaOS program as one hunk, assembled by hand. usage: mk_kjsum.py <out>

It opens dos.library, adds the squares of 1..20000 in a five-instruction loop (100000 instructions), writes a line to
Output(), closes the library and returns 12 when the sum is right, 10 when it is wrong, 5 without dos.library.
(Results stay below 21: the startup script of the test stops at FailAt 21 and would lose the result.)
"""
import struct, sys
N = 20000
expected = sum(i * i for i in range(1, N + 1)) & 0xFFFFFFFF
msg = b'kj68 hello\n'
w = []          # (offset, words...) in order
code = [
    0x2C78, 0x0004,             # 00 movea.l 4.w,a6
    0x43FA, 0x0052,             # 04 lea dosname(pc),a1          0x58 - 0x06
    0x7000,                     # 08 moveq #0,d0
    0x4EAE, 0xFDD8,             # 0A jsr -552(a6)                OpenLibrary
    0x4A80,                     # 0E tst.l d0
    0x6742,                     # 10 beq.s nodos                 0x54 - 0x12
    0x2A40,                     # 12 movea.l d0,a5
    0x7800,                     # 14 moveq #0,d4
    0x263C, N >> 16, N & 0xFFFF,  # 16 move.l #N,d3
    0x2203,                     # 1C loop: move.l d3,d1
    0xC2C3,                     # 1E mulu.w d3,d1
    0xD881,                     # 20 add.l d1,d4
    0x5383,                     # 22 subq.l #1,d3
    0x66F6,                     # 24 bne.s loop
    0x2C4D,                     # 26 movea.l a5,a6
    0x4EAE, 0xFFC4,             # 28 jsr -60(a6)                 Output
    0x2200,                     # 2C move.l d0,d1
    0x41FA, 0x0034,             # 2E lea msg(pc),a0              0x64 - 0x30
    0x2408,                     # 32 move.l a0,d2
    0x7600 | len(msg),          # 34 moveq #len,d3
    0x4EAE, 0xFFD0,             # 36 jsr -48(a6)                 Write
    0x224D,                     # 3A movea.l a5,a1
    0x2C78, 0x0004,             # 3C movea.l 4.w,a6
    0x4EAE, 0xFE62,             # 40 jsr -414(a6)                CloseLibrary
    0x0C84, expected >> 16, expected & 0xFFFF,  # 44 cmpi.l #expected,d4
    0x6604,                     # 4A bne.s bad
    0x700C,                     # 4C moveq #12,d0
    0x4E75,                     # 4E rts
    0x700A,                     # 50 bad: moveq #10,d0
    0x4E75,                     # 52 rts
    0x7005,                     # 54 nodos: moveq #5,d0
    0x4E75,                     # 56 rts
]
body = b''.join(struct.pack('>H', x) for x in code)
assert len(body) == 0x58, hex(len(body))
body += b'dos.library\0'
assert len(body) == 0x64
body += msg
body += b'\0' * (-len(body) % 4)
longs = len(body) // 4
hunk = struct.pack('>6I', 0x3F3, 0, 1, 0, 0, longs) + struct.pack('>2I', 0x3E9, longs) + body + struct.pack('>I', 0x3F2)
open(sys.argv[1], 'wb').write(hunk)
print('kjsum: %d bytes, code %d bytes, expected sum %08x' % (len(hunk), len(body), expected))
