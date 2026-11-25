"""Mesh levels of detail and draw distance through automation
(Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3B to 3D and 3K; Docs/Architecture.md §7.4, §8.3
"Mesh levels", §13.7): LodTree.gltf imported through asset.import {settings: {Lods: FromGltf}} keeps the setting in its
.meta (asset.getImportSettings, the other settings at their defaults), and its tree, placed through entity.create with a
RenderDistance, draws its level 0 near the Scene view's camera, both levels of its first switch inside that switch's
fade band and its last level far away (stats.get's meshLodDraws and lodCrossFades), and nothing once a CullDistance set
through entity.update lies nearer than it (distanceCulledMeshes), when the Scene view's screenshot is the scene's
without the tree, byte for byte.

The tree's bounding sphere has the radius sqrt(1.2^2 + 2^2 + 1.2^2) = 2.623 m about (0, 2, 0) (MakeGltfFixtures.py's
LodTree.gltf), and the Scene view's camera looks at that centre with a vertical field of view of 50 degrees, so its
screen size at a distance d is 2.623 / tan(25 deg) / d = 5.625 / d: 0.70 at 8 m (level 0), 0.40 at 14.06 m (the
middle of the band around its first switch, 0.4, whose FadeWidth 0.3 spans 0.34 to 0.46) and 0.094 at 60 m (below its
second switch, 0.15: level 2).

The Scene view renders at the editor's frames, so the test waits, bounded by FRAME_WAIT_SECONDS, for the frame after a
change to reach stats.get; no passing outcome depends on how long that takes.
"""

from __future__ import annotations

import shutil
import time
import unittest
from pathlib import Path
from typing import Any, Callable

from harness import AutomationTestCase, engine_client

FRAME_WAIT_SECONDS = 30.0
GLTF_FIXTURES = Path(__file__).resolve().parents[1] / "Data" / "Assets" / "Gltf"
TREE_MESH = "Assets/Models/LodTree.gltf#mesh:0:Tree"
SHOT = {"view": "scene", "width": 160, "height": 90}
# The tree's two submeshes: its trunk and its crown.
SUBMESHES = 2


def scene_view(client: engine_client.EngineClient) -> dict[str, Any]:
    """stats.get's entry of the view named "scene"."""
    return next(view for view in client.call("stats.get")["views"] if view["name"] == "scene")


class MeshLodTests(AutomationTestCase):
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

    def look_from(self, client: engine_client.EngineClient, distance: float) -> None:
        """Puts the Scene view's camera `distance` metres from the tree's bounding sphere's centre, looking at it."""
        client.call("viewport.camera", {"position": [0, 2, distance], "target": [0, 2, 0], "verticalFov": 50})

    def test_levels_follow_the_screen_size_cross_fade_in_a_band_and_cull_by_distance(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        source_dir = self.directory / "Downloads"
        source_dir.mkdir()
        shutil.copyfile(GLTF_FIXTURES / "LodTree.gltf", source_dir / "LodTree.gltf")
        client.call("asset.import", {"source": str(source_dir / "LodTree.gltf"), "destDir": "Assets/Models",
                                     "settings": {"Lods": "FromGltf"}})
        settings = client.call("asset.getImportSettings", {"asset": "Assets/Models/LodTree.gltf"})["settings"]
        self.assertEqual(settings["Lods"], "FromGltf")
        self.assertEqual(settings["LodScreenSizes"], [0.5, 0.25, 0.125, 0.0625])
        self.assertFalse(settings["Wind"]["Enabled"])
        client.call("edit.batch", {"label": "Tree", "ops": [
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-40, 20, 0]}, "DirectionalLight": {"Intensity": 2}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"AntiAliasing": "None", "BloomEnabled": False, "SsaoEnabled": False}}}}]})
        self.look_from(client, 8)
        empty = self.shoot(client)
        client.call("entity.create", {"name": "Tree", "components": {
            "MeshRenderer": {"Mesh": TREE_MESH}, "RenderDistance": {"FadeWidth": 0.3}}})

        near = self.wait_for_scene_view(client, lambda view: view["meshLodDraws"][0] >= SUBMESHES, "level 0 drawn")
        self.assertEqual(near["meshLodDraws"][1:], [0, 0, 0, 0])
        self.assertEqual(near["lodCrossFades"], 0)
        self.assertEqual(near["distanceCulledMeshes"], 0)

        # In the middle of the first switch's band both levels draw, each submesh twice.
        self.look_from(client, 14.06)
        band = self.wait_for_scene_view(client, lambda view: view["lodCrossFades"] == 1, "a cross-fade")
        self.assertGreaterEqual(band["meshLodDraws"][0], SUBMESHES)
        self.assertEqual(band["meshLodDraws"][1:], [SUBMESHES, 0, 0, 0])

        self.look_from(client, 60)
        far = self.wait_for_scene_view(client, lambda view: view["meshLodDraws"][2] == SUBMESHES, "level 2 drawn")
        self.assertEqual(far["meshLodDraws"][1], 0)
        self.assertEqual(far["lodCrossFades"], 0)

        # A cull distance nearer than the camera culls the tree: it draws nothing, so the image is the empty scene's.
        client.call("entity.update", {"entity": "/Tree", "components": {"RenderDistance": {"CullDistance": 30}}})
        culled = self.wait_for_scene_view(client, lambda view: view["distanceCulledMeshes"] == 1, "the tree culled")
        self.assertEqual(culled["meshLodDraws"][1:], [0, 0, 0, 0])
        self.look_from(client, 8)
        self.wait_for_scene_view(client, lambda view: view["distanceCulledMeshes"] == 0, "the tree drawn again")
        client.call("entity.update", {"entity": "/Tree", "components": {"RenderDistance": {"CullDistance": 5}}})
        self.wait_for_scene_view(client, lambda view: view["distanceCulledMeshes"] == 1, "the tree culled at 8 m")
        self.assertEqual(self.shoot(client), empty)
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Tree", "components": {"RenderDistance": {"FadeWidth": 0.75}}})
        client.call("scene.save")

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()


if __name__ == "__main__":
    unittest.main()
