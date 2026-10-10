"""Runtime M9 observations: actual host timing, isolated game-view history and native GPU allocations.

These scenarios exercise exported executables through automation. Reads never advance a paused game's simulation;
capture dimensions and private capture frame identities must not replace the displayed game view's history.
"""

from __future__ import annotations

import math
import time
from typing import Any

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game,
                       export_tiny_game, exported_executable)


class RuntimeRenderStatsTests(AutomationTestCase):
    """Export through the editor, then observe the actual Runtime rather than an injected statistics source."""

    def start_game(self, rendering: bool, vulkan_api: str = "1.4"
                   ) -> tuple[engine_client.EditorProcess, engine_client.EngineClient]:
        editor = self.start_editor()
        client = self.connect(editor)
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("project.setSettings", {"patch": {"Rendering": {
            "ShadowMapSize": 512, "SsaoHalfResolution": True}}})
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.assertEqual(editor.wait(), EXIT_SUCCESS, editor.output())
        arguments = ["--headless", "--automation", "--paused", "--expect-no-errors"]
        arguments += GPU_EDITOR_ARGUMENTS if rendering else ["--renderer", "none"]
        if rendering:
            arguments.append(f"--vulkan-api={vulkan_api}")
        game = engine_client.launch_editor(executable, arguments, self.directory / "Runtime", TINY_GAME_NAME)
        self.editors.append(game)
        return game, self.connect(game)

    def wait_for_stats(self, client: engine_client.EngineClient, gpu: bool = False,
                       after_frame: int = -1) -> dict[str, Any]:
        # A bounded observation wait, not a frame-time performance gate. A paused session still renders host frames.
        deadline = time.monotonic() + 60.0
        while True:
            stats = client.call("stats.get")
            self.assertEqual([view["name"] for view in stats["views"]], ["game"])
            ready = stats["fps"] > 0 and stats["cpuMilliseconds"] > 0
            view = stats["views"][0]
            if gpu:
                ready = (ready and view["available"] and view["gpuAvailable"]
                         and int(view["frame"]) > after_frame)
            if ready or time.monotonic() >= deadline:
                self.assertTrue(ready, stats)
                return stats

    def assert_finite_timings(self, stats: dict[str, Any]) -> None:
        for key in ("fps", "cpuMilliseconds", "droppedSeconds"):
            self.assertTrue(math.isfinite(stats[key]), (key, stats))
            self.assertGreaterEqual(stats[key], 0)

    def test_runtime_stats_without_renderer_measure_frames_without_stepping(self) -> None:
        game, client = self.start_game(rendering=False)
        before = client.call("play.state")
        stats = self.wait_for_stats(client)
        self.assert_finite_timings(stats)
        self.assertEqual(stats["entities"], 3)
        self.assertEqual(stats["bodies"], 0)
        self.assertEqual(stats["voices"], 0)
        self.assertTrue(stats["scriptAvailable"])
        self.assertGreater(stats["scriptHeapBytes"], 0)
        self.assertLessEqual(stats["scriptHeapBytes"], stats["scriptHardLimitBytes"])
        self.assertEqual(stats["memoryAllocationCount"], 0)
        self.assertEqual(stats["maxMemoryAllocationCount"], 0)
        view = stats["views"][0]
        self.assertFalse(view["available"])
        self.assertFalse(view["gpuAvailable"])
        self.assertEqual(view["frame"], "")
        self.assertEqual(view["gpuFrame"], "")
        self.assertEqual(view["passes"], [])
        after = client.call("play.state")
        self.assertEqual(after["tick"], before["tick"])
        self.assertEqual(after["stateHash"], before["stateHash"])
        client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())

    def test_runtime_stats_track_submitted_game_frames_and_survive_private_capture(self) -> None:
        self.assert_rendered_game_statistics("1.4")

    def test_runtime_stats_track_submitted_game_frames_and_survive_private_capture_vulkan13(self) -> None:
        self.assert_rendered_game_statistics("1.3")

    def assert_rendered_game_statistics(self, vulkan_api: str) -> None:
        if not self.require_gpu():
            return
        game, client = self.start_game(rendering=True, vulkan_api=vulkan_api)

        # Extract a real game snapshot before holding the session paused. Host render frames keep advancing at this tick.
        client.call("play.step", {"ticks": 1, "render": "last"})
        before = client.call("play.state")
        stats = self.wait_for_stats(client, gpu=True)
        self.assert_finite_timings(stats)
        self.assertGreater(stats["memoryAllocationCount"], 0)
        self.assertLessEqual(stats["memoryAllocationCount"], stats["maxMemoryAllocationCount"])
        view = stats["views"][0]
        self.assertEqual((view["width"], view["height"]), (WINDOW_WIDTH, WINDOW_HEIGHT))
        self.assertTrue(view["frame"].isdigit())
        self.assertTrue(view["gpuFrame"].isdigit())
        self.assertLessEqual(int(view["gpuFrame"]), int(view["frame"]))
        self.assertTrue(view["passes"])
        self.assertTrue(any(entry["gpuAvailable"] for entry in view["passes"]))
        for entry in view["passes"]:
            for key in ("cpuMilliseconds", "gpuMilliseconds"):
                self.assertTrue(math.isfinite(entry[key]))
                self.assertGreaterEqual(entry[key], 0)
        shot = client.call("viewport.screenshot", {"view": "game", "width": 73, "height": 51})
        self.assertEqual((shot["width"], shot["height"]), (73, 51))
        following = self.wait_for_stats(client, gpu=True, after_frame=int(view["frame"]))
        game_view = following["views"][0]
        self.assertEqual((game_view["width"], game_view["height"]), (WINDOW_WIDTH, WINDOW_HEIGHT))
        self.assertGreaterEqual(int(game_view["gpuFrame"]), int(view["gpuFrame"]))
        self.assertLessEqual(int(game_view["gpuFrame"]), int(game_view["frame"]))
        after = client.call("play.state")
        self.assertEqual(after["tick"], before["tick"])
        self.assertEqual(after["stateHash"], before["stateHash"])
        client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())
