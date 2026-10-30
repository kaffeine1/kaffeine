#!/usr/bin/env python3
"""Build the real graphics-base setup, font registry, heap and library route."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def extract_function(source, name):
    start = re.search(r"^ULONG " + re.escape(name) + r"\(", source, re.MULTILINE)
    if not start:
        raise ValueError("Missing production function: " + name)
    opening = source.index("{", start.start())
    # Skip comments and string/character literals while counting real braces.
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         source[opening:], re.DOTALL)
    depth = 0
    for match in tokens:
        token = match.group()
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if not depth:
                return source[start.start():opening + match.end()] + "\n"
    raise ValueError("Unterminated production function: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--reservation-only", action="store_true")
    args = parser.parse_args()
    test_dir = Path(__file__).resolve().parent
    source_dir = (args.source_dir or test_dir.parents[3] / "rom/kaffeine").resolve()
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    shared = test_dir.parent / "kaffeine_host"
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-ffunction-sections", "-fdata-sections", "-I" + str(test_dir / "gfxbase_stubs"),
             "-I" + str(shared / "gfx_stubs"), "-I" + str(shared / "memory_stubs"),
             "-I" + str(shared / "stubs"), "-I" + str(source_dir)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    function = extract_function((source_dir / "kaffeine_thunks.c").read_text(),
                                "Kaffeine_OpenLibrary")
    symbols = sorted(set(re.findall(r"\bkaffeine_thunks_[a-z0-9_]+\b", function)))
    wrapper = build_dir / "open_library.c"
    wrapper.write_text('#include <string.h>\n#include <aros/debug.h>\n'
                       '#include "kaffeine_intern.h"\n#include "kaffeine_thunks.h"\n'
                       '#include "kaffeine_gfxbase.h"\n' +
                       "\n".join(("const ULONG " + n + " = 0;" if n.endswith("_count") else
                                   "const struct M68KThunkEntry " + n + "[] = {{0, NULL}};")
                                  for n in symbols) + "\n" + function)
    commands, objects = [], []
    names = ["memory", "messages", "cpu", "rastport"]
    if "Kaffeine_SetupGraphicsBase" in function:
        names += ["gfxbase"]
    for name in names + ["open_library"]:
        source = wrapper if name == "open_library" else source_dir / ("kaffeine_" + name + ".c")
        target = build_dir / (name + ".o")
        commands.append([*shlex.split(os.environ.get("CC", "clang")), "-std=c99", *flags,
                         "-c", str(source), "-o", str(target)])
        objects.append(str(target))
    commands.append([*shlex.split(os.environ.get("CXX", "clang++")), "-std=c++20", *flags,
                     str(test_dir / "test_gfxbase.cpp"), *objects,
                     "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                     "-o", str(build_dir / "test_gfxbase")])
    commands.append([str(build_dir / "test_gfxbase")] +
                    (["--reservation-only"] if args.reservation_only else []))
    (build_dir / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    logs = []
    for command in commands:
        print(shlex.join(command), flush=True)
        env = dict(os.environ)
        if args.sanitize:
            env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1")
            env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        completed = subprocess.run(command, env=env, text=True, timeout=30,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(completed.stdout, end="", flush=True)
        logs.append(shlex.join(command) + "\n" + completed.stdout)
        (build_dir / "run.log").write_text("\n".join(logs))
        if completed.returncode:
            raise subprocess.CalledProcessError(completed.returncode, command)


if __name__ == "__main__":
    main()
