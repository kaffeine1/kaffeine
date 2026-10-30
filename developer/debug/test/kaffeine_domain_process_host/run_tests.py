#!/usr/bin/env python3
"""Compile complete production domain startup against native collaborators."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", action="store_true",
                        help="only check refused-cleanup ownership on the old source")
    args = parser.parse_args()
    test_dir = Path(__file__).resolve().parent
    source_dir = (args.source_dir or test_dir.parents[3] / "rom" / "kaffeine").resolve()
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    sibling_stubs = test_dir.parent / "kaffeine_domain_run_host" / "stubs"
    if not sibling_stubs.is_dir():
        raise SystemExit("missing sibling kaffeine_domain_run_host native stubs")
    flags = ["-g", "-O1", "-Wall", "-Wextra", "-Werror",
             "-I" + str(test_dir / "stubs"), "-I" + str(sibling_stubs),
             "-I" + str(test_dir), "-I" + str(source_dir),
             "-include", str(test_dir / "fixture_intern.h")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if args.baseline:
        flags += ["-DKAFFEINE_FIXTURE_BASELINE=1"]
    commands = [
        [os.environ.get("CC", "clang"), "-std=c99", *flags,
         "-c", str(source_dir / "kaffeine_domain_process.c"),
         "-o", str(build_dir / "domain_process.o")],
        [os.environ.get("CXX", "clang++"), "-std=c++20", *flags,
         str(test_dir / "test_domain_process.cpp"), str(build_dir / "domain_process.o"),
         "-o", str(build_dir / "test_domain_process")],
        [str(build_dir / "test_domain_process")],
    ]
    logs = []
    (build_dir / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    for command in commands:
        print(shlex.join(command), flush=True)
        env = dict(os.environ)
        if args.sanitize:
            env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1")
            env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        completed = subprocess.run(command, timeout=30, env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(completed.stdout, end="", flush=True)
        logs.append(shlex.join(command) + "\n" + completed.stdout)
        (build_dir / "run.log").write_text("\n".join(logs))
        if completed.returncode:
            raise subprocess.CalledProcessError(completed.returncode, command)


if __name__ == "__main__":
    main()
