#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Portable preview packaging, isolated QEMU sessions and opt-in local reports."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

ROLES = {"kernel", "bsp", "dtb", "disk", "sources"}
DELIVERY = ("preview.py", "README.md", "Start.command", "Start.cmd")
RESERVED = {"manifest.json", "SHA256SUMS"} | set(DELIVERY)
LOGS = ("serial.log", "serial-aux.log", "qemu.log")
LOG_LIMIT = 32 * 1024 * 1024
REPORT_LIMIT = 64 * 1024
STATES = {"success", "failure", "unsupported", "timeout", "not-tested"}
IMPORT_BYTES = 256 * 1024 * 1024
IMPORT_FILES = 4096


class PreviewError(Exception):
    pass


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    if path.stat().st_size > 1024 * 1024:
        raise PreviewError("JSON file exceeds 1 MiB")
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, value):
    with path.open("x", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")


@contextmanager
def new_archive(output):
    """Remove only our incomplete output; never overwrite an existing file."""
    with output.open("xb") as handle:
        try:
            with zipfile.ZipFile(handle, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                yield archive
        except BaseException:
            handle.close()
            output.unlink()
            raise


def bundle_file(root, name):
    """Portable relative names; neither links nor Windows device aliases."""
    if not isinstance(name, str) or not name or len(name) > 180:
        raise PreviewError("Invalid package path")
    parts = name.split("/")
    for part in parts:
        stem = part.split(".")[0].upper()
        if (not re.fullmatch(r"[A-Za-z0-9_.-]+", part)
                or part in {".", ".."} or part.endswith((".", " "))
                or stem in {"CON", "PRN", "AUX", "NUL"}
                or re.fullmatch(r"(?:COM|LPT)[0-9]", stem)):
            raise PreviewError("Non-portable package path: " + name)
    path = root
    for part in PurePosixPath(name).parts:
        path = path / part
        if path.is_symlink():
            raise PreviewError("Package links are not allowed: " + name)
    if not path.is_file():
        raise PreviewError("Missing package file: " + name)
    return path


def verify(root):
    root = Path(root).resolve()
    manifest_path = bundle_file(root, "manifest.json")
    m = read_json(manifest_path)
    expected = {"schema", "build_id", "source_commit", "target",
                "kaffeine_library_sha256", "files"}
    if (not isinstance(m, dict) or set(m) != expected
            or type(m["schema"]) is not int or m["schema"] != 1):
        raise PreviewError("Unsupported manifest schema")
    if (m["target"] != "aros-aarch64-raspi3b"
            or not isinstance(m["build_id"], str)
            or not re.fullmatch(r"[a-z0-9][a-z0-9.-]{0,79}", m["build_id"])
            or not isinstance(m["source_commit"], str)
            or not re.fullmatch(r"[0-9a-f]{40}", m["source_commit"])
            or not isinstance(m["kaffeine_library_sha256"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", m["kaffeine_library_sha256"])):
        raise PreviewError("Invalid build identity or unsupported target")
    if not isinstance(m["files"], list) or not 6 <= len(m["files"]) <= 256:
        raise PreviewError("Expected boot files, matching sources and license notices")
    paths, roles, found = {name.casefold() for name in RESERVED}, set(), {}
    for entry in m["files"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "role", "sha256", "bytes"}:
            raise PreviewError("Invalid file entry")
        name, role = entry["path"], entry["role"]
        path = bundle_file(root, name)
        normalized = name.casefold()
        if any(normalized == other or normalized.startswith(other + "/")
               or other.startswith(normalized + "/") for other in paths):
            raise PreviewError("Duplicate or conflicting package name")
        paths.add(normalized)
        if role not in ROLES | {"notice"} or (role != "notice" and role in roles):
            raise PreviewError("Duplicate or unknown package role")
        roles.add(role)
        if (type(entry["bytes"]) is not int or entry["bytes"] <= 0
                or path.stat().st_size != entry["bytes"]
                or not isinstance(entry["sha256"], str)
                or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])
                or digest(path) != entry["sha256"]):
            raise PreviewError("Package checksum/size mismatch: " + name)
        found[role] = path
    if roles != ROLES | {"notice"}:
        raise PreviewError("Package lacks boot files, matching sources or license notices")
    disk_size = found["disk"].stat().st_size
    if disk_size < 1024 * 1024 or disk_size & (disk_size - 1):
        raise PreviewError("The raspi3b raw SD image must have a power-of-two size")
    return m, found


def assemble(root, output):
    """Only manifest-listed payloads and these two delivery files enter the ZIP."""
    root, output = Path(root).resolve(), Path(output).absolute()
    m, _ = verify(root)
    here = Path(__file__).resolve().parent
    files = [("manifest.json", root / "manifest.json")]
    files += [(name, here / name) for name in DELIVERY]
    files += [(e["path"], bundle_file(root, e["path"])) for e in m["files"]]
    sums = "".join(digest(path) + "  " + name + "\n" for name, path in files)
    # Exclusive creation: never replace an existing release or a link to one.
    with new_archive(output) as archive:
        for name, path in files:
            archive.write(path, name)
        archive.writestr("SHA256SUMS", sums)
    print("Created", output.name, "sha256", digest(output))


def qemu_command(qemu, files, session):
    # QEMU option strings escape commas by doubling them; argv is never a shell.
    disk = str(session / "disk.img").replace(",", ",,")
    return [qemu, "-name", "Kaffeine community preview", "-M", "raspi3b",
            "-m", "1G", "-smp", "4", "-accel", "tcg,thread=single",
            "-kernel", str(files["kernel"]), "-initrd", str(files["bsp"]),
            "-dtb", str(files["dtb"]), "-drive", "file=" + disk + ",format=raw,if=sd",
            "-device", "usb-kbd", "-device", "usb-mouse", "-nic", "none",
            "-serial", "file:serial.log", "-serial", "file:serial-aux.log",
            "-monitor", "none", "-no-reboot"]


def supervise(proc, session, seconds):
    """Stop/reap only the QEMU child this launcher created."""
    start, reason = time.monotonic(), "qemu-exited"
    try:
        while proc.poll() is None:
            if time.monotonic() - start >= seconds:
                reason = "session-timeout"
                break
            if any((session / name).exists() and (session / name).stat().st_size > LOG_LIMIT
                   for name in LOGS):
                reason = "log-limit"
                break
            time.sleep(0.2)
    except KeyboardInterrupt:
        reason = "user-stop"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        proc.wait(timeout=5)
    return {"stop_reason": reason, "qemu_return_code": proc.returncode,
            "elapsed_seconds": round(time.monotonic() - start, 3)}


def import_application(source, disk, mcopy):
    """Copy an explicitly selected app into this session, before QEMU opens it."""
    source = Path(source).absolute()
    if source.is_symlink() or not source.is_dir():
        raise PreviewError("Choose a real application directory, without links")
    source = source.resolve()
    if source == disk.parent or source in disk.parents or disk.parent in source.parents:
        raise PreviewError("The application and session directories must be separate")
    executable = shutil.which(mcopy)
    if not executable:
        raise PreviewError("Application import needs mtools; specify mcopy with --mcopy")
    # Keep the mcopy basename: mtools installations may dispatch via symlinks.
    executable = str(Path(executable).absolute())
    with disk.open("rb") as handle:
        header = handle.read(512)
        if (len(header) != 512 or header[510:512] != b"\x55\xaa"
                or header[450] != 0x0c
                or int.from_bytes(header[454:458], "little") != 2048
                or any(header[462:510])):
            raise PreviewError("This application importer requires the preview FAT32 disk layout")
        sectors = int.from_bytes(header[458:462], "little")
        if sectors < 65536 or (2048 + sectors) * 512 > disk.stat().st_size:
            raise PreviewError("Invalid preview partition size")
        handle.seek(1048576)
        boot = handle.read(512)
        if (len(boot) != 512 or boot[510:512] != b"\x55\xaa"
                or boot[11:13] != b"\x00\x02"
                or boot[82:90] != b"FAT32   "):
            raise PreviewError("Invalid preview FAT32 boot sector")
    pending, directories, names, total = [], [], set(), 0
    for folder, dirs, files in os.walk(source, followlinks=False):
        for name in sorted(dirs + files):
            path = Path(folder) / name
            relative = path.relative_to(source)
            if (len(relative.parts) > 16 or len(relative.as_posix()) > 220
                    or path.is_symlink()):
                raise PreviewError("Application import refuses links or excessively deep paths")
            stem = name.split(".")[0].upper()
            if (not re.fullmatch(r"[A-Za-z0-9_. +()-]+", name)
                    or name in {".", ".."} or name.endswith((".", " "))
                    or stem in {"CON", "PRN", "AUX", "NUL"}
                    or re.fullmatch(r"(?:COM|LPT)[0-9]", stem)):
                raise PreviewError("Application contains a filename unsupported by the preview importer")
            key = relative.as_posix().casefold()
            if key in names:
                raise PreviewError("Application filenames collide on the guest filesystem")
            names.add(key)
            if len(names) > 8192:
                raise PreviewError("Application exceeds the 8192-entry import limit")
            if path.is_dir():
                directories.append(relative)
                continue
            if not path.is_file():
                raise PreviewError("Application import accepts only regular files and directories")
            info = path.stat()
            total += info.st_size
            pending.append((path, relative, info))
            if len(pending) > IMPORT_FILES or total > IMPORT_BYTES:
                raise PreviewError("Application exceeds the 4096-file or 256 MiB import limit")
    if not pending:
        raise PreviewError("The selected application directory is empty")
    with tempfile.TemporaryDirectory(prefix=".import-", dir=disk.parent) as temp:
        staged = Path(temp) / "Imported"
        staged.mkdir()
        for relative in directories:
            (staged / relative).mkdir(parents=True, exist_ok=True)
        for path, relative, info in pending:
            target = staged / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            flags = (os.O_RDONLY | getattr(os, "O_BINARY", 0)
                     | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
            with os.fdopen(os.open(path, flags), "rb") as handle, target.open("xb") as out:
                before = os.fstat(handle.fileno())
                if not stat.S_ISREG(before.st_mode) or (
                        before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns) != (
                        info.st_ino, info.st_dev, info.st_size, info.st_mtime_ns):
                    raise PreviewError("Application changed during import; retry with a stable copy")
                remaining = info.st_size
                while remaining:
                    block = handle.read(min(1024 * 1024, remaining))
                    if not block:
                        raise PreviewError("Application changed during import; retry with a stable copy")
                    out.write(block)
                    remaining -= len(block)
                if handle.read(1):
                    raise PreviewError("Application changed during import; retry with a stable copy")
                after = os.fstat(handle.fileno())
                if after.st_size != before.st_size or after.st_mtime_ns != before.st_mtime_ns:
                    raise PreviewError("Application changed during import; retry with a stable copy")
            if target.stat().st_size != info.st_size:
                raise PreviewError("Incomplete application staging copy")
        env = dict(os.environ, MTOOLSRC=os.devnull)
        with (disk.parent / "import.log").open("xb") as log:
            try:
                result = subprocess.run([executable, "-s", "-i", str(disk) + "@@1048576",
                                         str(staged), "::KaffeinePreview/Applications/"],
                                        stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                        env=env, timeout=60, check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                raise PreviewError("Application import could not finish; see the local import.log") from error
        if result.returncode:
            raise PreviewError("Application import failed; see the local import.log")
    return {"files": len(pending), "bytes": total}


def run(root, session, qemu, seconds, import_app=None, mcopy="mcopy"):
    root, session = Path(root).resolve(), Path(session).absolute()
    m, files = verify(root)
    executable = shutil.which(qemu)
    if not executable:
        raise PreviewError("Install QEMU and specify qemu-system-aarch64 with --qemu")
    executable = str(Path(executable).resolve())
    session.mkdir(parents=True, exist_ok=False)
    # Reuse is deliberately explicit at the file level: each run starts fresh.
    shutil.copyfile(files["disk"], session / "disk.img")
    os.chmod(session / "disk.img", 0o600)
    expected_disk = next(e["sha256"] for e in m["files"] if e["role"] == "disk")
    if digest(session / "disk.img") != expected_disk:
        raise PreviewError("Disk copy did not match the verified base image")
    info = {"schema": 1, "build_id": m["build_id"], "source_commit": m["source_commit"],
            "manifest_sha256": digest(root / "manifest.json"),
            "kaffeine_library_sha256": m["kaffeine_library_sha256"],
            "host_system": platform.system(), "host_machine": platform.machine(),
            "host_release": platform.release(),
            "backend": "not-observed", "machine": "raspi3b", "tcg_threads": 1,
            "session_limit_seconds": seconds}
    write_json(session / "identity.json", info)
    if import_app is not None:
        try:
            imported = import_application(import_app, session / "disk.img", mcopy)
        except BaseException:
            write_json(session / "result.json", {"stop_reason": "import-failed"})
            raise
        write_json(session / "import.json", imported)
        print("Application imported into SYS:KaffeinePreview/Applications/Imported.")
    command = qemu_command(executable, files, session)
    print("Close the QEMU window or press Ctrl-C here to end the session.")
    print("Select jit or moira inside the guest Shell; see README.md.")
    with (session / "qemu.log").open("xb") as log:
        try:
            proc = subprocess.Popen(command, cwd=session, stdin=subprocess.DEVNULL,
                                    stdout=log, stderr=subprocess.STDOUT)
        except OSError:
            write_json(session / "result.json", {"stop_reason": "launch-failed"})
            raise
        try:
            result = supervise(proc, session, seconds)
        except BaseException:
            write_json(session / "result.json", {"stop_reason": "launcher-error"})
            raise
    write_json(session / "result.json", result)
    print("Session ended:", result["stop_reason"], "; disk retained in", session)
    # A QEMU exit is not an application result.
    return 0 if result["stop_reason"] in {"qemu-exited", "user-stop"} and proc.returncode == 0 else 2


def report(session, output, state, include_logs):
    """No disk, screenshot, environment, application or arbitrary file collection."""
    session, output = Path(session).resolve(), Path(output).absolute()
    identity = read_json(bundle_file(session, "identity.json"))
    result = read_json(bundle_file(session, "result.json"))
    identity_keys = {"schema", "build_id", "source_commit", "manifest_sha256",
                     "kaffeine_library_sha256", "host_system", "host_machine", "host_release", "backend",
                     "machine", "tcg_threads", "session_limit_seconds"}
    result_keys = {"stop_reason", "qemu_return_code", "elapsed_seconds"}
    # Reconstruct metadata from an allowlist even if a local file was modified.
    info = {"session": {k: identity[k] for k in identity_keys if k in identity},
            "launcher": {k: result[k] for k in result_keys if k in result},
            "application_result": state,
            "backend_evidence": "Fill in the observed backend line/counters in the issue."}
    with new_archive(output) as archive:
        archive.writestr("report.json", json.dumps(info, indent=2) + "\n")
        if include_logs:
            for name in LOGS:
                if not (session / name).exists() and not (session / name).is_symlink():
                    continue
                path = bundle_file(session, name)
                with path.open("rb") as log:
                    log.seek(max(0, path.stat().st_size - REPORT_LIMIT))
                    tail = log.read(REPORT_LIMIT)
                archive.writestr(name, tail)
    print("Local report:", output, "— review its contents before sharing.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("verify", help="verify a staged/extracted preview")
    check.add_argument("bundle", type=Path)
    pack = commands.add_parser("assemble", help="package an audited staging folder")
    pack.add_argument("bundle", type=Path)
    pack.add_argument("output", type=Path)
    launch = commands.add_parser("run", help="boot a fresh, isolated writable disk copy")
    launch.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    launch.add_argument("--session", type=Path, required=True, help="new directory; never reused")
    launch.add_argument("--qemu", default="qemu-system-aarch64")
    launch.add_argument("--minutes", type=int, choices=range(1, 61), default=15)
    launch.add_argument("--import-app", type=Path, help="selected app directory; needs mtools")
    launch.add_argument("--mcopy", default="mcopy", help="mtools mcopy executable")
    start = commands.add_parser("start", help="start a fresh session beside this extracted kit")
    start.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    start.add_argument("--qemu", default="qemu-system-aarch64")
    start.add_argument("--import-app", type=Path, help="selected app directory; needs mtools")
    start.add_argument("--mcopy", default="mcopy", help="mtools mcopy executable")
    collect = commands.add_parser("report", help="create a local, reviewable report ZIP")
    collect.add_argument("session", type=Path)
    collect.add_argument("output", type=Path)
    collect.add_argument("--result", choices=sorted(STATES), default="not-tested")
    collect.add_argument("--include-logs", action="store_true",
                         help="include at most 64 KiB from each log; may contain private data")
    args = parser.parse_args(argv)
    try:
        if args.command == "verify":
            m, _ = verify(args.bundle)
            print("Verified package:", m["build_id"], "(integrity, not runtime acceptance)")
        elif args.command == "assemble":
            assemble(args.bundle, args.output)
        elif args.command == "run":
            return run(args.bundle, args.session, args.qemu, args.minutes * 60,
                       args.import_app, args.mcopy)
        elif args.command == "start":
            name = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
            return run(args.bundle, args.bundle.resolve() / "sessions" / name, args.qemu, 900,
                       args.import_app, args.mcopy)
        else:
            report(args.session, args.output, args.result, args.include_logs)
    except (PreviewError, OSError, ValueError, KeyError, TypeError) as error:
        print("Preview error:", error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
