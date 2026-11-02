#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Compile the manual Intuition CurrentTime() thunk verbatim.

The thunk and Kaffeine_IORefuse() are extracted on every production-source
change, never copied. Native CurrentTime() is the test's recording function.
Both LVO 84 registrations are copied as well, the generated one bound to a
poisoned thunk, so the real dispatcher shows which table wins. Fail closed if
a body, a registration or the manual table's macro rows change shape.
"""
from pathlib import Path
import re
import sys

core, output = map(Path, sys.argv[1:])
source = (core / "kaffeine_thunks.c").read_text()
generated = (core / "kaffeine_thunks_gen.c").read_text()
exec_io = (core / "kaffeine_exec_io.c").read_text()


def function(text, declaration):
    assert text.count(declaration) == 1, declaration
    begin = text.index(declaration)
    # Production top-level function braces are at column zero.
    return text[begin + 1:text.index("\n}", begin) + 2] + "\n"


def table(text, name):
    begin = text.index("const struct M68KThunkEntry " + name + "[] = {")
    return text[begin:text.index("\n};", begin) + 3]


def registration(text, lvo, name):
    matches = re.findall(r"\{\s*" + str(lvo) + r",\s*" + name + r"\s*\}", text)
    assert len(matches) == 1, (lvo, name)
    return matches[0]


manual_table = table(source, "kaffeine_thunks_intuition_manual")
manual = registration(manual_table, 84, "thunk_intuition_CurrentTime")
assert len(re.findall(r"\{\s*84\s*,", manual_table)) == 1
# The macro rows of the same table must not register LVO 84 a second time.
headers = "\n".join(path.read_text() for path in sorted(core.glob("kaffeine_*.h")))
macros = re.findall(r"\b(KAFFEINE_INTUITION_\w+_THUNKS)\b", manual_table)
assert macros
for macro in macros:
    rows = re.findall(r"#define " + macro + r"\b((?:[^\n]*\\\n)*[^\n]*)", headers)
    assert len(rows) == 1, macro
    assert not re.search(r"\{\s*84\s*,", rows[0]), macro
generated_entry = registration(table(generated, "kaffeine_thunks_intuition_gen"),
                               84, "thunk_intuition_CurrentTime")
thunk = function(source, "\nstatic IPTR thunk_intuition_CurrentTime(")
refuse = function(exec_io, "\nIPTR Kaffeine_IORefuse(")

output.write_text('''/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Generated from production sources by currenttime_extract.py. */
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_thunks.h"
#include "kaffeine_messages.h"
#include <aros/debug.h>
/* rom/intuition/intuition.conf. The test records the native call. */
void CurrentTime(ULONG *seconds, ULONG *micros);
void currenttime_debug(const char *format, ...);
IPTR currenttime_generated_thunk(struct KaffeineContext *ctx, void *cpu);
#define bug currenttime_debug
''' + refuse + "#undef bug\n" + thunk + '''
const struct M68KThunkEntry currenttime_manual[] = { ''' + manual + ''' };
#define thunk_intuition_CurrentTime currenttime_generated_thunk
const struct M68KThunkEntry currenttime_generated[] = { ''' + generated_entry + ''' };
#undef thunk_intuition_CurrentTime
''')
