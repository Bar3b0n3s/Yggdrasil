"""The Runtime executable and exported games (Docs/Architecture.md §14.1, §14.3, §13.5 "Runtime subset", §13.9; Roadmap
M7 acceptance): the manifest's name for the user-data folder, headless runs that log no error, the missing-manifest exit
code, the automation subset served by an exported build, the physics an exported game simulates (§9, Roadmap M11), read
through that subset's physics.bodyInfo, and the voices an exported game holds, read through audio.stats (§10, Roadmap
M12).

The exports come from project.export and the games run a play session rendered by the scene renderer
(Docs/Decisions/0012-m7-decisions.md decisions 10 and 12). test_runtime_missing_manifest_exits_3 needs only the Runtime.
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
# The entities of the tiny game's scene (tiny_game.build_tiny_game).
TINY_GAME_ENTITIES = 3


class RuntimeTests(AutomationTestCase):
    """Exported builds of the tiny game, and the bare Runtime."""

    def export_game(self, lockstep_ticks: int = 0, with_physics: bool = False) -> tuple[Path, str]:
        """Exports the tiny game (a fresh project) and returns its executable; the exporting editor is shut down first.
        With `lockstep_ticks`, the editor first plays the game in lockstep with its default seed for that many ticks and
        the state hash it reached is returned too (empty otherwise). With `with_physics`, the scene also holds a static
        Ground box whose top face is at y = 0 and a dynamic Ball of radius 0.5 whose centre is at y = 1."""
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        if with_physics:
            client.call("entity.create", {"name": "Ground", "components": {
                "Transform": {"Translation": [0, -0.5, 0]}, "RigidBody": {"Type": "Static"},
                "BoxCollider": {"HalfExtents": [50, 0.5, 50]}}})
            client.call("entity.create", {"name": "Ball", "components": {
                "Transform": {"Translation": [0, 1, 0]}, "RigidBody": {"Type": "Dynamic"},
                "SphereCollider": {"Radius": 0.5}}})
            client.call("scene.save")
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

    def test_exported_user_data_folder_uses_manifest_name(self) -> None:
        executable, _ = self.export_game()
        user_data = self.directory / "GameUserData"
        completed = self.run_game(executable, ["--headless", "--renderer", "none", "--frames", "5"], user_data)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr.decode("utf-8", errors="replace"))
        # Logs, crash reports and user:// live under the manifest's Name, never under the engine's product name.
        self.assertTrue((user_data / TINY_GAME_NAME / "Logs").is_dir())
        self.assertFalse((user_data / app_name()).exists())

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

    def test_runtime_missing_manifest_exits_3(self) -> None:
        # The bare Runtime of the build has no Game.json next to it (§4.1: a required file missing is InitFailed).
        runtime = runtime_executable(automation_config())
        self.assertTrue(runtime.is_file(),
                        f"build the Runtime first: python Scripts/Build.py --config {automation_config()}")
        completed = self.run_game(runtime, ["--headless", "--renderer", "none", "--frames", "1"],
                                  self.directory / "Bare")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        self.assertEqual(completed.returncode, EXIT_INIT_FAILED, stderr)
        self.assertIn("Game.json", stderr)
        # Without a manifest there is no application name, so nothing is written into a user-data folder.
        self.assertFalse((self.directory / "Bare" / app_name()).exists())

    def test_exported_runtime_simulates_physics(self) -> None:
        # The exported game runs its play session's physics (§9) with the project settings in Game.pak: the ball falls
        # onto the ground and rests there, as physics.bodyInfo of the Runtime's automation subset (§13.5) reports.
        executable, _ = self.export_game(with_physics=True)
        arguments = ["--headless", "--automation", "--paused", "--renderer", "none"]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Physics", TINY_GAME_NAME)
        self.editors.append(game)
        client = self.connect(game)
        # A fifth of a second in, the ball falls, touching nothing.
        client.call("play.step", {"ticks": 12, "render": "none"})
        falling = client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assertLess(falling["linearVelocity"][1], -1.0)
        self.assertEqual(falling["contacts"], [])
        # A second in, it rests on the ground.
        client.call("play.step", {"ticks": 48, "render": "none"})
        ball = client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assertEqual(ball["type"], "Dynamic")
        for axis in range(3):
            self.assertLess(abs(ball["linearVelocity"][axis]), 0.05)
        self.assertEqual([pair["other"]["name"] for pair in ball["contacts"]], ["Ground"])
        transform = client.call("entity.get", {"entity": "/Ball", "components": ["Transform"]})["entity"]
        self.assertAlmostEqual(transform["components"]["Transform"]["Translation"][1], 0.5, delta=0.05)
        client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())

    def test_runtime_automation_subset(self) -> None:
        if not self.require_gpu():
            return
        executable, editor_hash = self.export_game(lockstep_ticks=STEPPED_TICKS)
        # --paused: the session waits at tick 0, so the client starts from a known tick.
        arguments = ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Automated", TINY_GAME_NAME)
        self.editors.append(game)
        client = self.connect(game)
        info = client.call("session.info")
        self.assertEqual(info["playState"], "Paused")
        self.assertEqual(info["project"]["name"], TINY_GAME_NAME)
        self.assertTrue(info["readOnly"])
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
        # The shared reads address the play scene, the Runtime's only scene.
        tree = client.call("scene.tree", {"format": "Json"})
        self.assertEqual(len(tree["entities"]), TINY_GAME_ENTITIES)
        cube = client.call("entity.get", {"entity": "/Cube", "components": ["MeshRenderer"]})
        self.assertIn("MeshRenderer", cube["entity"]["components"])
        bounds = client.call("entity.bounds", {"entities": ["/Cube"]})
        self.assertTrue(bounds["bounds"][0]["hasBounds"])
        # physics.bodyInfo is served too (§13.5 Runtime subset): the cube has no collider, so it has no body.
        with self.assertRaises(engine_client.EngineError) as no_body:
            client.call("physics.bodyInfo", {"entity": "/Cube"})
        self.assert_engine_error(no_body.exception, engine_client.NOT_FOUND, "NotFound")
        self.assertEqual(no_body.exception.issues[0]["pointer"], "/entity")
        self.assertIn("nextCursor", client.call("log.read", {"cursor": "end"}))
        self.assertIn("nextCursor", client.call("events.read", {"cursor": "end"}))
        with self.assertRaises(engine_client.EngineError) as edit_scene:
            client.call("scene.tree", {"target": "edit"})
        self.assert_engine_error(edit_scene.exception, engine_client.INVALID_STATE)
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

    def test_runtime_audio_stats_reports_the_game_voices(self) -> None:
        # M12 (Docs/Decisions/0015-m12-decisions.md): audio.stats is in the Runtime subset (§13.5). The game plays a
        # sound effect created through automation and cooked into Game.pak; the exported Runtime's session, started
        # paused at tick 0 (--paused), holds its voice paused (§10.2).
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("asset.create", {"type": "SoundEffect", "path": "Assets/Audio/Hum.sfx",
                                     "values": {"Layers": [{"Wave": "Sine", "Duration": 2.0}]}})
        client.call("entity.create", {"name": "Speaker", "components": {"AudioSource": {
            "Clip": "Assets/Audio/Hum.sfx", "PlayOnStart": True, "Loop": True, "Spatial": False}}})
        client.call("scene.save")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.editors[-1].wait()

        arguments = ["--headless", "--automation", "--paused", "--renderer", "none"]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Audio", TINY_GAME_NAME)
        self.editors.append(game)
        game_client = self.connect(game)
        stats = game_client.call("audio.stats")
        self.assertEqual(stats["deviceState"], "None")
        self.assertEqual(stats["voiceCount"], 1)
        voice = stats["voices"][0]
        self.assertEqual(voice["entity"]["name"], "Speaker")
        self.assertEqual(voice["clip"]["path"], "Assets/Audio/Hum.sfx")
        self.assertTrue(voice["paused"])
        game_client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())


if __name__ == "__main__":
    unittest.main()
