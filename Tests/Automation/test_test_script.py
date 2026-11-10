"""Scripts/Test.py's runs (Docs/Architecture.md §2.3). Every Tests process it starts gets a user-data root below
the run's own directory, bin/TestResults/<report stem>/UserData, so its log and its crash reports stay with the run and
never reach the user's real user-data folder (AGENTS.md "Tests"; the "Milestone review" of
Docs/Decisions/0029-m18-contract.md): that test runs the Tests binary of the automation suite's configuration. A run
that ends with a non-zero exit code and no crash report names the last test case its process started and its last log
lines (Architecture §15.2). The automation suite's run is bounded by its configuration's default unless --timeout is
given (Docs/Decisions/0039-script-budget-cpu-time.md decision 5): those tests and the ones of a run without a report
start no process. No editor.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from harness import REPOSITORY_ROOT, app_name, automation_config

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Test as test_script  # noqa: E402  (Scripts/ is on sys.path above)
from Lib.process import ProcessResult  # noqa: E402
from Lib.report import Console, Status  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
