#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""Extract unchanged generated arithmetic thunks and an independent Utility ABI oracle."""
from pathlib import Path
import re
import sys

source_dir, conf, output = map(Path, sys.argv[1:])
source = (source_dir / "kaffeine_thunks_gen.c").read_text()
bodies = []
for name in ("SMult32", "UMult32"):
    matches = re.findall(r"^static IPTR thunk_utility_" + name +
                         r"\([^;]*?\)\n\{.*?^\}", source, re.M | re.S)
    if len(matches) != 1:
        raise SystemExit("expected one generated thunk: " + name)
    bodies.append(matches[0])

# Classic user vectors start at -30, six bytes apart. Read names and skips
# from the public .conf, independently of the dispatcher policy and tables.
vectors, inside, lvo = [], False, 30
for line in conf.read_text().splitlines():
    line = line.strip()
    if line == "##begin functionlist":
        inside = True
        continue
    if line == "##end functionlist":
        break
    if not inside or not line or line.startswith("#"):
        continue
    skip = re.fullmatch(r"\.skip\s+(\d+)", line)
    if skip:
        lvo += 6 * int(skip[1])
        continue
    if line.startswith("."):
        continue
    name = re.search(r"\b([A-Za-z_]\w*)\s*\(", line)
    if not name:
        raise SystemExit("unrecognized Utility declaration: " + line)
    vectors.append((lvo, name[1]))
    lvo += 6
if not vectors:
    raise SystemExit("empty Utility ABI oracle")
output.write_text("/* Generated verbatim thunks and public utility.conf vector oracle. */\n" +
                  "\n\n".join(bodies) +
                  "\n\nstruct UtilityConfVector { ULONG lvo; const char *name; };\n" +
                  "static const UtilityConfVector utility_conf_vectors[] = {\n" +
                  "".join(f'    {{ {lvo}, "{name}" }},\n' for lvo, name in vectors) +
                  "};\n")
