"""The perf capture's summaries and report (Docs/Architecture.md §15.1 T6; Docs/Decisions/0024-m15-contract.md decision
16, stream D): warning-only numbers, so these tests check the arithmetic and the format, never a timing. They need no
editor build; the capture itself runs in CI.py's hardening stage.
"""

from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import perf  # noqa: E402  (Scripts/ is on sys.path above)


class PerfTests(unittest.TestCase):
    @unittest.skip("contract stub: perf (ADR 0024 decision 16, stream D)")
    def test_summaries_and_budgets(self) -> None:
        summary = perf.summarize("playCopy/1000Entities", "ms", [20.0, 40.0, 25.0, 31.0, 35.0])
        self.assertEqual(summary.samples, 5)
        self.assertEqual(summary.median, 31.0)
        self.assertEqual(summary.maximum, 40.0)
        self.assertEqual(summary.p95, 40.0)
        self.assertAlmostEqual(summary.mean, 30.2)
        self.assertEqual(summary.budget, 30.0)
        self.assertTrue(summary.over_budget)
        untracked = perf.summarize("scene/Physics/frameMs", "ms", [1.0])
        self.assertIsNone(untracked.budget)
        self.assertFalse(untracked.over_budget)
        for invalid in ([], [math.nan]):
            with self.assertRaises(ValueError):
                perf.summarize("x", "ms", invalid)

    @unittest.skip("contract stub: perf (ADR 0024 decision 16, stream D)")
    def test_the_report_round_trips_canonically(self) -> None:
        capture = {"format": perf.PERF_FORMAT, "version": perf.PERF_VERSION, "commit": "0" * 40, "config": "Release",
                   "host": {"system": "windows", "machine": "x86_64", "device": "Test", "deviceClass": "test-1x",
                            "renderer": "vulkan"},
                   "metrics": [perf.metric_to_json(perf.summarize("playCopy/1000Entities", "ms", [12.0]))],
                   "scenes": [], "warnings": []}
        with tempfile.TemporaryDirectory(prefix="Perf-") as directory:
            path = Path(directory) / "TestResults" / perf.PERF_REPORT_NAME
            perf.write_perf_report(path, capture)
            text = path.read_text(encoding="utf-8")
            self.assertEqual(json.loads(text), capture)
            self.assertNotIn("\r", text)
            self.assertEqual(perf.read_perf_report(path), capture)
            path.write_text(json.dumps({**capture, "version": 2}), encoding="utf-8")
            with self.assertRaises(ValueError):
                perf.read_perf_report(path)


if __name__ == "__main__":
    unittest.main()
