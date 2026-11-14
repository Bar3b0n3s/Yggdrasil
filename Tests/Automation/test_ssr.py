"""Screen-space reflections through automation (Docs/Architecture.md §8.3; Docs/Decisions/0037-m26-contract.md
decision 18, stream X-D): a PostProcess with SsrEnabled adds the SSR pass to the editor's displayed views (their
stats.get rows) and to viewport.screenshot, whose two screenshots of one state are byte-identical and differ from the
image without SSR, while SSR off records nothing, so its screenshot is the image before SSR was enabled, byte for byte.

Every wait here is a bounded observation wait (the displayed views report a frame after the edit that changed them),
never a timing gate.
"""

from __future__ import annotations

import time
import unittest
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client

OBSERVATION_SECONDS = 60.0
SHOT = {"view": "scene", "width": 160, "height": 90}


class SsrTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()

    def latest_frame(self, client: engine_client.EngineClient) -> int:
        """The latest frame a displayed view rendered, -1 before any."""
        return max((int(view["frame"]) for view in client.call("stats.get")["views"] if view["available"]), default=-1)

    def wait_for_view_rows(self, client: engine_client.EngineClient, edited: int, present: bool) -> None:
        """Waits (bounded by OBSERVATION_SECONDS) until a displayed view rendered a frame after `edited` whose stats
        rows hold SSR (`present`) or not."""
        deadline = time.monotonic() + OBSERVATION_SECONDS
        while True:
            stats = client.call("stats.get")
            views: list[dict[str, Any]] = [view for view in stats["views"]
                                           if view["available"] and int(view["frame"]) > edited]
            if any(("SSR" in {entry["name"] for entry in view["passes"]}) == present for view in views):
                return
            if time.monotonic() >= deadline:
                self.fail(f"no displayed view {'recorded' if present else 'left out'} the SSR pass within "
                          f"{OBSERVATION_SECONDS:.0f} s: {stats}")

    def test_ssr_reflects_in_the_views_and_ssr_off_is_byte_identical(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        # A mirror floor (a white metal of roughness 0) in front of a red-emitting cube, which SSR reflects.
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Mirror.material",
                                     "values": {"Metallic": 1, "Roughness": 0}})
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Glow.material",
                                     "values": {"BaseColor": [0, 0, 0, 1], "Roughness": 1, "Emissive": [1, 0.2, 0.05]}})
        client.call("edit.batch", {"label": "SSR scene", "ops": [
            {"method": "entity.create", "params": {"name": "Floor", "components": {
                "Transform": {"Scale": [8, 1, 8]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Plane", "Materials": ["Assets/Materials/Mirror.material"]}}}},
            {"method": "entity.create", "params": {"name": "Cube", "components": {
                "Transform": {"Translation": [0, 0.5, -1]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube", "Materials": ["Assets/Materials/Glow.material"]}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"SsrEnabled": False, "AntiAliasing": "None", "BloomEnabled": False}}}}]})
        client.call("viewport.camera", {"position": [0, 2, 4], "target": [0, 0, -1]})
        before = self.shoot(client)
        self.wait_for_view_rows(client, self.latest_frame(client), present=False)

        client.call("entity.update", {"entity": "/Post", "components": {"PostProcess": {"SsrEnabled": True}}})
        reflected = self.shoot(client)
        # SSR has no history: a second screenshot of the same state is byte-identical.
        self.assertEqual(self.shoot(client), reflected)
        self.assertNotEqual(reflected, before)
        self.wait_for_view_rows(client, self.latest_frame(client), present=True)

        # SSR off records nothing: the screenshot is the image before SSR was enabled, byte for byte.
        client.call("entity.update", {"entity": "/Post", "components": {"PostProcess": {"SsrEnabled": False}}})
        self.assertEqual(self.shoot(client), before)
        self.wait_for_view_rows(client, self.latest_frame(client), present=False)
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
