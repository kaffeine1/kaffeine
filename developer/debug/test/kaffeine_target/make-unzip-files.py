#!/usr/bin/env python3
"""Build the deterministic Zip test archives for the UnZip 6.0 steps.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)

Writes three archives to the directory given on the command line:
  - tree.zip: five members with fixed timestamps and fixed Unix permissions
    covering an empty file, a stored file, a deflated file, a member with a
    comment, an archive comment, subdirectories, and a deep nested path.
    Subdirectories are implicit in the member paths rather than separate
    directory entries ending in '/', so checkdir() creates them with mkdir()
    on the first extraction and stat()s them on the second extraction over
    the same tree without changing stdout between the first and second runs.
  - bad_trunc.zip: tree.zip with the last 60 bytes removed so the
    end-of-central-directory record is missing (exit code 9, PK_NOZIP).
  - bad_crc.zip: a two-member archive whose second member carries a
    deliberately corrupted CRC-32 in its local and central headers (exit
    code 2, PK_ERR).
"""

from pathlib import Path
import sys
import zipfile


def add_member(zf, name, data, date_time, mode, compress_type, comment=b""):
    info = zipfile.ZipInfo(filename=name, date_time=date_time)
    info.compress_type = compress_type
    info.create_system = 3  # UNIX_
    info.create_version = 30
    info.extract_version = 20
    info.external_attr = (mode & 0xFFFF) << 16
    info.comment = comment
    if compress_type == zipfile.ZIP_DEFLATED:
        zf.writestr(info, data, compress_type=compress_type, compresslevel=6)
    else:
        zf.writestr(info, data, compress_type=compress_type)


def build_tree_zip(path):
    with zipfile.ZipFile(path, "w") as zf:
        zf.comment = b"Info-ZIP UnZip 6.0 test archive for kaffeine"
        add_member(
            zf,
            "empty.txt",
            b"",
            (2020, 1, 2, 3, 4, 6),
            0o100644,
            zipfile.ZIP_STORED,
        )
        add_member(
            zf,
            "sub/stored.txt",
            b"Stored member inside sub/ with mode 0644 and fixed timestamp.\n",
            (2021, 5, 14, 9, 30, 0),
            0o100644,
            zipfile.ZIP_STORED,
        )
        add_member(
            zf,
            "sub/nested/deflated.txt",
            (
                b"Deflated member inside sub/nested/ with mode 0755.\n"
                b"Repeated lines make deflate compress this payload cleanly.\n"
                * 8
            ),
            (2022, 8, 20, 14, 15, 20),
            0o100755,
            zipfile.ZIP_DEFLATED,
        )
        add_member(
            zf,
            "sub/commented.txt",
            (
                b"Deflated member carrying a central-directory file comment\n"
                b"and owner-only rw------- (0600) permissions.\n"
                * 4
            ),
            (2023, 11, 3, 18, 45, 10),
            0o100600,
            zipfile.ZIP_DEFLATED,
            comment=b"Member comment on sub/commented.txt",
        )
        add_member(
            zf,
            "deep/path_alpha/path_beta/path_gamma/path_delta/"
            "long_leaf_filename_for_dos_locks.txt",
            (
                b"Deep nested member exercising multi-level CreateDir, Lock,\n"
                b"Examine, SetFileDate and SetProtection on long paths.\n"
                * 6
            ),
            (2024, 2, 29, 12, 0, 0),
            0o100700,
            zipfile.ZIP_DEFLATED,
        )


def build_bad_trunc_zip(tree_path, out_path):
    raw = tree_path.read_bytes()
    out_path.write_bytes(raw[:-60])


def build_bad_crc_zip(path):
    with zipfile.ZipFile(path, "w") as zf:
        add_member(
            zf,
            "ok.txt",
            b"Valid first member.\n",
            (2022, 8, 20, 14, 15, 20),
            0o100644,
            zipfile.ZIP_STORED,
        )
        add_member(
            zf,
            "corrupt.txt",
            b"Deflated second member whose CRC will fail.\n" * 3,
            (2022, 8, 20, 14, 15, 20),
            0o100644,
            zipfile.ZIP_DEFLATED,
        )
    with zipfile.ZipFile(path, "r") as zf:
        good_crc = zf.getinfo("corrupt.txt").CRC
    good_bytes = good_crc.to_bytes(4, "little")
    bad_bytes = (good_crc ^ 0xDEADBEEF).to_bytes(4, "little")
    raw = path.read_bytes()
    if raw.count(good_bytes) != 2:
        raise RuntimeError("expected corrupt.txt CRC in local and central headers")
    path.write_bytes(raw.replace(good_bytes, bad_bytes))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-unzip-files.py OUT_DIR")
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    tree_path = out_dir / "tree.zip"
    build_tree_zip(tree_path)
    build_bad_trunc_zip(tree_path, out_dir / "bad_trunc.zip")
    build_bad_crc_zip(out_dir / "bad_crc.zip")


if __name__ == "__main__":
    main()
