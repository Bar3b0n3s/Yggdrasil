"""Runs Python unittest suites and writes a JUnit report (Docs/Architecture.md §2.3 "Test.py ... --junit", §15.7).

Scripts/Test.py starts this file as a script with the interpreter a suite needs (the MCP bridge's tests need
Tools/MCP/.venv, where the MCP SDK is installed), once per suite run:

    <python> Scripts/Lib/unittest_runner.py --junit <file> --name <suite> [--filter <pattern>] <directory>...

Each directory's test_*.py modules are discovered with the directory as their top level, one JUnit <testsuite> per
directory named "<suite>.<directory path with dots>" ("Automation.Release.Tools.MCP.tests"). Progress (each test and
its outcome) goes to standard error. Python tests never skip (Docs/Decisions/0008-m4-decisions.md decision 16):
skipped tests, expected failures and unexpected successes are counted, and the exit code is 0 only when every test
passed and none was skipped; 1 when a test failed or errored, a module failed to import, or nothing ran; 4 when the
only problem is skipped tests (Test.py decides whether contract mode allows them).
"""

from __future__ import annotations

import argparse
import sys
import time
import traceback
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any

# Run as a script, this file's directory is sys.path[0]; the developer scripts' library modules there must not shadow
# the modules the tests import.
if sys.path and Path(sys.path[0]).resolve() == Path(__file__).resolve().parent:
    sys.path.pop(0)

EXIT_PASSED = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_SKIPPED = 4


class RecordingResult(unittest.TextTestResult):
    """A verbose text result that also records every outcome, with its duration, for the JUnit report."""

    def __init__(self, stream: Any, descriptions: bool, verbosity: int) -> None:
        super().__init__(stream, descriptions, verbosity)
        self.records: list[dict[str, Any]] = []
        self.started: dict[str, float] = {}

    def record(self, test: unittest.TestCase, outcome: str, message: str = "", detail: str = "") -> None:
        started = self.started.pop(test.id(), time.monotonic())
        self.records.append({"id": test.id(), "outcome": outcome, "message": message, "detail": detail,
                             "duration": time.monotonic() - started})

    def start_test(self, test: unittest.TestCase) -> None:
        self.started[test.id()] = time.monotonic()
        super().startTest(test)

    def add_success(self, test: unittest.TestCase) -> None:
        super().addSuccess(test)
        self.record(test, "passed")

    def add_failure(self, test: unittest.TestCase, error: Any) -> None:
        super().addFailure(test, error)
        self.record(test, "failure", _first_line(error[1]), "".join(traceback.format_exception(*error)))

    def add_error(self, test: unittest.TestCase, error: Any) -> None:
        super().addError(test, error)
        self.record(test, "error", _first_line(error[1]), "".join(traceback.format_exception(*error)))

    def add_skip(self, test: unittest.TestCase, reason: str) -> None:
        super().addSkip(test, reason)
        self.record(test, "skipped", reason)

    def add_expected_failure(self, test: unittest.TestCase, error: Any) -> None:
        super().addExpectedFailure(test, error)
        self.record(test, "skipped", "expected failure (Python tests never mark failures as expected)")

    def add_unexpected_success(self, test: unittest.TestCase) -> None:
        super().addUnexpectedSuccess(test)
        self.record(test, "failure", "unexpected success of a test marked as an expected failure")

    def add_sub_test(self, test: unittest.TestCase, subtest: unittest.TestCase, error: Any) -> None:
        super().addSubTest(test, subtest, error)
        if error is not None:
            outcome = "failure" if issubclass(error[0], test.failureException) else "error"
            started = self.started.get(test.id(), time.monotonic())
            self.records.append({"id": subtest.id(), "outcome": outcome, "message": _first_line(error[1]),
                                 "detail": "".join(traceback.format_exception(*error)),
                                 "duration": time.monotonic() - started})

    # unittest calls these names; the implementations above keep the scripts' snake_case naming.
    startTest = start_test
    addSuccess = add_success
    addFailure = add_failure
    addError = add_error
    addSkip = add_skip
    addExpectedFailure = add_expected_failure
    addUnexpectedSuccess = add_unexpected_success
    addSubTest = add_sub_test


def _first_line(error: BaseException) -> str:
    text = str(error).strip()
    return f"{type(error).__name__}: {text.splitlines()[0] if text else ''}"


def discover(directory: Path, patterns: str | None) -> unittest.TestSuite:
    """The test_*.py tests of `directory` (its own top level), filtered like unittest -k by comma-separated patterns."""
    loader = unittest.TestLoader()
    if patterns:
        loader.testNamePatterns = [pattern if "*" in pattern else f"*{pattern}*"
                                   for pattern in (part.strip() for part in patterns.split(",")) if pattern]
    return loader.discover(str(directory), pattern="test_*.py", top_level_dir=str(directory))


def suite_label(directory: Path) -> str:
    """The directory relative to the working directory with dots for slashes ("Tools.MCP.tests"), or its name."""
    try:
        relative = directory.resolve().relative_to(Path.cwd().resolve())
    except ValueError:
        return directory.name
    return ".".join(relative.parts) or directory.name


def write_junit(path: Path, groups: list[tuple[str, list[dict[str, Any]]]]) -> None:
    root = ElementTree.Element("testsuites")
    for name, records in groups:
        counts = {outcome: sum(1 for record in records if record["outcome"] == outcome)
                  for outcome in ("failure", "error", "skipped")}
        suite = ElementTree.SubElement(root, "testsuite", {
            "name": name, "tests": str(len(records)), "failures": str(counts["failure"]),
            "errors": str(counts["error"]), "skipped": str(counts["skipped"]),
            "time": f"{sum(record['duration'] for record in records):.3f}",
        })
        for record in records:
            module, _, test_name = record["id"].rpartition(".")
            case = ElementTree.SubElement(suite, "testcase", {"classname": f"{name}.{module}", "name": test_name,
                                                              "time": f"{record['duration']:.3f}"})
            if record["outcome"] in ("failure", "error", "skipped"):
                element = ElementTree.SubElement(case, record["outcome"], {"message": record["message"]})
                element.text = record["detail"] or None
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run unittest suites and write a JUnit report.")
    parser.add_argument("directories", nargs="+", type=Path, help="directories holding test_*.py modules")
    parser.add_argument("--junit", type=Path, required=True, help="the JUnit XML file to write")
    parser.add_argument("--name", required=True, help="the suite name prefix of the JUnit test suites")
    parser.add_argument("--filter", help="run only tests whose id matches one of these comma-separated patterns "
                                         "(unittest -k semantics)")
    arguments = parser.parse_args()
    groups: list[tuple[str, list[dict[str, Any]]]] = []
    failed = skipped = ran = 0
    for directory in arguments.directories:
        if not directory.is_dir():
            print(f"unittest_runner: {directory} is not a directory", file=sys.stderr)
            return EXIT_USAGE
        suite = discover(directory.resolve(), arguments.filter)
        runner = unittest.TextTestRunner(stream=sys.stderr, verbosity=2, resultclass=RecordingResult)
        print(f"== {directory.as_posix()}", file=sys.stderr, flush=True)
        result = runner.run(suite)
        if not isinstance(result, RecordingResult):
            raise TypeError("the runner did not use RecordingResult")
        groups.append((f"{arguments.name}.{suite_label(directory)}", result.records))
        ran += result.testsRun
        failed += sum(1 for record in result.records if record["outcome"] in ("failure", "error"))
        skipped += sum(1 for record in result.records if record["outcome"] == "skipped")
    write_junit(arguments.junit, groups)
    print(f"unittest_runner: {ran} test(s) ran, {failed} failed, {skipped} skipped", file=sys.stderr)
    if failed or ran == 0:
        return EXIT_FAILED
    return EXIT_SKIPPED if skipped else EXIT_PASSED


if __name__ == "__main__":
    sys.exit(main())
