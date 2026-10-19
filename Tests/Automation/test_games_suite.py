"""Scripts/Lib/games.py and the games suite and stage (Docs/Architecture.md §15.1 T5, §15.8; Roadmap M16 to M18;
Docs/Decisions/0026-m16-contract.md decisions 7 and 8, stream B): project discovery and the Testing settings, the
evaluation of a test run, of script.check, project.validate, the upgrade dry run and the smoke export; the suite's
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
from Lib import exports, games, hardening, paths, provenance  # noqa: E402
from Lib.process import ProcessResult  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402

SMOKE_SUITE = "Assets/Tests/Smoke.test.luau"
EDITOR_ONLY_SUITE = "Assets/Tests/EditorOnly.test.luau"
REPLAY = "Assets/Tests/Replays/Rally.replay"
REPLAY_HASH = "0123456789abcdef"
SKIP_BUILD_SETTINGS = "contract stub: games.check_build_settings (ADR 0027 decision 4, B1, stream B)"
SKIP_GAME_NAME = "contract stub: the Dist half names the package after the game (ADR 0027 decision 4, B2, stream B)"


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
    def __init__(self, message: str, error_code: str = "Unknown") -> None:
        super().__init__(message)
        self.error_code = error_code
        self.detail = message


class FakeEditorSession:
    """The editor half's editor, faked through Test.py's import_engine_client seam (the editor halves start editors
    through it like the export and determinism suites, decision 8): canned results per method, a hook run on test.run
    with the copy's directory, and the exit code the process ends with after session.shutdown. It is the process and
    the client at once. input.replay {withoutInput: true} fails with Validation unless `input_independent` (the replay's
    verification then passes without its input)."""

    def __init__(self, exit_code: int = 0, on_test_run: Callable[[Path], None] | None = None,
                 input_independent: bool = False, shutdown_error: Exception | None = None,
                 wait_error: Exception | None = None) -> None:
        self.exit_code = exit_code
        self.on_test_run = on_test_run
        self.input_independent = input_independent
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
                raise FakeEngineError("replay verification failed at checkpoint tick 60", "Validation")
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
    (ExportMethods.cpp, Exporter.cpp). The plain export reports `smoke`. It counts its launches, so a test sees whether
    the Release half's session was shared."""

    def __init__(self, smoke: dict[str, Any], shutdown_error: Exception | None = None,
                 wait_error: Exception | None = None, name: str = "Game", manifest: bool = True) -> None:
        super().__init__(shutdown_error=shutdown_error, wait_error=wait_error)
        self.output_root: Path | None = None
        self.smoke = smoke
        self.name = name
        self.manifest = manifest
        self.launches = 0
        self.exports: list[dict[str, Any]] = []

    def launch_editor(self, editor: Path, arguments: list[str], *rest: object, **options: object) -> FakeEditorSession:
        self.launches += 1
        return super().launch_editor(editor, arguments, *rest, **options)

    def call(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        if method != "project.export":
            return super().call(method, params, timeout)
        self.methods.append(method)
        self.exports.append(dict(params or {}))
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
        return {"outputDirectory": str(output), "executable": str(executable), "testSuiteCount": 1, "replayCount": 1}


class FakeFeatureTestRun:
    """`<package>/Game --headless --feature-test` faked through Test.py's run_streamed: it writes the Runtime's reports
    (FeatureTest-Dist.{json,xml} below its working directory, whatever the game) with `state_hash` as the replay's hash,
    and the package's testing log; it records each run's working directory."""

    def __init__(self, state_hash: str, name: str = "Game") -> None:
        self.state_hash = state_hash
        self.name = name  # the game's Name: its package directory, testing user-data folder and log are named after it
        self.directories: list[Path] = []

    def __call__(self, command: list[str], cwd: Path | None = None, **options: object) -> ProcessResult:
        directory = Path(cwd or ".")
        self.directories.append(directory)
        reports = directory / "bin" / "TestResults"
        reports.mkdir(parents=True, exist_ok=True)
        document = run_document("Dist", [suite_row(SMOKE_SUITE), replay_row(REPLAY, self.state_hash)])
        (reports / "FeatureTest-Dist.json").write_text(json.dumps(document), encoding="utf-8")
        (reports / "FeatureTest-Dist.xml").write_text("<testsuites/>", encoding="utf-8")
        package = exports.package_directory("Dist", self.name)
        log = exports.testing_user_data(package, self.name) / "Logs" / f"{self.name}.log"
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

    def test_a_game_needs_a_committed_replay_with_input_events(self) -> None:
        # The milestone review's R09: Roadmap M16 to M18 accept a demo by its suites and a replay recorded from tick 0
        # that plays it, so a game whose Testing.Replays matches nothing, or only replays without input, fails.
        idle = "Assets/Tests/Replays/Idle.replay"
        self.assertEqual(games.check_replay_acceptance({REPLAY: 3}), [])
        self.assertEqual(games.check_replay_acceptance({REPLAY: 3, idle: 0}), [])
        none = games.check_replay_acceptance({})
        self.assertEqual(len(none), 1)
        self.assertIn("Testing.Replays matches no replay", none[0])
        without_input = games.check_replay_acceptance({REPLAY: 0, idle: 0})
        self.assertEqual(len(without_input), 1)
        for text in ("no replay of Testing.Replays has input events", REPLAY, idle, "Roadmap M16 to M18"):
            self.assertIn(text, without_input[0])

    @unittest.skip(SKIP_BUILD_SETTINGS)
    def test_a_game_needs_a_start_scene_that_its_build_scenes_list(self) -> None:
        # ADR 0027 decision 4, B1 (M17 pipeline map G2): Roadmap M17 and M18 accept a demo with "StartScene and
        # Export.BuildScenes set", but the validator and the exporter accept an empty BuildScenes (FeatureTest relies on
        # it), so only the games suite checks the rule; that BuildScenes lists StartScene is this ADR's convention.
        main = "Assets/Scenes/Main.scene"
        roadmap = "(Roadmap M17 and M18: StartScene and Export.BuildScenes set)"
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

    @unittest.skip(SKIP_GAME_NAME)
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

    @unittest.skip(SKIP_BUILD_SETTINGS)
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
                                                     "withoutInput": True}])
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
                      require_gpu: bool = False, events: int = 2) -> dict[str, Step]:
        """run_games over `configs` on a staged game whose replay has `events` input events, with a device, `session`
        as the Release editor and `run` as the exported package's --feature-test run; bin/Exports goes to a temporary
        directory. Its steps by name."""
        with tempfile.TemporaryDirectory(prefix="GamesDistHalf-") as directory:
            root = Path(directory)
            project = make_game_repository(root / "Repository", events=events)
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
        self.assertEqual(session.exports, [{"config": "Dist", "testing": True}, {"config": "Dist", "smokeTest": True}])
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

    @unittest.skip(SKIP_GAME_NAME)
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
            "Script": {"Script": "Assets/Scripts/Brick.luau", "Fields": {"Points": 1}}}})
        client.call("prefab.create", {"entity": "/Brick", "path": brick, "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": brick, "name": "Brick2",
                                           "components": {"Script": {"Fields": {"Points": 5}}}})
        # A game needs a replay whose input plays it (games.check_replay_acceptance): the suite taps a key the Hopper
        # moves on, so the recording carries input events and fails the input check's run without them.
        client.call("script.write", {"path": "Assets/Scripts/Hopper.luau", "source": "--!strict\nlocal Hopper = {}\n"
                                     "function Hopper.OnFixedUpdate(self: any, dt: number)\n"
                                     "\tif Input.IsKeyDown(\"A\") then\n"
                                     "\t\tself.Entity.Transform:Translate(vector.create(0, 1, 0))\n\tend\nend\n"
                                     "return Script.Define(\"Hopper\", Hopper)\n"})
        client.call("entity.create", {"name": "Hopper",
                                      "components": {"Script": {"Script": "Assets/Scripts/Hopper.luau"}}})
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
