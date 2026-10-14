"""Testing exports for the export and determinism suites (Docs/Architecture.md §14, §15.1 T3 and T3d, §15.8;
Docs/Decisions/0024-m15-contract.md decisions 9 and 10). Stream B implements this module; Scripts/Test.py's export suite
writes the packages and reports these functions read, and its determinism suite compares the exported runs' hashes with
the editor runs' and the committed ones.

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

from pathlib import Path

from Lib import paths

EXPORTS_ROOT = paths.BIN_ROOT / "Exports"
FEATURE_TEST_NAME = "FeatureTest"
# The four runs of the determinism stage, in reporting order (§15.1 T3d, §15.8).
DETERMINISM_RUNS = ("Editor Debug", "Editor Release", "Exported Release", "Exported Dist")
REPLAY_SUITE_PREFIX = "replay:"


def package_directory(config: str, name: str = FEATURE_TEST_NAME) -> Path:
    """bin/Exports/<config>/<name>: where the export suite exports `name` in `config` (Release or Dist)."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decision 9, stream B)")


def package_executable(package: Path, name: str = FEATURE_TEST_NAME) -> Path:
    """The exported executable of the package directory `package`: <package>/<name>.exe on Windows, <package>/<name> on
    Linux, <package>/<name>.app/Contents/MacOS/<name> on macOS."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decision 9, stream B)")


def testing_user_data(package: Path, name: str = FEATURE_TEST_NAME) -> Path:
    """Where a --feature-test run of the package writes its user data without --user-data-dir (§14.3,
    GetTestingUserDataRoot in Engine/Project/GameManifest.h): <package>/bin/TestUserData/<name>, on macOS too (the
    directory that holds the .app)."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decision 9, stream B)")


def read_replay_hashes(report: Path) -> dict[str, str]:
    """{replay path: finalStateHash} of every "replay:<path>" suite row of the FeatureTest report `report` whose
    finalStateHash is not empty, passed or failed, so a drifted hash is reported as the hash it is (whether the row
    passed is the export suite's check); a row without a hash, or no row, is absent. Raises ValueError naming the file for
    a report that is not a FeatureTest TestRunResult."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decision 10, stream B)")


def report_inputs(config: str, kind: str) -> list[Path]:
    """The files a report depends on, for stale_report_reason: for kind "exported" (the export suite's
    bin/TestResults/FeatureTest-<config>.json, config Release or Dist) the <config> Runtime executable
    (bin/<config>-<system>-<arch>/Runtime/Runtime(.exe)), the Release editor that exported
    (bin/Release-<system>-<arch>/Editor/Editor(.exe)) and every file of Projects/FeatureTest outside Library/; for kind
    "editor" (the feature suite's bin/TestResults/Feature-<config>.json, which the export suite merges) the <config>
    editor and the same project files. Raises ValueError for another kind or configuration."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decisions 9 and 10, stream B)")


def stale_report_reason(report: Path, inputs: list[Path]) -> str | None:
    """None when `report` exists and its modification time is later than that of every existing path of `inputs`;
    otherwise why it does not count: "<report> is missing" or "<report> is older than <the newest input>"."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decisions 9 and 10, stream B)")


def compare_determinism_runs(committed: dict[str, str], runs: dict[str, dict[str, str]]) -> dict[str, list[str]]:
    """The determinism failures per replay of `committed` ({replay path: committed FinalStateHash}) against `runs`
    ({run label of DETERMINISM_RUNS: {replay path: hash}}): for each replay, "<run>: no hash" for a run without it,
    "<run>: <hash> differs from the committed <hash>", and "runs differ: <run> <hash>, ..." when two runs disagree, in
    DETERMINISM_RUNS order; a replay without failures maps to []."""
    raise NotImplementedError("contract stub: exports (ADR 0024 decision 10, stream B)")
