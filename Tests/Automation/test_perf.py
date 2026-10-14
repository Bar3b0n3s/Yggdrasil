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

import Perf  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import perf  # noqa: E402


def capture_document(**changes: object) -> dict[str, object]:
    capture: dict[str, object] = {
        "format": perf.PERF_FORMAT, "version": perf.PERF_VERSION, "commit": "0" * 40, "config": "Release",
        "host": {"system": "windows", "machine": "x86_64", "device": "Test", "deviceClass": "test-1x",
                 "renderer": "vulkan"},
        "metrics": [perf.metric_to_json(perf.summarize("playCopy/1000Entities", "ms", [12.0]))],
        "scenes": [], "warnings": []}
    capture.update(changes)
    return capture


class PerfTests(unittest.TestCase):
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

    def test_nearest_rank_percentile_and_even_medians(self) -> None:
        values = [float(value) for value in range(1, 21)]  # 1 .. 20
        summary = perf.summarize("scene/Timing/simulationMs", "ms", list(reversed(values)))
        self.assertEqual(summary.p95, 19.0)  # rank ceil(0.95 * 20) = 19
        self.assertEqual(summary.median, 10.5)
        self.assertEqual(summary.maximum, 20.0)
        within = perf.summarize("playCopy/1000Entities", "ms", [29.0, 30.0, 31.0, 30.0])
        self.assertFalse(within.over_budget)  # the median equals the budget: not over it
        with self.assertRaises(ValueError):
            perf.summarize("x", "ms", [1.0, math.inf])

    def test_the_report_round_trips_canonically(self) -> None:
        capture = capture_document()
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

    def test_scene_rows_and_malformed_reports(self) -> None:
        frame = perf.metric_to_json(perf.summarize("scene/Physics/frameMs", "ms", [4.0, 5.0]))
        simulation = perf.metric_to_json(perf.summarize("scene/Physics/simulationMs", "ms", [1.0, 2.0]))
        scene = {"scene": "Physics", "ticks": 488, "frameMs": frame, "simulationMs": simulation,
                 "gpuPassMs": {"Opaque": 0.5}, "memory": {"privateStart": 1, "privateEnd": 2, "privatePeak": 3,
                                                          "residentPeak": 4}}
        headless = {**scene, "scene": "Timing", "frameMs": None, "gpuPassMs": {}, "memory": None}
        metrics = sorted([frame, simulation, perf.metric_to_json(perf.summarize("playCopy/1000Entities", "ms", [9.0]))],
                         key=lambda metric: metric["name"])
        capture = capture_document(metrics=metrics, scenes=[scene, headless], warnings=["a warning"])
        with tempfile.TemporaryDirectory(prefix="Perf-") as directory:
            path = Path(directory) / perf.PERF_REPORT_NAME
            perf.write_perf_report(path, capture)
            text = path.read_text(encoding="utf-8")
            self.assertEqual(text, json.dumps(capture, indent=2, sort_keys=True) + "\n")
            self.assertEqual(perf.read_perf_report(path), capture)

            malformed = [
                capture_document(format="Other"),
                {key: value for key, value in capture.items() if key != "host"},
                capture_document(host={"system": "windows"}),
                capture_document(metrics=list(reversed(metrics))),
                capture_document(scenes=[headless, scene]),
                capture_document(scenes=[{key: value for key, value in scene.items() if key != "memory"}]),
                capture_document(warnings=[1]),
            ]
            for document in malformed:
                with self.subTest(document=document):
                    with self.assertRaises(ValueError):
                        perf.write_perf_report(path, document)
            path.write_text("not json", encoding="utf-8")
            with self.assertRaises(ValueError) as raised:
                perf.read_perf_report(path)
            self.assertIn(str(path), str(raised.exception))
            # A failed write leaves the earlier report in place.
            self.assertEqual(path.read_text(encoding="utf-8"), "not json")

    def test_the_capture_reads_scene_names_gpu_passes_and_the_device_line(self) -> None:
        self.assertEqual(Perf.scene_name("Assets/Scenes/Golden/Bloom.scene"), "Golden/Bloom")
        self.assertEqual(Perf.scene_name("Assets/Scenes/Physics.scene"), "Physics")
        stats = {"views": [
            {"name": "scene", "gpuAvailable": True, "passes": [
                {"name": "Opaque", "gpuAvailable": True, "gpuMilliseconds": 9.0}]},
            {"name": "game", "gpuAvailable": True, "passes": [
                {"name": "Opaque", "gpuAvailable": True, "gpuMilliseconds": 1.5},
                {"name": "Bloom", "gpuAvailable": False, "gpuMilliseconds": 0.0},
                {"name": "Tonemap", "gpuAvailable": True, "gpuMilliseconds": 0.25}]},
        ]}
        self.assertEqual(Perf.Capture.gpu_pass_times(stats), {"Opaque": 1.5, "Tonemap": 0.25})  # the game view's
        del stats["views"][1]
        self.assertEqual(Perf.Capture.gpu_pass_times(stats), {"Opaque": 9.0})  # the scene view's without a game view
        self.assertEqual(Perf.Capture.gpu_pass_times({"views": []}), {})
        line = ("[21:00:00.000] [Engine] [info] Graphics device: 'NVIDIA GeForce RTX 5070 Ti Laptop GPU' (Discrete, "
                "Vulkan 1.4, class nvidia-61x, queue family 0); optional paths: none; validation off")
        match = Perf.DEVICE_LINE.search(line)
        self.assertIsNotNone(match)
        self.assertEqual((match.group("name"), match.group("class")),
                         ("NVIDIA GeForce RTX 5070 Ti Laptop GPU", "nvidia-61x"))


if __name__ == "__main__":
    unittest.main()
