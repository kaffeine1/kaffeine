"""Compile production OpenLibrary lookup/wrappers without native GUI APIs.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)
Bodies, registrations and generated tables are extracted verbatim. Native
table functions are poisoned; setup and disk loading are test collaborators.
"""
from pathlib import Path
import re
import sys


def one(pattern, text):
    matches = re.findall(pattern, text, re.S)
    assert len(matches) == 1, pattern
    return matches[0]


source_dir, output = map(Path, sys.argv[1:])
source = (source_dir / "kaffeine_thunks.c").read_text()
generated = (source_dir / "kaffeine_thunks_gen.c").read_text()
memory = (source_dir / "kaffeine_memory.c").read_text()
lookup = one(r"(ULONG Kaffeine_OpenLibrary\([^\n]*\n\{.*?\n\})", source)
wrappers = source[source.index("static IPTR open_library("):
                  source.index("/* -414: CloseLibrary(")]
find = one(r"(ULONG Kaffeine_FindLibBase\([^\n]*\n\{.*?\n\})", memory)
entries = []
for lvo, name in ((552, "OpenLibrary"), (408, "OldOpenLibrary")):
    entries.append(one(r"(\{\s*" + str(lvo) + r",\s*thunk_exec_" + name + r"\s*\})", source))
    assert wrappers.count("static IPTR thunk_exec_" + name + "(") == 1

tables = re.findall(r"const struct M68KThunkEntry kaffeine_thunks_\w+_gen\[\] = \{.*?\n\};", generated, re.S)
assert tables
registry = one(r"(const struct M68KLibThunkSet kaffeine_all_gen_libs\[\] = \{.*?\n\};)", generated)
count = one(r"(const ULONG kaffeine_all_gen_libs_count = \d+;)", generated)
symbols = sorted(set(re.findall(r"\{\s*\d+,\s*(thunk_\w+)\s*\}", "\n".join(tables))))
manual = sorted({name.removesuffix("_count") for name in
                 re.findall(r"\bkaffeine_thunks_\w+\b", lookup)})

output.write_text('''/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Generated from production sources by library_paths_extract.py. */
#include <string.h>
#include <aros/debug.h>
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
#include "kaffeine_gfxbase.h"
#include "kaffeine_p96.h"
extern IPTR library_paths_unexpected_thunk(struct KaffeineContext *, void *);
''' + "\n".join(
    f"extern const struct M68KThunkEntry {name}[];\nextern const ULONG {name}_count;"
    for name in manual
) + "\n" + "\n".join(f"#define {name} library_paths_unexpected_thunk" for name in symbols)
  + "\n" + "\n".join(tables) + "\n" + registry + "\n" + count + "\n"
  + "\n".join(f"#undef {name}" for name in symbols) + "\n"
  + find + "\n" + lookup + "\n" + wrappers
  + "\nconst struct M68KThunkEntry kaffeine_thunks_exec[] = {\n    "
  + ",\n    ".join(entries) + '''
};
const ULONG kaffeine_thunks_exec_count =
    sizeof(kaffeine_thunks_exec) / sizeof(kaffeine_thunks_exec[0]);
''')
