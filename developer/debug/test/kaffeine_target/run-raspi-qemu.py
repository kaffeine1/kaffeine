#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Run 68k test programs through kaffeine.library in an AROS raspi-aarch64 VM.

The VM boots the given kernel and BSP package with a copy of an AROS
raspi-aarch64 SD image. kaffeine.library, the test programs and an
S:User-Startup that runs them are copied onto the image with mtools. Each
program runs twice: from the Shell, which loads it with LoadSeg() and calls
RunCommand(), and through Run, which starts a new CLI process with
SystemTagList() whose shell runs the program the same way. The results are
written to SYS:EMU68 and read back once S:User-Startup and every Run wrapper
have written their completion markers, or when the timeout expires. A boot
finishing does not make its programs pass: each execution needs a return
code of zero (or the CODE of --expect-rc), nonempty output and no failed
checks. With --expect, an execution also needs output that is byte for byte
the given file, as the bzip2 self-test does. Results are also saved in
results.json in each boot directory and summary.json in the run directory.

A program can take keys from QEMU's USB keyboard: with --keys it writes
SYS:EMU68/NAME-keys-N.txt when it is ready for them, NAME being the name
that it runs under and N 1 from the Shell or, once NAME-shell-rc.txt is
there, 2 through Run. The runner looks for the file every five seconds and
types the chords once for each such file through QMP send-key, if that
execution has not ended yet; keys that it cannot type fail the execution
that waited for them. With --mouse a program takes the USB mouse the same
way: for each SYS:EMU68/NAME-mouse-N.txt the runner moves the mouse and
presses its buttons as the script says, through QMP input-send-event, to
exact pixels of the display while the guest takes each report before the
next one (see mouse_steps() and mouse_motion()). Input stops when its
execution ends. With more programs --keys and --mouse need --separate, and
S:User-Startup waits for the Run execution of such a program to end.

An application can bring its own files: with --files the files and
directories in a directory of the Mac go next to the program in SYS:EMU68,
directories with all they hold, as an application keeps its libraries,
fonts and preferences in its own drawer, so that PROGDIR: finds them.

Both UARTs are captured. QEMU connects the first -serial to the PL011 and the
second to the mini UART. The bootstrap of this tree writes its own output and
the kernel debug output, which includes the kaffeine call trace, to the UART
named by the serial0 alias of the device tree: the mini UART for
bcm2710-rpi-3-b.dtb (serial-aux.log). The nightly bootstrap writes to the
PL011 (serial.log). The call trace is copied to calls.txt from both logs,
each call named after the thunk that ran, or for an LVO without a thunk
after the library's AROS .conf file.

example:
  run-raspi-qemu.py --image base.img --kernel aros-aarch64-raspi.img \\
      --bsp aros-aarch64-bsp.rom --dtb bcm2710-rpi-3-b.dtb \\
      --library kaffeine.library --mtools /path/to/mtools/bin \\
      --run-dir /path/to/run --separate --qemu-log int --idle-timeout 30 \\
      --args c_stdio "one two" --args dos_lvo_cli cli \\
      build/hello_long build/c_stdio dos_lvo_cli=build/dos_lvo

Only one VM runs at a time, and it is always stopped at the end. The image
copy is deleted unless --keep-image is given. With --separate each program
gets its own boot in a subdirectory of the run directory, so a program that
hangs or crashes the guest does not hide the results of the others.
"""
import argparse
import collections
import contextlib
import json
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

FAT_OFFSET = 1048576            # FAT32 partition of the AROS raspi SD images
# The PL011 and the mini UART; the bootstrap picks the one it writes to.
UART_LOGS = ('serial.log', 'serial-aux.log')
STATE = Path('/private/tmp/emu68-arm')  # short: UNIX socket paths are limited
SOCK = STATE / 'qmp.sock'
AROS_DIR = Path(__file__).resolve().parents[4]
KAFFEINE_DIR = AROS_DIR / 'rom' / 'kaffeine'
# A thunk table entry: { LVO, thunk_<lib>_<Func> } or, for the manual thunks
# kept in their own files, { LVO, kaffeine_<lib>_<Func> }.
THUNK_ENTRY = re.compile(r'\{\s*(\d+)\s*,\s*(?:thunk|kaffeine)_([a-z]+)_(\w+)\s*\}')


def mtool(args, name, *rest, check=True):
    return subprocess.run([str(Path(args.mtools) / name), '-i',
                           '%s@@%d' % (args.disk, FAT_OFFSET), *rest],
                          check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def qmp_reply(f):
    """The response to the last command, past the asynchronous events that
    QMP may send before it; OSError for a closed connection or no JSON."""
    while True:
        line = f.readline()
        if not line:
            raise OSError('QMP closed the connection')
        try:
            reply = json.loads(line)
        except ValueError:
            raise OSError('QMP sent no JSON: %r' % line[:80])
        if 'return' in reply or 'error' in reply:
            return reply


def qmp_command(cmd, arguments=None):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    try:
        s.connect(str(SOCK))
        f = s.makefile('rw')
        f.readline()
        f.write(json.dumps({'execute': 'qmp_capabilities'}) + '\n')
        f.flush()
        qmp_reply(f)
        msg = {'execute': cmd}
        if arguments:
            msg['arguments'] = arguments
        f.write(json.dumps(msg) + '\n')
        f.flush()
        return qmp_reply(f)
    finally:
        s.close()


def qmp(cmd, arguments=None):
    return json.dumps(qmp_command(cmd, arguments))


def shot_path(run, name):
    """screen-NAME.png in the run directory, or screen-NAME-2.png and so on
    when a shot of that name was saved already: the Shell execution and the
    Run execution of a program play its mouse script each."""
    path, n = run / ('screen-%s.png' % name), 2
    while path.exists():
        path, n = run / ('screen-%s-%d.png' % (name, n)), n + 1
    return path


def screen_shot(path):
    """Save the screen of the guest as a PNG at path: None, or an error."""
    try:
        reply = qmp_command('screendump', {'filename': str(path), 'format': 'png'})
    except OSError as e:
        return 'screendump %s: %s' % (path.name, e)
    if 'error' in reply:
        return 'screendump %s: %s' % (path.name, reply['error'].get('desc', reply['error']))
    print('screen saved as %s' % path.name)
    return None


def stopped(rest):
    """The error of input stopped by alive(), when input was still to come:
    it would reach whatever runs after the execution that asked for it."""
    return ['the execution ended or the boot ran out of time before %d of the input steps '
            'were sent' % rest] if rest else []


def send_keys(chords, alive=lambda: True):
    """Type chords on QEMU's USB keyboard: QMP qcodes separated by spaces,
    with + between keys held together, for example 'a b shift+c ret', while
    alive() says that the execution that asked for them still runs and the
    boot has time left. The errors, none when every chord went through."""
    chords = chords.split()
    for i, chord in enumerate(chords):
        if not alive():
            return stopped(len(chords) - i)
        try:
            reply = qmp_command('send-key', {'keys': [{'type': 'qcode', 'data': key}
                                                      for key in chord.split('+')]})
        except OSError as e:
            return ['send-key %s: %s' % (chord, e)]
        if 'error' in reply:
            return ['send-key %s: %s' % (chord, reply['error'].get('desc', reply['error']))]
        time.sleep(0.2)
    return []


MOUSE_BUTTONS = {'': 'left', 'r': 'right', 'm': 'middle'}
# down, up and click for each button: the presses (True) and releases
MOUSE_PRESSES = {prefix + action: (button, presses)
                 for prefix, button in MOUSE_BUTTONS.items()
                 for action, presses in (('down', (True,)), ('up', (False,)),
                                         ('click', (True, False)))}
MOUSE_REPORT_PAUSE = 0.05       # seconds for the guest to take one report
MOUSE_CHUNK = 60                # counts of one report on each axis
MOUSE_BUTTON_PAUSE = 0.1
MOUSE_HOME = -4096              # a motion past any corner of the display
MOUSE_HOME_PAUSE = 1.0
MOUSE_ALIVE_EVERY = 1.0         # seconds of a wait between two looks at alive()


def mouse_pair(word, value, minimum=None):
    """The two numbers X,Y of a step of a mouse script."""
    try:
        x, y = (int(number) for number in value.split(','))
    except ValueError:
        raise ValueError('%s: expected two numbers, X,Y' % word)
    if minimum is not None and min(x, y) < minimum:
        raise ValueError('%s: the numbers must be %d or more' % (word, minimum))
    return x, y


SHOT_NAME = re.compile(r'[A-Za-z0-9_-]{1,32}$')


def mouse_steps(script):
    """The steps of a mouse script, words separated by spaces: home, to:X,Y,
    by:DX,DY, down, up, click (rdown, rup, rclick for the right button,
    mdown, mup, mclick for the middle one), wait:MS and shot:NAME, which
    saves the screen as screen-NAME.png in the run directory. A step is
    ('move', dx, dy), one report of the USB mouse, ('button', name, down),
    ('wait', seconds) or ('shot', name); ValueError for a word that is none
    of these."""
    steps = []
    for word in script.split():
        verb, colon, value = word.partition(':')
        if not colon and verb == 'home':
            steps += [('move', MOUSE_HOME, MOUSE_HOME), ('wait', MOUSE_HOME_PAUSE)]
        elif colon and verb == 'to':
            steps += mouse_steps('home') + mouse_motion(*mouse_pair(word, value, 0))
        elif colon and verb == 'by':
            steps += mouse_motion(*mouse_pair(word, value))
        elif colon and verb == 'wait':
            if not (value.isascii() and value.isdigit()):
                raise ValueError('%s: expected milliseconds' % word)
            steps.append(('wait', int(value) / 1000))
        elif colon and verb == 'shot':
            if not SHOT_NAME.match(value):
                raise ValueError('%s: expected a name of letters, digits, - and _' % word)
            steps.append(('shot', value))
        elif not colon and verb in MOUSE_PRESSES:
            button, presses = MOUSE_PRESSES[verb]
            steps += [('button', button, down) for down in presses]
        else:
            raise ValueError('%s: not a step of a mouse script' % word)
    return steps


def mouse_motion(dx, dy):
    """The reports for a motion of exactly dx, dy pixels. With the input
    preferences of the SD image, mouse acceleration and two counts a pixel,
    AROS's Intuition doubles a report whose larger axis has more than 2
    counts and then halves every report, keeping the remainder for the next
    one: a report of 3 counts or more on its larger axis moves the pointer
    by as many pixels, and one of 2 counts by one pixel, with no remainder
    either way. So a motion of 3 pixels or more goes in reports of 3 to
    MOUSE_CHUNK counts on their larger axis, which stay exact also when QEMU
    merges two of them because the guest has not taken the first one yet,
    and a motion of 1 or 2 pixels in reports of 2 counts a pixel."""
    size = max(abs(dx), abs(dy))
    n, counts = (-(-size // MOUSE_CHUNK), 1) if size >= 3 else (size, 2)
    return [('move', counts * (dx * (i + 1) // n - dx * i // n),
             counts * (dy * (i + 1) // n - dy * i // n)) for i in range(n)]


def send_mouse(script, alive=lambda: True, shot=None):
    """Move QEMU's USB mouse and press its buttons through QMP
    input-send-event, step by step, while alive() says that the execution
    that asked for them still runs and the boot has time left: it is asked
    before the first step, before each button, at each second of a wait and
    at least once a second during a motion. shot(name) saves the screen for
    a shot step and gives an error or None; without it a shot is skipped. A
    script that stops with only waits and shots left is no error. The
    errors, none when every step went through."""
    steps = mouse_steps(script)
    asked = None                # when alive() was asked last
    for i, step in enumerate(steps):
        rest = sum(1 for later in steps[i:] if later[0] not in ('wait', 'shot'))
        if step[0] == 'shot':
            error = shot(step[1]) if shot else None
            if error:
                return [error]
            continue
        if step[0] == 'wait':
            pause = step[1]
            while pause > 0:
                asked = time.monotonic()
                if not alive():
                    return stopped(rest)
                time.sleep(min(pause, MOUSE_ALIVE_EVERY))
                pause -= MOUSE_ALIVE_EVERY
            continue
        if step[0] == 'button' or asked is None or time.monotonic() - asked >= MOUSE_ALIVE_EVERY:
            asked = time.monotonic()
            if not alive():
                return stopped(rest)
        if step[0] == 'move':
            events = [{'type': 'rel', 'data': {'axis': axis, 'value': value}}
                      for axis, value in (('x', step[1]), ('y', step[2])) if value]
            pause = MOUSE_REPORT_PAUSE
        else:
            events = [{'type': 'btn', 'data': {'down': step[2], 'button': step[1]}}]
            pause = MOUSE_BUTTON_PAUSE
        try:
            reply = qmp_command('input-send-event', {'events': events})
        except OSError as e:
            return ['input-send-event %s: %s' % (step, e)]
        if 'error' in reply:
            return ['input-send-event %s: %s' % (step, reply['error'].get('desc', reply['error']))]
        time.sleep(pause)
    return []


def input_execution(read_file, name, n):
    """The execution that marker n of a program belongs to, 'shell' for 1
    and 'run' for 2, or None when it has ended: S:User-Startup writes
    NAME-shell-rc.txt as the Shell execution ends, before Run starts the
    other one, and the program tells its executions apart by that file."""
    if n == 1:
        return 'shell' if read_file('%s-shell-rc.txt' % name) is None else None
    return 'run' if not marker_done(read_file('%s-run-done.txt' % name)) else None


INPUT_KINDS = {'keys': ('typing', 'no keys typed'), 'mouse': ('moving', 'no mouse input sent')}


def serve_input(read_file, send, name, kind, script, served, errors, alive=lambda: True):
    """Send the script once for each new marker of that kind, keys or
    mouse, of a program whose execution still runs, while it runs and
    alive() says that the boot goes on; a marker of an ended execution is an
    error of the program's, as the input would reach whatever runs then."""
    doing, nothing = INPUT_KINDS[kind]
    for n, mode in ((1, 'shell'), (2, 'run')):
        marker = '%s-%s-%d.txt' % (name, kind, n)
        if marker in served or read_file(marker) is None:
            continue
        served.add(marker)
        if input_execution(read_file, name, n) is None:
            errors.append({'program': name, 'mode': mode,
                           'error': '%s came after the execution ended; %s' % (marker, nothing)})
            continue
        print('%s: %s %r for %s' % (name, doing, script, marker))
        running = lambda: alive() and input_execution(read_file, name, n) is not None
        errors.extend({'program': name, 'mode': mode, 'error': error}
                      for error in send(script, running))


def serve_inputs(args, names, read_file, served, errors, alive=lambda: True, shot=None):
    """Each marker of a program that waits for keys or the mouse gets its
    input once; shot(name) saves the screen for the shot steps of a mouse
    script."""
    mouse = send_mouse if shot is None else lambda script, running: send_mouse(script, running, shot)
    for name in names:
        for kind, scripts, send in (('keys', args.keys, send_keys),
                                    ('mouse', args.mouse, mouse)):
            if name in scripts:
                serve_input(read_file, send, name, kind, scripts[name], served, errors, alive)


def guest_poll(args, names, read_file, served, errors, alive=lambda: True, shot=None):
    """One look at the guest: the input for the markers that came, then
    whether S:User-Startup and every Run execution have ended. The markers
    come first, so that one whose execution came and went since the last
    look is still an error of that execution."""
    serve_inputs(args, names, read_file, served, errors, alive, shot)
    return guest_completed(read_file, names)


def program_tail(name, arguments, inputs):
    tail = (' <SYS:EMU68/%s.in' % name) if name in inputs else ''
    return tail + ((' ' + arguments[name]) if arguments.get(name) else '')


def run_script(name, arguments, inputs, cpu=None):
    """Executed by the new CLI started by Run. Capture RC on the very next
    line: Copy and Echo would otherwise replace the program's return code.
    Redirect the program itself so the Shell closes its RAM: output before
    Copy reads it; Run does not have to duplicate a FAT file handle. The
    error stream (*>) goes to its own file, as from the Shell."""
    lines = ['FailAt 21',
             'Stack 1048576',
             'SYS:EMU68/%s >RAM:%s-run.txt *>RAM:%s-run-err.txt%s' % (
                 name, name, name, program_tail(name, arguments, inputs)),
             'Echo "rc $RC" >SYS:EMU68/%s-run-rc.txt' % name,
             # A failed/partial Copy must not be followed by a done marker.
             'FailAt 1',
             'Copy RAM:%s-run.txt SYS:EMU68/%s-run.txt' % (name, name),
             'Copy RAM:%s-run-err.txt SYS:EMU68/%s-run-err.txt' % (name, name),
             'Echo "done" >SYS:EMU68/%s-run-done.txt' % name]
    if cpu:
        lines.insert(2, 'Set KAFFEINE_CPU %s' % cpu)
    return '\n'.join(lines) + '\n'


WANDERER_WAIT = 10              # seconds for Wanderer to open its desktop


def user_startup(programs, arguments, inputs, run_wait, waited=(), wanderer=False, cpu=None):
    """S:User-Startup for the programs. For each program in waited, those
    that take keys or the mouse, it goes on only once their Run execution
    has ended: Wanderer starts after S:User-Startup and takes the active
    window, and with it the input. With wanderer, Wanderer starts first,
    for programs that need Workbench: AppIcons, AppWindows and AppMenuItems
    and their AppMessages."""
    lines = ['FailAt 21',
             'Stack 1048576',
             'Echo "started" >SYS:EMU68/started.txt']
    if cpu:
        lines.insert(2, 'Set KAFFEINE_CPU %s' % cpu)
    if wanderer:
        lines += ['Run >NIL: <NIL: WANDERER:Wanderer',
                  'Wait %d SECS' % WANDERER_WAIT]
    for name in programs:
        # The redirections have to follow the command name.
        tail = program_tail(name, arguments, inputs)
        lines += ['Protect SYS:EMU68/%s +e' % name,
                  'SYS:EMU68/%s >SYS:EMU68/%s-shell.txt *>SYS:EMU68/%s-shell-err.txt%s' % (
                      name, name, name, tail),
                  'Echo "rc $RC" >SYS:EMU68/%s-shell-rc.txt' % name,
                  'Run >NIL: Execute SYS:EMU68/%s-run.script' % name,
                  # A pause between launches, not proof that Run finished.
                  'Wait %d SECS' % run_wait]
        if name in waited:
            lines += ['Lab wait-%s' % name,
                      'If NOT EXISTS SYS:EMU68/%s-run-done.txt' % name,
                      '    Wait 1 SECS',
                      '    Skip wait-%s BACK' % name,
                      'EndIf']
    lines += ['Wait 2 SECS',
              'Echo "done" >SYS:EMU68/done.txt']
    return '\n'.join(lines) + '\n'


def startup_script(args, names):
    """S:User-Startup for these programs with the options given: it waits
    for the Run execution of each program that takes keys or the mouse."""
    return user_startup(names, args.arguments, args.inputs, args.run_wait,
                        set(args.keys) | set(args.mouse), args.wanderer,
                        getattr(args, 'cpu', None))


def guest_file(args, name):
    r = mtool(args, 'mtype', '::EMU68/' + name, check=False)
    return r.stdout.decode('latin-1') if r.returncode == 0 else None


def save_text(path, text):
    """Save a guest file, its bytes decoded as Latin-1, in UTF-8."""
    path.write_text(text, encoding='utf-8')


def guest_file_report(name, text, compared):
    """What the runner prints for a guest file: its text, or only the size
    of an output that --expect compares, which can hold any byte value."""
    if name in compared:
        return '== %s: %d bytes, compared with the expected file' % (name, len(text))
    return '== %s\n%s' % (name, text.rstrip())


def load_text(path):
    """Read a guest file back as saved: newline='' keeps carriage returns,
    so text.encode('latin-1') gives the bytes the guest wrote."""
    if not path.exists():
        return None
    with path.open(encoding='utf-8', errors='replace', newline='') as f:
        return f.read()


def marker_done(text):
    return text is not None and text.strip() == 'done'


def guest_completed(read_file, names):
    """The startup marker alone says nothing about background programs."""
    return (marker_done(read_file('done.txt')) and
            all(marker_done(read_file('%s-run-done.txt' % name)) for name in names))


def parse_result(name, mode, output, rc_text, completed=True, expected=None, expected_rc=0):
    """Assess one execution without QEMU or filesystem dependencies.

    Missing/malformed artifacts are incomplete, even if partial output looks
    successful. A completed execution fails on a return code other than
    expected_rc (0 by default), a FAIL line, or a test summary reporting
    failures. Programs such as hello need no check summary, but must produce
    output and return expected_rc. With expected bytes, the output must be
    exactly those bytes, and its lines are not read as checks: a compressed
    file can contain anything.
    """
    if expected_rc is None:
        expected_rc = 0
    errors, failures = [], []
    rc_match = re.fullmatch(r'rc\s+(-?\d+)', rc_text.strip()) if rc_text else None
    rc = int(rc_match.group(1)) if rc_match else None
    if not completed:
        errors.append('missing or invalid completion marker')
    if rc is None:
        errors.append('missing or invalid return code')
    elif rc != expected_rc:
        if expected_rc == 0:
            failures.append('return code %d' % rc)
        else:
            failures.append('return code %d (expected rc %d)' % (rc, expected_rc))
    if output is None or not output.strip():
        errors.append('missing or empty output')
    lines = output.splitlines() if output else []
    matches = None
    if expected is not None and output:
        data = output.encode('latin-1', errors='replace')
        matches = data == expected
        if not matches:
            offset = next((i for i, (got, want) in enumerate(zip(data, expected)) if got != want),
                          min(len(data), len(expected)))
            failures.append('output differs from the expected %d bytes at byte %d of %d' % (
                len(expected), offset, len(data)))
        lines = []
    failed_lines = [line.strip() for line in lines if re.match(r'^\s*FAIL(?:\s|:|$)', line)]
    summaries = []
    for line in lines:
        match = re.fullmatch(r'(.+):\s*(\d+) checks,\s*(\d+) failed\s*', line.strip())
        if match:
            summaries.append({'name': match.group(1), 'checks': int(match.group(2)),
                              'failed': int(match.group(3))})
    if failed_lines:
        failures.append('%d FAIL line(s)' % len(failed_lines))
    if any(summary['failed'] for summary in summaries):
        failures.append('test summary reports failed checks')
    return {'program': name, 'mode': mode,
            'status': 'incomplete' if errors else 'fail' if failures else 'pass',
            'return_code': rc, 'errors': errors, 'failures': failures,
            'failed_lines': failed_lines, 'check_summaries': summaries,
            'output_matches': matches,
            'last_line': ('output matches the expected %d bytes' % len(expected)) if matches else
                         next((line.strip() for line in reversed(lines) if line.strip()),
                              '(no output)')}


def assess_boot(names, read_file, vm, expected=None, expected_rc=None):
    """Keep VM completion and program verdicts separate in a JSON-ready report."""
    results = []
    for name in names:
        for mode in ('shell', 'run'):
            results.append(parse_result(
                name, mode, read_file('%s-%s.txt' % (name, mode)),
                read_file('%s-%s-rc.txt' % (name, mode)),
                completed=(mode == 'shell' or
                           marker_done(read_file('%s-run-done.txt' % name))),
                expected=(expected or {}).get(name),
                expected_rc=(expected_rc or {}).get(name, 0)))
    # input that --keys or --mouse could not send fails the execution that
    # waited for it
    for result in results:
        for input_error in vm.get('input_errors', []):
            if (input_error['program'], input_error['mode']) == (result['program'], result['mode']):
                result['failures'].append(input_error['error'])
                if result['status'] == 'pass':
                    result['status'] = 'fail'
    return {'schema_version': 1, 'vm': vm, 'results': results,
            'passed': vm['completed'] and all(result['status'] == 'pass' for result in results)}


def exceptions(log, out):
    """Copy the exception records of a QEMU -d int log to out, except
    interrupts and the system calls of the kernel, and return a count per
    exception type."""
    counts = collections.Counter()
    keep = False
    with log.open('r', errors='replace') as src, out.open('w') as dst:
        for line in src:
            m = re.match(r'Taking exception \d+ \[([^\]]+)\]', line)
            if m:
                counts[m.group(1)] += 1
                keep = m.group(1) not in ('IRQ', 'FIQ', 'SVC')
            elif not line.startswith('...'):
                keep = False
            if keep:
                dst.write(line)
    return counts


DEVICES = {'timer', 'console'}


def thunk_names():
    """Map (library, LVO) to the function name, from the thunk tables in
    every kaffeine_thunks*.c. <lib> is the library name without .library,
    except gfx for graphics, or without .device for a device. The
    generated table comes last: kaffeine runs the manual thunk of an LVO that
    has both."""
    names = {}
    sources = sorted(KAFFEINE_DIR.glob('kaffeine_thunks*.c'),
                     key=lambda path: (path.name == 'kaffeine_thunks_gen.c', path.name))
    for path in sources:
        for lvo, lib, func in THUNK_ENTRY.findall(path.read_text(errors='replace')):
            lib = 'graphics' if lib == 'gfx' else lib
            names.setdefault((lib + ('.device' if lib in DEVICES else '.library'), int(lvo)), func)
    return names


def conf_names(library, known):
    """Map the LVOs of a library to function names from its AROS .conf file,
    for the calls that have no thunk. The function list is aligned on the
    names the thunk tables give, at -6 (exec and dos list the standard
    vectors) or at -30, the first user vector, which also wins a tie. A
    device lists its functions after BeginIO and AbortIO, from -42."""
    base = library.split('.')[0]
    for conf in (AROS_DIR / 'rom' / base / (base + '.conf'),
                 AROS_DIR / 'workbench' / 'libs' / base / (base + '.conf')):
        if conf.exists():
            break
    else:
        return {}
    slots, listing, device = [], False, False
    for line in conf.read_text(errors='replace').splitlines():
        line = line.strip()
        if line.split()[:1] == ['beginio_func']:
            device = True
        elif line.startswith('##begin functionlist'):
            listing = True
        elif line.startswith('##end functionlist'):
            listing = False
        elif listing and line.startswith('.skip'):
            slots += [None] * int(line.split()[1])
        elif listing and line and not line.startswith(('.', '#')):
            m = re.search(r'(\w+)\s*\(', line)
            slots.append(m.group(1) if m else None)
    # max() returns the first of equal counts.
    first = 7 if device else max((5, 1), key=lambda f: sum(
        known.get((library, (f + i) * 6)) == name for i, name in enumerate(slots)))
    return {(first + i) * 6: name for i, name in enumerate(slots) if name}


def calls(logs, out):
    """Write the kaffeine call trace of the serial logs to out, one line per
    call with the function name, and return a one line summary. Only the
    UART the bootstrap writes to carries the trace, so the logs are read in
    turn."""
    logs = [log for log in logs if log.exists()]
    if not logs:
        return 'kaffeine calls: no serial log'
    names = thunk_names()
    total = 0
    unimplemented = collections.Counter()
    last = None
    with out.open('w') as dst, contextlib.ExitStack() as stack:
        sources = [stack.enter_context(log.open('r', errors='replace')) for log in logs]
        for line in (line for src in sources for line in src):
            m = re.search(r'\[kaffeine\] (CALL|GEN|UNIMPL) (\S+) LVO -(\d+)', line)
            if not m:
                continue
            kind, lib, lvo = m.group(1), m.group(2), int(m.group(3))
            if (lib, lvo) not in names:
                for conf_lvo, name in conf_names(lib, names).items():
                    names.setdefault((lib, conf_lvo), name)
                names.setdefault((lib, lvo), '?')
            last = '%s %s -%d %s' % (kind, lib, lvo, names[(lib, lvo)])
            dst.write(last + '\n')
            total += 1
            if kind == 'UNIMPL':
                unimplemented['%s -%d' % (lib, lvo)] += 1
    return 'kaffeine calls: %d; unimplemented: %s; last: %s' % (
        total, ', '.join('%s (%d)' % kv for kv in sorted(unimplemented.items())) or 'none', last)


def program_name(program):
    """A program is a file, or NAME=FILE to install a file under NAME."""
    return program.split('=', 1)[0] if '=' in program else Path(program).name


def run_guest(args, run, programs, names):
    """Install the programs on the image copy, boot it, wait for the guest
    and copy its result files to run/guest. Return the VM completion status,
    independently of whether the programs passed their checks."""
    mtool(args, 'mmd', '::EMU68')
    if args.library:
        mtool(args, 'mcopy', '-o', args.library, '::Libs/kaffeine.library')
    for prog, name in zip(programs, names):
        # the drawer first, so that the program installed under NAME wins
        if name in args.files:
            for entry in sorted(Path(args.files[name]).iterdir()):
                mtool(args, 'mcopy', '-s', '-o', str(entry), '::EMU68/' + entry.name)
        mtool(args, 'mcopy', '-o', prog, '::EMU68/' + name)
        script = run / ('%s-run.script' % name)
        script.write_text(run_script(name, args.arguments, args.inputs, args.cpu))
        mtool(args, 'mcopy', '-o', str(script), '::EMU68/' + script.name)
        if name in args.inputs:
            mtool(args, 'mcopy', '-o', args.inputs[name], '::EMU68/%s.in' % name)
    (run / 'User-Startup').write_text(startup_script(args, names))
    mtool(args, 'mcopy', '-o', str(run / 'User-Startup'), '::S/User-Startup')

    cmd = [args.qemu, '-name', 'AROS-emu68-test', '-M', 'raspi3b', '-m', '1G', '-smp', '4',
           '-accel', 'tcg,thread=' + args.tcg_thread,
           '-kernel', args.kernel, '-initrd', args.bsp, '-dtb', args.dtb,
           '-drive', 'file=%s,format=raw,if=sd' % args.disk,
           '-device', 'usb-kbd', '-device', 'usb-mouse', '-nic', 'none', '-display', 'none',
           '-serial', 'file:%s' % (run / 'serial.log'),
           '-serial', 'file:%s' % (run / 'serial-aux.log'),
           '-qmp', 'unix:%s,server=on,wait=off' % SOCK, '-monitor', 'none', '-no-reboot']
    if args.qemu_log:
        cmd += ['-d', args.qemu_log, '-D', str(run / 'qemu-debug.log')]
    (run / 'qemu-cmd.txt').write_text(' '.join(cmd) + '\n')
    with (run / 'qemu.log').open('wb') as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    started = time.monotonic()
    finished = guest_started = False
    stop_reason = 'timeout'
    served, input_errors = set(), []
    output_size, output_changed = -1, started
    try:
        while time.monotonic() - started < args.timeout:
            if proc.poll() is not None:
                print('QEMU exited early with status', proc.returncode)
                stop_reason = 'qemu_exited'
                break
            # input for the markers that came, then whether the guest is done
            if guest_poll(args, names, lambda f: guest_file(args, f), served, input_errors,
                          lambda: proc.poll() is None and
                          time.monotonic() - started < args.timeout,
                          lambda name: screen_shot(shot_path(run, name))):
                finished = True
                stop_reason = 'completed'
                break
            if args.idle_timeout:
                # A crashed or hung program leaves the debug output silent.
                if not guest_started and guest_file(args, 'started.txt') is not None:
                    guest_started, output_changed = True, time.monotonic()
                size = sum((run / name).stat().st_size for name in UART_LOGS
                           if (run / name).exists())
                if size != output_size:
                    output_size, output_changed = size, time.monotonic()
                elif guest_started and time.monotonic() - output_changed >= args.idle_timeout:
                    print('%s: no debug output for %d s' % (run.name, args.idle_timeout))
                    stop_reason = 'idle_timeout'
                    break
            time.sleep(5)
        elapsed = time.monotonic() - started
        print('%s: guest %s after %.0f s (%s)' % (
            run.name, 'finished' if finished else 'did NOT finish', elapsed, stop_reason))
    finally:
        if proc.poll() is None:
            try:
                print(qmp('screendump', {'filename': str(run / 'screen-final.png'), 'format': 'png'}).strip())
                qmp('quit')
            except OSError as e:
                print('QMP failed (%s), killing QEMU' % e)
                proc.kill()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        SOCK.unlink(missing_ok=True)

    guest = run / 'guest'
    guest.mkdir()
    compared = {'%s-%s.txt' % (name, mode) for name in args.expected for mode in ('shell', 'run')}
    listing = mtool(args, 'mdir', '-b', '::EMU68', check=False).stdout.decode('latin-1')
    for entry in sorted(listing.split()):
        name = entry.split('/')[-1]
        if name.endswith('.txt'):
            text = guest_file(args, name)
            if text is not None:
                save_text(guest / name, text)
                print(guest_file_report(name, text, compared))
    return {'completed': finished, 'stop_reason': stop_reason,
            'elapsed_seconds': round(elapsed, 3), 'qemu_return_code': proc.returncode,
            'input_errors': input_errors}


def boot(args, run, programs):
    """Boot the VM once, save the result report and return it to the caller."""
    if subprocess.run(['pgrep', '-x', 'qemu-system-aarch64'], stdout=subprocess.DEVNULL).returncode == 0:
        sys.exit('another qemu-system-aarch64 is running; stop it first')
    run.mkdir(parents=True, exist_ok=False)
    STATE.mkdir(parents=True, exist_ok=True)
    SOCK.unlink(missing_ok=True)
    names = [program_name(prog) for prog in programs]
    programs = [prog.split('=', 1)[-1] for prog in programs]
    args.disk = run / 'sdcard.img'
    shutil.copyfile(args.image, args.disk)
    try:
        vm = run_guest(args, run, programs, names)
    finally:
        if not args.keep_image:
            args.disk.unlink(missing_ok=True)

    print(calls([run / name for name in UART_LOGS], run / 'calls.txt'))
    debug_log = run / 'qemu-debug.log'
    if debug_log.exists():
        counts = exceptions(debug_log, run / 'exceptions.txt')
        print('QEMU exceptions: %s' % (', '.join('%s %d' % kv for kv in sorted(counts.items())) or 'none'))
        # The kernel makes hundreds of thousands of system calls in a boot.
        subprocess.run(['gzip', '-f', str(debug_log)], check=False)

    guest = run / 'guest'
    report = assess_boot(names, lambda name: load_text(guest / name), vm,
                         args.expected, getattr(args, 'expected_rc', None))
    report['directory'] = str(run)
    (run / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--image', required=True, help='base SD image (copied, not modified)')
    p.add_argument('--kernel', required=True)
    p.add_argument('--bsp', required=True)
    p.add_argument('--dtb', required=True)
    p.add_argument('--library', help='kaffeine.library to install in SYS:Libs')
    p.add_argument('--mtools', required=True, help='directory with mcopy, mmd, mtype')
    p.add_argument('--run-dir', required=True, help='new directory for this run')
    p.add_argument('--qemu', default='qemu-system-aarch64')
    p.add_argument('--tcg-thread', choices=('single', 'multi'), default='single',
                   help='QEMU CPU scheduling (default: single); multi gives each of the '
                        'four virtual CPUs its own host thread')
    p.add_argument('--qemu-log', metavar='ITEMS',
                   help='QEMU -d items, for example int; logged to qemu-debug.log, and the '
                        'exceptions other than interrupts and system calls are copied to '
                        'exceptions.txt')
    p.add_argument('--args', nargs=2, action='append', default=[], metavar=('PROGRAM', 'ARGS'),
                   help='command line arguments for PROGRAM (its file name)')
    p.add_argument('--input', nargs=2, action='append', default=[], metavar=('PROGRAM', 'FILE'),
                   help='redirect the input of PROGRAM from a copy of FILE')
    p.add_argument('--keys', nargs=2, action='append', default=[], metavar=('PROGRAM', 'CHORDS'),
                   help='type CHORDS (QMP qcodes, + for keys held together) on the USB '
                        'keyboard each time PROGRAM writes SYS:EMU68/PROGRAM-keys-N.txt')
    p.add_argument('--mouse', nargs=2, action='append', default=[], metavar=('PROGRAM', 'SCRIPT'),
                   help='move the USB mouse and press its buttons as SCRIPT says each time '
                        'PROGRAM writes SYS:EMU68/PROGRAM-mouse-N.txt; shot:NAME saves the '
                        'screen as screen-NAME.png in the run directory')
    p.add_argument('--expect', nargs=2, action='append', default=[], metavar=('PROGRAM', 'FILE'),
                   help='PROGRAM passes only if its output is byte for byte FILE')
    p.add_argument('--expect-rc', nargs=2, action='append', default=[],
                   metavar=('PROGRAM', 'CODE'),
                   help='PROGRAM passes only if its return code is CODE (default: 0)')
    p.add_argument('--files', nargs=2, action='append', default=[], metavar=('PROGRAM', 'DIR'),
                   help='install the files and directories in DIR next to PROGRAM in '
                        'SYS:EMU68, as the drawer of an application')
    p.add_argument('--wanderer', action='store_true',
                   help='start Wanderer before the programs, for programs that need Workbench: '
                        'AppIcons, AppWindows, AppMenuItems and their AppMessages')
    p.add_argument('--separate', action='store_true',
                   help='boot once for each program, in a subdirectory of the run directory')
    p.add_argument('--cpu', choices=('moira', 'jit'),
                   help='select KAFFEINE_CPU in both the Shell and Run test scripts')
    p.add_argument('--run-wait', type=int, default=5,
                   help='pause after launching each Run (default 5 seconds); completion '
                        'is checked with per-program markers until --timeout')
    p.add_argument('--timeout', type=int, default=420, help='seconds to wait for each boot')
    p.add_argument('--idle-timeout', type=int, default=0, metavar='SECONDS',
                   help='stop a boot when the guest has started S:User-Startup and both '
                        'UARTs stay silent this long; 0 (default) never')
    p.add_argument('--keep-image', action='store_true')
    p.add_argument('programs', nargs='+', metavar='PROGRAM',
                   help='a 68k executable, or NAME=FILE to install it under another name')
    args = p.parse_args()
    # Guest output is Latin-1 text: a stdout that cannot encode a character
    # escapes it instead of stopping the runner before results.json.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')
    args.arguments = dict(args.args)
    args.inputs = dict(args.input)
    for option, pairs in (('--keys', args.keys), ('--mouse', args.mouse),
                          ('--expect-rc', args.expect_rc)):
        counts = collections.Counter(name for name, _ in pairs)
        twice = sorted(name for name, count in counts.items() if count > 1)
        if twice:
            p.error('%s names a program more than once: %s' % (option, ', '.join(twice)))
    for name, script in args.mouse:
        try:
            mouse_steps(script)
        except ValueError as e:
            p.error('--mouse %s: %s' % (name, e))
    args.keys = dict(args.keys)
    args.mouse = dict(args.mouse)
    args.files = dict(args.files)
    for name, path in args.files.items():
        if not Path(path).is_dir():
            p.error('--files of %s: %s is no directory' % (name, path))
    args.expected = {}
    for name, path in args.expect:
        try:
            args.expected[name] = Path(path).read_bytes()
        except OSError as e:
            p.error('cannot read the expected output of %s: %s' % (name, e))
    args.expected_rc = {}
    for name, code in args.expect_rc:
        if not re.fullmatch(r'-?[0-9]+', code):
            p.error('--expect-rc %s: %r is no integer' % (name, code))
        args.expected_rc[name] = int(code)
    names = [program_name(program) for program in args.programs]
    if any(not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', name) for name in names):
        p.error('program names must contain only letters, digits, underscores, dots and hyphens, '
                'and must not start with a dot or hyphen; use NAME=FILE for other file names')
    if len({name.lower() for name in names}) != len(names):
        p.error('program names must be unique (the FAT volume is case-insensitive)')
    # An expectation that names no program would leave its output or return code unchecked.
    for option, table in (('--expect', args.expected), ('--expect-rc', args.expected_rc)):
        unknown = sorted(set(table) - set(names))
        if unknown:
            p.error('%s names no program: %s (use the NAME of NAME=FILE, spelled as given)'
                    % (option, ', '.join(unknown)))
    for option, scripts in (('--keys', args.keys), ('--mouse', args.mouse)):
        unknown = sorted(set(scripts) - set(names))
        if unknown:
            p.error('%s names no program: %s (use the NAME of NAME=FILE, spelled as given)'
                    % (option, ', '.join(unknown)))
        if scripts and len(names) > 1 and not args.separate:
            p.error('%s needs --separate when more programs run: the window of another one '
                    'could take the input' % option)
    unknown = sorted(set(args.files) - set(names))
    if unknown:
        p.error('--files names no program: %s (use the NAME of NAME=FILE, spelled as given)'
                % ', '.join(unknown))
    if args.files and len(names) > 1 and not args.separate:
        p.error('--files needs --separate when more programs run: their files would share '
                'SYS:EMU68')
    if args.run_wait < 0 or args.idle_timeout < 0 or args.timeout <= 0:
        p.error('--run-wait and --idle-timeout must be nonnegative, and --timeout must be positive')

    run = Path(args.run_dir)
    if args.separate:
        run.mkdir(parents=True, exist_ok=False)
        results = [boot(args, run / program_name(prog), [prog]) for prog in args.programs]
    else:
        results = [boot(args, run, args.programs)]
    passed = all(report['passed'] for report in results)
    (run / 'summary.json').write_text(json.dumps(
        {'schema_version': 1, 'passed': passed, 'boots': results}, indent=2) + '\n')
    print('\nsummary (program verdicts, separate from VM completion):')
    for report in results:
        print('  %s: VM %s' % (report['directory'], report['vm']['stop_reason']))
        for result in report['results']:
            details = '; '.join(result['errors'] + result['failures']) or result['last_line']
            print('  %-14s %-5s %-10s rc %s: %s' % (
                result['program'], result['mode'], result['status'],
                result['return_code'] if result['return_code'] is not None else '?', details))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
