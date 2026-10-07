#!/usr/bin/env python3
"""Run the test suites (Docs/Architecture.md §2.3, §15).

Suites and the configurations the §15.8 matrix runs them in when --config is not given:
  unit         Debug, Release   the doctest executable bin/<OutputDir>/Tests/Tests, every test suite except GPU and
                                Golden (T0 static + T1 unit)
  gpu          Debug, Release   the GPU test cases (doctest suite GPU, T2), validation and synchronization validation
                                on, run twice per configuration: under the device's API version (1.4 where available)
                                and capped with --vulkan-api=1.3, so both paths of every Vulkan 1.4 capability run
                                (§8.1, §15.3); --vulkan-api restricts the run to one cap
  golden       Release          the golden images (doctest suite Golden, §15.4), compared with
                                Tests/Golden/<DeviceClass>/
  feature      Debug, Release   not available yet (FeatureTest, M14)
  automation   Release          not available yet (automation core, M4)
  determinism  Debug, Release   not available yet (M14)
  games        Release          not available yet (M16-M18)
A suite that does not exist yet is reported with the distinct status "not-available"; it neither passes nor fails.

GPU and golden runs (Docs/Decisions/0009-m5-decisions.md decision 14): a test case without a usable Vulkan device passes
after logging that it found none, unless --require-gpu is given (CI.py and PreCommit.py pass it unless they get
--gpu-optional), which makes it fail. Such a run ends with the status "warning" and the number of test cases that found
no device. A gpu run under the 1.4 cap also ends with "warning" when host-image-copy cases were skipped (the device or a
format lacks the 1.4 path, §8.1); under the 1.3 cap those skips are expected and only reported in the detail. The
golden run reports the device class that rendered, read from the harness's log lines
(Tests/Source/Support/GoldenImage.cpp), and ends with "warning" when goldens are missing for that device class (smoke
mode) or when --update-golden wrote candidates into Tests/Golden/<DeviceClass>/, which must be reviewed (the git diff)
before they are committed. A failed comparison writes <Name>-actual.png, <Name>-expected.png and <Name>-diff.png into
bin/TestResults/Golden/.

Skipped test cases (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): after every run, the Tests binary lists
the test cases the run's filters select, skipped ones included (doctest --no-skip --list-test-cases --reporters=xml
--out=<file>; each test case carries its suite and its doctest::skip flag). The only test cases that may stay skipped
are the child-process targets of the ChildTargets suite (Test::ChildTargetSuite): a skipped test case in any other
suite, such as a contract task's not yet implemented test, fails the run and is named. --allow-skips (contract mode,
only for the commit of a milestone's contract task: PreCommit.py --contract) reports them without failing. The counts
are in the step detail and, with --json, in the step's "skipCheck" object.

--junit writes bin/TestResults/<Suite>-<Config>[-<Cap>].xml (JUnit; test suite named "<Suite>.<Config>[.<Cap>]", where
<Cap> is Vulkan13 or Vulkan14 for the gpu suite). The console then shows a summary and every failure parsed from that
file instead of doctest's console reporter. If the test process dies before writing it, a JUnit file with one error
test case records the crash.

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

SUITE_TIMEOUT_SECONDS = 1800.0
# The doctest suites of the device-dependent stages (Tests/Source/Support/HeadlessGpuFixture.h: Test::GpuSuite and
# Test::GoldenSuite). The unit stage runs every other suite (§15.8).
GPU_TEST_SUITE = "GPU"
GOLDEN_TEST_SUITE = "Golden"
UNIT_EXCLUDED_TEST_SUITES = f"{GPU_TEST_SUITE},{GOLDEN_TEST_SUITE}"
DOCTEST_SUMMARY_PATTERN = re.compile(
    r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped"
)
# The doctest suite of the test cases that only run in a child process spawned by another test: Test::ChildTargetSuite
# (Tests/Source/Support/TestOptions.h). They are the only test cases that stay skipped (Docs/Decisions/
# 0003-m1-contract-decisions.md decision 7).
CHILD_TARGET_SUITE = "ChildTargets"
LISTING_TIMEOUT_SECONDS = 120.0
UNEXPECTED_SKIPS_IN_DETAIL = 3
NAMES_IN_DETAIL = 4

# The Vulkan API caps of the gpu suite (§8.1): the Tests option --vulkan-api=<cap>. 1.4 is also the cap of a run
# without the option.
VULKAN_API_CAPS = ("1.4", "1.3")
# The cap whose runs must exercise the Vulkan 1.4 paths: a skipped host-image-copy case there is a warning.
FULL_API_CAP = "1.4"
# The log line Test::ReportGpuUnavailable writes for a GPU test case without a device (HeadlessGpuFixture.cpp).
NO_DEVICE_PATTERN = re.compile(r"GPU test without a device \(passes without running\): (?P<reason>.*)$")
# The log line of a host-image-copy case the device cannot run (HostImageUploadTests.cpp, ReportHostCopySkipped).
HOST_COPY_SKIP_PATTERN = re.compile(r"Host image copy skipped for (?P<case>'[^']*' \([^)]*\)): (?P<skip_reason>.*)$")
# The log lines of the golden harness (GoldenImage.cpp): "Golden image '<name>' on device class '<class>'" followed by
# ": matched", ": goldens missing", ": wrote the candidate" or " does not match".
GOLDEN_PATTERN = re.compile(
    r"Golden image '(?P<name>[^']+)' on device class '(?P<device>[^']+)'"
    r"(?::\s(?P<event>matched|goldens missing|wrote the candidate)|\s(?P<mismatch>does not match))"
)
REPORTED_LINES_PATTERN = re.compile(
    f"{NO_DEVICE_PATTERN.pattern}|{HOST_COPY_SKIP_PATTERN.pattern}|{GOLDEN_PATTERN.pattern}"
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
        Suite("gpu", ("Debug", "Release"), True),
        Suite("golden", ("Release",), True),
        Suite("feature", ("Debug", "Release"), False, "M14 (FeatureTest)"),
        Suite("automation", ("Release",), False, "M4 (automation core)"),
        Suite("determinism", ("Debug", "Release"), False, "M14 (determinism stage)"),
        Suite("games", ("Release",), False, "M16-M18 (demo games)"),
    )
}


@dataclasses.dataclass(frozen=True)
class TestRun:
    """One run of the Tests executable: a suite in a configuration, with the options of that run."""

    suite: str
    config: str
    filters: tuple[str, ...]  # the doctest filters that select the suite's test cases
    options: tuple[str, ...] = ()  # Tests options of the run (--vulkan-api=1.3, --require-gpu, --update-golden)
    cap: str = ""  # the gpu suite's API cap ("1.3" or "1.4")

    @property
    def name(self) -> str:
        return f"{self.suite} {self.config}" + (f" (Vulkan {self.cap})" if self.cap else "")

    @property
    def report_stem(self) -> str:
        return "-".join([self.suite.capitalize(), self.config, *([cap_label(self.cap)] if self.cap else [])])

    @property
    def junit_suite_name(self) -> str:
        return ".".join([self.suite.capitalize(), self.config, *([cap_label(self.cap)] if self.cap else [])])


def cap_label(cap: str) -> str:
    return "Vulkan" + cap.replace(".", "")


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


@dataclasses.dataclass
class DeviceReport:
    """What a gpu or golden run's log says about the device: test cases without one, and the golden images."""

    no_device: list[str] = dataclasses.field(default_factory=list)  # the reasons, one per test case
    host_copy_skips: list[str] = dataclasses.field(default_factory=list)  # "<case>: <reason>", one per skipped case
    device_classes: set[str] = dataclasses.field(default_factory=set)
    matched: set[str] = dataclasses.field(default_factory=set)
    goldens_missing: set[str] = dataclasses.field(default_factory=set)
    candidates_written: set[str] = dataclasses.field(default_factory=set)
    mismatched: set[str] = dataclasses.field(default_factory=set)

    @staticmethod
    def parse(lines: list[str]) -> DeviceReport:
        report = DeviceReport()
        for line in lines:
            no_device = NO_DEVICE_PATTERN.search(line)
            if no_device:
                # A test case reports once in the log; doctest's console reporter repeats the message in other words.
                report.no_device.append(no_device.group("reason").strip())
                continue
            host_copy_skip = HOST_COPY_SKIP_PATTERN.search(line)
            if host_copy_skip:
                skip_reason = host_copy_skip.group("skip_reason").strip()
                report.host_copy_skips.append(f"{host_copy_skip.group('case')}: {skip_reason}")
                continue
            golden = GOLDEN_PATTERN.search(line)
            if not golden:
                continue
            report.device_classes.add(golden.group("device"))
            name = golden.group("name")
            if golden.group("mismatch"):
                report.mismatched.add(name)
            elif golden.group("event") == "matched":
                report.matched.add(name)
            elif golden.group("event") == "goldens missing":
                report.goldens_missing.add(name)
            else:
                report.candidates_written.add(name)
        return report

    def to_json(self) -> dict[str, Any]:
        return {
            "noDevice": len(self.no_device),
            "hostCopySkips": self.host_copy_skips,
            "deviceClasses": sorted(self.device_classes),
            "matched": sorted(self.matched),
            "goldensMissing": sorted(self.goldens_missing),
            "candidatesWritten": sorted(self.candidates_written),
            "mismatched": sorted(self.mismatched),
        }

    def describe_host_copy_skips(self) -> str:
        return f"host image copy skipped for {len(self.host_copy_skips)} case(s) ({self.host_copy_skips[0]})"

    def notes(self, require_gpu: bool, cap: str) -> list[str]:
        """The things a reader must look at even though the run passed."""
        notes: list[str] = []
        if self.no_device and not require_gpu:
            notes.append(f"{len(self.no_device)} test case(s) found no GPU device and passed without running "
                         f"({self.no_device[0]}); --require-gpu fails them instead")
        if self.host_copy_skips and cap == FULL_API_CAP:
            notes.append(f"{self.describe_host_copy_skips()}: the Vulkan 1.4 path did not run there")
        classes = ", ".join(sorted(self.device_classes))
        if self.goldens_missing:
            notes.append(f"goldens missing on device class {classes} for {describe_names(self.goldens_missing)} "
                         f"(smoke mode, never a pass of the comparison)")
        if self.candidates_written:
            directories = ", ".join(f"Tests/Golden/{device}/" for device in sorted(self.device_classes))
            notes.append(f"--update-golden wrote {len(self.candidates_written)} candidate(s) into {directories}: "
                         f"review the diff before committing")
        return notes


def describe_names(names: set[str] | list[str]) -> str:
    ordered = sorted(names)
    shown = ", ".join(ordered[:NAMES_IN_DETAIL])
    more = len(ordered) - NAMES_IN_DETAIL
    return shown + (f" and {more} more" if more > 0 else "")


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


def junit_path(run: TestRun) -> Path:
    return paths.TEST_RESULTS_ROOT / f"{run.report_stem}.xml"


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


def plan_runs(suite: str, config: str, arguments: argparse.Namespace) -> list[TestRun]:
    """The Tests runs of `suite` in `config`: one, except for the gpu suite, which runs once per API cap."""
    gpu_options = ("--require-gpu",) if arguments.require_gpu else ()
    if suite == "unit":
        return [TestRun(suite, config, (f"--test-suite-exclude={UNIT_EXCLUDED_TEST_SUITES}",))]
    if suite == "gpu":
        caps = [arguments.vulkan_api] if arguments.vulkan_api else list(VULKAN_API_CAPS)
        return [TestRun(suite, config, (f"--test-suite={GPU_TEST_SUITE}",), (f"--vulkan-api={cap}", *gpu_options), cap)
                for cap in caps]
    if suite == "golden":
        golden_options = ("--update-golden",) if arguments.update_golden else ()
        return [TestRun(suite, config, (f"--test-suite={GOLDEN_TEST_SUITE}",), (*gpu_options, *golden_options))]
    raise ValueError(f"no runs for suite '{suite}'")


def run_tests(run: TestRun, arguments: argparse.Namespace, console: Console) -> Step:
    executable = tests_executable(run.config)
    if not executable.is_file():
        return Step(run.name, Status.FAILED, f"{paths.display_path(executable)} not found: run python Scripts/Build.py "
                                             f"--config {run.config} --project Tests", exit_code=EXIT_INIT_FAILED)
    filters = list(run.filters)
    if arguments.filter:
        filters.append(f"--test-case={arguments.filter}")
    command = [str(executable), *filters, *run.options]
    report = junit_path(run) if arguments.junit else None
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        command += ["--reporters=junit", f"--out={report}"]

    console.heading(f"{run.name}: {format_command([paths.display_path(executable), *filters, *run.options])}")
    try:
        result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(), timeout=arguments.timeout,
                              echo=console.stream, collect=REPORTED_LINES_PATTERN)
    except ToolNotFoundError as error:
        return Step(run.name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)

    data: dict[str, object] = {"suite": run.suite, "config": run.config, "executable": paths.display_path(executable)}
    if run.cap:
        data["vulkanApi"] = run.cap
    counts: TestCounts | None = None
    if report is not None:
        data["junit"] = paths.display_path(report)
        try:
            counts = read_junit(report, run.junit_suite_name)
        except (OSError, ElementTree.ParseError):
            write_crash_junit(report, run.junit_suite_name, f"Tests {result.describe_exit()} without a JUnit report",
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

    # Only the device suites report on the device: the unit suite's own harness tests check golden images of a device
    # class no machine has, on the CPU.
    device = DeviceReport.parse(result.collected) if run.suite != "unit" else DeviceReport()
    if run.suite != "unit":
        data["device"] = device.to_json()

    if result.timed_out:
        if report is not None:
            write_crash_junit(report, run.junit_suite_name, f"Tests {result.describe_exit()}", result.tail(60))
        return Step(run.name, Status.TIMEOUT, f"Tests {result.describe_exit()}", result.duration, data=data)

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
        if device.mismatched:
            detail += f"; golden mismatches: {describe_names(device.mismatched)} (outputs in bin/TestResults/Golden/)"
        return Step(run.name, Status.FAILED, detail, result.duration, data=data)
    if counts is None:
        return Step(run.name, Status.FAILED, "Tests exited with code 0 but printed no doctest summary", result.duration,
                    data=data)
    if counts.executed == 0:
        return Step(run.name, Status.FAILED, "no test case ran" + (" (check --filter)" if arguments.filter else ""),
                    result.duration, data=data)
    passed = f"{counts.passed} test case(s) passed"
    if skips is None:
        if arguments.allow_skips:
            return Step(run.name, Status.PASSED, f"{passed}; {skip_error} (--allow-skips)", result.duration, data=data)
        return Step(run.name, Status.FAILED, skip_error, result.duration, data=data)
    child_targets = f"{len(skips.child_targets)} child-process target(s) in the {CHILD_TARGET_SUITE} suite"
    if skips.unexpected and not arguments.allow_skips:
        return Step(run.name, Status.FAILED, f"{len(skips.unexpected)} skipped test case(s) outside the "
                                             f"{CHILD_TARGET_SUITE} suite: {describe_unexpected_skips(skips)}; only "
                                             f"the {child_targets} may stay skipped (Roadmap rule 3; --allow-skips is "
                                             f"for a contract task's commit)", result.duration, data=data)
    if arguments.allow_skips:
        detail = (f"{passed}; contract mode (--allow-skips): {len(skips.skipped)} skipped, {len(skips.unexpected)} "
                  f"of them outside the {CHILD_TARGET_SUITE} suite; {child_targets}")
    elif skips.skipped:
        detail = f"{passed}; {len(skips.skipped)} skipped, all of them among the {child_targets}"
    else:
        detail = f"{passed}; none skipped"
    if device.matched:
        detail += (f"; {len(device.matched)} golden image(s) matched on device class "
                   f"{', '.join(sorted(device.device_classes))}")
    if device.host_copy_skips and run.cap != FULL_API_CAP:
        detail += f"; {device.describe_host_copy_skips()}, as expected under this cap"
    notes = device.notes(arguments.require_gpu, run.cap)
    if notes:
        return Step(run.name, Status.WARNING, f"{detail}; {'; '.join(notes)}", result.duration, data=data)
    return Step(run.name, Status.PASSED, detail, result.duration, data=data)


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
    parser.add_argument("--require-gpu", action="store_true",
                        help="gpu and golden suites: a test case without a usable Vulkan device fails instead of "
                             "passing without running (CI.py and PreCommit.py pass it unless --gpu-optional, §15.2)")
    parser.add_argument("--vulkan-api", choices=VULKAN_API_CAPS,
                        help="gpu suite: run under this API cap only (default: once under each, §8.1)")
    parser.add_argument("--update-golden", action="store_true",
                        help="golden suite: write each image as the golden candidate of this machine's device class "
                             "into Tests/Golden/<DeviceClass>/ instead of comparing; review the diff before committing")
    parser.add_argument("--filter", help="run only test cases matching this doctest wildcard pattern")
    parser.add_argument("--allow-skips", action="store_true",
                        help="contract mode, only for the commit of a milestone's contract task (Roadmap rule 3; "
                             "PreCommit.py --contract): report test cases skipped outside the "
                             f"{CHILD_TARGET_SUITE} suite instead of failing the run")
    parser.add_argument("--timeout", type=float, default=SUITE_TIMEOUT_SECONDS,
                        help=f"seconds per Tests run (default: {SUITE_TIMEOUT_SECONDS:.0f})")
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
    if arguments.update_golden and "golden" not in arguments.suites:
        parser.error("--update-golden needs the golden suite (--suite golden)")
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
            for run in plan_runs(name, config, arguments):
                step = run_tests(run, arguments, console)
                steps.append(step)
                console.result(step)

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
