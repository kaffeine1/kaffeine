"""Author: Michele Dipace (@kaffeine1)

Compares the engine sources here with the ones in an AROS tree that has
the engine (workbench/libs/m68kjit, its two test directories, and the
interface in rom/m68kemu).

AROS takes no component under a project's name, so there the engine is
m68kjit.library and every name that abbreviated "kaffeine-jit68" is
changed with it. A file here is the AROS file with the names of this
repository, plus the author line. Two files also differ in a few lines of
their first comment, which say here where the code comes from and say
there what it does. This script makes those changes on the text of each
file here and compares the result with the AROS file. Anything else is
reported, and the result is 1.

usage: python3 -I -B compare-with-aros.py AROS_TREE
"""
import re
import sys
from pathlib import Path

here = Path(__file__).resolve().parent.parent
aros = Path(sys.argv[1])
LIB = 'workbench/libs/m68kjit/'
PROBE = 'developer/debug/test/m68kjit/'
HOST = 'developer/debug/test/m68kjit_host/'
PAIRS = [
    ('include/m68kemu_engine.h', 'rom/m68kemu/m68kemu_engine.h'),
    ('include/ENGINE.md', 'rom/m68kemu/ENGINE.md'),
    ('engine/kj68_engine.c', LIB + 'm68kjit_engine.c'),
    ('engine/kj68_intern.h', LIB + 'm68kjit_intern.h'),
    ('engine/kj68_platform.h', LIB + 'm68kjit_platform.h'),
    ('engine/kj68_translate.c', LIB + 'm68kjit_translate.c'),
    ('aros/kaffeine-jit68.conf', LIB + 'm68kjit.conf'),
    ('aros/kj68_library.c', LIB + 'm68kjit_library.c'),
    ('aros/kj68_platform.c', LIB + 'm68kjit_platform.c'),
    ('aros/kj68probe.c', PROBE + 'm68kjitprobe.c'),
    ('host/kj68_host_tests.c', HOST + 'm68kjit_host_tests.c'),
    ('host/kj68_moira_compare.cpp', HOST + 'm68kjit_moira_compare.cpp'),
    ('host/kj68_platform_host.c', HOST + 'm68kjit_platform_host.c'),
    ('host/include/exec/types.h', HOST + 'include/exec/types.h'),
]


def aros_names(t):
    """The names of this repository as AROS has them."""
    t = t.replace('kaffeine_jit68_host', 'm68kjit_host').replace('kaffeine-jit68', 'm68kjit')
    t = t.replace('-DKJ68_', '-DM68KJIT_')      # a compiler option quoted in a message
    t = re.sub(r'\bKJ68_GetEngine', 'M68KJit_GetEngine', t)
    t = re.sub(r'\bKJ68Base\b', 'M68KJitBase', t)
    t = re.sub(r'\bKJ68Counters\b', 'M68KJitCounters', t)
    t = re.sub(r'\bKJ68PROBE\b', 'M68KJITPROBE', t)
    t = re.sub(r'\bKJ68_', 'M68KJIT_', t)
    t = re.sub(r'\bKJ68\b', 'M68KJit', t)
    t = re.sub(r'\bkj68probe\b', 'm68kjitprobe', t)
    t = re.sub(r'\bkj68_', 'm68kjit_', t)
    t = re.sub(r'\bkj_', 'mj_', t)
    return t


# The first comment of the translator: where it comes from, here; what it does, there.
TRANSLATOR_HERE = r'    m68kjit engine: the AArch64 translator\. It comes from the.*?It contains no\s+Emu68 code\.\n'
TRANSLATOR_AROS = ("    m68kjit engine: the AArch64 translator. It decodes 68k instructions\n"
                   "    and emits AArch64 code for them: guest memory through the host's map,\n"
                   "    an exit to the host wherever the host has to act, translations shared\n"
                   "    by the CPUs of a domain.\n")
# The first comment of the host checks: which earlier suite their programs come from.
CHECKS_HERE = ("    and expected results of the hosted suite of kaffeine.library, the earlier\n"
               "    library of the Kaffeine project, called the legacy suite below (test_hosted_jit.cpp,\n"
               "    sha256 c451efc8) are reused where their oracle is not Moira.\n")
CHECKS_AROS = ("    and expected results of an earlier hosted suite of this translator,\n"
               "    called the legacy suite below, are reused where their oracle is not\n"
               "    Moira.\n")

bad = 0
for mine, theirs in PAIRS:
    a = (here / mine).read_text(encoding='ascii')
    b = (aros / theirs).read_text(encoding='ascii')
    a = ''.join(l for l in a.splitlines(keepends=True) if l.strip() != 'Author: Michele Dipace (@kaffeine1)')
    a = aros_names(a)
    if mine == 'engine/kj68_translate.c':
        a, n = re.subn(TRANSLATOR_HERE, lambda m: TRANSLATOR_AROS, a, flags=re.S)
        assert n == 1
    if mine == 'host/kj68_host_tests.c':
        assert a.count(CHECKS_HERE) == 1
        a = a.replace(CHECKS_HERE, CHECKS_AROS)
    if a != b:
        bad += 1
        la, lb = a.split('\n'), b.split('\n')
        first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
        print('DIFFERS %s and %s, first at line %d' % (mine, theirs, first + 1))
        print('  here: %s' % (la[first] if first < len(la) else '(end)'))
        print('  AROS: %s' % (lb[first] if first < len(lb) else '(end)'))
print('%d files compared, %d differ' % (len(PAIRS), bad))
sys.exit(1 if bad else 0)
