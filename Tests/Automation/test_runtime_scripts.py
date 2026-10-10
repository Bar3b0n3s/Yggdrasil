"""M13 shared script/replay methods against a real exported runtime."""

from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

from harness import EXIT_FAILED, EXIT_SUCCESS, AutomationTestCase, engine_client
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game, exported_executable


class RuntimeScriptTests(AutomationTestCase):
    def run_runtime(self, executable: Path, arguments: list[str], user_data: Path) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [str(executable), "--headless", "--renderer", "none", *arguments, f"--user-data-dir={user_data}"],
            cwd=self.directory, stdin=subprocess.DEVNULL, capture_output=True, timeout=120.0, check=False)

    def test_exported_runtime_script_eval_errors_and_replay(self) -> None:
        editor = self.connect(self.start_editor())
        self.create_project(editor, TINY_GAME_NAME)
        build_tiny_game(editor)
        editor.call("script.write", {"path": "Assets/Scripts/Mover.luau", "source":
                    'local Mover = {Fields = {Speed = Field.Number(1)}}; '
                    'function Mover.OnFixedUpdate(self: any, dt: number) '
                    'self.Entity.Transform:Translate(vector.create(self.Speed * dt, 0, 0)) end; '
                    'return Script.Define("Mover", Mover)'})
        editor.call("entity.update", {"entity": "/Cube", "components": {
            "Script": {"Script": "Assets/Scripts/Mover.luau", "Fields": {"Speed": 2}}}})
        editor.call("scene.save")
        editor.call("input.record", {"action": "start"})
        editor.call("play.step", {"ticks": 3, "render": "none"})
        path = "Assets/Replays/Runtime.replay"
        saved = editor.call("input.record", {"action": "stop", "path": path,
                                             "expect": [{"tick": 3, "luau": "Time.GetTick() == 3"}]})
        editor.call("play.stop")
        executable = exported_executable(export_tiny_game(editor))
        editor.call("session.shutdown")
        self.editors[-1].wait()
        process = engine_client.launch_editor(executable, ["--headless", "--renderer", "none", "--automation", "--paused"],
                                               self.directory / "RuntimeScripts", TINY_GAME_NAME)
        self.editors.append(process)
        runtime = self.connect(process)
        environment = runtime.call("script.eval", {"context": "play", "code":
                                   "return {editor = Application.IsEditor(), headless = Application.IsHeadless()}"})
        self.assertEqual(environment["value"], {"editor": False, "headless": True})
        self.assertEqual(runtime.call("script.eval", {"context": "play", "code": "1 + 2"})["value"], 3)
        with self.assertRaises(engine_client.EngineError) as edit:
            runtime.call("script.eval", {"context": "edit", "code": "1"})
        self.assert_engine_error(edit.exception, engine_client.UNSUPPORTED, "Unsupported")
        cursor = runtime.call("script.errors", {"since": "end"})["nextCursor"]
        event_cursor = runtime.call("events.read", {"cursor": "end"})["nextCursor"]
        log_cursor = runtime.call("log.read", {"cursor": "end"})["nextCursor"]
        for count in (1, 2):
            with self.assertRaises(engine_client.EngineError) as fault:
                runtime.call("script.eval", {"context": "play", "code": 'error("runtime eval fault")'})
            self.assert_engine_error(fault.exception, engine_client.SCRIPT_ERROR, "Script")
            errors = runtime.call("script.errors", {"since": cursor})["errors"]
            self.assertEqual(len(errors), 1, errors)
            self.assertEqual(errors[0]["count"], count)
            events = runtime.call("events.read", {"cursor": event_cursor, "types": ["ScriptErrorRaised"]})["events"]
            self.assertEqual(len(events), count, events)
            for event in events:
                self.assertEqual(event["tick"], 0)
                self.assertEqual(event["id"], errors[0]["entity"]["id"])
                self.assertEqual(event["path"], errors[0]["script"])
                self.assertEqual(event["message"], errors[0]["message"])
                self.assertEqual(event["name"], "")
                self.assertFalse(event["dirty"])
            logs = runtime.call("log.read", {"cursor": log_cursor, "loggers": ["Script"],
                                            "contains": "runtime eval fault"})["entries"]
            self.assertEqual(len(logs), count, logs)
            for entry in logs:
                self.assertEqual(entry["level"], "Error")
                self.assertEqual(entry["tick"], 0)
                self.assertEqual(entry["scriptFile"], errors[0]["script"])
                self.assertEqual(entry["scriptLine"], errors[0]["line"])
        waited = runtime.call("play.waitFor", {"until": "Time.GetTick() >= 2", "timeoutTicks": 5})
        self.assertEqual(waited["tick"], 2)
        runtime.call("script.eval", {"context": "play", "code": 'Audio.SetGroupVolume("Music", 0.25)'})
        before_failed_restart = runtime.call("play.state")
        with self.assertRaises(engine_client.EngineError):
            runtime.call("input.record", {"action": "start", "restart": True, "scene": "Assets/Scripts/Mover.luau"})
        after_failed_restart = runtime.call("play.state")
        for field in ("stateHash", "tick", "state", "lockstep"):
            self.assertEqual(after_failed_restart[field], before_failed_restart[field])
        self.assertEqual(runtime.call("script.eval", {"context": "play", "code":
                                                      'Audio.GetGroupVolume("Music")'})["value"], 0.25)
        # The second restart exposes a replacement that accidentally captured the first session's gameplay mix.
        for _ in range(2):
            replayed = runtime.call("input.replay", {"path": path, "verify": True, "strictHash": True})
            self.assertEqual(replayed["stateHash"], saved["stateHash"])
            self.assertTrue(replayed["hashMatched"])
            self.assertEqual(runtime.call("script.eval", {"context": "play", "code":
                                                          'Audio.GetGroupVolume("Music")'})["value"], 1)
        retained = runtime.call("script.errors", {"since": cursor})["errors"]
        self.assertEqual(len(retained), 1, retained)
        self.assertEqual(retained[0]["count"], 2)
        self.assertEqual(runtime.call("events.read", {"cursor": event_cursor,
                                                      "types": ["ScriptErrorRaised"]})["events"], events)
        self.assertEqual(runtime.call("log.read", {"cursor": log_cursor, "loggers": ["Script"],
                                                   "contains": "runtime eval fault"})["entries"], logs)
        runtime.call("input.record", {"action": "start", "restart": True,
                                      "parameters": {"level": 7}, "seed": 42})
        runtime.call("play.step", {"ticks": 2, "render": "none"})
        for invalid_path in ("../Escaped.replay", "project://Assets/Denied.replay", "user://Other.replay",
                             str(self.directory / "Outside.replay")):
            with self.subTest(path=invalid_path):
                with self.assertRaises(engine_client.EngineError):
                    runtime.call("input.record", {"action": "stop", "path": invalid_path})
                self.assertTrue(runtime.call("play.state")["recording"])
        recorded = runtime.call("input.record", {"action": "stop", "path": "Nested/Local.replay", "expect": [
            {"tick": 0, "luau": "Scene.GetLoadParameters().level == 7"},
            {"tick": 2, "luau": "Time.GetTick() == 2"}]})
        self.assertTrue(recorded["path"].startswith("user://Replays/"))
        reproduced = runtime.call("input.replay", {"path": recorded["path"], "verify": True, "strictHash": True})
        self.assertEqual(reproduced["stateHash"], recorded["stateHash"])
        self.assertTrue(all(item["satisfied"] for item in reproduced["expect"]))
        runtime.call("session.shutdown")
        self.assertEqual(process.wait(), EXIT_SUCCESS, process.output())

        cooked = self.run_runtime(executable, ["--replay", path, "--verify"], self.directory / "RuntimeCli")
        self.assertEqual(cooked.returncode, EXIT_SUCCESS, cooked.stderr.decode("utf-8", errors="replace"))
        self.assertIn(saved["stateHash"], cooked.stderr.decode("utf-8", errors="replace"))
        user_data = self.directory / "RuntimeScripts"
        local = self.run_runtime(executable, ["--replay", recorded["path"], "--verify"], user_data)
        self.assertEqual(local.returncode, EXIT_SUCCESS, local.stderr.decode("utf-8", errors="replace"))
        recording = user_data / TINY_GAME_NAME / "Replays/Nested/Local.replay"
        document = json.loads(recording.read_text(encoding="utf-8"))
        document["FinalStateHash"] = "ffffffffffffffff" if document["FinalStateHash"] != "ffffffffffffffff" else "0000000000000000"
        recording.write_text(json.dumps(document), encoding="utf-8")
        mismatch = self.run_runtime(executable, ["--replay", recorded["path"], "--verify"], user_data)
        self.assertEqual(mismatch.returncode, EXIT_FAILED, mismatch.stderr.decode("utf-8", errors="replace"))

    def test_exported_runtime_scene_load_and_prefab_keep_cooked_script_schemas(self) -> None:
        editor = self.connect(self.start_editor())
        self.create_project(editor, TINY_GAME_NAME)
        build_tiny_game(editor)
        editor.call("script.write", {"path": "Assets/Scripts/Spawned.luau", "source":
                    'local T = {Fields = {Amount = Field.Number(1)}}; '
                    'function T.OnCreate(self: any) assert(self.Amount == 9); self.Entity.Name = "Spawned9" end; '
                    'return Script.Define("Spawned", T)'})
        editor.call("entity.create", {"name": "Template", "components": {"Script": {
            "Script": "Assets/Scripts/Spawned.luau", "Fields": {"Amount": 9}}}})
        editor.call("prefab.create", {"entity": "/Template", "path": "Assets/Prefabs/Spawned.prefab"})
        editor.call("entity.destroy", {"entities": ["/Template"]})
        editor.call("script.write", {"path": "Assets/Scripts/Transition.luau", "source":
                    'local T = {}; function T.OnCreate(self: any) Time.SetTimeScale(0.5) end; '
                    'function T.OnFixedUpdate(self: any) '
                    'local scene = Assets.Load("Assets/Scenes/Second.scene"); assert(scene ~= nil); '
                    'Scene.Load(scene, {level = 7}) end; '
                    'return Script.Define("Transition", T)'})
        editor.call("entity.update", {"entity": "/Cube", "components": {
            "Script": {"Script": "Assets/Scripts/Transition.luau"}}})
        editor.call("scene.save")
        editor.call("scene.new", {"path": "Assets/Scenes/Second.scene"})
        editor.call("script.write", {"path": "Assets/Scripts/Second.luau", "source":
                    'local T = {Fields = {Amount = Field.Number(1)}}; '
                    'function T.OnCreate(self: any) assert(self.Amount == 11); '
                    'assert(Scene.GetLoadParameters().level == 7); self.Entity.Name = "Loaded11"; '
                    'local prefab = Assets.Load("Assets/Prefabs/Spawned.prefab"); assert(prefab ~= nil); '
                    'Scene.Instantiate(prefab) end; '
                    'function T.OnFixedUpdate(self: any) if Time.GetTick() >= 3 then Application.Quit(7) end end; '
                    'return Script.Define("Second", T)'})
        editor.call("entity.create", {"name": "Receiver", "components": {"Script": {
            "Script": "Assets/Scripts/Second.luau", "Fields": {"Amount": 11}}}})
        editor.call("scene.save")
        executable = exported_executable(export_tiny_game(editor))
        editor.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS)
        process = engine_client.launch_editor(
            executable, ["--headless", "--renderer", "none", "--automation", "--paused"],
            self.directory / "RuntimeSceneLoad", TINY_GAME_NAME)
        self.editors.append(process)
        runtime = self.connect(process)
        self.assertEqual(runtime.call("script.eval", {"context": "play", "code": "Time.GetTimeScale()"})["value"], 0.5)
        event_cursor = runtime.call("events.read", {"cursor": "end"})["nextCursor"]
        log_cursor = runtime.call("log.read", {"cursor": "end"})["nextCursor"]
        with self.assertRaises(engine_client.EngineError):
            runtime.call("script.eval", {"context": "play", "code": 'error("before scene replacement")'})
        runtime.call("play.step", {"ticks": 2, "render": "none"})
        loaded = runtime.call("entity.get", {"entity": "/Loaded11"})["entity"]
        self.assertEqual(loaded["name"], "Loaded11")
        self.assertEqual(runtime.call("entity.get", {"entity": "/Spawned9"})["entity"]["name"], "Spawned9")
        errors = runtime.call("script.errors")["errors"]
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("before scene replacement", errors[0]["message"])
        with self.assertRaises(engine_client.EngineError) as fault:
            runtime.call("script.eval", {"context": "play", "entity": loaded["id"],
                                          "code": 'error("after scene replacement")'})
        self.assert_engine_error(fault.exception, engine_client.SCRIPT_ERROR, "Script")
        errors = runtime.call("script.errors")["errors"]
        self.assertEqual(len(errors), 2, errors)
        self.assertEqual(errors[1]["entity"]["id"], loaded["id"])
        events = runtime.call("events.read", {"cursor": event_cursor, "types": ["ScriptErrorRaised"]})["events"]
        self.assertEqual(len(events), 2, events)
        logs = runtime.call("log.read", {"cursor": log_cursor, "loggers": ["Script"],
                                         "contains": "scene replacement"})["entries"]
        self.assertEqual(len(logs), 2, logs)
        for error, event, entry, tick in zip(errors, events, logs, (0, 2), strict=True):
            self.assertEqual(event["tick"], tick)
            self.assertEqual(event["id"], error["entity"]["id"])
            self.assertEqual(event["path"], error["script"])
            self.assertEqual(event["message"], error["message"])
            self.assertEqual(entry["tick"], tick)
            self.assertEqual(entry["entity"], event["id"])
            self.assertEqual(entry["scriptFile"], event["path"])
        runtime.call("session.shutdown")
        self.assertEqual(process.wait(), EXIT_SUCCESS, process.output())
        quit_run = self.run_runtime(executable, ["--frames", "10"], self.directory / "RuntimeQuit")
        self.assertEqual(quit_run.returncode, 7, quit_run.stderr.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    unittest.main()
