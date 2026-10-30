#!/usr/bin/env python3
"""Check production process startup and Signal ownership without an AROS SDK."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    position = brace + 1
    state = "code"
    while position < len(source):
        char = source[position]
        following = source[position:position + 2]
        if state == "comment":
            if following == "*/":
                state = "code"
                position += 2
                continue
        elif state == "line":
            if char == "\n":
                state = "code"
        elif state in ('"', "'"):
            if char == "\\":
                position += 2
                continue
            if char == state:
                state = "code"
        elif following in ("/*", "//"):
            state = "comment" if following == "/*" else "line"
            position += 2
            continue
        elif char in ('"', "'"):
            state = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
        position += 1
    raise ValueError("Unterminated production function: " + signature)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path,
                        help="Production rom/kaffeine directory; default is the enclosing repository")
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="Explicit directory for generated source and test executable")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    test_dir = Path(__file__).resolve().parent
    source_dir = args.source_dir or test_dir.parents[3] / "rom" / "kaffeine"
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    io = (source_dir / "kaffeine_exec_io.c").read_text()
    run = (source_dir / "kaffeine_run.c").read_text()
    names = ["static LONG execute_owned(", "static LONG start_owned(",
             "static void dispose_context(", "static LONG setup_and_execute("]
    if "static void cleanup_owned(" in run:
        names.insert(0, "static void cleanup_owned(")
    async_header = (source_dir / "kaffeine_async.h").read_text()
    production = function(async_header, "static inline ULONG Kaffeine_AsyncSignalMask(") + "\n"
    production += function(io, "IPTR Kaffeine_Signal(") + "\n"
    production += "\n".join(function(run, name) for name in names)
    fixture = (test_dir / "test_integration.cpp").read_text()
    marker = "/* PRODUCTION_FUNCTIONS */"
    if fixture.count(marker) != 1:
        raise ValueError("Expected one production-function insertion marker")
    generated = build_dir / "production_functions.cpp"
    generated.write_text(fixture.replace(marker, production))
    command = shlex.split(os.environ.get("CXX", "clang++"))
    command += ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += [str(generated), "-o", str(build_dir / "test_integration")]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True, timeout=15)
    environment = os.environ.copy()
    if args.sanitize:
        if sys.platform == "darwin":
            environment.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
        else:
            environment.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    result = subprocess.run([str(build_dir / "test_integration")], env=environment, timeout=15)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
