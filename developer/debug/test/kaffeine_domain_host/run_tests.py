#!/usr/bin/env python3
"""Build the complete production heap/copier/domain module with host collaborators."""
import argparse
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
    source_dir = args.source_dir or test_dir.parents[3] / "rom/kaffeine"
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-ffunction-sections", "-fdata-sections"]
    shared_tests = test_dir.parent / "kaffeine_host"
    flags += ["-I" + str(test_dir / "domain_stubs")]
    flags += ["-I" + str(shared_tests / name) for name in ("memory_stubs", "stubs")]
    flags += ["-I" + str(source_dir)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    objects = []
    for name in ("memory", "messages", "domain_modules"):
        target = build_dir / (name + ".o")
        command = shlex.split(os.environ.get("CC", "clang"))
        command += ["-std=c99", *flags, "-c", str(source_dir / ("kaffeine_" + name + ".c")), "-o", str(target)]
        print(shlex.join(command), flush=True)
        subprocess.run(command, check=True, timeout=30)
        objects.append(str(target))
    command = shlex.split(os.environ.get("CXX", "clang++"))
    command += ["-std=c++20", *flags, str(test_dir / "test_domain.cpp"), *objects]
    command += ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
    command += ["-o", str(build_dir / "test_domain")]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True, timeout=30)
    environment = os.environ.copy()
    if args.sanitize:
        leak = "0" if sys.platform == "darwin" else "1"
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=" + leak + ":abort_on_error=1")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    subprocess.run([str(build_dir / "test_domain")], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
