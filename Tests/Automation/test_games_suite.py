"""Scripts/Lib/games.py and the games suite and stage (Docs/Architecture.md §15.1 T5, §15.8; Roadmap M16, M17, M25
and M27; Docs/Decisions/0026-m16-contract.md decisions 7 and 8, stream B): project discovery and the Testing settings,
the evaluation of a test run, of script.check, project.validate, the upgrade dry run and the smoke export; the suite's
registration, steps, device and build rules and outputs in Test.py, the editor half's checks against a faked editor,
the Dist half's against a faked editor and Runtime, Test.py's shared helpers, the CI.py stage and PreCommit's Debug run
(recorded, nothing run); and an end-to-end run of the editor half on a small game built through automation in the
test.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import re
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

from harness import REPOSITORY_ROOT, AutomationTestCase, app_name, automation_config
from lock_holder import isolated_lock_directory
from tiny_game import TINY_SCENE, build_tiny_game

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CI  # noqa: E402  (Scripts/ is on sys.path above)
import PreCommit  # noqa: E402
import Test as test_script  # noqa: E402
from Lib import exports, games, hardening, paths, provenance  # noqa: E402
from Lib.process import ProcessResult  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402

SMOKE_SUITE = "Assets/Tests/Smoke.test.luau"
EDITOR_ONLY_SUITE = "Assets/Tests/EditorOnly.test.luau"
REPLAY = "Assets/Tests/Replays/Rally.replay"
REPLAY_HASH = "0123456789abcdef"


def git_stage(repository: Path, initialize: bool = False) -> None:
    """Stages every file of `repository` (initializing it first when asked), as a commit about to be made holds them;
    the suite's editor half materializes the index (decision 8). No system or user configuration is read here, and a
    new repository gets this repository's .gitattributes, which gives it the demo projects' -text rules;
    materialize_index overrides the line-ending configuration itself."""
    environment = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull}
    if initialize:
        shutil.copyfile(REPOSITORY_ROOT / ".gitattributes", repository / ".gitattributes")
    commands = ([["init", "--quiet"]] if initialize else []) + [["add", "--all"]]
    for command in commands:
        subprocess.run(["git", "-C", str(repository), *command], check=True, capture_output=True, env=environment,
                       timeout=120)


def write_project(root: Path, relative: str, testing: dict | None = None, build_settings: bool = True) -> Path:
    """<root>/<relative>/<name>.eproj with `testing` as its Testing member; read_testing lets the rest be absent. With
    `build_settings` its StartScene is TINY_SCENE and Export.BuildScenes lists it, the demo acceptance's build settings
    (games.check_build_settings, Docs/Decisions/0027-m17-contract.md decision 4, B1), so the editor halves keep
    passing once B1 checks them."""
    directory = root / relative
    directory.mkdir(parents=True, exist_ok=True)
    project_file = directory / f"{directory.name}.eproj"
    document: dict[str, Any] = {"Format": "Project", "Version": 1, "Name": directory.name}
    if build_settings:
        document.update({"StartScene": TINY_SCENE, "Export": {"BuildScenes": [TINY_SCENE]}})
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
    replays=("Assets/Tests/Replays/*.replay",))


def write_replay(project: Path, relative: str = REPLAY, state_hash: str = REPLAY_HASH, events: int = 0) -> None:
    """A replay header as committed: its FinalStateHash is what every run must reproduce, and its `events` input events
    are what the input check (games.evaluate_input_check) plays it without."""
    replay = project / relative
    replay.parent.mkdir(parents=True, exist_ok=True)
    recorded = [{"Tick": tick, "Type": "Key", "Key": "Space", "State": "Tap"} for tick in range(events)]
    replay.write_text(json.dumps({"Format": "Replay", "Version": 2, "Events": recorded, "FinalTick": 600,
                                  "FinalStateHash": state_hash}, indent="\t") + "\n", encoding="utf-8")


def make_game_repository(root: Path, stray_replay: bool = False, events: int = 2,
                         build_settings: bool = True) -> games.GameProject:
    """A git repository at `root` with the game <root>/Tests/Projects/Game staged: one suite, the replay glob and the
    replay with REPLAY_HASH and `events` input events (a game needs a replay with input, games.check_replay_acceptance),
    with `stray_replay` a replay below Assets/ that Testing.Replays does not match, and without `build_settings` no
    StartScene and no Export.BuildScenes (write_project)."""
    project_file = write_project(root, "Tests/Projects/Game", {
        "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}], "Replays": ["Assets/Tests/Replays/*.replay"]},
        build_settings)
    write_replay(project_file.parent, events=events)
    if stray_replay:
        write_replay(project_file.parent, "Assets/Stray.replay")
    git_stage(root, initialize=True)
    return games.GameProject(project_file.parent, project_file, "Game")


def quiet_console() -> Console:
    """A --json Console whose output is discarded, for run_games against faked editors and devices: the suite prints
    the GPU suite's no-device line, which the automation suite running these tests would otherwise count as one of its
    own tests passing without a device, ending a --require-gpu run as a warning."""
    console = Console(json_mode=True)
    console.stream = io.StringIO()
    return console


class FakeEngineError(Exception):
    def __init__(self, message: str, error_code: str = "Unknown", data: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.error_code = error_code
        self.detail = message
        self.data = data if data is not None else {}


class FakeEditorSession:
    """The editor half's editor, faked through Test.py's import_engine_client seam (the editor halves start editors
    through it like the export and determinism suites, decision 8): canned results per method, a hook run on test.run
    with the copy's directory, and the exit code the process ends with after session.shutdown. It is the process and
    the client at once. input.replay {withoutInput: true} fails with Validation unless `input_independent` (the replay's
    verification then passes without its input); its error's data holds `input_check_outcome` as the playback's
    InputReplayResult ("replay"), as the editor's does."""

    def __init__(self, exit_code: int = 0, on_test_run: Callable[[Path], None] | None = None,
                 input_independent: bool = False, shutdown_error: Exception | None = None,
                 wait_error: Exception | None = None, input_check_outcome: dict[str, Any] | None = None) -> None:
        self.exit_code = exit_code
        self.on_test_run = on_test_run
        self.input_independent = input_independent
        self.input_check_outcome = input_check_outcome
        # Raised by session.shutdown {} (a forced shutdown still succeeds) and by the wait for the process's exit.
        self.shutdown_error = shutdown_error
        self.wait_error = wait_error
        self.arguments: list[str] = []
        self.methods: list[str] = []
        self.replayed: list[dict[str, Any]] = []
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
        if method == "input.replay":
            self.replayed.append(dict(params or {}))
            if not self.input_independent:
                data = {"replay": self.input_check_outcome} if self.input_check_outcome is not None else {}
                raise FakeEngineError("replay verification failed at checkpoint tick 60", "Validation", data)
            return {"passed": True, "withoutInput": True, "finalTick": 600, "stateHash": REPLAY_HASH}
        if method == "session.shutdown" and self.shutdown_error is not None and not (params or {}).get("force"):
            raise self.shutdown_error
        results: dict[str, dict[str, Any]] = {
            "script.check": {"diagnostics": []},
            "project.validate": {"diagnostics": [], "errorCount": 0, "warningCount": 0},
            "project.upgrade": {"changedFiles": [], "unchangedCount": 3},
            "test.run": self.document,
            "play.state": {"state": "Paused"},
        }
        return results.get(method, {})

    def wait(self, timeout: float | None = None) -> int:
        if self.wait_error is not None:
            raise self.wait_error
        return self.exit_code

    def kill(self) -> None:
        return None

    def output(self) -> str:
        return ""


class FakeExportingSession(FakeEditorSession):
    """The Release editor of the Dist half: project.export {testing: true} leaves below `output_root` (set by the test)
    the package of the game `name` (its Name, which the exporter gives the executable whatever the .eproj's file stem):
    the executable and, unless `manifest` is false, its Game.json (at the package root, or in the bundle's
    Contents/Resources on macOS), and reports its outputDirectory and executable, as the editor does
    (ExportMethods.cpp, Exporter.cpp); with featureTest it also reports REPLAY at `feature_test_hash` in its
    featureTest block and leaves that run's log in the package's testing user data, as the export's own --feature-test
    run of the package does (Docs/Decisions/0030-m19-contract.md decision 10; Exporter.h step 6a). The plain export
    reports `smoke`. It counts its launches, so a test sees whether the Release half's session was shared, and records
    each export's params and timeout."""

    def __init__(self, smoke: dict[str, Any], shutdown_error: Exception | None = None,
                 wait_error: Exception | None = None, name: str = "Game", manifest: bool = True,
                 feature_test_hash: str = REPLAY_HASH) -> None:
        super().__init__(shutdown_error=shutdown_error, wait_error=wait_error)
        self.output_root: Path | None = None
        self.smoke = smoke
        self.name = name
        self.manifest = manifest
        self.feature_test_hash = feature_test_hash
        self.launches = 0
        self.exports: list[dict[str, Any]] = []
        self.export_timeouts: list[float | None] = []

    def launch_editor(self, editor: Path, arguments: list[str], *rest: object, **options: object) -> FakeEditorSession:
        self.launches += 1
        return super().launch_editor(editor, arguments, *rest, **options)

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        if method != "project.export":
            return super().call(method, params, timeout)
        self.methods.append(method)
        self.exports.append(dict(params or {}))
        self.export_timeouts.append(timeout)
        if not (params or {}).get("testing"):
            return dict(self.smoke)
        if self.output_root is None:
            raise FakeEngineError("the test set no output_root for project.export")
        output = self.output_root / f"Export{len(self.exports)}" / self.name
        executable = exports.package_executable(output, self.name)
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"")
        if self.manifest:
            resources = output / f"{self.name}.app" / "Contents" / "Resources" if paths.host().is_macos else output
            resources.mkdir(parents=True, exist_ok=True)
            (resources / "Game.json").write_text(json.dumps({"Format": "GameManifest", "Version": 1, "Name": self.name,
                                                            "EngineVersion": "0.1.0", "Testing": True}),
                                                 encoding="utf-8")
        result: dict[str, Any] = {"outputDirectory": str(output), "executable": str(executable), "testSuiteCount": 1,
                                  "replayCount": 1}
        if (params or {}).get("featureTest"):
            result["featureTest"] = {"ran": True, "exitCode": 0, "passed": True, "reportPath": "",
                                     "replays": [{"path": REPLAY, "stateHash": self.feature_test_hash, "passed": True}]}
            log = exports.testing_user_data(output, self.name) / "Logs" / f"{executable.stem}.log"
            log.parent.mkdir(parents=True)
            log.write_text("the export's own feature-test run\n", encoding="utf-8")
        return result


class EditedExportSession(FakeExportingSession):
    """A FakeExportingSession of the game `name` whose testing export's result (and package) `edit` changes before the
    Dist half reads it."""

    def __init__(self, smoke: dict[str, Any], edit: Callable[[dict[str, Any]], object], name: str = "Game") -> None:
        super().__init__(smoke, name=name)
        self.edit = edit

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        result = super().call(method, params, timeout)
        if method == "project.export" and (params or {}).get("testing"):
            self.edit(result)
        return result


class FakeDeterminismSession(FakeEditorSession):
    """The determinism suite's --renderer none editor (Test.py verify_replays): input.replay {verify, strictHash} plays
    every replay to REPLAY_HASH, and its input check (withoutInput) fails with `input_check_outcome` as its error data's
    "replay", as FakeEditorSession's does; it records each input.replay's params."""

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        if method == "input.replay" and not (params or {}).get("withoutInput"):
            self.methods.append(method)
            self.replayed.append(dict(params or {}))
            return {"passed": True, "finalTick": 600, "stateHash": REPLAY_HASH}
        if method == "play.state":
            self.methods.append(method)
            return {"state": "Edit"}
        return super().call(method, params, timeout)


class FakeFeatureTestRun:
    """`<package>/Game --headless --feature-test` faked through Test.py's run_streamed: it writes the Runtime's reports
    (FeatureTest-Dist.{json,xml} below its working directory, whatever the game) with `state_hash` as the replay's hash,
    and the package's testing log, named after the stem of the executable it runs as the Runtime names it
    (ProcessContext.cpp; Docs/Architecture.md §4.4: Logs/<exe>.log); it records each run's command and working
    directory. Without `writes_log` it writes its reports but no log, as a Runtime that put its user data elsewhere
    would."""

    def __init__(self, state_hash: str, name: str = "Game", writes_log: bool = True) -> None:
        self.state_hash = state_hash
        self.name = name  # the game's Name: its package directory and testing user-data folder are named after it
        self.writes_log = writes_log
        self.commands: list[list[str]] = []
        self.directories: list[Path] = []

    def __call__(self, command: list[str], cwd: Path | None = None, **options: object) -> ProcessResult:
        directory = Path(cwd or ".")
        self.commands.append([str(part) for part in command])
        self.directories.append(directory)
        reports = directory / "bin" / "TestResults"
        reports.mkdir(parents=True, exist_ok=True)
        document = run_document("Dist", [suite_row(SMOKE_SUITE), replay_row(REPLAY, self.state_hash)])
        (reports / "FeatureTest-Dist.json").write_text(json.dumps(document), encoding="utf-8")
        (reports / "FeatureTest-Dist.xml").write_text("<testsuites/>", encoding="utf-8")
        if self.writes_log:
            package = exports.package_directory("Dist", self.name)
            log = exports.testing_user_data(package, self.name) / "Logs" / f"{Path(command[0]).stem}.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text("ran\n", encoding="utf-8")
        return ProcessResult([str(part) for part in command], 0, False, 0.0, "")


class DiscoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="Games-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

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

    def test_a_directory_with_two_project_files_is_refused(self) -> None:
        write_project(self.root, "Projects/Twice")
        (self.root / "Projects" / "Twice" / "Other.eproj").write_text("{}", encoding="utf-8")
        with self.assertRaises(ValueError) as raised:
            games.discover_projects(self.root)
        self.assertIn("Twice", str(raised.exception))

    def test_no_project_is_an_empty_list(self) -> None:
        self.assertEqual(games.discover_projects(self.root), [])
        write_project(self.root, "Projects/FeatureTest")
        self.assertEqual(games.discover_projects(self.root), [])

    def test_index_discovery_lists_only_staged_projects(self) -> None:
        write_project(self.root, "Tests/Projects/Staged")
        git_stage(self.root, initialize=True)
        write_project(self.root, "Projects/Untracked")
        self.assertEqual([project.name for project in games.discover_projects(self.root, "disk")],
                         ["Untracked", "Staged"])
        self.assertEqual([project.name for project in games.discover_projects(self.root, "index")], ["Staged"])
        with self.assertRaises(ValueError):
            games.discover_projects(self.root, "git")

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

    def test_read_testing_applies_the_default_modes(self) -> None:
        project_file = write_project(self.root, "Projects/Game", {
            "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE},
                       {"Script": EDITOR_ONLY_SUITE, "Scene": TINY_SCENE, "Modes": ["Editor"]}],
            "Replays": ["project://Assets/Tests/Replays/*.replay"]})
        testing = games.read_testing(project_file)
        self.assertEqual(testing.suites, TESTING.suites)
        self.assertEqual(testing.replays, ("project://Assets/Tests/Replays/*.replay",))
        self.assertEqual(games.read_testing(write_project(self.root, "Projects/Bare")),
                         games.TestingSettings(suites=(), replays=()))

    def test_replay_event_counts_are_read_from_each_replay(self) -> None:
        project = self.root / "Projects" / "Game"
        write_replay(project, events=3)
        write_replay(project, "Assets/Tests/Replays/Idle.replay")
        self.assertEqual(games.replay_event_counts(project, [REPLAY, "Assets/Tests/Replays/Idle.replay"]),
                         {REPLAY: 3, "Assets/Tests/Replays/Idle.replay": 0})
        (project / REPLAY).write_text(json.dumps({"Format": "Replay", "Version": 2}), encoding="utf-8")
        with self.assertRaises(ValueError) as raised:
            games.replay_event_counts(project, [REPLAY])
        self.assertIn(REPLAY, str(raised.exception))

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
    def test_a_complete_run_passes(self) -> None:
        editor = run_document("Editor", [suite_row(SMOKE_SUITE), suite_row(EDITOR_ONLY_SUITE),
                                         replay_row(REPLAY, REPLAY_HASH)])
        self.assertEqual(games.evaluate_run(editor, "Editor", TESTING, {REPLAY: REPLAY_HASH}), [])
        # A Dist run leaves the Editor-only suite out.
        dist = run_document("Dist", [suite_row(SMOKE_SUITE), replay_row(REPLAY, REPLAY_HASH)])
        self.assertEqual(games.evaluate_run(dist, "Dist", TESTING, {REPLAY: REPLAY_HASH}), [])

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

    def test_a_replay_that_testing_replays_does_not_match_is_named(self) -> None:
        with tempfile.TemporaryDirectory(prefix="GamesReplays-") as directory:
            project = Path(directory)
            for relative in (REPLAY, "Assets/Stray.replay", "Library/Cache/Old.replay"):
                write_replay(project, relative)
            self.assertEqual(games.check_replays_matched(project, [REPLAY, "Assets/Stray.replay"]), [])
            failures = games.check_replays_matched(project, [REPLAY])
            self.assertEqual(len(failures), 1)
            self.assertIn("Assets/Stray.replay", failures[0])

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

    def test_the_input_check_passes_only_when_verification_without_input_fails(self) -> None:
        # M16 friction S2-01 (FX-4): Opening.replay ended on a restart, so an empty-input copy verified at its hash.
        self.assertEqual(games.evaluate_input_check(REPLAY, 4, None, ("Validation", "replay verification failed")), [])
        self.assertEqual(games.evaluate_input_check(REPLAY, 0, {"passed": True}, None), [])
        passed = games.evaluate_input_check(REPLAY, 4, {"passed": True, "finalTick": 902}, None)
        self.assertEqual(len(passed), 1)
        # M16 friction S3-01: the remedy names the real rule (the input shows at a checkpoint, the play stays in the
        # scene).
        for text in (REPLAY, "4 input event(s)", "902", "proves nothing about its input",
                     "shows in the world at a checkpoint (one every 60 ticks)", "restart"):
            self.assertIn(text, passed[0])
        refused = games.evaluate_input_check(REPLAY, 4, None, ("NotFound", "replay 'x' is missing"))
        self.assertEqual(len(refused), 1)
        self.assertIn("could not run", refused[0])
        self.assertIn("NotFound", refused[0])

    def test_the_input_check_warns_when_only_the_final_hash_does_not_depend_on_the_input(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 3 (A-F29; ADR 0026 friction F29): the editor halves run the
        # input check with continuePastCheckpoint, so a run without input that differed at checkpoints but reached the
        # recorded final hash is seen: the check still passes (the replay failed without its input), and the half
        # prints the engine's MakeFinalHashIndependentWarning word for word.
        final_hash = "3e0c91a2b4d5f607"
        restored = {"passed": False, "checkpointFailed": True, "checkpointsFailed": 2, "firstFailedCheckpointTick": 60,
                    "hashMatched": True, "expectedStateHash": final_hash, "stateHash": final_hash,
                    "expect": [{"tick": 30, "evaluated": True, "satisfied": True}]}
        expected = (f"'{REPLAY}': its final state hash does not depend on its input: without its input events the "
                    f"run first differed from the recording at checkpoint tick 60, then reached the recorded final "
                    f"state hash {final_hash}, so only its checkpoints prove the play; end the recording while the "
                    f"played state is still in the scene")
        self.assertEqual(games.input_check_warning(REPLAY, restored), expected)
        self.assertEqual(games.evaluate_input_check(REPLAY, 4, None, ("Validation", "replay verification failed")), [])
        # The final hash differs too, no checkpoint failed, an expectation failed, or no outcome: no warning.
        self.assertIsNone(games.input_check_warning(REPLAY, {**restored, "hashMatched": False}))
        unchecked = {**restored, "checkpointsFailed": 0, "checkpointFailed": False}
        self.assertIsNone(games.input_check_warning(REPLAY, unchecked))
        self.assertIsNone(games.input_check_warning(REPLAY, {**restored, "expect": [{"tick": 30, "evaluated": True,
                                                                                    "satisfied": False}]}))
        self.assertIsNone(games.input_check_warning(REPLAY, None))

    def test_projects_that_share_a_stem_or_a_name_are_refused(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 12 (AL-STEMS; ADR 0027 decision 12's accepted limit): two staged
        # games whose .eproj files share a stem would share their games steps and reports, and two whose Names are equal
        # their Dist package directory; the suite refuses both, naming both project files.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_file = write_project(root, "Projects/Rally")
            second_file = write_project(root, "Tests/Projects/Rally")
            other_file = write_project(root, "Projects/Racer")
            first = games.GameProject(first_file.parent, first_file, "Rally")
            second = games.GameProject(second_file.parent, second_file, "Rally")
            other = games.GameProject(other_file.parent, other_file, "Racer")
            self.assertEqual(games.check_distinct_projects([first, other]), [])
            stems = games.check_distinct_projects([first, second, other])
            self.assertEqual(len(stems), 2)  # the shared stem, and the shared Name "Rally" both write
            self.assertTrue(any("share the file stem 'Rally'" in line for line in stems))
            self.assertTrue(any("share the Name 'Rally'" in line for line in stems))
            for line in stems:
                self.assertIn(paths.display_path(first_file), line)
                self.assertIn(paths.display_path(second_file), line)

            # Distinct stems with one Name collide in the Dist package directory only.
            renamed_file = write_project(root, "Projects/Rally2")
            document = json.loads(renamed_file.read_text(encoding="utf-8"))
            document["Name"] = "Racer"
            renamed_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
            renamed = games.GameProject(renamed_file.parent, renamed_file, "Rally2")
            names = games.check_distinct_projects([other, renamed])
            self.assertEqual(len(names), 1)
            self.assertIn("share the Name 'Racer'", names[0])
            self.assertIn("bin/Exports/Dist/Racer", names[0])

            # A Name that cannot be read is named.
            broken_file = write_project(root, "Projects/Broken")
            broken_file.write_text("{", encoding="utf-8")
            broken = games.GameProject(broken_file.parent, broken_file, "Broken")
            unreadable = games.check_distinct_projects([other, broken])
            self.assertEqual(len(unreadable), 1)
            self.assertIn(paths.display_path(broken_file), unreadable[0])

    def test_projects_whose_stems_or_names_differ_only_in_letter_case_are_refused(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 12: the steps' report files and the Dist package directories are
        # files, which collide on a file system that ignores letter case (Windows), so a pair differing only in case is
        # refused too, naming both spellings; a single project is compared with nothing, so its Name is not read.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            upper_file = write_project(root, "Projects/Rally")
            lower_file = write_project(root, "Tests/Projects/rally")
            upper = games.GameProject(upper_file.parent, upper_file, "Rally")
            lower = games.GameProject(lower_file.parent, lower_file, "rally")
            lines = games.check_distinct_projects([upper, lower])
            self.assertEqual(len(lines), 2)
            self.assertTrue(any("share the file stem 'Rally' (and 'rally'" in line for line in lines), lines)
            self.assertTrue(any("share the Name 'Rally' (and 'rally'" in line for line in lines), lines)
            missing = games.GameProject(root / "Projects/Missing", root / "Projects/Missing/Missing.eproj", "Missing")
            self.assertEqual(games.check_distinct_projects([missing]), [])

    def test_a_native_game_gets_the_preference_and_its_module_before_its_editor_starts(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 35 (N-E): the editor halves of a game with Native/ start an
        # editor whose user data allows native code (the harness's own file, never a method) and whose project copy
        # holds a current build of the module, since a project opening loads a build and never builds; a game without
        # Native/ gets neither, and a module that does not build fails the half with its first error.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project_file = write_project(root, "Tests/Projects/Natives")
            user_data = root / "UserData"
            self.assertEqual(games.prepare_native_editor(project_file.parent, user_data, "Debug"), [])
            self.assertFalse((user_data / app_name() / "Editor.json").exists())

            native = project_file.parent / "Native"
            native.mkdir()
            (native / "Broken.cpp").write_text("this is not C++\n", encoding="utf-8")
            failures = games.prepare_native_editor(project_file.parent, user_data, "Debug")
            preferences = json.loads((user_data / app_name() / "Editor.json").read_text(encoding="utf-8"))
            self.assertIs(preferences["AllowNativeCode"], True)
            # The header the editor's reader requires (EditorCore/EditorPreferences.h), without which it reads the file
            # as malformed and leaves native code off.
            self.assertEqual((preferences["Format"], preferences["Version"]), ("EditorPreferences", 1))
            self.assertTrue(failures)
            self.assertTrue(any("Native/Broken.cpp" in line or "Unsupported" in line for line in failures), failures)

            # An existing user://Editor.json keeps every other member.
            kept = {"Format": "EditorPreferences", "Version": 1, "RecentProjects": ["C:/Games/Other/Other.eproj"],
                    "AllowNativeCode": False}
            (user_data / app_name() / "Editor.json").write_text(json.dumps(kept), encoding="utf-8")
            shutil.rmtree(project_file.parent / "Library")
            games.prepare_native_editor(project_file.parent, user_data, "Debug")
            rewritten = json.loads((user_data / app_name() / "Editor.json").read_text(encoding="utf-8"))
            self.assertEqual(rewritten, {**kept, "AllowNativeCode": True})

    def test_the_native_preparation_names_each_problem_in_its_failure_lines(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decisions 34 and 35: a preference file that cannot take the preference, a
        # build that cannot start, that times out or that ends without its record, and a platform without native
        # builds each fail the editor half with a line naming it. The Windows cases run as on Windows on every
        # platform (only Windows builds a module, so elsewhere the preparation would stop before its build).
        windows = types.SimpleNamespace(name="nt")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = write_project(root, "Tests/Projects/Natives").parent
            (project / "Native").mkdir()
            user_data = root / "UserData"
            preferences = user_data / app_name() / "Editor.json"
            preferences.parent.mkdir(parents=True)
            built = subprocess.CompletedProcess([], 0, "", "")
            for text, problem in (("[1, 2]", "is not a JSON object"), ("{not json", "cannot be read")):
                with self.subTest(preferences=text), mock.patch.object(games, "os", windows), \
                        mock.patch.object(games.subprocess, "run", return_value=built):
                    preferences.write_text(text, encoding="utf-8")
                    lines = games.prepare_native_editor(project, user_data, "Debug")
                    self.assertEqual(len(lines), 1, lines)
                    self.assertIn(problem, lines[0])
            preferences.unlink()

            # Without a record (none was written), the line ends with the last line the build printed.
            unrecorded = subprocess.CompletedProcess([], 1, "", "no MSBuild\nthe toolchain is missing\n")
            failures: dict[str, Any] = {
                "could not start": OSError("no interpreter"),
                "did not finish": subprocess.TimeoutExpired(["BuildNative.py"], 1.0),
                "exited with 1): the toolchain is missing": unrecorded,
            }
            for problem, outcome in failures.items():
                raised = isinstance(outcome, BaseException)
                options = {"side_effect": outcome} if raised else {"return_value": outcome}
                with self.subTest(build=problem), mock.patch.object(games, "os", windows), \
                        mock.patch.object(games.subprocess, "run", **options):
                    lines = games.prepare_native_editor(project, user_data, "Release")
                    self.assertEqual(len(lines), 1, lines)
                    self.assertIn(problem, lines[0])
                    self.assertIn("Release", lines[0])

            with mock.patch.object(games, "os", types.SimpleNamespace(name="posix")), \
                    mock.patch.object(games.subprocess, "run") as run:
                lines = games.prepare_native_editor(project, user_data, "Debug")
            run.assert_not_called()
            self.assertEqual(len(lines), 1, lines)
            self.assertTrue(lines[0].startswith("Unsupported"), lines)

    def test_a_game_needs_a_committed_replay_with_input_events(self) -> None:
        # The milestone review's R09: the demo acceptance (Roadmap M16, M17, M25 and M27) accepts a demo by its suites
        # and a replay recorded from tick 0 that plays it, so a game whose Testing.Replays matches nothing, or only
        # replays without input, fails, citing that acceptance.
        idle = "Assets/Tests/Replays/Idle.replay"
        self.assertEqual(games.check_replay_acceptance({REPLAY: 3}), [])
        self.assertEqual(games.check_replay_acceptance({REPLAY: 3, idle: 0}), [])
        none = games.check_replay_acceptance({})
        self.assertEqual(len(none), 1)
        self.assertIn("Testing.Replays matches no replay", none[0])
        self.assertIn(games.DEMO_ACCEPTANCE, none[0])
        without_input = games.check_replay_acceptance({REPLAY: 0, idle: 0})
        self.assertEqual(len(without_input), 1)
        for text in ("no replay of Testing.Replays has input events", REPLAY, idle, games.DEMO_ACCEPTANCE):
            self.assertIn(text, without_input[0])

    def test_a_game_needs_a_start_scene_that_its_build_scenes_list(self) -> None:
        # ADR 0027 decision 4, B1 (M17 pipeline map G2): Roadmap M17, M25 and M27 accept a demo with "StartScene and
        # Export.BuildScenes set", but the validator and the exporter accept an empty BuildScenes (FeatureTest relies on
        # it), so only the games suite checks the rule; that BuildScenes lists StartScene is this ADR's convention. The
        # failure lines are that ADR's frozen strings, renumbered by Docs/Decisions/0028-backlog-program.md decision 14.
        main = "Assets/Scenes/Main.scene"
        roadmap = "(Roadmap M17, M25 and M27: StartScene and Export.BuildScenes set)"
        start = f"StartScene is empty: set it to the scene the game starts with {roadmap}"
        build = (f"Export.BuildScenes is empty: list the scenes the export must load strictly, StartScene included "
                 f"{roadmap}")
        unlisted = (f"Export.BuildScenes does not list the StartScene '{main}': add it (ADR 0027 decision 4: the "
                    f"demos' convention)")
        cases: dict[str, tuple[dict[str, Any], list[str]]] = {
            "complete": ({"StartScene": main, "Export": {"BuildScenes": [main, "Assets/Scenes/Level2.scene"]}}, []),
            "nothing set": ({}, [start, build]),
            "both empty": ({"StartScene": "", "Export": {"BuildScenes": []}}, [start, build]),
            "no start scene": ({"Export": {"BuildScenes": [main]}}, [start]),
            "no build scenes": ({"StartScene": main, "Export": {"Exclude": ["Assets/Tests/**"]}}, [build]),
            "start scene not listed": ({"StartScene": main, "Export": {"BuildScenes": ["Assets/Scenes/Menu.scene"]}},
                                       [unlisted]),
        }
        with tempfile.TemporaryDirectory(prefix="GamesBuildSettings-") as directory:
            for name, (members, expected) in cases.items():
                with self.subTest(case=name):
                    project_file = Path(directory) / name.replace(" ", "-") / "Game.eproj"
                    project_file.parent.mkdir()
                    project_file.write_text(json.dumps({"Format": "Project", "Version": 1, "Name": "Game", **members}),
                                            encoding="utf-8")
                    self.assertEqual(games.check_build_settings(project_file), expected)
            refused: dict[str, Any] = {
                "scene": {"Format": "Scene", "Version": 1},
                "array": [],
                "start": {"Format": "Project", "Version": 1, "StartScene": 3},
                "export": {"Format": "Project", "Version": 1, "Export": ["Assets/Scenes/Main.scene"]},
                "build": {"Format": "Project", "Version": 1, "Export": {"BuildScenes": main}},
                "build items": {"Format": "Project", "Version": 1, "Export": {"BuildScenes": [3]}},
                "missing": None,
            }
            for name, document in refused.items():
                with self.subTest(refused=name):
                    project_file = Path(directory) / f"Refused-{name.replace(' ', '-')}.eproj"
                    if document is not None:
                        project_file.write_text(json.dumps(document), encoding="utf-8")
                    with self.assertRaises(ValueError) as raised:
                        games.check_build_settings(project_file)
                    self.assertIn(project_file.name, str(raised.exception))
        # The committed dry-run game already complies.
        breakout = REPOSITORY_ROOT / "Tests" / "Projects" / "Breakout" / "Breakout.eproj"
        self.assertEqual(games.check_build_settings(breakout), [])

    def test_the_game_name_is_read_from_a_package_manifest_or_a_project_file(self) -> None:
        # ADR 0027 decision 4, B2 (M17 pipeline map R6): the exporter names the package's executable, its testing user
        # data and its log after the project's Name, which the testing export's Game.json carries, whatever the .eproj's
        # file stem; read_game_name reads it from either document and refuses anything else, naming the file.
        with tempfile.TemporaryDirectory(prefix="GamesGameName-") as directory:
            root = Path(directory)
            accepted: dict[str, dict[str, Any]] = {
                "Game.json": {"Format": "GameManifest", "Version": 1, "Name": "Falling", "EngineVersion": "0.1.0",
                              "Testing": True},
                "Tetris.eproj": {"Format": "Project", "Version": 1, "Name": "Falling"},
            }
            for file_name, document in accepted.items():
                with self.subTest(accepted=file_name):
                    path = root / file_name
                    path.write_text(json.dumps(document), encoding="utf-8")
                    self.assertEqual(games.read_game_name(path), "Falling")
            refused: dict[str, Any] = {
                "Scene.json": {"Format": "Scene", "Version": 1, "Name": "Falling"},
                "Unnamed.json": {"Format": "GameManifest", "Version": 1},
                "Empty.json": {"Format": "GameManifest", "Version": 1, "Name": ""},
                "Numbered.eproj": {"Format": "Project", "Version": 1, "Name": 7},
                "Array.json": [],
                "Missing.json": None,
            }
            for file_name, document in refused.items():
                with self.subTest(refused=file_name):
                    path = root / file_name
                    if document is not None:
                        path.write_text(json.dumps(document), encoding="utf-8")
                    with self.assertRaises(ValueError) as raised:
                        games.read_game_name(path)
                    self.assertIn(file_name, str(raised.exception))
            broken = root / "Broken.json"
            broken.write_text("{not json", encoding="utf-8")
            with self.assertRaises(ValueError) as raised:
                games.read_game_name(broken)
            self.assertIn(broken.name, str(raised.exception))

    def test_a_game_name_that_is_not_one_folder_name_is_refused(self) -> None:
        # The Dist half replaces bin/Exports/Dist/<Name> (install_package removes an earlier package there), so a Name
        # that is not one folder name (one the exporter never writes: Paths::ValidateAppName refuses it) must never
        # become a path that leaves that folder.
        with tempfile.TemporaryDirectory(prefix="GamesGameFolder-") as directory:
            manifest = Path(directory) / "Game.json"
            for game in ("Falling Blocks", "Tetris-2"):
                with self.subTest(accepted=game):
                    manifest.write_text(json.dumps({"Format": "GameManifest", "Version": 1, "Name": game}),
                                        encoding="utf-8")
                    self.assertEqual(games.read_game_name(manifest), game)
            for game in (".", "..", "../Escape", "Nested/Game", "Nested\\Game", "C:Game", "Game?"):
                with self.subTest(refused=game):
                    manifest.write_text(json.dumps({"Format": "GameManifest", "Version": 1, "Name": game}),
                                        encoding="utf-8")
                    with self.assertRaises(ValueError) as raised:
                        games.read_game_name(manifest)
                    self.assertIn("Game.json", str(raised.exception))
                    self.assertIn("not one folder name", str(raised.exception))

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

    def test_a_smoke_test_that_did_not_run_warns_without_a_device_and_fails_with_require_gpu(self) -> None:
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        self.assertEqual(games.check_smoke(ran, require_gpu=True), ([], []))
        self.assertEqual(len(games.check_smoke({**ran, "smokeTestExitCode": 1}, require_gpu=False)[0]), 1)
        not_run = {"smokeTestRan": False, "smokeTestExitCode": None, "warnings": ["no Vulkan device"]}
        failures, warnings = games.check_smoke(not_run, require_gpu=False)
        self.assertEqual((failures, len(warnings)), ([], 1))
        failures, warnings = games.check_smoke(not_run, require_gpu=True)
        self.assertEqual((len(failures), warnings), (1, []))

    def test_the_feature_test_check_compares_the_exports_hashes_with_the_packages_own_run(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 10 (A-DISTSELF): the hashes a testing Dist export's featureTest
        # reports must equal those the Dist half's own --feature-test run reads; a replay that could not play (an empty
        # stateHash) matches one the run's report has no hash for; a run the export could not start warns, or fails with
        # --require-gpu, and a result without the block fails.
        other = "Assets/Tests/Replays/Other.replay"
        replays = [{"path": REPLAY, "stateHash": REPLAY_HASH, "passed": True},
                   {"path": other, "stateHash": "", "passed": False}]
        result = {"featureTest": {"ran": True, "exitCode": 1, "passed": False, "reportPath": "", "replays": replays},
                  "warnings": []}
        self.assertEqual(games.check_feature_test(result, {REPLAY: REPLAY_HASH}, require_gpu=True), ([], []))
        failures, warnings = games.check_feature_test(result, {REPLAY: "fedcba9876543210", other: REPLAY_HASH},
                                                      require_gpu=False)
        self.assertEqual((len(failures), warnings), (2, []))
        self.assertTrue(failures[0].startswith(f"{other}: "), failures)
        self.assertIn("no state hash", failures[0])
        self.assertTrue(failures[1].startswith(f"{REPLAY}: "), failures)
        self.assertIn("fedcba9876543210", failures[1])
        not_run = {"featureTest": {"ran": False, "exitCode": None, "passed": False, "reportPath": "", "replays": []},
                   "warnings": ["no Vulkan device"]}
        failures, warnings = games.check_feature_test(not_run, {}, require_gpu=False)
        self.assertEqual((failures, len(warnings)), ([], 1))
        self.assertIn("no Vulkan device", warnings[0])
        failures, warnings = games.check_feature_test(not_run, {}, require_gpu=True)
        self.assertEqual((len(failures), warnings), (1, []))
        self.assertEqual(len(games.check_feature_test({"warnings": []}, {}, require_gpu=False)[0]), 1)
        malformed = {"featureTest": {"ran": True, "replays": [{"path": REPLAY}]}}
        self.assertEqual(len(games.check_feature_test(malformed, {}, require_gpu=False)[0]), 1)

    def test_report_paths_are_named_per_project_and_configuration(self) -> None:
        project = games.GameProject(Path("Breakout"), Path("Breakout/Breakout.eproj"), "Breakout")
        results = paths.TEST_RESULTS_ROOT
        self.assertEqual(games.editor_report_path(project, "Debug"), results / "Games-Breakout-Editor-Debug.json")
        self.assertEqual(games.dist_report_path(project, ".xml"), results / "Games-Breakout-Dist.xml")
        self.assertEqual(games.junit_path("Release"), results / "Games-Release.xml")


class SuiteTests(unittest.TestCase):
    def test_the_suite_is_available_in_debug_release_and_dist(self) -> None:
        suite = test_script.SUITES["games"]
        self.assertTrue(suite.available)
        self.assertEqual(suite.configurations, ("Debug", "Release", "Dist"))
        self.assertEqual(suite.allowed, paths.CONFIGURATIONS)

    def test_main_hands_the_requested_configurations_to_run_games(self) -> None:
        isolated_lock_directory(self)  # the run takes the GPU lock: this test's own, never the machine's
        ran: list[tuple[str, ...]] = []

        def run_games(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[Step]:
            ran.append(tuple(configs))
            return [Step("games", Status.PASSED, "recorded")]

        with mock.patch.object(test_script, "run_games", run_games), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(test_script.main(["--suite", "games", "--config", "Debug", "--json"]), 0)
            self.assertEqual(test_script.main(["--suite", "games", "--json"]), 0)
        self.assertEqual(ran, [("Debug",), ("Debug", "Release", "Dist")])

    def test_without_a_game_project_the_suite_passes_with_a_note(self) -> None:
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects", return_value=[]):
            steps = test_script.run_games(["Debug"], arguments, quiet_console())
        self.assertEqual([(step.name, step.status) for step in steps], [("games", Status.PASSED)])
        self.assertIn("no game project", steps[0].detail)

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
                    steps = test_script.run_games(["Release", "Dist"], arguments, quiet_console())
                audit.assert_called_once_with(project.directory, "index")
                materialize.assert_not_called()
                statuses = {step.name: step.status for step in steps}
                self.assertEqual(statuses["games Game audit"], Status.PASSED)
                self.assertEqual(statuses["games Game Editor Release"], expected)
                self.assertEqual(statuses["games Game Dist"], expected)

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
            steps = test_script.run_games(["Debug"], arguments, quiet_console())
        audit_step = next(step for step in steps if step.name == "games Game audit")
        self.assertEqual(audit_step.status, Status.FAILED)
        self.assertIn("hash-mismatch", audit_step.detail)
        self.assertIn("Assets/Scenes/Main.scene", audit_step.detail)

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
                steps = test_script.run_games(["Debug"], arguments, quiet_console())
        audit.assert_called_once_with(staged, "index")
        notes = [step for step in steps if step.name.startswith("games Untracked")]
        self.assertEqual([(step.name, step.status) for step in notes], [("games Untracked", Status.PASSED)])
        self.assertIn("not staged", notes[0].detail)

    def test_a_directory_with_several_project_files_fails_the_suite(self) -> None:
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
        with mock.patch.object(games, "discover_projects",
                               side_effect=ValueError("Tests/Projects/Twice holds 2 *.eproj files")):
            steps = test_script.run_games(["Debug"], arguments, quiet_console())
        self.assertEqual([(step.name, step.status) for step in steps], [("games", Status.FAILED)])
        self.assertIn("Tests/Projects/Twice", steps[0].detail)

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
                steps = test_script.run_games(["Debug"], arguments, quiet_console())
        probe.assert_not_called()
        statuses = {step.name: step for step in steps}
        self.assertEqual(statuses["games Game audit"].status, Status.PASSED)
        editor_step = statuses["games Game Editor Debug"]
        self.assertEqual((editor_step.status, editor_step.exit_code), (Status.FAILED, 3))
        self.assertIn("Build.py", editor_step.detail)

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
                test_script.run_games(["Release", "Dist"], arguments, quiet_console())
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
            steps = test_script.run_games(["Debug"], arguments, quiet_console())
        return next(step for step in steps if step.name == f"games {project.name} Editor Debug")

    def test_the_editor_half_prepares_a_native_game_before_its_editor_starts_and_fails_on_its_lines(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 35: the editor half hands games.prepare_native_editor the copy it
        # opens, the editor's user-data root and its configuration before the editor starts; a failure line fails the
        # half without starting the editor.
        class LaunchRecordingSession(FakeEditorSession):
            """Records the user-data root the editor is launched with."""

            user_data: Path | None = None

            def launch_editor(self, editor: Path, arguments: list[str], *rest: object,
                              **options: object) -> FakeEditorSession:
                self.user_data = Path(str(rest[0]))
                return super().launch_editor(editor, arguments, *rest, **options)

        calls: list[tuple[Path, Path, str, bool, bool]] = []
        lines: list[str] = []

        def prepare(project_directory: Path, user_data: Path, config: str) -> list[str]:
            calls.append((project_directory, user_data, config, any(project_directory.glob("*.eproj")),
                          bool(session.arguments)))
            return list(lines)

        for failure in ("", "the native module's Debug build failed: Native/Broken.cpp:1:1: NATIVE_COMPILE_ERROR"):
            lines[:] = [failure] if failure else []
            calls.clear()
            session = LaunchRecordingSession()
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(prefix="GamesNative-") as directory, \
                    mock.patch.object(games, "prepare_native_editor", side_effect=prepare):
                project = make_game_repository(Path(directory))
                step = self.run_editor_half(project, session)
                self.assertEqual(len(calls), 1)
                copy, user_data, config, holds_project, launched = calls[0]
                self.assertEqual(config, "Debug")
                self.assertTrue(holds_project, copy)
                self.assertFalse(launched)
                self.assertFalse(copy.is_relative_to(project.directory))
                if failure:
                    self.assertEqual(step.status, Status.FAILED)
                    self.assertIn("Native/Broken.cpp", step.detail)
                    self.assertEqual(session.arguments, [])
                else:
                    self.assertEqual(step.status, Status.PASSED, step.detail)
                    self.assertEqual(session.project_directory(), copy)
                    self.assertEqual(session.user_data, user_data)

    def test_the_editor_half_prints_the_input_checks_final_hash_warning_and_passes(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 3 (A-F29): a replay whose run without input differed at its
        # checkpoints but reached the recorded final hash passes the input check; the editor half prints the warning,
        # keeps it in its step's data and stays passed.
        outcome = {"passed": False, "checkpointFailed": True, "checkpointsFailed": 1, "firstFailedCheckpointTick": 60,
                   "hashMatched": True, "expectedStateHash": REPLAY_HASH, "stateHash": REPLAY_HASH, "expect": []}
        with tempfile.TemporaryDirectory(prefix="GamesInputWarning-") as directory:
            project = make_game_repository(Path(directory), events=2)
            step = self.run_editor_half(project, FakeEditorSession(input_check_outcome=outcome))
        self.assertEqual(step.status, Status.PASSED, step.detail)
        self.assertEqual(step.data["warnings"], [games.input_check_warning(REPLAY, outcome)])
        # Without that outcome (the final hash differed too) there is no warning.
        with tempfile.TemporaryDirectory(prefix="GamesInputWarning-") as directory:
            project = make_game_repository(Path(directory), events=2)
            plain = self.run_editor_half(project, FakeEditorSession(input_check_outcome={**outcome,
                                                                                         "hashMatched": False}))
        self.assertEqual(plain.status, Status.PASSED, plain.detail)
        self.assertNotIn("warnings", plain.data)

    def test_the_determinism_suite_prints_the_input_checks_final_hash_warning_and_passes(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 3 (A-F29): the determinism suite runs the same input check
        # (continuePastCheckpoint), keeps games.input_check_warning's line of a replay whose run without input differed
        # at its checkpoints but reached the recorded final hash, prints it and lists it under the step's "warnings";
        # the step stays passed. Without that outcome (the final hash differed too) there is no warning.
        outcome = {"passed": False, "checkpointFailed": True, "checkpointsFailed": 1, "firstFailedCheckpointTick": 60,
                   "hashMatched": True, "expectedStateHash": REPLAY_HASH, "stateHash": REPLAY_HASH, "expect": []}
        warning = games.input_check_warning(REPLAY, outcome)
        self.assertIsNotNone(warning)
        for final_hash_matched in (True, False):
            with self.subTest(final_hash_matched=final_hash_matched), \
                    tempfile.TemporaryDirectory(prefix="DeterminismInputWarning-") as directory:
                project_file = write_project(Path(directory), "FeatureTest",
                                             {"Replays": ["Assets/Tests/Replays/*.replay"]})
                write_replay(project_file.parent, events=2)
                session = FakeDeterminismSession(input_check_outcome={**outcome, "hashMatched": final_hash_matched})
                console = quiet_console()
                arguments = test_script.parse_arguments(["--suite", "determinism", "--config", "Debug"])
                with mock.patch.object(test_script, "FEATURE_PROJECT", project_file.parent), \
                        mock.patch.object(test_script, "FEATURE_PROJECT_FILE", project_file), \
                        mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                        mock.patch.object(test_script, "import_engine_client", return_value=session.module()):
                    step = test_script.run_determinism(["Debug"], arguments, console)
                self.assertEqual(step.status, Status.PASSED, step.detail)
                self.assertIn({"path": REPLAY, "verify": True, "strictHash": True, "withoutInput": True,
                               "continuePastCheckpoint": True}, session.replayed)
                if final_hash_matched:
                    self.assertEqual(step.data["warnings"], [f"Editor Debug: {warning}"])
                    self.assertIn(f"warning: Editor Debug: {warning}", console.stream.getvalue())
                else:
                    self.assertNotIn("warnings", step.data)
                    self.assertNotIn("warning:", console.stream.getvalue())

    def test_staged_projects_that_share_a_stem_fail_the_games_step_before_any_project_runs(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 12 (AL-STEMS): two staged games named Rally would share their
        # steps and reports, so the suite fails its "games" step naming both and audits or runs neither. The Names are
        # the staged ones the halves run with (the milestone review): two staged games named Shared collide although
        # an unstaged edit renames one in the working tree, and a project file broken only in the working tree is
        # still read from its staged blob.
        with tempfile.TemporaryDirectory(prefix="GamesStems-") as directory:
            root = Path(directory)
            write_project(root, "Projects/Rally")
            write_project(root, "Tests/Projects/Rally")
            shared = [write_project(root, f"Projects/{name}") for name in ("Alpha", "Beta")]
            for project_file in shared:
                document = json.loads(project_file.read_text(encoding="utf-8"))
                document["Name"] = "Shared"
                project_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
            intact = write_project(root, "Projects/Gamma")
            git_stage(root, initialize=True)
            renamed = json.loads(shared[1].read_text(encoding="utf-8"))
            renamed["Name"] = "Other"
            shared[1].write_text(json.dumps(renamed, indent="\t") + "\n", encoding="utf-8")
            intact.write_text("{", encoding="utf-8")
            discover = games.discover_projects

            def discover_in_repository(_: Path, files: str = "disk") -> list[games.GameProject]:
                return discover(root, files)

            arguments = test_script.parse_arguments(["--suite", "games", "--config", "Debug"])
            with mock.patch.object(games, "discover_projects", side_effect=discover_in_repository), \
                    mock.patch.object(provenance, "audit_project") as audit, \
                    mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "probe_rendering_editor") as probe:
                steps = test_script.run_games(["Debug"], arguments, quiet_console())
        audit.assert_not_called()
        probe.assert_not_called()
        self.assertEqual([(step.name, step.status) for step in steps], [("games", Status.FAILED)])
        self.assertIn("share the file stem 'Rally'", steps[0].detail)
        self.assertIn("Projects/Rally/Rally.eproj", steps[0].detail)
        self.assertIn("Tests/Projects/Rally/Rally.eproj", steps[0].detail)
        failures = steps[0].data["failures"]
        self.assertTrue(any("Projects/Alpha/Alpha.eproj" in line and "Projects/Beta/Beta.eproj" in line
                            and "share the Name 'Shared'" in line for line in failures), failures)
        self.assertFalse(any("Gamma" in line for line in failures), failures)

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
        order = ["script.check", "project.validate", "project.upgrade", "test.run", "input.replay", "session.shutdown"]
        self.assertEqual([method for method in session.methods if method in order], order)

    def test_the_editor_half_fails_a_game_without_build_settings_first(self) -> None:
        # ADR 0027 decision 4, B1: check_game_in_editor checks the staged copy's build settings first, so PreCommit's
        # Debug half enforces the demo acceptance's "StartScene and Export.BuildScenes set". Its lines come first among
        # the half's failures, the half's other checks still run, and a project file it cannot read is one failure line
        # naming the file.
        with tempfile.TemporaryDirectory(prefix="GamesBuildSettings-") as directory:
            project = make_game_repository(Path(directory), build_settings=False)
            session = FakeEditorSession()
            step = self.run_editor_half(project, session)
        self.assertEqual(step.status, Status.FAILED)
        failures = step.data["failures"]
        self.assertEqual(len(failures), 2, failures)
        self.assertTrue(failures[0].startswith("StartScene is empty: "), failures)
        self.assertTrue(failures[1].startswith("Export.BuildScenes is empty: "), failures)
        order = ["script.check", "project.validate", "project.upgrade", "test.run", "input.replay", "session.shutdown"]
        self.assertEqual([method for method in session.methods if method in order], order)

        with tempfile.TemporaryDirectory(prefix="GamesBuildSettingsMalformed-") as directory:
            root = Path(directory)
            project = make_game_repository(root)
            document = json.loads(project.project_file.read_text(encoding="utf-8"))
            document["Export"] = [TINY_SCENE]
            project.project_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
            git_stage(root)
            step = self.run_editor_half(project, FakeEditorSession())
        self.assertEqual(step.status, Status.FAILED)
        failures = step.data["failures"]
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(project.project_file.name, failures[0])

    def test_the_editor_half_lists_the_build_settings_lines_first_and_runs_its_other_checks_either_way(self) -> None:
        # ADR 0027 decision 4, B1 ("Frozen interfaces"): check_build_settings' lines come before those of every later
        # check of the editor half, and a project file it cannot check is one line naming the file, after which every
        # later check still runs, in order. A stray replay gives a later check a failure of its own.
        order = ["script.check", "project.validate", "project.upgrade", "test.run", "input.replay", "session.shutdown"]
        with tempfile.TemporaryDirectory(prefix="GamesBuildSettingsFirst-") as directory:
            project = make_game_repository(Path(directory), stray_replay=True, build_settings=False)
            session = FakeEditorSession()
            step = self.run_editor_half(project, session)
        self.assertEqual(step.status, Status.FAILED)
        failures = step.data["failures"]
        self.assertEqual(len(failures), 3, failures)
        self.assertTrue(failures[0].startswith("StartScene is empty: "), failures)
        self.assertTrue(failures[1].startswith("Export.BuildScenes is empty: "), failures)
        self.assertIn("Assets/Stray.replay", failures[2])
        self.assertEqual([method for method in session.methods if method in order], order)

        with tempfile.TemporaryDirectory(prefix="GamesBuildSettingsUnchecked-") as directory:
            root = Path(directory)
            project = make_game_repository(root, stray_replay=True)
            document = json.loads(project.project_file.read_text(encoding="utf-8"))
            document["Export"] = [TINY_SCENE]
            project.project_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
            git_stage(root)
            session = FakeEditorSession()
            step = self.run_editor_half(project, session)
        self.assertEqual(step.status, Status.FAILED)
        failures = step.data["failures"]
        self.assertEqual(len(failures), 2, failures)
        self.assertTrue(failures[0].startswith("the build settings cannot be checked: "), failures)
        self.assertIn(project.project_file.name, failures[0])
        self.assertIn("Assets/Stray.replay", failures[1])
        self.assertEqual([method for method in session.methods if method in order], order)

    def test_the_editor_half_runs_on_the_staged_blobs_not_the_working_tree(self) -> None:
        # The milestone review's R12: the editor half must see what a fresh clone of the commit holds. Unstaged bytes
        # (a replay whose committed hash was changed on disk) and an untracked replay are not in the copy, so the half
        # passes against the staged hash; a working-tree copy would fail on both.
        seen: dict[str, Any] = {}

        def inspect_copy(copy: Path) -> None:
            seen["untracked"] = (copy / "Assets" / "Untracked.replay").exists()
            seen["hash"] = json.loads((copy / REPLAY).read_text(encoding="utf-8"))["FinalStateHash"]
            seen["files"] = sorted(path.relative_to(copy).as_posix() for path in copy.rglob("*") if path.is_file())

        with tempfile.TemporaryDirectory(prefix="GamesIndexCopy-") as directory:
            project = make_game_repository(Path(directory))
            staged = sorted(path.relative_to(project.directory).as_posix() for path in project.directory.rglob("*")
                            if path.is_file())
            write_replay(project.directory, state_hash="fedcba9876543210", events=2)
            write_replay(project.directory, "Assets/Untracked.replay", events=2)
            session = FakeEditorSession(on_test_run=inspect_copy)
            step = self.run_editor_half(project, session)
        self.assertEqual(step.status, Status.PASSED, step.detail)
        self.assertEqual(seen, {"untracked": False, "hash": REPLAY_HASH, "files": staged})
        self.assertEqual(step.data["replays"], {REPLAY: REPLAY_HASH})

    def test_a_game_without_a_replay_with_input_events_fails_both_halves(self) -> None:
        # The milestone review's R09: a game whose replays carry no input (or that matches none) proves nothing about
        # its gameplay; the editor halves and the Dist half both fail it.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        for events in (0, None):
            with self.subTest(events=events), tempfile.TemporaryDirectory(prefix="GamesNoInput-") as directory:
                root = Path(directory)
                if events is None:
                    project_file = write_project(root, "Tests/Projects/Game", {
                        "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}], "Replays": []})
                    git_stage(root, initialize=True)
                    project = games.GameProject(project_file.parent, project_file, "Game")
                    expected = "Testing.Replays matches no replay"
                else:
                    project = make_game_repository(root, events=events)
                    expected = "no replay of Testing.Replays has input events"
                session = FakeEditorSession()
                session.document = run_document("Editor", [suite_row(SMOKE_SUITE)] + (
                    [replay_row(REPLAY, REPLAY_HASH)] if events is not None else []))
                step = self.run_editor_half(project, session)
                self.assertEqual(step.status, Status.FAILED)
                self.assertIn(expected, step.detail)
                self.assertEqual(session.replayed, [])
        steps = self.run_dist_half(["Dist"], FakeExportingSession(ran), FakeFeatureTestRun(REPLAY_HASH), events=0)
        self.assertEqual(steps["games Game Dist"].status, Status.FAILED)
        self.assertIn("no replay of Testing.Replays has input events", steps["games Game Dist"].detail)

    def test_the_editor_half_requires_each_replay_with_input_events_to_fail_without_them(self) -> None:
        # M16 friction S2-01 (FX-4): a committed replay whose verification still passes without its input events (S2's
        # version 1 autopilot replay: no checkpoint, and a restart in place before its one final hash) proves nothing
        # about the gameplay it recorded.
        for input_independent in (False, True):
            with self.subTest(input_independent=input_independent), \
                    tempfile.TemporaryDirectory(prefix="GamesInputCheck-") as directory:
                project = make_game_repository(Path(directory), events=2)
                session = FakeEditorSession(input_independent=input_independent)
                step = self.run_editor_half(project, session)
                self.assertEqual(session.replayed, [{"path": REPLAY, "verify": True, "strictHash": True,
                                                     "withoutInput": True, "continuePastCheckpoint": True}])
                order = ["test.run", "input.replay", "play.stop", "session.shutdown"]
                self.assertEqual([method for method in session.methods if method in order], order)
                if input_independent:
                    self.assertEqual(step.status, Status.FAILED)
                    self.assertIn(REPLAY, step.detail)
                    self.assertIn("proves nothing about its input", step.detail)
                else:
                    self.assertEqual(step.status, Status.PASSED, step.detail)
                    self.assertIn("1 replay(s) with input events fail without it", step.detail)
                    self.assertEqual(step.data["inputChecked"], [REPLAY])

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

    def run_dist_half(self, configs: list[str], session: FakeExportingSession, run: FakeFeatureTestRun,
                      require_gpu: bool = False, events: int = 2, native: bool = False) -> dict[str, Step]:
        """run_games over `configs` on a staged game whose replay has `events` input events, with a device, `session`
        as the Release editor and `run` as the exported package's --feature-test run; bin/Exports goes to a temporary
        directory. With `native` the game has a staged Native/ source, and the native preparation of its editors
        (games.prepare_native_editor) succeeds without building. Its steps by name."""
        with tempfile.TemporaryDirectory(prefix="GamesDistHalf-") as directory, contextlib.ExitStack() as stack:
            root = Path(directory)
            project = make_game_repository(root / "Repository", events=events)
            if native:
                (project.directory / "Native").mkdir()
                source = project.directory / "Native" / "Game.cpp"
                source.write_text("// The game's native module.\n", encoding="utf-8")
                git_stage(root / "Repository")
                stack.enter_context(mock.patch.object(games, "prepare_native_editor", return_value=[]))
            session.output_root = root / "EditorExports"
            passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
            argv = ["--suite", "games", "--config", ",".join(configs), *(["--require-gpu"] if require_gpu else [])]
            arguments = test_script.parse_arguments(argv)
            with mock.patch.object(games, "discover_projects", return_value=[project]), \
                    mock.patch.object(provenance, "audit_project", return_value=passing), \
                    mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "runtime_executable", return_value=Path(sys.executable)), \
                    mock.patch.object(test_script, "probe_rendering_editor", return_value=None), \
                    mock.patch.object(test_script, "import_engine_client", return_value=session.module()), \
                    mock.patch.object(test_script, "run_streamed", run), \
                    mock.patch.object(exports, "EXPORTS_ROOT", root / "Exports"):
                steps = test_script.run_games(configs, arguments, quiet_console())
                package = exports.package_directory("Dist", "Game")
                self.assertTrue(exports.package_executable(package, "Game").is_file())
        return {step.name: step for step in steps}

    def test_the_dist_half_runs_the_testing_export_outside_the_repository_and_compares_its_hashes(self) -> None:
        # ADR 0026 decision 8 step 3: the Release half's session exports for testing, the package runs --feature-test
        # in a temporary working directory (the export stage's FeatureTest-Dist reports are never overwritten), and its
        # hash must equal the committed one and the editor half's.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        session = FakeExportingSession(ran)
        run = FakeFeatureTestRun(REPLAY_HASH)
        steps = self.run_dist_half(["Release", "Dist"], session, run)
        self.assertEqual(steps["games Game Editor Release"].status, Status.PASSED, steps["games Game Editor Release"])
        self.assertEqual(steps["games Game Dist"].status, Status.PASSED, steps["games Game Dist"].detail)
        self.assertEqual(session.launches, 1)
        self.assertEqual(session.exports, [{"config": "Dist", "testing": True, "featureTest": True},
                                           {"config": "Dist", "smokeTest": True}])
        # The testing export runs the package's feature test itself (Exporter.h step 6a), so it is waited for that run's
        # bound beyond --timeout, which still bounds the plain export.
        timeout = test_script.SUITE_TIMEOUT_SECONDS
        self.assertEqual(session.export_timeouts, [timeout + test_script.EXPORT_FEATURE_TEST_RUN_SECONDS, timeout])
        self.assertEqual(len(run.directories), 1)
        self.assertFalse(run.directories[0].is_relative_to(REPOSITORY_ROOT))
        self.assertEqual(steps["games Game Dist"].data["hashes"], {"Editor Release": {REPLAY: REPLAY_HASH},
                                                                   "Exported Dist": {REPLAY: REPLAY_HASH}})

        drifted = self.run_dist_half(["Release", "Dist"], FakeExportingSession(ran),
                                     FakeFeatureTestRun("fedcba9876543210"))["games Game Dist"]
        self.assertEqual(drifted.status, Status.FAILED)
        self.assertIn(REPLAY, drifted.detail)
        self.assertTrue(any(line.startswith(f"{REPLAY}: Exported Dist: fedcba9876543210")
                            for line in drifted.data["failures"]), drifted.data["failures"])

    def test_the_dist_half_waits_for_the_exports_of_a_native_game_beyond_their_runtime_build(self) -> None:
        # The milestone review (Docs/Decisions/0030-m19-contract.md decisions 10 and 33): an export of a game with
        # Native/ builds its Runtime first (Exporter.h step 3a), bounded by NativeBuildBoundSeconds (NativeBuild.h), so
        # the Dist half waits that much longer for both its exports; a game without Native/ keeps the plain bounds.
        header = REPOSITORY_ROOT / "Editor" / "Source" / "EditorCore" / "Native" / "NativeBuild.h"
        pattern = r"constexpr uint32_t (NativeBuild\w+Seconds) = (\d+);"
        declared = re.findall(pattern, header.read_text(encoding="utf-8"))
        bounds = {name: int(value) for name, value in declared}
        self.assertEqual(test_script.EXPORT_NATIVE_BUILD_SECONDS,
                         bounds["NativeBuildTimeoutSeconds"] + bounds["NativeBuildMarginSeconds"])
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        session = FakeExportingSession(ran)
        steps = self.run_dist_half(["Release", "Dist"], session, FakeFeatureTestRun(REPLAY_HASH), native=True)
        self.assertEqual(steps["games Game Dist"].status, Status.PASSED, steps["games Game Dist"].detail)
        timeout = test_script.SUITE_TIMEOUT_SECONDS
        native = test_script.EXPORT_NATIVE_BUILD_SECONDS
        self.assertEqual(session.export_timeouts,
                         [timeout + test_script.EXPORT_FEATURE_TEST_RUN_SECONDS + native, timeout + native])

    def test_the_dist_half_compares_the_testing_exports_feature_test_hashes_with_its_own_run(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 10 (A-DISTSELF; Roadmap M19's acceptance "the hashes the builder
        # sees from the testing Dist export equal the games suite's Dist half"): the Dist half's testing export passes
        # featureTest, and the featureTest.replays hashes it reports must equal those the half's own --feature-test run
        # of the package reads; a difference fails the Dist step, naming the replay.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}

        def reporting(state_hash: str) -> Callable[[dict[str, Any]], object]:
            def edit(result: dict[str, Any]) -> None:
                result["featureTest"] = {"ran": True, "exitCode": 0, "passed": True, "reportPath": "",
                                         "replays": [{"path": REPLAY, "stateHash": state_hash, "passed": True}]}
            return edit

        session = EditedExportSession(ran, reporting(REPLAY_HASH))
        steps = self.run_dist_half(["Dist"], session, FakeFeatureTestRun(REPLAY_HASH))
        self.assertEqual(steps["games Game Dist"].status, Status.PASSED, steps["games Game Dist"].detail)
        self.assertEqual(session.exports[0], {"config": "Dist", "testing": True, "featureTest": True})

        differing = self.run_dist_half(["Dist"], EditedExportSession(ran, reporting("fedcba9876543210")),
                                       FakeFeatureTestRun(REPLAY_HASH))["games Game Dist"]
        self.assertEqual(differing.status, Status.FAILED)
        self.assertIn(REPLAY, differing.detail)
        self.assertIn("fedcba9876543210", differing.detail)

    def test_a_dist_smoke_test_that_did_not_run_warns_or_fails_with_require_gpu(self) -> None:
        not_run = {"smokeTestRan": False, "smokeTestExitCode": None, "warnings": ["no graphics device"]}
        for require_gpu, expected in ((False, Status.WARNING), (True, Status.FAILED)):
            with self.subTest(require_gpu=require_gpu):
                step = self.run_dist_half(["Dist"], FakeExportingSession(not_run),
                                          FakeFeatureTestRun(REPLAY_HASH), require_gpu)["games Game Dist"]
                self.assertEqual(step.status, expected, step.detail)
                self.assertIn("no graphics device", step.detail)

    def test_a_dist_only_run_exports_from_a_release_editor_of_its_own(self) -> None:
        session = FakeExportingSession({"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []})
        steps = self.run_dist_half(["Dist"], session, FakeFeatureTestRun(REPLAY_HASH))
        self.assertEqual(list(steps), ["games Game audit", "games Game Dist"])
        self.assertEqual(steps["games Game Dist"].status, Status.PASSED, steps["games Game Dist"].detail)
        # No editor half ran in that session: only the exports and the shutdown.
        self.assertEqual(session.methods, ["project.export", "project.export", "session.shutdown"])
        self.assertEqual(steps["games Game Dist"].data["hashes"], {"Exported Dist": {REPLAY: REPLAY_HASH}})

    def test_the_dist_half_finds_the_package_by_the_project_name_not_the_file_stem(self) -> None:
        # ADR 0027 decision 4, B2 (M17 pipeline map R6): the exporter names the executable, the testing user data and
        # its log after the project's Name (ProjectSettings::Name), while the Dist half looked for them under the
        # .eproj's file stem, so a game whose Name differs from its file stem failed with "the package holds no
        # .../Game.exe". The game's name comes from the testing export's manifest and its executable from
        # project.export's result; the steps and reports keep the stem. A testing export without a readable manifest
        # fails the Dist step with one line naming Game.json, as a package that cannot be installed does.
        name = "Falling"
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        for manifest in (True, False):
            with self.subTest(manifest=manifest), tempfile.TemporaryDirectory(prefix="GamesGameName-") as directory:
                session = FakeExportingSession(ran, name=name, manifest=manifest)
                run = FakeFeatureTestRun(REPLAY_HASH, name)
                root = Path(directory)
                repository = root / "Repository"
                project_file = write_project(repository, "Tests/Projects/Game", {
                    "Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}],
                    "Replays": ["Assets/Tests/Replays/*.replay"]})
                document = json.loads(project_file.read_text(encoding="utf-8"))
                document["Name"] = name
                project_file.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")
                write_replay(project_file.parent, events=2)
                git_stage(repository, initialize=True)
                project = games.GameProject(project_file.parent, project_file, "Game")
                session.output_root = root / "EditorExports"
                passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
                arguments = test_script.parse_arguments(["--suite", "games", "--config", "Dist"])
                with mock.patch.object(games, "discover_projects", return_value=[project]), \
                        mock.patch.object(provenance, "audit_project", return_value=passing), \
                        mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                        mock.patch.object(test_script, "runtime_executable", return_value=Path(sys.executable)), \
                        mock.patch.object(test_script, "probe_rendering_editor", return_value=None), \
                        mock.patch.object(test_script, "import_engine_client", return_value=session.module()), \
                        mock.patch.object(test_script, "run_streamed", run), \
                        mock.patch.object(exports, "EXPORTS_ROOT", root / "Exports"):
                    steps = {step.name: step for step in test_script.run_games(["Dist"], arguments, quiet_console())}
                    self.assertEqual(list(steps), ["games Game audit", "games Game Dist"])
                    step = steps["games Game Dist"]
                    package = exports.package_directory("Dist", name)
                    if not manifest:
                        self.assertEqual(step.status, Status.FAILED)
                        self.assertEqual(len(step.detail.splitlines()), 1, step.detail)
                        self.assertIn("Game.json", step.detail)
                        self.assertEqual(run.directories, [])
                        self.assertFalse(exports.package_directory("Dist", "Game").exists())
                        continue
                    self.assertEqual(step.status, Status.PASSED, step.detail)
                    self.assertFalse(exports.package_directory("Dist", "Game").exists())
                    self.assertEqual(step.data["package"], paths.display_path(package))
                    self.assertTrue(exports.package_executable(package, name).is_file())
                    self.assertTrue((exports.testing_user_data(package, name) / "Logs" / f"{name}.log").is_file())
                self.assertEqual(len(run.directories), 1)

    def test_the_dist_half_fails_at_once_on_a_testing_export_without_its_executable_below_its_output(self) -> None:
        # ADR 0027 decision 4, B2: the Dist half takes the executable from project.export's result and requires it below
        # the result's outputDirectory; a result without either, or an executable outside the output (an escape through
        # ".." included), fails the Dist step at once with one line naming it, and nothing is installed or run.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}

        def outside(result: dict[str, Any]) -> None:
            result["executable"] = str(Path(result["outputDirectory"]).parent / Path(result["executable"]).name)

        def escaping(result: dict[str, Any]) -> None:
            result["executable"] = str(Path(result["outputDirectory"]) / ".." / Path(result["executable"]).name)

        cases: dict[str, tuple[Callable[[dict[str, Any]], object], str]] = {
            "no executable": (lambda result: result.pop("executable"), "result has no executable"),
            "no output directory": (lambda result: result.pop("outputDirectory"), "result has no outputDirectory"),
            "outside": (outside, "is not below its outputDirectory"),
            "escaping": (escaping, "is not below its outputDirectory"),
        }
        for name, (edit, text) in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory(prefix="GamesDistLayout-") as directory:
                root = Path(directory)
                session = EditedExportSession(ran, edit)
                session.output_root = root / "EditorExports"
                run = FakeFeatureTestRun(REPLAY_HASH)
                step = self.run_dist_only(make_game_repository(root / "Repository"), session, run, root)
                self.assertEqual(step.status, Status.FAILED)
                self.assertEqual(len(step.detail.splitlines()), 1, step.detail)
                self.assertIn(text, step.detail)
                self.assertNotIn("package", step.data)
                self.assertFalse((root / "Exports").exists())
                self.assertEqual(run.directories, [])
                self.assertEqual(session.methods, ["project.export", "session.shutdown"])

    def test_on_macos_the_dist_half_reads_the_manifest_in_the_bundle_beside_the_executable(self) -> None:
        # ADR 0027 decision 4, B2: on macOS the manifest is <bundle>/Contents/Resources/Game.json, found from the
        # executable's Contents/MacOS folder, never spelled from the .eproj's stem or a name; a Game.json at the
        # output's root is not the package's. The Windows gate runs this branch only here.
        with tempfile.TemporaryDirectory(prefix="GamesMacBundle-") as directory:
            output = Path(directory) / "Falling"
            executable = output / "Falling.app" / "Contents" / "MacOS" / "Falling"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"")
            resources = output / "Falling.app" / "Contents" / "Resources"
            resources.mkdir()
            (resources / "Game.json").write_text(json.dumps({"Format": "GameManifest", "Version": 1,
                                                             "Name": "Falling"}), encoding="utf-8")
            (output / "Game.json").write_text(json.dumps({"Format": "GameManifest", "Version": 1, "Name": "Game"}),
                                              encoding="utf-8")
            result = {"outputDirectory": str(output), "executable": str(executable)}
            macos = types.SimpleNamespace(is_macos=True)
            with mock.patch.object(paths, "host", return_value=macos):
                self.assertEqual(test_script.locate_testing_package(result),
                                 (output, Path("Falling.app", "Contents", "MacOS", "Falling"), "Falling"))
                (resources / "Game.json").unlink()
                with self.assertRaises(ValueError) as raised:
                    test_script.locate_testing_package(result)
            self.assertIn(paths.display_path(resources / "Game.json"), str(raised.exception))
            windows = types.SimpleNamespace(is_macos=False)
            with mock.patch.object(paths, "host", return_value=windows):
                self.assertEqual(test_script.locate_testing_package(result)[2], "Game")

    def test_the_dist_half_runs_the_executable_that_the_testing_export_names(self) -> None:
        # ADR 0027 decision 4, B2 ("Frozen interfaces"): the executable is the installed package joined with
        # project.export's `executable` relative to its outputDirectory, never a path spelled from the Name. Here the
        # export names an executable that is not called after the game: the Dist half runs it, and nothing is looked
        # for at exports.package_executable(<package>, <Name>).
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        renamed: dict[str, Path] = {}

        def rename(result: dict[str, Any]) -> None:
            executable = Path(result["executable"])
            launcher = executable.with_name(paths.executable_name("Launcher"))
            executable.replace(launcher)
            result["executable"] = str(launcher)
            renamed["relative"] = launcher.relative_to(Path(result["outputDirectory"]))

        with tempfile.TemporaryDirectory(prefix="GamesDistExecutable-") as directory:
            root = Path(directory)
            session = EditedExportSession(ran, rename)
            session.output_root = root / "EditorExports"
            run = FakeFeatureTestRun(REPLAY_HASH)
            step = self.run_dist_only(make_game_repository(root / "Repository"), session, run, root)
            package = root / "Exports" / "Dist" / "Game"
            self.assertEqual(step.status, Status.PASSED, step.detail)
            self.assertEqual([command[0] for command in run.commands], [str(package / renamed["relative"])])
            self.assertFalse(exports.package_executable(package, "Game").exists())

    def test_the_dist_half_finds_the_log_named_after_the_executable_stem_not_the_name(self) -> None:
        # The Runtime names its log after its executable's stem (ProcessContext.cpp; Docs/Architecture.md §4.4:
        # Logs/<exe>.log), while its user data takes the Name. The exporter calls the executable <Name>.exe on Windows
        # but <Name> on Linux and macOS, so a Name with a dot in it, here "Tetris.Classic", logs to Tetris.log there:
        # the Dist half looked for Tetris.Classic.log and failed a run that had passed. The export is given the Linux
        # layout (its executable without .exe) on every host, the macOS bundle's executable already having it.
        name = "Tetris.Classic"
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}

        def without_exe(result: dict[str, Any]) -> None:
            executable = Path(result["executable"])
            if executable.suffix == ".exe":
                result["executable"] = str(executable.replace(executable.with_suffix("")))

        with tempfile.TemporaryDirectory(prefix="GamesDistLog-") as directory:
            root = Path(directory)
            session = EditedExportSession(ran, without_exe, name)
            session.output_root = root / "EditorExports"
            run = FakeFeatureTestRun(REPLAY_HASH, name)
            step = self.run_dist_only(make_game_repository(root / "Repository"), session, run, root)
            logs = root / "Exports" / "Dist" / name / "bin" / "TestUserData" / name / "Logs"
            self.assertEqual(step.status, Status.PASSED, step.detail)
            self.assertEqual(Path(run.commands[0][0]).name, name)
            self.assertTrue((logs / "Tetris.log").is_file())
            self.assertFalse((logs / f"{name}.log").exists())

    def test_the_dist_half_checks_the_log_of_its_own_run_not_the_one_the_export_left(self) -> None:
        # The review of W19.1 (Docs/Decisions/0030-m19-contract.md decision 10): the testing export's own feature-test
        # run writes its log into the package's testing user data, where the half's check_testing_user_data looks for
        # the log of its own run, so a run that wrote no log there passed. The half removes the export's user data
        # before its run: a run that writes no log fails, naming the log, and one that writes it passes.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        for writes_log in (False, True):
            with self.subTest(writes_log=writes_log), \
                    tempfile.TemporaryDirectory(prefix="GamesDistOwnLog-") as directory:
                root = Path(directory)
                session = FakeExportingSession(ran)
                session.output_root = root / "EditorExports"
                run = FakeFeatureTestRun(REPLAY_HASH, writes_log=writes_log)
                step = self.run_dist_only(make_game_repository(root / "Repository"), session, run, root)
                log = exports.testing_user_data(root / "Exports" / "Dist" / "Game", "Game") / "Logs" / \
                    f"{Path(run.commands[0][0]).stem}.log"
                if writes_log:
                    self.assertEqual(step.status, Status.PASSED, step.detail)
                    self.assertEqual(log.read_text(encoding="utf-8"), "ran\n")
                    continue
                self.assertEqual(step.status, Status.FAILED)
                self.assertIn(f"the run wrote no {paths.display_path(log)}", step.data["failures"])
                self.assertFalse(log.exists())

    def run_dist_only(self, project: games.GameProject, session: FakeExportingSession, run: FakeFeatureTestRun,
                      root: Path) -> Step:
        """run_games in Dist on the staged `project` with a device, `session` as the Release editor, `run` as the
        package's --feature-test run and bin/Exports in `root`: its "games <Name> Dist" step."""
        passing = provenance.ProjectAudit(project.directory, "index", {}, [], [])
        arguments = test_script.parse_arguments(["--suite", "games", "--config", "Dist"])
        with mock.patch.object(games, "discover_projects", return_value=[project]), \
                mock.patch.object(provenance, "audit_project", return_value=passing), \
                mock.patch.object(test_script, "editor_executable", return_value=Path(sys.executable)), \
                mock.patch.object(test_script, "runtime_executable", return_value=Path(sys.executable)), \
                mock.patch.object(test_script, "probe_rendering_editor", return_value=None), \
                mock.patch.object(test_script, "import_engine_client", return_value=session.module()), \
                mock.patch.object(test_script, "run_streamed", run), \
                mock.patch.object(exports, "EXPORTS_ROOT", root / "Exports"):
            steps = test_script.run_games(["Dist"], arguments, quiet_console())
        return next(step for step in steps if step.name == f"games {project.name} Dist")

    def test_a_dist_only_run_fails_its_dist_step_on_what_fails_after_the_exports(self) -> None:
        # The milestone review's R08: without an editor half, a refused session.shutdown, a lost connection or an exit
        # that timed out after the Dist checks had passed was dropped, so the Dist step passed.
        ran = {"smokeTestRan": True, "smokeTestExitCode": 0, "warnings": []}
        cases = {
            "refused": (FakeEngineError("the open scene has unsaved changes", "InvalidState"), None, Status.FAILED,
                        "session.shutdown refused to exit (the open scene has unsaved changes)"),
            "connection": (ConnectionError("the editor closed the connection"), None, Status.FAILED,
                           "the Release editor failed: the editor closed the connection"),
            "timeout": (None, TimeoutError("no exit within 60 s"), Status.TIMEOUT,
                        "the Release editor did not answer in time: no exit within 60 s"),
        }
        for name, (shutdown_error, wait_error, status, text) in cases.items():
            with self.subTest(case=name):
                session = FakeExportingSession(ran, shutdown_error=shutdown_error, wait_error=wait_error)
                step = self.run_dist_half(["Dist"], session, FakeFeatureTestRun(REPLAY_HASH))["games Game Dist"]
                self.assertEqual(step.status, status, step.detail)
                self.assertIn(text, step.detail)
                self.assertEqual(len(step.data["failures"]), 1, step.data["failures"])
                self.assertIn(text, step.data["failures"][0])

    def test_the_shared_helpers_take_a_game_name_and_leave_out_build_output(self) -> None:
        # Decision 8's refactors: copy_project leaves out Build/ as well as Library/ (unless keep_cache),
        # install_package installs a package under its game's name, and runtime_executable is the export suite's
        # Runtime lookup.
        with tempfile.TemporaryDirectory(prefix="GamesHelpers-") as directory:
            root = Path(directory)
            source = write_project(root, "Source/Game").parent
            for relative in ("Build/Dist/Game/Game.exe", "Library/Cache/Any.bin", "Assets/Scenes/Main.scene"):
                (source / relative).parent.mkdir(parents=True, exist_ok=True)
                (source / relative).write_text("x", encoding="utf-8")
            for keep_cache, expected in ((False, ["Assets", "Game.eproj"]),
                                         (True, ["Assets", "Game.eproj", "Library"])):
                with self.subTest(keep_cache=keep_cache):
                    destination = root / f"Copy-{keep_cache}"
                    test_script.copy_project(source, destination, keep_cache=keep_cache)
                    self.assertEqual(sorted(path.name for path in destination.iterdir()), expected)
            output = root / "Output"
            output.mkdir()
            (output / "Game.bin").write_text("x", encoding="utf-8")
            with mock.patch.object(exports, "EXPORTS_ROOT", root / "Exports"):
                package = test_script.install_package(output, "Dist", "Game")
            self.assertEqual(package, root / "Exports" / "Dist" / "Game")
            self.assertTrue((package / "Game.bin").is_file())
        runtime = Path("bin") / paths.output_directory_name("Dist") / "Runtime" / paths.executable_name("Runtime")
        self.assertEqual(test_script.runtime_executable("Dist", Path("bin")), runtime)

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

    def test_the_editor_half_passes_a_game_and_fails_a_hand_edited_hash(self) -> None:
        # The suite's editor half starts a rendering editor (decision 8).
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor())
        root = self.create_project(client, "Game")
        build_tiny_game(client)
        # A prefab instance that overrides a script field, as Breakout's brick rows do: the suite's --strict editor must
        # open it (the M16 dry run's S1-04, where every --strict editor refused such a project).
        brick = "Assets/Prefabs/Brick.prefab"
        client.call("script.write", {"path": "Assets/Scripts/Brick.luau", "source": "--!strict\nlocal Brick = {}\n"
                                     "Brick.Fields = { Points = Field.Integer(1, { Min = 1 }) }\n"
                                     "return Script.Define(\"Brick\", Brick)\n"})
        client.call("entity.create", {"name": "Brick", "components": {
            "Transform": {"Translation": [0, 3, -4]},
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Brick.luau", "Fields": {"Points": 1}}]}}})
        client.call("prefab.create", {"entity": "/Brick", "path": brick, "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": brick, "name": "Brick2",
                                           "components": {"Script": {"Scripts": [{"Fields": {"Points": 5}}]}}})
        # A game needs a replay whose input plays it (games.check_replay_acceptance): the suite taps a key the Hopper
        # moves on, so the recording carries input events and fails the input check's run without them.
        client.call("script.write", {"path": "Assets/Scripts/Hopper.luau", "source": "--!strict\nlocal Hopper = {}\n"
                                     "function Hopper.OnFixedUpdate(self: any, dt: number)\n"
                                     "\tif Input.IsKeyDown(\"A\") then\n"
                                     "\t\tself.Entity.Transform:Translate(vector.create(0, 1, 0))\n\tend\nend\n"
                                     "return Script.Define(\"Hopper\", Hopper)\n"})
        client.call("entity.create", {"name": "Hopper", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Hopper.luau"}]}}})
        client.call("script.write", {"path": SMOKE_SUITE, "source": "--!strict\nreturn Test.Suite(\"Smoke\", "
                                     "function()\n\tTest.Case(\"passes\", function()\n\t\tTest.InjectKey(\"A\", "
                                     "\"Tap\")\n\t\tTest.WaitTicks(4)\n\t\tTest.Expect(true, \"true\")\n\tend)\n"
                                     "end)\n"})
        # The demo acceptance's build settings, which the editor half checks first (ADR 0027 decision 4, B1).
        client.call("project.setSettings", {"patch": {
            "StartScene": TINY_SCENE, "Export": {"BuildScenes": [TINY_SCENE]},
            "Testing": {"Suites": [{"Script": SMOKE_SUITE, "Scene": TINY_SCENE}],
                        "Replays": ["Assets/Tests/Replays/*.replay"]}}})
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
        passed = next(step for step in steps if step.name == f"games Game Editor {automation_config()}")
        self.assertEqual(passed.status, Status.PASSED, passed.detail)
        self.assertEqual(passed.data["inputChecked"], [REPLAY])
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
