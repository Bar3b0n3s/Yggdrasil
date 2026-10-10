"""M13 editor host seams through the real headless application loop."""

from __future__ import annotations

import sys
import time
import unittest
from typing import Any

from harness import AutomationTestCase, engine_client


class EditorScriptHostTests(AutomationTestCase):
    def eval_play(self, client: engine_client.EngineClient, code: str) -> Any:
        return client.call("script.eval", {"context": "play", "code": code})["value"]

    def wait_for_value(self, client: engine_client.EngineClient, code: str, expected: Any) -> None:
        # Poll another process's safe-point publication; the deadline only bounds a failure.
        deadline = time.monotonic() + 30.0
        while True:
            value = self.eval_play(client, code)
            if value == expected:
                return
            if time.monotonic() >= deadline:
                self.fail(f"editor never published {expected!r}; last value was {value!r}")

    def wait_for_edit(self, client: engine_client.EngineClient) -> None:
        deadline = time.monotonic() + 30.0
        while client.call("play.state")["state"] != "Edit":
            if time.monotonic() >= deadline:
                self.fail("Application.Quit did not return the editor to Edit")

    def test_script_save_reloads_existing_instance_and_required_module(self) -> None:
        client, root = self.open_editor_with_scene()
        module = "Assets/Scripts/Dependency.luau"
        path = "Assets/Scripts/Host.luau"
        source = '''
local Dependency = require("./Dependency")
local Host = { Fields = { Count = Field.Number(0), Reloads = Field.Number(0) } }
function Host:Value() return Dependency.Value end
function Host:OnHotReload() self.Reloads += 1 end
return Script.Define("Host", Host)
'''
        client.call("script.write", {"path": module, "source": "return {Value = 1}"})
        client.call("script.write", {"path": path, "source": source})
        client.call("entity.create", {"name": "Host", "components": {"Script": {"Script": path}}})
        client.call("play.start", {"paused": True})
        self.eval_play(client, 'Scene.FindByName("Host"):GetScript().Count = 91')
        probe = 'local s = Scene.FindByName("Host"):GetScript(); return {s.Count, s:Value(), s.Reloads}'
        self.assertEqual(self.eval_play(client, probe), [91, 1, 0])
        changed = source.replace("return Dependency.Value", "return Dependency.Value * 2")
        client.call("script.write", {"path": path, "source": changed})
        self.wait_for_value(client, probe, [91, 2, 1])
        client.call("script.write", {"path": module, "source": "return {Value = 10}"})
        self.wait_for_value(client, probe, [91, 20, 2])
        # An external save uses the same publication queue, including transitive dependent notification deduplication.
        (root / module).write_text("return {Value = 20}", encoding="utf-8")
        client.call("project.refreshAssets")
        self.wait_for_value(client, probe, [91, 40, 3])
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        client.call("script.write", {"path": path, "source": "local ="})
        deadline = time.monotonic() + 30.0
        errors: list[dict] = []
        while not errors:
            errors = client.call("script.errors", {"since": cursor})["errors"]
            if time.monotonic() >= deadline:
                self.fail("failed script import never reached script.errors")
        error = next(row for row in errors if row["kind"] == "compile")
        self.assertEqual(error["script"], path)
        self.assertEqual(error["line"], 1)
        self.assertGreater(error["column"], 0)
        self.assertEqual(self.eval_play(client, probe), [91, 40, 3])
        client.call("script.write", {"path": path, "source": changed})
        client.call("play.stop")
        client.call("play.start", {"lockstep": True})
        client.call("script.write", {"path": module, "source": "return {Value = 100}"})
        client.call("play.step", {"ticks": 2})
        self.assertEqual(self.eval_play(client, probe), [0, 40, 0])
        client.call("play.stop")
        client.call("play.start", {"paused": True})
        self.assertEqual(self.eval_play(client, probe), [0, 200, 0])

    def test_application_quit_returns_rpc_result_and_ends_ordinary_play(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"paused": True})
        self.assertEqual(self.eval_play(client, "Application.Quit(7); return 42"), 42)
        self.wait_for_edit(client)
        self.assertTrue(client.call("session.info"))
        for callback in ("OnStart", "OnFixedUpdate"):
            with self.subTest(callback=callback):
                path = "Assets/Scripts/Quit.luau"
                source = (f"local Quit = {{}}; function Quit:{callback}() Application.Quit(0) end; "
                          'return Script.Define("Quit", Quit)')
                client.call("script.write", {"path": path, "source": source})
                client.call("entity.create", {"name": callback, "components": {"Script": {"Script": path}}})
                client.call("play.start")
                self.wait_for_edit(client)
                client.call("entity.destroy", {"entities": [f"/{callback}"]})

    def test_type_error_policy_blocks_play_without_changing_edit_state(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/BadType.luau", "source":
                    '--!strict\nlocal value: number = "wrong"\nreturn {Value = value}'})
        client.call("script.write", {"path": "Assets/Scripts/Consumer.luau", "source":
                    'local M = require("./BadType"); return Script.Define("Consumer", {})'})
        client.call("entity.create", {"name": "Consumer", "components": {
            "Script": {"Script": "Assets/Scripts/Consumer.luau"}}})
        client.call("project.setSettings", {"patch": {"Scripting": {"BlockPlayOnTypeErrors": False}}})
        client.call("play.start", {"paused": True})
        client.call("play.stop")
        client.call("project.setSettings", {"patch": {"Scripting": {"BlockPlayOnTypeErrors": True}}})
        with self.assertRaises(engine_client.EngineError) as rejected:
            client.call("play.start")
        self.assert_engine_error(rejected.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("BadType.luau", str(rejected.exception.data))
        self.assertEqual(client.call("play.state")["state"], "Edit")
        (root / ".luaurc").write_bytes(b"\xff")
        with self.assertRaises(engine_client.EngineError) as unavailable:
            client.call("play.start")
        self.assert_engine_error(unavailable.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn(".luaurc", str(unavailable.exception.data))
        self.assertEqual(client.call("play.state")["state"], "Edit")

    def test_headless_environment_and_cursor_use_real_editor_window(self) -> None:
        client, _ = self.open_editor_with_scene()
        platform = {"win32": "Windows", "linux": "Linux", "darwin": "macOS"}[sys.platform]
        probe = ('return {editor = Application.IsEditor(), headless = Application.IsHeadless(), '
                 'platform = Application.GetPlatform(), validSize = Application.GetWindowSize() ~= vector.zero}')
        expected = {"editor": True, "headless": True, "platform": platform, "validSize": True}
        self.assertEqual(client.call("script.eval", {"context": "edit", "code": probe})["value"], expected)
        client.call("play.start", {"paused": True})
        self.assertEqual(self.eval_play(client, probe), expected)
        for mode in ("Hidden", "Locked", "Normal"):
            self.assertEqual(self.eval_play(client, f'Input.SetCursorMode("{mode}"); return Input.GetCursorMode()'), mode)
        self.eval_play(client, 'Input.SetCursorMode("Locked")')
        client.call("play.stop")
        client.call("play.start", {"paused": True})
        self.assertEqual(self.eval_play(client, "Input.GetCursorMode()"), "Normal")


if __name__ == "__main__":
    unittest.main()
