"""The Python side of the coverage gates (Docs/Architecture.md §15.6; Docs/Decisions/0022-m14-contract.md decisions 6 and
7): the feature suite's evaluation of an editor report, the cross-mode union merge and gate 5, each proven against the
seeded-gap fixtures in Tests/Data/CoverageGaps. These tests need no editor build; they never call an automation method.
"""

from __future__ import annotations

import json
import sys
import unittest
import json
import subprocess
import sys
import unittest

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import coverage  # noqa: E402  (Scripts/ is on sys.path above)

GAPS = REPOSITORY_ROOT / "Tests" / "Data" / "CoverageGaps"


class FeatureReportTests(unittest.TestCase):
    @unittest.skip("contract stub: ADR 0022 decision 7 (stream A)")
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

    @unittest.skip("contract stub: ADR 0022 decision 7 (stream A)")
    def test_unevaluated_gates_and_skipped_cases_fail_the_feature_suite(self) -> None:
        report = coverage.read_feature_report(GAPS / "Merge" / "Unevaluated" / "FeatureTest-Editor.json")
        failures = coverage.evaluate_feature_report(report)
        self.assertTrue(any("not evaluated" in failure for failure in failures), failures)
        report = coverage.read_feature_report(GAPS / "Merge" / "SkippedCase" / "FeatureTest-Editor.json")
        self.assertTrue(any("skipped" in failure for failure in coverage.evaluate_feature_report(report)))


class MergeTests(unittest.TestCase):
    @unittest.skip("contract stub: ADR 0022 decision 6 (stream A)")
    def test_merge_requires_union_over_modes(self) -> None:
        for case in sorted(path for path in (GAPS / "Merge").iterdir() if (path / "Expected.json").is_file()):
            with self.subTest(case=case.name):
                expected = json.loads((case / "Expected.json").read_text(encoding="utf-8"))
                reports = [coverage.read_feature_report(case / f"FeatureTest-{mode}.json") for mode in coverage.MODES]
                self.assertEqual(coverage.merge_coverage(reports), expected["Failures"])

    @unittest.skip("contract stub: ADR 0022 decision 6 (stream A)")
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

    @unittest.skip("contract stub: ADR 0022 decision 6 (stream A)")
    def test_merge_rejects_a_missing_or_duplicated_mode(self) -> None:
        case = GAPS / "Merge" / "Covered"
        editor = coverage.read_feature_report(case / "FeatureTest-Editor.json")
        release = coverage.read_feature_report(case / "FeatureTest-Release.json")
        self.assertTrue(coverage.merge_coverage([editor, release]))
        self.assertTrue(coverage.merge_coverage([editor, release, release]))


class MethodGateTests(unittest.TestCase):
    @unittest.skip("contract stub: ADR 0022 decision 6 (stream A)")
    def test_method_gate_names_uncovered_methods(self) -> None:
        directory = GAPS / "Gate5"
        catalog = json.loads((directory / "Methods.json").read_text(encoding="utf-8"))
        registered = [method["name"] for method in catalog["Methods"]]
        called = coverage.read_called_methods(directory / "CalledMethods.txt")
        expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
        self.assertEqual(coverage.uncovered_methods(registered, called), expected["Missing"])
        self.assertEqual(coverage.read_called_methods(directory / "Absent.txt"), set())


if __name__ == "__main__":
    unittest.main()
