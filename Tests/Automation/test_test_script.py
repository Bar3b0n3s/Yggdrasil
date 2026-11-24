"""Scripts/Test.py's runs (Docs/Architecture.md §2.3). Every Tests process it starts gets a user-data root below
the run's own directory, bin/TestResults/<report stem>/UserData, so its log and its crash reports stay with the run and
never reach the user's real user-data folder (AGENTS.md "Tests"; the "Milestone review" of
Docs/Decisions/0029-m18-contract.md): that test runs the Tests binary of the automation suite's configuration. A run
that ends with a non-zero exit code and no crash report names the last test case its process started and its last log
lines (Architecture §15.2). The automation suite's run is bounded by its configuration's default unless --timeout is
given (Docs/Decisions/0039-script-budget-cpu-time.md decision 5): those tests and the ones of a run without a report
start no process. A sharded unit run (--parallel, Lib/shards.py) runs every case of its selection in exactly one
shard, n shards at a time, merges the shards' reports, keeps the skip check, names a failed shard and bounds each shard
by its expected time: the plan and the run are tested on stand-in processes, and the shards' --first and --last on the
real Tests binary. No editor.
"""

from __future__ import annotations

import contextlib
import io
import itertools
import math
import os
import shutil
import sys
import tempfile
import types
import unittest
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock

from harness import REPOSITORY_ROOT, app_name, automation_config

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Test as test_script  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import shards  # noqa: E402
from Lib.process import ProcessMemory, ProcessResult  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402

# A child-process target that fails an assertion (Tests/Source/Support/RecordingAssertHandlerTests.cpp). Selected with
# --no-skip and started without --child-process, as Test.py starts its Tests processes, it runs in a top-level Tests
# process, which FatalError(Assert) ends after writing a crash report into the process's user-data root.
ASSERTING_TARGET = "RecordingAssertHandler: the target case fails an assertion"


def real_user_data_root() -> Path | None:
    """The user's real user-data root, where a Tests process without --user-data-dir writes (Engine/Source/Engine/
    Platform/Paths.h, GetUserDataRoot): %LOCALAPPDATA% on Windows, ~/Library/Application Support on macOS,
    $XDG_DATA_HOME or ~/.local/share elsewhere; None when the environment names none."""
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA")
        return Path(local) if local else None
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support"
    data_home = os.environ.get("XDG_DATA_HOME")
    return Path(data_home) if data_home and Path(data_home).is_absolute() else Path.home() / ".local" / "share"


class TestsRunUserDataTests(unittest.TestCase):
    def test_a_crashing_tests_run_writes_its_report_below_the_run_and_names_it(self) -> None:
        config = automation_config()
        executable = test_script.tests_executable(config)
        self.assertTrue(executable.is_file(), f"{executable} is missing: python Scripts/Build.py --config {config}")
        results = Path(tempfile.mkdtemp(prefix="TestScriptUserData-")).resolve()
        self.addCleanup(shutil.rmtree, results, ignore_errors=True)
        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", "unit", "--config", config])
        run = test_script.TestRun("unit", config, ("--no-skip", f"--test-case={ASSERTING_TARGET}"))
        console = Console()
        console.stream = io.StringIO()
        user_data = results / f"Unit-{config}" / "UserData"
        crashes = user_data / app_name() / "Crashes"
        streamed = test_script.run_streamed

        def run_with_own_root(command: list[str], **options: Any) -> Any:
            # Checked before the process starts: without its own root, its crash report would land in the user's real
            # user-data folder.
            self.assertIn(f"--user-data-dir={user_data}", command,
                          "Test.py starts the Tests process without the run's own user-data root")
            return streamed(command, **options)

        # Every run of the suite and configuration in the checkout shares the root, and unit runs take no machine lock,
        # so two can overlap: an earlier run's report, and another run's Tests.log held open (without FILE_SHARE_DELETE
        # on Windows, as the engine's log holds it), stay as they are, and the run names only its own report.
        logs = user_data / app_name() / "Logs"
        logs.mkdir(parents=True)
        crashes.mkdir(parents=True)
        earlier = crashes / "crash-1-1.txt"
        earlier.write_text("an earlier run's report", encoding="utf-8")
        with (logs / "Tests.log").open("a", encoding="utf-8") as other_log, \
                mock.patch.object(test_script.paths, "TEST_RESULTS_ROOT", results), \
                mock.patch.object(test_script, "run_streamed", run_with_own_root):
            other_log.write("another run's log line\n")
            other_log.flush()
            step = test_script.run_tests(run, arguments, console)

        self.assertEqual(step.status, Status.FAILED, step.detail)
        self.assertTrue(step.detail.startswith("Tests exited with code"), step.detail)
        self.assertEqual(earlier.read_text(encoding="utf-8"), "an earlier run's report")
        reports = sorted(path for path in crashes.glob("crash-*.txt") if path != earlier)
        self.assertEqual(len(reports), 1, reports)
        report = reports[0]
        text = report.read_text(encoding="utf-8", errors="replace")
        self.assertIn("Reason: Fatal error (Assert)", text)
        self.assertIn("The recorded answer is 41", text)
        # The report names the test case that was running, and the process's log the case as it started.
        self.assertIn(f"\n  TestCase: {ASSERTING_TARGET}\n", text)
        match = test_script.CRASH_REPORT_NAME_PATTERN.match(report.name)
        self.assertIsNotNone(match, report.name)
        log_text = (logs / "Tests.log").read_text(encoding="utf-8", errors="replace")
        self.assertTrue(log_text.startswith("another run's log line\n"))
        process_id = match.group(1) if match else ""
        self.assertIn(f"] [trace] Test case of process {process_id}: {ASSERTING_TARGET}\n", log_text)
        # The report's path stays visible in the run's output: the process's own line, Test.py's line, the step's detail
        # and its JSON data.
        shown = test_script.paths.display_path(report)
        output = console.stream.getvalue()
        self.assertIn(f"Crash report written to '{report.as_posix()}'", output)
        self.assertIn(f"CRASH REPORT: {shown}", output)
        self.assertIn(shown, step.detail)
        self.assertNotIn(earlier.name, step.detail)
        self.assertEqual(step.data["crashReports"], [shown])
        self.assertEqual(step.data["userData"], test_script.paths.display_path(user_data))
        real = real_user_data_root()
        if real is not None:
            self.assertFalse((real / app_name() / "Crashes" / report.name).exists(),
                             f"the run wrote {report.name} into the real user-data folder {real}")


class EndWithoutReportTests(unittest.TestCase):
    """A Tests run that ends with a non-zero exit code, no failed test case and no crash report of its own names, from
    the log files below the run's user-data root, the last test case its process started and the last lines that
    process's doctest thread logged. No process starts: run_streamed returns the ended run."""

    PROCESS_ID = 4242
    THREAD = "7700"  # the doctest thread of process 4242
    OTHER_THREAD = "8800"  # the doctest thread of an overlapping run's process, 5151
    CASE = "Application: --expect-no-gpu-errors fails a run whose device reported an error"

    @staticmethod
    def line(thread: str, level: str, message: str) -> str:
        return f"[2026-11-02 15:36:23.495] [{thread}] [Engine] [{level}] {message}\n"

    def run_without_report(self, logs: dict[str, str], exit_code: int = 1) -> Any:
        """Runs the gpu suite's Debug run under the 1.4 cap against a Tests process that ended with `exit_code` and left
        `logs` (file name: text) in its log folder and no crash report; returns the step."""
        results = Path(tempfile.mkdtemp(prefix="TestScriptEndWithoutReport-")).resolve()
        self.addCleanup(shutil.rmtree, results, ignore_errors=True)
        executable = results / "bin" / test_script.paths.executable_name("Tests")
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"")
        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", "gpu", "--config", "Debug"])
        run = test_script.TestRun("gpu", "Debug", ("--test-suite=GPU",), ("--vulkan-api=1.4",), "1.4")
        folder = test_script.paths.log_folder(results / run.report_stem / "UserData")
        folder.mkdir(parents=True)
        for name, text in logs.items():
            (folder / name).write_text(text, encoding="utf-8")
        console = Console()
        console.stream = io.StringIO()
        ended = ProcessResult([str(executable)], exit_code, False, 1.0, "", process_id=self.PROCESS_ID)
        no_skips = test_script.SkipCheck([], [], [])
        with mock.patch.object(test_script.paths, "TEST_RESULTS_ROOT", results), \
                mock.patch.object(test_script, "tests_executable", return_value=executable), \
                mock.patch.object(test_script, "run_streamed", return_value=ended), \
                mock.patch.object(test_script, "list_skipped_test_cases", return_value=no_skips):
            return test_script.run_tests(run, arguments, console)

    def test_a_run_that_ends_without_a_report_names_its_last_test_case_and_log_lines(self) -> None:
        # The process's earlier case, then its last one; an overlapping run's process logs in between and after.
        log = "".join([
            self.line(self.THREAD, "trace", f"Test case of process {self.PROCESS_ID}: Application: an earlier case"),
            self.line(self.THREAD, "info", "The earlier case's line"),
            self.line(self.THREAD, "trace", f"Test case of process {self.PROCESS_ID}: {self.CASE}"),
            self.line(self.THREAD, "info", "Engine context: window 'Application test' created (64x64)"),
            self.line(self.OTHER_THREAD, "trace", "Test case of process 5151: Another run's case"),
            self.line(self.THREAD, "info", "Frame loop started: Manual clock, 60 Hz, unthrottled"),
            self.line(self.OTHER_THREAD, "info", "Another run's line"),
            self.line(self.THREAD, "error", "ApplicationTests: a provoked GPU message"),
            self.line(self.OTHER_THREAD, "info", "Another run's last line"),
        ])
        step = self.run_without_report({"Tests.log": log})

        self.assertEqual(step.status, Status.FAILED, step.detail)
        self.assertTrue(step.detail.startswith("Tests exited with code 1; no crash report"), step.detail)
        if os.name == "nt":
            self.assertIn("ended by TerminateProcess", step.detail)
        self.assertIn(f"the last test case it started: '{self.CASE}'", step.detail)
        own = [line for line in log.splitlines() if f"] [{self.THREAD}] " in line][2:]
        self.assertEqual(step.data["lastTestCase"], self.CASE)
        self.assertEqual(step.data["lastLogLines"], own)
        self.assertIn(f"its last log lines: {' | '.join(own[-test_script.LOG_LINES_IN_DETAIL:])}", step.detail)
        self.assertNotIn("Another run", step.detail)

    def test_the_last_test_case_is_found_before_the_log_rotated(self) -> None:
        # The case started in what is now Tests.1.log; the process went on logging into the new Tests.log.
        older = "".join([
            self.line(self.THREAD, "trace", f"Test case of process {self.PROCESS_ID}: {self.CASE}"),
            self.line(self.THREAD, "info", "Before the rotation"),
        ])
        newer = "".join([
            self.line(self.OTHER_THREAD, "info", "Another run's line"),
            self.line(self.THREAD, "info", "After the rotation"),
        ])
        step = self.run_without_report({"Tests.2.log": self.line(self.THREAD, "info", "Long before"),
                                        "Tests.1.log": older, "Tests.log": newer})

        self.assertEqual(step.data["lastTestCase"], self.CASE)
        self.assertEqual(step.data["lastLogLines"], [*older.splitlines(), newer.splitlines()[1]])
        self.assertIn(f"the last test case it started: '{self.CASE}'", step.detail)

    def test_a_run_whose_log_names_no_test_case_says_so(self) -> None:
        # Another process's case only, and an exception status no handler saw.
        other = self.line(self.OTHER_THREAD, "trace", "Test case of process 5151: Other")
        step = self.run_without_report({"Tests.log": other}, exit_code=0xC0000409)

        self.assertEqual(step.status, Status.FAILED, step.detail)
        self.assertIn("its log names no test case it started", step.detail)
        self.assertNotIn("its last log lines", step.detail)
        self.assertIsNone(step.data["lastTestCase"])
        self.assertEqual(step.data["lastLogLines"], [])
        if os.name == "nt":
            self.assertIn("Tests exited with code 0xC0000409", step.detail)
            self.assertIn("an exception status that no handler of the process saw", step.detail)

    def test_an_exit_code_of_the_tests_binary_is_named_as_its_own_end(self) -> None:
        # Exit code 5: the timeout watchdog ended the process after its Critical line, which no outside kill does.
        case = self.line(self.THREAD, "trace", f"Test case of process {self.PROCESS_ID}: {self.CASE}")
        step = self.run_without_report({"Tests.log": case}, exit_code=5)

        self.assertIn("no crash report: exit code 5 is the Tests binary's own, a test case past its time limit",
                      step.detail)
        self.assertNotIn("TerminateProcess", step.detail)
        self.assertIn(f"the last test case it started: '{self.CASE}'", step.detail)


class SuiteBoundTests(unittest.TestCase):
    # The Debug automation suite took 1,562 s on an unloaded machine and timed out at the 1,800 s every run had, beside
    # PreCommit's other suites (Docs/Decisions/0039-script-budget-cpu-time.md decision 5).
    UNLOADED_DEBUG_AUTOMATION_SECONDS = 1562.0
    # The Debug unit suite takes about 880 s unloaded; it took 1,212 s beside the phase's other worktrees and passed
    # 1,800 s under heavier load.
    UNLOADED_DEBUG_UNIT_SECONDS = 880.0

    def automation_bound(self, config: str, *options: str) -> float:
        """The timeout run_automation gives the suite's runner in `config`, which a stand-in run_streamed records."""
        bounds: list[float] = []

        def timed_out_run(command: list[str], **settings: Any) -> ProcessResult:
            bounds.append(settings["timeout"])
            return ProcessResult(command, None, True, settings["timeout"], "")

        directory = Path(tempfile.mkdtemp(prefix="TestScriptBound-")).resolve()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        stand_in = directory / "StandIn"
        stand_in.write_text("", encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", "automation", "--config", config, "--filter",
                                                     "test_none", *options])
        console = Console()
        console.stream = io.StringIO()
        with mock.patch.object(test_script, "editor_executable", return_value=stand_in), \
                mock.patch.object(test_script, "venv_interpreter", return_value=stand_in), \
                mock.patch.object(test_script, "run_streamed", timed_out_run):
            step = test_script.run_automation(config, arguments, console)
        self.assertEqual(step.status, Status.TIMEOUT, step.detail)
        self.assertEqual(len(bounds), 1)
        return bounds[0]

    def test_the_debug_automation_run_is_bounded_by_more_than_twice_its_unloaded_time(self) -> None:
        self.assertGreaterEqual(test_script.DEBUG_AUTOMATION_TIMEOUT_SECONDS,
                                2 * self.UNLOADED_DEBUG_AUTOMATION_SECONDS)
        self.assertEqual(self.automation_bound("Debug"), test_script.DEBUG_AUTOMATION_TIMEOUT_SECONDS)
        self.assertEqual(self.automation_bound("Release"), test_script.SUITE_TIMEOUT_SECONDS)

    def run_tests_bound(self, suite: str, config: str, *options: str) -> float:
        """The timeout run_tests gives the Tests process of `suite` in `config`."""
        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", suite, "--config", config, *options])
        return test_script.tests_timeout(test_script.TestRun(suite, config, ()), arguments)

    def test_the_debug_unit_run_is_bounded_by_three_times_its_unloaded_time(self) -> None:
        self.assertGreaterEqual(test_script.DEBUG_UNIT_TIMEOUT_SECONDS, 3 * self.UNLOADED_DEBUG_UNIT_SECONDS)
        self.assertEqual(self.run_tests_bound("unit", "Debug"), test_script.DEBUG_UNIT_TIMEOUT_SECONDS)
        self.assertEqual(self.run_tests_bound("unit", "Release"), test_script.SUITE_TIMEOUT_SECONDS)
        self.assertEqual(self.run_tests_bound("gpu", "Debug"), test_script.SUITE_TIMEOUT_SECONDS)
        self.assertEqual(self.run_tests_bound("unit", "Debug", "--timeout", "90"), 90.0)

    def test_a_given_timeout_bounds_every_run(self) -> None:
        self.assertEqual(self.automation_bound("Debug", "--timeout", "90"), 90.0)
        self.assertEqual(self.automation_bound("Release", "--timeout", "90"), 90.0)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(test_script.parse_arguments(["--suite", "unit", "--config", "Debug"]).timeout,
                             test_script.SUITE_TIMEOUT_SECONDS)
            self.assertEqual(test_script.parse_arguments(["--suite", "unit", "--timeout", "90"]).timeout, 90.0)


def selection_names(count: int) -> list[str]:
    """The names of a selection of `count` cases, in run order."""
    return [f"Shards: case {index:04} works" for index in range(count)]


class ShardPlanTests(unittest.TestCase):
    """Lib/shards.py's plan: contiguous ranges that cover the selection exactly once, cut where the measured costs reach
    each k/n of the total, with timeouts that follow the shards' expected times."""

    def assert_covers_once(self, plans: list[shards.ShardPlan], cases: list[str]) -> None:
        positions = [position for plan in plans for position in range(plan.first, plan.last + 1)]
        self.assertEqual(positions, list(range(1, len(cases) + 1)))
        self.assertEqual([plan.index for plan in plans], list(range(len(plans))))
        for plan in plans:
            self.assertGreaterEqual(plan.last, plan.first)
            self.assertEqual(list(plan.cases), cases[plan.first - 1:plan.last])
            self.assertEqual(plan.count, len(plans))
            self.assertEqual(plan.options(), (f"--first={plan.first}", f"--last={plan.last}"))

    def test_every_case_of_the_selection_runs_in_exactly_one_shard(self) -> None:
        for case_count, shard_count in itertools.product((1, 2, 7, 12, 100, 5498), (1, 2, 3, 12, 16, 24)):
            cases = selection_names(case_count)
            # Uneven costs (every seventh case 40 times dearer), none, one dear case alone, and one free case.
            costings = {"uneven": {case: 40.0 if index % 7 == 0 else 1.0 for index, case in enumerate(cases)},
                        "unmeasured": {}, "partly": {cases[0]: 500.0}, "free": {cases[-1]: 0.0}}
            for name, costs in costings.items():
                with self.subTest(cases=case_count, shards=shard_count, costs=name):
                    plans = shards.plan_shards(cases, shard_count, costs)
                    self.assertEqual(len(plans), min(case_count, shard_count))
                    self.assert_covers_once(plans, cases)
        self.assertEqual(shards.plan_shards([], 4, {}), [])
        with self.assertRaises(ValueError):
            shards.plan_shards(selection_names(3), 0, {})

    def test_measured_costs_balance_the_shards(self) -> None:
        # 100 cases of 1 s, the eleventh 50 s: 149 s, so a third is 49.7 s. Equal counts would put 82 s in the first
        # shard; the costs cut after the dear case and where the sum is nearest 99.3 s.
        cases = selection_names(100)
        costs = {case: 1.0 for case in cases} | {cases[10]: 50.0}
        plans = shards.plan_shards(cases, 3, costs)
        self.assertEqual([(plan.first, plan.last) for plan in plans], [(1, 11), (12, 50), (51, 100)])
        self.assertEqual([plan.expected_seconds for plan in plans], [60.0, 39.0, 50.0])
        self.assertEqual([plan.label for plan in plans], ["shard 0/3", "shard 1/3", "shard 2/3"])

    def test_without_a_measurement_the_shards_hold_equal_case_counts(self) -> None:
        plans = shards.plan_shards(selection_names(10), 3, {})
        self.assertEqual([(plan.first, plan.last) for plan in plans], [(1, 3), (4, 7), (8, 10)])
        self.assertEqual([plan.expected_seconds for plan in plans], [None, None, None])

    def test_a_case_the_measurement_lacks_costs_the_measured_average(self) -> None:
        # Measured 3 s and 1 s, so the two new cases cost 2 s each: 8 s, cut at 4 s.
        cases = selection_names(4)
        plans = shards.plan_shards(cases, 2, {cases[0]: 3.0, cases[1]: 1.0})
        self.assertEqual([(plan.first, plan.last, plan.expected_seconds) for plan in plans], [(1, 2, 4.0), (3, 4, 4.0)])
        # A case measured at 0 s is a measurement: with 6 s and 0 s measured, the new cases cost 3 s each.
        plans = shards.plan_shards(cases, 2, {cases[0]: 6.0, cases[1]: 0.0})
        self.assertEqual([(plan.first, plan.last, plan.expected_seconds) for plan in plans], [(1, 1, 6.0), (2, 4, 6.0)])
        # Every case measured at 0 s: the cuts fall by case count, each shard expecting 0 s.
        plans = shards.plan_shards(cases, 2, {case: 0.0 for case in cases})
        self.assertEqual([(plan.first, plan.last, plan.expected_seconds) for plan in plans], [(1, 2, 0.0), (3, 4, 0.0)])

    def test_a_run_has_shards_of_about_two_minutes_of_work_and_at_least_as_many_as_run_at_a_time(self) -> None:
        # A sanitized process keeps the memory its work took, so a shard holds about SHARD_SECONDS of measured work.
        cases = selection_names(1000)
        self.assertEqual(shards.shard_count(cases, 4, {case: 1.0 for case in cases}),
                         -(-1000 // int(shards.SHARD_SECONDS)))
        self.assertEqual(shards.shard_count(cases, 4, {}), 1000 // shards.SHARD_CASES)  # without a measurement
        self.assertEqual(shards.shard_count(cases[:100], 12, {}), 12)  # at least as many as run at a time
        self.assertEqual(shards.shard_count(cases[:5], 12, {}), 5)  # at most one per case
        self.assertEqual(shards.shard_count([], 12, {}), 0)
        # A case the measurement lacks counts at the measured average: 10 measured at 60 s and 10 new, 1,200 s.
        self.assertEqual(shards.shard_count(cases[:20], 1, {case: 60.0 for case in cases[:10]}), 10)
        with self.assertRaises(ValueError):
            shards.shard_count(cases, 0, {})

    def test_the_dearest_shards_start_first(self) -> None:
        cases = selection_names(4)
        plans = [shards.ShardPlan(index, 4, index + 1, index + 1, (cases[index],), expected)
                 for index, expected in enumerate((5.0, 20.0, 20.0, 1.0))]
        self.assertEqual([plan.index for plan in shards.start_order(plans)], [1, 2, 0, 3])
        unmeasured = [shards.ShardPlan(index, 4, index + 1, index + 1, (cases[index],), None) for index in range(4)]
        self.assertEqual([plan.index for plan in shards.start_order(unmeasured)], [0, 1, 2, 3])

    def test_a_shards_timeout_follows_its_expected_time_within_the_runs_bound(self) -> None:
        self.assertEqual(shards.shard_timeout(300.0, 3600.0), 300.0 * shards.SHARD_TIMEOUT_FACTOR)
        self.assertEqual(shards.shard_timeout(20.0, 3600.0), shards.SHARD_MINIMUM_TIMEOUT_SECONDS)
        self.assertEqual(shards.shard_timeout(2000.0, 3600.0), 3600.0)
        # Without a measurement, a shard is bounded by what is left of the run's bound when it starts.
        self.assertEqual(shards.shard_timeout(None, 1797.0), 1797.0)

    def test_a_cases_passes_and_subcases_count_for_that_case(self) -> None:
        # doctest's JUnit reporter writes one <testcase> per pass through a case, named after the subcases it entered; a
        # case's own name may hold the separator.
        names = {"Json: reads", "Json: reads numbers", "Paths: a/b joins"}
        self.assertEqual(shards.case_of_entry("Json: reads", names), "Json: reads")
        self.assertEqual(shards.case_of_entry("Json: reads/an integer/nested", names), "Json: reads")
        self.assertEqual(shards.case_of_entry("Json: reads numbers", names), "Json: reads numbers")
        self.assertEqual(shards.case_of_entry("Paths: a/b joins", names), "Paths: a/b joins")
        self.assertEqual(shards.case_of_entry("Paths: a/b joins/twice", names), "Paths: a/b joins")
        self.assertIsNone(shards.case_of_entry("Paths: a", names))
        self.assertIsNone(shards.case_of_entry("Other: case", names))
        entries = [("Json: reads/first", 1.5), ("Json: reads/second", 0.5), ("Paths: a/b joins", 2.0), ("Other", 9.0)]
        self.assertEqual(shards.case_seconds(entries, names), {"Json: reads": 2.0, "Paths: a/b joins": 2.0})
        plan = shards.ShardPlan(0, 2, 1, 2, ("Json: reads", "Json: reads numbers"), None)
        self.assertEqual(shards.check_shard_cases(plan, ["Json: reads/first", "Json: reads/second",
                                                         "Json: reads numbers"]), [])
        self.assertEqual(shards.check_shard_cases(plan, ["Json: reads/first", "Paths: a/b joins"]),
                         ["ran outside its range: Paths: a/b joins", "did not run: Json: reads numbers"])

    def test_the_kept_costs_are_read_by_case(self) -> None:
        directory = Path(tempfile.mkdtemp(prefix="ShardCosts-"))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        report = directory / "Unit-Release-Asan.xml"
        self.assertEqual(shards.read_case_costs(report, ["Json: reads"]), {})  # no measurement yet
        write_shard_report(report, [("Json: reads/first", 1.5, None), ("Json: reads/second", 0.5, None),
                                    ("Json: removed", 4.0, None), ("Json: instant", 0.0, None)])
        # A case doctest timed at 0 s is measured, at 0; a case the report lacks is not.
        self.assertEqual(shards.read_case_costs(report, ["Json: reads", "Json: instant", "Json: new"]),
                         {"Json: reads": 2.0, "Json: instant": 0.0})
        report.write_text("<testsuites><testsuite", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "cannot be read"):
            shards.read_case_costs(report, ["Json: reads"])


def write_shard_report(path: Path, entries: list[tuple[str, float, str | None]]) -> None:
    """A JUnit report as doctest writes it: one <testcase> per (name, seconds, failure message or None) entry."""
    suite = ElementTree.Element("testsuite", {
        "name": "Tests", "errors": "0", "failures": str(sum(1 for _, _, failure in entries if failure is not None)),
        "tests": str(len(entries))})
    for name, seconds, failure in entries:
        case = ElementTree.SubElement(suite, "testcase", {"classname": "ShardsTests.cpp", "name": name,
                                                          "time": f"{seconds:g}", "status": "run"})
        if failure is not None:
            ElementTree.SubElement(case, "failure", {"message": failure, "type": "CHECK"}).text = failure
    root = ElementTree.Element("testsuites")
    root.append(suite)
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


class FakeClock:
    """The monotonic clock of a sharded run, which its waits advance."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeShardProcess:
    """A shard's stand-in process: it runs until `clock` reaches `ends_at`, then ends with `returncode`; terminate_tree
    ends it at once."""

    def __init__(self, pid: int, returncode: int, clock: FakeClock, ends_at: float) -> None:
        self.pid = pid
        self.returncode: int | None = None
        self.ending_code = returncode
        self.clock = clock
        self.ends_at = ends_at
        self.terminated = False

    def poll(self) -> int | None:
        if self.returncode is None and self.clock.now >= self.ends_at:
            self.returncode = self.ending_code
        return self.returncode


class ShardedRun:
    """What run_sharded returns: the step, each shard's command, the run's results root, the kept costs and the
    console's text."""

    def __init__(self, step: Step, commands: list[list[str]], results: Path, kept: Path, output: str,
                 most_at_once: int) -> None:
        self.step = step
        self.commands = commands
        self.results = results
        self.kept = kept
        self.output = output
        self.most_at_once = most_at_once  # the most stand-ins that ran at once


class ShardedRunTests(unittest.TestCase):
    """run_sharded_tests (Test.py --parallel) on stand-in processes: start_to_file plays each shard from its command's
    --first and --last over a listing and writes the JUnit report doctest would, so no Tests process starts."""

    SUBCASES_CASE = "Shards: case 2 works"
    CHILD_TARGET = "Shards: the child-process target"
    PID_BASE = 4000
    SHARD_SECONDS = 10.0  # how long a stand-in that ends runs, so the run samples its memory

    @classmethod
    def listing(cls) -> list[Any]:
        """Nine cases, the third passing through two subcases, and a skipped child-process target among them."""
        cases = [test_script.ListedTestCase(f"Shards: case {index} works", "Shards", False) for index in range(9)]
        cases.insert(4, test_script.ListedTestCase(cls.CHILD_TARGET, test_script.CHILD_TARGET_SUITE, True))
        return cases

    def run_sharded(self, argv: tuple[str, ...] = (), behaviour: dict[int, str] | None = None,
                    seconds: Callable[[str], float] = lambda name: 1.0, listing: list[Any] | None = None,
                    directory: Path | None = None) -> ShardedRun:
        """Runs `Test.py --suite unit --config Release --parallel 3 <argv>` over `listing` (default: listing()) in
        `directory` (a new one by default). Each shard's stand-in passes, or as `behaviour` says by the order in which
        the shards start: "fail" (its first case fails), "crash" (it ends without a report), "hang" (it runs, its log
        naming its first case, until it is ended) or "everything" (it ignores its range). A stand-in that ends runs
        SHARD_SECONDS on the run's clock from its start; its cases take `seconds(name)` each in its report."""
        if directory is None:
            directory = Path(tempfile.mkdtemp(prefix="ShardedRun-")).resolve()
            self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        results = directory / "TestResults"
        kept = directory / "Machine" / shards.COSTS_DIRECTORY_NAME / "Unit-Release.xml"
        executable = directory / test_script.paths.executable_name("Tests")
        executable.write_bytes(b"")
        listed = listing if listing is not None else self.listing()
        selection = [case.name for case in listed if not case.skipped]
        commands: list[list[str]] = []
        started: list[FakeShardProcess] = []
        most_at_once = [0]
        clock = FakeClock()

        def start(command: list[str], output: Path, **_options: Any) -> FakeShardProcess:
            process = start_shard(command, output)
            started.append(process)
            most_at_once[0] = max(most_at_once[0], sum(1 for each in started if each.poll() is None))
            return process

        def start_shard(command: list[str], output: Path) -> FakeShardProcess:
            commands.append(list(command))
            index = len(commands) - 1
            values = {part.split("=", 1)[0]: part.split("=", 1)[1] for part in command if "=" in part}
            first, last = int(values["--first"]), int(values["--last"])
            kind = (behaviour or {}).get(index, "pass")
            ran = selection if kind == "everything" else selection[first - 1:last]
            output.write_text(f"shard {index}'s output\n", encoding="utf-8")
            pid = self.PID_BASE + index
            if kind == "hang":
                logs = test_script.paths.log_folder(Path(values["--user-data-dir"]))
                logs.mkdir(parents=True, exist_ok=True)
                (logs / "Tests.log").write_text(f"[2026-11-24 10:00:00.000] [77] [Engine] [trace] Test case of process "
                                                f"{pid}: {ran[0]}\n", encoding="utf-8")
                return FakeShardProcess(pid, 1, clock, math.inf)
            if kind == "crash":
                return FakeShardProcess(pid, 3, clock, clock.now + self.SHARD_SECONDS)
            entries: list[tuple[str, float, str | None]] = []
            for name in ran:
                failure = "CHECK( ok ) is NOT correct!" if kind == "fail" and name == ran[0] else None
                if name == self.SUBCASES_CASE:
                    entries += [(f"{name}/first", seconds(name) / 2, failure),
                                (f"{name}/second", seconds(name) / 2, None)]
                else:
                    entries.append((name, seconds(name), failure))
            write_shard_report(Path(values["--out"]), entries)
            return FakeShardProcess(pid, 1 if kind == "fail" else 0, clock, clock.now + self.SHARD_SECONDS)

        def terminate(process: FakeShardProcess) -> None:
            process.returncode = 1
            process.terminated = True

        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", "unit", "--config", "Release", "--parallel", "3",
                                                     *argv])
        [run] = test_script.plan_runs("unit", "Release", arguments)
        console = Console()
        console.stream = io.StringIO()
        with mock.patch.object(test_script.paths, "TEST_RESULTS_ROOT", results), \
                mock.patch.object(test_script, "tests_executable", return_value=executable), \
                mock.patch.object(test_script, "list_test_cases", return_value=listed), \
                mock.patch.object(test_script, "start_to_file", start), \
                mock.patch.object(test_script, "terminate_tree", terminate), \
                mock.patch.object(test_script, "query_process_memory",
                                  return_value=ProcessMemory(300 << 20, 250 << 20, 400 << 20)), \
                mock.patch.object(test_script.hardening, "available_memory_bytes", return_value=32 << 30), \
                mock.patch.object(shards, "machine_costs_path", return_value=kept), \
                mock.patch.object(test_script, "time", types.SimpleNamespace(monotonic=clock.monotonic,
                                                                             sleep=clock.sleep)):
            step = test_script.run_sharded_tests(run, arguments, console)
        return ShardedRun(step, commands, results, kept, console.stream.getvalue(), most_at_once[0])

    def merged_entries(self, report: Path, suite_name: str = "Unit.Release") -> list[tuple[str, str]]:
        """(shard property, test case name) of every <testcase> of the merged report `report`, in order; every test
        suite is named `suite_name`."""
        entries: list[tuple[str, str]] = []
        for suite in ElementTree.parse(report).getroot().iter("testsuite"):
            self.assertEqual(suite.get("name"), suite_name)
            shard = next(item.get("value", "") for item in suite.iter("property")
                         if item.get("name") == shards.SHARD_PROPERTY)
            entries += [(shard, case.get("name", "")) for case in suite.iter("testcase")]
        return entries

    def test_every_case_runs_in_one_shard_and_the_reports_merge_into_the_runs(self) -> None:
        sharded = self.run_sharded(("--junit",))
        step = sharded.step
        self.assertEqual(step.status, Status.PASSED, step.detail)
        selection = [case.name for case in self.listing() if not case.skipped]
        # Three shards of three cases (no measurement yet), each with its own report, output and user-data root.
        self.assertEqual(len(sharded.commands), 3)
        for index, command in enumerate(sharded.commands):
            shard = sharded.results / "Unit-Release" / f"Shard{index}"
            self.assertIn(f"--first={3 * index + 1}", command)
            self.assertIn(f"--last={3 * index + 3}", command)
            self.assertIn(f"--out={shard.with_suffix('.xml')}", command)
            self.assertIn(f"--user-data-dir={shard / 'UserData'}", command)
            self.assertIn("--test-suite-exclude=GPU,Golden", command)
        # One report, bin/TestResults/Unit-Release.xml: each shard's suite under the run's name, naming its shard, and
        # every case of the selection once, its subcases' passes in its own shard.
        entries = self.merged_entries(sharded.results / "Unit-Release.xml")
        cases = [(shard, shards.case_of_entry(name, frozenset(selection))) for shard, name in entries]
        self.assertEqual(sorted({case for _, case in cases}, key=selection.index), selection)
        for case in selection:
            self.assertEqual(len({shard for shard, name in cases if name == case}), 1, case)
        self.assertEqual([shard for shard, _ in cases], ["0/3"] * 4 + ["1/3"] * 3 + ["2/3"] * 3)
        self.assertEqual((step.data["testCases"], step.data["passed"], step.data["failed"]), (10, 10, 0))
        self.assertEqual(step.data["junit"], test_script.paths.display_path(sharded.results / "Unit-Release.xml"))
        # The skip check is the listing's: the child-process target stays skipped.
        self.assertEqual(step.data["skipCheck"], {"skipped": 1, "childTargets": 1, "unexpected": [], "allowed": False})
        self.assertIn("10 test case(s) passed in 3 shard(s), 3 at a time", step.detail)
        self.assertIn("1 skipped, all of them among the 1 child-process target(s)", step.detail)
        self.assertEqual([shard["peakPrivateBytes"] for shard in step.data["shards"]], [300 << 20] * 3)
        self.assertEqual(step.data["leastAvailableBytes"], 32 << 30)

    def test_a_skipped_case_outside_the_child_targets_fails_the_sharded_run_naming_it(self) -> None:
        listing = self.listing()
        listing.insert(7, test_script.ListedTestCase("Shards: a contract case", "Shards", True))
        sharded = self.run_sharded(listing=listing)
        self.assertEqual(sharded.step.status, Status.FAILED, sharded.step.detail)
        self.assertIn("1 skipped test case(s) outside the ChildTargets suite: 'Shards: a contract case'",
                      sharded.step.detail)
        self.assertIn("UNEXPECTED SKIP: Shards: a contract case", sharded.output)
        # Every shard still ran its cases, and only contract mode lets the run pass.
        self.assertEqual(sharded.step.data["passed"], 10)
        self.assertFalse(sharded.kept.exists())
        allowed = self.run_sharded(("--allow-skips",), listing=listing)
        self.assertEqual(allowed.step.status, Status.PASSED, allowed.step.detail)
        self.assertIn("contract mode (--allow-skips): 2 skipped, 1 of them outside the ChildTargets suite",
                      allowed.step.detail)

    def test_a_failed_shard_is_named_with_its_failures(self) -> None:
        sharded = self.run_sharded(("--junit",), behaviour={1: "fail", 2: "crash"})
        step = sharded.step
        self.assertEqual(step.status, Status.FAILED, step.detail)
        self.assertTrue(step.detail.startswith("2 of 3 shard(s) failed: shard 1/3: 1 of 3 test case(s) failed; "
                                               "shard 2/3: Tests exited with code 3"), step.detail)
        self.assertEqual(len(step.data["failures"]), 1)
        self.assertTrue(step.data["failures"][0].startswith("shard 1/3: Shards: case 3 works (ShardsTests.cpp): CHECK"),
                        step.data["failures"])
        self.assertIn("FAILED: shard 1/3: Shards: case 3 works", sharded.output)
        # The merged report holds the failure in shard 1's suite and an error test case for shard 2, which left none.
        report = ElementTree.parse(sharded.results / "Unit-Release.xml").getroot()
        suites = list(report.iter("testsuite"))
        self.assertEqual(len(suites), 3)
        self.assertIsNotNone(suites[1].find("testcase/failure"))
        error = suites[2].find("testcase/error")
        self.assertIsNotNone(error)
        self.assertIn("shard 2/3", error.get("message", "") if error is not None else "")
        # A failed run measures nothing for the next one.
        self.assertFalse(sharded.kept.exists())

    def test_a_shard_that_ran_other_cases_than_its_range_fails(self) -> None:
        sharded = self.run_sharded(behaviour={0: "everything"})
        self.assertEqual(sharded.step.status, Status.FAILED, sharded.step.detail)
        self.assertIn("shard 0/3: ran other test cases than its range 1..3: 6 problem(s), first: ran outside its "
                      "range: Shards: case 3 works", sharded.step.detail)

    def test_a_shard_past_its_timeout_is_ended_naming_its_last_case(self) -> None:
        # Measured at 300 s a case, the nine cases make nine shards (2,700 s over 120 s a shard), each expecting 300 s,
        # so each one's timeout is three times that; the other shards keep running beside the hung one.
        directory = Path(tempfile.mkdtemp(prefix="ShardedRun-")).resolve()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        kept = directory / "Machine" / shards.COSTS_DIRECTORY_NAME / "Unit-Release.xml"
        selection = [case.name for case in self.listing() if not case.skipped]
        write_shard_report(kept, [(name, 300.0, None) for name in selection])
        sharded = self.run_sharded(behaviour={0: "hang"}, directory=directory)
        step = sharded.step
        self.assertEqual(step.status, Status.TIMEOUT, step.detail)
        timeout = 300.0 * shards.SHARD_TIMEOUT_FACTOR
        self.assertEqual([shard["timeoutSeconds"] for shard in step.data["shards"]], [timeout] * 9)
        self.assertEqual([shard["expectedSeconds"] for shard in step.data["shards"]], [300.0] * 9)
        self.assertEqual(step.data["shards"][0]["seconds"], timeout)
        self.assertEqual(step.detail, f"1 of 9 shard(s) failed: shard 0/9: Tests timed out ({timeout:.0f} s from 0 s, "
                                      f"expected 300 s, timeout {timeout:.0f} s); the last test case it started: "
                                      f"'Shards: case 0 works'")
        self.assertEqual(step.data["shards"][0]["lastTestCase"], "Shards: case 0 works")
        self.assertEqual(step.data["passed"], 9)
        # A given --timeout bounds the listing and the shards together, so a shard's timeout is at most what is left of
        # it when the shard starts.
        capped = self.run_sharded(("--timeout", "700"), behaviour={0: "hang"}, directory=directory)
        self.assertEqual(capped.step.data["shards"][0]["timeoutSeconds"], 700.0)
        for shard in capped.step.data["shards"]:
            self.assertLessEqual(shard["startedAtSeconds"] + shard["timeoutSeconds"], 700.0, shard)
        # Without a measurement, a shard has no expected time, and what is left of the run's bound is its timeout.
        unmeasured = self.run_sharded(behaviour={0: "hang"})
        self.assertEqual(unmeasured.step.data["shards"][0]["timeoutSeconds"], test_script.SUITE_TIMEOUT_SECONDS)

    def test_the_shards_run_n_at_a_time_the_dearest_first_within_the_runs_bound(self) -> None:
        # Measured at 300 s, 200 s and 100 s a case in turn: nine shards of one case each, three at a time, which start
        # in the order of their expected times, the next as one ends.
        directory = Path(tempfile.mkdtemp(prefix="ShardedRun-")).resolve()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        kept = directory / "Machine" / shards.COSTS_DIRECTORY_NAME / "Unit-Release.xml"
        selection = [case.name for case in self.listing() if not case.skipped]
        costs = {name: (300.0, 200.0, 100.0)[index % 3] for index, name in enumerate(selection)}
        write_shard_report(kept, [(name, seconds, None) for name, seconds in costs.items()])
        # Each run measures the same costs again, so the kept measurement stays the same.
        sharded = self.run_sharded(seconds=costs.__getitem__, directory=directory)
        self.assertEqual(sharded.step.status, Status.PASSED, sharded.step.detail)
        self.assertEqual(sharded.most_at_once, 3)
        self.assertEqual([command[next(index for index, part in enumerate(command) if part.startswith("--first="))]
                          for command in sharded.commands],
                         [f"--first={first}" for first in (1, 4, 7, 2, 5, 8, 3, 6, 9)])
        started = {shard["first"]: shard["startedAtSeconds"] for shard in sharded.step.data["shards"]}
        self.assertEqual([started[first] for first in (1, 4, 7)], [0.0] * 3)
        self.assertTrue(all(started[first] >= self.SHARD_SECONDS for first in (2, 5, 8, 3, 6, 9)), started)
        self.assertEqual(sharded.step.data["parallel"], 3)
        # A run whose bound runs out leaves its last shards unstarted: they fail as timeouts, named.
        short = self.run_sharded(("--timeout", "15"), seconds=costs.__getitem__, directory=directory)
        self.assertEqual(short.step.status, Status.TIMEOUT, short.step.detail)
        self.assertEqual(len(short.commands), 6)
        self.assertIn("did not start: the run's 15 s ran out first", short.step.detail)
        unstarted = [shard for shard in short.step.data["shards"] if "seconds" not in shard]
        self.assertEqual(sorted(shard["first"] for shard in unstarted), [3, 6, 9])
        unstarted_report = short.results / "Unit-Release" / "Shard2.xml"
        error = ElementTree.parse(unstarted_report).getroot().find("testsuite/testcase/error")
        self.assertIsNotNone(error)
        self.assertIn("shard 2/9: did not start", error.get("message", "") if error is not None else "")

    def test_a_full_passing_run_keeps_the_costs_that_size_the_next_and_a_filtered_run_keeps_none(self) -> None:
        directory = Path(tempfile.mkdtemp(prefix="ShardedRun-")).resolve()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)

        def seconds(name: str) -> float:
            return 30.0 if name == "Shards: case 0 works" else 1.0

        first = self.run_sharded(seconds=seconds, directory=directory)
        self.assertEqual(first.step.status, Status.PASSED, first.step.detail)
        self.assertEqual(first.step.data["keptCosts"], test_script.paths.display_path(first.kept))
        self.assertEqual(first.step.data["measuredCases"], 0)
        # The next run is sized from them: the 30 s case alone in its shard (38 s in all, a third is 12.7 s), and the
        # shards start dearest first.
        second = self.run_sharded(seconds=seconds, directory=directory)
        self.assertEqual(second.step.data["measuredCases"], 9)
        self.assertEqual([(shard["first"], shard["last"], shard["expectedSeconds"])
                          for shard in second.step.data["shards"]], [(1, 1, 30.0), (2, 2, 1.0), (3, 9, 7.0)])
        self.assertEqual([next(part for part in command if part.startswith("--first=")) for command in second.commands],
                         ["--first=1", "--first=3", "--first=2"])
        # A run of part of the selection measures only that part, so it leaves the kept costs alone.
        kept = second.kept.read_bytes()
        filtered = self.run_sharded(("--filter", "Shards: case 1*"), seconds=lambda name: 5.0, directory=directory)
        self.assertEqual(filtered.step.status, Status.PASSED, filtered.step.detail)
        self.assertNotIn("keptCosts", filtered.step.data)
        self.assertEqual(second.kept.read_bytes(), kept)

    def test_the_tests_binary_runs_each_case_of_the_selection_in_exactly_one_shard(self) -> None:
        # The real Tests binary of the automation suite's configuration: doctest's --first and --last over the listing's
        # order. ExpectLog's cases are interleaved with its skipped child-process targets, and ReadFormatHeader passes
        # through three subcases.
        config = automation_config()
        executable = test_script.tests_executable(config)
        self.assertTrue(executable.is_file(), f"{executable} is missing: python Scripts/Build.py --config {config}")
        results = Path(tempfile.mkdtemp(prefix="TestScriptShards-")).resolve()
        self.addCleanup(shutil.rmtree, results, ignore_errors=True)
        selected = "ExpectLog:*,JsonReader:*"
        with contextlib.redirect_stderr(io.StringIO()):
            arguments = test_script.parse_arguments(["--suite", "unit", "--config", config, "--parallel", "4",
                                                     "--filter", selected, "--exclude-test-suites", "Fuzz", "--junit"])
        [run] = test_script.plan_runs("unit", config, arguments)
        console = Console()
        console.stream = io.StringIO()
        kept = results / "Machine" / "Unit.xml"
        with mock.patch.object(test_script.paths, "TEST_RESULTS_ROOT", results), \
                mock.patch.object(shards, "machine_costs_path", return_value=kept):
            step = test_script.run_sharded_tests(run, arguments, console)
        self.assertEqual(step.status, Status.PASSED, step.detail)
        listed = test_script.list_test_cases(executable, [*run.filters, f"--test-case={selected}"])
        selection = [case.name for case in listed if not case.skipped]
        self.assertGreater(len(selection), 20)
        self.assertEqual(step.data["skipCheck"]["skipped"], len(listed) - len(selection))
        self.assertGreater(step.data["skipCheck"]["skipped"], 0)
        # The merged report holds the selection's cases in the binary's order, each case's passes in one shard.
        names = frozenset(selection)
        cases = [(shard, shards.case_of_entry(name, names))
                 for shard, name in self.merged_entries(results / f"{run.report_stem}.xml", run.junit_suite_name)]
        self.assertNotIn(None, [case for _, case in cases])
        order = [case for index, (_, case) in enumerate(cases) if index == 0 or cases[index - 1][1] != case]
        self.assertEqual(order, selection)
        for case in selection:
            self.assertEqual(len({shard for shard, name in cases if name == case}), 1, case)
        self.assertEqual({shard for shard, _ in cases}, {"0/4", "1/4", "2/4", "3/4"})
        passes = [shard for shard, name in cases if name == "JsonReader: ReadFormatHeader checks the format and the "
                                                              "version range"]
        self.assertGreater(len(passes), 1)
        # A filtered run measures part of the selection, so it keeps no costs.
        self.assertFalse(kept.exists())

    def test_shards_are_refused_where_they_mean_nothing(self) -> None:
        for argv in (["--suite", "unit", "--parallel", "0"], ["--suite", "gpu", "--parallel", "2"],
                     ["--suite", "automation", "--parallel", "2"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                test_script.parse_arguments(argv)
        if test_script.debugheap.is_available():
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                test_script.parse_arguments(["--suite", "unit", "--parallel", "2", "--debug-heap"])


if __name__ == "__main__":
    unittest.main()
