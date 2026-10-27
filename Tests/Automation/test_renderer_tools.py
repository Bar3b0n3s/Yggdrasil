"""M18's renderer tools through the wire protocol: shaders.reload, the editor's shader hot reload, and profiler.capture,
one Chrome trace with CPU and GPU zones (Docs/Decisions/0029-m18-contract.md decisions 3 and 11)."""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client


class RendererToolsTests(AutomationTestCase):
    """shaders.reload and profiler.capture against a headless editor."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a 64 x 36 screenshot of the scene view."""
        result = client.call("viewport.screenshot", {"view": "scene", "width": 64, "height": 36})
        return Path(result["path"]).read_bytes()

    def make_scene(self, client: engine_client.EngineClient) -> None:
        """A cube in front of the scene view's camera, so every frame records the forward stage."""
        client.call("entity.create", {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})

    def read_trace(self, result: dict[str, Any]) -> list[dict[str, Any]]:
        """The complete ("X") events of the Chrome trace profiler.capture wrote, which must match the result's zone
        counts: CPU zones in category "cpu", GPU zones in category "gpu"."""
        trace = json.loads(Path(result["path"]).read_text(encoding="utf-8"))
        events = [event for event in trace["traceEvents"] if event.get("ph") == "X"]
        self.assertEqual(sum(1 for event in events if event["cat"] == "cpu"), result["cpuZones"])
        self.assertEqual(sum(1 for event in events if event["cat"] == "gpu"), result["gpuZones"])
        self.assertEqual(len(events), result["cpuZones"] + result["gpuZones"])
        # A capture of a few frames fits the profiler's rings: the trace is complete, and says so.
        self.assertEqual(result["droppedZones"], 0)
        self.assertEqual(trace["otherData"]["droppedZones"], 0)
        return events

    def test_shaders_reload_swaps_in_the_compiled_shaders_without_a_restart(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.make_scene(client)
        before = self.shoot(client)
        first = client.call("shaders.reload")
        second = client.call("shaders.reload")
        self.assertGreater(first["pipelineCount"], 0)
        self.assertEqual(second["pipelineCount"], first["pipelineCount"])
        self.assertEqual(second["generation"], first["generation"] + 1)
        # The same compiled shaders draw the same image through the reloaded pipelines.
        self.assertEqual(self.shoot(client), before)

    def test_shaders_reload_is_unsupported_without_a_renderer(self) -> None:
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("shaders.reload")
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")

    def test_profiler_capture_writes_one_chrome_trace_with_cpu_and_gpu_zones(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.make_scene(client)
        result = client.call("profiler.capture", {"frames": 10})
        self.assertEqual(result["frames"], 10)
        self.assertGreater(result["cpuZones"], 0)
        self.assertGreater(result["gpuZones"], 0)
        events = self.read_trace(result)
        cpu = {event["name"] for event in events if event["cat"] == "cpu"}
        gpu = {event["name"] for event in events if event["cat"] == "gpu"}
        # The frame loop's CPU zones, and the scene view's stages and passes on the GPU timeline.
        self.assertIn("FrameLoop::RunFrame", cpu)
        self.assertIn("Stage:Forward", cpu)
        self.assertIn("Stage:Forward", gpu)
        self.assertIn("ForwardOpaque", gpu)

    def test_profiler_capture_without_a_renderer_holds_cpu_zones_only(self) -> None:
        client, _ = self.open_editor_with_scene()
        result = client.call("profiler.capture", {"frames": 3})
        self.assertEqual(result["frames"], 3)
        self.assertGreater(result["cpuZones"], 0)
        self.assertEqual(result["gpuZones"], 0)
        events = self.read_trace(result)
        self.assertIn("FrameLoop::RunFrame", {event["name"] for event in events})
        for frames in (0, 601):
            with self.subTest(frames=frames):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("profiler.capture", {"frames": frames})
                self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)


if __name__ == "__main__":
    unittest.main()
