"""Headless editor camera and viewport preferences through the real host."""

from __future__ import annotations

import json
import math
import subprocess
import unittest
from typing import Any

from harness import EXIT_SUCCESS, AutomationTestCase, engine_client
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game,
                       exported_executable, exported_manifest)

# What the headless editor's Game view reports before any image is rendered (EditorViewportState's default extent).
HEADLESS_GAME_VIEW = (640, 360)

# A behaviour whose state depends on the view: each tick it moves its entity along the camera ray through the cursor, by
# a distance that depends on the view's width (PlaySession.h, view coordinates).
POINTER_SCRIPT = """local Pointer = {}

function Pointer.OnFixedUpdate(self: any, dt: number)
	local holder = Scene.FindByName("Camera")
	local camera = if holder then holder:GetComponent("Camera") else nil
	if camera then
		local mouse = Input.GetMousePosition()
		local origin, direction = camera:ScreenToWorldRay(mouse.x, mouse.y)
		local size = Application.GetWindowSize()
		self.Entity.Transform.Translation = origin + direction * (4 + size.x / 160)
	end
end

return Script.Define("Pointer", Pointer)
"""


def evaluate(client: engine_client.EngineClient, code: str) -> Any:
    """The value of a play-context script.eval."""
    return client.call("script.eval", {"context": "play", "code": code})["value"]


def view_contract(width: int, height: int) -> str:
    """Luau that is true when scripts see a width x height view: the window size, the cursor injected at the centre
    pixel and the camera ray through it, which must be the primary camera's forward direction."""
    return (f"local mouse = Input.GetMousePosition(); "
            f"local origin, direction = "
            f"Scene.FindByName('Camera'):GetComponent('Camera'):ScreenToWorldRay(mouse.x, mouse.y); "
            f"return Application.GetWindowSize() == vector.create({width}, {height}, 0) "
            f"and mouse == vector.create({width / 2 - 0.5}, {height / 2 - 0.5}, 0) "
            f"and vector.magnitude(direction - Scene.FindByName('Camera').Transform:Forward()) < 0.0001")


class EditorViewportTests(AutomationTestCase):
    """Viewport session changes preserve scene content, history and revision."""

    def test_viewport_camera_get_set_and_invalid_pose(self) -> None:
        client, _ = self.open_editor_with_scene()
        initial = client.call("viewport.camera")
        self.assertEqual(initial["position"], [0, 3, 10])
        self.assertEqual(initial["target"], [0, 0, 0])
        moved = client.call("viewport.camera", {"position": [4, 5, 9]})
        self.assertEqual(moved["position"], [4, 5, 9])
        self.assertEqual(moved["target"], initial["target"])
        aimed = client.call("viewport.camera", {"target": [1, 2, 3]})
        self.assertEqual(aimed["position"], moved["position"])
        self.assertEqual(aimed["target"], [1, 2, 3])
        for target in ([4, 5, 9], [4, 8, 9]):
            with self.assertRaises(engine_client.EngineError) as refused:
                client.call("viewport.camera", {"target": target})
            self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
            unchanged = client.call("viewport.camera")
            self.assertEqual(unchanged["position"], aimed["position"])
            self.assertEqual(unchanged["target"], aimed["target"])

    def test_viewport_frame_fits_entities(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Left", "components": {"Transform": {"Translation": [-8, -2, 1]}}})
        client.call("entity.create", {"name": "Right", "components": {"Transform": {"Translation": [12, 6, 3]}}})
        before = client.call("viewport.camera")
        framed = client.call("viewport.frame", {"entities": ["/Left", "/Right"]})
        self.assertEqual(framed["target"], [2, 2, 2])
        self.assertEqual({entity["name"] for entity in framed["entities"]}, {"Left", "Right"})
        distance = math.dist(framed["position"], framed["target"])
        self.assertGreater(distance, 1)
        forward = [(b - a) / distance for a, b in zip(framed["position"], framed["target"], strict=True)]
        horizontal_length = math.hypot(forward[0], forward[2])
        right = [-forward[2] / horizontal_length, 0, forward[0] / horizontal_length]
        up = [right[1] * forward[2] - right[2] * forward[1],
              right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0]]
        for point in ([-8, -2, 1], [12, 6, 3]):
            offset = [a - b for a, b in zip(point, framed["position"], strict=True)]
            depth = sum(a * b for a, b in zip(offset, forward, strict=True))
            self.assertGreater(depth, 0)
            x = abs(sum(a * b for a, b in zip(offset, right, strict=True)))
            y = abs(sum(a * b for a, b in zip(offset, up, strict=True)))
            self.assertLessEqual(x, depth * math.tan(math.radians(30)) * 16 / 9 / 1.1 + 1e-4)
            self.assertLessEqual(y, depth * math.tan(math.radians(30)) / 1.1 + 1e-4)
        self.assertNotEqual(framed["position"], before["position"])
        for entities in ([], ["/Left", "/Missing"]):
            with self.assertRaises(engine_client.EngineError):
                client.call("viewport.frame", {"entities": entities})
            unchanged = client.call("viewport.camera")
            self.assertEqual(unchanged["position"], framed["position"])
            self.assertEqual(unchanged["target"], framed["target"])

    def test_viewport_set_options_preserves_unspecified_members(self) -> None:
        client, _ = self.open_editor_with_scene()
        expected = {"grid": True, "gizmos": True, "colliders": False, "icons": True, "wireframe": False}
        defaults = client.call("viewport.setOptions")
        for key, value in expected.items():
            self.assertEqual(defaults[key], value)
        for key in expected:
            expected[key] = not expected[key]
            changed = client.call("viewport.setOptions", {key: expected[key]})
            for member, value in expected.items():
                self.assertEqual(changed[member], value)
        with self.assertRaises(engine_client.EngineError):
            client.call("viewport.setOptions", {"grid": "invalid"})
        unchanged = client.call("viewport.setOptions")
        for key, value in expected.items():
            self.assertEqual(unchanged[key], value)

    def test_viewport_camera_projection_and_clip_range(self) -> None:
        client, _ = self.open_editor_with_scene()
        initial = client.call("viewport.camera")
        self.assertEqual(initial["projection"], "Perspective")
        self.assertEqual(initial["verticalFov"], 60)
        self.assertAlmostEqual(initial["nearClip"], 0.1, places=6)
        self.assertEqual(initial["farClip"], 1000)
        # The scene view's Orthographic toggle, observed and set through automation.
        orthographic = client.call("viewport.camera", {"projection": "orthographic", "orthographicSize": 6,
                                                       "nearClip": 0.5, "farClip": 300})
        self.assertEqual(orthographic["projection"], "Orthographic")
        self.assertEqual(orthographic["orthographicSize"], 6)
        self.assertEqual(orthographic["nearClip"], 0.5)
        self.assertEqual(orthographic["farClip"], 300)
        self.assertEqual(orthographic["position"], initial["position"])
        self.assertEqual(client.call("viewport.camera"), orthographic)
        for params in ({"verticalFov": 0}, {"verticalFov": 180}, {"orthographicSize": -1}, {"nearClip": 400},
                       {"farClip": 0.25}, {"projection": "Fisheye"},
                       {"position": [0.000005, 10, 0], "target": [0, 0, 0]}):
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("viewport.camera", params)
                self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
                self.assertEqual(client.call("viewport.camera"), orthographic)
        restored = client.call("viewport.camera", {"projection": "Perspective", "nearClip": 0.1, "farClip": 1000})
        self.assertEqual(restored["projection"], "Perspective")
        self.assertEqual(restored["farClip"], 1000)

    def test_viewport_set_options_debug_view_and_game_resolution(self) -> None:
        client, _ = self.open_editor_with_scene()
        defaults = client.call("viewport.setOptions")
        self.assertEqual(defaults["debugView"], "Lit")
        self.assertEqual(defaults["gameResolution"], [0, 0])
        changed = client.call("viewport.setOptions", {"debugView": "shadowcascades", "gameResolution": [1280, 720]})
        self.assertEqual(changed["debugView"], "ShadowCascades")
        self.assertEqual(changed["gameResolution"], [1280, 720])
        self.assertEqual(changed["grid"], defaults["grid"])
        for params in ({"debugView": "Sepia"}, {"gameResolution": [1280]}, {"gameResolution": [0, 720]},
                       {"gameResolution": [9000, 720]}, {"grid": False, "debugView": "Unknown"}):
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("viewport.setOptions", params)
                self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
                self.assertEqual(client.call("viewport.setOptions"), changed)
        free = client.call("viewport.setOptions", {"gameResolution": [0, 0], "debugView": "Lit"})
        self.assertEqual(free["gameResolution"], [0, 0])
        self.assertEqual(free["debugView"], "Lit")

    def test_scripts_measure_the_game_view_in_the_editor_and_the_exported_runtime(self) -> None:
        # One coordinate contract (PlaySession.h, view coordinates): window size, cursor and camera rays are measured in
        # the session's view, never the editor window: ordinary play's is the Game view's image, a lockstep session's
        # the project's window size, as in the exported game.
        editor = self.connect(self.start_editor())
        self.create_project(editor, TINY_GAME_NAME)
        build_tiny_game(editor)
        width, height = HEADLESS_GAME_VIEW
        self.assertNotEqual((width, height), (WINDOW_WIDTH, WINDOW_HEIGHT))
        for start, view in (({"paused": True}, HEADLESS_GAME_VIEW),
                            ({"lockstep": True}, (WINDOW_WIDTH, WINDOW_HEIGHT))):
            with self.subTest(start=start):
                editor.call("play.start", start)
                editor.call("input.inject", {"events": [{"type": "mouseMove",
                                                         "position": [view[0] / 2 - 0.5, view[1] / 2 - 0.5]}]})
                editor.call("play.step", {"ticks": 1, "render": "none"})
                self.assertTrue(evaluate(editor, view_contract(*view)))
                editor.call("play.stop")
        executable = exported_executable(export_tiny_game(editor))
        editor.call("session.shutdown", {"force": True})
        self.editors[-1].wait()

        process = engine_client.launch_editor(executable,
                                               ["--headless", "--renderer", "none", "--automation", "--paused"],
                                               self.directory / "RuntimeView", TINY_GAME_NAME)
        self.editors.append(process)
        runtime = self.connect(process)
        # The exported game's view is its window's framebuffer, the project's window size.
        runtime.call("input.inject", {"events": [{"type": "mouseMove",
                                                  "position": [WINDOW_WIDTH / 2 - 0.5, WINDOW_HEIGHT / 2 - 0.5]}]})
        runtime.call("play.step", {"ticks": 1, "render": "none"})
        self.assertTrue(evaluate(runtime, view_contract(WINDOW_WIDTH, WINDOW_HEIGHT)))
        runtime.call("session.shutdown")

    def test_view_dependent_recording_reproduces_in_replays_test_runs_and_the_exported_runtime(self) -> None:
        # Recording, replay and test-run sessions keep the project's window size in every host, never the Game view's
        # image (640x360 in this headless editor), so a replay whose gameplay reads the view reproduces its hash
        # anywhere.
        editor = self.connect(self.start_editor())
        self.create_project(editor, TINY_GAME_NAME)
        build_tiny_game(editor)
        written = editor.call("script.write", {"path": "Assets/Scripts/Pointer.luau", "source": POINTER_SCRIPT})
        self.assertEqual(written["diagnostics"], [], written)
        editor.call("entity.update", {"entity": "/Cube", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Pointer.luau"}]}}})
        editor.call("scene.save")
        editor.call("input.record", {"action": "start"})
        editor.call("input.inject", {"events": [{"type": "mouseMove", "position": [80, 45]}]})
        editor.call("play.step", {"ticks": 3, "render": "none"})
        self.assertTrue(evaluate(editor,
                                 f"Application.GetWindowSize() == vector.create({WINDOW_WIDTH}, {WINDOW_HEIGHT}, 0)"))
        path = "Assets/Replays/Pointer.replay"
        saved = editor.call("input.record", {"action": "stop", "path": path})
        editor.call("play.stop")

        replayed = editor.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertEqual(replayed["stateHash"], saved["stateHash"])
        editor.call("play.stop")
        editor.call("project.setSettings", {"patch": {"Testing": {"Replays": [path]}}})
        tested = engine_client.load_offloaded(editor.call("test.run", timeout=120.0))
        self.assertTrue(tested["passed"], tested)
        self.assertEqual([suite["finalStateHash"] for suite in tested["suites"]], [saved["stateHash"]])

        executable = exported_executable(export_tiny_game(editor))
        editor.call("session.shutdown", {"force": True})
        self.editors[-1].wait()
        # The exported game's window is the project's (framebuffer = view), then twice as large, as a scaled (Retina)
        # display's framebuffer is: the replay keeps the project's window size either way.
        manifest_path = exported_manifest(executable)
        for scale in (1, 2):
            with self.subTest(framebuffer_scale=scale):
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest["Window"]["Width"], manifest["Window"]["Height"] = WINDOW_WIDTH * scale, WINDOW_HEIGHT * scale
                manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
                runtime = subprocess.run(
                    [str(executable), "--headless", "--renderer", "none", "--replay", path, "--verify",
                     f"--user-data-dir={self.directory / f'RuntimeReplay{scale}'}"],
                    cwd=self.directory, stdin=subprocess.DEVNULL, capture_output=True, timeout=120.0, check=False)
                output = runtime.stderr.decode("utf-8", errors="replace")
                self.assertEqual(runtime.returncode, EXIT_SUCCESS, output)
                self.assertIn(saved["stateHash"], output)

    def test_viewport_controls_without_renderer(self) -> None:
        client, _ = self.open_editor_with_scene(renderer="none")
        client.call("entity.create", {"name": "Focus"})
        client.call("scene.save")
        before = client.call("session.info")["_meta"]
        history = client.call("edit.history")["entries"]
        client.call("viewport.camera", {"position": [5, 4, 8], "target": [1, 0, 0]})
        client.call("viewport.frame", {"entities": ["/Focus"]})
        client.call("viewport.setOptions", {"wireframe": True, "grid": False})
        after = client.call("session.info")["_meta"]
        self.assertEqual(after["revision"], before["revision"])
        self.assertFalse(after["dirty"])
        self.assertEqual(client.call("edit.history")["entries"], history)


if __name__ == "__main__":
    unittest.main()
