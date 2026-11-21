# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write the two CIA timer programs that AROS's test_m68kemu.c embeds as hunk files, byte for byte.
usage: extract_cia.py <test_m68kemu.c> <out dir>"""
import re, struct, sys
src = open(sys.argv[1]).read()
for array, name in (('m68k_cia_binary', 'ciawait'), ('m68k_cia_busy_binary', 'ciabusy')):
    m = re.search(r'static const UBYTE %s\[\] = \{(.*?)\};' % array, src, re.S)
    b = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
    longs = struct.unpack('>I', b[20:24])[0]
    assert b[:4] == b'\x00\x00\x03\xf3' and b[-4:] == b'\x00\x00\x03\xf2' and len(b) == 32 + 4 * longs + 4, (name, len(b), longs)
    open('%s/%s' % (sys.argv[2], name), 'wb').write(b)
    print('%s: %s, %d bytes, %d longwords of code' % (name, array, len(b), longs))
