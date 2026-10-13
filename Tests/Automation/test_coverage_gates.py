"""The Python side of the coverage gates (Docs/Architecture.md §15.6; Docs/Decisions/0022-m14-contract.md decisions 6
and 7): the feature suite's evaluation of an editor report, the cross-mode union merge and gate 5, each proven against
the seeded-gap fixtures in Tests/Data/CoverageGaps, and the parts of Scripts/Test.py's feature and determinism suites
that need no FeatureTest content: the project snapshot, the replay globs and the hash comparison, plus the determinism
verification against a replay a test records. Only the last test starts an editor.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, editor_executable

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Test as test_script  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import coverage  # noqa: E402

GAPS = REPOSITORY_ROOT / "Tests" / "Data" / "CoverageGaps"


def write_report(path: Path, document: dict) -> Path:
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


class FeatureReportTests(unittest.TestCase):
    def test_feature_suite_names_each_uncovered_entry_of_a_seeded_gap(self) -> None:
        for gate in coverage.PER_MODE_GATES:
            with self.subTest(gate=gate):
                directory = GAPS / f"Gate{gate}" / "Editor"
                expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
                report = coverage.read_feature_report(directory / "FeatureTest-Editor.json")
                failures = coverage.evaluate_feature_report(report)
                self.assertEqual(
                    sorted(line for line in failures if line.startswith("UNCOVERED")),
                    [f"UNCOVERED gate {gate} (Editor): {entry}" for entry in expected["Missing"]],
                )

    def test_every_per_mode_fixture_fails_exactly_its_gate_in_python_too(self) -> None:
        # The C++ acceptance test re-evaluates the counters; the feature suite reads the recorded gates. Both must
        # agree.
        fixtures = 0
        for gate in coverage.PER_MODE_GATES:
            for mode in coverage.MODES:
                with self.subTest(gate=gate, mode=mode):
                    directory = GAPS / f"Gate{gate}" / mode
                    expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
                    self.assertEqual((expected["Format"], expected["Version"]), ("CoverageGap", 1))
                    self.assertEqual((expected["Gate"], expected["Mode"]), (gate, mode))
                    report = coverage.read_feature_report(directory / f"FeatureTest-{mode}.json")
                    self.assertEqual(report.mode, mode)
                    failed = [result for result in report.gates if not result.passed]
                    self.assertEqual([result.gate for result in failed], [gate])
                    self.assertEqual(list(failed[0].missing), expected["Missing"])
                    # A Release or Dist report never lists an EditorOnly entry.
                    if mode != "Editor":
                        self.assertTrue(all(mode in counter.modes for counter in report.counters))
                    fixtures += 1
        self.assertEqual(fixtures, len(coverage.PER_MODE_GATES) * len(coverage.MODES))

    def test_unevaluated_gates_and_skipped_cases_fail_the_feature_suite(self) -> None:
        report = coverage.read_feature_report(GAPS / "Merge" / "Unevaluated" / "FeatureTest-Editor.json")
        failures = coverage.evaluate_feature_report(report)
        self.assertTrue(any("not evaluated" in failure for failure in failures), failures)
        report = coverage.read_feature_report(GAPS / "Merge" / "SkippedCase" / "FeatureTest-Editor.json")
        self.assertTrue(any("skipped" in failure for failure in coverage.evaluate_feature_report(report)))
        covered = coverage.read_feature_report(GAPS / "Merge" / "Covered" / "FeatureTest-Editor.json")
        self.assertEqual(coverage.evaluate_feature_report(covered), [])

    def test_a_report_without_gates_or_with_malformed_members_is_named(self) -> None:
        source = json.loads((GAPS / "Merge" / "Covered" / "FeatureTest-Editor.json").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="CoverageReports-") as directory:
            plain = json.loads(json.dumps(source))
            plain["coverage"]["gates"] = []
            report = coverage.read_feature_report(write_report(Path(directory) / "Plain.json", plain))
            self.assertEqual(sum("absent" in line for line in coverage.evaluate_feature_report(report)), 6)

            def broken(change: Callable[[dict], object]) -> dict:
                document = json.loads(json.dumps(source))
                change(document)
                return document

            cases = {
                "mode": broken(lambda document: document.update(mode="Shipping")),
                "unsorted": broken(lambda document: document["coverage"]["counters"].reverse()),
                "duplicate": broken(lambda document: document["coverage"]["counters"].append(
                    document["coverage"]["counters"][-1])),
                "subset": broken(lambda document: document["coverage"]["gates"].pop()),
                "count": broken(lambda document: document["coverage"]["counters"][0].update(count=True)),
                "passed": broken(lambda document: document.pop("passed")),
            }
            for name, document in cases.items():
                with self.subTest(case=name):
                    path = write_report(Path(directory) / f"{name}.json", document)
                    with self.assertRaises(ValueError) as raised:
                        coverage.read_feature_report(path)
                    self.assertIn(str(path), str(raised.exception))
            garbage = Path(directory) / "Garbage.json"
            garbage.write_bytes(b"\xff{")
            with self.assertRaises(ValueError):
                coverage.read_feature_report(garbage)


class MergeTests(unittest.TestCase):
    def test_merge_requires_union_over_modes(self) -> None:
        cases = sorted(path for path in (GAPS / "Merge").iterdir() if (path / "Expected.json").is_file())
        self.assertEqual([case.name for case in cases], ["Covered", "Extra", "Mismatch", "Missing", "Passed",
                                                         "SkippedCase", "Uncovered", "Unevaluated"])
        for case in cases:
            with self.subTest(case=case.name):
                expected = json.loads((case / "Expected.json").read_text(encoding="utf-8"))
                reports = [coverage.read_feature_report(case / f"FeatureTest-{mode}.json") for mode in coverage.MODES]
                self.assertEqual(coverage.merge_coverage(reports), expected["Failures"])
                # Every expected line names a frozen rule, in their reporting order.
                rules = [line.split(": ", 2)[1] for line in expected["Failures"]]
                self.assertEqual(rules, sorted(rules, key=coverage.MERGE_RULES.index))

    def test_merge_coverage_command_prints_each_failure_and_sets_the_exit_code(self) -> None:
        for case in ("Covered", "Uncovered"):
            with self.subTest(case=case):
                directory = GAPS / "Merge" / case
                expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
                reports = [str(directory / f"FeatureTest-{mode}.json") for mode in coverage.MODES]
                process = subprocess.run(
                    [sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Test.py"), "--merge-coverage", *reports],
                    capture_output=True, text=True, timeout=60.0, check=False)
                lines = [line for line in process.stdout.splitlines() if line.startswith("merge: ")]
                self.assertEqual(lines, expected["Failures"], process.stdout + process.stderr)
                self.assertEqual(process.returncode, 1 if expected["Failures"] else 0, process.stdout + process.stderr)

    def test_merge_coverage_command_reports_a_missing_or_unreadable_report_as_its_mode(self) -> None:
        # Review finding M02 (decision 6): a report that does not exist or cannot be read fails the report rule for the
        # mode of its position with exit 1, the CLI's only failure exit; exit 2 stays argparse's usage error.
        case = GAPS / "Merge" / "Covered"
        with tempfile.TemporaryDirectory(prefix="MergeMissing-") as directory:
            broken = Path(directory) / "FeatureTest-Release.json"
            broken.write_text("{\"mode\": ", encoding="utf-8")
            for reports, expected in (
                    ([case / "FeatureTest-Editor.json", case / "FeatureTest-Release.json",
                      Path(directory) / "Missing.json"],
                     ["merge: report: Dist"]),
                    ([case / "FeatureTest-Editor.json", broken, case / "FeatureTest-Dist.json"],
                     ["merge: report: Release"])):
                with self.subTest(expected=expected):
                    process = subprocess.run(
                        [sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Test.py"), "--merge-coverage",
                         *[str(report) for report in reports], "--json"],
                        capture_output=True, text=True, timeout=60.0, check=False)
                    self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
                    self.assertEqual(json.loads(process.stdout)["failures"], expected, process.stdout + process.stderr)

    def test_merge_rejects_a_missing_or_duplicated_mode(self) -> None:
        case = GAPS / "Merge" / "Covered"
        editor = coverage.read_feature_report(case / "FeatureTest-Editor.json")
        release = coverage.read_feature_report(case / "FeatureTest-Release.json")
        dist = coverage.read_feature_report(case / "FeatureTest-Dist.json")
        self.assertTrue(coverage.merge_coverage([editor, release]))
        self.assertTrue(coverage.merge_coverage([editor, release, release]))
        self.assertEqual(coverage.merge_coverage([editor, release]), ["merge: report: Dist"])
        self.assertEqual(coverage.merge_coverage([editor, release, release]), ["merge: report: Release",
                                                                               "merge: report: Dist"])
        # A report in another mode's place names another mode.
        self.assertEqual(coverage.merge_coverage([release, editor, dist]), ["merge: report: Editor",
                                                                            "merge: report: Release"])
        self.assertEqual(coverage.merge_coverage([editor, release, dist]), [])


class MethodGateTests(unittest.TestCase):
    def test_method_gate_names_uncovered_methods(self) -> None:
        directory = GAPS / "Gate5"
        catalog = json.loads((directory / "Methods.json").read_text(encoding="utf-8"))
        registered = [method["name"] for method in catalog["Methods"]]
        called = coverage.read_called_methods(directory / "CalledMethods.txt")
        expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
        self.assertEqual(coverage.uncovered_methods(registered, called), expected["Missing"])
        self.assertEqual(coverage.read_called_methods(directory / "Absent.txt"), set())


class FeatureSuiteTests(unittest.TestCase):
    def test_the_project_snapshot_names_every_change_outside_library(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FeatureSnapshot-") as directory:
            root = Path(directory)
            (root / "Assets").mkdir()
            (root / "Library").mkdir()
            (root / "Assets" / "Kept.scene").write_text("kept", encoding="utf-8")
            (root / "Assets" / "Changed.meta").write_text("before", encoding="utf-8")
            (root / "Assets" / "Removed.luau").write_text("gone", encoding="utf-8")
            (root / "Library" / "Cache.bin").write_text("cache", encoding="utf-8")
            before = test_script.snapshot_project(root)
            self.assertNotIn("Library/Cache.bin", before)
            (root / "Assets" / "Changed.meta").write_text("after", encoding="utf-8")
            (root / "Assets" / "Removed.luau").unlink()
            (root / "Assets" / "Added.png").write_bytes(b"\x89PNG")
            (root / "Library" / "Cache.bin").write_text("rewritten", encoding="utf-8")
            self.assertEqual(test_script.compare_snapshots(before, test_script.snapshot_project(root)), [
                "added: Assets/Added.png", "changed: Assets/Changed.meta", "removed: Assets/Removed.luau"])

    def test_replay_globs_follow_the_editor_rules(self) -> None:
        with tempfile.TemporaryDirectory(prefix="FeatureReplays-") as directory:
            root = Path(directory)
            for path in ("Assets/Tests/Replays/Input.replay", "Assets/Tests/Replays/Deep/Determinism.replay",
                         "Assets/Other.replay", "Assets/Tests/Replays/Notes.txt"):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text("{}", encoding="utf-8")
            expand = test_script.expand_replay_globs
            self.assertEqual(expand(root, ["Assets/Tests/Replays/*.replay"]), ["Assets/Tests/Replays/Input.replay"])
            self.assertEqual(expand(root, ["Assets/**/*.replay"]), [
                "Assets/Other.replay", "Assets/Tests/Replays/Deep/Determinism.replay",
                "Assets/Tests/Replays/Input.replay"])
            self.assertEqual(expand(root, ["project://Assets/Other.replay", "Assets/Other.replay"]),
                             ["Assets/Other.replay"])
            self.assertEqual(expand(root, ["Assets/Tests/?nput.replay"]), [])
            with self.assertRaises(ValueError):
                expand(root, ["Assets/Missing.replay"])

    def test_replay_hashes_must_match_the_committed_hash_and_agree_across_configurations(self) -> None:
        outcome = test_script.ReplayOutcome
        committed = {"Assets/A.replay": "00000000000000aa", "Assets/B.replay": "00000000000000bb"}
        agreed = {"Debug": [outcome("Assets/A.replay", "00000000000000aa"),
                            outcome("Assets/B.replay", "00000000000000bb")],
                  "Release": [outcome("Assets/A.replay", "00000000000000aa"),
                              outcome("Assets/B.replay", "00000000000000bb")]}
        self.assertEqual(test_script.compare_replay_hashes(committed, agreed), {"Assets/A.replay": [],
                                                                               "Assets/B.replay": []})
        diverged = {"Debug": [outcome("Assets/A.replay", "00000000000000aa"),
                              outcome("Assets/B.replay", "00000000000000bb")],
                    "Release": [outcome("Assets/A.replay", "00000000000000ff"),
                                outcome("Assets/B.replay", "", ["input.replay failed: replay failed: quit"])]}
        problems = test_script.compare_replay_hashes(committed, diverged)
        self.assertEqual(len(problems["Assets/A.replay"]), 2)
        self.assertIn("Release: state hash 00000000000000ff differs", problems["Assets/A.replay"][0])
        self.assertIn("the configurations disagree", problems["Assets/A.replay"][1])
        self.assertEqual(problems["Assets/B.replay"], ["Release: input.replay failed: replay failed: quit"])


class DeterminismVerificationTests(AutomationTestCase):
    def test_determinism_verification_reports_the_hash_of_a_recorded_replay(self) -> None:
        # A replay recorded in this editor verifies with its own hash in a --renderer none editor on a copy of the
        # project without Library/, as the determinism suite runs it; a tampered committed hash is reported.
        client, root = self.open_editor_with_scene("Determinism")
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 3})
        saved = client.call("input.record", {"action": "stop", "path": "Assets/Tests/Replays/Steps.replay"})
        client.call("session.shutdown", {"force": True})
        self.editors[-1].wait()
        work = self.directory / "Work"
        project_file = test_script.copy_project(root, work / "Copy")
        self.assertFalse((work / "Copy" / "Library").exists())
        replays = test_script.expand_replay_globs(work / "Copy", ["Assets/Tests/Replays/*.replay"])
        self.assertEqual(replays, ["Assets/Tests/Replays/Steps.replay"])
        outcomes = test_script.verify_replays(editor_executable(), project_file, replays, self.directory / "VerifyData",
                                              120.0)
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(outcomes[0].problems, [])
        self.assertEqual(outcomes[0].state_hash, saved["stateHash"])
        committed = json.loads((root / replays[0]).read_text(encoding="utf-8"))["FinalStateHash"]
        self.assertEqual(test_script.compare_replay_hashes({replays[0]: committed}, {"Editor": outcomes}),
                         {replays[0]: []})
        tampered = "0000000000000001" if committed != "0000000000000001" else "0000000000000002"
        problems = test_script.compare_replay_hashes({replays[0]: tampered}, {"Editor": outcomes})
        self.assertEqual(len(problems[replays[0]]), 1)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
