#!/usr/bin/env python
# Copied and modified from upstream Wren
# See it's copyright file for more information.

from __future__ import print_function

import os.path
import random
import subprocess
from argparse import ArgumentParser
from collections import defaultdict
import re
from subprocess import Popen, PIPE
import sys
import threading
from pathlib import Path
import platform
import tempfile
from typing import Optional, Set, Dict, List

# Runs the tests.

parser = ArgumentParser()
parser.add_argument('--suffix', default='')
parser.add_argument('suite', nargs='?')
parser.add_argument('--show-passes', '-p', action='store_true', help='list the tests that pass')
parser.add_argument('--static-output', '-s', action='store_true', help="don't overwrite the status lines")
parser.add_argument('--show-cmdline', '-c', action='store_true', help="show the command line used to build the test")
parser.add_argument('--num-threads', '-j', type=int, default=-1,
                    help="The number of tests to compile in parallel, -1 for auto")

args = parser.parse_args(sys.argv[1:])

config = args.suffix.lstrip('_d')
is_debug = args.suffix.startswith('_d')

max_threads = args.num_threads
if max_threads == -1:
    # Default to the number of available CPU cores.
    # See the os.cpu_count docs.
    max_threads = len(os.sched_getaffinity(0))

WRENCC_DIR = Path(__file__).parent.parent
WREN_DIR: Path = WRENCC_DIR / "lib" / "wren-main"
WREN_APP: Path = WRENCC_DIR / "build" / f"wrencc{args.suffix}"
API_TEST_RUNNER: Path = WRENCC_DIR / "build" / "test" / "api-test-runner"

WREN_APP_WITH_EXT = WREN_APP
if platform.system() == "Windows":
    WREN_APP_WITH_EXT += ".exe"

if not WREN_APP_WITH_EXT.is_file():
    print("The binary file 'wrencc' was not found, expected it to be at " + str(WREN_APP.absolute()))
    print("In order to run the tests, you need to build the compiler first!")
    sys.exit(1)

# print("Wren Test Directory - " + WREN_DIR)
# print("Wren Test App - " + WREN_APP)

# While we're using multiple threads, only one
# of them will be allowed to do anything other
# than running the compiler or executable at a time.
# This mutex is locked when running a test, and
# released before and re-acquired after running
# the compiler or executable.
WORKING_LOCK = threading.Lock()

# The threads semaphore is used to enforce the limit
# of the maximum number of parallel tests.
THREADS_SEM = threading.BoundedSemaphore(max_threads)

EXPECT_PATTERN = re.compile(r'// expect: ?(.*)')
EXPECT_ERROR_PATTERN = re.compile(r'// expect error(?! line)')
EXPECT_ERROR_LINE_PATTERN = re.compile(r'// expect error line (\d+)')
EXPECT_RUNTIME_ERROR_PATTERN = re.compile(r'// expect (handled )?runtime error: (.+)')
ERROR_PATTERN = re.compile(r'WrenCC at [^:]*:(\d+) - Error')
STACK_TRACE_PATTERN = re.compile(r'(?:\[\./)?test/.* line (\d+)] in')
STDIN_PATTERN = re.compile(r'// stdin: (.*)')
SKIP_PATTERN = re.compile(r'// skip: (.*)')
NONTEST_PATTERN = re.compile(r'// nontest')

# Used to figure out which modules need compiling
IMPORT_PATTERN = re.compile(r'^\s*import\s+"([^"]*)"')

IGNORED_ERR_LINES = {"Parse errors found, aborting"}

# This lists tests that should succeed even if they're supposed to fail
OVERRIDE_SHOULD_SUCCEED: Set[str] = set()
OVERRIDE_SHOULD_FAIL_COMPILATION: Dict[str, str] = dict()

# This is the list of tests that are known to currently fail, so we can differentiate tests
# that are failing due to regressions and those that need to be delt with for the first time.
EXPECTED_FAILURES: Set[str] = set()

# This changes the expected error messages for failures
EDIT_ERROR_MESSAGES: Dict[str, str] = dict()

passed = 0
expected_failed_passed = []
expected_failed = 0
failed = 0
num_skipped = 0
skipped = defaultdict(int)
expectations = 0

threads_in_progress: List[threading.Thread] = []


class Test:
    compiler_args: Optional[List[str | Path]]

    def __init__(self, path: Path):
        self.path = path
        self.name = str(self.path.relative_to(WREN_DIR))  # eg test/language/something.wren
        self.output = []
        self.compile_errors = set()
        self.runtime_error_line = 0
        self.runtime_error_message = None
        self.compile_error_expected = False
        self.runtime_error_status_expected = False
        self.linking_error_expected = False
        self.input_bytes = None
        self.failures = []
        self.compiler_args = None
        self.expected_to_fail = self.name in EXPECTED_FAILURES
        self.use_api_runner = False
        self.ignore_output = False

    def parse(self):
        global num_skipped
        global skipped
        global expectations

        input_lines = []
        line_num = 1

        # Check if this test is supposed to succeed or fail differently in wrencc
        override_success = self.name in OVERRIDE_SHOULD_SUCCEED
        override_compile_time_fail = OVERRIDE_SHOULD_FAIL_COMPILATION.get(self.name, None)
        edit_error_messages = EDIT_ERROR_MESSAGES.get(self.name, None)

        # Note #1: we have unicode tests that require utf-8 decoding.
        # Note #2: python `open` on 3.x modifies contents regarding newlines.
        # To prevent this, we specify newline='' and we don't use the
        # readlines/splitlines/etc family of functions, these
        # employ the universal newlines concept which does this.
        # We have tests that embed \r and \r\n for validation, all of which
        # get manipulated in a not helpful way by these APIs.

        local_expectations = 0
        with open(self.path, 'r', encoding="utf-8", newline='', errors='replace') as file:
            data = file.read()
            lines = re.split('\n|\r\n', data)
            for line in lines:
                if len(line) <= 0:
                    line_num += 1
                    continue

                match = EXPECT_PATTERN.search(line)
                if match:
                    self.output.append((match.group(1), line_num))
                    local_expectations += 1

                match = EXPECT_ERROR_PATTERN.search(line)
                if match:
                    self.compile_errors.add(line_num)

                    self.compile_error_expected = True
                    local_expectations += 1

                match = EXPECT_ERROR_LINE_PATTERN.search(line)
                if match:
                    self.compile_errors.add(int(match.group(1)))

                    self.compile_error_expected = True
                    local_expectations += 1

                match = EXPECT_RUNTIME_ERROR_PATTERN.search(line)
                if match:
                    self.runtime_error_line = line_num
                    self.runtime_error_message = match.group(2)
                    # If the runtime error isn't handled, it should exit with WREN_EX_SOFTWARE.
                    if match.group(1) != "handled ":
                        self.runtime_error_status_expected = True
                    local_expectations += 1

                match = STDIN_PATTERN.search(line)
                if match:
                    input_lines.append(match.group(1))

                match = SKIP_PATTERN.search(line)
                if match:
                    num_skipped += 1
                    skipped[match.group(1)] += 1
                    return False

                # Not a test file at all, so ignore it.
                match = NONTEST_PATTERN.search(line)
                if match:
                    return False

                line_num += 1

        if override_success:
            self.compile_error_expected = False
            self.compile_errors.clear()
            self.runtime_error_status_expected = False
            self.runtime_error_message = None
            local_expectations = 0
            # The test might generate output that it doesn't have expects for, since
            # it was intended to fail.
            self.ignore_output = True
        if override_compile_time_fail == "linking":
            self.linking_error_expected = True
            self.compile_error_expected = True
        elif override_compile_time_fail:
            error_lines = [int(s.strip()) for s in override_compile_time_fail.split(',')]
            self.compile_errors = set(error_lines)
            self.compile_error_expected = True
            local_expectations = len(self.compile_errors)

        if edit_error_messages:
            for msg_block in edit_error_messages.split(",,,"):  # Three commas to allow their use in error messages
                parts = msg_block.split("=", 1)
                line = parts[0]
                msg = parts[1]

                if line == "runtime":
                    self.runtime_error_message = msg
                else:
                    # Add compile errors here if required
                    raise Exception(f"Invalid edit-error-message line '{line}'")

        expectations += local_expectations

        # If any input is fed to the test in stdin, concatenate it into one string.
        if input_lines:
            self.input_bytes = "\n".join(input_lines).encode("utf-8")

        # If we got here, it's a valid test.
        return True

    def run(self, compiler_path: Path, type: str):
        # These are obviously the Windows filenames, but they
        # also work just fine on Linux.
        extension = "exe"
        if self.use_api_runner:
            extension = "dll"

        # Auto-delete the file when we're done with it
        tmp_name = "wrencc-test-%08x.%s" % (int(random.random() * 0xffffffff), extension)
        dest_file = Path(tempfile.gettempdir()) / tmp_name
        try:
            self._run_impl(compiler_path, type, dest_file)
        finally:
            if dest_file.exists():
                os.remove(dest_file)

    def _run_impl(self, compiler_path: Path, type: str, testprog_path: Path):
        success = self.compile(compiler_path, testprog_path)

        if not success:
            # TODO handle errors like before
            return

        # Invoke wren and run the test.
        # For normal tests we just run the executable directly, but C API tests
        # go through a separate runner executable.
        if self.use_api_runner:
            proc_args = [API_TEST_RUNNER, testprog_path, self.path]
        else:
            proc_args = [testprog_path]
        proc = Popen(proc_args, stdin=PIPE, stdout=PIPE, stderr=PIPE)

        # If a test takes longer than five seconds, kill it.
        #
        # This is mainly useful for running the tests while stress testing the GC,
        # which can make a few pathological tests much slower.
        timed_out = [False]

        def kill_process(p):
            timed_out[0] = True
            p.kill()

        timer = threading.Timer(5, kill_process, [proc])

        try:
            WORKING_LOCK.release()
            timer.start()
            out, err = proc.communicate(self.input_bytes)
        finally:
            timer.cancel()
            WORKING_LOCK.acquire()

        if timed_out[0]:
            self.failed("Timed out.")
        else:
            self.validate(type == "example", proc.returncode, out, err)

    def compile(self, cc: Path, dest_file: Path) -> bool:
        # Build the arguments
        self.compiler_args = [
            cc,
            "-o", dest_file,
            "--module=test",
            self.path,
        ]

        # If this is an API test, build it as a shared library so we can test
        # it against the C code.
        if self.use_api_runner:
            self.compiler_args.append("--output-type=shared")

        # Add each of the modules we're using, including transitive dependencies
        transitive_deps = self.resolve_module_dependencies()
        for module in transitive_deps:
            mod_name = str(module.relative_to(self.path.parent)).removesuffix('.wren')
            self.compiler_args.append("--module=" + mod_name)
            self.compiler_args.append(module)

        # Run the compiler
        proc = subprocess.Popen(
            stdin=subprocess.DEVNULL,
            stdout=PIPE,
            stderr=PIPE,
            encoding="utf-8",
            args=self.compiler_args,
        )

        # Allow other stuff to happen in Python while the compiler is running
        try:
            WORKING_LOCK.release()
            out, err = proc.communicate(None)
        finally:
            WORKING_LOCK.acquire()

        # This should only happen if the compiler crashes - in which case we shouldn't
        # be worrying about individual lines.
        if proc.returncode < 0:
            self.failed("Compiler exited with negative return code " + str(proc.returncode))
            print()
            print("Compiler stdout: " + out.strip())
            print("Compiler stderr: " + err.strip())
            return False

        # It's probably bad form, but we want to be really sure the compiler never writes to stdout
        if out:
            raise Exception(f"Compiler wrote to stdout for test {self.path}: {out}")

        lines = err.replace('\r\n', '\n').split('\n')
        self.validate_compile_errors(lines)

        compile_failed = proc.returncode != 0
        if compile_failed and not self.compile_error_expected:
            self.failed("Compiler exited with non-zero return code")
        if not compile_failed and self.compile_error_expected:
            self.failed("Compiler succeeded when it was expected to fail")

        if proc.returncode != 0:
            return False
        return True

    def validate(self, is_example, exit_code, out, err):
        if self.compile_errors and self.runtime_error_message:
            self.failed("Test error: Cannot expect both compile and runtime errors.")
            return

        try:
            out = out.decode("utf-8").replace('\r\n', '\n')
            err = err.decode("utf-8").replace('\r\n', '\n')
        except:
            self.failed('Error decoding output.')
            return

        error_lines = err.split('\n')

        # If there is no output, consider that to be no lines rather than a single blank one
        if error_lines == ['']:
            error_lines = []

        # Validate that an expected runtime error occurred.
        if self.runtime_error_message:
            self.validate_runtime_error(error_lines)

        self.validate_exit_code(exit_code, error_lines)

        # Ignore output from examples.
        if is_example: return

        self.validate_output(out)

    def validate_runtime_error(self, error_lines):
        if len(error_lines) < 1:
            self.failed('Expected runtime error "{0}" and got none.',
                        self.runtime_error_message)
            return

        # Skip any compile errors. This can happen if there is a compile error in
        # a module loaded by the module being tested.
        line = 0
        while ERROR_PATTERN.search(error_lines[line]):
            line += 1

        if error_lines[line] != self.runtime_error_message:
            self.failed('Expected runtime error "{0}" and got:',
                        self.runtime_error_message)
            self.failed(error_lines[line])

        # Skip the stack trace handling, since we don't actually have them yet.
        # TODO re-enable once stacktraces are supported
        if True:
            return

        # Make sure the stack trace has the right line. Skip over any lines that
        # come from builtin libraries.
        match = False
        stack_lines = error_lines[line + 1:]
        for stack_line in stack_lines:
            match = STACK_TRACE_PATTERN.search(stack_line)
            if match: break

        if not match:
            self.failed('Expected stack trace and got:')
            for stack_line in stack_lines:
                self.failed(stack_line)
        else:
            stack_line = int(match.group(1))
            if stack_line != self.runtime_error_line:
                self.failed('Expected runtime error on line {0} but was on line {1}.',
                            self.runtime_error_line, stack_line)

    def validate_compile_errors(self, error_lines):
        # Validate that every compile error was expected.
        found_errors = set()
        found_link_error = False
        for line in error_lines:
            if line in IGNORED_ERR_LINES:
                continue

            if "Programme ld.gold failed with status code" in line:
                found_link_error = True

            if self.linking_error_expected:
                # If a linker error is expected, ignore everything since
                # the linker output will be dumped here.
                continue

            match = ERROR_PATTERN.search(line)
            if match:
                error_line = float(match.group(1))
                if error_line in self.compile_errors:
                    found_errors.add(error_line)
                else:
                    self.failed('Unexpected error:')
                    self.failed(line)
            elif line != '':
                self.failed('Unexpected output on stderr:')
                self.failed(line)

        # Validate that every expected error occurred.
        for line in self.compile_errors - found_errors:
            self.failed('Missing expected error on line {0}.', line)

        if self.linking_error_expected and not found_link_error:
            self.failed('Missing expected link error message')

        # We don't need to check found_linker_error is false if it's
        # not supposed to be there, as that error would be caught
        # using the normal system.

    def validate_exit_code(self, exit_code, error_lines):
        error_status = exit_code != 0
        if error_status == self.runtime_error_status_expected:
            return

        self.failed('Expecting non-zero return code? {0}. Got {1}. Stderr:',
                    self.runtime_error_status_expected, exit_code)
        self.failures += error_lines

    def validate_output(self, out):
        # Skip validating the output for tests that are expected
        # to fail in Wren but succeed here.
        if self.ignore_output:
            return

        # Remove the trailing last empty line.
        out_lines = out.split('\n')
        if out_lines[-1] == '':
            del out_lines[-1]

        index = 0
        for line in out_lines:
            if sys.version_info < (3, 0):
                line = line.encode('utf-8')

            if index >= len(self.output):
                self.failed('Got output "{0}" when none was expected.', line)
            elif self.output[index][0] != line:
                self.failed('Expected output "{0}" on line {1} and got "{2}".',
                            self.output[index][0], self.output[index][1], line)
            index += 1

        while index < len(self.output):
            self.failed('Missing expected output "{0}" on line {1}.',
                        self.output[index][0], self.output[index][1])
            index += 1

    def failed(self, message, *args):
        if args:
            message = message.format(*args)
        self.failures.append(message)

    @staticmethod
    def resolve_module_path(current: Path, module_name: str) -> Path:
        path = current.parent / f"{module_name}.wren"

        # Get rid of any /../ stuff
        return path.resolve()

    def resolve_module_dependencies(self) -> Set[Path]:
        # Modules may reference us - that's fine, just make sure we're using the same path as resolve_module_path
        # results in.
        self_path = self.path.resolve()

        # In case we have any transitive dependencies on modules we didn't directly import, figure that
        # out now by scanning those modules we did import.
        # Note we deal with everything as a path, since modules can use relative imports.
        scanned = set()
        remaining = {self_path}
        while len(remaining) > 0:
            # Take an arbitrary module we haven't looked at yet, and find it's imports:
            target_module = remaining.pop()
            imports = scan_for_module_imports(target_module)
            import_paths = [Test.resolve_module_path(target_module, name) for name in imports]

            # Ignore imports of files that don't exist
            import_paths = [path for path in import_paths if path.exists()]

            # Now mark this module as scanned
            scanned.add(target_module)

            # We'll need to investigate any modules it found
            remaining.update(import_paths)

            # And remove any scanned modules from the remaining list, otherwise we could scan the same module twice
            remaining.difference_update(scanned)

        # Remove ourselves, since this is a list of dependencies
        scanned.remove(self_path)

        return scanned


def color_text(text, color):
    """Converts text to a string and wraps it in the ANSI escape sequence for
    color, if supported."""

    # No ANSI escapes on Windows.
    if sys.platform == 'win32':
        return str(text)

    return color + str(text) + '\033[0m'


def green(text):
    return color_text(text, '\033[32m')


def pink(text):
    return color_text(text, '\033[91m')


def red(text):
    return color_text(text, '\033[31m')


def yellow(text):
    return color_text(text, '\033[33m')


def walk(path: Path, callback, ignored=None):
    """
    Walks [dir], and executes [callback] on each file unless it is [ignored].
    """

    if not ignored:
        ignored = []
    ignored += [".", ".."]

    for file in [file for file in path.iterdir()]:
        if file.name in ignored:
            continue
        nfile: Path = path / file
        if nfile.is_dir():
            walk(nfile, callback)
        else:
            run_parallel(callback, nfile, file.name)


def run_parallel(task, arg, name: str):
    def run():
        try:
            assert WORKING_LOCK.acquire()
            task(arg)
        finally:
            threads_in_progress.remove(thread)
            WORKING_LOCK.release()
            THREADS_SEM.release()

    assert THREADS_SEM.acquire()
    thread = threading.Thread(name="worker-" + name, target=run)
    threads_in_progress.append(thread)
    thread.start()


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
        print("")


def run_script(app, path: Path, type):
    global passed
    global expected_failed
    global failed
    global num_skipped

    if os.path.splitext(path)[1] != '.wren':
        return

    rel_path = str(path.relative_to(WREN_DIR))

    # Check if we are just running a subset of the tests.
    if args.suite:
        if not rel_path.startswith(args.suite):
            return

    # Update the status line.
    print_line('({}) Passed: {} Failed: {} XFailed: {} Skipped: {} '.format(
        os.path.relpath(app, WREN_DIR), green(passed), red(failed), yellow(expected_failed), yellow(num_skipped)))

    # Make a nice short path relative to the working directory.

    # Read the test and parse out the expectations.
    test = Test(path)

    # If this is testing the C API, build the Wren file as a shared object and
    # run our testing executable to load it and run the test C code.
    if type == "api test":
        test.use_api_runner = True

    if not test.parse():
        # It's a skipped or non-test file.
        return

    test.run(app, type)

    cmdline_str = " ".join([str(a) for a in test.compiler_args])

    # Display the results.
    if len(test.failures) == 0:
        passed += 1
        if args.show_passes:
            print_line(green('PASS') + ': ' + rel_path, keep=True)
            if args.show_cmdline:
                print('      ' + cmdline_str)

        if test.expected_to_fail:
            print_line(
                red(f"The test {rel_path} was expected to fail, but passed! Please update expected-failures.txt."),
                keep=True)
            expected_failed_passed.append(test.name)
    elif test.expected_to_fail:
        expected_failed += 1
        # Expected to fail, so don't show the full details
        print_line(yellow('XFAIL') + ': ' + rel_path, keep=True)
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


def run_test(path, example=False):
    run_script(WREN_APP, path, "test")


def run_api_test(path):
    run_script(WREN_APP, path, "api test")


def run_example(path: Path):
    rel = str(path.relative_to(WREN_DIR))

    # Don't run examples that require user input.
    if "animals" in rel: return
    if "guess_number" in rel: return

    # This one is annoyingly slow.
    if "skynet" in rel: return

    run_script(WREN_APP, path, "example")


def load_list(path: Path) -> Dict[str, Optional[str]]:
    items = dict()
    with open(path, 'r') as fi:
        for line in fi:
            line = line.split('#')[0]  # Comments
            line = line.strip()
            if not line:
                continue

            parts = line.split(':', 1)
            key = parts[0].strip()
            value = parts[1].strip() if len(parts) == 2 else None
            items[key] = value

    return items


def scan_for_module_imports(path: Path) -> Set[str]:
    imports = set()

    with open(path, 'r', encoding="utf-8", newline='', errors='replace') as file:
        data = file.read()
        lines = re.split('\n|\r\n', data)
        for line in lines:
            match = IMPORT_PATTERN.search(line)
            if match:
                imports.add(match.group(1))

    return imports


def main():
    # Load the override list
    OVERRIDE_SHOULD_SUCCEED.update(load_list(WRENCC_DIR / 'test' / 'should-succeed.txt'))
    OVERRIDE_SHOULD_FAIL_COMPILATION.update(load_list(WRENCC_DIR / 'test' / 'should-fail-compilation.txt'))
    EXPECTED_FAILURES.update(load_list(WRENCC_DIR / 'test' / 'expected-failures.txt'))
    EDIT_ERROR_MESSAGES.update(load_list(WRENCC_DIR / 'test' / 'edit-error-message.txt'))

    walk(WREN_DIR / 'test', run_test, ignored=['api', 'benchmark'])
    walk(WREN_DIR / 'test' / 'api', run_api_test)
    walk(WREN_DIR / 'example', run_example)

    # Wait for any remaining threads to finish
    while True:
        with WORKING_LOCK:
            if len(threads_in_progress) == 0:
                break
            waiting_on = threads_in_progress[0]
        waiting_on.join()

    print_line()
    if failed == 0 and expected_failed == 0:
        print('All ' + green(passed) + ' tests passed (' + str(expectations) + ' expectations)')
    else:
        print(green(passed) + ' tests passed. ' + yellow(expected_failed) + ' tests failed expectedly. ' + red(
            failed) + ' tests failed.')

    for key in sorted(skipped.keys()):
        print('Skipped ' + yellow(skipped[key]) + ' tests: ' + key)

    # We print a warning for each test, but re-print them here to make it super obvious
    if len(expected_failed_passed) != 0:
        print(red("The following tests were expected to fail, but passed! Please update expected-failures.txt,"))
        print(red(" to make sure someone notices if they are later broken!"))
        for name in sorted(expected_failed_passed):
            print("   " + name)

    if failed != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
