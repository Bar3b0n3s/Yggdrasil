"""Sprites, sorting, animation, the pixel-perfect camera and the 2D editor mode through automation (Docs/Decisions/
0032-m21-contract.md decisions 3 to 7 and 14; Roadmap M21's sprites and 2D editor mode, R-2D-AUTO)."""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client
from test_environment import png_pixels

# engine://Textures/Checker (64 x 64): a sheet's Texture takes any asset reference (W21.1's review), here an engine
# path.
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
    def test_a_sprite_animator_advances_its_sheet_frame_by_ticks(self) -> None:
        # Decision 5 (Roadmap M21 "a tick-driven SpriteAnimator playing SpriteSheet clips"): the SpriteAnimation phase
        # writes the SpriteRenderer's Frame from the clip, one step per clip frame for its ticks; the phase runs in Play
        # and Simulate alike, and this drives a Simulate session.
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

    def test_a_sprite_sheet_takes_its_texture_by_any_asset_reference(self) -> None:
        # Architecture §7.1 (W21.1's review): asset.create and asset.setProperties take a sheet's Texture as a handle, a
        # project path or an engine path, as every AssetRef param, and write the handle it names; one that names
        # nothing is refused at the Texture.
        client, _ = self.open_editor_with_scene()
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Spin.spritesheet", "values": SHEET})
        created = client.call("asset.getProperties", {"asset": "Assets/Sheets/Spin.spritesheet"})
        self.assertEqual(created["values"]["Texture"], "0000000000000184")
        # The Missing checker (64 x 64) holds the sheet's frames as the Checker does.
        patched = client.call("asset.setProperties", {
            "asset": "Assets/Sheets/Spin.spritesheet", "values": {"Texture": "engine://Textures/Missing"}})
        self.assertEqual(patched["values"]["Texture"], "0000000000000185")
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("asset.setProperties", {
                "asset": "Assets/Sheets/Spin.spritesheet", "values": {"Texture": "Assets/Textures/Missing.png"}})
        self.assertEqual(raised.exception.data["errorCode"], "NotFound")
        self.assertEqual(raised.exception.data["location"]["pointer"], "/values/Texture")

    def test_a_sprite_sheet_stores_its_texture_uncompressed(self) -> None:
        # ADR 0028's phase-2 ledger and Docs/Decisions/0034-m23-contract.md decision 13 (M23's landing): asset.create
        # and asset.setProperties of a sprite sheet set its imported Texture's Compression to None in the sheet's own
        # undo step, so sprite art is never stored as BC blocks.
        client, root = self.open_editor_with_scene()
        (root / "Assets" / "Textures").mkdir(parents=True, exist_ok=True)
        shutil.copy(REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Textures" / "Rgba.png",
                    root / "Assets" / "Textures" / "Hero.png")
        client.call("project.refreshAssets")

        def compression() -> str:
            settings = client.call("asset.getImportSettings", {"asset": "Assets/Textures/Hero.png"})["settings"]
            return settings["Compression"]

        self.assertEqual(compression(), "Auto")
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Hero.spritesheet", "values": {
            "Texture": "Assets/Textures/Hero.png", "Frames": [{"Rect": {"X": 0, "Y": 0, "Width": 8, "Height": 8}}]}})
        self.assertEqual(compression(), "None")
        client.call("edit.undo")
        self.assertFalse((root / "Assets" / "Sheets" / "Hero.spritesheet").exists())
        self.assertEqual(compression(), "Auto")

        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Empty.spritesheet"})
        self.assertEqual(compression(), "Auto")
        patched = client.call("asset.setProperties", {
            "asset": "Assets/Sheets/Empty.spritesheet", "values": {"Texture": "Assets/Textures/Hero.png"}})
        self.assertNotEqual(patched["undoIndex"], 0)
        self.assertEqual(compression(), "None")

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
    def test_the_2d_mode_makes_a_2d_camera_that_frames_in_the_plane(self) -> None:
        # Decision 14: viewport.setOptions {mode2D} is the toolbar's 2D toggle. Turning it on makes the scene camera
        # orthographic, looking down -Z at its target's X and Y; framing keeps a 2D camera; turning it off keeps the
        # camera.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Hero", "components": {
            "Transform": {"Translation": [3, -1, 2]}, "SpriteRenderer": {}}})
        self.assertFalse(client.call("viewport.setOptions")["mode2D"])
        client.call("viewport.camera", {"position": [4, 3, 10], "target": [4, 1, 2]})
        self.assertTrue(client.call("viewport.setOptions", {"mode2D": True})["mode2D"])
        camera = client.call("viewport.camera")
        self.assertEqual(camera["projection"], "Orthographic")
        self.assertEqual(camera["position"], [4, 1, 500])
        self.assertEqual(camera["target"], [4, 1, 0])
        framed = client.call("viewport.frame", {"entities": ["/Hero"]})
        self.assertEqual(framed["target"], [3, -1, 0])
        self.assertEqual(framed["position"], [3, -1, 500])
        self.assertFalse(client.call("viewport.setOptions", {"mode2D": False})["mode2D"])
        kept = client.call("viewport.camera")
        self.assertEqual(kept["projection"], "Orthographic")
        self.assertEqual(kept["position"], [3, -1, 500])

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
