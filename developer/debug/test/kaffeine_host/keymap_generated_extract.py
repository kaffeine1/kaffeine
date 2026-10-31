"""Compile the production generated keymap fallback table verbatim.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
"""
from pathlib import Path
import re
import sys


source, output = map(Path, sys.argv[1:])
text = source.read_text()
functions = []
for name in ("SetKeyMapDefault", "AskKeyMapDefault", "MapRawKey", "MapANSI"):
    matches = re.findall(
        r"(static IPTR thunk_keymap_" + name + r"\([^\n]*\n\{.*?\n\})",
        text, re.S)
    assert len(matches) == 1, name
    functions.append(matches[0])
tables = re.findall(
    r"const struct M68KThunkEntry kaffeine_thunks_keymap_gen\[\] = \{.*?\n\};\n"
    r"const ULONG kaffeine_thunks_keymap_gen_count = \d+;", text, re.S)
assert len(tables) == 1
output.write_text('''/* Extracted verbatim from the production generated file. */
#include <proto/keymap.h>
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
''' + "\n\n".join(functions) + "\n\n" + tables[0] + "\n")
