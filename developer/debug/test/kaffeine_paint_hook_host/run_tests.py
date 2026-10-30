#!/usr/bin/env python3
"""Run the real bounded native backfill bridge with original synthetic hooks."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--shared-test-dir", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    test = Path(__file__).resolve().parent
    source = (args.source_dir or test.parents[3] / "rom/kaffeine").resolve()
    shared = (args.shared_test_dir or test.parent / "kaffeine_host").resolve()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    includes = [test / "paint_stubs", shared / "io_stubs", shared / "m68klib_stubs", shared / "gfx_stubs", shared / "memory_stubs", shared / "stubs", source]
    # Disposable source snapshots may supply only changed files. Remaining
    # implementation headers come from the same public source tree as the fixture.
    base = shared.parents[3] / "rom/kaffeine"
    includes.append(base)
    flags = ["-g", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-pthread", "-ffunction-sections", "-fdata-sections"]
    flags += ["-I" + str(p) for p in includes]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    commands, objects = [], []
    for name in ["paint_compile", "paint_hook", "memory", "messages", "exec_io", "m68klib"]:
        target = build / (name + ".o")
        commands.append([*shlex.split(os.environ.get("CC", "clang")), "-std=c99", *flags,
                         "-c", str(source / ("kaffeine_" + name + ".c")), "-o", str(target)])
        objects.append(str(target))
    binary = build / "test_paint_hook"
    commands += [[*shlex.split(os.environ.get("CXX", "clang++")), "-std=c++20", *flags,
                  str(test / "test_paint_hook.cpp"), *objects, "-Wl,-dead_strip" if sys.platform=="darwin" else "-Wl,--gc-sections", "-o", str(binary)], [str(binary)]]
    (build / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    output = []
    for command in commands:
        env = dict(os.environ)
        if args.sanitize:
            env.setdefault("ASAN_OPTIONS", ("detect_leaks=0" if sys.platform=="darwin" else "detect_leaks=1")+":halt_on_error=1")
            env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        print(shlex.join(command), flush=True)
        done = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=60)
        print(done.stdout, end="", flush=True)
        output.append(shlex.join(command) + "\n" + done.stdout)
        (build / "run.log").write_text("\n".join(output))
        if done.returncode:
            return done.returncode
    return 0

if __name__ == "__main__":
    sys.exit(main())
