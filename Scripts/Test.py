#!/usr/bin/env python3
"""Run the test suites (Docs/Architecture.md §2.3, §15).

Suites and the configurations the §15.8 matrix runs them in when --config is not given:
  unit         Debug, Release   the doctest executable bin/<OutputDir>/Tests/Tests, every test suite except GPU and
                                Golden (T0 static + T1 unit)
  gpu          Debug, Release   not available yet (Graphics foundation, M5)
  golden       Release          not available yet (M5)
  feature      Debug, Release   not available yet (FeatureTest, M14)
  automation   Release          the Python suites Tests/Automation and Tools/MCP/tests (§15.7) against the headless
                                editor bin/<OutputDir>/Editor/Editor, run by Scripts/Lib/unittest_runner.py with
                                Tools/MCP/.venv's interpreter (Scripts/Setup.py creates it; the bridge's tests need the
                                MCP SDK), then the method coverage gate (§15.6 gate 5, Roadmap M4)
  determinism  Debug, Release   not available yet (M14)
  games        Release          not available yet (M16-M18)
A suite that does not exist yet is reported with the distinct status "not-available"; it neither passes nor fails.

Skipped test cases (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): after a unit run, the Tests binary lists
the test cases the run's filters select, skipped ones included (doctest --no-skip --list-test-cases --reporters=xml
--out=<file>; each test case carries its suite and its doctest::skip flag). The only test cases that may stay skipped
are the child-process targets of the ChildTargets suite (Test::ChildTargetSuite): a skipped test case in any other
suite, such as a contract task's not yet implemented test, fails the run and is named. --allow-skips (contract mode,
only for the commit of a milestone's contract task: PreCommit.py --contract) reports them without failing. The counts
are in the step detail and, with --json, in the step's "skipCheck" object.

The automation suite (Roadmap M4, Docs/Decisions/0008-m4-decisions.md decisions 33 and 34; its skip rule is decision 16):
  - The tests start editors themselves (Tests/Automation/harness.py, the bridge's launcher); ENGINE_AUTOMATION_CONFIG
    names the configuration whose editor they use. A missing editor build or virtual environment fails the run (exit
    code 3); Python tests never skip, so a skipped test fails it too (--allow-skips reports them instead).
  - Method coverage: every engine_client call is appended to the file ENGINE_AUTOMATION_COVERAGE names. After the run,
    `Editor --headless --renderer none --dump-reference <dir>` lists the registered methods (Methods.json leaves out the
    debug.* test hooks), and the run fails naming every registered method no Python test called. With --filter the
    gate is not checked, because a partial run cannot cover every method.
  - --filter takes comma-separated unittest -k patterns on test ids ("test_batch", "*Launcher*,test_client").

--junit writes bin/TestResults/<Suite>-<Config>.xml (JUnit; test suite named "<Suite>.<Config>"; the automation suite
has one test suite per test directory and one for the coverage gate). The console then shows a summary and every
failure parsed from that file instead of doctest's console reporter. If the test process dies before writing it, a
JUnit file with one error test case records the crash.

Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested suites exists
yet, 2 usage error, 3 a test executable or the MCP virtual environment is missing (build it, or run Setup.py),
5 timeout.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Test.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import datetime
import json
import os
import re
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any

from Lib import paths
from Lib.process import (
    ToolNotFoundError,
    child_environment,
    describe_exit_code,
    format_command,
    run_captured,
    run_streamed,
)
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
# The doctest suite of the test cases that only run in a child process spawned by another test: Test::ChildTargetSuite
# (Tests/Source/Support/TestOptions.h). They are the only test cases that stay skipped (Docs/Decisions/
# 0003-m1-contract-decisions.md decision 7).
CHILD_TARGET_SUITE = "ChildTargets"
LISTING_TIMEOUT_SECONDS = 120.0
UNEXPECTED_SKIPS_IN_DETAIL = 3

# The automation suite (§15.7): its test directories, the runner, and the environment the tests read.
AUTOMATION_TEST_DIRECTORIES = (paths.REPOSITORY_ROOT / "Tests" / "Automation",
                               paths.REPOSITORY_ROOT / "Tools" / "MCP" / "tests")
UNITTEST_RUNNER = paths.REPOSITORY_ROOT / "Scripts" / "Lib" / "unittest_runner.py"
MCP_VENV = paths.REPOSITORY_ROOT / "Tools" / "MCP" / ".venv"
RUNNER_EXIT_SKIPPED = 4  # unittest_runner.py: every test passed, but some were skipped
AUTOMATION_CONFIG_VARIABLE = "ENGINE_AUTOMATION_CONFIG"
AUTOMATION_COVERAGE_VARIABLE = "ENGINE_AUTOMATION_COVERAGE"
# Settings of the bridge and the tests that a developer's environment must not leak into the suite.
AUTOMATION_CLEARED_VARIABLES = ("ENGINE_MCP_TRANSCRIPT", "ENGINE_MCP_USER_DATA_DIR")
DUMP_REFERENCE_TIMEOUT_SECONDS = 300.0
COVERAGE_CASE_NAME = "Method coverage: every registered method has at least one Python test"
UNCOVERED_IN_DETAIL = 5


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
        Suite("automation", ("Release",), True),
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


class SkipCheckError(Exception):
    """The Tests binary could not list its test cases."""


@dataclasses.dataclass
class SkipCheck:
    """The test cases a run's filters select that doctest::skip leaves out, by name."""

    skipped: list[str]
    child_targets: list[str]  # the selected test cases of the ChildTargets suite, skipped or not
    unexpected: list[str]  # skipped test cases outside the ChildTargets suite

    def to_json(self, allowed: bool) -> dict[str, Any]:
        return {"skipped": len(self.skipped), "childTargets": len(self.child_targets),
                "unexpected": self.unexpected, "allowed": allowed}


def list_skipped_test_cases(executable: Path, filters: list[str]) -> SkipCheck:
    """Ask the Tests binary which test cases `filters` select, skipped ones included (--no-skip). doctest's XML
    reporter gives each <TestCase> its suite and its doctest::skip flag; the listing goes to a file (--out), so the log
    on stderr never interleaves with it."""
    with tempfile.TemporaryDirectory(prefix="Test-SkipCheck-") as directory:
        listing = Path(directory) / "TestCases.xml"
        command = [str(executable), *filters, "--no-skip", "--list-test-cases", "--reporters=xml", f"--out={listing}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=LISTING_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            raise SkipCheckError(str(error)) from error
        if not result.succeeded:
            raise SkipCheckError(f"{format_command(command)} {result.describe_exit()}: {result.tail(5)}")
        try:
            cases = list(ElementTree.parse(listing).getroot().iter("TestCase"))
        except (OSError, ElementTree.ParseError) as error:
            raise SkipCheckError(f"cannot read the test case listing of {format_command(command)}: {error}") from error
    skipped = sorted(case.get("name", "") for case in cases if case.get("skipped") == "true")
    child_targets = sorted(case.get("name", "") for case in cases if case.get("testsuite") == CHILD_TARGET_SUITE)
    unexpected = sorted(case.get("name", "") for case in cases
                        if case.get("skipped") == "true" and case.get("testsuite") != CHILD_TARGET_SUITE)
    return SkipCheck(skipped, child_targets, unexpected)


def describe_unexpected_skips(skips: SkipCheck) -> str:
    names = ", ".join(f"'{name}'" for name in skips.unexpected[:UNEXPECTED_SKIPS_IN_DETAIL])
    more = len(skips.unexpected) - UNEXPECTED_SKIPS_IN_DETAIL
    return names + (f" and {more} more" if more > 0 else "")


def tests_executable(config: str) -> Path:
    return paths.output_directory(config) / "Tests" / paths.executable_name("Tests")


def junit_path(suite: str, config: str) -> Path:
    return paths.TEST_RESULTS_ROOT / f"{suite.capitalize()}-{config}.xml"


def read_junit(path: Path, suite_name: str | None) -> TestCounts:
    """Count a JUnit file's test cases and, with `suite_name`, rename its test suites (doctest names its suite after the
    executable's path)."""
    tree = ElementTree.parse(path)
    root = tree.getroot()
    counts = TestCounts()
    for test_suite in root.iter("testsuite"):
        if suite_name is not None:
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
    filters = [f"--test-suite-exclude={UNIT_EXCLUDED_TEST_SUITES}"]
    if arguments.filter:
        filters.append(f"--test-case={arguments.filter}")
    command = [str(executable), *filters]
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

    skips: SkipCheck | None = None
    skip_error = ""
    try:
        skips = list_skipped_test_cases(executable, filters)
        data["skipCheck"] = skips.to_json(arguments.allow_skips)
        if not arguments.allow_skips:
            for skipped_case in skips.unexpected:
                console.print(f"UNEXPECTED SKIP: {skipped_case}")
    except SkipCheckError as error:
        skip_error = f"cannot list the skipped test cases: {error}"
        data["skipCheck"] = {"error": skip_error, "allowed": arguments.allow_skips}
        console.print(skip_error)

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
    passed = f"{counts.passed} test case(s) passed"
    if skips is None:
        if arguments.allow_skips:
            return Step(name, Status.PASSED, f"{passed}; {skip_error} (--allow-skips)", result.duration, data=data)
        return Step(name, Status.FAILED, skip_error, result.duration, data=data)
    child_targets = f"{len(skips.child_targets)} child-process target(s) in the {CHILD_TARGET_SUITE} suite"
    if skips.unexpected and not arguments.allow_skips:
        return Step(name, Status.FAILED, f"{len(skips.unexpected)} skipped test case(s) outside the "
                                         f"{CHILD_TARGET_SUITE} suite: {describe_unexpected_skips(skips)}; only the "
                                         f"{child_targets} may stay skipped (Roadmap rule 3; --allow-skips is for a "
                                         f"contract task's commit)", result.duration, data=data)
    if arguments.allow_skips:
        detail = (f"{passed}; contract mode (--allow-skips): {len(skips.skipped)} skipped, {len(skips.unexpected)} "
                  f"of them outside the {CHILD_TARGET_SUITE} suite; {child_targets}")
    elif skips.skipped:
        detail = f"{passed}; {len(skips.skipped)} skipped, all of them among the {child_targets}"
    else:
        detail = f"{passed}; none skipped"
    return Step(name, Status.PASSED, detail, result.duration, data=data)


def editor_executable(config: str) -> Path:
    return paths.output_directory(config) / "Editor" / paths.executable_name("Editor")


def venv_interpreter() -> Path:
    return MCP_VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


class CoverageError(Exception):
    """The registered methods could not be listed."""


def registered_methods(editor: Path) -> list[str]:
    """Every registered method except the test hooks, from the editor's own catalogue (--dump-reference writes
    Methods.json without the debug.* hooks, ADR 0008 decision 9)."""
    with tempfile.TemporaryDirectory(prefix="Test-Reference-") as directory:
        output = Path(directory) / "Reference"
        command = [str(editor), "--headless", "--renderer", "none", "--dump-reference", str(output),
                   f"--user-data-dir={Path(directory) / 'UserData'}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=DUMP_REFERENCE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            raise CoverageError(str(error)) from error
        if not result.succeeded:
            raise CoverageError(f"{editor.name} --dump-reference {result.describe_exit()}: {result.tail(3)}")
        try:
            catalog = json.loads((output / "Methods.json").read_text(encoding="utf-8"))
            names = sorted(str(method["name"]) for method in catalog["Methods"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise CoverageError(f"cannot read the method catalogue of {format_command(command)}: {error}") from error
    if not names:
        raise CoverageError(f"{paths.display_path(editor)} registers no methods")
    return names


def called_methods(coverage_file: Path) -> set[str]:
    """The methods the tests called (engine_client.CallLog appends one name per line)."""
    try:
        return {line.strip() for line in coverage_file.read_text(encoding="utf-8").splitlines() if line.strip()}
    except FileNotFoundError:
        return set()


def append_coverage_junit(report: Path, suite_name: str, uncovered: list[str], registered: int) -> None:
    """Adds the coverage gate to the automation JUnit file as a test suite of its own."""
    tree = ElementTree.parse(report)
    root = tree.getroot()
    suite = ElementTree.SubElement(root, "testsuite", {"name": f"{suite_name}.coverage", "tests": "1",
                                                       "failures": "1" if uncovered else "0", "errors": "0",
                                                       "skipped": "0", "time": "0"})
    case = ElementTree.SubElement(suite, "testcase", {"classname": f"{suite_name}.coverage",
                                                      "name": COVERAGE_CASE_NAME, "time": "0"})
    if uncovered:
        failure = ElementTree.SubElement(case, "failure", {
            "message": f"{len(uncovered)} of {registered} registered method(s) have no Python test"})
        failure.text = "\n".join(uncovered)
    ElementTree.indent(root, space="\t")
    tree.write(report, encoding="utf-8", xml_declaration=True)


def run_automation(config: str, arguments: argparse.Namespace, console: Console) -> Step:
    name = f"automation {config}"
    editor = editor_executable(config)
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                         f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    interpreter = venv_interpreter()
    if not interpreter.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(interpreter)} not found: run python Scripts/Setup.py "
                                         f"(it creates the MCP bridge's virtual environment the suite runs in)",
                    exit_code=EXIT_INIT_FAILED)
    suite_name = f"Automation.{config}"
    report = junit_path("automation", config) if arguments.junit else None
    data: dict[str, object] = {"suite": "automation", "config": config, "editor": paths.display_path(editor)}

    with tempfile.TemporaryDirectory(prefix="Test-Automation-") as directory:
        coverage_file = Path(directory) / "CalledMethods.txt"
        runner_report = report if report is not None else Path(directory) / "Automation.xml"
        runner_report.parent.mkdir(parents=True, exist_ok=True)
        runner_report.unlink(missing_ok=True)
        command = [str(interpreter), str(UNITTEST_RUNNER), "--junit", str(runner_report), "--name", suite_name,
                   *(["--filter", arguments.filter] if arguments.filter else []),
                   *(str(test_directory) for test_directory in AUTOMATION_TEST_DIRECTORIES)]
        environment = child_environment({AUTOMATION_CONFIG_VARIABLE: config,
                                         AUTOMATION_COVERAGE_VARIABLE: str(coverage_file)})
        for variable in AUTOMATION_CLEARED_VARIABLES:
            environment.pop(variable, None)
        console.heading(f"automation ({config}): {paths.display_path(editor)}")
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment, timeout=arguments.timeout,
                                  echo=console.stream)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)

        counts: TestCounts | None = None
        try:
            counts = read_junit(runner_report, None)
        except (OSError, ElementTree.ParseError):
            write_crash_junit(runner_report, suite_name, f"the automation suite {result.describe_exit()} without a "
                                                         f"JUnit report", result.tail(60))
        if report is not None:
            data["junit"] = paths.display_path(report)
        if counts is not None:
            data.update({"testCases": counts.executed, "passed": counts.passed, "failed": counts.failed,
                         "skipped": counts.skipped})
            if counts.failures:
                # The runner already streamed each traceback; the summary names the test and its message.
                summaries = [failure.split(" Traceback (most recent call last):", 1)[0].splitlines()[0]
                             for failure in counts.failures]
                data["failures"] = summaries
                for summary in summaries:
                    console.print(f"FAILED: {summary}")
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"the automation suite {result.describe_exit()}", result.duration,
                        data=data)

        coverage_detail = "method coverage not checked (--filter selects part of the suite)"
        uncovered: list[str] = []
        coverage_failure = ""
        if not arguments.filter:
            try:
                registered = registered_methods(editor)
                uncovered = sorted(set(registered) - called_methods(coverage_file))
                data["coverage"] = {"registered": len(registered), "uncovered": uncovered}
                coverage_detail = f"method coverage {len(registered) - len(uncovered)}/{len(registered)}"
                if report is not None and counts is not None:
                    append_coverage_junit(report, suite_name, uncovered, len(registered))
                for method in uncovered:
                    console.print(f"UNCOVERED METHOD: {method}")
                if uncovered:
                    shown = ", ".join(uncovered[:UNCOVERED_IN_DETAIL])
                    more = len(uncovered) - UNCOVERED_IN_DETAIL
                    coverage_failure = (f"{len(uncovered)} registered method(s) have no Python test: {shown}"
                                        + (f" and {more} more" if more > 0 else ""))
            except CoverageError as error:
                coverage_failure = f"cannot check method coverage: {error}"
                data["coverage"] = {"error": str(error)}

    skips_allowed = result.exit_code == RUNNER_EXIT_SKIPPED and arguments.allow_skips
    if result.exit_code not in (0, RUNNER_EXIT_SKIPPED) or counts is None:
        if counts is not None and counts.failed:
            detail = f"{counts.failed} of {counts.executed} test(s) failed"
        else:
            detail = f"the automation suite exited with code {describe_exit_code(result.exit_code)}"
        if coverage_failure:
            detail += f"; {coverage_failure}"
        return Step(name, Status.FAILED, detail, result.duration, data=data)
    if result.exit_code == RUNNER_EXIT_SKIPPED and not skips_allowed:
        return Step(name, Status.FAILED, f"{counts.skipped} test(s) skipped: Python tests never skip (a missing editor "
                                         f"build or virtual environment is a failure; --allow-skips is contract "
                                         f"mode)", result.duration, data=data)
    if coverage_failure:
        return Step(name, Status.FAILED, coverage_failure, result.duration, data=data)
    skipped = f"; contract mode (--allow-skips): {counts.skipped} skipped" if skips_allowed else ""
    return Step(name, Status.PASSED, f"{counts.passed} test(s) passed{skipped}; {coverage_detail}", result.duration,
                data=data)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run test suites in the configurations of the Architecture §15.8 matrix.",
        epilog="Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested "
               "suites exists yet, 2 usage error, 3 a test executable or the MCP virtual environment is missing, 5 "
               "timeout.",
    )
    parser.add_argument("--suite", default="all",
                        help=f"comma-separated suites: {', '.join(SUITES)} or all (default: all)")
    parser.add_argument("--config", help="comma-separated configurations, overriding the matrix (Debug, Release; "
                                         "test executables do not exist in Dist)")
    parser.add_argument("--junit", action="store_true", help="write JUnit XML into bin/TestResults/")
    parser.add_argument("--update-golden", action="store_true", help="write golden image candidates (golden suite)")
    parser.add_argument("--filter", help="run only test cases matching this pattern (unit: a doctest wildcard pattern "
                                         "on test case names; automation: comma-separated unittest -k patterns on "
                                         "test ids)")
    parser.add_argument("--allow-skips", action="store_true",
                        help="contract mode, only for the commit of a milestone's contract task (Roadmap rule 3; "
                             "PreCommit.py --contract): report test cases skipped outside the "
                             f"{CHILD_TARGET_SUITE} suite (unit) and skipped Python tests (automation) instead of "
                             "failing")
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
        runner = run_automation if name == "automation" else run_unit
        for config in arguments.configs or suite.configurations:
            step = runner(config, arguments, console)
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
