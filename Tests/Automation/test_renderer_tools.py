"""M18's renderer tools through the wire protocol: shaders.reload, the editor's shader hot reload, and profiler.capture,
one Chrome trace with CPU and GPU zones (Docs/Decisions/0029-m18-contract.md decisions 3 and 11). Skipped until streams
B, C and E implement and register the methods."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

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

    @unittest.skip("contract stub: shaders.reload (Docs/Decisions/0029-m18-contract.md decision 3, streams B and E)")
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

    @unittest.skip("contract stub: shaders.reload (Docs/Decisions/0029-m18-contract.md decision 3, streams B and E)")
    def test_shaders_reload_is_unsupported_without_a_renderer(self) -> None:
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("shaders.reload")
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")

    @unittest.skip("contract stub: profiler.capture (Docs/Decisions/0029-m18-contract.md decision 11, streams C and E)")
    def test_profiler_capture_writes_one_chrome_trace_with_cpu_and_gpu_zones(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.make_scene(client)
        result = client.call("profiler.capture", {"frames": 10})
        self.assertEqual(result["frames"], 10)
        self.assertGreater(result["cpuZones"], 0)
        self.assertGreater(result["gpuZones"], 0)
        trace = json.loads(Path(result["path"]).read_text(encoding="utf-8"))
        events = [event for event in trace["traceEvents"] if event.get("ph") == "X"]
        self.assertEqual(len(events), result["cpuZones"] + result["gpuZones"])
        # Ten frames fit the profiler's rings: the trace is complete, and says so.
        self.assertEqual(result["droppedZones"], 0)
        self.assertEqual(trace["otherData"]["droppedZones"], 0)
        names = {event["name"] for event in events}
        # The frame loop's CPU zones, and the scene view's stages and passes on the GPU.
        self.assertIn("FrameLoop::RunFrame", names)
        self.assertIn("Stage:Forward", names)
        self.assertIn("ForwardOpaque", names)

    @unittest.skip("contract stub: profiler.capture (Docs/Decisions/0029-m18-contract.md decision 11, streams C and E)")
    def test_profiler_capture_without_a_renderer_holds_cpu_zones_only(self) -> None:
        client, _ = self.open_editor_with_scene()
        result = client.call("profiler.capture", {"frames": 3})
        self.assertGreater(result["cpuZones"], 0)
        self.assertEqual(result["gpuZones"], 0)
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("profiler.capture", {"frames": 0})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)


if __name__ == "__main__":
    unittest.main()
