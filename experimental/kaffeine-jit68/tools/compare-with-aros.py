"""Author: Michele Dipace (@kaffeine1)

Compares the engine sources here with the ones in an AROS tree that has
the branches proposed to AROS (workbench/libs/kaffeine-jit68, its two test
directories, and the interface in rom/m68kemu). A file here is the AROS
file plus the author line; the translator also differs in one sentence of
its first comment, because here a script writes it. Anything else is
reported, and the result is 1.

usage: python3 -I -B compare-with-aros.py AROS_TREE
"""
import sys
from pathlib import Path

here = Path(__file__).resolve().parent.parent
aros = Path(sys.argv[1])
LIB = 'workbench/libs/kaffeine-jit68/'
PROBE = 'developer/debug/test/kaffeine-jit68/'
HOST = 'developer/debug/test/kaffeine_jit68_host/'
PAIRS = [
    ('include/m68kemu_engine.h', 'rom/m68kemu/m68kemu_engine.h'),
    ('include/ENGINE.md', 'rom/m68kemu/ENGINE.md'),
    ('engine/kj68_engine.c', LIB + 'kj68_engine.c'),
    ('engine/kj68_intern.h', LIB + 'kj68_intern.h'),
    ('engine/kj68_platform.h', LIB + 'kj68_platform.h'),
    ('engine/kj68_translate.c', LIB + 'kj68_translate.c'),
    ('aros/kaffeine-jit68.conf', LIB + 'kaffeine-jit68.conf'),
    ('aros/kj68_library.c', LIB + 'kj68_library.c'),
    ('aros/kj68_platform.c', LIB + 'kj68_platform.c'),
    ('aros/kj68probe.c', PROBE + 'kj68probe.c'),
    ('host/kj68_host_tests.c', HOST + 'kj68_host_tests.c'),
    ('host/kj68_platform_host.c', HOST + 'kj68_platform_host.c'),
    ('host/include/exec/types.h', HOST + 'include/exec/types.h'),
]
HERE_SAYS = ('    tools/make-translate.py writes this file from that one. It contains no\n'
             '    Emu68 code.\n')
AROS_SAYS = '    From here on it is source of this tree. It contains no Emu68 code.\n'
bad = 0
for mine, theirs in PAIRS:
    a = (here / mine).read_text(encoding='ascii')
    b = (aros / theirs).read_text(encoding='ascii')
    a = ''.join(l for l in a.splitlines(keepends=True) if l.strip() != 'Author: Michele Dipace (@kaffeine1)')
    if mine == 'engine/kj68_translate.c':
        assert a.count(HERE_SAYS) == 1
        a = a.replace(HERE_SAYS, AROS_SAYS)
    if a != b:
        bad += 1
        la, lb = a.split('\n'), b.split('\n')
        first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
        print('DIFFERS %s and %s, first at line %d' % (mine, theirs, first + 1))
print('%d files compared, %d differ' % (len(PAIRS), bad))
sys.exit(1 if bad else 0)
