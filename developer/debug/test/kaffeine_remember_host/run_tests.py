#!/usr/bin/env python3
"""Build complete production Remember, heap, message guards and CPU accessors."""
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
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    test_dir = Path(__file__).resolve().parent
    source_dir = (args.source_dir or test_dir.parents[3] / "rom/kaffeine").resolve()
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    shared = test_dir.parent / "kaffeine_host"
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-ffunction-sections", "-fdata-sections", "-I" + str(test_dir / "remember_stubs"),
             "-I" + str(shared / "memory_stubs"), "-I" + str(shared / "stubs"),
             "-I" + str(source_dir)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    commands, objects = [], []
    for name in ("memory", "messages", "cpu", "remember"):
        target = build_dir / (name + ".o")
        commands.append([*shlex.split(os.environ.get("CC", "clang")), "-std=c99", *flags,
                         "-c", str(source_dir / ("kaffeine_" + name + ".c")), "-o", str(target)])
        objects.append(str(target))
    commands.append([*shlex.split(os.environ.get("CXX", "clang++")), "-std=c++20", *flags,
                     str(test_dir / "test_remember.cpp"), *objects,
                     "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                     "-o", str(build_dir / "test_remember")])
    commands.append([str(build_dir / "test_remember")])
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
