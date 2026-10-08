"""The game and camera views of viewport.screenshot, and the exported game's screenshot (Docs/Architecture.md §8.13,
§13.5, §13.11 step 7; Docs/Decisions/0009-m5-decisions.md decision 33 deferred both views to M7; Roadmap M7 acceptance
test_exported_screenshot_matches_editor).

The views render the tiny game (tiny_game.py) through the scene renderer: in Edit mode the edit scene through its primary
camera, while playing the play scene (Docs/Decisions/0012-m7-decisions.md decision 9). The PNG comparison is the C++ one
(compare_png runs Testing/ImageCompare through the Tests executable's comparison entry point). Every test here renders, so
each starts with require_gpu().
"""

from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

from harness import (EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, automation_config, engine_client,
                     tests_executable)
from test_screenshot import png_size
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game,
                       exported_executable)

SCREENSHOT_TICK = 60
RUNTIME_TIMEOUT_SECONDS = 120.0
COMPARE_TIMEOUT_SECONDS = 120.0
GAME_VIEW = {"view": "game", "width": WINDOW_WIDTH, "height": WINDOW_HEIGHT}
# The comparison entry point of the Tests executable (Tests/Source/Engine/Testing/ImageCompareTests.cpp), a ChildTargets
# case run with --no-skip; its --child-argument is {"actual": <path>, "expected": <path>}.
COMPARE_PNGS_TEST_CASE = "ImageCompare: two PNG files match with the golden thresholds (child target)"


def compare_png(actual: Path, expected: Path) -> subprocess.CompletedProcess[bytes]:
    """Compares two PNGs with Testing/ImageCompare's rule and the golden thresholds of §15.4 (more than 0.1 % of the
    pixels off by more than 2/255, or any pixel by more than 24/255, fails), through a process of the build so the C++
    comparison is the one used (the Tests executable of harness.automation_config(), running its comparison entry
    point); exit code 0 when they match, its output naming the differences otherwise."""
    argument = json.dumps({"actual": str(actual), "expected": str(expected)})
    command = [str(tests_executable(automation_config())), "--no-skip",
               f"--test-case={COMPARE_PNGS_TEST_CASE}", f"--child-argument={argument}", "--child-process"]
    return subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=COMPARE_TIMEOUT_SECONDS,
                          check=False)


class GameViewTests(AutomationTestCase):
    """Rendering editors and exported games."""

    def open_tiny_game(self) -> engine_client.EngineClient:
        client = self.connect(self.start_editor(renderer="vulkan"))
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        return client

    def shut_down(self, client: engine_client.EngineClient) -> None:
        """session.shutdown discarding the tests' scene edits, then the exit code: --expect-no-gpu-errors makes any
        validation message fail it."""
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    def test_game_view_renders_the_primary_camera_in_edit_mode(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        edit = client.call("viewport.screenshot", GAME_VIEW)
        self.assertEqual(edit["view"], "Game")
        self.assertEqual(edit["target"], "Edit")
        self.assertEqual(edit["camera"]["path"], "/Camera")
        self.assertEqual(png_size(Path(edit["path"])), (WINDOW_WIDTH, WINDOW_HEIGHT))
        # The game view is the primary camera's, not the scene view's.
        scene = client.call("viewport.screenshot", {**GAME_VIEW, "view": "scene"})
        self.assertNotEqual(Path(scene["path"]).read_bytes(), Path(edit["path"]).read_bytes())

        # Without a primary camera the game view is InvalidState naming the validator code.
        client.call("entity.update", {"entity": "/Camera", "components": {"Camera": {"Primary": False}}})
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "game"})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertIn("SCENE_NO_PRIMARY_CAMERA", raised.exception.detail)
        self.shut_down(client)

    def test_game_view_renders_the_play_scene_while_playing(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 10})
        play = client.call("viewport.screenshot", GAME_VIEW)
        self.assertEqual(play["target"], "Play")
        self.assertEqual(play["camera"]["path"], "/Camera")
        self.assertEqual(png_size(Path(play["path"])), (WINDOW_WIDTH, WINDOW_HEIGHT))
        # An explicit target "edit" still renders the edit scene while playing.
        edit = client.call("viewport.screenshot", {**GAME_VIEW, "target": "edit"})
        self.assertEqual(edit["target"], "Edit")
        client.call("play.stop")
        self.shut_down(client)

    def test_game_view_shows_play_scene_writes_made_since_the_last_tick(self) -> None:
        # A screenshot between ticks renders PlaySession::ExtractView: after play.pause (lockstep) and an entity.update
        # of the play scene, the image shows the new pose, never the WorldTransform of the last tick.
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 2})
        client.call("play.pause")
        before = Path(client.call("viewport.screenshot", GAME_VIEW)["path"]).read_bytes()
        client.call("entity.update", {"entity": "/Cube", "target": "play",
                                      "components": {"Transform": {"Translation": [100, 0, 0]}}})
        after = Path(client.call("viewport.screenshot", GAME_VIEW)["path"]).read_bytes()
        self.assertNotEqual(after, before)
        # The cube left the view: the image is the empty scene's, which the edit scene without its cube also shows.
        client.call("play.stop")
        client.call("entity.update", {"entity": "/Cube", "components": {"Transform": {"Translation": [100, 0, 0]}}})
        empty = Path(client.call("viewport.screenshot", GAME_VIEW)["path"]).read_bytes()
        self.assertEqual(after, empty)
        self.shut_down(client)

    def test_camera_screenshot_renders_an_entity_camera(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("entity.create", {"name": "SideCamera", "components": {
            "Transform": {"Translation": [5, 1, 0], "EulerAngles": [0, 90, 0]}, "Camera": {"Primary": False}}})
        side = client.call("viewport.screenshot", {"view": "game", "camera": "/SideCamera", "width": 160, "height": 90})
        self.assertEqual(side["camera"]["path"], "/SideCamera")
        self.assertEqual(png_size(Path(side["path"])), (160, 90))
        for camera, code in (("/Cube", engine_client.INVALID_PARAMS), ("/Nobody", engine_client.NOT_FOUND)):
            with self.subTest(camera=camera):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("viewport.screenshot", {"view": "game", "camera": camera})
                self.assert_engine_error(raised.exception, code)
                self.assertEqual(raised.exception.issues[0]["pointer"], "/camera")
        self.shut_down(client)

    def test_compare_png_matches_equal_views_and_reports_different_ones(self) -> None:
        # The helper test_exported_screenshot_matches_editor relies on: two renders of one view match, the views of two
        # cameras do not, and the output names the differences.
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        first = Path(client.call("viewport.screenshot", GAME_VIEW)["path"])
        second = Path(client.call("viewport.screenshot", GAME_VIEW)["path"])
        client.call("entity.create", {"name": "Above", "components": {
            "Transform": {"Translation": [0, 6, 0.01], "EulerAngles": [-89, 0, 0]}, "Camera": {"Primary": False}}})
        other = Path(client.call("viewport.screenshot", {**GAME_VIEW, "camera": "/Above"})["path"])
        self.shut_down(client)

        matched = compare_png(second, first)
        self.assertEqual(matched.returncode, EXIT_SUCCESS, matched.stdout.decode("utf-8", errors="replace"))
        differed = compare_png(other, first)
        output = differed.stdout.decode("utf-8", errors="replace")
        self.assertNotEqual(differed.returncode, EXIT_SUCCESS, output)
        self.assertIn("differ", output)

    def test_exported_screenshot_matches_editor(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": SCREENSHOT_TICK, "render": "last"})
        editor_shot = Path(client.call("viewport.screenshot", {**GAME_VIEW, "maxDimension": 8192})["path"])
        client.call("play.stop")
        executable = exported_executable(export_tiny_game(client))
        self.shut_down(client)

        runtime_shot = self.directory / "Runtime.png"
        command = [str(executable), "--headless", "--frames", str(SCREENSHOT_TICK + 1), *GPU_EDITOR_ARGUMENTS,
                   "--screenshot-at", f"{SCREENSHOT_TICK}:{runtime_shot}", f"--user-data-dir={self.directory / 'Game'}"]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                   timeout=RUNTIME_TIMEOUT_SECONDS, check=False)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr.decode("utf-8", errors="replace"))
        self.assertEqual(png_size(runtime_shot), (WINDOW_WIDTH, WINDOW_HEIGHT))
        compared = compare_png(runtime_shot, editor_shot)
        self.assertEqual(compared.returncode, EXIT_SUCCESS, compared.stdout.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    unittest.main()
