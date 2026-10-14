"""Scripts/Lib/games.py and the games suite and stage (Docs/Architecture.md §15.1 T5, §15.8; Roadmap M16 to M18;
Docs/Decisions/0026-m16-contract.md decisions 7 and 8, stream B): project discovery and the Testing settings, the
evaluation of a test run, of script.check, project.validate, the upgrade dry run and the smoke export; the suite's
registration, steps, device and build rules and outputs in Test.py, the editor half's checks against a faked editor,
the CI.py stage and PreCommit's Debug run (recorded, nothing run); and an end-to-end run of the editor half on a small
game built through automation in the test.
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
import types
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock
from xml.etree import ElementTree

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config
from tiny_game import TINY_SCENE, build_tiny_game

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CI  # noqa: E402  (Scripts/ is on sys.path above)
import PreCommit  # noqa: E402
import Test as test_script  # noqa: E402
from Lib import games, hardening, paths, provenance  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402

SKIP_LIB = "contract stub: games (ADR 0026 decision 7, stream B)"
SKIP_SUITE = "contract stub: games suite and stage (ADR 0026 decision 8, stream B)"
SMOKE_SUITE = "Assets/Tests/Smoke.test.luau"
EDITOR_ONLY_SUITE = "Assets/Tests/EditorOnly.test.luau"
REPLAY = "Assets/Tests/Replays/Rally.replay"
REPLAY_HASH = "0123456789abcdef"


def git_stage(repository: Path, initialize: bool = False) -> None:
    """Stages every file of `repository` (initializing it first when asked), as a commit about to be made holds them;
    the suite's editor half materializes the index (decision 8). No system or user configuration is read."""
    environment = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull}
    commands = ([["init", "--quiet"]] if initialize else []) + [["add", "--all"]]
    for command in commands:
        subprocess.run(["git", "-C", str(repository), *command], check=True, capture_output=True, env=environment,
                       timeout=120)


def write_project(root: Path, relative: str, testing: dict | None = None) -> Path:
    """<root>/<relative>/<name>.eproj with `testing` as its Testing member; read_testing lets the rest be absent."""
    directory = root / relative
    directory.mkdir(parents=True, exist_ok=True)
    project_file = directory / f"{directory.name}.eproj"
    document = {"Format": "Project", "Version": 1, "Name": directory.name}
    if testing is not None:
        document["Testing"] = testing
    project_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
    return project_file


def run_document(mode: str, suites: list[dict], passed: bool = True, cases: list[dict] | None = None) -> dict:
    """A TestRunResult as test.run returns it and --feature-test writes it."""
    return {"mode": mode, "passed": passed, "cancelled": False, "cases": cases or [], "suites": suites,
            "coverage": {"registryFingerprint": "0000000000000000", "counters": [], "gates": []}}


def suite_row(script: str, passed: bool = True) -> dict:
    return {"suite": Path(script).stem, "script": script, "passed": passed, "finalStateHash": ""}


def replay_row(path: str, state_hash: str, passed: bool = True) -> dict:
    return {"suite": f"{games.REPLAY_SUITE_PREFIX}{path}", "script": "", "passed": passed, "finalStateHash": state_hash}


TESTING = games.TestingSettings(
    suites=(games.SuiteEntry(SMOKE_SUITE, TINY_SCENE, games.DEFAULT_SUITE_MODES),
            games.SuiteEntry(EDITOR_ONLY_SUITE, TINY_SCENE, ("Editor",))),
    replays=("Assets/Tests/Replays/**",))


def write_replay(project: Path, relative: str = REPLAY, state_hash: str = REPLAY_HASH) -> None:
    """A replay header as committed: its FinalStateHash is what every run must reproduce."""
    replay = project / relative
    replay.parent.mkdir(parents=True, exist_ok=True)
    replay.write_text(json.dumps({"Format": "Replay", "Version": 1, "FinalTick": 600, "FinalStateHash": state_hash},
                                 indent="\t") + "\n", encoding="utf-8")


def make_game_repository(root: Path, stray_replay: bool = False) -> games.GameProject:
    """A git repository at `root` with the game <root>/Tests/Projects/Game staged: one suite, the replay glob and the
    replay with REPLAY_HASH, and with `stray_replay` a replay below Assets/ that Testing.Replays does not match."""
    project_file = write_project(root, "Tests/Projects/Game", {
        "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}], "Replays": ["Assets/Tests/Replays/**"]})
    write_replay(project_file.parent)
    if stray_replay:
        write_replay(project_file.parent, "Assets/Stray.replay")
    git_stage(root, initialize=True)
    return games.GameProject(project_file.parent, project_file, "Game")


class FakeEngineError(Exception):
    pass


class FakeEditorSession:
    """The editor half's editor, faked through Test.py's import_engine_client seam (the editor halves start editors
    through it like the export and determinism suites, decision 8): canned results per method, a hook run on test.run
    with the copy's directory, and the exit code the process ends with after session.shutdown. It is the process and
    the client at once."""

    def __init__(self, exit_code: int = 0, on_test_run: Callable[[Path], None] | None = None) -> None:
        self.exit_code = exit_code
        self.on_test_run = on_test_run
        self.arguments: list[str] = []
        self.methods: list[str] = []
        self.document = run_document("Editor", [suite_row(SMOKE_SUITE), replay_row(REPLAY, REPLAY_HASH)])

    def module(self) -> types.SimpleNamespace:
        return types.SimpleNamespace(EngineError=FakeEngineError, ConnectionClosed=ConnectionError,
                                     ProtocolError=ValueError, CallLog=lambda *arguments: None,
                                     launch_editor=self.launch_editor, load_offloaded=lambda result: result)

    def launch_editor(self, editor: Path, arguments: list[str], *rest: object, **options: object) -> FakeEditorSession:
        self.arguments = list(arguments)
        return self

    def project_directory(self) -> Path:
        return Path(self.arguments[self.arguments.index("--project") + 1]).parent

    def connect(self, client_name: str, call_log: object = None) -> FakeEditorSession:
        return self

    def __enter__(self) -> FakeEditorSession:
        return self

    def __exit__(self, *exception: object) -> None:
        return None

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        self.methods.append(method)
        if method == "test.run" and self.on_test_run is not None:
            self.on_test_run(self.project_directory())
        results: dict[str, dict[str, Any]] = {
            "script.check": {"diagnostics": []},
            "project.validate": {"diagnostics": [], "errorCount": 0, "warningCount": 0},
            "project.upgrade": {"changedFiles": [], "unchangedCount": 3},
            "test.run": self.document,
        }
        return results.get(method, {})

    def wait(self, timeout: float | None = None) -> int:
        return self.exit_code

    def kill(self) -> None:
        return None

    def output(self) -> str:
        return ""


class DiscoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="Games-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    @unittest.skip(SKIP_LIB)
    def test_projects_below_both_roots_are_found_and_feature_test_is_excluded(self) -> None:
        write_project(self.root, "Projects/FeatureTest")
        write_project(self.root, "Projects/Tetris")
        write_project(self.root, "Tests/Projects/Breakout")
        (self.root / "Projects" / "NoProject").mkdir()
        (self.root / "Projects" / "Notes.txt").write_text("not a project", encoding="utf-8")
        found = games.discover_projects(self.root)
        self.assertEqual([project.name for project in found], ["Tetris", "Breakout"])
        self.assertEqual(found[1], games.GameProject(self.root / "Tests" / "Projects" / "Breakout",
                                                     self.root / "Tests" / "Projects" / "Breakout" / "Breakout.eproj",
                                                     "Breakout"))

    @unittest.skip(SKIP_LIB)
    def test_a_directory_with_two_project_files_is_refused(self) -> None:
        write_project(self.root, "Projects/Twice")
        (self.root / "Projects" / "Twice" / "Other.eproj").write_text("{}", encoding="utf-8")
        with self.assertRaises(ValueError) as raised:
            games.discover_projects(self.root)
        self.assertIn("Twice", str(raised.exception))

    @unittest.skip(SKIP_LIB)
    def test_no_project_is_an_empty_list(self) -> None:
        self.assertEqual(games.discover_projects(self.root), [])
        write_project(self.root, "Projects/FeatureTest")
        self.assertEqual(games.discover_projects(self.root), [])

    @unittest.skip(SKIP_LIB)
    def test_index_discovery_lists_only_staged_projects(self) -> None:
        write_project(self.root, "Tests/Projects/Staged")
        git_stage(self.root, initialize=True)
        write_project(self.root, "Projects/Untracked")
        self.assertEqual([project.name for project in games.discover_projects(self.root, "disk")],
                         ["Untracked", "Staged"])
        self.assertEqual([project.name for project in games.discover_projects(self.root, "index")], ["Staged"])
        with self.assertRaises(ValueError):
            games.discover_projects(self.root, "git")

    @unittest.skip(SKIP_LIB)
    def test_the_soak_keeps_its_own_project_list(self) -> None:
        # FeatureTest stays soaked and the dry run under Tests/Projects is not (ADR 0025): the soak never uses games.
        write_project(self.root, "Projects/FeatureTest")
        write_project(self.root, "Tests/Projects/Breakout")
        self.assertEqual([path.name for path in hardening.soak_projects(self.root)], ["FeatureTest"])
        self.assertEqual([project.name for project in games.discover_projects(self.root)], ["Breakout"])


class TestingSettingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="GamesTesting-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    @unittest.skip(SKIP_LIB)
    def test_read_testing_applies_the_default_modes(self) -> None:
        project_file = write_project(self.root, "Projects/Game", {
            "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE},
                       {"Script": EDITOR_ONLY_SUITE, "Scene": TINY_SCENE, "Modes": ["Editor"]}],
            "Replays": ["project://Assets/Tests/Replays/**"]})
        testing = games.read_testing(project_file)
        self.assertEqual(testing.suites, TESTING.suites)
        self.assertEqual(testing.replays, ("project://Assets/Tests/Replays/**",))
        self.assertEqual(games.read_testing(write_project(self.root, "Projects/Bare")),
                         games.TestingSettings(suites=(), replays=()))

    @unittest.skip(SKIP_LIB)
    def test_committed_hashes_are_read_from_each_replay_header(self) -> None:
        project = self.root / "Projects" / "Game"
        replay = project / REPLAY
        replay.parent.mkdir(parents=True)
        replay.write_text(json.dumps({"Format": "Replay", "Version": 1, "FinalTick": 600,
                                      "FinalStateHash": REPLAY_HASH}, indent="\t") + "\n", encoding="utf-8")
        self.assertEqual(games.committed_hashes(project, [REPLAY]), {REPLAY: REPLAY_HASH})
        replay.write_text(json.dumps({"Format": "Replay", "Version": 1}), encoding="utf-8")
        with self.assertRaises(ValueError) as raised:
            games.committed_hashes(project, [REPLAY])
        self.assertIn(REPLAY, str(raised.exception))


class EvaluationTests(unittest.TestCase):
    @unittest.skip(SKIP_LIB)
    def test_a_complete_run_passes(self) -> None:
        editor = run_document("Editor", [suite_row(SMOKE_SUITE), suite_row(EDITOR_ONLY_SUITE),
                                         replay_row(REPLAY, REPLAY_HASH)])
        self.assertEqual(games.evaluate_run(editor, "Editor", TESTING, {REPLAY: REPLAY_HASH}), [])
        # A Dist run leaves the Editor-only suite out.
        dist = run_document("Dist", [suite_row(SMOKE_SUITE), replay_row(REPLAY, REPLAY_HASH)])
        self.assertEqual(games.evaluate_run(dist, "Dist", TESTING, {REPLAY: REPLAY_HASH}), [])

    @unittest.skip(SKIP_LIB)
    def test_each_defect_of_a_run_is_named(self) -> None:
        committed = {REPLAY: REPLAY_HASH}
        complete = [suite_row(SMOKE_SUITE), suite_row(EDITOR_ONLY_SUITE), replay_row(REPLAY, REPLAY_HASH)]
        defects = {
            "mode": (run_document("Release", complete), TESTING),
            "passed": (run_document("Editor", complete, passed=False), TESTING),
            "cancelled": ({**run_document("Editor", complete), "cancelled": True}, TESTING),
            "skipped": (run_document("Editor", complete, cases=[{"suite": "Smoke", "case": "passes",
                                                                 "status": "skipped"}]), TESTING),
            "no suite": (run_document("Editor", complete), games.TestingSettings(suites=(), replays=())),
            SMOKE_SUITE: (run_document("Editor", complete[1:]), TESTING),
            REPLAY: (run_document("Editor", complete[:2]), TESTING),
            "failed replay": (run_document("Editor", [*complete[:2], replay_row(REPLAY, REPLAY_HASH, False)]),
                              TESTING),
            "fedcba9876543210": (run_document("Editor", [*complete[:2], replay_row(REPLAY, "fedcba9876543210")]),
                                 TESTING),
        }
        for name, (document, testing) in defects.items():
            with self.subTest(defect=name):
                failures = games.evaluate_run(document, "Editor", testing, committed)
                self.assertTrue(failures)
                if "/" in name or name.isalnum():
                    self.assertTrue(any(name in failure for failure in failures), failures)

    @unittest.skip(SKIP_LIB)
    def test_a_replay_that_testing_replays_does_not_match_is_named(self) -> None:
        with tempfile.TemporaryDirectory(prefix="GamesReplays-") as directory:
            project = Path(directory)
            for relative in (REPLAY, "Assets/Stray.replay", "Library/Cache/Old.replay"):
                write_replay(project, relative)
            self.assertEqual(games.check_replays_matched(project, [REPLAY, "Assets/Stray.replay"]), [])
            failures = games.check_replays_matched(project, [REPLAY])
            self.assertEqual(len(failures), 1)
            self.assertIn("Assets/Stray.replay", failures[0])

    @unittest.skip(SKIP_LIB)
    def test_the_dist_half_compares_every_run_with_the_committed_hashes(self) -> None:
        committed = {REPLAY: REPLAY_HASH}
        same = {"Editor Debug": {REPLAY: REPLAY_HASH}, "Editor Release": {REPLAY: REPLAY_HASH},
                "Exported Dist": {REPLAY: REPLAY_HASH}}
        self.assertEqual(games.compare_game_hashes(committed, same), [])
        for run in ("Exported Dist", "Editor Release"):
            with self.subTest(differs=run):
                failures = games.compare_game_hashes(committed, {**same, run: {REPLAY: "fedcba9876543210"}})
                self.assertTrue(failures)
                self.assertTrue(all(failure.startswith(f"{REPLAY}: ") for failure in failures), failures)
                self.assertTrue(any(run in failure for failure in failures), failures)

    @unittest.skip(SKIP_LIB)
    def test_script_check_and_validate_fail_on_any_diagnostic(self) -> None:
        self.assertEqual(games.check_script_check({"diagnostics": []}), [])
        self.assertEqual(len(games.check_script_check({"diagnostics": [
            {"file": "Assets/Scripts/Ball.luau", "line": 3, "code": "SCRIPT_TYPE_ERROR", "message": "x"}]})), 1)
        self.assertEqual(games.check_validate({"diagnostics": [], "errorCount": 0, "warningCount": 0}), [])
        warning = {"severity": "Warning", "code": "PHYSICS_ADJACENT_STATIC_BODIES", "message": "merge them",
                   "file": "Assets/Scenes/Main.scene", "asset": ""}
        failures = games.check_validate({"diagnostics": [warning], "errorCount": 0, "warningCount": 1})
        self.assertEqual(len(failures), 1)
        self.assertIn("PHYSICS_ADJACENT_STATIC_BODIES", failures[0])

    @unittest.skip(SKIP_LIB)
    def test_the_upgrade_dry_run_names_each_changed_file_and_the_upgrade_command(self) -> None:
        project = games.GameProject(REPOSITORY_ROOT / "Tests" / "Projects" / "Breakout",
                                    REPOSITORY_ROOT / "Tests" / "Projects" / "Breakout" / "Breakout.eproj", "Breakout")
        self.assertEqual(games.check_upgrade_dry_run({"changedFiles": [], "unchangedCount": 9}, project), [])
        failures = games.check_upgrade_dry_run({"changedFiles": ["Assets/Scenes/Main.scene", "Breakout.eproj"],
                                                "unchangedCount": 7}, project)
        self.assertEqual(len(failures), 2)
        for failure, changed in zip(failures, ["Assets/Scenes/Main.scene", "Breakout.eproj"]):
            self.assertIn(changed, failure)
            self.assertIn("Editor --headless --project Tests/Projects/Breakout --upgrade", failure)

    @unittest.skip(SKIP_LIB)
    def test_a_smoke_test_that_did_not_run_warns_without_a_device_and_fails_with_require_gpu(self) -> None:
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        self.assertEqual(games.check_smoke(ran, require_gpu=True), ([], []))
        self.assertEqual(len(games.check_smoke({**ran, "smokeTestExitCode": 1}, require_gpu=False)[0]), 1)
        not_run = {"smokeTestRan": False, "smokeTestExitCode": 0, "warnings": ["no Vulkan device"]}
        failures, warnings = games.check_smoke(not_run, require_gpu=False)
        self.assertEqual((failures, len(warnings)), ([], 1))
        failures, warnings = games.check_smoke(not_run, require_gpu=True)
        self.assertEqual((len(failures), warnings), (1, []))

    @unittest.skip(SKIP_LIB)
    def test_report_paths_are_named_per_project_and_configuration(self) -> None:
        project = games.GameProject(Path("Breakout"), Path("Breakout/Breakout.eproj"), "Breakout")
        results = paths.TEST_RESULTS_ROOT
        self.assertEqual(games.editor_report_path(project, "Debug"), results / "Games-Breakout-Editor-Debug.json")
        self.assertEqual(games.dist_report_path(project, ".xml"), results / "Games-Breakout-Dist.xml")
        self.assertEqual(games.junit_path("Release"), results / "Games-Release.xml")


class SuiteTests(unittest.TestCase):
    @unittest.skip(SKIP_SUITE)
    def test_the_suite_is_available_in_debug_release_and_dist(self) -> None:
        suite = test_script.SUITES["games"]
        self.assertTrue(suite.available)
        self.assertEqual(suite.configurations, ("Debug", "Release", "Dist"))
        self.assertEqual(suite.allowed, paths.CONFIGURATIONS)

    @unittest.skip(SKIP_SUITE)
    def test_main_hands_the_requested_configurations_to_run_games(self) -> None:
        ran: list[tuple[str, ...]] = []

        def run_games(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[Step]:
            ran.append(tuple(configs))
            return [Step("games", Status.PASSED, "recorded")]

        with mock.patch.object(test_script, "run_games", run_games), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(test_script.main(["--suite", "games", "--config", "Debug", "--json"]), 0)
            self.assertEqual(test_script.main(["--suite", "games", "--json"]), 0)
        self.assertEqual(ran, [("Debug",), ("Debug", "Release", "Dist")])

    @unittest.skip(SKIP_SUITE)
    def test_without_a_game_project_the_suite_passes_with_a_note(self) -> None:
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects", return_value=[]):
            steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        self.assertEqual([(step.name, step.status) for step in steps], [("games", Status.PASSED)])
        self.assertIn("no game project", steps[0].detail)

    @unittest.skip(SKIP_SUITE)
    def test_without_a_device_the_audit_runs_and_the_editor_and_dist_steps_warn_or_fail_with_require_gpu(self) -> None:
        project = games.GameProject(REPOSITORY_ROOT / "Tests" / "Projects" / "Game",
                                    REPOSITORY_ROOT / "Tests" / "Projects" / "Game" / "Game.eproj", "Game")
        passing = provenance.ProjectAudit(project.directory, "index", {"files": 1, "entries": 1, "transcriptLines": 2,
                                                                       "requests": 1}, [], [])
        for require_gpu, expected in ((False, Status.WARNING), (True, Status.FAILED)):
            with self.subTest(require_gpu=require_gpu):
                argv = ["--suite", "games", "--config", "Release,Dist", *(["--require-gpu"] if require_gpu else [])]
                arguments = test_script.parse_arguments(argv)
                # The builds are looked up first (the probe needs an editor) and exist here; nothing is copied
                # before the device decision.
                with mock.patch.object(games, "discover_projects", return_value=[project]), \
                        mock.patch.object(provenance, "audit_project", return_value=passing) as audit, \
                        mock.patch.object(provenance, "materialize_index") as materialize, \
                        mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                        mock.patch.object(test_script, "runtime_executable", return_value=Path(sys.executable)), \
                        mock.patch.object(test_script, "probe_rendering_editor", return_value="no Vulkan device"):
                    steps = test_script.run_games(["Release", "Dist"], arguments, Console(json_mode=True))
                audit.assert_called_once_with(project.directory, "index")
                materialize.assert_not_called()
                statuses = {step.name: step.status for step in steps}
                self.assertEqual(statuses["games Game audit"], Status.PASSED)
                self.assertEqual(statuses["games Game Editor Release"], expected)
                self.assertEqual(statuses["games Game Dist"], expected)

    @unittest.skip(SKIP_SUITE)
    def test_a_failing_audit_fails_its_step_naming_each_finding(self) -> None:
        project = games.GameProject(REPOSITORY_ROOT / "Tests" / "Projects" / "Game",
                                    REPOSITORY_ROOT / "Tests" / "Projects" / "Game" / "Game.eproj", "Game")
        finding = provenance.Finding("hash-mismatch", "Assets/Scenes/Main.scene", "changed outside the editor")
        failing = provenance.ProjectAudit(project.directory, "index", {}, [finding], [])
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects", return_value=[project]), \
                mock.patch.object(provenance, "audit_project", return_value=failing), \
                mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                mock.patch.object(test_script, "probe_rendering_editor", return_value="no Vulkan device"):
            steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        audit_step = next(step for step in steps if step.name == "games Game audit")
        self.assertEqual(audit_step.status, Status.FAILED)
        self.assertIn("hash-mismatch", audit_step.detail)
        self.assertIn("Assets/Scenes/Main.scene", audit_step.detail)

    @unittest.skip(SKIP_SUITE)
    def test_an_unstaged_project_is_named_and_neither_audited_nor_run(self) -> None:
        with tempfile.TemporaryDirectory(prefix="GamesUnstaged-") as directory:
            root = Path(directory)
            write_project(root, "Tests/Projects/Staged")
            git_stage(root, initialize=True)
            write_project(root, "Projects/Untracked")
            discover = games.discover_projects

            def discover_in_repository(_: Path, files: str = "disk") -> list[games.GameProject]:
                return discover(root, files)

            staged = root / "Tests" / "Projects" / "Staged"
            passing = provenance.ProjectAudit(staged, "index", {}, [], [])
            arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
            with mock.patch.object(games, "discover_projects", side_effect=discover_in_repository), \
                    mock.patch.object(provenance, "audit_project", return_value=passing) as audit, \
                    mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "probe_rendering_editor", return_value="no Vulkan device"):
                steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        audit.assert_called_once_with(staged, "index")
        notes = [step for step in steps if step.name.startswith("games Untracked")]
        self.assertEqual([(step.name, step.status) for step in notes], [("games Untracked", Status.PASSED)])
        self.assertIn("not staged", notes[0].detail)

    @unittest.skip(SKIP_SUITE)
    def test_a_directory_with_several_project_files_fails_the_suite(self) -> None:
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects",
                               side_effect=ValueError("Tests/Projects/Twice holds 2 *.eproj files")):
            steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        self.assertEqual([(step.name, step.status) for step in steps], [("games", Status.FAILED)])
        self.assertIn("Tests/Projects/Twice", steps[0].detail)

    @unittest.skip(SKIP_SUITE)
    def test_a_missing_build_fails_with_exit_code_3_and_the_build_hint(self) -> None:
        project = games.GameProject(REPOSITORY_ROOT / "Tests" / "Projects" / "Game",
                                    REPOSITORY_ROOT / "Tests" / "Projects" / "Game" / "Game.eproj", "Game")
        passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with tempfile.TemporaryDirectory(prefix="GamesNoBuild-") as directory:
            missing = Path(directory) / "Editor" / "Editor.exe"
            with mock.patch.object(games, "discover_projects", return_value=[project]), \
                    mock.patch.object(provenance, "audit_project", return_value=passing), \
                    mock.patch.object(test_script, "editor_executable", return_value=missing), \
                    mock.patch.object(test_script, "probe_rendering_editor") as probe:
                steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        probe.assert_not_called()
        statuses = {step.name: step for step in steps}
        self.assertEqual(statuses["games Game audit"].status, Status.PASSED)
        editor_step = statuses["games Game Editor Debug"]
        self.assertEqual((editor_step.status, editor_step.exit_code), (Status.FAILED, 3))
        self.assertIn("Build.py", editor_step.detail)

    @unittest.skip(SKIP_SUITE)
    def test_with_junit_the_audit_steps_go_into_the_first_requested_configuration_file(self) -> None:
        project = games.GameProject(REPOSITORY_ROOT / "Tests" / "Projects" / "Game",
                                    REPOSITORY_ROOT / "Tests" / "Projects" / "Game" / "Game.eproj", "Game")
        passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Release,Dist", "--junit"])
        with tempfile.TemporaryDirectory(prefix="GamesJunit-") as directory:
            results = Path(directory)

            def junit_path(config: str) -> Path:
                return results / f"Games-{config}.xml"

            with mock.patch.object(games, "discover_projects", return_value=[project]), \
                    mock.patch.object(provenance, "audit_project", return_value=passing), \
                    mock.patch.object(games, "junit_path", side_effect=junit_path), \
                    mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "runtime_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "probe_rendering_editor", return_value="no Vulkan device"):
                test_script.run_games(["Release", "Dist"], arguments, Console(json_mode=True))
            cases = {config: [case.get("name") for case in
                              ElementTree.parse(results / f"Games-{config}.xml").getroot().iter("testcase")]
                     for config in ("Release", "Dist")}
        self.assertEqual(cases["Release"], ["games Game audit", "games Game Editor Release"])
        self.assertEqual(cases["Dist"], ["games Game Dist"])

    def run_editor_half(self, project: games.GameProject, session: FakeEditorSession) -> Step:
        """run_games in Debug on `project` with a device and `session` as the editor; its "games Game Editor Debug"
        step."""
        passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects", return_value=[project]), \
                mock.patch.object(provenance, "audit_project", return_value=passing), \
                mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                mock.patch.object(test_script, "probe_rendering_editor", return_value=None), \
                mock.patch.object(test_script, "import_engine_client", return_value=session.module()):
            steps = test_script.run_games(["Debug"], arguments, Console(json_mode=True))
        return next(step for step in steps if step.name == f"games {project.name} Editor Debug")

    @unittest.skip(SKIP_SUITE)
    def test_the_editor_half_checks_a_strict_editor_on_an_index_copy(self) -> None:
        with tempfile.TemporaryDirectory(prefix="GamesEditorHalf-") as directory:
            project = make_game_repository(Path(directory))
            session = FakeEditorSession()
            step = self.run_editor_half(project, session)
            self.assertEqual(step.status, Status.PASSED, step.detail)
            for argument in ("--strict", "--expect-no-gpu-errors"):
                self.assertIn(argument, session.arguments)
            self.assertEqual(session.arguments[session.arguments.index("--renderer") + 1], "vulkan")
            self.assertFalse(session.project_directory().is_relative_to(project.directory))
        order = ["script.check", "project.validate", "project.upgrade", "test.run", "session.shutdown"]
        self.assertEqual([method for method in session.methods if method in order], order)

    @unittest.skip(SKIP_SUITE)
    def test_the_editor_half_fails_on_a_stray_replay_a_changed_copy_or_a_failed_shutdown(self) -> None:
        def change_copy(copy: Path) -> None:
            (copy / "Assets" / "Scenes").mkdir(parents=True, exist_ok=True)
            (copy / "Assets" / "Scenes" / "Main.scene").write_text("{}", encoding="utf-8")
            (copy / "Library" / "Cache").mkdir(parents=True, exist_ok=True)
            (copy / "Library" / "Cache" / "Any.bin").write_bytes(b"cache")

        cases = {"Assets/Stray.replay": (True, FakeEditorSession()),
                 "Assets/Scenes/Main.scene": (False, FakeEditorSession(on_test_run=change_copy)),
                 "exit": (False, FakeEditorSession(exit_code=1))}
        for name, (stray_replay, session) in cases.items():
            with self.subTest(defect=name), tempfile.TemporaryDirectory(prefix="GamesEditorHalf-") as directory:
                project = make_game_repository(Path(directory), stray_replay=stray_replay)
                step = self.run_editor_half(project, session)
                self.assertEqual(step.status, Status.FAILED)
                if "/" in name:
                    self.assertIn(name, step.detail)
                self.assertNotIn("Library", step.detail)

    @unittest.skip(SKIP_SUITE)
    def test_ci_runs_the_games_stage_over_debug_release_and_dist(self) -> None:
        stage = CI.STAGE_BY_NAME["games"]
        self.assertTrue(stage.available)
        self.assertEqual(stage.configurations, ("Debug", "Release", "Dist"))
        self.assertEqual(CI.TIMEOUTS["games"], 3600.0)
        self.assertIn("games", CI.parse_arguments(["--milestone"]).selected)
        self.assertIn("games", CI.parse_arguments([]).selected)
        for options, expected in (([], ["--require-gpu"]), (["--gpu-optional"], []),
                                  (["--contract"], ["--require-gpu", "--allow-skips"])):
            with self.subTest(options=options):
                calls: list[tuple[str, str, list[str]]] = []
                runner = CI.Runner(CI.parse_arguments(options), Console(json_mode=True))

                def script(name: str, script_name: str, script_arguments: list[str], timeout: float,
                           expected_exit: int = 0, expected_output: object = None) -> Step:
                    calls.append((name, script_name, list(script_arguments)))
                    return Step(name, Status.PASSED, "recorded")

                with mock.patch.object(runner, "script", script):
                    runner.games(["Debug", "Release", "Dist"])
                self.assertEqual(calls, [("games Debug, Release, Dist", "Test.py",
                                          ["--suite", "games", "--config", "Debug,Release,Dist", "--junit",
                                           *expected])])

    @unittest.skip(SKIP_SUITE)
    def test_precommit_runs_the_games_suite_in_debug(self) -> None:
        calls: list[tuple[str, str, list[str]]] = []

        def run_step(name: str, script: str, arguments: list[str], console: Console, *_: object) -> Step:
            calls.append((name, script, list(arguments)))
            return Step(name, Status.PASSED, "recorded")

        with mock.patch.object(PreCommit, "run_step", run_step), \
                mock.patch.object(PreCommit, "run_static_checks", return_value=[]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(PreCommit.main(["--json"]), 0)
        tests = next(arguments for name, _, arguments in calls if name == "tests")
        self.assertEqual(tests[:4], ["--suite", "unit,gpu,golden,feature,automation,games", "--config", "Debug"])


class GamesSuiteEndToEndTests(AutomationTestCase):
    """The editor half on a game built through automation in the test (the tiny game with one suite and one replay
    recorded from tick 0), staged in a temporary git repository so the suite materializes its index as it does for the
    demos. It needs the automation configuration's editor; the Dist half needs a Dist build and a device and is the
    games stage's (CI.py)."""

    @unittest.skip(SKIP_SUITE)
    def test_the_editor_half_passes_a_game_and_fails_a_hand_edited_hash(self) -> None:
        # The suite's editor half starts a rendering editor (decision 8).
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor())
        root = self.create_project(client, "Game")
        build_tiny_game(client)
        client.call("script.write", {"path": SMOKE_SUITE, "source": "--!strict\nreturn Test.Suite(\"Smoke\", "
                                     "function()\n\tTest.Case(\"passes\", function() Test.Expect(true, \"true\") "
                                     "end)\nend)\n"})
        client.call("project.setSettings", {"patch": {"Testing": {
            "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}], "Replays": ["Assets/Tests/Replays/**"]}}})
        client.call("scene.save", {})
        client.call("test.run", {"filter": "Smoke", "record": f"project://{REPLAY}"})
        client.call("session.shutdown")
        git_stage(root, initialize=True)
        project = games.GameProject(root, root / "Game.eproj", "Game")
        arguments = test_script.parse_arguments(["--suite", "games", "--config", automation_config()])
        passing = provenance.ProjectAudit(root, "disk", {}, [], [])
        with mock.patch.object(games, "discover_projects", return_value=[project]), \
                mock.patch.object(provenance, "audit_project", return_value=passing):
            steps = test_script.run_games([automation_config()], arguments, Console(json_mode=True))
        statuses = {step.name: step.status for step in steps}
        self.assertEqual(statuses[f"games Game Editor {automation_config()}"], Status.PASSED)
        replay = root / REPLAY
        document = json.loads(replay.read_text(encoding="utf-8"))
        document["FinalStateHash"] = "0000000000000000"
        replay.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
        git_stage(root)
        with mock.patch.object(games, "discover_projects", return_value=[project]), \
                mock.patch.object(provenance, "audit_project", return_value=passing):
            steps = test_script.run_games([automation_config()], arguments, Console(json_mode=True))
        editor_step = next(step for step in steps if step.name == f"games Game Editor {automation_config()}")
        self.assertEqual(editor_step.status, Status.FAILED)
        self.assertIn(REPLAY, editor_step.detail)


if __name__ == "__main__":
    unittest.main()
