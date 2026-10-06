#!/usr/bin/env python3
"""Run the test suites (Docs/Architecture.md §2.3, §15).

Suites and the configurations the §15.8 matrix runs them in when --config is not given:
  unit         Debug, Release   the doctest executable bin/<OutputDir>/Tests/Tests, every test suite except GPU and
                                Golden (T0 static + T1 unit)
  gpu          Debug, Release   not available yet (Graphics foundation, M5)
  golden       Release          not available yet (M5)
  feature      Debug, Release   not available yet (FeatureTest, M14)
  automation   Release          not available yet (automation core, M4)
  determinism  Debug, Release   not available yet (M14)
  games        Release          not available yet (M16-M18)
A suite that does not exist yet is reported with the distinct status "not-available"; it neither passes nor fails.

--junit writes bin/TestResults/<Suite>-<Config>.xml (JUnit; test suite named "<Suite>.<Config>"). The console then
shows a summary and every failure parsed from that file instead of doctest's console reporter. If the test process
dies before writing it, a JUnit file with one error test case records the crash.

Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested suites exists
yet, 2 usage error, 3 a test executable is missing (build it first), 5 timeout.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Test.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import datetime
import re
import xml.etree.ElementTree as ElementTree
from pathlib import Path

from Lib import paths
from Lib.process import ToolNotFoundError, child_environment, describe_exit_code, run_streamed
from Lib.report import (
    EXIT_FAILED,
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
)

UNIT_TIMEOUT_SECONDS = 1800.0
# The unit stage runs every doctest suite except the device-dependent ones, which have their own stages (§15.8).
UNIT_EXCLUDED_TEST_SUITES = "GPU,Golden"
DOCTEST_SUMMARY_PATTERN = re.compile(
    r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped"
)


@dataclasses.dataclass(frozen=True)
class Suite:
    name: str
    configurations: tuple[str, ...]  # the §15.8 matrix
    available: bool
    planned: str = ""  # where an unavailable suite lands (Docs/Roadmap.md)


SUITES = {
    suite.name: suite
    for suite in (
        Suite("unit", ("Debug", "Release"), True),
        Suite("gpu", ("Debug", "Release"), False, "M5 (Graphics foundation)"),
        Suite("golden", ("Release",), False, "M5 (golden comparator)"),
        Suite("feature", ("Debug", "Release"), False, "M14 (FeatureTest)"),
        Suite("automation", ("Release",), False, "M4 (automation core)"),
        Suite("determinism", ("Debug", "Release"), False, "M14 (determinism stage)"),
        Suite("games", ("Release",), False, "M16-M18 (demo games)"),
    )
}


@dataclasses.dataclass
class TestCounts:
    executed: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    failures: list[str] = dataclasses.field(default_factory=list)


def tests_executable(config: str) -> Path:
    return paths.output_directory(config) / "Tests" / paths.executable_name("Tests")


def junit_path(suite: str, config: str) -> Path:
    return paths.TEST_RESULTS_ROOT / f"{suite.capitalize()}-{config}.xml"


def read_junit(path: Path, suite_name: str) -> TestCounts:
    """Count doctest's JUnit test cases and rename its test suite (doctest names it after the executable's path)."""
    tree = ElementTree.parse(path)
    root = tree.getroot()
    counts = TestCounts()
    for test_suite in root.iter("testsuite"):
        test_suite.set("name", suite_name)
    for case in root.iter("testcase"):
        problems = case.findall("failure") + case.findall("error")
        if case.find("skipped") is not None or case.get("status", "run") not in ("run", ""):
            counts.skipped += 1
            continue
        counts.executed += 1
        if problems:
            counts.failed += 1
            for problem in problems:
                message = problem.get("message", "") or ""
                text = (problem.text or "").strip()
                counts.failures.append(f"{case.get('name')} ({case.get('classname')}): {message} {text}".strip())
        else:
            counts.passed += 1
    tree.write(path, encoding="utf-8", xml_declaration=True)
    return counts


def write_crash_junit(path: Path, suite_name: str, message: str, output_tail: str) -> None:
    """A JUnit file for a run that produced none (crash, timeout), so CI result viewers show the failure."""
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    root = ElementTree.Element("testsuites")
    suite = ElementTree.SubElement(
        root, "testsuite",
        {"name": suite_name, "tests": "1", "failures": "0", "errors": "1", "skipped": "0", "timestamp": timestamp},
    )
    case = ElementTree.SubElement(suite, "testcase", {"classname": suite_name, "name": "test process"})
    ElementTree.SubElement(case, "error", {"message": message}).text = output_tail
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def run_unit(config: str, arguments: argparse.Namespace, console: Console) -> Step:
    name = f"unit {config}"
    executable = tests_executable(config)
    if not executable.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(executable)} not found: run python Scripts/Build.py "
                                         f"--config {config} --project Tests", exit_code=EXIT_INIT_FAILED)
    command = [str(executable), f"--test-suite-exclude={UNIT_EXCLUDED_TEST_SUITES}"]
    if arguments.filter:
        command.append(f"--test-case={arguments.filter}")
    report = junit_path("unit", config) if arguments.junit else None
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        command += ["--reporters=junit", f"--out={report}"]

    console.heading(f"unit ({config}): {paths.display_path(executable)}")
    try:
        result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(), timeout=arguments.timeout,
                              echo=console.stream)
    except ToolNotFoundError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)

    data: dict[str, object] = {"suite": "unit", "config": config, "executable": paths.display_path(executable)}
    counts: TestCounts | None = None
    if report is not None:
        data["junit"] = paths.display_path(report)
        try:
            counts = read_junit(report, f"Unit.{config}")
        except (OSError, ElementTree.ParseError):
            write_crash_junit(report, f"Unit.{config}", f"Tests {result.describe_exit()} without a JUnit report",
                              result.tail(60))
    else:
        summary = DOCTEST_SUMMARY_PATTERN.findall(result.output)
        if summary:
            # doctest's "test cases: N" counts the executed cases; filtered-out ones are reported as skipped.
            executed, passed, failed, skipped = (int(value) for value in summary[-1])
            counts = TestCounts(executed, passed, failed, skipped)

    if counts is not None:
        data.update({"testCases": counts.executed, "passed": counts.passed, "failed": counts.failed,
                     "skipped": counts.skipped})
        if counts.failures:
            data["failures"] = counts.failures
            for failure in counts.failures:
                console.print(f"FAILED: {failure}")

    if result.timed_out:
        if report is not None:
            write_crash_junit(report, f"Unit.{config}", f"Tests {result.describe_exit()}", result.tail(60))
        return Step(name, Status.TIMEOUT, f"Tests {result.describe_exit()}", result.duration, data=data)
    if result.exit_code != 0:
        if counts is not None and counts.failed:
            detail = f"{counts.failed} of {counts.executed} test case(s) failed"
        else:
            detail = f"Tests exited with code {describe_exit_code(result.exit_code)}"
        return Step(name, Status.FAILED, detail, result.duration, data=data)
    if counts is None:
        return Step(name, Status.FAILED, "Tests exited with code 0 but printed no doctest summary", result.duration,
                    data=data)
    if counts.executed == 0:
        return Step(name, Status.FAILED, "no test case ran" + (" (check --filter)" if arguments.filter else ""),
                    result.duration, data=data)
    skipped = f", {counts.skipped} skipped" if counts.skipped else ""
    return Step(name, Status.PASSED, f"{counts.passed} test case(s) passed{skipped}", result.duration, data=data)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run test suites in the configurations of the Architecture §15.8 matrix.",
        epilog="Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested "
               "suites exists yet, 2 usage error, 3 a test executable is missing, 5 timeout.",
    )
    parser.add_argument("--suite", default="all",
                        help=f"comma-separated suites: {', '.join(SUITES)} or all (default: all)")
    parser.add_argument("--config", help="comma-separated configurations, overriding the matrix (Debug, Release; "
                                         "test executables do not exist in Dist)")
    parser.add_argument("--junit", action="store_true", help="write JUnit XML into bin/TestResults/")
    parser.add_argument("--update-golden", action="store_true", help="write golden image candidates (golden suite)")
    parser.add_argument("--filter", help="run only test cases matching this doctest wildcard pattern")
    parser.add_argument("--timeout", type=float, default=UNIT_TIMEOUT_SECONDS,
                        help=f"seconds per suite run (default: {UNIT_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    try:
        arguments.suites = paths.parse_name_list(arguments.suite, tuple(SUITES), "suite")
        arguments.configs = paths.parse_configurations(arguments.config, ("Debug", "Release")) if arguments.config \
            else None
    except ValueError as error:
        parser.error(str(error))
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    try:
        paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"Test.py: error: {error}")
        return EXIT_USAGE

    steps: list[Step] = []
    for name in arguments.suites:
        suite = SUITES[name]
        if not suite.available:
            step = Step(name, Status.NOT_AVAILABLE, f"the {name} suite does not exist yet; planned for {suite.planned}",
                        data={"suite": name})
            steps.append(step)
            console.result(step)
            continue
        for config in arguments.configs or suite.configurations:
            step = run_unit(config, arguments, console)
            steps.append(step)
            console.result(step)
    if arguments.update_golden and not SUITES["golden"].available:
        console.print("note: --update-golden has no effect, the golden suite does not exist yet")

    ran = [step for step in steps if step.status != Status.NOT_AVAILABLE]
    exit_code = overall_exit_code(steps)
    if not ran:
        console.print("Test.py: none of the requested suites exists yet, so nothing was tested")
        exit_code = EXIT_FAILED
    console.summary("Test summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "steps": [step.to_json() for step in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
