# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Write arial.font, the font contents file that diskfont.library looks for in FONTS: before it opens arial/15.
The AROS build installs the size files of workbench/fonts/arial and no contents file. One entry, for the size
file given: its height, style and flags are read from the TextFont in it. usage: mk_arialfont.py <arial/15> <out>"""
import struct, sys
src, out = sys.argv[1:3]
b = open(src, 'rb').read()
assert struct.unpack('>I', b[:4])[0] == 0x3F3, 'not a hunk file'
# The file: a hunk header and HUNK_CODE with its length (32 bytes), "moveq #-1,d0; rts" (4), then the
# DiskFontHeader: its node (14), dfh_FileID (2), dfh_Revision (2), dfh_Segment (4), dfh_Name (32) and dfh_TF,
# whose message is 20 bytes; tf_YSize, tf_Style and tf_Flags follow it.
assert struct.unpack('>H', b[0x32:0x34])[0] == 0x0F80, 'no DiskFontHeader where one is expected'
tf = 32 + 4 + 14 + 2 + 2 + 4 + 32
ysize, style, flags = struct.unpack('>HBB', b[tf + 20:tf + 24])
assert ysize == 15 and flags & 0x02, (ysize, style, flags)
name = b'arial/15'
entry = name + bytes(256 - len(name)) + struct.pack('>HBB', ysize, style, flags)
open(out, 'wb').write(struct.pack('>HH', 0x0F00, 1) + entry)
print('ysize %d style 0x%02x flags 0x%02x, %d bytes' % (ysize, style, flags, 4 + len(entry)))
