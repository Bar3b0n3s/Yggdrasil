"""Scripts/Test.py's runs of the Tests binary (Docs/Architecture.md §2.3): every Tests process it starts gets a
user-data root below the run's own directory, bin/TestResults/<report stem>/UserData, so its log and its crash reports
stay with the run and never reach the user's real user-data folder (AGENTS.md "Tests"; the "Milestone review" of
Docs/Decisions/0029-m18-contract.md). Runs the Tests binary of the automation suite's configuration; no editor.
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
        self.assertTrue((logs / "Tests.log").read_text(encoding="utf-8").startswith("another run's log line\n"))
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


if __name__ == "__main__":
    unittest.main()
