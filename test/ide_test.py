#!/usr/bin/env python
# The test suite for the IDE support library
# This runner is largely copied-and-pasted from test.py - at some point
# they should be merged to a common library both use, but I won't do that
# until I properly know what this runner needs to do.
import argparse
import os
import subprocess
import threading
from pathlib import Path
import platform
import sys
from typing import List, Optional

EXE_SUFFIX = ''
if platform.system() == 'Windows':
    EXE_SUFFIX = '.exe'

WRENC_DIR = Path(__file__).parent.parent
TEST_EXE: Path = WRENC_DIR / 'build' / 'ide-support' / f'ide-test-tool{EXE_SUFFIX}'
TESTS_DIR: Path = WRENC_DIR / 'ide-support' / 'test'

passed = 0
failed = 0
skipped = []
expectations = 0

args: argparse.Namespace


class Assertion:
    # The zero-indexed line this assertion is on.
    line: int

    # The byte position of where this action takes place.
    position: int

    # The raw assertion string, straight out of the file.
    raw: str

    # The 'mode' part of the assertion string. For the simple assertion:
    #   complete:a,b,c
    # This is 'complete'.
    mode: str

    # The argument to the assertion, in the above example it's 'a,b,c'.
    arg: str

    # The length of the line this assertion is on. Set for edit commands.
    length_of_line: int

    def __init__(self, assertion: str, line: int, position: int):
        self.line = line
        self.position = position
        self.raw = assertion

        # Parse the assertion, removing the leading and trailing deliminator
        # characters.
        assertion = assertion[1:-1]

        parts = assertion.split(':', 2)
        self.mode = parts[0]
        self.arg = parts[1] if len(parts) > 1 else ''

        if self.mode not in ['complete', 'delete-line']:
            raise Exception(f"Invalid assertion '{assertion}': bad mode '{self.mode}'.")

    def get_command(self) -> str:
        if self.mode == 'complete':
            return f''
        if self.mode == 'delete-line':
            # Replace the contents of this line (but not the newline, to
            # keep the line numbering constant) with an empty string.
            return f'{self.length_of_line}:'

        raise Exception('Unknown mode for assertion.get_command: ' + self.mode)

    def cmd_name(self) -> str:
        if self.mode == 'delete-line':
            return 'edit'
        return self.mode

    def validate(self, output_lines: List[str]) -> Optional[str]:
        header = f'========== COMMAND {self.line}.{self.cmd_position()} {self.cmd_name()}'
        if output_lines[0] != header:
            return f'Invalid header. Expected: "{header}", got "{output_lines[0]}".'

        output_lines = output_lines[1:]

        if self.mode == 'complete':
            return self._validate_complete(output_lines)

        # Anything that edits text won't get a response back, it's just to
        # set things up for other tests.
        if self.cmd_name() == 'edit':
            return None

        raise Exception('Unknown mode for assertion.validate: ' + self.mode)

    def _validate_complete(self, output_lines: List[str]) -> Optional[str]:
        completions = []

        for line in output_lines:
            parts = line.split(' ')
            if parts[0] != 'entry':
                return f'Invalid line, no "entry" marker: {line}'
            completions.append(parts[1])

        for thing in self.arg.split(','):
            negate = thing[0] == '/'
            if negate:
                thing = thing[1:]
            matched = thing in completions

            if not negate and not matched:
                return f'Missing expected completion "{thing}". Available completions: {completions}'
            if negate and matched:
                return f'Found unexpected completion "{thing}". Available completions: {completions}'

        return None

    def cmd_position(self) -> int:
        if self.mode == 'delete-line':
            return 0
        return self.position

    def update(self, lines: List[str]):
        self.length_of_line = len(lines[self.line])


class Test:
    path: Path
    failures: List[str]
    proc_args: List[str | Path]
    assertions: List[Assertion]

    def __init__(self, path: Path):
        self.path = path
        self.failures = []
        self.assertions = []

    def parse(self):
        global expectations

        lines = []

        # Use binary mode, as we need to get byte offsets for the query
        # commands to send to the devtool.
        with open(self.path, 'rb') as file:
            full_file = file.read()
            for line_num, line in enumerate(full_file.splitlines()):
                # If this line is a comment, cut it off, to allow
                # commenting-out tests.
                # Don't bother with multi-line comments or '//' in strings,
                # "just don't do that" is fine since we're writing the tests.
                line = line.split(b'//')[0]

                # Find all the command strings
                while True:
                    assert_start = line.find('¬'.encode('utf-8'))
                    if assert_start == -1:
                        break

                    assert_end = line.find(b'!', assert_start)

                    # The exclamation mark itself is part of the command,
                    # so advance past it so we remove it.
                    assert_end += 1

                    # Grab the assertion text, and convert it to a string - we
                    # only kept it in bytes so we could get byte offsets from
                    # the line start.
                    assertion = line[assert_start:assert_end].decode('utf-8')

                    # Cut out the assertion
                    line = line[:assert_start] + line[assert_end:]

                    obj = Assertion(assertion, line_num, assert_start)
                    self.assertions.append(obj)

                lines.append(line)

        expectations += len(self.assertions)

        for assertion in self.assertions:
            assertion.update(lines)

        return True

    def run(self):
        # We can pass the test to the runner tool with the assertions in it,
        # and it'll remove them before parsing.
        self.proc_args = [TEST_EXE, self.path]

        proc = subprocess.Popen(
            self.proc_args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

        # If a test takes longer than five seconds, kill it.
        timed_out = [False]

        def kill_process(p):
            timed_out[0] = True
            p.kill()

        timer = threading.Timer(5, kill_process, [proc])

        # Feed in the commands that the assertions will validate
        command_input = ''
        for cmd in self.assertions:
            command_input += f'cmd:{cmd.line}.{cmd.cmd_position()}:{cmd.cmd_name()}:{cmd.get_command()}\n'

        if args.show_raw_output:
            print(f'\nCommand input for {get_test_name(self.path)}:')
            print(command_input.strip())

        try:
            timer.start()
            out, err = proc.communicate(command_input.encode('utf-8'))
        finally:
            timer.cancel()

        out = out.decode("utf-8").replace('\r\n', '\n')
        err = err.decode("utf-8").replace('\r\n', '\n')

        # Debug printing
        if args.show_raw_output:
            print(f'Command output for {get_test_name(self.path)}:')
            print(out)

        if timed_out[0]:
            self.failed('Timed out.')
        elif proc.returncode != 0:
            self.failed(f'Dev tool exited with non-zero return code: {proc.returncode} (error: {err.strip()})')
        elif err:
            self.failed(f'Got non-empty stderr: {err}')
        else:
            self.validate(out)

    def failed(self, message, *args):
        if args:
            message = message.format(*args)
        self.failures.append(message)

    def validate(self, out):
        lines = out.splitlines()

        # Remove all the debug information, that's fine if the user turns it on.
        lines = [line for line in lines if not line.startswith("[ide debug]")]

        # Line content-line number pairs.
        # self.expected_lines += obj.get_expectations()
        # expected_output = list([(line, num) for num, line in enumerate(self.expected_lines)])

        # We can't exactly match the output since the auto-completer might
        # suggest more stuff than the test expects, so split the output into
        # that for each assertion.
        command_prefix = '========== COMMAND'

        command_output: List[List[str]] = []
        current_output: List[str]

        for line in lines:
            # Recognise the start of the next command
            if line.startswith(command_prefix):
                current_output = [line]
                command_output.append(current_output)
                continue

            current_output.append(line)

        index = 0
        for output_lines in command_output:
            if index >= len(self.assertions):
                self.failed('Got output "{0}" when none was expected.', output_lines[0])
                index += 1
                continue

            assertion = self.assertions[index]
            failure_message = assertion.validate(output_lines)

            if failure_message is not None:
                self.failed('Assertion "{0}" error on line {1}, got error: {2}.',
                            assertion.mode, assertion.line, failure_message)

            index += 1

        while index < len(self.assertions):
            assertion = self.assertions[index]
            self.failed('Missing expected output for assertion {0} on line {1}.',
                        assertion.mode, assertion.line)
            index += 1


# Gets a test name from it's file, with forward-stroke path separators.
# For example, lib\wren-main\test\a\b\c.wren would be a/b/c.wren.
def get_test_name(path: Path) -> str:
    name = str(path.relative_to(TESTS_DIR))

    # Always use forward-stroke as the path separator, regardless of OS.
    return name.replace(os.sep, '/')


def colour_text(text, color):
    """Converts text to a string and wraps it in the ANSI escape sequence for
    colour, if supported."""

    # No ANSI escapes on Windows.
    if sys.platform == 'win32':
        return str(text)

    return color + str(text) + '\033[0m'


def green(text):
    return colour_text(text, '\033[32m')


def pink(text):
    return colour_text(text, '\033[91m')


def red(text):
    return colour_text(text, '\033[31m')


def yellow(text):
    return colour_text(text, '\033[33m')


def print_line(line=None, keep=False):
    erase = not args.static_output
    if erase:
        # Erase the line.
        print('\033[2K', end='')
        # Move the cursor to the beginning.
        print('\r', end='')

    if line:
        print(line, end='')
        sys.stdout.flush()

    if not erase or keep:
        # If we're not going to overwrite the same line, we need to write
        # each message on it's own line.
        print('')


def run_test(path: Path):
    global passed
    global failed
    global skipped

    if os.path.splitext(path)[1] != '.wren':
        return

    rel_path = get_test_name(path)

    # Check if we are just running a subset of the tests.
    if args.suite:
        if not rel_path.startswith(args.suite):
            return

    # Update the status line.
    print_line('Passed: {} Failed: {} Skipped: {} '.format(green(passed), red(failed), yellow(len(skipped))))

    # Read the test and parse out the expectations.
    test = Test(path)

    if not test.parse():
        # It's a skipped or non-test file.
        return

    test.run()

    cmdline_str = ' '.join([str(a) for a in test.proc_args])

    # Display the results.
    if len(test.failures) == 0:
        passed += 1
        if args.show_passes:
            print_line(green('PASS') + ': ' + rel_path, keep=True)
            if args.show_cmdline:
                print('      ' + cmdline_str)
    else:
        failed += 1
        print_line(red('FAIL') + ': ' + rel_path, keep=True)
        if args.show_cmdline:
            print('      ' + cmdline_str)
        for failure in test.failures:
            print('      ' + pink(failure))
        print('')


def walk(path: Path, callback, ignored=None):
    """
    Walks [dir], and executes [callback] on each file unless it is [ignored].
    """

    if not ignored:
        ignored = []
    ignored += ['.', '..']

    for file in [file for file in path.iterdir()]:
        if file.name in ignored:
            continue
        nfile: Path = path / file
        if nfile.is_dir():
            walk(nfile, callback)
        else:
            callback(nfile)


def main():
    global args

    parser = argparse.ArgumentParser()
    parser.add_argument('suite', nargs='?')
    parser.add_argument('--show-passes', '-p', action='store_true', help='list the tests that pass')
    parser.add_argument('--static-output', '-s', action='store_true', help="don't overwrite the status lines")
    parser.add_argument('--show-cmdline', '-c', action='store_true', help="show the command line used to run the test")
    parser.add_argument('--show-raw-output', '-r', action='store_true', help="show the stdout from the devtool")
    args = parser.parse_args(sys.argv[1:])

    walk(TESTS_DIR, run_test)

    print_line()
    if failed == 0:
        print('All ' + green(passed) + ' tests passed (' + str(expectations) + ' expectations)')
    else:
        print(green(passed) + ' tests passed. ' + red(failed) + ' tests failed.')

    # for key in sorted(skipped.keys()):
    #     print('Skipped ' + yellow(skipped[key]) + ' tests: ' + key)
    print('Skipped ' + yellow(len(skipped)) + ' tests.')

    if failed != 0:
        sys.exit(1)


if __name__ == '__main__':
    main()
