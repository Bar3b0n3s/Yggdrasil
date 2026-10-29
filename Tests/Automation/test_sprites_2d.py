"""Sprites, sorting, animation, the pixel-perfect camera and the 2D editor mode through automation (Docs/Decisions/
0032-m21-contract.md decisions 3 to 7 and 14; Roadmap M21's sprites and 2D editor mode, R-2D-AUTO)."""

from __future__ import annotations

import unittest
from pathlib import Path

from harness import AutomationTestCase
from test_environment import png_pixels

SKIP_SPRITES = ("contract stub: sprites, sprite animation and the pixel-perfect camera "
                "(ADR 0032 decisions 3 to 7, streams A, B and C)")
SKIP_2D_MODE = "contract stub: the 2D editor mode and 2D picking (ADR 0032 decision 14, streams A and D)"

SHEET = {
    "Texture": "engine://Textures/Checker",
    "Frames": [
        {"Name": "A", "Rect": {"X": 0, "Y": 0, "Width": 8, "Height": 8}},
        {"Name": "B", "Rect": {"X": 8, "Y": 0, "Width": 8, "Height": 8}},
        {"Name": "C", "Rect": {"X": 16, "Y": 0, "Width": 8, "Height": 8}},
    ],
    "Clips": [{"Name": "Spin", "Frames": [{"Frame": 0, "Ticks": 2}, {"Frame": 1, "Ticks": 1}, {"Frame": 2, "Ticks": 1}],
               "Loop": True}],
}


class Sprite2DTests(AutomationTestCase):
    @unittest.skip(SKIP_SPRITES)
    def test_a_sprite_animator_advances_its_sheet_frame_by_ticks(self) -> None:
        # Decision 5 (Roadmap M21 "a tick-driven SpriteAnimator playing SpriteSheet clips"): the SpriteAnimation phase
        # writes the SpriteRenderer's Frame from the clip, one step per clip frame for its ticks, in Play and Simulate.
        client, _ = self.open_editor_with_scene()
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Spin.spritesheet", "values": SHEET})
        client.call("entity.create", {"name": "Spinner", "components": {
            "SpriteRenderer": {"Sheet": "Assets/Sheets/Spin.spritesheet"},
            "SpriteAnimator": {"Clip": "Spin"}}})
        client.call("play.start", {"lockstep": True, "mode": "simulate"})
        frames = []
        for _ in range(5):
            client.call("play.step", {"ticks": 1})
            sprite = client.call("entity.get", {"entity": "/Spinner", "target": "play"})["entity"]["components"]
            frames.append(sprite["SpriteRenderer"]["Frame"])
        self.assertEqual(frames, [0, 0, 1, 2, 0])
        client.call("play.stop")

    @unittest.skip(SKIP_SPRITES)
    def test_a_pixel_perfect_camera_renders_its_reference_resolution(self) -> None:
        # Decision 6: the game view of a PixelPerfectCamera at 320 x 180 in a 960 x 540 view is upscaled by 3 with point
        # sampling: the built-in white square at 16 pixels per unit, centred, covers scene pixels [152, 168) x [82, 98),
        # so its edges fall exactly on 3-pixel boundaries of the view.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 0, 10]},
            "Camera": {"Primary": True, "Clear": "Color", "ClearColor": [0, 0, 0]},
            "PixelPerfectCamera": {"ReferenceWidth": 320, "ReferenceHeight": 180, "PixelsPerUnit": 16}}})
        client.call("entity.create", {"name": "Square", "components": {"SpriteRenderer": {}}})
        shot = client.call("viewport.screenshot", {"view": "game", "width": 960, "height": 540, "maxDimension": 960})
        width, height, pixels = png_pixels(Path(shot["path"]))
        self.assertEqual((width, height), (960, 540))

        def pixel(x: int, y: int) -> bytes:
            offset = (y * width + x) * 4
            return pixels[offset:offset + 4]

        self.assertEqual(pixel(456, 246), bytes([255, 255, 255, 255]))
        self.assertEqual(pixel(503, 293), bytes([255, 255, 255, 255]))
        self.assertEqual(pixel(455, 246), bytes([0, 0, 0, 255]))
        self.assertEqual(pixel(504, 293), bytes([0, 0, 0, 255]))


class Editor2DModeTests(AutomationTestCase):
    @unittest.skip(SKIP_2D_MODE)
    def test_the_2d_mode_looks_down_z_and_picks_the_topmost_sprite(self) -> None:
        # Decision 14: viewport.setOptions {mode2D} turns the scene view's camera into an orthographic one looking down
        # -Z, and viewport.pick reports the sprite drawn last under the pixel, before any mesh: the one in the later
        # sorting layer (decision 4), although it comes first in canonical order and has the lower OrderInLayer.
        client, _ = self.open_editor_with_scene()
        client.call("project.setSettings",
                    {"patch": {"SortingLayers": {"Layers": ["Default", "Background", "Foreground"]}}})
        client.call("entity.create", {"name": "Front", "components": {
            "SpriteRenderer": {"SortingLayer": "Foreground", "OrderInLayer": 0}}})
        client.call("entity.create", {"name": "Back", "components": {
            "SpriteRenderer": {"SortingLayer": "Background", "OrderInLayer": 5}}})
        options = client.call("viewport.setOptions", {"mode2D": True})
        self.assertTrue(options["mode2D"])
        camera = client.call("viewport.camera", {})
        self.assertEqual(camera["projection"], "Orthographic")
        self.assertEqual(camera["position"][0:2], camera["target"][0:2])
        size = client.call("viewport.camera", {"position": [0, 0, 500], "target": [0, 0, 0], "orthographicSize": 2})
        self.assertEqual(size["orthographicSize"], 2)
        picked = client.call("viewport.pick", {"x": 320, "y": 180, "view": "scene"})
        self.assertTrue(picked["raycast"]["hit"])
        self.assertEqual(picked["raycast"]["entity"]["name"], "Front")


if __name__ == "__main__":
    unittest.main()
