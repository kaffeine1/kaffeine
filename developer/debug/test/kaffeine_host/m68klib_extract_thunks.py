"""Build the library and seglist thunks without unrelated GUI/native APIs.

The bodies are extracted on every production-source change, never copied or
reimplemented. Fail closed if their source boundaries or registrations change.
"""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
dos_source = Path(sys.argv[2]).read_text()
dos_header = Path(sys.argv[3]).read_text()
start = source.index("static IPTR open_library(")
end = source.index("/* -120/-126: Disable()/Enable()", start)
for lvo, name in ((552, "OpenLibrary"), (408, "OldOpenLibrary"), (414, "CloseLibrary")):
    assert re.search(r"\{\s*" + str(lvo) + r",\s*thunk_exec_" + name + r"\s*\}", source)
    assert source[start:end].count("static IPTR thunk_exec_" + name + "(") == 1

def function(text, declaration):
    begin = text.index(declaration)
    # Production top-level function braces are at column zero.
    return text[begin:text.index("\n}", begin) + 2] + "\n"


def registration(text, lvo, name):
    pattern = r"\{\s*" + str(lvo) + r",\s*" + name + r"\s*\}"
    matches = re.findall(pattern, text)
    assert len(matches) == 1, (lvo, name)
    return matches[0]


dos_table = source[source.index("const struct M68KThunkEntry kaffeine_thunks_dos[]"):
                   source.index("const ULONG kaffeine_thunks_dos_count")]
assert "KAFFEINE_DOS_BASE_THUNKS" in dos_table
unload_entry = registration(dos_header, 156, "kaffeine_dos_UnLoadSeg")
run_entry = registration(dos_header, 504, "kaffeine_dos_RunCommand")
dos_bodies = "\n".join((
    function(dos_source, "IPTR kaffeine_dos_UnLoadSeg("),
    function(dos_source, "static BOOL guest_range("),
    function(dos_source, "static const char *guest_string("),
    function(dos_source, "static IPTR bad_dos_buffer("),
    function(dos_source, "IPTR kaffeine_dos_RunCommand("),
))

Path(sys.argv[4]).write_text('''/* Generated from the production Exec and DOS thunks. */
#include <string.h>
#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "kaffeine_intern.h"
#include "kaffeine_locks.h"
#include "kaffeine_p96.h"
#include "kaffeine_thunks.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_segments.h"
#include "kaffeine_thunks_memory.h"
extern IPTR m68klib_test_stop(struct KaffeineContext *, void *);
''' + source[start:end] + dos_bodies + '''
const struct M68KThunkEntry kaffeine_thunks_exec[] = {
    {552, thunk_exec_OpenLibrary}, {408, thunk_exec_OldOpenLibrary},
    {414, thunk_exec_CloseLibrary}, {198, thunk_exec_AllocMem},
    {210, thunk_exec_FreeMem},
    {60, m68klib_test_stop}
};
const ULONG kaffeine_thunks_exec_count =
    sizeof(kaffeine_thunks_exec) / sizeof(kaffeine_thunks_exec[0]);
const struct M68KThunkEntry kaffeine_thunks_dos[] = {
    ''' + unload_entry + ", " + run_entry + ''', {0, NULL}
};
const ULONG kaffeine_thunks_dos_count =
    sizeof(kaffeine_thunks_dos) / sizeof(kaffeine_thunks_dos[0]) - 1;
''')
