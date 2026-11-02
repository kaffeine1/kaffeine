#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Offline delivery/lifecycle tests. No QEMU or AROS application is run."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("preview", HERE / "preview.py")
preview = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preview)


class PreviewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "staged"
        self.root.mkdir()
        self.manifest = {"schema": 1, "build_id": "offline-fixture-001",
                         "target": "aros-aarch64-raspi3b", "source_commit": "a" * 40,
                         "kaffeine_library_sha256": "b" * 64, "files": []}
        for role in sorted(preview.ROLES | {"notice"}):
            path = self.root / (role + ".bin")
            path.write_bytes(bytes(1024 * 1024) if role == "disk" else role.encode())
            self.manifest["files"].append({"role": role, "path": path.name,
                                          "bytes": path.stat().st_size,
                                          "sha256": preview.digest(path)})
        self.save_manifest()

    def save_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def metadata(self):
        session = self.base / "session"
        session.mkdir()
        preview.write_json(session / "identity.json", {"schema": 1, "build_id": "fixture",
                           "backend": "not-observed", "SECRET": "do not include"})
        preview.write_json(session / "result.json", {"stop_reason": "qemu-exited",
                           "qemu_return_code": 0, "elapsed_seconds": 2, "SECRET": "private"})
        (session / "disk.img").write_bytes(b"PRIVATE drawing or document")
        (session / "other.txt").write_text("PRIVATE", encoding="utf-8")
        return session

    def test_verified_build(self):
        m, files = preview.verify(self.root)
        self.assertEqual(m["build_id"], "offline-fixture-001")
        self.assertEqual(set(files), preview.ROLES | {"notice"})

    def test_changed_payload_refused(self):
        (self.root / "kernel.bin").write_bytes(b"changed")
        with self.assertRaises(preview.PreviewError):
            preview.verify(self.root)

    def test_missing_file_refused(self):
        (self.root / "bsp.bin").unlink()
        with self.assertRaises(preview.PreviewError):
            preview.verify(self.root)

    def test_source_and_notice_required(self):
        for role in ("sources", "notice"):
            with self.subTest(role=role):
                original = self.manifest["files"]
                self.manifest["files"] = [e for e in original if e["role"] != role]
                self.save_manifest()
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)
                self.manifest["files"] = original

    def test_duplicate_path_and_role_refused(self):
        for field, value in (("path", "kernel.bin"), ("role", "kernel")):
            with self.subTest(field=field):
                entry = dict(self.manifest["files"][0])
                self.manifest["files"][0][field] = value
                self.save_manifest()
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)
                self.manifest["files"][0] = entry

    def test_invalid_identity_refused(self):
        for field, value in (("schema", 2), ("schema", True), ("target", "pc-x86_64"),
                             ("build_id", "private/path"), ("source_commit", "master"),
                             ("kaffeine_library_sha256", "unverified")):
            with self.subTest(field=field):
                original = self.manifest[field]
                self.manifest[field] = value
                self.save_manifest()
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)
                self.manifest[field] = original

    def test_non_power_of_two_disk_refused(self):
        disk = self.root / "disk.bin"
        disk.write_bytes(disk.read_bytes() + b"x")
        for e in self.manifest["files"]:
            if e["role"] == "disk":
                e.update(bytes=disk.stat().st_size, sha256=preview.digest(disk))
        self.save_manifest()
        with self.assertRaisesRegex(preview.PreviewError, "power-of-two"):
            preview.verify(self.root)

    def test_case_collisions_and_reserved_paths_refused(self):
        for name in ("BSP.bin", "README.md"):
            with self.subTest(name=name):
                path = self.root / name
                path.write_bytes(b"bsp")
                self.manifest["files"].append({"path": name, "role": "notice", "bytes": 3,
                                              "sha256": preview.digest(path)})
                self.save_manifest()
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)
                self.manifest["files"].pop()

    def test_nonportable_paths_refused(self):
        for name in ("../private", "/kernel.bin", "C:/kernel", "a\\b", "a//b",
                     "CON", "nul.txt", "COM1.bin", "LPT9", "file.", "a/../b", ""):
            with self.subTest(name=name), self.assertRaises(preview.PreviewError):
                preview.bundle_file(self.root, name)

    def test_reserved_delivery_filename_cannot_be_a_payload_directory(self):
        kernel = next(e for e in self.manifest["files"] if e["role"] == "kernel")
        for parent in ("README.md", "start.CMD"):
            with self.subTest(parent=parent):
                directory = self.root / parent
                directory.mkdir()
                (directory / "kernel.bin").write_bytes(b"kernel")
                kernel["path"] = parent + "/kernel.bin"
                self.save_manifest()
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)
                out = self.base / "must-not-exist.zip"
                with self.assertRaises(preview.PreviewError):
                    preview.assemble(self.root, out)
                self.assertFalse(out.exists())

    def test_case_insensitive_payload_file_directory_conflict_in_either_order(self):
        # File systems differ: model the two existing files independently of
        # their spelling to test a Linux-built package destined for Windows.
        kernel = next(e for e in self.manifest["files"] if e["role"] == "kernel")
        bsp = next(e for e in self.manifest["files"] if e["role"] == "bsp")
        kernel["path"], bsp["path"] = "payload/kernel.bin", "PAYLOAD"
        actual_file = preview.bundle_file

        def mapped(root, name):
            return actual_file(root, {"payload/kernel.bin": "kernel.bin", "PAYLOAD": "bsp.bin"}.get(name, name))

        for entries in (self.manifest["files"][:], list(reversed(self.manifest["files"]))):
            self.manifest["files"] = entries
            self.save_manifest()
            with patch.object(preview, "bundle_file", side_effect=mapped):
                with self.assertRaises(preview.PreviewError):
                    preview.verify(self.root)

    def make_link(self, target, link, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError):
            self.skipTest("Host cannot create symbolic links")

    def test_payload_links_refused(self):
        self.make_link(self.root / "bsp.bin", self.root / "alias.bin")
        with self.assertRaises(preview.PreviewError):
            preview.bundle_file(self.root, "alias.bin")

    def test_directory_links_refused(self):
        self.make_link(self.root, self.root / "alias", directory=True)
        with self.assertRaises(preview.PreviewError):
            preview.bundle_file(self.root, "alias/bsp.bin")

    def test_assemble_only_allowlisted_files_and_checksums(self):
        (self.root / "PRIVATE.local.txt").write_text("not in delivery", encoding="utf-8")
        out = self.base / "preview.zip"
        preview.assemble(self.root, out)
        with zipfile.ZipFile(out) as archive:
            expected = preview.RESERVED | {e["path"] for e in self.manifest["files"]}
            self.assertEqual(set(archive.namelist()), expected)
            self.assertNotIn("PRIVATE.local.txt", archive.namelist())
            for line in archive.read("SHA256SUMS").decode().splitlines():
                checksum, name = line.split("  ", 1)
                self.assertEqual(preview.hashlib.sha256(archive.read(name)).hexdigest(), checksum)
            extracted = self.base / "extracted"
            archive.extractall(extracted)
        self.assertEqual(preview.verify(extracted)[0], self.manifest)

    def test_assemble_does_not_overwrite(self):
        out = self.base / "existing.zip"
        out.write_bytes(b"retain")
        with self.assertRaises(FileExistsError):
            preview.assemble(self.root, out)
        self.assertEqual(out.read_bytes(), b"retain")

    def test_failed_archive_removed(self):
        out = self.base / "incomplete.zip"
        with self.assertRaises(RuntimeError):
            with preview.new_archive(out) as archive:
                archive.writestr("a", "b")
                raise RuntimeError("controlled failure")
        self.assertFalse(out.exists())

    def test_command_uses_private_disk_two_uarts_and_single_tcg(self):
        _, files = preview.verify(self.root)
        session = self.base / "sessions, with spaces" / "one"
        command = preview.qemu_command("qemu-system-aarch64", files, session)
        self.assertEqual(command[command.index("-accel") + 1], "tcg,thread=single")
        self.assertEqual(command[command.index("-nic") + 1], "none")
        self.assertEqual(command.count("-serial"), 2)
        self.assertIn("file:serial.log", command)
        self.assertIn("file:serial-aux.log", command)
        self.assertIn("file=" + str(session / "disk.img").replace(",", ",,") +
                      ",format=raw,if=sd", command)
        self.assertNotIn(str(files["disk"]), command)
        self.assertFalse(any("virtfs" in item or "hostfwd" in item for item in command))

    def test_real_standin_lifecycle_and_disk_isolation(self):
        # Executes Python, not QEMU. Exercise the actual subprocess/wait/report path.
        session = self.base / "real-process-session"
        script = self.base / "standin.py"
        script.write_text("from pathlib import Path\n"
                          "Path('serial.log').write_bytes(b'bootstrap')\n"
                          "Path('serial-aux.log').write_bytes(b'guest')\n"
                          "with Path('disk.img').open('r+b') as f: f.write(b'EDIT')\n",
                          encoding="utf-8")
        base_hash = preview.digest(self.root / "disk.bin")
        with patch.object(preview, "qemu_command", return_value=[sys.executable, str(script)]):
            rc = preview.run(self.root, session, sys.executable, 10)
        self.assertEqual(rc, 0)
        self.assertEqual(preview.digest(self.root / "disk.bin"), base_hash)
        self.assertNotEqual(preview.digest(session / "disk.img"), base_hash)
        self.assertEqual(preview.read_json(session / "identity.json")["backend"], "not-observed")
        self.assertEqual(preview.read_json(session / "result.json")["stop_reason"], "qemu-exited")
        with self.assertRaises(FileExistsError):
            preview.run(self.root, session, sys.executable, 10)

    def test_timeout_reaps_real_standin_without_app_success(self):
        session = self.base / "timeout-session"
        script = self.base / "wait.py"
        script.write_text("import time\ntime.sleep(30)\n", encoding="utf-8")
        children = []
        popen = subprocess.Popen

        def record(*args, **kwargs):
            proc = popen(*args, **kwargs)
            children.append(proc)
            return proc

        with patch.object(preview, "qemu_command", return_value=[sys.executable, str(script)]), \
                patch.object(preview.subprocess, "Popen", side_effect=record):
            self.assertEqual(preview.run(self.root, session, sys.executable, 0.1), 2)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())
        self.assertEqual(preview.read_json(session / "result.json")["stop_reason"], "session-timeout")

    def test_log_limit_reaps_child(self):
        session = self.base / "large-log-session"
        script = self.base / "noisy.py"
        script.write_text("from pathlib import Path\nimport time\n"
                          "Path('serial-aux.log').write_bytes(b'x' * 100)\ntime.sleep(30)\n",
                          encoding="utf-8")
        with patch.object(preview, "LOG_LIMIT", 20), \
                patch.object(preview, "qemu_command", return_value=[sys.executable, str(script)]):
            self.assertEqual(preview.run(self.root, session, sys.executable, 10), 2)
        self.assertEqual(preview.read_json(session / "result.json")["stop_reason"], "log-limit")

    def test_launch_failure_is_reportable(self):
        session = self.base / "bad-executable-session"
        with patch.object(preview.subprocess, "Popen", side_effect=OSError("test launch error")):
            with self.assertRaises(OSError):
                preview.run(self.root, session, sys.executable, 10)
        out = self.base / "launch-failure.zip"
        preview.report(session, out, "not-tested", True)
        with zipfile.ZipFile(out) as archive:
            info = json.loads(archive.read("report.json"))
            self.assertEqual(info["launcher"]["stop_reason"], "launch-failed")
            self.assertEqual(info["application_result"], "not-tested")

    def test_supervisor_interrupt_and_kill_escalation(self):
        from unittest.mock import Mock
        session = self.metadata()
        proc = Mock()
        proc.poll.return_value = None
        proc.returncode = -9
        proc.wait.side_effect = [subprocess.TimeoutExpired("stand-in", 5), -9]
        with patch.object(preview.time, "sleep", side_effect=KeyboardInterrupt):
            result = preview.supervise(proc, session, 60)
        self.assertEqual(result["stop_reason"], "user-stop")
        proc.terminate.assert_called_once_with()
        proc.kill.assert_called_once_with()
        self.assertEqual(proc.wait.call_count, 2)

    def test_default_report_excludes_logs_disks_and_unknown_metadata(self):
        session = self.metadata()
        (session / "qemu.log").write_bytes(b"private log text")
        out = self.base / "report.zip"
        preview.report(session, out, "not-tested", False)
        with zipfile.ZipFile(out) as archive:
            self.assertEqual(archive.namelist(), ["report.json"])
            text = archive.read("report.json").decode()
            self.assertNotIn("SECRET", text)
            self.assertNotIn("PRIVATE", text)
            info = json.loads(text)
            self.assertEqual(info["application_result"], "not-tested")
            self.assertEqual(info["session"]["backend"], "not-observed")

    def test_log_tails_explicit_bounded_and_both_uarts(self):
        session = self.metadata()
        for name in preview.LOGS:
            (session / name).write_bytes(b"OLD" + b"x" * preview.REPORT_LIMIT + b"LAST")
        out = self.base / "logs.zip"
        preview.report(session, out, "timeout", True)
        with zipfile.ZipFile(out) as archive:
            self.assertEqual(set(archive.namelist()), {"report.json"} | set(preview.LOGS))
            for name in preview.LOGS:
                data = archive.read(name)
                self.assertEqual(len(data), preview.REPORT_LIMIT)
                self.assertTrue(data.endswith(b"LAST"))
                self.assertNotIn(b"OLD", data)

    def test_report_log_link_refused_and_partial_zip_removed(self):
        session = self.metadata()
        self.make_link(session / "other.txt", session / "serial.log")
        out = self.base / "refused.zip"
        with self.assertRaises(preview.PreviewError):
            preview.report(session, out, "failure", True)
        self.assertFalse(out.exists())

    def test_report_does_not_overwrite(self):
        session = self.metadata()
        out = self.base / "retain.zip"
        out.write_bytes(b"retain")
        with self.assertRaises(FileExistsError):
            preview.report(session, out, "success", False)
        self.assertEqual(out.read_bytes(), b"retain")

    def test_cli_rejects_invalid_limits_and_results(self):
        for args in (["run", "--session", "new", "--minutes", "0"],
                     ["run", "--session", "new", "--minutes", "61"],
                     ["report", "session", "out.zip", "--result", "jit-defect"]):
            with self.subTest(args=args), self.assertRaises(SystemExit):
                preview.main(args)

    def test_double_click_start_selects_fresh_session_not_backend(self):
        with patch.object(preview, "run", return_value=0) as run:
            self.assertEqual(preview.main(["start", "--bundle", str(self.root)]), 0)
            first = run.call_args.args
            self.assertEqual(first[0], self.root)
            self.assertEqual(first[1].parent, self.root.resolve() / "sessions")
            self.assertEqual(first[2:], ("qemu-system-aarch64", 900))
            self.assertEqual(preview.main(["start", "--bundle", str(self.root)]), 0)
            self.assertNotEqual(first[1], run.call_args.args[1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
