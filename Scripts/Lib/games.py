"""The demo game projects and the evaluation helpers of the games suite (Docs/Architecture.md §15.1 T5, §15.8, §13.12;
Roadmap M16 to M18; Docs/Decisions/0026-m16-contract.md decision 7, stream B), shared by Scripts/Test.py's games suite
and Scripts/AuditProvenance.py --demos.

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
"""

from __future__ import annotations

import dataclasses
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from Lib import exports

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


@dataclasses.dataclass(frozen=True)
class GameProject:
    """One discovered game project."""

    directory: Path  # absolute
    project_file: Path  # directory / "<name>.eproj"
    name: str  # the .eproj's stem: the exported executable's name and the testing user-data folder's


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
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def read_testing(project_file: Path) -> TestingSettings:
    """The Testing settings of the .eproj `project_file`: each Testing.Suites entry's Script, Scene and Modes (absent
    Modes are DEFAULT_SUITE_MODES) and Testing.Replays, absent members read as empty. Raises ValueError naming the file
    for a document that is not a .eproj JSON object, or a member of the wrong type."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def committed_hashes(project_directory: Path, replays: Iterable[str]) -> dict[str, str]:
    """{replay: FinalStateHash} for each project-relative .replay path of `replays` (Test.py's expand_replay_globs of
    Testing.Replays), read from the replay file's header as committed. Raises ValueError naming a replay that cannot be
    read, is not a Replay document, or has no 16-hexadecimal-digit FinalStateHash."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def evaluate_run(document: dict[str, Any], mode: str, testing: TestingSettings, committed: dict[str, str]) -> list[str]:
    """The failures of one test run of a game: `document` is the TestRunResult that test.run returns (the editor halves)
    or that --feature-test writes (the exported half), `mode` the run mode it must report ("Editor" or "Dist"). It fails
    when: the document's mode is not `mode`; it did not pass or was cancelled; a case row is skipped; Testing.Suites is
    empty (every demo has at least one suite, Roadmap M16 to M18); a suite entry whose modes include `mode` has no suite
    row with its Script (a Dist run leaves Editor-only suites out); a replay of `committed` has no "replay:<path>" row,
    or that row failed; a row's finalStateHash differs from the committed hash. Coverage gates are not evaluated: games
    leave Testing.CoverageGates off (coverage.evaluate_feature_report is FeatureTest's)."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def check_replays_matched(project_directory: Path, matched: Iterable[str]) -> list[str]:
    """The failures of the replays below Assets/ of `project_directory` (an editor half's copy) against `matched`
    (Test.py's expand_replay_globs of Testing.Replays): one line per project-relative .replay file that `matched` does
    not hold, naming it, since an export would never run it; none when every replay is matched."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def compare_game_hashes(committed: dict[str, str], runs: dict[str, dict[str, str]]) -> list[str]:
    """The determinism failures of the Dist half: `committed` is committed_hashes', `runs` maps each run of this games
    run ("Editor Debug", "Editor Release" and "Exported Dist", exports.DETERMINISM_RUNS' labels) to its {replay: hash}.
    exports.compare_determinism_runs' lines, each prefixed with its replay ("<replay>: <line>"), so a hash the Dist
    export computes differently from an editor half, or from the committed one, fails naming the replay; none when
    every run reproduces every committed hash."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def check_script_check(result: dict[str, Any]) -> list[str]:
    """The failures of a script.check result: one line per diagnostic ("<file>:<line>: <code>: <message>"); none when
    its diagnostics are empty."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def check_validate(result: dict[str, Any]) -> list[str]:
    """The failures of a project.validate result: one line per diagnostic, warnings included ("<severity> <code>:
    <message> (<file or asset>)"), because a demo's project.validate must be clean (Roadmap M16 to M18)."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def check_upgrade_dry_run(result: dict[str, Any], project: GameProject) -> list[str]:
    """The failures of a project.upgrade {dryRun: true} result (the canonical re-save check, §15.8): one line per
    changedFiles entry, each naming UPGRADE_COMMAND for the project's repository-relative directory, since an engine
    change that alters canonical output must upgrade the demo in the same change (§13.12)."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def check_smoke(result: dict[str, Any], require_gpu: bool) -> tuple[list[str], list[str]]:
    """(failures, warnings) of a plain Dist project.export {smokeTest: true} result: a smokeTestExitCode other than 0 is
    a failure; smokeTestRan false (no device, Exporter.h step 6) is a failure with `require_gpu`, else a warning that
    names the reason the export reported."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def editor_report_path(project: GameProject, config: str) -> Path:
    """bin/TestResults/Games-<Name>-Editor-<config>.json: the test.run document of the project's editor half in
    `config`, written with --junit."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def dist_report_path(project: GameProject, suffix: str) -> Path:
    """bin/TestResults/Games-<Name>-Dist<suffix> (suffix ".json" or ".xml"): the exported --feature-test run's report,
    copied there from the run's temporary working directory, so the export stage's FeatureTest-Dist reports are never
    overwritten."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")


def junit_path(config: str) -> Path:
    """bin/TestResults/Games-<config>.xml: the JUnit file of one requested configuration's steps (the audit steps go
    into the first requested configuration's file, in Debug, Release, Dist order)."""
    raise NotImplementedError("contract stub: games (ADR 0026 decision 7, stream B)")
