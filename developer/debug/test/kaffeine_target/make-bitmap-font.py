#!/usr/bin/env python3
"""Build an original, redistributable 68k bitmap-font test fixture.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)

The files use the public DiskFontHeader, FontContents and Amiga HUNK
formats. No installed font, program, ROM or private font data is read.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def make_font():
    name = b"kaffeinedfprobe.font\0"
    data = bytearray(110)  # moveq/rts, DiskFontHeader, 52-byte TextFont
    data[:4] = bytes.fromhex("70ff4e75")
    relocs = []

    def word(offset, value):
        struct.pack_into(">H", data, offset, value & 0xffff)

    def pointer(offset, target):
        struct.pack_into(">I", data, offset, target)
        relocs.append(offset)

    # The DiskFontHeader starts after a four-byte executable failure stub.
    data[12] = 12  # NT_FONT in dfh_DF
    pointer(14, 26)
    word(18, 0x0f80)  # DFH_ID
    word(20, 0)  # revision
    data[26:26 + len(name)] = name

    tf = 58
    data[tf + 8] = 12
    pointer(tf + 10, 26)
    word(tf + 18, 0)
    word(tf + 20, 8)
    data[tf + 22] = 0
    data[tf + 23] = 0x62  # DISKFONT | PROPORTIONAL | DESIGNED
    for offset, value in ((24, 4), (26, 6), (28, 1), (30, 0), (38, 2)):
        word(tf + offset, value)
    data[tf + 32] = 65
    data[tf + 33] = 67

    bitmap = bytes.fromhex("0000 5b60 b6a0 fb60 b6a0 bb60 0000 0000")
    glyphs = (0x00000003, 0x00030003, 0x00060003, 0x00090003)
    spacing = (4, 5, 4, 4)
    kerning = (0, -1, 1, 0)
    pointer(tf + 34, len(data))
    data.extend(bitmap)
    data.extend(b"\0" * (-len(data) % 4))
    pointer(tf + 40, len(data))
    data.extend(struct.pack(">4I", *glyphs))
    pointer(tf + 44, len(data))
    data.extend(struct.pack(">4h", *spacing))
    pointer(tf + 48, len(data))
    data.extend(struct.pack(">4h", *kerning))
    data.extend(b"\0" * (-len(data) % 4))
    longs = len(data) // 4

    # One CHIP-memory code hunk. The stub returns -1 if run accidentally.
    hunk = struct.pack(">8I", 1011, 0, 1, 0, 0, longs | 0x40000000, 1001, longs)
    hunk += data
    hunk += struct.pack(">3I", 1004, len(relocs), 0)
    hunk += struct.pack(">" + "I" * len(relocs), *relocs)
    hunk += struct.pack(">2I", 0, 1010)

    path = b"kaffeinedfprobe/8\0"
    descriptor = struct.pack(">2H", 0x0f00, 1)
    descriptor += path + bytes(256 - len(path))
    descriptor += struct.pack(">HBB", 8, 0, 0x62)
    assert len(descriptor) == 264
    assert relocs == [14, 68, 92, 98, 102, 106]
    return {"kaffeinedfprobe.font": descriptor, "kaffeinedfprobe/8": hunk}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = []
    for name, data in make_font().items():
        path = args.output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        manifest.append({"path": name, "bytes": len(data),
                         "sha256": hashlib.sha256(data).hexdigest()})
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
