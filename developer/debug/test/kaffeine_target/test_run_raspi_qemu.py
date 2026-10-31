# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Offline tests of result verdicts, the Run completion protocol, the
keys of --keys and the function names in calls.txt."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    'run_raspi_qemu', Path(__file__).with_name('run-raspi-qemu.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

PASS_OUTPUT = 'ok   arithmetic\nsample: 1 checks, 0 failed\n'


class ResultTests(unittest.TestCase):
    def assess(self, output=PASS_OUTPUT, rc='rc 0\n', completed=True):
        return runner.parse_result('sample', 'run', output, rc, completed)

    def test_passing_checks(self):
        result = self.assess()
        self.assertEqual(result['status'], 'pass')
        self.assertEqual(result['return_code'], 0)
        self.assertEqual(result['check_summaries'][0]['checks'], 1)

    def test_hello_needs_no_check_summary(self):
        self.assertEqual(self.assess('Hello from 68k\n')['status'], 'pass')

    def test_nonzero_return_codes_fail(self):
        for rc in (5, 10, 20, -1):
            with self.subTest(rc=rc):
                result = self.assess(rc='rc %d\n' % rc)
                self.assertEqual(result['status'], 'fail')
                self.assertEqual(result['return_code'], rc)

    def test_fail_line_overrides_zero_rc(self):
        result = self.assess('FAIL division\nsample: 1 checks, 0 failed\n')
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['failed_lines'], ['FAIL division'])

    def test_failed_summary_overrides_zero_rc(self):
        result = self.assess('sample: 25 checks, 5 failed\n')
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['check_summaries'][0]['failed'], 5)

    def test_missing_or_invalid_rc_is_incomplete(self):
        for rc in (None, '', 'rc $RC\n', 'rc 0\nrc 10\n'):
            with self.subTest(rc=rc):
                self.assertEqual(self.assess(rc=rc)['status'], 'incomplete')

    def test_missing_or_empty_output_is_incomplete(self):
        for output in (None, '', '\n  \n'):
            with self.subTest(output=output):
                self.assertEqual(self.assess(output=output)['status'], 'incomplete')

    def test_partial_run_cannot_pass_on_output_and_rc_alone(self):
        self.assertEqual(self.assess(completed=False)['status'], 'incomplete')

    def test_alias_and_crlf_output(self):
        result = runner.parse_result('sample_alias', 'shell',
                                     PASS_OUTPUT.replace('\n', '\r\n'), 'rc 0\r\n')
        self.assertEqual(result['status'], 'pass')

    def test_word_starting_with_fail_is_not_a_failed_check(self):
        self.assertEqual(self.assess('FAILSAFE mode is enabled\n')['status'], 'pass')


class ExpectedOutputTests(unittest.TestCase):
    """Byte for byte outputs, such as those of the bzip2 self-test."""
    DATA = bytes(range(256)) * 3 + b'BZh9\r\n\rFAIL x\nsample: 1 checks, 1 failed\n'

    def assess(self, data, rc='rc 0\n'):
        output = data.decode('latin-1') if data is not None else None
        return runner.parse_result('bz', 'shell', output, rc, True, self.DATA)

    def test_identical_bytes_pass_and_their_lines_are_no_checks(self):
        result = self.assess(self.DATA)
        self.assertEqual(result['status'], 'pass')
        self.assertTrue(result['output_matches'])
        self.assertEqual(result['failed_lines'], [])
        self.assertEqual(result['check_summaries'], [])
        self.assertEqual(result['last_line'], 'output matches the expected %d bytes' % len(self.DATA))

    def test_a_different_byte_fails_at_its_offset(self):
        data = bytearray(self.DATA)
        data[300] ^= 1
        result = self.assess(bytes(data))
        self.assertEqual(result['status'], 'fail')
        self.assertFalse(result['output_matches'])
        self.assertIn('at byte 300 of %d' % len(data), result['failures'][0])

    def test_shorter_and_longer_outputs_fail(self):
        for data, offset in ((self.DATA[:-1], len(self.DATA) - 1), (self.DATA + b'\n', len(self.DATA))):
            with self.subTest(size=len(data)):
                result = self.assess(data)
                self.assertEqual(result['status'], 'fail')
                self.assertIn('at byte %d' % offset, result['failures'][0])

    def test_return_code_and_missing_output_still_count(self):
        self.assertEqual(self.assess(self.DATA, 'rc 3\n')['status'], 'fail')
        result = self.assess(None)
        self.assertEqual(result['status'], 'incomplete')
        self.assertIsNone(result['output_matches'])

    def test_compared_output_is_reported_by_size_only(self):
        text = self.DATA.decode('latin-1')
        report = runner.guest_file_report('bz-shell.txt', text, {'bz-shell.txt', 'bz-run.txt'})
        self.assertEqual(report, '== bz-shell.txt: %d bytes, compared with the expected file'
                         % len(self.DATA))
        self.assertTrue(all(32 <= ord(c) < 127 for c in report))
        plain = runner.guest_file_report('c_stdio-shell.txt', 'ok   argc\n', {'bz-shell.txt'})
        self.assertEqual(plain, '== c_stdio-shell.txt\nok   argc')

    def test_stdout_without_latin1_escapes_guest_text(self):
        argv = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools']
        with tempfile.TemporaryDirectory() as directory:
            raw = io.BytesIO()
            ascii_out = io.TextIOWrapper(raw, encoding='ascii')
            report = {'directory': directory, 'passed': True,
                      'vm': {'completed': True, 'stop_reason': 'completed'},
                      'results': [{'program': 'sample', 'mode': 'shell', 'status': 'pass',
                                   'return_code': 0, 'errors': [], 'failures': [],
                                   'last_line': 'caf\xe9 \x1b[2J'}]}
            with mock.patch.object(runner, 'boot', return_value=report), \
                    mock.patch.object(runner.sys, 'argv', argv + ['--run-dir', directory, 'sample']), \
                    mock.patch.object(runner.sys, 'stdout', ascii_out):
                self.assertEqual(runner.main(), 0)
            ascii_out.flush()
            self.assertIn(b'caf\\xe9', raw.getvalue())

    def test_saved_guest_file_keeps_every_byte(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bz-shell.txt'
            runner.save_text(path, self.DATA.decode('latin-1'))
            self.assertEqual(runner.load_text(path).encode('latin-1'), self.DATA)
            self.assertIsNone(runner.load_text(Path(directory) / 'missing.txt'))

    def test_assess_boot_applies_the_expectation_to_both_executions(self):
        files = {'done.txt': 'done\n', 'bz-run-done.txt': 'done\n',
                 'bz-shell.txt': self.DATA.decode('latin-1'), 'bz-shell-rc.txt': 'rc 0\n',
                 'bz-run.txt': self.DATA[:-2].decode('latin-1'), 'bz-run-rc.txt': 'rc 0\n'}
        report = runner.assess_boot(['bz'], files.get, {'completed': True, 'stop_reason': 'completed'},
                                    {'bz': self.DATA})
        self.assertEqual([r['status'] for r in report['results']], ['pass', 'fail'])
        self.assertFalse(report['passed'])


class ExpectedReturnCodeTests(unittest.TestCase):
    """--expect-rc: deliberate nonzero exits pass only on their exact code."""

    def assess(self, output=PASS_OUTPUT, rc='rc 20\n', expected=None, expected_rc=20):
        return runner.parse_result('cpu_faults', 'shell', output, rc, True, expected, expected_rc)

    def test_expected_return_code_passes_with_plain_checks_and_expected_bytes(self):
        for code in (1, 2, 7, 20):
            with self.subTest(code=code):
                result = self.assess(rc='rc %d\n' % code, expected_rc=code)
                self.assertEqual(result['status'], 'pass')
                self.assertEqual(result['return_code'], code)
                self.assertEqual(result['failures'], [])
        data = b'bad palette warning\n'
        result = self.assess(data.decode('latin-1'), 'rc 1\n', expected=data, expected_rc=1)
        self.assertEqual(result['status'], 'pass')
        self.assertTrue(result['output_matches'])
        self.assertEqual(result['failures'], [])

    def test_zero_or_another_return_code_fails_and_names_the_expected_code(self):
        for got, want in ((0, 20), (1, 20), (20, 1), (0, 7), (5, 2)):
            with self.subTest(got=got, want=want):
                result = self.assess(rc='rc %d\n' % got, expected_rc=want)
                self.assertEqual(result['status'], 'fail')
                self.assertEqual(result['return_code'], got)
                self.assertEqual(result['failures'],
                                 ['return code %d (expected rc %d)' % (got, want)])

    def test_matching_return_code_still_fails_when_output_fails(self):
        result = self.assess('FAIL bounds\ncpu_faults: 4 checks, 0 failed\n', 'rc 20\n')
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['failures'], ['1 FAIL line(s)'])
        result = self.assess('cpu_faults: 4 checks, 1 failed\n', 'rc 20\n')
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['failures'], ['test summary reports failed checks'])
        result = self.assess('wrong\n', 'rc 1\n', expected=b'expected\n', expected_rc=1)
        self.assertEqual(result['status'], 'fail')
        self.assertFalse(result['output_matches'])
        self.assertIn('output differs from the expected', result['failures'][0])

    def test_default_zero_return_code_behaviour_is_unchanged(self):
        for expected_rc in (0, None):
            with self.subTest(expected_rc=expected_rc):
                self.assertEqual(self.assess(rc='rc 0\n', expected_rc=expected_rc)['status'],
                                 'pass')
                result = self.assess(rc='rc 20\n', expected_rc=expected_rc)
                self.assertEqual(result['status'], 'fail')
                self.assertEqual(result['failures'], ['return code 20'])

    def test_assess_boot_and_summary_report_expected_rc(self):
        files = {'done.txt': 'done\n', 'probe-run-done.txt': 'done\n',
                 'ok-run-done.txt': 'done\n',
                 'probe-shell.txt': PASS_OUTPUT, 'probe-shell-rc.txt': 'rc 20\n',
                 'probe-run.txt': PASS_OUTPUT, 'probe-run-rc.txt': 'rc 0\n',
                 'ok-shell.txt': PASS_OUTPUT, 'ok-shell-rc.txt': 'rc 0\n',
                 'ok-run.txt': PASS_OUTPUT, 'ok-run-rc.txt': 'rc 0\n'}
        vm = {'completed': True, 'stop_reason': 'completed'}
        report = runner.assess_boot(['probe', 'ok'], files.get, vm, expected_rc={'probe': 20})
        self.assertFalse(report['passed'])
        self.assertEqual([r['status'] for r in report['results']],
                         ['pass', 'fail', 'pass', 'pass'])
        self.assertIn('expected rc 20', report['results'][1]['failures'][0])
        with tempfile.TemporaryDirectory() as directory:
            report['directory'] = directory
            argv = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                    '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                    '--run-dir', directory, '--expect-rc', 'probe', '20', 'probe', 'ok']
            out = io.StringIO()
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(out):
                self.assertEqual(runner.main(), 1)
            self.assertEqual(boot.call_args.args[0].expected_rc, {'probe': 20})
            self.assertIn('expected rc 20', out.getvalue())
        files['probe-run-rc.txt'] = 'rc 20\n'
        self.assertTrue(runner.assess_boot(['probe', 'ok'], files.get, vm,
                                           expected_rc={'probe': 20})['passed'])


class CompletionTests(unittest.TestCase):
    def test_startup_done_does_not_complete_background_run(self):
        files = {'done.txt': 'done\n', 'sample-run.txt': PASS_OUTPUT,
                 'sample-run-rc.txt': 'rc 0\n'}
        self.assertFalse(runner.guest_completed(files.get, ['sample']))
        files['sample-run-done.txt'] = 'done\n'
        self.assertTrue(runner.guest_completed(files.get, ['sample']))

    def test_run_done_does_not_replace_startup_done(self):
        files = {'sample-run-done.txt': 'done\n'}
        self.assertFalse(runner.guest_completed(files.get, ['sample']))

    def test_every_run_needs_its_own_marker(self):
        files = {'done.txt': 'done\n', 'first-run-done.txt': 'done\n'}
        self.assertFalse(runner.guest_completed(files.get, ['first', 'second']))
        files['second-run-done.txt'] = ''  # Echo has not finished writing it.
        self.assertFalse(runner.guest_completed(files.get, ['first', 'second']))
        files['second-run-done.txt'] = 'done\n'
        self.assertTrue(runner.guest_completed(files.get, ['first', 'second']))

    def test_run_script_captures_rc_before_copying_output(self):
        script = runner.run_script('sample', {'sample': 'one "two words"'}, {'sample': 'in'})
        lines = script.splitlines()
        command = ('SYS:EMU68/sample >RAM:sample-run.txt *>RAM:sample-run-err.txt '
                   '<SYS:EMU68/sample.in one "two words"')
        position = lines.index(command)
        self.assertEqual(lines[position + 1], 'Echo "rc $RC" >SYS:EMU68/sample-run-rc.txt')
        self.assertEqual(lines[position + 2], 'FailAt 1')
        self.assertEqual(lines[position + 3], 'Copy RAM:sample-run.txt SYS:EMU68/sample-run.txt')
        self.assertEqual(lines[position + 4],
                         'Copy RAM:sample-run-err.txt SYS:EMU68/sample-run-err.txt')
        self.assertEqual(lines[-1], 'Echo "done" >SYS:EMU68/sample-run-done.txt')

    def test_cpu_backend_is_selected_before_both_launch_modes(self):
        shell = runner.user_startup(['sample'], {}, {}, 15, cpu='jit')
        child = runner.run_script('sample', {}, {}, cpu='jit')
        for script in (shell, child):
            self.assertLess(script.index('Set KAFFEINE_CPU jit\n'),
                            script.index('SYS:EMU68/sample >'))
        self.assertNotIn('Set KAFFEINE_CPU', runner.run_script('sample', {}, {}))

    def test_shell_execution_saves_the_error_stream(self):
        script = runner.user_startup(['sample'], {'sample': 'one'}, {'sample': 'in'}, 15)
        self.assertIn('SYS:EMU68/sample >SYS:EMU68/sample-shell.txt '
                      '*>SYS:EMU68/sample-shell-err.txt <SYS:EMU68/sample.in one\n', script)

    def test_startup_waits_for_the_run_execution_of_input_programs(self):
        """Wanderer starts after S:User-Startup and takes the active window:
        a program that takes keys or the mouse keeps it until its Run
        execution has ended."""
        script = runner.user_startup(['typed', 'plain'], {}, {}, 15, {'typed'})
        lines = script.splitlines()
        loop = ['Lab wait-typed', 'If NOT EXISTS SYS:EMU68/typed-run-done.txt',
                '    Wait 1 SECS', '    Skip wait-typed BACK', 'EndIf']
        start = lines.index('Run >NIL: Execute SYS:EMU68/typed-run.script')
        self.assertEqual(lines[start + 1:start + 2 + len(loop)], ['Wait 15 SECS'] + loop)
        self.assertLess(start + 1 + len(loop), lines.index('SYS:EMU68/plain >SYS:EMU68/plain-shell.txt '
                                                           '*>SYS:EMU68/plain-shell-err.txt'))
        self.assertNotIn('wait-plain', script)
        self.assertEqual(runner.user_startup(['plain'], {}, {}, 15),
                         runner.user_startup(['plain'], {}, {}, 15, {'other'}))
        args = mock.Mock(arguments={}, inputs={}, run_wait=15, keys={'typed': 'a'},
                         mouse={'moved': 'click'}, wanderer=False)
        script = runner.startup_script(args, ['typed', 'moved', 'plain'])
        self.assertIn('Skip wait-typed BACK', script)
        self.assertIn('Skip wait-moved BACK', script)
        self.assertNotIn('wait-plain', script)
        self.assertNotIn('Wanderer', script)

    def test_startup_starts_wanderer_first_when_asked(self):
        """--wanderer: Wanderer runs before the first program, after a pause
        for its desktop."""
        lines = runner.user_startup(['sample'], {}, {}, 15, (), True).splitlines()
        start = lines.index('Run >NIL: <NIL: WANDERER:Wanderer')
        self.assertEqual(lines[start + 1], 'Wait %d SECS' % runner.WANDERER_WAIT)
        self.assertLess(start, lines.index('SYS:EMU68/sample >SYS:EMU68/sample-shell.txt '
                                           '*>SYS:EMU68/sample-shell-err.txt'))
        args = mock.Mock(arguments={}, inputs={}, run_wait=15, keys={}, mouse={}, wanderer=True)
        self.assertIn('Run >NIL: <NIL: WANDERER:Wanderer\n', runner.startup_script(args, ['sample']))

    def test_startup_launches_wrapper_instead_of_copying_partial_output(self):
        script = runner.user_startup(['sample'], {}, {}, 15)
        self.assertIn('Run >NIL: Execute SYS:EMU68/sample-run.script\n', script)
        self.assertIn('Wait 15 SECS\n', script)
        self.assertNotIn('Copy RAM:sample-run.txt', script)


class BootVerdictTests(unittest.TestCase):
    def setUp(self):
        self.files = {'done.txt': 'done\n', 'sample-run-done.txt': 'done\n',
                      'sample-shell.txt': PASS_OUTPUT, 'sample-run.txt': PASS_OUTPUT,
                      'sample-shell-rc.txt': 'rc 0\n', 'sample-run-rc.txt': 'rc 0\n'}

    def assess(self, completed=True, stop_reason='completed'):
        return runner.assess_boot(['sample'], self.files.get,
                                  {'completed': completed, 'stop_reason': stop_reason})

    def test_success_requires_both_executions(self):
        report = self.assess()
        self.assertTrue(report['passed'])
        self.assertEqual([result['mode'] for result in report['results']], ['shell', 'run'])

    def test_finished_vm_with_failed_program_does_not_pass(self):
        self.files['sample-shell-rc.txt'] = 'rc 10\n'
        report = self.assess()
        self.assertTrue(report['vm']['completed'])
        self.assertFalse(report['passed'])
        self.assertEqual(report['results'][0]['status'], 'fail')

    def test_run_failure_does_not_hide_behind_passing_shell(self):
        self.files['sample-run-rc.txt'] = 'rc 20\n'
        self.assertFalse(self.assess()['passed'])

    def test_marker_without_result_files_does_not_pass(self):
        for name in ('sample-run-rc.txt', 'sample-run.txt', 'sample-shell-rc.txt'):
            with self.subTest(name=name):
                value = self.files.pop(name)
                self.assertFalse(self.assess()['passed'])
                self.files[name] = value

    def test_timeout_does_not_pass_even_with_successful_output(self):
        for reason in ('timeout', 'idle_timeout', 'qemu_exited'):
            with self.subTest(reason=reason):
                self.assertFalse(self.assess(False, reason)['passed'])

    def test_cli_exit_and_json_follow_program_verdicts(self):
        for rc, exit_code in (('rc 0\n', 0), ('rc 10\n', 1), (None, 1)):
            with self.subTest(rc=rc), tempfile.TemporaryDirectory() as directory:
                self.files['sample-run-rc.txt'] = rc
                report = self.assess()
                report['directory'] = directory
                argv = ['run-raspi-qemu.py', '--image', 'unused.img',
                        '--kernel', 'unused.kernel', '--bsp', 'unused.bsp',
                        '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                        '--run-dir', directory, 'sample']
                with mock.patch.object(runner, 'boot', return_value=report), \
                        mock.patch.object(runner.sys, 'argv', argv), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(runner.main(), exit_code)
                summary = json.loads((Path(directory) / 'summary.json').read_text())
                self.assertEqual(summary['passed'], exit_code == 0)
                self.assertEqual(summary['boots'][0]['vm']['completed'], True)

    def test_cli_reads_expected_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / 'sample1.bz2'
            reference.write_bytes(b'BZh1\r\n')
            report = self.assess()
            report['directory'] = directory
            argv = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                    '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                    '--run-dir', directory, '--expect', 'sample', str(reference),
                    'sample']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                runner.main()
            self.assertEqual(boot.call_args.args[0].expected, {'sample': b'BZh1\r\n'})
            argv[argv.index(str(reference))] = str(Path(directory) / 'missing.bz2')
            with mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                runner.main()

    def test_cli_rejects_expectations_without_a_program(self):
        """A misspelled name, another case or the file of NAME=FILE would
        leave the output unchecked, so they are usage errors."""
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / 'sample1.bz2'
            reference.write_bytes(b'BZh1')
            report = self.assess()
            report['directory'] = directory
            base = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                    '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                    '--run-dir', directory]
            for name in ('sampel', 'Sample', 'build/bzip2'):
                with self.subTest(name=name):
                    argv = base + ['--expect', name, str(reference), 'sample=build/bzip2']
                    with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                            mock.patch.object(runner.sys, 'argv', argv), \
                            contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                        runner.main()
                    boot.assert_not_called()
            argv = base + ['--expect', 'sample', str(reference), 'sample=build/bzip2']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                runner.main()
            self.assertEqual(boot.call_args.args[0].expected, {'sample': b'BZh1'})

    def test_cli_validates_expect_rc(self):
        with tempfile.TemporaryDirectory() as directory:
            report = self.assess()
            report['directory'] = directory
            base = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                    '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                    '--run-dir', directory]
            for extra in (['--expect-rc', 'sampel', '20', 'sample=build/cpu_faults'],
                          ['--expect-rc', 'Sample', '20', 'sample=build/cpu_faults'],
                          ['--expect-rc', 'build/cpu_faults', '20', 'sample=build/cpu_faults'],
                          ['--expect-rc', 'sample', 'twenty', 'sample'],
                          ['--expect-rc', 'sample', '2\u00b2', 'sample'],
                          ['--expect-rc', 'sample', '', 'sample'],
                          ['--expect-rc', 'sample', '1', '--expect-rc', 'sample', '2', 'sample']):
                with self.subTest(extra=extra):
                    argv = base + extra
                    with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                            mock.patch.object(runner.sys, 'argv', argv), \
                            contextlib.redirect_stderr(io.StringIO()), \
                            self.assertRaises(SystemExit):
                        runner.main()
                    boot.assert_not_called()
            argv = base + ['--expect-rc', 'first', '20', '--expect-rc', 'second', '7',
                           'first=build/cpu_faults', 'second=build/dos_strings', 'third']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                runner.main()
            self.assertEqual(boot.call_args.args[0].expected_rc, {'first': 20, 'second': 7})


class QemuSchedulingTests(unittest.TestCase):
    """Check the real CLI-to-Popen path without running QEMU or mtools."""

    def test_default_and_explicit_tcg_threads_reach_qemu(self):
        for options, selected in (([], 'single'), (['--tcg-thread', 'single'], 'single'),
                                  (['--tcg-thread', 'multi'], 'multi')):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)
                report = {'directory': directory, 'passed': True, 'results': [],
                          'vm': {'completed': True, 'stop_reason': 'completed'}}
                argv = ['run-raspi-qemu.py', '--image', 'unused.img',
                        '--kernel', 'unused.kernel', '--bsp', 'unused.bsp',
                        '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                        '--run-dir', directory] + options + ['sample']
                with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                        mock.patch.object(runner.sys, 'argv', argv), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(runner.main(), 0)
                args = boot.call_args.args[0]
                args.disk = run / 'unused-copy.img'
                proc = mock.Mock(returncode=0)
                proc.poll.return_value = 0
                with mock.patch.object(runner, 'mtool',
                                       return_value=mock.Mock(stdout=b'', returncode=0)), \
                        mock.patch.object(runner.subprocess, 'Popen', return_value=proc) as popen, \
                        mock.patch.object(runner, 'SOCK', run / 'unused.sock'), \
                        contextlib.redirect_stdout(io.StringIO()):
                    vm = runner.run_guest(args, run, [], [])
                popen.assert_called_once()
                cmd = popen.call_args.args[0]
                self.assertEqual(cmd[cmd.index('-accel') + 1], 'tcg,thread=' + selected)
                self.assertEqual(cmd[cmd.index('-smp') + 1], '4')
                self.assertEqual(cmd[cmd.index('-M') + 1], 'raspi3b')
                self.assertEqual(cmd[cmd.index('-display') + 1], 'none')
                self.assertIn('-accel tcg,thread=' + selected,
                              (run / 'qemu-cmd.txt').read_text())
                self.assertEqual(vm['stop_reason'], 'qemu_exited')
                self.assertFalse(vm['completed'])

    def test_invalid_tcg_thread_is_rejected_before_boot(self):
        argv = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                '--run-dir', 'unused-results', '--tcg-thread', 'auto', 'sample']
        with mock.patch.object(runner, 'boot') as boot, \
                mock.patch.object(runner.sys, 'argv', argv), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            runner.main()
        self.assertEqual(error.exception.code, 2)
        boot.assert_not_called()


class KeyTests(unittest.TestCase):
    """--keys: QMP replies past events, errors of send-key, and markers
    served once, only while their execution runs."""

    def test_reply_skips_events(self):
        f = io.StringIO('{"event": "RTC_CHANGE", "data": {}}\n{"return": {}}\n')
        self.assertEqual(runner.qmp_reply(f), {'return': {}})
        f = io.StringIO('{"event": "X"}\n{"error": {"class": "GenericError", "desc": "no"}}\n')
        self.assertEqual(runner.qmp_reply(f)['error']['desc'], 'no')

    def test_reply_closed_or_garbled_is_oserror(self):
        for text in ('', '{"event": "X"}\n', 'not json\n'):
            with self.subTest(text=text), self.assertRaises(OSError):
                runner.qmp_reply(io.StringIO(text))

    def test_send_keys_reports_the_first_error(self):
        sent = []

        def command(cmd, arguments):
            keys = [key['data'] for key in arguments['keys']]
            sent.append(keys)
            if keys == ['bogus']:
                return {'error': {'class': 'GenericError', 'desc': "Parameter 'keys' is invalid"}}
            return {'return': {}}
        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep'):
            self.assertEqual(runner.send_keys('a shift+b ret'), [])
            self.assertEqual(sent, [['a'], ['shift', 'b'], ['ret']])
            sent.clear()
            errors = runner.send_keys('a bogus c')
        self.assertEqual(sent, [['a'], ['bogus']])
        self.assertEqual(len(errors), 1)
        self.assertIn('bogus', errors[0])

    def test_send_keys_survives_a_lost_connection(self):
        with mock.patch.object(runner, 'qmp_command', side_effect=OSError('broken pipe')), \
                mock.patch.object(runner.time, 'sleep'):
            errors = runner.send_keys('a b')
        self.assertEqual(len(errors), 1)
        self.assertIn('broken pipe', errors[0])

    def serve(self, files, served, errors, typed):
        runner.serve_input(files.get, lambda chords, alive: typed.append(chords) or [],
                           'keys', 'keys', 'a ret', served, errors)

    def test_each_live_marker_gets_the_keys_once(self):
        files, served, errors, typed = {'keys-keys-1.txt': 'ready\n'}, set(), [], []
        with contextlib.redirect_stdout(io.StringIO()):
            self.serve(files, served, errors, typed)
            self.serve(files, served, errors, typed)
            self.assertEqual(typed, ['a ret'])
            files.update({'keys-shell-rc.txt': 'rc 0\n', 'keys-keys-2.txt': 'ready\n'})
            self.serve(files, served, errors, typed)
        self.assertEqual(typed, ['a ret', 'a ret'])
        self.assertEqual(errors, [])

    def test_a_marker_of_an_ended_execution_gets_no_keys(self):
        """The Shell execution ended before the runner saw its marker: the
        keys would reach the Run execution or the Shell."""
        for files, mode in (({'keys-keys-1.txt': 'ready\n', 'keys-shell-rc.txt': 'rc 20\n'}, 'shell'),
                            ({'keys-keys-2.txt': 'ready\n', 'keys-shell-rc.txt': 'rc 0\n',
                              'keys-run-done.txt': 'done\n'}, 'run')):
            with self.subTest(mode=mode):
                served, errors, typed = set(), [], []
                with contextlib.redirect_stdout(io.StringIO()):
                    self.serve(files, served, errors, typed)
                self.assertEqual(typed, [])
                self.assertEqual([(e['program'], e['mode']) for e in errors], [('keys', mode)])

    def test_key_errors_fail_their_execution(self):
        files = {'done.txt': 'done\n', 'sample-run-done.txt': 'done\n',
                 'sample-shell.txt': PASS_OUTPUT, 'sample-run.txt': PASS_OUTPUT,
                 'sample-shell-rc.txt': 'rc 0\n', 'sample-run-rc.txt': 'rc 0\n'}
        vm = {'completed': True, 'stop_reason': 'completed',
              'input_errors': [{'program': 'sample', 'mode': 'run', 'error': 'send-key x: no'}]}
        report = runner.assess_boot(['sample'], files.get, vm)
        self.assertFalse(report['passed'])
        shell, run = report['results']
        self.assertEqual((shell['status'], run['status']), ('pass', 'fail'))
        self.assertIn('send-key x: no', run['failures'])

    def test_keys_with_more_programs_need_separate(self):
        base = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools',
                '--keys', 'one', 'a ret']
        with tempfile.TemporaryDirectory() as directory:
            report = {'directory': directory, 'passed': True, 'results': [],
                      'vm': {'completed': True, 'stop_reason': 'completed'}}
            argv = base + ['--run-dir', directory, 'one', 'two']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                runner.main()
            boot.assert_not_called()
            argv = base + ['--run-dir', str(Path(directory) / 'sep'), '--separate', 'one', 'two']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                runner.main()
            self.assertEqual(boot.call_count, 2)
            self.assertEqual(boot.call_args.args[0].keys, {'one': 'a ret'})


class MouseTests(unittest.TestCase):
    """--mouse: mouse script parsing, QMP input-send-event calls and markers
    served once, only while their execution runs."""

    def test_mouse_steps_parses_moves_waits_and_buttons(self):
        home = [('move', runner.MOUSE_HOME, runner.MOUSE_HOME),
                ('wait', runner.MOUSE_HOME_PAUSE)]
        self.assertEqual(runner.mouse_steps(''), [])
        self.assertEqual(runner.mouse_steps('home'), home)
        self.assertEqual(runner.mouse_steps('by:3,-2'), [('move', 3, -2)])
        self.assertEqual(runner.mouse_steps('by:-120,0'), [('move', -60, 0), ('move', -60, 0)])
        self.assertEqual(runner.mouse_steps('wait:250'), [('wait', 0.25)])
        for prefix, button in (('', 'left'), ('r', 'right'), ('m', 'middle')):
            with self.subTest(button=button):
                down = ('button', button, True)
                up = ('button', button, False)
                self.assertEqual(runner.mouse_steps(prefix + 'down'), [down])
                self.assertEqual(runner.mouse_steps(prefix + 'up'), [up])
                self.assertEqual(runner.mouse_steps(prefix + 'click'), [down, up])
        self.assertEqual(runner.mouse_steps('to:10,20')[:2], home)
        self.assertEqual(runner.mouse_steps('by:0,0'), [])
        self.assertEqual(runner.mouse_steps('home by:3,-2 down wait:250 up'),
                         home + [('move', 3, -2), ('button', 'left', True),
                                 ('wait', 0.25), ('button', 'left', False)])
        self.assertEqual(runner.mouse_steps('shot:icon-1 shot:A_b'),
                         [('shot', 'icon-1'), ('shot', 'A_b')])

    def test_mouse_steps_rejects_invalid_words(self):
        for word in ('jump', 'rel:3,0', 'by:a,b', 'by:1,2,3', 'wait:-5', 'wait:x',
                     'wait:', 'wait:\u00b2', 'home:1', 'click:2', 'to:-1,5', 'to:5,-1',
                     'by:1', 'shot:', 'shot', 'shot:a/b', 'shot:a.png', 'shot:' + 'x' * 33):
            with self.subTest(word=word):
                with self.assertRaises(ValueError) as cm:
                    runner.mouse_steps(word)
                self.assertIn(word, str(cm.exception))

    def test_send_mouse_sends_events_and_pauses(self):
        calls = []

        def command(cmd, arguments):
            calls.append((cmd, arguments))
            return {'return': {}}

        def sleep(seconds):
            calls.append(('sleep', seconds))

        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep', sleep):
            self.assertEqual(runner.send_mouse('by:3,-2'), [])
            self.assertEqual(calls, [
                ('input-send-event',
                 {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': 3}},
                             {'type': 'rel', 'data': {'axis': 'y', 'value': -2}}]}),
                ('sleep', runner.MOUSE_REPORT_PAUSE)])

            calls.clear()
            self.assertEqual(runner.send_mouse('by:0,5'), [])
            self.assertEqual(calls, [
                ('input-send-event',
                 {'events': [{'type': 'rel', 'data': {'axis': 'y', 'value': 5}}]}),
                ('sleep', runner.MOUSE_REPORT_PAUSE)])

            calls.clear()
            self.assertEqual(runner.send_mouse('by:7,0'), [])
            self.assertEqual(calls, [
                ('input-send-event',
                 {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': 7}}]}),
                ('sleep', runner.MOUSE_REPORT_PAUSE)])

            calls.clear()
            self.assertEqual(runner.send_mouse('down up'), [])
            self.assertEqual(calls, [
                ('input-send-event',
                 {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]}),
                ('sleep', runner.MOUSE_BUTTON_PAUSE),
                ('input-send-event',
                 {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]}),
                ('sleep', runner.MOUSE_BUTTON_PAUSE)])

            calls.clear()
            self.assertEqual(runner.send_mouse('rclick'), [])
            self.assertEqual(calls, [
                ('input-send-event',
                 {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'right'}}]}),
                ('sleep', runner.MOUSE_BUTTON_PAUSE),
                ('input-send-event',
                 {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'right'}}]}),
                ('sleep', runner.MOUSE_BUTTON_PAUSE)])

            calls.clear()
            self.assertEqual(runner.send_mouse('wait:300'), [])
            self.assertEqual(calls, [('sleep', 0.3)])

    def test_send_mouse_saves_shots(self):
        calls = []

        def command(cmd, arguments):
            calls.append((cmd, arguments))
            return {'return': {}}

        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep'):
            shots = []
            self.assertEqual(runner.send_mouse('shot:one down shot:two',
                                               shot=lambda name: shots.append(name)), [])
            self.assertEqual(shots, ['one', 'two'])
            self.assertEqual([cmd for cmd, _ in calls], ['input-send-event'])
            # without a shot function a shot is skipped
            calls.clear()
            self.assertEqual(runner.send_mouse('shot:one up'), [])
            self.assertEqual([cmd for cmd, _ in calls], ['input-send-event'])
            # the error of a shot ends the script there
            calls.clear()
            self.assertEqual(runner.send_mouse('shot:one down', shot=lambda name: 'full disk'),
                             ['full disk'])
            self.assertEqual(calls, [])
        # a script that ends with only shots and waits left is no error
        self.assertEqual(runner.send_mouse('shot:late wait:100', alive=lambda: False,
                                           shot=lambda name: None), [])

    def test_shot_path_keeps_every_shot(self):
        with tempfile.TemporaryDirectory() as directory:
            run = runner.Path(directory)
            self.assertEqual(runner.shot_path(run, 'icon').name, 'screen-icon.png')
            (run / 'screen-icon.png').write_bytes(b'')
            self.assertEqual(runner.shot_path(run, 'icon').name, 'screen-icon-2.png')
            (run / 'screen-icon-2.png').write_bytes(b'')
            self.assertEqual(runner.shot_path(run, 'icon').name, 'screen-icon-3.png')
            self.assertEqual(runner.shot_path(run, 'other').name, 'screen-other.png')

    def test_screen_shot_reports_qmp_errors(self):
        path = runner.Path('/tmp/screen-x.png')
        with mock.patch.object(runner, 'qmp_command', return_value={'return': {}}) as command:
            self.assertIsNone(runner.screen_shot(path))
            command.assert_called_once_with('screendump', {'filename': str(path), 'format': 'png'})
        with mock.patch.object(runner, 'qmp_command',
                               return_value={'error': {'desc': 'no display'}}):
            self.assertEqual(runner.screen_shot(path), 'screendump screen-x.png: no display')
        with mock.patch.object(runner, 'qmp_command', side_effect=OSError('broken pipe')):
            self.assertEqual(runner.screen_shot(path), 'screendump screen-x.png: broken pipe')

    def test_send_mouse_reports_the_first_error(self):
        sent = []

        def command(cmd, arguments):
            sent.append((cmd, arguments))
            if len(sent) == 2:
                return {'error': {'class': 'GenericError', 'desc': 'no'}}
            return {'return': {}}

        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep') as sleep:
            errors = runner.send_mouse('down up down')
        self.assertEqual(len(sent), 2)
        self.assertEqual(sleep.call_count, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn('no', errors[0])

    def test_send_mouse_survives_a_lost_connection(self):
        sent = []

        def command(cmd, arguments):
            sent.append((cmd, arguments))
            raise OSError('broken pipe')

        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep') as sleep:
            errors = runner.send_mouse('down up')
        self.assertEqual(len(sent), 1)
        sleep.assert_not_called()
        self.assertEqual(len(errors), 1)
        self.assertIn('broken pipe', errors[0])

    def serve(self, files, served, errors, sent, kind='mouse'):
        runner.serve_input(files.get, lambda script, alive: sent.append(script) or [],
                           'mouse', kind, 'click', served, errors)

    def test_each_live_marker_gets_the_mouse_script_once(self):
        files, served, errors, sent = {'mouse-mouse-1.txt': 'ready\n'}, set(), [], []
        with contextlib.redirect_stdout(io.StringIO()):
            self.serve(files, served, errors, sent)
            self.serve(files, served, errors, sent)
            self.assertEqual(sent, ['click'])
            files.update({'mouse-shell-rc.txt': 'rc 0\n', 'mouse-mouse-2.txt': 'ready\n'})
            self.serve(files, served, errors, sent)
        self.assertEqual(sent, ['click', 'click'])
        self.assertEqual(errors, [])

    def test_a_marker_of_an_ended_execution_gets_no_mouse_input(self):
        for files, mode in (({'mouse-mouse-1.txt': 'ready\n',
                              'mouse-shell-rc.txt': 'rc 20\n'}, 'shell'),
                            ({'mouse-mouse-2.txt': 'ready\n',
                              'mouse-shell-rc.txt': 'rc 0\n',
                              'mouse-run-done.txt': 'done\n'}, 'run')):
            with self.subTest(mode=mode):
                served, errors, sent = set(), [], []
                with contextlib.redirect_stdout(io.StringIO()):
                    self.serve(files, served, errors, sent)
                self.assertEqual(sent, [])
                self.assertEqual([(e['program'], e['mode']) for e in errors],
                                 [('mouse', mode)])
                self.assertIn('no mouse input sent', errors[0]['error'])

    def test_keys_and_mouse_markers_do_not_trigger_each_other(self):
        for files, kind in (({'mouse-keys-1.txt': 'ready\n'}, 'mouse'),
                            ({'mouse-mouse-1.txt': 'ready\n'}, 'keys')):
            with self.subTest(kind=kind):
                served, errors, sent = set(), [], []
                with contextlib.redirect_stdout(io.StringIO()):
                    self.serve(files, served, errors, sent, kind=kind)
                self.assertEqual(sent, [])
                self.assertEqual(served, set())
                self.assertEqual(errors, [])

    def test_each_program_gets_its_own_input(self):
        """The boot loop serves the keys of one program and the mouse
        script of another, each to its own markers."""
        args = mock.Mock(keys={'typed': 'a ret'}, mouse={'moved': 'click'})
        files = {'typed-keys-1.txt': 'ready\n', 'moved-mouse-1.txt': 'ready\n',
                 'other-mouse-1.txt': 'ready\n'}
        sent, served, errors = [], set(), []
        with mock.patch.object(runner, 'send_keys', lambda s, a: sent.append(('keys', s)) or []), \
                mock.patch.object(runner, 'send_mouse',
                                  lambda s, a: sent.append(('mouse', s)) or []), \
                contextlib.redirect_stdout(io.StringIO()):
            runner.serve_inputs(args, ['typed', 'moved', 'other'], files.get, served, errors)
            runner.serve_inputs(args, ['typed', 'moved', 'other'], files.get, served, errors)
        self.assertEqual(sent, [('keys', 'a ret'), ('mouse', 'click')])
        self.assertEqual(errors, [])

    def test_send_mouse_stops_when_its_execution_ends(self):
        """No button or motion after alive() says no; an error only when
        input was still to come."""
        sent, answers = [], [True, True, False]

        def command(cmd, arguments):
            sent.append(arguments['events'][0]['type'])
            return {'return': {}}
        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep'):
            errors = runner.send_mouse('click wait:300 by:10,0 click',
                                       lambda: answers.pop(0) if answers else False)
        self.assertEqual(sent, ['btn', 'btn'])
        self.assertEqual(len(errors), 1)
        self.assertIn('3 of the input steps', errors[0])
        with mock.patch.object(runner, 'qmp_command', command), \
                mock.patch.object(runner.time, 'sleep'):
            self.assertEqual(runner.send_mouse('wait:300 wait:200', lambda: False), [])

    def test_send_mouse_looks_at_alive_every_second_of_a_wait(self):
        looks, sleeps = [], []
        with mock.patch.object(runner.time, 'sleep', sleeps.append):
            self.assertEqual(runner.send_mouse('wait:2500', lambda: looks.append(1) or True), [])
        self.assertEqual(sleeps, [1.0, 1.0, 0.5])
        self.assertEqual(len(looks), 3)
        looks, sleeps = [True, False], []
        with mock.patch.object(runner.time, 'sleep', sleeps.append), \
                mock.patch.object(runner, 'qmp_command', return_value={'return': {}}) as qmp:
            errors = runner.send_mouse('wait:5000 click', lambda: looks.pop(0))
        self.assertEqual(sleeps, [1.0])
        qmp.assert_not_called()
        self.assertIn('2 of the input steps', errors[0])

    def test_send_mouse_looks_at_alive_during_a_motion(self):
        """A long motion asks alive() before its first report and then
        once a second, and stops when the execution has ended."""
        with mock.patch.object(runner, 'qmp_command', return_value={'return': {}}) as qmp, \
                mock.patch.object(runner.time, 'sleep'):
            errors = runner.send_mouse('by:6000,0', lambda: False)
        qmp.assert_not_called()
        self.assertIn('100 of the input steps', errors[0])
        clock, answers = [0.0], [True, True, True, False]

        def sleep(seconds):
            clock[0] += seconds
        with mock.patch.object(runner, 'qmp_command', return_value={'return': {}}) as qmp, \
                mock.patch.object(runner, 'MOUSE_REPORT_PAUSE', 0.25), \
                mock.patch.object(runner.time, 'sleep', sleep), \
                mock.patch.object(runner.time, 'monotonic', lambda: clock[0]):
            errors = runner.send_mouse('by:6000,0', lambda: answers.pop(0))
        self.assertEqual(qmp.call_count, 12)
        self.assertIn('88 of the input steps', errors[0])

    def test_send_keys_stops_when_its_execution_ends(self):
        answers = [True, False]
        with mock.patch.object(runner, 'qmp_command', return_value={'return': {}}) as qmp, \
                mock.patch.object(runner.time, 'sleep'):
            errors = runner.send_keys('a b c', lambda: answers.pop(0))
        self.assertEqual(qmp.call_count, 1)
        self.assertIn('2 of the input steps', errors[0])

    def test_input_stops_with_its_execution(self):
        """serve_input() hands the script an alive() that turns false once
        the execution that wrote the marker ends."""
        files, served, errors, seen = {'mouse-mouse-1.txt': 'ready\n'}, set(), [], []

        def send(script, alive):
            seen.append(alive())
            files['mouse-shell-rc.txt'] = 'rc 0\n'
            seen.append(alive())
            return []
        with contextlib.redirect_stdout(io.StringIO()):
            runner.serve_input(files.get, send, 'mouse', 'mouse', 'click', served, errors)
        self.assertEqual(seen, [True, False])
        seen.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            runner.serve_input({'mouse-mouse-1.txt': 'ready\n'}.get, send, 'mouse', 'mouse',
                               'click', set(), errors, lambda: False)
        self.assertEqual(seen, [False, False])

    def test_a_marker_between_two_looks_still_fails_its_execution(self):
        """Both executions came and went since the last look: the guest is
        done, and their markers are errors, not input never sent."""
        args = mock.Mock(keys={}, mouse={'moved': 'click'})
        files = {'done.txt': 'done\n', 'moved-run-done.txt': 'done\n',
                 'moved-mouse-1.txt': 'ready\n', 'moved-mouse-2.txt': 'ready\n',
                 'moved-shell-rc.txt': 'rc 0\n'}
        served, errors = set(), []
        with mock.patch.object(runner, 'send_mouse') as send:
            self.assertTrue(runner.guest_poll(args, ['moved'], files.get, served, errors))
        send.assert_not_called()
        self.assertEqual([(e['program'], e['mode']) for e in errors],
                         [('moved', 'shell'), ('moved', 'run')])

    def test_mouse_errors_fail_their_execution(self):
        files = {'done.txt': 'done\n', 'sample-run-done.txt': 'done\n',
                 'sample-shell.txt': PASS_OUTPUT, 'sample-run.txt': PASS_OUTPUT,
                 'sample-shell-rc.txt': 'rc 0\n', 'sample-run-rc.txt': 'rc 0\n'}
        error = 'input-send-event move: no'
        vm = {'completed': True, 'stop_reason': 'completed',
              'input_errors': [{'program': 'sample', 'mode': 'shell', 'error': error}]}
        report = runner.assess_boot(['sample'], files.get, vm)
        self.assertFalse(report['passed'])
        shell, run = report['results']
        self.assertEqual((shell['status'], run['status']), ('fail', 'pass'))
        self.assertIn(error, shell['failures'])

    def test_mouse_cli_validation_and_separate(self):
        base = ['run-raspi-qemu.py', '--image', 'unused.img', '--kernel', 'unused.kernel',
                '--bsp', 'unused.bsp', '--dtb', 'unused.dtb', '--mtools', 'unused-tools']
        with tempfile.TemporaryDirectory() as directory:
            report = {'directory': directory, 'passed': True, 'results': [],
                      'vm': {'completed': True, 'stop_reason': 'completed'}}
            for extra in (['--mouse', 'other', 'click', 'one'],
                          ['--mouse', 'one', 'click', 'one', 'two'],
                          ['--mouse', 'one', 'to:1', 'one'],
                          ['--mouse', 'one', 'click', '--mouse', 'one', 'click', 'one'],
                          ['--keys', 'one', 'a', '--keys', 'one', 'b', 'one']):
                with self.subTest(extra=extra):
                    argv = base + ['--run-dir', directory] + extra
                    with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                            mock.patch.object(runner.sys, 'argv', argv), \
                            contextlib.redirect_stderr(io.StringIO()), \
                            self.assertRaises(SystemExit):
                        runner.main()
                    boot.assert_not_called()
            argv = base + ['--mouse', 'one', 'click', '--run-dir',
                           str(Path(directory) / 'sep'), '--separate', 'one', 'two']
            with mock.patch.object(runner, 'boot', return_value=report) as boot, \
                    mock.patch.object(runner.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                runner.main()
            self.assertEqual(boot.call_count, 2)
            self.assertEqual(boot.call_args.args[0].mouse, {'one': 'click'})


def hid_reports(steps):
    """The reports of QEMU's USB mouse for the motion of the steps, each one
    taken by the guest before the next motion comes: at most 127 counts on
    an axis, the rest in the reports after it (hw/input/hid.c)."""
    for step in steps:
        if step[0] != 'move':
            continue
        dx, dy = step[1:]
        while dx or dy:
            report = (max(-127, min(127, dx)), max(-127, min(127, dy)))
            yield report
            dx, dy = dx - report[0], dy - report[1]


def intuition_pointer(reports, start=(512, 384), display=(1024, 768)):
    """Where the reports take the pointer in AROS's Intuition with mouse
    acceleration and two counts a pixel (rom/intuition/inputhandler.c): a
    report plus the remainder of the last one is doubled when its larger
    axis has more than 2 counts, then each axis is divided by 2 as C
    divides, the remainder kept, and the pointer stays on the display.
    The position and the remainders at the end."""
    (x, y), carry = start, (0, 0)
    for dx, dy in reports:
        dx, dy = dx + carry[0], dy + carry[1]
        if max(abs(dx), abs(dy)) > 2:
            dx, dy = dx * 2, dy * 2
        carry = (dx - 2 * int(dx / 2), dy - 2 * int(dy / 2))
        x = min(max(x + int(dx / 2), 0), display[0] - 1)
        y = min(max(y + int(dy / 2), 0), display[1] - 1)
    return (x, y), carry


class MotionTests(unittest.TestCase):
    """by:DX,DY and to:X,Y move the pointer exactly in AROS and leave no
    remainder, also next to the edges of the display and when the guest
    takes two reports of a long motion as one."""

    def reports(self, dx, dy):
        steps = runner.mouse_motion(dx, dy)
        self.assertTrue(all(step[0] == 'move' for step in steps))
        return [step[1:] for step in steps]

    def test_every_motion_is_exact(self):
        small = range(-3, 4)
        for dx in range(-150, 151):
            for dy in sorted(set(small) | set(range(-150, 151, 7))):
                with self.subTest(dx=dx, dy=dy):
                    reports = self.reports(dx, dy)
                    self.assertEqual(intuition_pointer(reports), ((512 + dx, 384 + dy), (0, 0)))
                    for rx, ry in reports:
                        if max(abs(dx), abs(dy)) >= 3:
                            self.assertGreaterEqual(max(abs(rx), abs(ry)), 3)
                            self.assertLessEqual(max(abs(rx), abs(ry)), runner.MOUSE_CHUNK)
                        else:
                            self.assertIn(rx, (-2, 0, 2))
                            self.assertIn(ry, (-2, 0, 2))

    def test_two_reports_taken_as_one_stay_exact(self):
        """QEMU merges the motion of reports that the guest has not taken
        yet; past 127 counts it would split them again."""
        for dx in range(-200, 201, 3):
            for dy in range(-200, 201, 5):
                reports = self.reports(dx, dy)
                if max(abs(dx), abs(dy)) < 3:
                    continue
                for i in range(len(reports) - 1):
                    merged = (reports[:i] + [(reports[i][0] + reports[i + 1][0],
                                              reports[i][1] + reports[i + 1][1])] +
                              reports[i + 2:])
                    with self.subTest(dx=dx, dy=dy, merged=i):
                        self.assertEqual(intuition_pointer(merged)[0], (512 + dx, 384 + dy))
                        self.assertLessEqual(max(abs(v) for v in merged[i]), 127)

    def test_small_motions_one_pixel_a_report(self):
        self.assertEqual(self.reports(1, 0), [(2, 0)])
        self.assertEqual(self.reports(0, -2), [(0, -2), (0, -2)])
        self.assertEqual(self.reports(-1, 1), [(-2, 2)])
        self.assertEqual(self.reports(2, -1), [(2, -2), (2, 0)])
        self.assertEqual(self.reports(0, 0), [])

    def test_small_motions_next_to_the_edges(self):
        for start, motion, end in (((1022, 5), (1, 0), (1023, 5)), ((1, 1), (-1, -1), (0, 0)),
                                   ((1023, 767), (-2, -2), (1021, 765)),
                                   ((0, 766), (2, 1), (2, 767))):
            with self.subTest(start=start, motion=motion):
                self.assertEqual(intuition_pointer(self.reports(*motion), start)[0], end)

    def test_to_reaches_the_pixel_from_anywhere(self):
        for display in ((1024, 768), (640, 256), (2048, 2048)):
            for start in ((0, 0), (display[0] - 1, display[1] - 1), (500 % display[0], 200)):
                for target in ((0, 0), (240, 150), (1, 2), (display[0] - 1, display[1] - 1)):
                    with self.subTest(display=display, start=start, target=target):
                        steps = runner.mouse_steps('to:%d,%d' % target)
                        self.assertEqual(steps[:2], runner.mouse_steps('home'))
                        self.assertEqual(
                            intuition_pointer(hid_reports(steps), start, display),
                            (target, (0, 0)))


class CallNameTests(unittest.TestCase):
    """Names from the thunk tables and the AROS .conf files of this tree.
    The expected names and LVOs are those of the NDK 3.2 fd files."""

    def names(self, *trace):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'serial-aux.log'
            log.write_text(''.join('[kaffeine] %s\n' % line for line in trace))
            out = Path(directory) / 'calls.txt'
            runner.calls([Path(directory) / 'serial.log', log], out)
            return out.read_text().splitlines()

    def test_library_with_manual_thunks_only(self):
        # Only kaffeine_thunks_mathieee.c has thunks for these libraries.
        known = runner.thunk_names()
        self.assertEqual(known[('mathieeedoubbas.library', 30)], 'IEEEDPFix')
        self.assertEqual(known[('mathieeedoubtrans.library', 126)], 'IEEEDPLog10')
        self.assertEqual(self.names('CALL mathieeedoubbas.library LVO -30',
                                    'CALL mathieeedoubbas.library LVO -36',
                                    'CALL mathieeedoubbas.library LVO -42',
                                    'CALL mathieeedoubbas.library LVO -72',
                                    'CALL mathieeedoubbas.library LVO -78',
                                    'CALL mathieeedoubtrans.library LVO -30',
                                    'CALL mathieeedoubtrans.library LVO -126'),
                         ['CALL mathieeedoubbas.library -30 IEEEDPFix',
                          'CALL mathieeedoubbas.library -36 IEEEDPFlt',
                          'CALL mathieeedoubbas.library -42 IEEEDPCmp',
                          'CALL mathieeedoubbas.library -72 IEEEDPSub',
                          'CALL mathieeedoubbas.library -78 IEEEDPMul',
                          'CALL mathieeedoubtrans.library -30 IEEEDPAtan',
                          'CALL mathieeedoubtrans.library -126 IEEEDPLog10'])

    def test_device_with_manual_thunks(self):
        # timer.device lists its functions after BeginIO and AbortIO.
        self.assertEqual(self.names('CALL timer.device LVO -30',
                                    'CALL timer.device LVO -42',
                                    'CALL timer.device LVO -66',
                                    'UNIMPL timer.device LVO -72'),
                         ['CALL timer.device -30 BeginIO',
                          'CALL timer.device -42 AddTime',
                          'CALL timer.device -66 GetSysTime',
                          'UNIMPL timer.device -72 GetUpTime'])

    def test_library_without_thunk_names(self):
        # No table has a thunk for mathieeesingbas.library, so its .conf
        # list has no name to align on and starts at -30.
        self.assertEqual(self.names('UNIMPL mathieeesingbas.library LVO -30',
                                    'UNIMPL mathieeesingbas.library LVO -42',
                                    'UNIMPL mathieeesingbas.library LVO -96'),
                         ['UNIMPL mathieeesingbas.library -30 IEEESPFix',
                          'UNIMPL mathieeesingbas.library -42 IEEESPCmp',
                          'UNIMPL mathieeesingbas.library -96 IEEESPCeil'])

    def test_libraries_with_generated_thunks(self):
        # exec.conf and dos.conf start at -6: Alert() has no thunk, and the
        # Examine() thunk is in a table macro, so both come from the .conf.
        self.assertEqual(self.names('CALL exec.library LVO -552',
                                    'GEN dos.library LVO -216',
                                    'CALL graphics.library LVO -60',
                                    'CALL utility.library LVO -150',
                                    'UNIMPL exec.library LVO -108',
                                    'CALL dos.library LVO -102'),
                         ['CALL exec.library -552 OpenLibrary',
                          'GEN dos.library -216 IsInteractive',
                          'CALL graphics.library -60 Text',
                          'CALL utility.library -150 SDivMod32',
                          'UNIMPL exec.library -108 Alert',
                          'CALL dos.library -102 Examine'])

    def test_manual_thunk_comes_before_generated_one(self):
        # kaffeine runs the manual thunk of an LVO that has both.
        with tempfile.TemporaryDirectory() as directory:
            tables = Path(directory)
            (tables / 'kaffeine_thunks_gen.c').write_text(
                '{ 30, thunk_sample_Generated },\n{ 36, thunk_sample_Second },\n')
            (tables / 'kaffeine_thunks_sample.c').write_text(
                '{ 30, kaffeine_sample_Manual },\n{ 60, thunk_gfx_Text },\n')
            with mock.patch.object(runner, 'KAFFEINE_DIR', tables):
                self.assertEqual(runner.thunk_names(),
                                 {('sample.library', 30): 'Manual',
                                  ('sample.library', 36): 'Second',
                                  ('graphics.library', 60): 'Text'})


if __name__ == '__main__':
    unittest.main()
