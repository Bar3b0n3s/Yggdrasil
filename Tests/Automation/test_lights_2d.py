"""2D lights and lit sprites through automation (Docs/Decisions/0032-m21-contract.md decisions 20 to 22; Roadmap M21's
2D lighting, R-2D-LIGHT)."""

from __future__ import annotations

import unittest
from pathlib import Path

from harness import AutomationTestCase
from test_environment import png_pixels

SKIP_LIGHTS_2D = "contract stub: the Lights2D pass and lit sprites (ADR 0032 decisions 20 to 22, stream L)"


class Lights2DTests(AutomationTestCase):
    @unittest.skip(SKIP_LIGHTS_2D)
    def test_a_lit_sprite_takes_the_2d_lights_and_an_unlit_one_keeps_its_texels(self) -> None:
        # Decisions 20 to 22 through viewport.screenshot's own extraction: under a PixelPerfectCamera of 320 x 180 at
        # scale 3, two built-in white squares, the left one lit; a global light of (0.25, 0.125, 1) lights the lit
        # square's texels to that colour (sRGB 137, 99, 255) and leaves the unlit one white; a point light far away adds
        # nothing.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 0, 10]},
            "Camera": {"Primary": True, "Clear": "Color", "ClearColor": [0, 0, 0]},
            "PixelPerfectCamera": {"ReferenceWidth": 320, "ReferenceHeight": 180, "PixelsPerUnit": 16}}})
        client.call("entity.create", {"name": "Lit", "components": {
            "Transform": {"Translation": [-2, 0, 0]}, "SpriteRenderer": {"Lit": True}}})
        client.call("entity.create", {"name": "Unlit", "components": {
            "Transform": {"Translation": [2, 0, 0]}, "SpriteRenderer": {}}})
        client.call("entity.create", {"name": "Sky", "components": {
            "Light2D": {"Type": "Global", "Color": [0.25, 0.125, 1], "Intensity": 1}}})
        client.call("entity.create", {"name": "Far", "components": {
            "Transform": {"Translation": [100, 100, 0]}, "Light2D": {"Type": "Point", "Radius": 2}}})
        shot = client.call("viewport.screenshot", {"view": "game", "width": 960, "height": 540, "maxDimension": 960})
        width, _, pixels = png_pixels(Path(shot["path"]))

        def pixel(x: int, y: int) -> bytes:
            offset = (y * width + x) * 4
            return pixels[offset:offset + 4]

        # The lit square covers scene pixels [120, 136) x [82, 98), view pixels [360, 408) x [246, 294); the unlit one
        # [552, 600) x [246, 294).
        lit = pixel(384, 270)
        for actual, expected in zip(lit[:3], (137, 99, 255)):
            self.assertLessEqual(abs(actual - expected), 2)
        self.assertEqual(pixel(576, 270), bytes([255, 255, 255, 255]))
        # Without any light the lit square is black, and the unlit one still white.
        client.call("entity.destroy", {"entity": "/Sky"})
        dark = client.call("viewport.screenshot", {"view": "game", "width": 960, "height": 540, "maxDimension": 960})
        width, _, pixels = png_pixels(Path(dark["path"]))
        self.assertEqual(pixel(384, 270), bytes([0, 0, 0, 255]))
        self.assertEqual(pixel(576, 270), bytes([255, 255, 255, 255]))


if __name__ == "__main__":
    unittest.main()
