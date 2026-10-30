#!/usr/bin/env python3
"""
Run compiler/emitter tests and the optional pinned Moira oracle.
"""
import argparse
import os
from pathlib import Path
import subprocess


def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-dir", type=Path,
                        help="repository root (discovered from this file when installed)")
    parser.add_argument("--oracle", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    repo = args.source_dir.resolve() if args.source_dir else next(
        (parent for parent in root.parents if (parent / "rom/kaffeine").is_dir()), None)
    code = root if (root / "kaffeine_paint_compile.c").is_file() else (
        repo / "rom/kaffeine" if repo else None)
    if code is None or not (code / "kaffeine_paint_compile.c").is_file():
        parser.error("cannot find compiler source; use --source-dir")
    cc = os.environ.get("CC", "clang")
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(code)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run([cc, "-std=c99", *flags, code / "kaffeine_paint_compile.c",
         root / "test_paint_compile.c", "-o", build / "test-paint"])
    run([build / "test-paint"])
    if not repo:
        parser.error("the native emitter regression requires a repository or --source-dir")
    target = repo / "developer/debug/test/kaffeine_target"
    if not (target / "paint_fixture_native.c").is_file():
        parser.error("--source-dir lacks the original paint fixture source")
    run([cc, "-std=c99", *flags, "-I", target, code / "kaffeine_paint_compile.c",
         root / "test_fixture_emit.c", "-o", build / "test-fixture-emit"])
    run([build / "test-fixture-emit"])
    if args.oracle:
        if not repo:
            parser.error("--oracle requires a discovered repository or --source-dir")
        moira = repo / "rom/kaffeine/Moira/Moira"
        if not (moira / "Moira.cpp").is_file():
            parser.error("--source-dir lacks the pinned Moira source")
        run([cc, "-std=c99", *flags, "-c", code / "kaffeine_paint_compile.c",
             "-o", build / "compiler.o"])
        # The independent oracle reuses only the original program builder.
        # C99 aggregate syntax in that builder is a documented Clang extension
        # in C++20; third-party Moira warning policy is left unchanged.
        cxx_flags = ["-std=c++20", "-O0", "-g", "-Wno-c99-extensions", "-I", str(code)]
        if args.sanitize:
            cxx_flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        run([os.environ.get("CXX", "clang++"), *cxx_flags, "-I", moira,
             root / "test_paint_oracle.cpp", moira / "Moira.cpp",
             moira / "MoiraDebugger.cpp", build / "compiler.o",
             "-o", build / "test-oracle"])
        run([build / "test-oracle"])


if __name__ == "__main__":
    main()
