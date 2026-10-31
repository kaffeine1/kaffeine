#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Compile unchanged public Intuition refresh bodies over recording layers."""
from pathlib import Path
import re
import sys

root, output = map(Path, sys.argv[1:])
parts = ["/* Native public Intuition bodies; ABI macros come from the fixture. */\n"]
for name, args in (("BeginRefresh", "struct Window *window"),
                   ("EndRefresh", "struct Window *window, BOOL complete")):
    source = (root / "rom/intuition" / (name.lower() + ".c")).read_text()
    matches = list(re.finditer(r"\n\{\n\s*AROS_LIBFUNC_INIT", source))
    if len(matches) != 1:
        raise SystemExit("expected exactly one native body: " + name)
    start = matches[0].start() + 1
    end, depth = start + 1, 1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    parts.append(f"static void source_{name}({args})\n" + source[start:end] + "\n")
output.write_text("\n".join(parts))
