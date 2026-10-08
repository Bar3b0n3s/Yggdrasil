"""The screenshot methods (Docs/Architecture.md §8.13, §13.5; Roadmap M5 and M7; Docs/Decisions/0009-m5-decisions.md
decisions 16 and 33, 0012-m7-decisions.md decision 9): viewport.screenshot renders a view of the target scene afresh
through the scene renderer (the editor's scene view, the game view through the primary camera, or a camera entity) and
editor.screenshot the whole editor UI on GLFW's null platform; both write a PNG into the project's output directory and
name its path. The game and camera views of a rendering editor are covered by test_game_view.py.

The tests that render start a headless editor with the Vulkan renderer and the GPU test options (harness.require_gpu);
the parameter checks, the view and camera errors and the --renderer none answers need no GPU. The golden images
"LitScene" (a scene through the viewport capture) and "ImGuiDemo" (the editor.screenshot of a --batch run) are compared by
the C++ golden suite (Tests/Source/Golden/GoldenTests.cpp).
"""

from __future__ import annotations

import base64
import struct
import unittest
from pathlib import Path

from harness import EXIT_SUCCESS, AutomationTestCase, engine_client

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_size(path: Path) -> tuple[int, int]:
    """The width and height of a PNG file, from its IHDR chunk (the first one, right after the signature)."""
    data = path.read_bytes()
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        raise AssertionError(f"{path} is not a PNG file")
    width, height = struct.unpack(">II", data[16:24])
    return width, height


class ScreenshotParamsTests(AutomationTestCase):
    """What every editor checks before it renders, a --renderer none editor included."""

    def test_screenshots_need_a_renderer(self) -> None:
        client, _ = self.open_editor_with_scene()
        for method, params in (("viewport.screenshot", {"view": "scene"}), ("editor.screenshot", {})):
            with self.subTest(method=method):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call(method, params)
                self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
                self.assertIn("--renderer none", raised.exception.detail)

    def test_viewport_screenshot_refuses_members_of_later_milestones_at_their_pointer(self) -> None:
        # M9's debug views and annotations (Docs/Decisions/0013-m8-decisions.md decision 12).
        client, _ = self.open_editor_with_scene()
        refused = [
            ({"view": "scene", "debugView": "AO"}, "/debugView"),
            ({"view": "game", "debugView": "ShadowCascades"}, "/debugView"),
            ({"view": "scene", "debugView": "Overdraw"}, "/debugView"),
            ({"view": "scene", "annotate": {"labels": "all"}}, "/annotate"),
        ]
        for params, pointer in refused:
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("viewport.screenshot", params)
                self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)

    def test_game_view_and_camera_errors_come_before_rendering(self) -> None:
        # The view's scene and camera are resolved before anything renders, so a --renderer none editor reports them.
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "game"})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertIn("SCENE_NO_PRIMARY_CAMERA", raised.exception.detail)

        client.call("entity.create", {"name": "Cube"})
        for camera, code in (("/Cube", engine_client.INVALID_PARAMS), ("/Nobody", engine_client.NOT_FOUND),
                             ("", engine_client.INVALID_PARAMS)):
            with self.subTest(camera=camera):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("viewport.screenshot", {"view": "game", "camera": camera})
                self.assert_engine_error(raised.exception, code)
                self.assertEqual(raised.exception.issues[0]["pointer"], "/camera")

        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "scene", "target": "play"})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertEqual(raised.exception.issues[0]["pointer"], "/target")

        # With a primary camera the game view gets as far as rendering, which needs a renderer.
        client.call("entity.create", {"name": "Camera", "components": {"Camera": {"Primary": True}}})
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "game"})
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
        self.assertIn("--renderer none", raised.exception.detail)

    def test_screenshot_params_are_validated(self) -> None:
        client, _ = self.open_editor_with_scene()
        invalid = [
            ("viewport.screenshot", {}),
            ("viewport.screenshot", {"view": "scene", "width": 0}),
            ("viewport.screenshot", {"view": "scene", "height": 8193}),
            ("viewport.screenshot", {"view": "top"}),
            ("viewport.screenshot", {"view": "scene", "zoom": 2}),
            ("viewport.screenshot", {"view": "scene", "target": "both"}),
            ("editor.screenshot", {"maxDimension": 0}),
        ]
        for method, params in invalid:
            with self.subTest(method=method, params=params):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call(method, params)
                self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)


class ScreenshotRenderingTests(AutomationTestCase):
    """Screenshots of a rendering editor."""

    def shut_down(self, client: engine_client.EngineClient) -> None:
        """session.shutdown, then the exit code: --expect-no-gpu-errors makes any validation message fail it."""
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    def test_viewport_screenshot_writes_the_scene_view_as_png(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        output = root / "Library" / "Automation" / "Out"

        shot = client.call("viewport.screenshot", {"view": "scene", "width": 320, "height": 180})
        path = Path(shot["path"])
        self.assertEqual(path.parent, output)
        self.assertEqual(path.suffix, ".png")
        self.assertEqual(shot["view"], "Scene")
        # The editor's scene-view camera is no entity: the camera summary is empty.
        self.assertEqual(shot["target"], "Edit")
        self.assertEqual(shot["camera"], {"id": "", "name": "", "path": ""})
        self.assertEqual(shot["mimeType"], "image/png")
        self.assertEqual((shot["width"], shot["height"]), (320, 180))
        self.assertEqual(png_size(path), (320, 180))
        self.assertEqual(shot["data"], "")

        # maxDimension downscales; inline returns the same PNG as base64; enum spellings are echoed canonically.
        small = client.call("viewport.screenshot", {"view": "SCENE", "maxDimension": 160, "inline": True})
        self.assertEqual(small["view"], "Scene")
        self.assertEqual((small["width"], small["height"]), (160, 90))
        self.assertEqual(base64.b64decode(small["data"], validate=True), Path(small["path"]).read_bytes())
        self.assertNotEqual(small["path"], shot["path"])

        # The scene view shows the scene: a cube in front of the scene-view camera changes the image, and every render of
        # one state is identical (no temporal effects, §8.3).
        empty = Path(client.call("viewport.screenshot", {"view": "scene", "width": 160, "height": 90})["path"]).read_bytes()
        client.call("entity.create", {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        cube = Path(client.call("viewport.screenshot", {"view": "scene", "width": 160, "height": 90})["path"]).read_bytes()
        again = Path(client.call("viewport.screenshot", {"view": "scene", "width": 160, "height": 90})["path"]).read_bytes()
        self.assertNotEqual(cube, empty)
        self.assertEqual(again, cube)
        client.call("scene.save")
        self.shut_down(client)

    @unittest.skip("contract stub: un-skipped by M8 stream A")
    def test_viewport_screenshot_renders_debug_views(self) -> None:
        # M8 (§8.5; ADR 0009 decision 33 deferred debugView here; Docs/Decisions/0013-m8-decisions.md decision 12): each
        # debug view renders, the names ignore case, an empty name is Lit, and an unknown one is InvalidParams.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Cube", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        size = {"view": "scene", "width": 160, "height": 90}

        def shoot(view: str | None) -> bytes:
            params = size if view is None else {**size, "debugView": view}
            return Path(client.call("viewport.screenshot", params)["path"]).read_bytes()

        lit = shoot(None)
        self.assertEqual(shoot(""), lit)
        self.assertEqual(shoot("LIT"), lit)
        images = {}
        for view in ("Albedo", "Normals", "Roughness", "Metallic", "Emissive"):
            with self.subTest(view=view):
                images[view] = shoot(view)
                self.assertNotEqual(images[view], lit)
        self.assertNotEqual(images["Albedo"], images["Normals"])
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {**size, "debugView": "Wireframe"})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(raised.exception.issues[0]["pointer"], "/debugView")
        self.shut_down(client)

    def test_editor_screenshot_writes_the_editor_ui_as_png(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")

        # The headless editor's window is 1600x900 (the default WindowSpecification); maxDimension defaults to 1024.
        shot = client.call("editor.screenshot")
        self.assertEqual(shot["mimeType"], "image/png")
        self.assertEqual((shot["width"], shot["height"]), (1024, 576))
        self.assertEqual(png_size(Path(shot["path"])), (1024, 576))
        self.assertEqual(Path(shot["path"]).parent, root / "Library" / "Automation" / "Out")

        full = client.call("editor.screenshot", {"maxDimension": 4096})
        self.assertEqual((full["width"], full["height"]), (1600, 900))
        self.assertEqual(png_size(Path(full["path"])), (1600, 900))
        self.shut_down(client)


if __name__ == "__main__":
    unittest.main()
