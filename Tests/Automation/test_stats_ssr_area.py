"""The SSR pass's traced area in stats.get (Docs/Decisions/0047-m32-fog-ssr.md decisions 10, 11 and 13;
Docs/Architecture.md §13.7): with SSR on, a displayed view reports, for a completed GPU frame, the trace image's 8 x 8
tiles, the tiles the trace covered and the pixels that traced; a floor glossy over a part of the view covers fewer
tiles than the whole, a rough floor none, and Rendering.SsrHalfResolution traces a quarter of the tiles, which the view
reports with ssrHalfResolution.

Every wait here is a bounded observation wait (a displayed view reports a completed frame after the edit that changed
it), never a timing gate.
"""

from __future__ import annotations

import time
import unittest
from typing import Any

from harness import AutomationTestCase, engine_client

OBSERVATION_SECONDS = 60.0


class StatsSsrAreaTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def latest_frame(self, client: engine_client.EngineClient) -> int:
        """The latest frame a displayed view rendered, -1 before any."""
        return max((int(view["frame"]) for view in client.call("stats.get")["views"] if view["available"]), default=-1)

    def wait_for_area(self, client: engine_client.EngineClient, after: int) -> dict[str, Any]:
        """Waits (bounded by OBSERVATION_SECONDS) until the scene view reports the traced area of a completed frame
        rendered after frame `after`, and returns that view."""
        deadline = time.monotonic() + OBSERVATION_SECONDS
        while True:
            stats = client.call("stats.get")
            for view in stats["views"]:
                if view["name"] == "scene" and view["ssrAreaFrame"] and int(view["ssrAreaFrame"]) > after:
                    return view
            if time.monotonic() >= deadline:
                self.fail(f"the scene view reported no traced SSR area after frame {after} within "
                          f"{OBSERVATION_SECONDS:.0f} s: {stats}")

    def test_the_views_report_the_area_the_ssr_trace_covered(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Mirror.material",
                                     "values": {"Metallic": 1, "Roughness": 0}})
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Rough.material",
                                     "values": {"Roughness": 1}})
        # A mirror strip down the middle of a rough floor, a rough cube to reflect (the default material's roughness,
        # 0.5, would trace under SsrMaxRoughness 0.6), SSR on.
        client.call("edit.batch", {"label": "SSR area scene", "ops": [
            {"method": "entity.create", "params": {"name": "Floor", "components": {
                "Transform": {"Scale": [8, 1, 8]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Plane", "Materials": ["Assets/Materials/Rough.material"]}}}},
            {"method": "entity.create", "params": {"name": "Strip", "components": {
                "Transform": {"Translation": [0, 0.01, 0], "Scale": [1, 1, 8]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Plane", "Materials": ["Assets/Materials/Mirror.material"]}}}},
            {"method": "entity.create", "params": {"name": "Cube", "components": {
                "Transform": {"Translation": [0, 0.5, -1]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube", "Materials": ["Assets/Materials/Rough.material"]}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"SsrEnabled": True, "AntiAliasing": "None", "BloomEnabled": False}}}}]})
        client.call("viewport.camera", {"position": [0, 4, 6], "target": [0, 0, -1]})

        partial = self.wait_for_area(client, self.latest_frame(client))
        self.assertFalse(partial["ssrHalfResolution"])
        self.assertGreater(partial["ssrTiles"], 0)
        self.assertGreater(partial["ssrTracedTiles"], 0)
        self.assertLess(partial["ssrTracedTiles"], partial["ssrTiles"])
        self.assertGreater(partial["ssrTracedPixels"], 0)
        self.assertLessEqual(partial["ssrTracedPixels"], partial["ssrTracedTiles"] * 64)
        width, height = partial["width"], partial["height"]
        self.assertEqual(partial["ssrTiles"], ((width + 7) // 8) * ((height + 7) // 8))

        # A rough strip traces nothing.
        client.call("entity.update", {"entity": "/Strip", "components": {
            "MeshRenderer": {"Materials": ["Assets/Materials/Rough.material"]}}})
        rough = self.wait_for_area(client, self.latest_frame(client))
        self.assertEqual(rough["ssrTracedTiles"], 0)
        self.assertEqual(rough["ssrTracedPixels"], 0)

        # Half resolution: a quarter of the tiles, about a quarter of the traced pixels.
        client.call("entity.update", {"entity": "/Strip", "components": {
            "MeshRenderer": {"Materials": ["Assets/Materials/Mirror.material"]}}})
        client.call("project.setSettings", {"patch": {"Rendering": {"SsrHalfResolution": True}}})
        half = self.wait_for_area(client, self.latest_frame(client))
        self.assertTrue(half["ssrHalfResolution"])
        self.assertEqual(half["ssrTiles"], ((width + 1) // 2 + 7) // 8 * (((height + 1) // 2 + 7) // 8))
        self.assertGreater(half["ssrTracedTiles"], 0)
        self.assertLess(half["ssrTracedPixels"], partial["ssrTracedPixels"])
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
