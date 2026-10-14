"""Testing exports for the export and determinism suites (Docs/Architecture.md §14, §15.1 T3 and T3d, §15.8;
Docs/Decisions/0024-m15-contract.md decisions 9 and 10). Scripts/Test.py's export suite writes the packages and reports
these functions read, and its determinism suite compares the exported runs' hashes with the editor runs' and the
committed ones.

The export suite leaves, per configuration <Config> of Release and Dist:
  - the testing export of Projects/FeatureTest in bin/Exports/<Config>/FeatureTest/ (EXPORTS_ROOT; on macOS the
    directory holds FeatureTest.app);
  - with --junit, the Runtime's reports bin/TestResults/FeatureTest-<Config>.json and .xml, written by
    `FeatureTest --headless --feature-test` run in the repository root.
A report's "suites" rows named "replay:<path>" carry each replay's "finalStateHash" (16 lowercase hex digits), the
hash the run observed whether or not the row passed.

The determinism suite reads the exported runs' hashes from those reports instead of rerunning them (ADR 0023), so a
report counts only when it is current: newer than every input of its run (stale_report_reason over report_inputs), which
leftover packages and reports of an earlier tree are not.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from Lib import paths

EXPORTS_ROOT = paths.BIN_ROOT / "Exports"
FEATURE_TEST_NAME = "FeatureTest"
# The four runs of the determinism stage, in reporting order (§15.1 T3d, §15.8).
DETERMINISM_RUNS = ("Editor Debug", "Editor Release", "Exported Release", "Exported Dist")
REPLAY_SUITE_PREFIX = "replay:"
# FeatureTest's suite whose case calls Debug.Break once (Assets/Tests/Api/Debug.test.luau), and the count its report row
# must show in every exported run.
DEBUG_BREAK_SUITE = "Api.Debug"
DEBUG_BREAK_COUNT = 1

# The project whose testing export the suites run, and its import cache, which no report depends on.
FEATURE_PROJECT = paths.REPOSITORY_ROOT / "Projects" / FEATURE_TEST_NAME
PROJECT_CACHE_DIRECTORY = "Library"
# The configurations of each report kind: the exported runs (Runtime) and the editor runs (the feature suite).
EXPORTED_CONFIGURATIONS = ("Release", "Dist")
EDITOR_CONFIGURATIONS = ("Debug", "Release")
# The editor whose project.export the export suite drives (decision 9).
EXPORTING_EDITOR_CONFIGURATION = "Release"


def package_directory(config: str, name: str = FEATURE_TEST_NAME) -> Path:
    """bin/Exports/<config>/<name>: where the export suite exports `name` in `config` (Release or Dist)."""
    return EXPORTS_ROOT / config / name


def package_executable(package: Path, name: str = FEATURE_TEST_NAME) -> Path:
    """The exported executable of the package directory `package`: <package>/<name>.exe on Windows, <package>/<name> on
    Linux, <package>/<name>.app/Contents/MacOS/<name> on macOS."""
    if paths.host().is_macos:
        return package / f"{name}.app" / "Contents" / "MacOS" / name
    return package / paths.executable_name(name)


def testing_user_data(package: Path, name: str = FEATURE_TEST_NAME) -> Path:
    """Where a --feature-test run of the package writes its user data without --user-data-dir (§14.3,
    GetTestingUserDataRoot in Engine/Project/GameManifest.h): <package>/bin/TestUserData/<name>, on macOS too (the
    directory that holds the .app)."""
    return package / "bin" / "TestUserData" / name


def read_replay_hashes(report: Path) -> dict[str, str]:
    """{replay path: finalStateHash} of every "replay:<path>" suite row of the FeatureTest report `report` whose
    finalStateHash is not empty, passed or failed, so a drifted hash is reported as the hash it is (whether the row
    passed is the export suite's check); a row without a hash, or no row, is absent. Raises ValueError naming the file
    for a report that is not a FeatureTest TestRunResult."""
    try:
        document = json.loads(report.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"{report}: cannot read the report: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{report}: not UTF-8 JSON: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("mode"), str) \
            or not isinstance(document.get("suites"), list):
        raise ValueError(f"{report}: not a FeatureTest TestRunResult (an object with a 'mode' and a 'suites' list)")
    hashes: dict[str, str] = {}
    for index, row in enumerate(document["suites"]):
        if not isinstance(row, dict) or not isinstance(row.get("suite"), str):
            raise ValueError(f"{report}: suites[{index}] is not a suite row with a 'suite' name")
        name = row["suite"]
        if not name.startswith(REPLAY_SUITE_PREFIX):
            continue
        state_hash = row.get("finalStateHash", "")
        if not isinstance(state_hash, str):
            raise ValueError(f"{report}: suites[{index}].finalStateHash must be a string")
        if state_hash:
            hashes[name[len(REPLAY_SUITE_PREFIX):]] = state_hash
    return hashes


def debug_break_failures(report: Path) -> list[str]:
    """The failures of the Dist-resident "Debug.Break counted no-op" (Roadmap M15, ADR 0024 decision 6) in the
    FeatureTest report `report`: its DEBUG_BREAK_SUITE row must exist and count exactly DEBUG_BREAK_COUNT breaks, the
    one Debug.Break its case calls; the case itself only shows that time keeps running. Raises ValueError naming the
    file for a report that is not a FeatureTest TestRunResult."""
    try:
        document = json.loads(report.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"{report}: cannot read the report: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{report}: not UTF-8 JSON: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("suites"), list):
        raise ValueError(f"{report}: not a FeatureTest TestRunResult (an object with a 'suites' list)")
    rows = [row for row in document["suites"] if isinstance(row, dict) and row.get("suite") == DEBUG_BREAK_SUITE]
    if len(rows) != 1:
        return [f"the report has {len(rows)} {DEBUG_BREAK_SUITE} suite rows, not one, so Debug.Break's count is "
                f"unchecked"]
    breaks = rows[0].get("breaks")
    if isinstance(breaks, bool) or breaks != DEBUG_BREAK_COUNT:
        return [f"the {DEBUG_BREAK_SUITE} suite counted {breaks!r} breaks, not {DEBUG_BREAK_COUNT}: Debug.Break must "
                f"stay a counted no-op in test runs"]
    return []


def _project_files(project: Path) -> list[Path]:
    """Every file below `project` outside its top-level Library/ directory, sorted."""
    files: list[Path] = []
    for directory, directories, names in os.walk(project):
        current = Path(directory)
        if current == project:
            directories[:] = [name for name in directories if name != PROJECT_CACHE_DIRECTORY]
        directories.sort()
        files.extend(current / name for name in sorted(names))
    return files


def report_inputs(config: str, kind: str) -> list[Path]:
    """The files a report depends on, for stale_report_reason: for kind "exported" (the export suite's
    bin/TestResults/FeatureTest-<config>.json, config Release or Dist) the <config> Runtime executable
    (bin/<config>-<system>-<arch>/Runtime/Runtime(.exe)), the Release editor that exported
    (bin/Release-<system>-<arch>/Editor/Editor(.exe)) and every file of Projects/FeatureTest outside Library/; for kind
    "editor" (the feature suite's bin/TestResults/Feature-<config>.json, which the export suite merges) the <config>
    editor and the same project files. Raises ValueError for another kind or configuration."""
    if kind == "exported":
        if config not in EXPORTED_CONFIGURATIONS:
            raise ValueError(f"an exported report has no configuration '{config}' (expected "
                             f"{', '.join(EXPORTED_CONFIGURATIONS)})")
        binaries = [paths.output_directory(config) / "Runtime" / paths.executable_name("Runtime"),
                    paths.output_directory(EXPORTING_EDITOR_CONFIGURATION) / "Editor" / paths.executable_name("Editor")]
    elif kind == "editor":
        if config not in EDITOR_CONFIGURATIONS:
            raise ValueError(f"an editor report has no configuration '{config}' (expected "
                             f"{', '.join(EDITOR_CONFIGURATIONS)})")
        binaries = [paths.output_directory(config) / "Editor" / paths.executable_name("Editor")]
    else:
        raise ValueError(f"unknown report kind '{kind}' (expected exported or editor)")
    return binaries + _project_files(FEATURE_PROJECT)


def stale_report_reason(report: Path, inputs: list[Path]) -> str | None:
    """None when `report` exists and its modification time is later than that of every existing path of `inputs`;
    otherwise why it does not count: "<report> is missing" or "<report> is older than <the newest input>"."""
    try:
        report_time = report.stat().st_mtime_ns
    except OSError:
        return f"{paths.display_path(report)} is missing"
    newest: Path | None = None
    newest_time = -1
    for path in inputs:
        try:
            modified = path.stat().st_mtime_ns
        except OSError:
            continue  # an input that does not exist (a project file removed since) cannot be newer
        if modified > newest_time:
            newest, newest_time = path, modified
    if newest is not None and newest_time >= report_time:
        return f"{paths.display_path(report)} is older than {paths.display_path(newest)}"
    return None


def compare_determinism_runs(committed: dict[str, str], runs: dict[str, dict[str, str]]) -> dict[str, list[str]]:
    """The determinism failures per replay of `committed` ({replay path: committed FinalStateHash}) against `runs`
    ({run label of DETERMINISM_RUNS: {replay path: hash}}): for each replay, "<run>: no hash" for a run without it,
    "<run>: <hash> differs from the committed <hash>", and "runs differ: <run> <hash>, ..." when two runs disagree, in
    DETERMINISM_RUNS order; a replay without failures maps to []."""
    ordered = ([run for run in DETERMINISM_RUNS if run in runs]
               + sorted(run for run in runs if run not in DETERMINISM_RUNS))
    failures: dict[str, list[str]] = {}
    for replay in sorted(committed):
        expected = committed[replay]
        lines: list[str] = []
        observed: list[tuple[str, str]] = []
        for run in ordered:
            state_hash = runs[run].get(replay, "")
            if not state_hash:
                lines.append(f"{run}: no hash")
                continue
            observed.append((run, state_hash))
            if state_hash != expected:
                lines.append(f"{run}: {state_hash} differs from the committed {expected or '(none)'}")
        if len({state_hash for _, state_hash in observed}) > 1:
            lines.append("runs differ: " + ", ".join(f"{run} {state_hash}" for run, state_hash in observed))
        failures[replay] = lines
    return failures
