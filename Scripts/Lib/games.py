"""The demo game projects and the evaluation helpers of the games suite (Docs/Architecture.md §15.1 T5, §15.8, §13.12;
Roadmap M16 to M18; Docs/Decisions/0026-m16-contract.md decision 7, stream B; Docs/Decisions/0027-m17-contract.md
decision 4, stream B), shared by Scripts/Test.py's games suite and Scripts/AuditProvenance.py --demos.

A game project is a directory directly below one of GAME_PROJECT_ROOTS whose root holds exactly one *.eproj, except
EXCLUDED_PROJECTS: Projects/FeatureTest is covered by the feature, export and determinism suites and never audited (ADR
0025 decision 7). hardening.soak_projects keeps its own list (every Projects/* with a .eproj, FeatureTest included; the
dry run under Tests/Projects is not soaked, ADR 0025), so the soak never depends on this module.

Discovery reads the working tree ("disk") or the staged files ("index", what a fresh clone of the commit about to be
made holds; git ls-files): the games suite and AuditProvenance.py --demos in index mode (their default) audit and run
only staged projects, and name each project found on disk but not staged in a passed step, so a half-built demo that
is not part of a commit never fails it and is never skipped silently. Apart from index discovery, which runs git, the
helpers are pure: they read files or the documents the editor and the Runtime return, start nothing, and return failure
lines ("<what>: <why>"), an empty list meaning the check passed.

The input check (M16 friction S2-01, FX-4): every committed replay with input events is also verified without them
(input.replay {verify: true, strictHash: true, withoutInput: true}; withoutInput without verify is refused, since it
would check nothing) in the editor halves, and that verification must fail (evaluate_input_check),
because a replay whose outcome does not depend on its input proves nothing about the gameplay it recorded. One rule
decides input independence everywhere (Docs/Architecture.md §13.6; the recording guards of input.record and test.run
{record} apply it too): a replay is input-independent when its input-free run passes verification (every checkpoint,
the final state hash, every expectation), whatever script errors that run raised. A game also needs at least one test
suite (evaluate_run) and at least one committed replay with input events (check_replay_acceptance), as the demo
acceptance of Roadmap M16 to M18 asks, and the build settings that acceptance names from M17 on, "StartScene and
Export.BuildScenes set", with Export.BuildScenes listing StartScene (check_build_settings, the first check of each
editor half; ADR 0027 decision 4, B1).

A GameProject's name is its .eproj's file stem, which names the suite's steps and reports only. The exporter names the
package's executable and its testing user data after the project's Name (ProjectSettings::Name), and the Runtime names
its log after the executable's stem (Docs/Architecture.md §4.4: Logs/<exe>.log), so the Dist half reads that Name from
the testing export's manifest (read_game_name; ADR 0027 decision 4, B2) and takes the executable from project.export's
result.
"""

from __future__ import annotations

import dataclasses
import json
import re
import subprocess
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from Lib import exports, paths, provenance
from Lib.process import format_command

GAME_PROJECT_ROOTS = ("Projects", "Tests/Projects")
EXCLUDED_PROJECTS = ("Projects/FeatureTest",)
# The run modes of §15.6 a Testing.Suites entry may name, and its default (ProjectSettings.h: TestSuiteSettings::Modes).
TEST_MODES = ("Editor", "Release", "Dist")
DEFAULT_SUITE_MODES = TEST_MODES
# The suite rows the runner writes for replays (FeatureTestRunner: "replay:<project-relative path>"), as the export and
# determinism suites read them.
REPLAY_SUITE_PREFIX = exports.REPLAY_SUITE_PREFIX
# The editor configurations whose halves the suite runs, and the exported configuration the Release editor exports
# (Dist, the last of the export suite's configurations).
EDITOR_CONFIGURATIONS = exports.EDITOR_CONFIGURATIONS
EXPORTED_CONFIGURATION = exports.EXPORTED_CONFIGURATIONS[-1]
# Report names below bin/TestResults/: Games-<Config>.xml (JUnit), Games-<Name>-Editor-<Config>.json (the editor half's
# test.run document) and Games-<Name>-Dist.{json,xml} (the exported --feature-test run's reports).
REPORT_PREFIX = "Games"
# The command a changed file of the upgrade dry run names (§13.12).
UPGRADE_COMMAND = "Editor --headless --project {project} --upgrade"

PROJECT_FILE_SUFFIX = ".eproj"
PROJECT_FORMAT = "Project"
# The package manifest of an export (Engine/Project/GameManifest.h: GameManifest::FileName and FormatName), at the
# package's root or, on macOS, in the bundle's Contents/Resources (Exporter.cpp: GetPackageResources).
GAME_MANIFEST_FILE = "Game.json"
GAME_MANIFEST_FORMAT = "GameManifest"
# What a game's Name never holds, since the package directory, its executable and the testing user-data folder are
# named after it (Engine/Platform/Paths.cpp: ValidateAppName refuses these characters and the relative folders "." and
# "..").
FORBIDDEN_NAME_CHARACTERS = '<>:"/\\|?*'
RELATIVE_FOLDER_NAMES = (".", "..")
# The failure lines of check_build_settings (ADR 0027 decision 4, B1, "Frozen interfaces"): the demo acceptance's rule
# (Roadmap M17 and M18) and the demos' convention that Export.BuildScenes lists StartScene.
BUILD_SETTINGS_RULE = "(Roadmap M17 and M18: StartScene and Export.BuildScenes set)"
START_SCENE_EMPTY = f"StartScene is empty: set it to the scene the game starts with {BUILD_SETTINGS_RULE}"
BUILD_SCENES_EMPTY = ("Export.BuildScenes is empty: list the scenes the export must load strictly, StartScene "
                      f"included {BUILD_SETTINGS_RULE}")
START_SCENE_NOT_LISTED = ("Export.BuildScenes does not list the StartScene '{scene}': add it (ADR 0027 decision 4: the "
                          "demos' convention)")
REPLAY_FILE_SUFFIX = ".replay"
ASSETS_DIRECTORY = "Assets"
GIT_TIMEOUT_SECONDS = 120.0
# A committed FinalStateHash: 16 hexadecimal digits (Engine/Source/Engine/Asset/ReplayData.h: ReplayToText writes them
# lowercase).
STATE_HASH_PATTERN = re.compile(r"[0-9a-fA-F]{16}")


@dataclasses.dataclass(frozen=True)
class GameProject:
    """One discovered game project."""

    directory: Path  # absolute
    project_file: Path  # directory / "<name>.eproj"
    # The .eproj's file stem, which names the suite's steps and reports only: the exported package, its executable and
    # testing user data take the project's Name (read_game_name), whatever the stem, and the log the executable's stem.
    name: str


@dataclasses.dataclass(frozen=True)
class SuiteEntry:
    """One Testing.Suites entry of a .eproj (the members the games suite reads)."""

    script: str  # project-relative "Assets/Tests/....test.luau"
    scene: str  # project-relative scene, "" for a suite without one
    modes: tuple[str, ...]  # of TEST_MODES, in the .eproj's order; DEFAULT_SUITE_MODES when the member is absent


@dataclasses.dataclass(frozen=True)
class TestingSettings:
    """The Testing settings of a .eproj that the games suite reads."""

    suites: tuple[SuiteEntry, ...]
    replays: tuple[str, ...]  # the Testing.Replays globs as written, "project://" prefixes included


def discover_projects(root: Path, files: str = "disk") -> list[GameProject]:
    """The game projects below `root` (the repository root): every directory directly below a GAME_PROJECT_ROOTS entry
    whose root holds exactly one *.eproj, except EXCLUDED_PROJECTS (compared as root-relative paths with "/"
    separators), sorted by their root-relative path. `files` is one of Lib/provenance.py's FILE_MODES: "disk" reads the
    working tree; "index" reads the staged files (`git -C <root> ls-files -z -- <GAME_PROJECT_ROOTS>`), so a project
    counts only when its *.eproj is staged, and an untracked or ignored one does not. A root that does not exist
    contributes nothing; no project at all is an empty list. Raises ValueError naming the directory when one holds
    several *.eproj files, or for a `files` value outside FILE_MODES, and RuntimeError naming the git command and its
    output when git is missing or fails in index mode."""
    if files not in provenance.FILE_MODES:
        raise ValueError(f"unknown file source '{files}' (expected {' or '.join(provenance.FILE_MODES)})")
    base = root.absolute()
    # {root-relative project directory: the names of its *.eproj files}
    candidates = _staged_project_files(base) if files == "index" else _disk_project_files(base)
    projects: list[GameProject] = []
    for relative in sorted(candidates):
        if relative in EXCLUDED_PROJECTS:
            continue
        names = sorted(candidates[relative])
        if len(names) != 1:
            raise ValueError(f"{relative} holds {len(names)} *{PROJECT_FILE_SUFFIX} files ({', '.join(names)}); a game "
                             f"project holds exactly one")
        directory = base / relative
        projects.append(GameProject(directory, directory / names[0], Path(names[0]).stem))
    return projects


def _disk_project_files(base: Path) -> dict[str, list[str]]:
    """The *.eproj files directly in each directory directly below a GAME_PROJECT_ROOTS entry of the working tree."""
    candidates: dict[str, list[str]] = {}
    for project_root in GAME_PROJECT_ROOTS:
        directory = base / project_root
        if not directory.is_dir():
            continue
        for child in directory.iterdir():
            if not child.is_dir():
                continue
            names = [entry.name for entry in child.iterdir() if entry.suffix == PROJECT_FILE_SUFFIX and entry.is_file()]
            if names:
                candidates[f"{project_root}/{child.name}"] = names
    return candidates


def _staged_project_files(base: Path) -> dict[str, list[str]]:
    """The staged *.eproj files directly in each directory directly below a GAME_PROJECT_ROOTS entry (git ls-files)."""
    command = ["git", "-C", str(base), "ls-files", "-z", "--", *GAME_PROJECT_ROOTS]
    try:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=GIT_TIMEOUT_SECONDS,
                                   check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"{format_command(command)} could not run: {error}") from error
    if completed.returncode != 0:
        output = completed.stderr.decode("utf-8", errors="replace").strip() or "no output"
        raise RuntimeError(f"{format_command(command)} exited with code {completed.returncode}: {output}")
    candidates: dict[str, list[str]] = {}
    for entry in completed.stdout.decode("utf-8", errors="surrogateescape").split("\0"):
        for project_root in GAME_PROJECT_ROOTS:
            if not entry.startswith(f"{project_root}/"):
                continue
            parts = entry[len(project_root) + 1:].split("/")
            if len(parts) == 2 and parts[1].endswith(PROJECT_FILE_SUFFIX):
                candidates.setdefault(f"{project_root}/{parts[0]}", []).append(parts[1])
    return candidates


def _read_json_object(path: Path, what: str) -> dict[str, Any]:
    """The JSON object in the UTF-8 file `path`; ValueError naming `what` otherwise."""
    try:
        document = json.loads(path.read_bytes().decode("utf-8"))
    except OSError as error:
        raise ValueError(f"{what}: cannot be read: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{what}: not UTF-8 JSON: {error}") from error
    if not isinstance(document, dict):
        raise ValueError(f"{what}: not a JSON object")
    return document


def _read_project(project_file: Path) -> tuple[str, dict[str, Any]]:
    """(the display path, the document) of the .eproj `project_file`; ValueError naming the file for one that cannot be
    read or is not a project file's JSON object."""
    name = paths.display_path(project_file)
    document = _read_json_object(project_file, name)
    if document.get("Format") != PROJECT_FORMAT:
        raise ValueError(f"{name}: not a project file (its Format is {document.get('Format')!r}, not "
                         f"'{PROJECT_FORMAT}')")
    return name, document


def read_testing(project_file: Path) -> TestingSettings:
    """The Testing settings of the .eproj `project_file`: each Testing.Suites entry's Script, Scene and Modes (absent
    Modes are DEFAULT_SUITE_MODES) and Testing.Replays, absent members read as empty. Raises ValueError naming the file
    for a document that is not a .eproj JSON object, or a member of the wrong type."""
    name, document = _read_project(project_file)
    testing = document.get("Testing", {})
    if not isinstance(testing, dict):
        raise ValueError(f"{name}: Testing must be an object")
    suites_value = testing.get("Suites", [])
    if not isinstance(suites_value, list):
        raise ValueError(f"{name}: Testing.Suites must be an array")
    suites: list[SuiteEntry] = []
    for index, entry in enumerate(suites_value):
        where = f"{name}: Testing.Suites[{index}]"
        if not isinstance(entry, dict):
            raise ValueError(f"{where} must be an object")
        script = entry.get("Script")
        if not isinstance(script, str):
            raise ValueError(f"{where}.Script must be a string")
        scene = entry.get("Scene", "")
        if scene is None:
            scene = ""
        if not isinstance(scene, str):
            raise ValueError(f"{where}.Scene must be a string")
        modes = entry.get("Modes", list(DEFAULT_SUITE_MODES))
        if not isinstance(modes, list) or not all(isinstance(mode, str) for mode in modes):
            raise ValueError(f"{where}.Modes must be an array of strings")
        unknown = [mode for mode in modes if mode not in TEST_MODES]
        if unknown:
            raise ValueError(f"{where}.Modes names the unknown run mode '{unknown[0]}' (expected "
                             f"{', '.join(TEST_MODES)})")
        suites.append(SuiteEntry(script, scene, tuple(modes)))
    replays = testing.get("Replays", [])
    if not isinstance(replays, list) or not all(isinstance(replay, str) for replay in replays):
        raise ValueError(f"{name}: Testing.Replays must be an array of strings")
    return TestingSettings(tuple(suites), tuple(replays))


def check_build_settings(project_file: Path) -> list[str]:
    """The failures of the .eproj `project_file` against the demo acceptance's build settings (Roadmap M17 and M18:
    "StartScene and Export.BuildScenes set"; Docs/Decisions/0027-m17-contract.md decision 4, B1, stream B), one line
    each, in this order:

    - StartScene absent or empty: "StartScene is empty: set it to the scene the game starts with (Roadmap M17 and
      M18: StartScene and Export.BuildScenes set)";
    - Export or Export.BuildScenes absent, or BuildScenes empty: "Export.BuildScenes is empty: list the scenes the
      export must load strictly, StartScene included (Roadmap M17 and M18: StartScene and Export.BuildScenes set)";
    - a non-empty StartScene that a non-empty Export.BuildScenes does not list (compared as written):
      "Export.BuildScenes does not list the StartScene '<StartScene>': add it (ADR 0027 decision 4: the demos'
      convention)".

    An empty list when every rule holds. Export.BuildScenes decides which scenes the export loads strictly beside
    StartScene, not what ships (Game.pak holds every registered asset below Assets/). The engine is unchanged: an empty
    Export.BuildScenes stays valid for the validator and the exporter (FeatureTest relies on it), and only the games
    suite applies this rule, first in each editor half (check_game_in_editor, on the index copy's project file, its
    lines first among the half's failures), so PreCommit's Debug half catches it. Raises ValueError naming the file for
    a file that cannot be read, a document that is not a .eproj JSON object (Format "Project"), a StartScene that is not
    a string, an Export that is not an object or an Export.BuildScenes that is not an array of strings."""
    name, document = _read_project(project_file)
    start_scene = document.get("StartScene", "")
    if not isinstance(start_scene, str):
        raise ValueError(f"{name}: StartScene must be a string")
    export = document.get("Export", {})
    if not isinstance(export, dict):
        raise ValueError(f"{name}: Export must be an object")
    build_scenes = export.get("BuildScenes", [])
    if not isinstance(build_scenes, list) or not all(isinstance(scene, str) for scene in build_scenes):
        raise ValueError(f"{name}: Export.BuildScenes must be an array of strings")
    failures: list[str] = []
    if not start_scene:
        failures.append(START_SCENE_EMPTY)
    if not build_scenes:
        failures.append(BUILD_SCENES_EMPTY)
    if start_scene and build_scenes and start_scene not in build_scenes:
        failures.append(START_SCENE_NOT_LISTED.format(scene=start_scene))
    return failures


def read_game_name(path: Path) -> str:
    """The Name of the game whose package manifest (Game.json, Format "GameManifest", Engine/Project/GameManifest.h)
    or project file (.eproj, Format "Project") is `path`: the name the exporter gives the executable and the testing
    user-data folder (Exporter.cpp names them after ProjectSettings::Name; the log in that folder takes the
    executable's stem, Docs/Architecture.md §4.4), whatever the .eproj's file stem (Docs/Decisions/0027-m17-contract.md
    decision 4, B2, stream B). The Dist half reads it, before it installs the package to
    exports.package_directory(EXPORTED_CONFIGURATION, <Name>), from the testing export's manifest:
    <outputDirectory>/Game.json, or on macOS the bundle's Contents/Resources/Game.json beside the Contents/MacOS folder
    of project.export's `executable`. Raises ValueError naming the file for a file that cannot be read, a document that
    is neither a GameManifest nor a Project JSON object, or one whose Name is not a non-empty string. Since the Dist
    half replaces the package directory named after it, a Name that is not one folder name (one holding a character of
    FORBIDDEN_NAME_CHARACTERS, or "." or "..", which Paths::ValidateAppName refuses and the exporter never writes) also
    raises ValueError naming the file."""
    name = paths.display_path(path)
    document = _read_json_object(path, name)
    formats = (GAME_MANIFEST_FORMAT, PROJECT_FORMAT)
    if document.get("Format") not in formats:
        raise ValueError(f"{name}: neither a game manifest nor a project file (its Format is "
                         f"{document.get('Format')!r}, not {' or '.join(repr(entry) for entry in formats)})")
    game = document.get("Name")
    if not isinstance(game, str) or not game:
        raise ValueError(f"{name}: its Name must be a non-empty string (it holds {game!r})")
    if game in RELATIVE_FOLDER_NAMES or any(character in FORBIDDEN_NAME_CHARACTERS for character in game):
        raise ValueError(f"{name}: its Name {game!r} is not one folder name, which the package directory, its "
                         f"executable and the testing user data are named after")
    return game


def committed_hashes(project_directory: Path, replays: Iterable[str]) -> dict[str, str]:
    """{replay: FinalStateHash} for each project-relative .replay path of `replays` (Test.py's expand_replay_globs of
    Testing.Replays), read from the replay file's header as committed. Raises ValueError naming a replay that cannot be
    read, is not a Replay document, or has no 16-hexadecimal-digit FinalStateHash."""
    hashes: dict[str, str] = {}
    for replay in replays:
        document = _read_json_object(project_directory / replay, replay)
        if document.get("Format") != "Replay":
            raise ValueError(f"{replay}: not a replay (its Format is {document.get('Format')!r}, not 'Replay')")
        state_hash = document.get("FinalStateHash")
        if not isinstance(state_hash, str) or not STATE_HASH_PATTERN.fullmatch(state_hash):
            raise ValueError(f"{replay}: no FinalStateHash of 16 hexadecimal digits (it holds {state_hash!r}); record "
                             f"the replay again through automation (§13.6)")
        hashes[replay] = state_hash
    return hashes


def replay_event_counts(project_directory: Path, replays: Iterable[str]) -> dict[str, int]:
    """{replay: number of input events} for each project-relative .replay path of `replays`, read from the replay file
    as committed: the replays with events are those the input check (evaluate_input_check) plays without them. Raises
    ValueError naming a replay that cannot be read, is not a Replay document or has no Events array."""
    counts: dict[str, int] = {}
    for replay in replays:
        document = _read_json_object(project_directory / replay, replay)
        if document.get("Format") != "Replay":
            raise ValueError(f"{replay}: not a replay (its Format is {document.get('Format')!r}, not 'Replay')")
        events = document.get("Events")
        if not isinstance(events, list):
            raise ValueError(f"{replay}: no Events array")
        counts[replay] = len(events)
    return counts


def evaluate_input_check(replay: str, events: int, result: dict[str, Any] | None,
                         error: tuple[str, str] | None) -> list[str]:
    """The failures of the input check of one committed replay with `events` input events (M16 friction S2-01): the
    replay verified with input.replay {verify: true, strictHash: true, withoutInput: true}, `result` being that
    call's result when it succeeded and `error` its (errorCode, detail) when it failed. The check passes only when
    that verification failed with Validation: a replay whose verification (every checkpoint, the final state hash,
    every expectation) still passes without its input proves nothing about the gameplay it recorded. That happens
    only when its input never shows in the world at a checkpoint (one every 60 ticks in a replay recorded with
    checkpoints), at its end or in an expectation: a replay without checkpoints (format 1, or upgraded from it by
    project.upgrade, which writes an empty Checkpoints list) that ends idle or after a restart, whatever its length;
    a replay shorter than 60 ticks that ends idle; input that changes only script state; or play after the last
    checkpoint that a restart undoes (M16 friction S3-01; a longer replay recorded with checkpoints that ends with a
    restart passes, its checkpoints having seen the play, while its final hash then proves only the restored state).
    Script errors in the input-free run do not matter (input.replay's success ignores them, as the recording guards
    do; §13.6). Another error fails naming it; a replay without events has nothing to check."""
    if events == 0:
        return []
    if error is None:
        tick = result.get("finalTick") if isinstance(result, dict) else None
        return [f"{replay}: its verification passes without its {events} input event(s) (input.replay {{verify: "
                f"true, strictHash: true, withoutInput: true}} reproduced every checkpoint, the final state hash at "
                f"tick {tick} and every expectation), so it proves nothing about its input: record it again so that "
                f"its input shows in the world at a checkpoint (one every 60 ticks) and it ends while the played state "
                f"is still in the scene, not after a restart"]
    code, detail = error
    if code != "Validation":
        return [f"{replay}: the input check could not run (input.replay {{verify: true, strictHash: true, "
                f"withoutInput: true}} failed with {code}: {detail})"]
    return []


def check_replay_acceptance(events: dict[str, int]) -> list[str]:
    """The failures of a game's committed replays against the demo acceptance (Roadmap M16 to M18: the suites and a
    replay recorded from tick 0 pass, and the replay's final hash is identical in the editor and the Dist export):
    `events` is replay_event_counts' {replay: input events} of the replays Testing.Replays matches. A game fails when
    Testing.Replays matches no replay, or when none of its replays has input events, since a replay without input proves
    nothing about the gameplay and the input check (evaluate_input_check) would have nothing to check. Testing.Suites'
    rule is evaluate_run's."""
    if not events:
        return ["Testing.Replays matches no replay: every game commits at least one replay recorded from tick 0 with "
                "the input that plays it (Roadmap M16 to M18; record one with test.run {record} or input.record)"]
    if not any(count > 0 for count in events.values()):
        names = ", ".join(sorted(events))
        return [f"no replay of Testing.Replays has input events ({names}): every game commits at least one replay "
                f"recorded from tick 0 with the input that plays it, which the input check then requires to matter "
                f"(Roadmap M16 to M18)"]
    return []


def _rows(document: dict[str, Any], key: str) -> list[dict[str, Any]] | None:
    """The object rows of the list `document[key]`, or None when it is not a list."""
    value = document.get(key)
    if not isinstance(value, list):
        return None
    return [row for row in value if isinstance(row, dict)]


def evaluate_run(document: dict[str, Any], mode: str, testing: TestingSettings, committed: dict[str, str]) -> list[str]:
    """The failures of one test run of a game: `document` is the TestRunResult that test.run returns (the editor halves)
    or that --feature-test writes (the exported half), `mode` the run mode it must report ("Editor" or "Dist"). It fails
    when: the document's mode is not `mode`; it did not pass or was cancelled; a case row is skipped; Testing.Suites is
    empty (every demo has at least one suite, Roadmap M16 to M18); a suite entry whose modes include `mode` has no suite
    row with its Script (a Dist run leaves Editor-only suites out); a replay of `committed` has no "replay:<path>" row,
    or that row failed; a row's finalStateHash differs from the committed hash. Coverage gates are not evaluated: games
    leave Testing.CoverageGates off (coverage.evaluate_feature_report is FeatureTest's)."""
    if not isinstance(document, dict):
        return [f"the {mode} run's result is not a TestRunResult object"]
    failures: list[str] = []
    if document.get("mode") != mode:
        failures.append(f"the run's mode is {document.get('mode')!r}, not {mode!r}")
    if document.get("passed") is not True:
        failures.append(f"the {mode} run did not pass (passed is {json.dumps(document.get('passed'))})")
    if document.get("cancelled") is not False:
        failures.append(f"the {mode} run was cancelled (cancelled is {json.dumps(document.get('cancelled'))})")
    cases = _rows(document, "cases")
    if cases is None:
        failures.append(f"the {mode} run's result has no cases array")
    else:
        failures.extend(f"skipped case ({mode}): {row.get('suite')}/{row.get('case')}: a game's run permits no skipped "
                        f"case" for row in cases if row.get("status") == "skipped")
    rows = _rows(document, "suites")
    if rows is None:
        failures.append(f"the {mode} run's result has no suites array")
        rows = []
    if not testing.suites:
        failures.append("Testing.Suites is empty: every game has at least one test suite (Roadmap M16 to M18)")
    replay_rows: dict[str, dict[str, Any]] = {}
    suite_rows: dict[str, dict[str, Any]] = {}
    for row in rows:
        name = row.get("suite")
        if isinstance(name, str) and name.startswith(REPLAY_SUITE_PREFIX):
            replay_rows[name[len(REPLAY_SUITE_PREFIX):]] = row
        elif isinstance(row.get("script"), str):
            suite_rows[row["script"]] = row
    for entry in testing.suites:
        if mode not in entry.modes:
            continue
        row = suite_rows.get(entry.script)
        if row is None:
            failures.append(f"{entry.script}: the {mode} run has no suite row for it, although its Modes include "
                            f"{mode}")
        elif row.get("passed") is not True:
            failures.append(f"{entry.script}: the suite failed in the {mode} run")
    for replay in sorted(committed):
        row = replay_rows.get(replay)
        if row is None:
            failures.append(f"{replay}: the {mode} run has no {REPLAY_SUITE_PREFIX}{replay} row, so it never played "
                            f"the committed replay")
            continue
        if row.get("passed") is not True:
            failures.append(f"{replay}: the replay failed in the {mode} run")
        state_hash = row.get("finalStateHash")
        if state_hash != committed[replay]:
            shown = state_hash if isinstance(state_hash, str) and state_hash else "(none)"
            failures.append(f"{replay}: the {mode} run ended with the state hash {shown}, not the committed "
                            f"FinalStateHash {committed[replay]}")
    for replay in sorted(set(replay_rows) - set(committed)):
        failures.append(f"{replay}: the {mode} run played it, but Testing.Replays as the games suite expands it does "
                        f"not match it")
    return failures


def check_replays_matched(project_directory: Path, matched: Iterable[str]) -> list[str]:
    """The failures of the replays below Assets/ of `project_directory` (an editor half's copy) against `matched`
    (Test.py's expand_replay_globs of Testing.Replays): one line per project-relative .replay file that `matched` does
    not hold, naming it, since an export would never run it; none when every replay is matched."""
    assets = project_directory / ASSETS_DIRECTORY
    if not assets.is_dir():
        return []
    expected = set(matched)
    found = sorted(path.relative_to(project_directory).as_posix() for path in assets.rglob(f"*{REPLAY_FILE_SUFFIX}")
                   if path.is_file())
    return [f"{replay}: a replay below Assets/ that Testing.Replays does not match, so no test run and no export plays "
            f"it (add it to Testing.Replays or delete it through automation)" for replay in found
            if replay not in expected]


def compare_game_hashes(committed: dict[str, str], runs: dict[str, dict[str, str]]) -> list[str]:
    """The determinism failures of the Dist half: `committed` is committed_hashes', `runs` maps each run of this games
    run ("Editor Debug", "Editor Release" and "Exported Dist", exports.DETERMINISM_RUNS' labels) to its {replay: hash}.
    exports.compare_determinism_runs' lines, each prefixed with its replay ("<replay>: <line>"), so a hash the Dist
    export computes differently from an editor half, or from the committed one, fails naming the replay; none when
    every run reproduces every committed hash."""
    compared = exports.compare_determinism_runs(committed, runs)
    return [f"{replay}: {line}" for replay in sorted(compared) for line in compared[replay]]


def check_script_check(result: dict[str, Any]) -> list[str]:
    """The failures of a script.check result: one line per diagnostic ("<file>:<line>: <code>: <message>"); none when
    its diagnostics are empty."""
    diagnostics = result.get("diagnostics")
    if not isinstance(diagnostics, list):
        return ["script.check returned no diagnostics array"]
    failures = [f"{item.get('file', '')}:{item.get('line', 0)}: {item.get('code', '')}: {item.get('message', '')}"
                if isinstance(item, dict) else f"script.check: a diagnostic that is not an object: {item!r}"
                for item in diagnostics]
    if not failures and result.get("passed") is False:
        failures.append("script.check did not pass, without a diagnostic")
    return failures


def check_validate(result: dict[str, Any]) -> list[str]:
    """The failures of a project.validate result: one line per diagnostic, warnings included ("<severity> <code>:
    <message> (<file or asset>)"), because a demo's project.validate must be clean (Roadmap M16 to M18)."""
    diagnostics = result.get("diagnostics")
    if not isinstance(diagnostics, list):
        return ["project.validate returned no diagnostics array"]
    failures: list[str] = []
    for item in diagnostics:
        if not isinstance(item, dict):
            failures.append(f"project.validate: a diagnostic that is not an object: {item!r}")
            continue
        subject = item.get("file") or item.get("asset") or "the project"
        failures.append(f"{item.get('severity', '')} {item.get('code', '')}: {item.get('message', '')} ({subject})")
    return failures


def check_upgrade_dry_run(result: dict[str, Any], project: GameProject) -> list[str]:
    """The failures of a project.upgrade {dryRun: true} result (the canonical re-save check, §15.8): one line per
    changedFiles entry, each naming UPGRADE_COMMAND for the project's repository-relative directory, since an engine
    change that alters canonical output must upgrade the demo in the same change (§13.12)."""
    changed = result.get("changedFiles")
    if not isinstance(changed, list):
        return ["project.upgrade {dryRun: true} returned no changedFiles array"]
    command = UPGRADE_COMMAND.format(project=paths.display_path(project.directory))
    return [f"{file}: not in its canonical form, so project.upgrade would rewrite it: run {command} (or "
            f"project.upgrade through automation) and commit the result in the same change (§13.12)"
            for file in changed]


def check_smoke(result: dict[str, Any], require_gpu: bool) -> tuple[list[str], list[str]]:
    """(failures, warnings) of a plain Dist project.export {smokeTest: true} result: a smokeTestExitCode other than 0 is
    a failure; smokeTestRan false (no device, Exporter.h step 6) is a failure with `require_gpu`, else a warning that
    names the reason the export reported."""
    if result.get("smokeTestRan") is True:
        exit_code = result.get("smokeTestExitCode")
        if exit_code != 0:
            return [f"the plain Dist export's smoke test exited with code {exit_code}"], []
        return [], []
    warnings = [str(warning) for warning in result.get("warnings", [])] if isinstance(result.get("warnings"), list) \
        else []
    reason = "; ".join(warnings) or "the export reported no reason"
    if require_gpu:
        return [f"the plain Dist export's smoke test did not run ({reason}) and --require-gpu is given"], []
    return [], [f"the plain Dist export's smoke test did not run ({reason}); --require-gpu fails instead"]


def editor_report_path(project: GameProject, config: str) -> Path:
    """bin/TestResults/Games-<Name>-Editor-<config>.json: the test.run document of the project's editor half in
    `config`, written with --junit."""
    return paths.TEST_RESULTS_ROOT / f"{REPORT_PREFIX}-{project.name}-Editor-{config}.json"


def dist_report_path(project: GameProject, suffix: str) -> Path:
    """bin/TestResults/Games-<Name>-Dist<suffix> (suffix ".json" or ".xml"): the exported --feature-test run's report,
    copied there from the run's temporary working directory, so the export stage's FeatureTest-Dist reports are never
    overwritten."""
    return paths.TEST_RESULTS_ROOT / f"{REPORT_PREFIX}-{project.name}-{EXPORTED_CONFIGURATION}{suffix}"


def junit_path(config: str) -> Path:
    """bin/TestResults/Games-<config>.xml: the JUnit file of one requested configuration's steps (the audit steps go
    into the first requested configuration's file, in Debug, Release, Dist order)."""
    return paths.TEST_RESULTS_ROOT / f"{REPORT_PREFIX}-{config}.xml"
