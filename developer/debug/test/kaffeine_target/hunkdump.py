#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Print the hunk structure of an Amiga HUNK executable, including the
# relocation format (long HUNK_RELOC32, or the 16-bit HUNK_RELOC32SHORT
# and HUNK_DREL32 forms), to check the test programs. Symbol and debug
# hunks are counted and skipped.
#   hunkdump.py FILE...
import struct
import sys

NAMES = {0x3e9: "CODE", 0x3ea: "DATA", 0x3eb: "BSS", 0x3ec: "RELOC32",
         0x3f0: "SYMBOL", 0x3f1: "DEBUG", 0x3f2: "END",
         0x3f7: "DREL32 (short format)", 0x3fc: "RELOC32SHORT"}


def offsets(offs):
    if len(offs) <= 8:
        return str(offs)
    return "%d offsets %s" % (len(offs), str(offs[:8])[:-1] + ", ...]")


def dump(path):
    data = open(path, "rb").read()
    pos = 0

    def get(fmt):
        nonlocal pos
        vals = struct.unpack_from(">" + fmt, data, pos)
        pos += struct.calcsize(">" + fmt)
        return vals

    assert get("L")[0] == 0x3f3, "not a HUNK executable"
    while get("L")[0]:
        pass
    num, first, last = get("LLL")
    sizes = get("%dL" % (last - first + 1))
    print("%s: %d bytes, hunks %d..%d, sizes %s" % (path, len(data), first, last,
                                                   [s * 4 for s in sizes]))
    while pos < len(data):
        kind = get("L")[0] & 0x3fffffff
        name = NAMES.get(kind, hex(kind))
        if kind in (0x3e9, 0x3ea, 0x3eb):
            n = get("L")[0]
            if kind != 0x3eb:
                pos += 4 * n
            print("  %s %d bytes" % (name, 4 * n))
        elif kind == 0x3ec:
            while True:
                n = get("L")[0]
                if not n:
                    break
                target = get("L")[0]
                print("  %s -> hunk %d at %s" % (name, target, offsets(list(get("%dL" % n)))))
        elif kind in (0x3f7, 0x3fc):
            words = 0
            while True:
                n = get("H")[0]
                words += 1
                if not n:
                    break
                target = get("H")[0]
                offs = list(get("%dH" % n))
                words += 1 + n
                print("  %s -> hunk %d at %s" % (name, target, offsets(offs)))
            if words % 2:
                pos += 2
        elif kind == 0x3f0:
            symbols = 0
            while True:
                n = get("L")[0] & 0xffffff
                if not n:
                    break
                pos += 4 * n + 4
                symbols += 1
            print("  %s, %d symbols" % (name, symbols))
        elif kind == 0x3f1:
            n = get("L")[0]
            pos += 4 * n
            print("  %s %d bytes" % (name, 4 * n))
        elif kind == 0x3f2:
            print("  END")
        else:
            raise SystemExit("unsupported hunk %s" % name)


for arg in sys.argv[1:]:
    dump(arg)
