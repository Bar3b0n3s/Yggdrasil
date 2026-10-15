"""Testing exports through automation and the export suite's pure parts (Docs/Architecture.md §7.6, §14.1 to §14.3,
§15.1 T3 and T3d; Roadmap M15; Docs/Decisions/0024-m15-contract.md decisions 6, 9 and 10, stream B). The FeatureTest
testing export in Release and Dist is the export stage's (Scripts/Test.py --suite export); these tests use the tiny
game.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config, engine_client
from tiny_game import TINY_GAME_NAME, TINY_SCENE, build_tiny_game

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Test as test_script  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import exports, paths  # noqa: E402
from Lib.process import ProcessResult  # noqa: E402
from Lib.report import EXIT_INIT_FAILED, Console, Status  # noqa: E402

SMOKE_SUITE = "Assets/Tests/Smoke.test.luau"
SMOKE_SOURCE = ("--!strict\nreturn Test.Suite(\"Smoke\", function()\n"
                "\tTest.Case(\"passes\", function() Test.Expect(true, \"true\") end)\nend)\n")


def write_feature_report(path: Path, mode: str, suites: list[dict]) -> Path:
    """A minimal FeatureTest TestRunResult with `suites` rows."""
    document = {"mode": mode, "passed": True, "cancelled": False, "cases": [], "suites": suites,
                "coverage": {"registryFingerprint": "0000000000000000", "counters": [], "gates": []},
                "jsonPath": "", "junitPath": "", "recordingPath": ""}
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def manifest_of(output: Path, name: str) -> dict:
    """The Game.json of the package in `output` (in the bundle's Resources on macOS)."""
    manifest = output / f"{name}.app" / "Contents" / "Resources" / "Game.json" if sys.platform == "darwin" \
        else output / "Game.json"
    return json.loads(manifest.read_text(encoding="utf-8"))


def add_smoke_suite(client: engine_client.EngineClient) -> None:
    """The tiny game's one test suite, SMOKE_SUITE on its main scene."""
    client.call("script.write", {"path": SMOKE_SUITE, "source": SMOKE_SOURCE})
    client.call("project.setSettings",
                {"patch": {"Testing": {"Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}]}}})


class TestingExportParamTests(AutomationTestCase):
    """project.export's testing param (moved from test_export.py by the contract; stream B's acceptance)."""

    def test_project_export_testing_param(self) -> None:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        # testing is accepted true and false (ADR 0024 decision 6), and reported with the counts; a project without
        # suites or replays exports for testing too.
        testing = client.call("project.export", {"config": automation_config(), "testing": True})
        self.assertTrue(testing["testing"])
        self.assertEqual((testing["testSuiteCount"], testing["replayCount"]), (0, 0))
        self.assertTrue(manifest_of(Path(testing["outputDirectory"]), TINY_GAME_NAME)["Testing"])
        plain = client.call("project.export", {"config": automation_config(), "testing": False})
        self.assertFalse(plain["testing"])
        self.assertFalse(manifest_of(Path(plain["outputDirectory"]), TINY_GAME_NAME)["Testing"])
        self.assertNotEqual(testing["outputDirectory"], plain["outputDirectory"])
        for result in (testing, plain):
            self.assertIsInstance(result["resourcesPatched"], bool)
            self.assertIsInstance(result["phaseTimings"], list)
        # The param is a boolean: anything else is refused at /testing, before anything is written.
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.export", {"config": automation_config(), "testing": "yes", "outDir": "Build/Refused"})
        self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(refused.exception.issues[0]["pointer"], "/testing")
        self.assertFalse((root / "Build" / "Refused").exists())


class TestingExportTests(AutomationTestCase):
    def test_testing_export_packages_tests_and_says_testing(self) -> None:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        add_smoke_suite(client)
        report = client.call("project.export", {"config": automation_config(), "testing": True})
        self.assertTrue(report["testing"])
        self.assertEqual(report["testSuiteCount"], 1)
        self.assertEqual(report["replayCount"], 0)
        output = Path(report["outputDirectory"])
        self.assertTrue(output.is_relative_to(root / "Build"))
        self.assertTrue(output.parent.name.endswith("-Testing"), output)
        self.assertTrue(manifest_of(output, TINY_GAME_NAME)["Testing"])
        plain = client.call("project.export", {"config": automation_config(), "testing": False})
        self.assertFalse(plain["testing"])

    def test_testing_export_refuses_an_invalid_suite_at_its_setting(self) -> None:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("script.write", {"path": "Assets/Tests/NotASuite.luau",
                                     "source": "--!strict\nlocal Plain = {}\nreturn Script.Define(\"Plain\", Plain)\n"})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [
            {"Script": "Assets/Tests/NotASuite.luau", "Scene": "Assets/Scenes/Missing.scene"}]}}})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.export", {"config": automation_config(), "testing": True})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED)
        issues = refused.exception.issues
        self.assertEqual([issue["pointer"] for issue in issues],
                         ["/Testing/Suites/0/Script", "/Testing/Suites/0/Scene"])
        self.assertTrue(all(issue["message"].startswith("TEST_SUITE_INVALID: ") for issue in issues), issues)
        self.assertFalse((root / "Build").exists())

    def test_testing_export_runs_its_suite_in_the_exported_runtime(self) -> None:
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        add_smoke_suite(client)
        report = client.call("project.export", {"config": automation_config(), "testing": True})
        package = Path(report["outputDirectory"])
        with tempfile.TemporaryDirectory(prefix="TestingExportRun-") as working:
            code, stderr = self.run_exported(Path(report["executable"]), Path(working))
            self.assertEqual(code, 0, stderr)
            result = json.loads((Path(working) / "bin" / "TestResults" / "FeatureTest-Release.json").read_text(
                encoding="utf-8"))
        self.assertTrue(result["passed"])
        self.assertEqual([row["suite"] for row in result["suites"]], ["Smoke"])
        self.assertTrue(
            (exports.testing_user_data(package, TINY_GAME_NAME) / "Logs" / f"{TINY_GAME_NAME}.log").is_file())
        self.assertEqual(test_script.check_testing_user_data(package, TINY_GAME_NAME), [])

    def run_exported(self, executable: Path, working: Path) -> tuple[int, str]:
        """Runs the exported executable's --feature-test headless without a device in `working`."""
        completed = subprocess.run([str(executable), "--headless", "--renderer", "none", "--feature-test"], cwd=working,
                                   stdin=subprocess.DEVNULL, capture_output=True, timeout=300.0, check=False)
        return completed.returncode, completed.stderr.decode("utf-8", errors="replace")


class ExportsLibTests(unittest.TestCase):
    """Scripts/Lib/exports.py: the parts of the export and determinism suites that need no build."""

    def test_package_paths(self) -> None:
        package = exports.package_directory("Dist")
        self.assertEqual(package, REPOSITORY_ROOT / "bin" / "Exports" / "Dist" / "FeatureTest")
        executable = exports.package_executable(package)
        if sys.platform == "win32":
            self.assertEqual(executable, package / "FeatureTest.exe")
        elif sys.platform == "darwin":
            self.assertEqual(executable, package / "FeatureTest.app" / "Contents" / "MacOS" / "FeatureTest")
        else:
            self.assertEqual(executable, package / "FeatureTest")
        self.assertEqual(exports.testing_user_data(package), package / "bin" / "TestUserData" / "FeatureTest")

    def test_replay_hashes_come_from_every_replay_row_with_a_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ExportsLib-") as directory:
            report = write_feature_report(Path(directory) / "FeatureTest-Dist.json", "Dist", [
                {"suite": "replay:Assets/Tests/Replays/Determinism.replay", "passed": True,
                 "finalStateHash": "2a75dca847e98e59"},
                {"suite": "replay:Assets/Tests/Replays/Input.replay", "passed": False,
                 "finalStateHash": "ff0e3f70e3200f87"},
                {"suite": "replay:Assets/Tests/Replays/Broken.replay", "passed": False, "finalStateHash": ""},
                {"suite": "Api.Entity", "passed": True, "finalStateHash": "0123456789abcdef"},
            ])
            # A failed row keeps its observed hash, so the determinism stage can name the drift.
            self.assertEqual(exports.read_replay_hashes(report),
                             {"Assets/Tests/Replays/Determinism.replay": "2a75dca847e98e59",
                              "Assets/Tests/Replays/Input.replay": "ff0e3f70e3200f87"})
            broken = Path(directory) / "Broken.json"
            broken.write_text("[]", encoding="utf-8")
            with self.assertRaises(ValueError):
                exports.read_replay_hashes(broken)

    def test_a_report_counts_only_when_newer_than_every_input(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ExportsLib-") as directory:
            root = Path(directory)
            report = root / "FeatureTest-Release.json"
            runtime = root / "Runtime.exe"
            runtime.write_text("runtime", encoding="utf-8")
            self.assertIn("missing", exports.stale_report_reason(report, [runtime]) or "")
            report.write_text("{}", encoding="utf-8")
            os.utime(runtime, (1_000_000, 1_000_000))
            os.utime(report, (2_000_000, 2_000_000))
            self.assertIsNone(exports.stale_report_reason(report, [runtime, root / "Absent.exe"]))
            os.utime(runtime, (3_000_000, 3_000_000))
            reason = exports.stale_report_reason(report, [runtime])
            self.assertIsNotNone(reason)
            self.assertIn("Runtime.exe", reason or "")
        inputs = exports.report_inputs("Dist", "exported")
        self.assertTrue(any(path.parent.name == "Runtime" and path.stem == "Runtime" for path in inputs))
        self.assertTrue(any(path.parent.name == "Editor" and path.stem == "Editor" for path in inputs))
        self.assertTrue(any("Library" not in path.parts and path.suffix == ".replay" for path in inputs))
        self.assertFalse(any("Library" in path.relative_to(REPOSITORY_ROOT).parts for path in inputs))
        with self.assertRaises(ValueError):
            exports.report_inputs("Dist", "other")

    def test_report_inputs_name_the_binaries_of_each_kind(self) -> None:
        release = paths.output_directory("Release")
        editor_inputs = exports.report_inputs("Release", "editor")
        self.assertEqual(editor_inputs[0], release / "Editor" / paths.executable_name("Editor"))
        self.assertFalse(any(path.parent.name == "Runtime" for path in editor_inputs))
        exported = exports.report_inputs("Release", "exported")
        self.assertEqual(exported[:2], [release / "Runtime" / paths.executable_name("Runtime"),
                                        release / "Editor" / paths.executable_name("Editor")])
        self.assertIn(exports.FEATURE_PROJECT / "FeatureTest.eproj", exported)
        for config, kind in (("Debug", "exported"), ("Dist", "editor")):
            with self.subTest(config=config, kind=kind), self.assertRaises(ValueError):
                exports.report_inputs(config, kind)

    def test_determinism_runs_are_compared_with_the_committed_hash_and_each_other(self) -> None:
        replay = "Assets/Tests/Replays/Determinism.replay"
        committed = {replay: "2a75dca847e98e59"}
        same = {run: {replay: "2a75dca847e98e59"} for run in exports.DETERMINISM_RUNS}
        self.assertEqual(exports.compare_determinism_runs(committed, same), {replay: []})
        drifted = dict(same)
        drifted["Exported Dist"] = {replay: "%016x" % zlib.crc32(b"drift")}
        failures = exports.compare_determinism_runs(committed, drifted)[replay]
        self.assertTrue(any(line.startswith("Exported Dist: ") for line in failures), failures)
        self.assertTrue(any(line.startswith("runs differ: ") for line in failures), failures)
        missing = dict(same)
        missing["Exported Release"] = {}
        self.assertEqual(exports.compare_determinism_runs(committed, missing)[replay], ["Exported Release: no hash"])


class ExportSuiteTests(unittest.TestCase):
    """Scripts/Test.py's export and determinism suites without a build: arguments, the user-data check, the merge's
    preconditions and the exported runs' reports (ADR 0024 decisions 9 and 10)."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="ExportSuite-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def parse(self, *argv: str) -> argparse.Namespace:
        return test_script.parse_arguments(list(argv))

    def assert_usage_error(self, *argv: str) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            self.parse(*argv)
        self.assertEqual(raised.exception.code, 2)

    def test_dist_is_accepted_only_by_the_export_determinism_and_games_suites(self) -> None:
        self.assertEqual(self.parse("--suite", "export", "--config", "Release,Dist").configs, ["Release", "Dist"])
        self.assertEqual(self.parse("--suite", "determinism", "--config", "Dist,Debug").configs, ["Debug", "Dist"])
        self.assertEqual(self.parse("--suite", "games", "--config", "Dist,Debug").configs, ["Debug", "Dist"])
        self.assertEqual(test_script.SUITES["export"].configurations, ("Release", "Dist"))
        self.assertEqual(test_script.SUITES["determinism"].configurations, ("Debug", "Release", "Dist"))
        self.assertEqual(test_script.SUITES["games"].configurations, ("Debug", "Release", "Dist"))
        self.assert_usage_error("--suite", "unit", "--config", "Dist")
        self.assert_usage_error("--suite", "export", "--config", "Debug")
        self.assert_usage_error("--suite", "feature", "--editor-report", "Feature.json")

    def test_the_user_data_check_names_a_missing_log_and_a_product_folder(self) -> None:
        package = self.directory / "Package"
        logs = exports.testing_user_data(package) / "Logs"
        failures = test_script.check_testing_user_data(package)
        self.assertEqual(len(failures), 1)
        self.assertIn("FeatureTest.log", failures[0])
        logs.mkdir(parents=True)
        (logs / "FeatureTest.log").write_text("log", encoding="utf-8")
        self.assertEqual(test_script.check_testing_user_data(package), [])
        (exports.testing_user_data(package).parent / paths.workspace_name()).mkdir()
        failures = test_script.check_testing_user_data(package)
        self.assertEqual(len(failures), 1)
        self.assertIn("product", failures[0])

    def test_the_merge_needs_both_configurations_and_a_current_editor_report(self) -> None:
        console = Console(json_mode=True)
        release = test_script.ExportedRun(test_script.Step("export Release", Status.PASSED))
        dist = test_script.ExportedRun(test_script.Step("export Dist", Status.PASSED))
        with contextlib.redirect_stderr(io.StringIO()):
            skipped = test_script.run_export_merge({"Release": release}, self.parse("--suite", "export"), console)
            self.assertEqual(skipped.status, Status.SKIPPED)
            self.assertIn("Dist", skipped.detail)
            arguments = self.parse("--suite", "export", "--editor-report", str(self.directory / "Absent.json"))
            missing = test_script.run_export_merge({"Release": release, "Dist": dist}, arguments, console)
        self.assertEqual(missing.status, Status.FAILED)
        self.assertEqual(missing.exit_code, EXIT_INIT_FAILED)
        self.assertIn("is missing", missing.detail)
        self.assertIn("Test.py --suite feature --config Release --junit", missing.detail)

    def test_exported_runs_count_only_with_current_reports(self) -> None:
        results = self.directory / "TestResults"
        results.mkdir()
        replay = "Assets/Tests/Replays/Determinism.replay"
        write_feature_report(results / "FeatureTest-Release.json", "Release",
                             [{"suite": f"replay:{replay}", "passed": True, "finalStateHash": "2a75dca847e98e59"}])
        with mock.patch.object(paths, "TEST_RESULTS_ROOT", results):
            runs, reasons = test_script.read_exported_runs(["Release", "Dist"])
        self.assertEqual(runs, {"Exported Release": {replay: "2a75dca847e98e59"}})
        self.assertEqual(list(reasons), ["Exported Dist"])
        self.assertIn("is missing", reasons["Exported Dist"])
        # A report older than an input of its run (here a project file) does not count.
        stale = results / "FeatureTest-Release.json"
        os.utime(stale, (1_000_000, 1_000_000))
        with mock.patch.object(paths, "TEST_RESULTS_ROOT", results):
            runs, reasons = test_script.read_exported_runs(["Release"])
        self.assertEqual(runs, {})
        self.assertIn("is older than", reasons["Exported Release"])

    def test_determinism_problems_list_every_run_and_its_hash(self) -> None:
        replay = "Assets/Tests/Replays/Determinism.replay"
        committed = {replay: "2a75dca847e98e59"}
        outcome = test_script.ReplayOutcome
        editors = {"Editor Debug": [outcome(replay, "2a75dca847e98e59")],
                   "Editor Release": [outcome(replay, "2a75dca847e98e59")]}
        exported = {"Exported Release": {replay: "2a75dca847e98e59"}, "Exported Dist": {replay: "2a75dca847e98e59"}}
        self.assertEqual(test_script.determinism_problems(committed, editors, exported), {replay: []})
        drifted = dict(exported, **{"Exported Dist": {replay: "00000000000000ff"}})
        problems = test_script.determinism_problems(committed, editors, drifted)[replay]
        self.assertEqual(problems[0], "Exported Dist: 00000000000000ff differs from the committed 2a75dca847e98e59")
        self.assertTrue(problems[1].startswith("runs differ: "), problems)
        self.assertEqual(problems[-1], "hashes: Editor Debug 2a75dca847e98e59, Editor Release 2a75dca847e98e59, "
                                       "Exported Release 2a75dca847e98e59, Exported Dist 00000000000000ff")
        failed = {"Editor Debug": [outcome(replay, "", ["input.replay failed: replay failed: quit"])],
                  "Editor Release": [outcome(replay, "2a75dca847e98e59")]}
        problems = test_script.determinism_problems(committed, failed, {})[replay]
        self.assertEqual(problems[:2],
                         ["Editor Debug: input.replay failed: replay failed: quit", "Editor Debug: no hash"])

    def test_every_exported_run_must_count_its_debug_break(self) -> None:
        # Roadmap M15's "Debug.Break counted no-op" in the shipping binary (ADR 0024 decision 6): the Api.Debug case
        # only shows that time keeps running after Debug.Break, so the export suite reads the row's break count.
        for config in exports.EXPORTED_CONFIGURATIONS:
            with self.subTest(config=config):
                row = {"suite": "Api.Debug", "passed": True, "breaks": 1}
                counted = write_feature_report(self.directory / f"{config}.json", config, [row])
                self.assertEqual(exports.debug_break_failures(counted), [])
                for breaks in (0, 2, True, None):
                    write_feature_report(counted, config, [{**row, "breaks": breaks}])
                    failures = exports.debug_break_failures(counted)
                    self.assertEqual(len(failures), 1, breaks)
                    self.assertIn("Api.Debug suite counted", failures[0])
                write_feature_report(counted, config, [{"suite": "Api.Time", "passed": True, "breaks": 0}])
                self.assertIn("0 Api.Debug suite rows", exports.debug_break_failures(counted)[0])
        (self.directory / "Broken.json").write_text("{", encoding="utf-8")
        with self.assertRaises(ValueError):
            exports.debug_break_failures(self.directory / "Broken.json")

    def test_an_exported_run_without_its_counted_debug_break_fails(self) -> None:
        # The export suite's configuration step, with the editor, the package's run and the six gates faked: a report
        # whose Api.Debug row counts one break passes, one that counts none fails naming it.
        def export_step(config: str, breaks: int) -> test_script.Step:
            output = self.directory / f"Export-{config}-{breaks}"
            package_root = self.directory / f"Package-{config}-{breaks}"
            executable = exports.package_executable(output)
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(b"")
            log = exports.testing_user_data(output) / "Logs" / f"{exports.FEATURE_TEST_NAME}.log"
            log.parent.mkdir(parents=True)
            log.write_text("log", encoding="utf-8")
            reports = self.directory / f"Reports-{config}-{breaks}"
            client = mock.Mock()
            client.call.return_value = {"outputDirectory": str(output), "files": [], "testSuiteCount": 1,
                                        "replayCount": 0}

            def run(command: list[str], **options: object) -> object:
                report = reports / "bin" / "TestResults" / f"{exports.FEATURE_TEST_NAME}-{config}.json"
                report.parent.mkdir(parents=True, exist_ok=True)
                write_feature_report(report, config, [{"suite": "Api.Debug", "passed": True, "breaks": breaks}])
                return ProcessResult(command, 0, False, 1.0, "")

            with (mock.patch.object(exports, "package_directory", return_value=package_root),
                  mock.patch.object(test_script, "run_streamed", run),
                  mock.patch.object(test_script.coverage, "evaluate_feature_report", return_value=[]),
                  contextlib.redirect_stderr(io.StringIO())):
                return test_script.run_export_configuration(client, engine_client, config, self.parse(),
                                                            Console(json_mode=True), reports).step

        for config in exports.EXPORTED_CONFIGURATIONS:
            with self.subTest(config=config):
                self.assertEqual(export_step(config, 1).status, Status.PASSED)
                failed = export_step(config, 0)
                self.assertEqual(failed.status, Status.FAILED)
                self.assertIn("Api.Debug suite counted 0 breaks, not 1", failed.detail)

    def test_the_default_suites_skip_a_configuration_a_suite_does_not_take(self) -> None:
        # `Test.py --config Debug` runs every suite that has a Debug run and lists the export suite's Debug as skipped;
        # a suite named explicitly with a configuration it does not take stays a usage error. Every suite exists since
        # the games suite became available (ADR 0026 decision 8), and it takes all three configurations.
        ran: list[tuple[str, tuple[str, ...]]] = []

        def suite(name: str, config: str, arguments: argparse.Namespace, console: Console) -> object:
            ran.append((name, (config,)))
            yield test_script.Step(f"{name} {config}", Status.PASSED)

        def determinism(configs: list[str], arguments: argparse.Namespace, console: Console) -> object:
            ran.append(("determinism", tuple(configs)))
            return test_script.Step(f"determinism {', '.join(configs)}", Status.PASSED)

        def export(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[object]:
            ran.append(("export", tuple(configs)))
            return [test_script.Step(f"export {config}", Status.PASSED) for config in configs]

        def games(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[object]:
            ran.append(("games", tuple(configs)))
            return [test_script.Step(f"games {', '.join(configs)}", Status.PASSED)]

        def main(*argv: str) -> tuple[int, dict]:
            ran.clear()
            output = io.StringIO()
            with (mock.patch.object(test_script, "run_suite", suite),
                  mock.patch.object(test_script, "run_determinism", determinism),
                  mock.patch.object(test_script, "run_export", export),
                  mock.patch.object(test_script, "run_games", games),
                  contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO())):
                exit_code = test_script.main([*argv, "--json"])
            return exit_code, json.loads(output.getvalue())

        exit_code, document = main("--config", "Debug")
        self.assertEqual(exit_code, 0)
        self.assertEqual(ran, [("unit", ("Debug",)), ("gpu", ("Debug",)), ("golden", ("Debug",)),
                               ("feature", ("Debug",)), ("automation", ("Debug",)), ("determinism", ("Debug",)),
                               ("games", ("Debug",))])
        statuses = {step["name"]: step["status"] for step in document["steps"]}
        self.assertEqual(statuses["export Debug"], "skipped")
        self.assertEqual(statuses["games Debug"], "passed")
        self.assertNotIn("not-available", statuses.values())
        skipped = next(step for step in document["steps"] if step["name"] == "export Debug")
        self.assertIn("not run in Debug: the export suite takes Release, Dist", skipped["detail"])
        exit_code, document = main("--config", "Debug,Release")
        self.assertIn(("export", ("Release",)), ran)
        self.assertIn(("determinism", ("Debug", "Release")), ran)
        self.assertIn(("games", ("Debug", "Release")), ran)
        self.assertEqual([step["name"] for step in document["steps"] if step["status"] == "skipped"],
                         ["export Debug"])
        exit_code, document = main("--config", "Dist")
        self.assertEqual(ran, [("export", ("Dist",)), ("determinism", ("Dist",)), ("games", ("Dist",))])
        # unit, gpu, golden, feature and automation do not run in Dist; export, determinism and games do.
        self.assertEqual([step["name"] for step in document["steps"] if step["status"] == "skipped"],
                         ["unit Dist", "gpu Dist", "golden Dist", "feature Dist", "automation Dist"])
        self.assert_usage_error("--suite", "export", "--config", "Debug")
        self.assert_usage_error("--suite", "unit,export", "--config", "Debug")
        self.assertEqual(self.parse("--suite", "unit,export", "--config", "Release").configs, ["Release"])

    def test_the_feature_determinism_and_export_editors_read_featuretest_strictly(self) -> None:
        # Architecture §6: --strict turns unknown fields and components of the committed project into errors.
        command = test_script.feature_command(Path("Editor"), self.directory / "Feature.xml", self.directory / "Data")
        self.assertIn("--strict", command)
        self.assertEqual(command[command.index("--project") + 1], str(test_script.FEATURE_PROJECT))
        self.assertIn("--run-tests", command)
        self.assertIn("--strict", test_script.EXPORT_EDITOR_ARGUMENTS)
        launched: list[list[str]] = []

        class Stop(Exception):
            pass

        def launch(editor: Path, arguments: list[str], *rest: object, **options: object) -> object:
            launched.append(arguments)
            raise Stop

        fake = mock.Mock(launch_editor=launch)
        with mock.patch.object(test_script, "import_engine_client", return_value=fake), self.assertRaises(Stop):
            test_script.verify_replays(Path("Editor"), self.directory / "P.eproj", [], self.directory / "Data", 1.0)
        self.assertEqual(launched, [["--headless", "--renderer", "none", "--audio-device", "none", "--automation",
                                     "--strict", "--project", str(self.directory / "P.eproj")]])


if __name__ == "__main__":
    unittest.main()
