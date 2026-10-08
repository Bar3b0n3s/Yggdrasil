"""The game and camera views of viewport.screenshot, and the exported game's screenshot (Docs/Architecture.md §8.13,
§13.5, §13.11 step 7; Docs/Decisions/0009-m5-decisions.md decision 33 deferred both views to M7; Roadmap M7 acceptance
test_exported_screenshot_matches_editor).

Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 9): stream B implements the views and
the scene renderer and removes the skips; the comparison needs stream C's Runtime and stream D's exporter. Every test
here renders, so each starts with require_gpu().
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from test_screenshot import png_size
from tiny_game import (TINY_GAME_NAME, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game,
                       exported_executable)

SCREENSHOT_TICK = 60
RUNTIME_TIMEOUT_SECONDS = 120.0
GAME_VIEW = {"view": "game", "width": WINDOW_WIDTH, "height": WINDOW_HEIGHT}


def compare_png(repository: Path, actual: Path, expected: Path) -> subprocess.CompletedProcess[bytes]:
    """Compares two PNGs with Testing/ImageCompare's rule and the golden thresholds of §15.4 (more than 0.1 % of the
    pixels off by more than 2/255, or any pixel by more than 24/255, fails), through a process of the build in
    `repository` so the C++ comparison is the one used (stream B chooses the entry point, ADR 0012 decision 9); exit
    code 0 when they match, its output naming the differences otherwise."""
    raise NotImplementedError("contract stub: the PNG comparison helper lands with M7 stream B")


class GameViewTests(AutomationTestCase):
    """Rendering editors and exported games."""

    def open_tiny_game(self) -> engine_client.EngineClient:
        client = self.connect(self.start_editor(renderer="vulkan"))
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        return client

    @unittest.skip("contract stub: un-skipped by M7 stream B")
    def test_game_view_renders_the_primary_camera_in_edit_and_play(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        edit = client.call("viewport.screenshot", GAME_VIEW)
        self.assertEqual(edit["view"], "Game")
        self.assertEqual(edit["target"], "Edit")
        self.assertEqual(edit["camera"]["path"], "/Camera")
        self.assertEqual(png_size(Path(edit["path"])), (WINDOW_WIDTH, WINDOW_HEIGHT))

        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 10})
        play = client.call("viewport.screenshot", GAME_VIEW)
        self.assertEqual(play["target"], "Play")
        client.call("play.stop")

        # Without a primary camera the game view is InvalidState naming the validator code.
        client.call("entity.update", {"entity": "/Camera", "components": {"Camera": {"Primary": False}}})
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "game"})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertIn("SCENE_NO_PRIMARY_CAMERA", raised.exception.detail)
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    @unittest.skip("contract stub: un-skipped by M7 stream B")
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
        client.call("session.shutdown")

    @unittest.skip("contract stub: un-skipped by M7 stream B")
    def test_camera_screenshot_renders_an_entity_camera(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("entity.create", {"name": "SideCamera", "components": {
            "Transform": {"Translation": [5, 1, 0], "EulerAngles": [0, 90, 0]}, "Camera": {"Primary": False}}})
        side = client.call("viewport.screenshot", {"view": "game", "camera": "/SideCamera", "width": 160, "height": 90})
        self.assertEqual(side["camera"]["path"], "/SideCamera")
        for camera, code in (("/Cube", engine_client.INVALID_PARAMS), ("/Nobody", engine_client.NOT_FOUND)):
            with self.subTest(camera=camera):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("viewport.screenshot", {"view": "game", "camera": camera})
                self.assert_engine_error(raised.exception, code)
                self.assertEqual(raised.exception.issues[0]["pointer"], "/camera")
        client.call("session.shutdown")

    @unittest.skip("contract stub: un-skipped by M7 stream B")
    def test_exported_screenshot_matches_editor(self) -> None:
        if not self.require_gpu():
            return
        client = self.open_tiny_game()
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": SCREENSHOT_TICK, "render": "last"})
        editor_shot = Path(client.call("viewport.screenshot", {**GAME_VIEW, "maxDimension": 8192})["path"])
        client.call("play.stop")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.editors[-1].wait()

        runtime_shot = self.directory / "Runtime.png"
        command = [str(executable), "--headless", "--frames", str(SCREENSHOT_TICK + 1), *GPU_EDITOR_ARGUMENTS,
                   "--screenshot-at", f"{SCREENSHOT_TICK}:{runtime_shot}", f"--user-data-dir={self.directory / 'Game'}"]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                   timeout=RUNTIME_TIMEOUT_SECONDS, check=False)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr.decode("utf-8", errors="replace"))
        self.assertEqual(png_size(runtime_shot), (WINDOW_WIDTH, WINDOW_HEIGHT))
        compared = compare_png(Path(__file__).resolve().parents[2], runtime_shot, editor_shot)
        self.assertEqual(compared.returncode, EXIT_SUCCESS, compared.stdout.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    unittest.main()
