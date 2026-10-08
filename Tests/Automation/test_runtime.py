"""The Runtime executable and exported games (Docs/Architecture.md §14.1, §14.3, §13.5 "Runtime subset", §13.9; Roadmap
M7 acceptance): the manifest's name for the user-data folder, headless runs that log no error, the missing-manifest exit
code, and the automation subset served by an exported build.

Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md decisions 10 and 12): stream C implements the
Runtime (manifest, paks, play session, rendering, --automation, --paused) and removes the skips; the exports come from
stream D's project.export.
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

from harness import (EXIT_INIT_FAILED, EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, app_name,
                     automation_config, engine_client, runtime_executable)
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game, exported_executable

RUNTIME_TIMEOUT_SECONDS = 120.0
# The ticks the Runtime's automation test steps, in the editor and in the exported game.
STEPPED_TICKS = 30


class RuntimeTests(AutomationTestCase):
    """Exported builds of the tiny game, and the bare Runtime."""

    def export_game(self, lockstep_ticks: int = 0) -> tuple[Path, str]:
        """Exports the tiny game (a fresh project) and returns its executable; the exporting editor is shut down first.
        With `lockstep_ticks`, the editor first plays the game in lockstep with its default seed for that many ticks and
        the state hash it reached is returned too (empty otherwise)."""
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        state_hash = ""
        if lockstep_ticks:
            client.call("play.start", {"lockstep": True})
            state_hash = client.call("play.step", {"ticks": lockstep_ticks, "render": "none"})["stateHash"]
            client.call("play.stop")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.editors[-1].wait()
        return executable, state_hash

    def run_game(self, executable: Path, arguments: list[str],
                 user_data: Path) -> subprocess.CompletedProcess[bytes]:
        command = [str(executable), *arguments, f"--user-data-dir={user_data}"]
        return subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=RUNTIME_TIMEOUT_SECONDS, check=False)

    @unittest.skip("contract stub: un-skipped by M7 stream C")
    def test_exported_user_data_folder_uses_manifest_name(self) -> None:
        executable, _ = self.export_game()
        user_data = self.directory / "GameUserData"
        completed = self.run_game(executable, ["--headless", "--renderer", "none", "--frames", "5"], user_data)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr.decode("utf-8", errors="replace"))
        # Logs, crash reports and user:// live under the manifest's Name, never under the engine's product name.
        self.assertTrue((user_data / TINY_GAME_NAME / "Logs").is_dir())
        self.assertFalse((user_data / app_name()).exists())

    @unittest.skip("contract stub: un-skipped by M7 stream C")
    def test_exported_runtime_headless_exits_zero(self) -> None:
        executable, _ = self.export_game()
        logic_only = ["--headless", "--renderer", "none", "--frames", "120", "--expect-no-errors"]
        completed = self.run_game(executable, logic_only, self.directory / "LogicOnly")
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr.decode("utf-8", errors="replace"))
        if not self.require_gpu():
            return
        rendering = ["--headless", "--frames", "120", "--expect-no-errors", *GPU_EDITOR_ARGUMENTS]
        rendered = self.run_game(executable, rendering, self.directory / "Rendering")
        self.assertEqual(rendered.returncode, EXIT_SUCCESS, rendered.stderr.decode("utf-8", errors="replace"))

    @unittest.skip("contract stub: un-skipped by M7 stream C")
    def test_runtime_missing_manifest_exits_3(self) -> None:
        runtime = runtime_executable(automation_config())
        self.assertTrue(runtime.is_file(),
                        f"build the Runtime first: python Scripts/Build.py --config {automation_config()}")
        completed = self.run_game(runtime, ["--headless", "--renderer", "none", "--frames", "1"],
                                  self.directory / "Bare")
        self.assertEqual(completed.returncode, EXIT_INIT_FAILED)
        self.assertIn("Game.json", completed.stderr.decode("utf-8", errors="replace"))

    @unittest.skip("contract stub: un-skipped by M7 stream C")
    def test_runtime_automation_subset(self) -> None:
        if not self.require_gpu():
            return
        executable, editor_hash = self.export_game(lockstep_ticks=STEPPED_TICKS)
        # --paused: the session waits at tick 0, so the client starts from a known tick.
        arguments = ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Automated", TINY_GAME_NAME)
        self.editors.append(game)
        client = self.connect(game)
        self.assertEqual(client.call("session.info")["playState"], "Paused")
        self.assertEqual(client.call("play.state")["tick"], 0)
        stepped = client.call("play.step", {"ticks": STEPPED_TICKS})
        self.assertEqual(stepped["tick"], STEPPED_TICKS)
        self.assertEqual(client.call("play.state")["tick"], STEPPED_TICKS)
        # The exported game's session is the editor's: the same scene document, seed and ticks give the same hash.
        self.assertEqual(stepped["stateHash"], editor_hash)
        shot = client.call("viewport.screenshot", {"view": "game", "width": 160, "height": 90})
        self.assertTrue(Path(shot["path"]).is_file())
        self.assertEqual(shot["view"], "Game")
        self.assertEqual(shot["target"], "Play")
        # The Runtime serves its subset only.
        editor_only = (("play.start", {}), ("play.stop", {}), ("entity.create", {"name": "X"}),
                       ("project.export", {"config": "Release"}))
        for method, params in editor_only:
            with self.subTest(method=method):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call(method, params)
                self.assert_engine_error(raised.exception, engine_client.METHOD_NOT_FOUND)
        with self.assertRaises(engine_client.EngineError) as scene_view:
            client.call("viewport.screenshot", {"view": "scene"})
        self.assert_engine_error(scene_view.exception, engine_client.UNSUPPORTED, "Unsupported")
        self.assertEqual(scene_view.exception.issues[0]["pointer"], "/view")
        client.call("play.resume")
        self.assertEqual(client.call("session.info")["playState"], "Play")
        client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())


if __name__ == "__main__":
    unittest.main()
