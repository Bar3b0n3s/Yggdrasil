"""Fog through automation (Docs/Decisions/0047-m32-fog-ssr.md decisions 2, 6 and 9; Docs/Architecture.md §5.3, §8.6):
a Fog component created and changed through entity.create and entity.update reaches viewport.screenshot, whose two
screenshots of one state are byte-identical; a second Fog in the scene is refused as a unique-per-scene component, a
value outside its range is refused, and removing the Fog brings back the image without it, byte for byte.

Every call here is a direct request and its answer; nothing waits on time.
"""

from __future__ import annotations

import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client

SHOT = {"view": "scene", "width": 160, "height": 90}


class FogTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()

    def test_fog_is_created_animated_and_removed_through_automation(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("edit.batch", {"label": "Fog scene", "ops": [
            {"method": "entity.create", "params": {"name": "Ground", "components": {
                "Transform": {"Scale": [10, 1, 20]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Plane"}}}},
            {"method": "entity.create", "params": {"name": "Pillar", "components": {
                "Transform": {"Translation": [0, 1, -8], "Scale": [0.6, 2, 0.6]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}}},
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-40, 20, 0]}, "DirectionalLight": {"Intensity": 2}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"AntiAliasing": "None", "BloomEnabled": False, "SsaoEnabled": False}}}}]})
        client.call("viewport.camera", {"position": [0, 1.5, 4], "target": [0, 1, -8]})
        clear = self.shoot(client)

        client.call("entity.create", {"name": "Haze", "components": {"Fog": {
            "Color": [0.7, 0.72, 0.75], "Density": 0.15, "StartDistance": 1}}})
        fogged = self.shoot(client)
        self.assertNotEqual(fogged, clear)
        self.assertEqual(self.shoot(client), fogged)

        # Thickening the fog, as a game does near a realm's heart, changes the image again.
        client.call("entity.update", {"entity": "/Haze", "components": {"Fog": {"Density": 0.6, "HeightDensity": 0.5}}})
        thick = self.shoot(client)
        self.assertNotEqual(thick, fogged)
        read = client.call("entity.get", {"entity": "/Haze", "components": ["Fog"]})["entity"]["components"]["Fog"]
        self.assertAlmostEqual(read["Density"], 0.6, places=6)
        self.assertAlmostEqual(read["HeightDensity"], 0.5, places=6)

        # A value outside the field's range and a second Fog are refused; the scene keeps its one fog.
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Haze", "components": {"Fog": {"MaxOpacity": 2}}})
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.create", {"name": "SecondHaze", "components": {"Fog": {}}})
        self.assertEqual(self.shoot(client), thick)

        # Without the Fog the image is the one before it, byte for byte.
        client.call("entity.update", {"entity": "/Haze", "removeComponents": ["Fog"]})
        self.assertEqual(self.shoot(client), clear)
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
