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
  feature      Debug, Release   FeatureTest in editor mode (§15.5, §15.6): Editor --headless --project
                                Projects/FeatureTest --run-tests, then the six per-mode coverage gates of its report and
                                a byte check that the run left Projects/FeatureTest unchanged outside Library/
  automation   Release          the Python suites Tests/Automation and Tools/MCP/tests (§15.7) against the headless
                                editor bin/<OutputDir>/Editor/Editor, run by Scripts/Lib/unittest_runner.py with
                                Tools/MCP/.venv's interpreter (Scripts/Setup.py creates it; the bridge's tests need the
                                MCP SDK), then the method coverage gate (§15.6 gate 5, Roadmap M4)
  determinism  Debug, Release   every Testing.Replays file of FeatureTest verified with input.replay
                                {verify, strictHash} in the --renderer none editor of each configuration (§15.1 T3d):
                                one step over all requested configurations, which compares their hashes (M15 adds the
                                exported runs)
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

Skipped test cases (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): after every run of the Tests binary
(unit, gpu, golden), it lists the test cases the run's filters select, skipped ones included (doctest --no-skip
--list-test-cases --reporters=xml --out=<file>; each test case carries its suite and its doctest::skip flag). The only
test cases that may stay skipped are the child-process targets of the ChildTargets suite (Test::ChildTargetSuite): a
skipped test case in any other suite, such as a contract task's not yet implemented test, fails the run and is named.
--allow-skips (contract mode, only for the commit of a milestone's contract task: PreCommit.py --contract) reports them
without failing. The counts are in the step detail and, with --json, in the step's "skipCheck" object.

The automation suite (Roadmap M4, Docs/Decisions/0008-m4-decisions.md decisions 33 and 34; its skip rule is
decision 16):
  - The tests start editors themselves (Tests/Automation/harness.py, the bridge's launcher); ENGINE_AUTOMATION_CONFIG
    names the configuration whose editor they use. A missing editor build or virtual environment fails the run (exit
    code 3); Python tests never skip, so a skipped test fails it too (--allow-skips reports them instead).
  - Method coverage: every engine_client call is appended to the file ENGINE_AUTOMATION_COVERAGE names. After the run,
    `Editor --headless --renderer none --dump-reference <dir>` lists the registered methods (Methods.json leaves out the
    debug.* test hooks), and the run fails naming every registered method no Python test called. With --filter the
    gate is not checked, because a partial run cannot cover every method.
  - --filter takes comma-separated unittest -k patterns on test ids ("test_batch", "*Launcher*,test_client").
  - Tests that need a rendering editor (the screenshot methods, Docs/Decisions/0009-m5-decisions.md decision 33) probe
    for a Vulkan device first (Tests/Automation/harness.py, require_gpu). Without one they print the GPU suite's
    no-device line and pass without running, which ends the run with the status "warning" like a gpu run; with
    --require-gpu (ENGINE_AUTOMATION_REQUIRE_GPU=1 for the tests) they fail instead.

The feature suite (Docs/Decisions/0022-m14-contract.md decision 7) runs `Editor --headless --project
Projects/FeatureTest --run-tests --audio-device none --expect-no-gpu-errors --junit <report>` per configuration, with
--user-data-dir in a temporary directory. With --junit the report is bin/TestResults/Feature-<Config>.xml and the
editor's JSON report lands beside it as Feature-<Config>.json; without it both live in a temporary directory that is
removed. It fails, printing one "UNCOVERED gate <g> (Editor): <id>" line per gap, when the editor exits non-zero, when
any of the gates 1, 1b, 2, 3, 4 and 9 is absent, unevaluated or failed, when a case row is skipped, and when any file
under Projects/FeatureTest outside Library/ differs from a byte snapshot taken before the run. An .hdr import needs the
GPU baker, so the run needs a Vulkan device: without one it prints the GPU suite's no-device line and passes without
running, as a warning, unless --require-gpu is given (Scripts/Lib/coverage.py reads and evaluates the report).

The determinism suite (decision 7, §15.1 T3d) copies Projects/FeatureTest without Library/ into a temporary directory
for each configuration, starts `Editor --headless --renderer none --audio-device none --automation` on it and runs
input.replay {path, verify: true, strictHash: true} for every file Testing.Replays matches (the editor's glob rules). A
replay fails when its verification fails, when its hash differs from its committed FinalStateHash, or, when two
configurations ran, when their hashes differ. It needs no GPU. With --junit it writes bin/TestResults/Determinism.xml,
one test case per replay. A project whose Testing.Replays matches nothing ends as a warning.

--merge-coverage <Editor.json> <Release.json> <Dist.json> runs only the cross-mode union merge of §15.6
(coverage.merge_coverage, decision 6) and prints one "merge: <rule>: <subject>" line per failure; it exits 0 when the
union covers every registered entry and every report passed, 1 otherwise. A report that does not exist or cannot be read
is a mode without a report: "merge: report: <mode>" for its position, exit 1.

--junit writes bin/TestResults/<Suite>-<Config>[-<Cap>].xml (JUnit; test suite named "<Suite>.<Config>[.<Cap>]", where
<Cap> is Vulkan13 or Vulkan14 for the gpu suite; the automation suite has one test suite per test directory and one for
the coverage gate; the feature suite keeps the editor's own suites FeatureTest and FeatureTest.coverage; the determinism
suite writes Determinism.xml). The console then shows a summary and every failure parsed from that file instead of
doctest's console reporter. If the test process dies before writing it, a JUnit file with one error test case records
the crash.

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
import hashlib
import json
import os
import re
import shutil
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from Lib import coverage, paths
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

# The automation suite (§15.7): its test directories, the runner, and the environment the tests read.
AUTOMATION_TEST_DIRECTORIES = (paths.REPOSITORY_ROOT / "Tests" / "Automation",
                               paths.REPOSITORY_ROOT / "Tools" / "MCP" / "tests")
UNITTEST_RUNNER = paths.REPOSITORY_ROOT / "Scripts" / "Lib" / "unittest_runner.py"
MCP_VENV = paths.REPOSITORY_ROOT / "Tools" / "MCP" / ".venv"
RUNNER_EXIT_SKIPPED = 4  # unittest_runner.py: every test passed, but some were skipped
AUTOMATION_CONFIG_VARIABLE = "ENGINE_AUTOMATION_CONFIG"
AUTOMATION_COVERAGE_VARIABLE = "ENGINE_AUTOMATION_COVERAGE"
# --require-gpu for the tests that start rendering editors (Tests/Automation/harness.py).
AUTOMATION_REQUIRE_GPU_VARIABLE = "ENGINE_AUTOMATION_REQUIRE_GPU"
# Settings of the bridge and the tests that a developer's environment must not leak into the suite.
AUTOMATION_CLEARED_VARIABLES = ("ENGINE_MCP_TRANSCRIPT", "ENGINE_MCP_USER_DATA_DIR")
DUMP_REFERENCE_TIMEOUT_SECONDS = 300.0
COVERAGE_CASE_NAME = "Method coverage: every registered method has at least one Python test"
UNCOVERED_IN_DETAIL = 5

# The feature and determinism suites (ADR 0022 decision 7): the FeatureTest project, the Library/ directory that both
# leave out (the editor's import cache), the options of the rendering editor's device probe (Tests/Automation/harness.py
# GPU_EDITOR_ARGUMENTS) and the text of the no-device line the GPU suite and the automation harness print.
FEATURE_PROJECT = paths.REPOSITORY_ROOT / "Projects" / "FeatureTest"
FEATURE_PROJECT_FILE = FEATURE_PROJECT / "FeatureTest.eproj"
PROJECT_CACHE_DIRECTORY = "Library"
GPU_PROBE_ARGUMENTS = ("--gpu-validation=sync", "--expect-no-gpu-errors")
GPU_PROBE_TIMEOUT_SECONDS = 120.0
NO_DEVICE_TEXT = "GPU test without a device (passes without running): "
FAILURES_IN_DETAIL = 3
DETERMINISM_REPORT_STEM = "Determinism"
EDITOR_SHUTDOWN_TIMEOUT_SECONDS = 60.0


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
        Suite("feature", ("Debug", "Release"), True),
        Suite("automation", ("Release",), True),
        Suite("determinism", ("Debug", "Release"), True),
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


def junit_path(report_stem: str) -> Path:
    return paths.TEST_RESULTS_ROOT / f"{report_stem}.xml"


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
    report = junit_path(run.report_stem) if arguments.junit else None
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
    report = junit_path(f"Automation-{config}") if arguments.junit else None
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
        for variable in (*AUTOMATION_CLEARED_VARIABLES, AUTOMATION_REQUIRE_GPU_VARIABLE):
            environment.pop(variable, None)
        if arguments.require_gpu:
            environment[AUTOMATION_REQUIRE_GPU_VARIABLE] = "1"
        console.heading(f"automation ({config}): {paths.display_path(editor)}")
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment, timeout=arguments.timeout,
                                  echo=console.stream, collect=NO_DEVICE_PATTERN)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
        no_device = DeviceReport.parse(result.collected).no_device
        data["noDevice"] = len(no_device)

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
                uncovered = coverage.uncovered_methods(registered, coverage.read_called_methods(coverage_file))
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
    detail = f"{counts.passed} test(s) passed{skipped}; {coverage_detail}"
    if no_device:
        # Only without --require-gpu: with it, such a test fails (harness.require_gpu).
        return Step(name, Status.WARNING, f"{detail}; {len(no_device)} test(s) found no GPU device and passed without "
                                          f"running ({no_device[0]}); --require-gpu fails them instead",
                    result.duration, data=data)
    return Step(name, Status.PASSED, detail, result.duration, data=data)



# --------------------------------------------------------------------------------------------------------------------
# The feature suite (ADR 0022 decision 7)
# --------------------------------------------------------------------------------------------------------------------


def probe_rendering_editor(editor: Path) -> str | None:
    """None when a headless editor can create its device here (`Editor --headless --frames 1` with the GPU test options
    exits 0); otherwise why not: its exit code and the last error line it printed (Tests/Automation/harness.py)."""
    with tempfile.TemporaryDirectory(prefix="Test-GpuProbe-") as directory:
        command = [str(editor), "--headless", "--frames", "1", *GPU_PROBE_ARGUMENTS, f"--user-data-dir={directory}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=GPU_PROBE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            return f"the editor could not run its device probe: {error}"
    if result.succeeded:
        return None
    lines = [line.strip() for line in result.output.splitlines() if line.strip()]
    errors = [line for line in lines if "[error]" in line or "[critical]" in line]
    cause = (errors or lines or ["no output"])[-1]
    return f"a headless editor {result.describe_exit()} creating its device: {cause}"


def snapshot_project(root: Path) -> dict[str, str]:
    """The SHA-256 of every file below `root` outside its Library/ directory, by '/'-separated relative path."""
    snapshot: dict[str, str] = {}
    for directory, directories, files in os.walk(root):
        current = Path(directory)
        if current == root:
            directories[:] = [name for name in directories if name != PROJECT_CACHE_DIRECTORY]
        directories.sort()
        for name in sorted(files):
            path = current / name
            snapshot[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def compare_snapshots(before: dict[str, str], after: dict[str, str]) -> list[str]:
    """One "<change>: <path>" line per file that a run changed, added or removed, sorted by path."""
    changes: list[str] = []
    for path in sorted(set(before) | set(after)):
        if path not in after:
            changes.append(f"removed: {path}")
        elif path not in before:
            changes.append(f"added: {path}")
        elif before[path] != after[path]:
            changes.append(f"changed: {path}")
    return changes


def describe_lines(lines: list[str]) -> str:
    shown = "; ".join(lines[:FAILURES_IN_DETAIL])
    more = len(lines) - FAILURES_IN_DETAIL
    return shown + (f" and {more} more" if more > 0 else "")


def run_feature(config: str, arguments: argparse.Namespace, console: Console) -> Step:
    """FeatureTest in editor mode: the run, the six per-mode gates of its report, no skipped case and an unchanged
    project (ADR 0022 decision 7)."""
    name = f"feature {config}"
    editor = editor_executable(config)
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                         f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    data: dict[str, object] = {"suite": "feature", "config": config, "editor": paths.display_path(editor),
                               "project": paths.display_path(FEATURE_PROJECT)}
    reason = probe_rendering_editor(editor)
    if reason is not None:
        data["noDevice"] = reason
        if arguments.require_gpu:
            return Step(name, Status.FAILED, f"no usable Vulkan device (--require-gpu): {reason}", data=data)
        console.print(f"{NO_DEVICE_TEXT}{name}: {reason}")
        return Step(name, Status.WARNING, f"no usable Vulkan device, so FeatureTest passed without running ({reason}); "
                                          f"--require-gpu fails instead", data=data)

    with tempfile.TemporaryDirectory(prefix="Test-Feature-") as directory:
        report = junit_path(f"Feature-{config}") if arguments.junit else Path(directory) / f"Feature-{config}.xml"
        json_report = report.with_suffix(".json")
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        json_report.unlink(missing_ok=True)
        command = [str(editor), "--headless", "--project", str(FEATURE_PROJECT), "--run-tests",
                   "--audio-device", "none", "--expect-no-gpu-errors", "--junit", str(report),
                   f"--user-data-dir={Path(directory) / 'UserData'}"]
        console.heading(f"{name}: {format_command([paths.display_path(editor), *command[1:]])}")
        before = snapshot_project(FEATURE_PROJECT)
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=arguments.timeout, echo=console.stream)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED, data=data)
        changes = compare_snapshots(before, snapshot_project(FEATURE_PROJECT))
        if arguments.junit:
            data["junit"] = paths.display_path(report)
            data["json"] = paths.display_path(json_report)
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"the FeatureTest run {result.describe_exit()}", result.duration,
                        data=data)

        failures: list[str] = []
        if result.exit_code != EXIT_SUCCESS:
            failures.append(f"the editor {result.describe_exit()}")
        try:
            feature = coverage.read_feature_report(json_report)
            data.update({"cases": len(json.loads(json_report.read_text(encoding="utf-8"))["cases"]),
                         "passedRun": feature.passed})
            failures += coverage.evaluate_feature_report(feature)
        except ValueError as error:
            failures.append(f"no readable report: {error}")
        failures += [f"Projects/FeatureTest {change} outside Library/" for change in changes]
        if changes:
            data["changedFiles"] = changes
    for failure in failures:
        console.print(failure if failure.startswith("UNCOVERED ") else f"FAILED: {failure}")
    if failures:
        data["failures"] = failures
        uncovered = [line for line in failures if line.startswith("UNCOVERED ")]
        other = [line for line in failures if not line.startswith("UNCOVERED ")]
        parts = ([describe_lines(other)] if other else []) + ([f"{len(uncovered)} uncovered entr"
                                                               f"{'y' if len(uncovered) == 1 else 'ies'} "
                                                               f"({describe_lines(uncovered)})"] if uncovered else [])
        return Step(name, Status.FAILED, "; ".join(parts), result.duration, data=data)
    return Step(name, Status.PASSED, "FeatureTest passed with the six editor-mode gates evaluated and green; "
                                     "Projects/FeatureTest unchanged", result.duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# The determinism suite (ADR 0022 decision 7, §15.1 T3d)
# --------------------------------------------------------------------------------------------------------------------


def glob_regex(pattern: str) -> re.Pattern[str]:
    """The editor's Testing.Replays glob rules (EditorCore/Export/Private/ExportPaths.h MatchesExportGlob): '*' any
    characters but '/', '?' one character but '/', "**" any characters ("**/" also no directory at all)."""
    parts: list[str] = []
    index = 0
    while index < len(pattern):
        if pattern.startswith("**/", index):
            parts.append("(?:.*/)?")
            index += 3
        elif pattern.startswith("**", index):
            parts.append(".*")
            index += 2
        elif pattern[index] == "*":
            parts.append("[^/]*")
            index += 1
        elif pattern[index] == "?":
            parts.append("[^/]")
            index += 1
        else:
            parts.append(re.escape(pattern[index]))
            index += 1
    return re.compile("".join(parts) + r"\Z")


def expand_replay_globs(project: Path, globs: list[str]) -> list[str]:
    """The project-relative .replay files below Assets/ that `globs` (Testing.Replays) match, sorted and unique, as the
    editor expands them. Raises ValueError for a glob without wildcards that names no replay."""
    assets = project / "Assets"
    files = sorted(path.relative_to(project).as_posix() for path in assets.rglob("*.replay") if path.is_file()) \
        if assets.is_dir() else []
    matched: set[str] = set()
    for glob in globs:
        pattern = glob[len("project://"):] if glob.startswith("project://") else glob
        expression = glob_regex(pattern)
        hits = [path for path in files if expression.match(path)]
        if not hits and not any(character in glob for character in "*?"):
            raise ValueError(f"the replay '{glob}' of Testing.Replays is missing")
        matched.update(hits)
    return sorted(matched)


@dataclasses.dataclass
class ReplayOutcome:
    """One replay verified by one editor: its observed state hash (empty when none) and why it failed."""

    path: str
    state_hash: str = ""
    problems: list[str] = dataclasses.field(default_factory=list)


def copy_project(source: Path, destination: Path) -> Path:
    """Copies the project `source` without its Library/ directory to `destination`; returns its project file."""
    shutil.copytree(source, destination, ignore=lambda directory, names: [PROJECT_CACHE_DIRECTORY]
                    if Path(directory) == source and PROJECT_CACHE_DIRECTORY in names else [])
    files = sorted(destination.glob("*.eproj"))
    if len(files) != 1:
        raise ValueError(f"{paths.display_path(source)} holds {len(files)} project files, not one")
    return files[0]


def verify_replays(editor: Path, project_file: Path, replays: list[str], user_data: Path,
                   timeout: float) -> list[ReplayOutcome]:
    """Starts `editor --headless --renderer none --audio-device none --automation` on `project_file` and runs
    input.replay {path, verify: true, strictHash: true} for each of `replays`, in order. Each outcome holds the observed
    stateHash and any failure; the editor is always stopped. Raises RuntimeError when the editor cannot be started or
    reached."""
    engine_client = import_engine_client()
    process = engine_client.launch_editor(editor, ["--headless", "--renderer", "none", "--audio-device", "none",
                                                   "--automation", "--project", str(project_file)],
                                          user_data, paths.workspace_name(), environment=child_environment())
    outcomes: list[ReplayOutcome] = []
    try:
        client = process.connect("determinism", call_log=engine_client.CallLog(None))
        with client:
            for path in replays:
                outcome = ReplayOutcome(path)
                try:
                    result = client.call("input.replay", {"path": path, "verify": True, "strictHash": True},
                                         timeout=timeout)
                except engine_client.EngineError as error:
                    result = error.data.get("replay") if isinstance(error.data.get("replay"), dict) else {}
                    outcome.problems.append(f"input.replay failed: {error.detail}")
                outcome.state_hash = str(result.get("stateHash", ""))
                if result and not result.get("passed", False) and not outcome.problems:
                    outcome.problems.append("verification failed")
                if result.get("stopReason"):
                    outcome.problems.append(f"stopped before its final tick: {result['stopReason']}")
                outcomes.append(outcome)
            try:
                client.call("session.shutdown", {"force": True}, timeout=EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
            except (engine_client.EngineError, engine_client.ConnectionClosed, TimeoutError):
                pass  # the process is stopped below either way
        process.wait(EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
    except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, OSError) as error:
        raise RuntimeError(f"{editor.name} on {paths.display_path(project_file)}: {error}\n"
                           f"{process.output()}") from error
    finally:
        process.kill()
    return outcomes


def compare_replay_hashes(committed: dict[str, str], outcomes: dict[str, list[ReplayOutcome]]) -> dict[str, list[str]]:
    """The problems of every replay (committed FinalStateHash by path; each configuration's outcomes), by path: its own
    verification problems per configuration, a hash that differs from the committed one, and hashes that differ between
    the configurations."""
    problems: dict[str, list[str]] = {path: [] for path in committed}
    for config, results in outcomes.items():
        for outcome in results:
            entry = problems.setdefault(outcome.path, [])
            entry += [f"{config}: {problem}" for problem in outcome.problems]
            expected = committed.get(outcome.path, "")
            if outcome.state_hash and outcome.state_hash != expected:
                entry.append(f"{config}: state hash {outcome.state_hash} differs from the committed FinalStateHash "
                             f"{expected or '(none)'}")
    for path in problems:
        hashes = {config: outcome.state_hash for config, results in outcomes.items()
                  for outcome in results if outcome.path == path and outcome.state_hash}
        if len(set(hashes.values())) > 1:
            problems[path].append("the configurations disagree: " + ", ".join(f"{config} {value}"
                                                                              for config, value in hashes.items()))
    return problems


def import_engine_client() -> Any:
    """Tools/Automation/engine_client.py, the standard-library automation client (Architecture §13.2)."""
    tools = str(paths.REPOSITORY_ROOT / "Tools" / "Automation")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import engine_client  # noqa: PLC0415  (Tools/Automation is on sys.path only from here)
    return engine_client


def write_determinism_junit(path: Path, problems: dict[str, list[str]]) -> None:
    root = ElementTree.Element("testsuites")
    failures = sum(1 for lines in problems.values() if lines)
    suite = ElementTree.SubElement(root, "testsuite", {"name": DETERMINISM_REPORT_STEM, "tests": str(len(problems)),
                                                       "failures": str(failures), "errors": "0", "skipped": "0"})
    for replay in sorted(problems):
        case = ElementTree.SubElement(suite, "testcase", {"classname": DETERMINISM_REPORT_STEM, "name": replay})
        if problems[replay]:
            ElementTree.SubElement(case, "failure", {"message": problems[replay][0]}).text = "\n".join(problems[replay])
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def run_determinism(configs: list[str], arguments: argparse.Namespace, console: Console) -> Step:
    """Every FeatureTest replay in the editor of each configuration in `configs`, compared with its committed hash and
    across the configurations (one step, ADR 0022 decision 7)."""
    name = f"determinism {', '.join(configs)}"
    data: dict[str, object] = {"suite": "determinism", "configs": configs}
    editors = {config: editor_executable(config) for config in configs}
    for config, editor in editors.items():
        if not editor.is_file():
            return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                             f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    started = time.monotonic()
    try:
        project = json.loads(FEATURE_PROJECT_FILE.read_text(encoding="utf-8"))
        globs = project.get("Testing", {}).get("Replays", [])
        if not isinstance(globs, list) or not all(isinstance(glob, str) for glob in globs):
            raise ValueError("Testing.Replays must be a list of strings")
        replays = expand_replay_globs(FEATURE_PROJECT, globs)
        committed: dict[str, str] = {}
        for replay in replays:
            document = json.loads((FEATURE_PROJECT / replay).read_text(encoding="utf-8"))
            committed[replay] = str(document.get("FinalStateHash", ""))
    except (OSError, ValueError, AttributeError) as error:
        return Step(name, Status.FAILED, f"cannot read the replays of {paths.display_path(FEATURE_PROJECT_FILE)}: "
                                         f"{error}", data=data)
    data["replays"] = replays
    if not replays:
        if arguments.junit:
            write_determinism_junit(junit_path(DETERMINISM_REPORT_STEM), {})
        return Step(name, Status.WARNING, f"Testing.Replays of {paths.display_path(FEATURE_PROJECT_FILE)} matches no "
                                          f"replay, so nothing was verified", data=data)

    outcomes: dict[str, list[ReplayOutcome]] = {}
    for config, editor in editors.items():
        console.heading(f"determinism {config}: {len(replays)} replay(s) in {paths.display_path(editor)} "
                        f"(--renderer none)")
        with tempfile.TemporaryDirectory(prefix=f"Test-Determinism-{config}-") as directory:
            try:
                project_file = copy_project(FEATURE_PROJECT, Path(directory) / "FeatureTest")
                outcomes[config] = verify_replays(editor, project_file, replays, Path(directory) / "UserData",
                                                  arguments.timeout)
            except (RuntimeError, ValueError, OSError) as error:
                return Step(name, Status.FAILED, f"the {config} editor could not verify the replays: {error}",
                            data=data)
        for outcome in outcomes[config]:
            console.print(f"  {config} {outcome.path}: {outcome.state_hash or '(no hash)'}"
                          + (f" ({'; '.join(outcome.problems)})" if outcome.problems else ""))
    problems = compare_replay_hashes(committed, outcomes)
    data["hashes"] = {config: {outcome.path: outcome.state_hash for outcome in results}
                      for config, results in outcomes.items()}
    if arguments.junit:
        report = junit_path(DETERMINISM_REPORT_STEM)
        write_determinism_junit(report, problems)
        data["junit"] = paths.display_path(report)
    duration = time.monotonic() - started
    failed = {replay: lines for replay, lines in problems.items() if lines}
    for replay, lines in sorted(failed.items()):
        for line in lines:
            console.print(f"FAILED: {replay}: {line}")
    if failed:
        data["failures"] = failed
        first = sorted(failed)[0]
        return Step(name, Status.FAILED, f"{len(failed)} of {len(replays)} replay(s) failed: {first}: "
                                         f"{failed[first][0]}", duration, data=data)
    compared = " and agreed across the configurations" if len(configs) > 1 else ""
    return Step(name, Status.PASSED, f"{len(replays)} replay(s) matched their committed FinalStateHash{compared}",
                duration, data=data)


def unreadable_report_failures(reports: list[coverage.FeatureReport | None]) -> list[str]:
    """The `report` rule's lines (coverage.merge_coverage) over the positional reports when some could not be read
    (None): a position whose report is missing, unreadable, names another mode or duplicates a mode fails for its
    mode."""
    failures: list[str] = []
    for index, mode in enumerate(coverage.MODES):
        count = sum(1 for report in reports if report is not None and report.mode == mode)
        report = reports[index] if index < len(reports) else None
        if report is None or report.mode != mode or count != 1:
            failures.append(f"merge: report: {mode}")
    return failures


def run_merge(arguments: argparse.Namespace, console: Console) -> int:
    """--merge-coverage: the cross-mode union merge of three reports (ADR 0022 decision 6). A report that does not exist
    or cannot be read is a mode without a report: it fails the `report` rule for its position's mode (exit 1, like every
    other merge failure); exit 2 stays argparse's usage error."""
    reports: list[coverage.FeatureReport | None] = []
    errors: list[str] = []
    for mode, path in zip(coverage.MODES, arguments.merge_coverage):
        try:
            reports.append(coverage.read_feature_report(Path(path)))
        except ValueError as error:
            console.print(f"Test.py: the {mode} report cannot be used: {error}")
            errors.append(str(error))
            reports.append(None)
    readable = [report for report in reports if report is not None]
    failures = unreadable_report_failures(reports) if errors else coverage.merge_coverage(readable)
    for failure in failures:
        console.print(failure)
    console.print(f"Coverage merge over {', '.join(report.mode for report in readable) or 'no readable report'}: "
                  f"{'passed' if not failures else f'{len(failures)} failure(s)'}")
    if arguments.json:
        emit_json({"success": not failures, "failures": failures, "errors": errors})
    return EXIT_SUCCESS if not failures else EXIT_FAILED


def run_suite(name: str, config: str, arguments: argparse.Namespace, console: Console) -> Iterator[Step]:
    """The steps of one suite in one configuration: the automation suite's Python run, the feature suite's editor run,
    or the Tests runs of a doctest suite (one per API cap for the gpu suite)."""
    if name == "automation":
        yield run_automation(config, arguments, console)
        return
    if name == "feature":
        yield run_feature(config, arguments, console)
        return
    for run in plan_runs(name, config, arguments):
        yield run_tests(run, arguments, console)


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
    parser.add_argument("--require-gpu", action="store_true",
                        help="gpu, golden, feature and automation suites: a test case (or a Python test that starts a "
                             "rendering editor, or the FeatureTest run) without a usable Vulkan device fails instead "
                             "of passing without running (CI.py and PreCommit.py pass it unless --gpu-optional, §15.2)")
    parser.add_argument("--vulkan-api", choices=VULKAN_API_CAPS,
                        help="gpu suite: run under this API cap only (default: once under each, §8.1)")
    parser.add_argument("--update-golden", action="store_true",
                        help="golden suite: write each image as the golden candidate of this machine's device class "
                             "into Tests/Golden/<DeviceClass>/ instead of comparing; review the diff before committing")
    parser.add_argument("--filter", help="run only test cases matching this pattern (unit, gpu, golden: a doctest "
                                         "wildcard pattern on test case names; automation: comma-separated unittest "
                                         "-k patterns on test ids)")
    parser.add_argument("--allow-skips", action="store_true",
                        help="contract mode, only for the commit of a milestone's contract task (Roadmap rule 3; "
                             "PreCommit.py --contract): report test cases skipped outside the "
                             f"{CHILD_TARGET_SUITE} suite (unit, gpu, golden) and skipped Python tests (automation) "
                             "instead of failing the run")
    parser.add_argument("--timeout", type=float, default=SUITE_TIMEOUT_SECONDS,
                        help=f"seconds per Tests or automation run (default: {SUITE_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--merge-coverage", nargs=3, metavar=("EDITOR_JSON", "RELEASE_JSON", "DIST_JSON"),
                        help="run only the cross-mode coverage merge of three FeatureTest reports (§15.6) and print "
                             "one 'merge: <rule>: <subject>' line per failure")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    if arguments.merge_coverage:
        return arguments
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
    if arguments.merge_coverage:
        return run_merge(arguments, console)
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
        if name == "determinism":
            # One step over every configuration, which compares their hashes.
            step = run_determinism(list(arguments.configs or suite.configurations), arguments, console)
            steps.append(step)
            console.result(step)
            continue
        for config in arguments.configs or suite.configurations:
            for step in run_suite(name, config, arguments, console):
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
