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
from typing import Any

from harness import REPOSITORY_ROOT, engine_client

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Perf  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import perf  # noqa: E402
from Lib.report import Console  # noqa: E402


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
        # M30: the navigation case's budget, about half of FeatureTest's 64 Hz fixed step.
        self.assertEqual(perf.summarize("navigation/150Agents/tickMs", "ms", [4.0]).budget, 8.0)
        self.assertTrue(perf.summarize("navigation/150Agents/tickMs", "ms", [7.0, 9.0, 10.0]).over_budget)
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

    def test_the_look_costs_sum_the_game_views_passes_against_a_baseline(self) -> None:
        # M32 (Docs/Decisions/0047-m32-fog-ssr.md Acceptance 5 and 6): the summed GPU time of the passes fog reaches,
        # from the game view's completed frame only, and each sample with an effect less the median without it.
        stats = {"views": [{"name": "scene", "gpuAvailable": True, "passes": []},
                           {"name": "game", "gpuAvailable": True, "passes": [
                               {"name": "ForwardOpaque", "gpuAvailable": True, "gpuMilliseconds": 1.5},
                               {"name": "Skybox", "gpuAvailable": True, "gpuMilliseconds": 0.25},
                               {"name": "Tonemap", "gpuAvailable": True, "gpuMilliseconds": 9.0},
                               {"name": "SSR", "gpuAvailable": False, "gpuMilliseconds": 4.0}]}]}
        view = Perf.game_view(stats)
        self.assertEqual(Perf.summed_gpu_time(view, Perf.FOG_PASSES), 1.75)
        self.assertIsNone(Perf.summed_gpu_time({"gpuAvailable": False, "passes": []}, Perf.FOG_PASSES))
        self.assertEqual(Perf.game_view({"views": []}), {})
        self.assertEqual(Perf.cost_samples([2.0, 2.5, 1.5], [1.0, 1.25, 0.75]), [1.0, 1.5, 0.5])
        self.assertEqual(len(Perf.ssr_tile_names()), Perf.SSR_COLUMNS * Perf.SSR_ROWS)
        self.assertEqual(sum(1 for op in Perf.ssr_floor_ops() if op["params"]["name"].startswith("Tile")), 35)
        self.assertEqual(perf.BUDGETS["fog/Fog/gpuCostMs"], 0.5)

    def test_the_fog_cost_measures_with_and_without_the_fog_in_one_opening_of_its_scene(self) -> None:
        # M32's review: the capture opened the fog's scene again after removing its Fog, which the editor refuses for
        # the active scene, so the metric was never captured. The editor's refusal stands in a fake client here.
        class FakeEditor:
            def __init__(self) -> None:
                self.active = ""
                self.calls: list[str] = []

            def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
                self.calls.append(method)
                if method == "scene.open":
                    if params["path"] == self.active:
                        raise engine_client.EngineError(method, -32000, "already open as the active scene", {})
                    self.active = params["path"]
                return {}

        class FixedSamples(Perf.Capture):
            def play_samples(self, client: Any, passes: tuple[str, ...]) -> tuple[list[float], dict[str, Any]]:
                fogged = "entity.update" not in client.calls
                return ([1.25, 1.5, 1.0] if fogged else [1.0, 1.0, 1.0]), {}

        capture = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        editor = FakeEditor()
        capture.fog_cost(editor)
        self.assertEqual(capture.warnings, [])
        self.assertEqual(editor.calls, ["scene.open", "entity.update"])
        self.assertEqual([metric.name for metric in capture.metrics], ["fog/Fog/gpuCostMs"])
        self.assertEqual(capture.metrics[0].median, 0.25)

    def test_the_mirror_cost_sums_the_reflection_rows_of_a_room_of_three_mirrors(self) -> None:
        # M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): reflections/Mirrors3/gpuMs sums the slots'
        # rows, each a mirror's whole view, over a generated room whose three Quad mirrors carry a PlanarReflection;
        # budget 2 ms.
        stats = {"views": [{"name": "game", "gpuAvailable": True, "passes": [
            {"name": "PlanarReflection0", "gpuAvailable": True, "gpuMilliseconds": 0.5},
            {"name": "PlanarReflection1", "gpuAvailable": True, "gpuMilliseconds": 0.25},
            {"name": "PlanarReflection2", "gpuAvailable": True, "gpuMilliseconds": 0.125},
            {"name": "ForwardOpaque", "gpuAvailable": True, "gpuMilliseconds": 3.0}]}]}
        self.assertEqual(Perf.summed_gpu_time(Perf.game_view(stats), Perf.MIRROR_PASSES), 0.875)
        mirrors = [op for op in Perf.mirror_room_ops() if "PlanarReflection" in op["params"]["components"]]
        self.assertEqual(len(mirrors), Perf.MIRROR_COUNT)
        for mirror in mirrors:
            self.assertEqual(mirror["params"]["components"]["MeshRenderer"]["Mesh"], "engine://Meshes/Quad")
        self.assertEqual(perf.BUDGETS[Perf.MIRROR_METRIC], 2.0)

    def test_the_mirror_cost_is_captured_only_when_the_view_drew_the_three_mirrors(self) -> None:
        class FakeEditor:
            def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
                return {}

        class FixedSamples(Perf.Capture):
            drawn = 3

            def play_samples(self, client: Any, passes: tuple[str, ...]) -> tuple[list[float], dict[str, Any]]:
                return [1.5, 1.25, 1.0], {"planarReflections": self.drawn}

        capture = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        capture.mirror_cost(FakeEditor())
        self.assertEqual(capture.warnings, [])
        self.assertEqual([metric.name for metric in capture.metrics], [Perf.MIRROR_METRIC])
        self.assertEqual(capture.metrics[0].median, 1.25)
        dropped = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        dropped.drawn = 2
        dropped.mirror_cost(FakeEditor())
        self.assertEqual(dropped.metrics, [])
        self.assertEqual(len(dropped.warnings), 1)
        self.assertIn("drew 2 of the 3", dropped.warnings[0])

    def test_the_forest_cost_sums_the_depth_shadow_and_forward_rows_of_a_forest_with_levels(self) -> None:
        # M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3K): lod/Forest/gpuMs sums the
        # DepthNormal row, the Shadows pass's rows and the ForwardOpaque row over a generated forest of 2,000 trees with
        # authored levels and 4,000 rocks with generated ones on a 1 km plane under a gale; budget 3 ms.
        stats = {"views": [{"name": "game", "gpuAvailable": True, "passes": [
            {"name": "DepthNormal", "gpuAvailable": True, "gpuMilliseconds": 0.5},
            {"name": "DirectionalShadows", "gpuAvailable": True, "gpuMilliseconds": 0.75},
            {"name": "ForwardOpaque", "gpuAvailable": True, "gpuMilliseconds": 1.25},
            {"name": "Tonemap", "gpuAvailable": True, "gpuMilliseconds": 4.0}]}]}
        self.assertEqual(Perf.summed_gpu_time(Perf.game_view(stats), Perf.FOREST_PASSES), 2.5)
        ops = Perf.forest_ops()
        meshes = [op["params"]["components"]["MeshRenderer"]["Mesh"] for op in ops
                  if "MeshRenderer" in op["params"]["components"]]
        self.assertEqual(meshes.count(Perf.FOREST_TREE_MESH), 2000)
        self.assertEqual(meshes.count(Perf.FOREST_ROCK_MESH), 4000)
        names = [op["params"]["name"] for op in ops]
        self.assertEqual(len(names), len(set(names)))
        half = Perf.FOREST_EXTENT / 2
        for op in ops:
            translation = op["params"]["components"].get("Transform", {}).get("Translation", [0, 0, 0])
            if op["params"]["name"].startswith(("Tree", "Rock")):
                self.assertLess(abs(translation[0]), half, op["params"]["name"])
                self.assertLess(abs(translation[2]), half, op["params"]["name"])
        self.assertEqual(sum(1 for op in ops if "Wind" in op["params"]["components"]), 1)
        self.assertEqual(perf.BUDGETS[Perf.FOREST_METRIC], 3.0)

    def test_the_forest_cost_is_captured_only_when_the_view_drew_a_coarser_level(self) -> None:
        class FakeEditor:
            def __init__(self) -> None:
                self.batches: list[int] = []
                self.meshes: set[str] = set()

            def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
                if method == "asset.info":
                    handle = "000000000000a001" if params["asset"] == Perf.FOREST_TREE_MESH else "000000000000a002"
                    return {"asset": {"id": handle, "path": params["asset"], "type": "Mesh"}}
                if method == "edit.batch":
                    self.batches.append(len(params["ops"]))
                    self.meshes |= {op["params"]["components"]["MeshRenderer"]["Mesh"] for op in params["ops"]
                                    if "MeshRenderer" in op["params"]["components"]}
                return {}

        class FixedSamples(Perf.Capture):
            draws = [900, 1200, 2400, 0, 0]

            def play_samples(self, client: Any, passes: tuple[str, ...]) -> tuple[list[float], dict[str, Any]]:
                return [2.5, 2.0, 1.5], {"meshLodDraws": self.draws, "lodCrossFades": 12}

        capture = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        editor = FakeEditor()
        capture.forest_cost(editor)
        self.assertEqual(capture.warnings, [])
        self.assertEqual([metric.name for metric in capture.metrics], [Perf.FOREST_METRIC])
        self.assertEqual(capture.metrics[0].median, 2.0)
        # The forest's ops go in batches of at most FOREST_BATCH_OPS, naming the meshes by the handles asset.info gives
        # (an op naming a project path would refresh the project's assets).
        self.assertEqual(sum(editor.batches), len(Perf.forest_ops()))
        self.assertLessEqual(max(editor.batches), Perf.FOREST_BATCH_OPS)
        self.assertEqual(editor.meshes, {"engine://Meshes/Plane", "000000000000a001", "000000000000a002"})
        finest = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        finest.draws = [4500, 0, 0, 0, 0]
        finest.forest_cost(FakeEditor())
        self.assertEqual(finest.metrics, [])
        self.assertEqual(len(finest.warnings), 1)
        self.assertIn("no coarser level", finest.warnings[0])

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
                "Vulkan 1.4, class nvidia-62x, queue family 0); optional paths: none; validation off")
        match = Perf.DEVICE_LINE.search(line)
        self.assertIsNotNone(match)
        self.assertEqual((match.group("name"), match.group("class")),
                         ("NVIDIA GeForce RTX 5070 Ti Laptop GPU", "nvidia-62x"))


if __name__ == "__main__":
    unittest.main()
