"""Planar reflections through automation (Docs/Decisions/0059-m42-planar-reflections.md decision 3; Docs/Architecture.md
§5.3, §8.3, §13.7): a PlanarReflection made through edit.batch on a built-in Quad mirror before a box is drawn by the
Scene view, which stats.get reports (planarReflections 1, the mirror's entity, the slot's PlanarReflection0 row); turned
away from the camera through entity.update, the mirror is culled (culledPlanarReflections 1) and draws nothing.

The Scene view renders at the editor's frames, so the test waits, bounded by FRAME_WAIT_SECONDS, for the frame after a
change to reach stats.get; no passing outcome depends on how long that takes.
"""

from __future__ import annotations

import time
import unittest
from typing import Any, Callable

from harness import AutomationTestCase, engine_client

FRAME_WAIT_SECONDS = 30.0


def scene_view(client: engine_client.EngineClient) -> dict[str, Any]:
    """stats.get's entry of the view named "scene"."""
    return next(view for view in client.call("stats.get")["views"] if view["name"] == "scene")


class PlanarReflectionTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def wait_for_scene_view(self, client: engine_client.EngineClient, condition: Callable[[dict[str, Any]], bool],
                            what: str) -> dict[str, Any]:
        """The Scene view's stats once `condition` holds, failing after FRAME_WAIT_SECONDS."""
        deadline = time.monotonic() + FRAME_WAIT_SECONDS
        while True:
            view = scene_view(client)
            if view["available"] and condition(view):
                return view
            if time.monotonic() > deadline:
                self.fail(f"the Scene view did not report {what} within {FRAME_WAIT_SECONDS:.0f} s: {view}")

    def test_a_mirror_is_drawn_and_culled_when_turned_away(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Mirror.material",
                                     "values": {"BaseColor": [0.95, 0.95, 0.95, 1], "Metallic": 1, "Roughness": 0.03}})
        result = client.call("edit.batch", {"label": "Mirror scene", "ops": [
            {"method": "entity.create", "params": {"name": "Mirror", "components": {
                "Transform": {"Translation": [0, 1, -3], "Scale": [3, 2, 1]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Quad", "Materials": ["Assets/Materials/Mirror.material"]},
                "PlanarReflection": {}}}},
            {"method": "entity.create", "params": {"name": "Box", "components": {
                "Transform": {"Translation": [0.8, 0.5, 1]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}}},
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-40, 20, 0]}, "DirectionalLight": {"Intensity": 2}}}}]})
        mirror = result["results"][0]["entity"]["id"]
        client.call("viewport.camera", {"position": [0, 1.2, 5], "target": [0, 1, -3]})

        drawn = self.wait_for_scene_view(client, lambda view: view["planarReflections"] == 1, "the mirror drawn")
        self.assertEqual(drawn["planarReflectionEntities"], [mirror])
        self.assertEqual(drawn["droppedPlanarReflections"], 0)
        self.assertEqual(drawn["culledPlanarReflections"], 0)
        self.assertIn("PlanarReflection0", [entry["name"] for entry in drawn["passes"]])

        # Turned half a turn, the glass faces away from the camera: the view culls it and records no slot.
        client.call("entity.update", {"entity": "/Mirror", "components": {"Transform": {"EulerAngles": [0, 180, 0]}}})
        culled = self.wait_for_scene_view(client, lambda view: view["culledPlanarReflections"] == 1,
                                          "the mirror culled")
        self.assertEqual(culled["planarReflections"], 0)
        self.assertEqual(culled["planarReflectionEntities"], [])
        self.assertNotIn("PlanarReflection0", [entry["name"] for entry in culled["passes"]])
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
