"""Compile selected production generated RastPort thunks verbatim in the host suite.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""
from pathlib import Path
import re
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
names = ("PolyDraw", "SetRast", "BltTemplate", "ClipBlit")
functions = []
for name in names:
    matches = re.findall(
        r"(static IPTR thunk_graphics_" + name + r"\([^\n]*\n\{.*?\n\})",
        text, re.S)
    assert len(matches) == 1, name
    functions.append(matches[0])

# The complete checked-in generated file must stop after every failed
# RastPort resolution, including functions outside the selected fixture.
declarations = list(re.finditer(
    r"struct RastPort \*(arg_\w+) = resolve_rp\([^\n]*;\n", text))
assert declarations
for declaration in declarations:
    after = text[declaration.end():]
    assert re.match(r"\s*if \(!" + declaration[1] + r"\)", after), declaration[1]

output.write_text('''/* Extracted verbatim from the production generated file. */
#include <proto/graphics.h>
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
extern struct RastPort *resolve_rp(struct KaffeineContext *, ULONG);
''' + "\n\n".join(functions) + "\n" + "\n".join(
    "IPTR gfx_generated_" + name + "(struct KaffeineContext *ctx, void *cpu)\n"
    "{ return thunk_graphics_" + name + "(ctx, cpu); }"
    for name in names) + "\n")
