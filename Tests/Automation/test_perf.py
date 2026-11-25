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

    def test_the_static_scenery_is_20000_static_entities_in_49_cells_in_bounded_batches(self) -> None:
        # M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3 and its W43.3 record): a Static root over 10,000
        # pairs of a root and its built-in mesh child on a 2 m grid, each child made after its parent, in edit.batch
        # calls of at most 1,000 ops; budget 2 ms for the frame, none for the state hash.
        ops = Perf.static_scenery_ops()
        self.assertEqual(len(ops), 3 + 2 * Perf.STATIC_PAIRS)
        self.assertEqual(Perf.STATIC_PAIRS, 10000)
        self.assertEqual(ops[0]["params"], {"name": "Scenery", "components": {"Static": {}}})
        made = {"/Scenery"}
        meshes: list[dict[str, Any]] = []
        for op in ops[3:]:
            params = op["params"]
            self.assertIn(params["parent"], made)
            made.add(f"{params['parent']}/{params['name']}")
            if "MeshRenderer" in params["components"]:
                meshes.append(params)
        self.assertEqual(len(made), 1 + 2 * Perf.STATIC_PAIRS)
        self.assertEqual(len(meshes), Perf.STATIC_PAIRS)
        self.assertEqual({params["components"]["MeshRenderer"]["Mesh"] for params in meshes},
                         {Perf.BUILTIN_CUBE_MESH, "engine://Meshes/Sphere"})
        places = [op["params"]["components"]["Transform"]["Translation"] for op in ops[3::2]]
        cells = {(math.floor(x / 32.0), math.floor(z / 32.0)) for x, _, z in places}
        self.assertEqual(len(cells), 49)
        batches = Perf.static_scenery_batches()
        self.assertTrue(all(0 < len(batch) <= Perf.EDIT_BATCH_OPS for batch in batches))
        self.assertEqual([op for batch in batches for op in batch], ops)
        self.assertEqual(perf.BUDGETS[Perf.STATIC_METRIC], 2.0)
        self.assertNotIn(Perf.STATE_HASH_METRIC, perf.BUDGETS)
        self.assertEqual(Perf.CAPTURE_PARTS[-1], "staticScenery")

    def test_the_static_scenery_is_captured_only_when_the_game_view_drew_its_static_set_alone(self) -> None:
        # The frame per rendered tick of the generated scene, and its play.state round trip less the median of an empty
        # scene's; the scene just saved is played as the active scene (the editor refuses to open it again), the empty
        # one is opened. The editor stands in a fake client here, the timings in fixed samples.
        class FakeEditor:
            def __init__(self, view: dict[str, Any], rebuilds: int) -> None:
                self.active = ""
                self.calls: list[str] = []
                self.stats = {"staticCellRebuilds": rebuilds, "views": [{"name": "scene"}, {"name": "game", **view}]}

            def call(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
                self.calls.append(method)
                if method in ("scene.new", "scene.open"):
                    if method == "scene.open" and params["path"] == self.active:
                        raise engine_client.EngineError(method, -32000, "already open as the active scene", {})
                    self.active = params["path"]
                return self.stats if method == "stats.get" else {}

        class FixedSamples(Perf.Capture):
            hashes: list[list[float]] = []  # the static scene's round trips, then the empty one's

            def step_chunks(self, client: Any, render: str) -> list[float]:
                return [7.0, 6.9, 7.1] if render == "None" else []  # per tick, each call's one hash included

            def extraction_ms(self, client: Any) -> float:
                return 0.05

            def state_round_trips(self, client: Any) -> list[float]:
                return self.hashes.pop(0)

        drawn = {"available": True, "cpuMilliseconds": 0.5, "staticItems": Perf.STATIC_PAIRS, "dynamicItems": 0,
                 "staticCells": 49, "staticCellsVisible": 49, "shadowDraws": 4,
                 "passes": [{"name": "ForwardOpaque", "drawCalls": 2}]}
        capture = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
        capture.hashes = [[210.0, 207.0, 205.0], [1.0, 1.0, 2.0]]
        editor = FakeEditor(drawn, 0)
        capture.static_scenery(editor)
        self.assertEqual(capture.warnings, [])
        self.assertEqual([metric.name for metric in capture.metrics], [Perf.STATIC_METRIC, Perf.STATE_HASH_METRIC])
        # The tick less its call's hash (207 ms over 30 ticks: 6.9), the view's recording and an extraction.
        self.assertAlmostEqual(capture.metrics[0].median, 0.1 + 0.5 + 0.05)
        self.assertEqual(capture.metrics[0].budget, 2.0)
        self.assertEqual(capture.metrics[1].median, 206.0)  # 207 less the empty scene's median of 1
        self.assertIsNone(capture.metrics[1].budget)
        batches = len(Perf.static_scenery_batches())
        self.assertEqual(editor.calls, ["project.setSettings", "scene.new", "scene.save", "scene.new"]
                         + ["edit.batch"] * batches
                         + ["scene.save", "play.start", "play.step"] + ["play.step", "stats.get"] * Perf.SCENE_CHUNKS
                         + ["play.stop", "scene.open", "play.start", "play.stop"])
        self.assertEqual(editor.active, Perf.STATIC_EMPTY_SCENE)
        for view, rebuilds in (({**drawn, "staticItems": Perf.STATIC_PAIRS - 1}, 0), ({**drawn, "dynamicItems": 1}, 0),
                               (drawn, 1)):
            with self.subTest(view=view, rebuilds=rebuilds):
                dropped = FixedSamples(Console(json_mode=True), engine_client, "vulkan")
                dropped.hashes = [[207.0], [1.0]]
                dropped.static_scenery(FakeEditor(view, rebuilds))
                self.assertEqual(dropped.metrics, [])
                self.assertEqual(len(dropped.warnings), 1)
                self.assertIn("not captured", dropped.warnings[0])

    def test_a_profiler_traces_zone_has_its_mean_duration(self) -> None:
        # M43: the static scenery's extraction part, from a profiler.capture trace (Chrome trace JSON, durations in us).
        events = [{"ph": "X", "name": "ExtractRenderSnapshot", "dur": 40.0},
                  {"ph": "X", "name": "ExtractRenderSnapshot", "dur": 60},
                  {"ph": "X", "name": "Stage:Prepare", "dur": 900}, {"ph": "B", "name": "ExtractRenderSnapshot"}]
        with tempfile.TemporaryDirectory(prefix="Perf-") as directory:
            trace = Path(directory) / "trace.json"
            trace.write_text(json.dumps({"traceEvents": events}), encoding="utf-8")
            self.assertAlmostEqual(Perf.mean_zone_ms(trace, "ExtractRenderSnapshot"), 0.05)
            with self.assertRaises(ValueError):
                Perf.mean_zone_ms(trace, "FrameLoop::Render")
            with self.assertRaises(ValueError):
                Perf.mean_zone_ms(Path(directory) / "missing.json", "ExtractRenderSnapshot")

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
