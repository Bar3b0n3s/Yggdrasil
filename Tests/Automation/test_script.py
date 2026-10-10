"""M13 script authoring, evaluation, diagnostics and watchdog automation acceptance."""

from __future__ import annotations

import threading
import time
import unittest

from harness import AutomationTestCase, engine_client


class ScriptTests(AutomationTestCase):
    def test_fields_only_patch_uses_existing_script_schema(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Owner.luau"
        source = """
local Owner = { Fields = {
    Amount = Field.Number(1, {Min = 0, Max = 5}),
    Mode = Field.Enum({"Idle", "Moving"}),
    References = Field.Array(Field.Array(Field.Asset("Script"))),
} }
return Script.Define("Owner", Owner)
"""
        script = client.call("script.write", {"path": path, "source": source})["script"]["id"]
        client.call("entity.create", {
            "name": "Owner", "components": {"Script": {"Script": path}},
        })
        patch = {"entity": "/Owner", "components": {"Script": {"Fields": {
            "Amount": 3, "Mode": "mOvInG", "References": [[path]],
        }}}}
        before = client.call("entity.get", {"entity": "/Owner"})["entity"]
        client.call("entity.update", dict(patch, dryRun=True))
        self.assertEqual(client.call("entity.get", {"entity": "/Owner"})["entity"], before)
        client.call("edit.batch", {"ops": [{"method": "entity.update", "params": patch}]})
        component = client.call("entity.get", {"entity": "/Owner"})["entity"]["components"]["Script"]
        self.assertEqual(component["Script"], script)
        self.assertEqual(component["Fields"], {
            "Amount": 3, "Mode": "Moving", "References": [[script]],
        })
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("entity.update", {"entity": "/Owner", "components": {
                "Script": {"Fields": {"Amount": 6}},
            }})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(invalid.exception.data["issues"][0]["pointer"],
                         "/components/Script/Fields/Amount")
        with self.assertRaises(engine_client.EngineError) as malformed:
            client.call("entity.update", {"entity": "/Owner", "components": {
                "Script": {"Fields": [1]},
            }})
        self.assert_engine_error(malformed.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(malformed.exception.data["issues"][0]["pointer"],
                         "/components/Script/Fields")
        self.assertEqual(client.call("entity.get", {"entity": "/Owner"})["entity"]["components"]["Script"],
                         component)

    def test_script_write_returns_diagnostics(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Mistake.luau"
        source = '--!strict\nlocal count: number = "wrong"\nreturn count\n'
        written = client.call("script.write", {"path": path, "source": source})
        self.assertGreater(written["undoIndex"], 0)
        errors = [row for row in written["diagnostics"] if row["severity"] == "Error"]
        self.assertTrue(errors, written)
        diagnostic = next(row for row in errors if row["line"] == 2)
        self.assertEqual(diagnostic["file"], path)
        self.assertGreater(diagnostic["column"], 0)
        self.assertEqual(diagnostic["endLine"], 2)
        self.assertGreater(diagnostic["endColumn"], diagnostic["column"])
        self.assertTrue(diagnostic["code"])
        self.assertEqual(client.call("script.read", {"path": path})["source"], source)
        self.assertFalse(client.call("script.check", {"paths": [path]})["passed"])
        client.call("edit.undo")
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("script.read", {"path": path})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")
        client.call("edit.redo")
        self.assertEqual(client.call("script.read", {"path": path})["script"]["id"], written["script"]["id"])

    def test_script_templates_and_recursive_field_schema(self) -> None:
        client, _ = self.open_editor_with_scene()
        for template, kind in (("Behaviour", "Behaviour"), ("Module", "Module"), ("Test", "TestSuite")):
            with self.subTest(template=template):
                path = f"Assets/Scripts/{template}.luau"
                created = client.call("script.create", {"path": path, "template": template})
                self.assertFalse(created["diagnostics"], created)
                self.assertEqual(client.call("script.fields", {"script": path})["kind"], kind)
                self.assertNotIn("{{Name}}", client.call("script.read", {"path": path})["source"])
        source = ('local Fields = {}; Fields.Fields = { '
                  'Values = Field.Array(Field.Number(2, { Min = 1, Max = 5 })), '
                  'Sound = Field.Asset("AudioClip") }; return Script.Define("Fields", Fields)')
        client.call("script.write", {"path": "Assets/Scripts/Fields.luau", "source": source})
        fields = client.call("script.fields", {"script": "Assets/Scripts/Fields.luau"})["fields"]
        self.assertEqual([field["name"] for field in fields], ["Sound", "Values"])
        self.assertEqual(fields[0]["schema"]["x-assetType"], "AudioClip")
        element = fields[1]["schema"]["items"]
        self.assertEqual((element["default"], element["minimum"], element["maximum"]), (2, 1, 5))

    def test_edit_eval_is_read_only_and_errors_survive_vm_replacement(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Before"})
        before = client.call("entity.get", {"entity": "/Before"})["entity"]
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        value = client.call("script.eval", {"context": "edit", "code": 'print("hello"); return { answer = 42 }'})
        self.assertEqual(value["value"], {"answer": 42})
        self.assertEqual(value["prints"], ["hello"])
        with self.assertRaises(engine_client.EngineError) as readonly:
            client.call("script.eval", {"context": "edit", "code": 'Scene.FindByName("Before").Name = "After"'})
        self.assert_engine_error(readonly.exception, engine_client.SCRIPT_ERROR, "Script")
        self.assertIn("scriptError", readonly.exception.data)
        self.assertEqual(client.call("entity.get", {"entity": "/Before"})["entity"], before)
        errors = client.call("script.errors", {"since": cursor, "limit": 1})
        self.assertEqual(len(errors["errors"]), 1)
        self.assertIn("read-only", errors["errors"][0]["message"])
        client.call("script.eval", {"context": "edit", "code": "1 + 2"})
        self.assertFalse(client.call("script.errors", {"since": errors["nextCursor"]})["errors"])
        self.assertEqual(client.call("script.errors", {"since": cursor})["errors"], errors["errors"])

    def test_wait_for_predicate(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"lockstep": True, "parameters": {"level": 4}, "pauseOnError": False})
        result = client.call("play.waitFor", {"until": 'Time.GetTick() >= 3 and { level = Scene.GetLoadParameters().level }',
                                              "timeoutTicks": 10})
        self.assertEqual((result["satisfied"], result["tick"], result["value"]), (True, 3, {"level": 4}))
        timeout = client.call("play.waitFor", {"until": "false", "timeoutTicks": 2})
        self.assertEqual((timeout["satisfied"], timeout["tick"], timeout["value"]), (False, 5, False))
        truthy = client.call("play.waitFor", {"until": "0", "timeoutTicks": 2})
        self.assertTrue(truthy["satisfied"])
        self.assertEqual(truthy["tick"], 6)
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("play.waitFor", {"until": "local =", "timeoutTicks": 2})
        self.assertTrue(invalid.exception.data)
        self.assertEqual(client.call("play.state")["tick"], 6)

    def test_entity_update_rejects_non_behaviour_script(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Target"})
        client.call("script.write", {"path": "Assets/Scripts/Library.luau", "source": "return { value = 1 }"})
        with self.assertRaises(engine_client.EngineError) as rejected:
            client.call("entity.update", {"entity": "/Target", "components": {
                "Script": {"Script": "Assets/Scripts/Library.luau"}}})
        self.assert_engine_error(rejected.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("SCRIPT_NOT_A_BEHAVIOUR", str(rejected.exception.data))
        self.assertNotIn("Script", client.call("entity.get", {"entity": "/Target"})["entity"]["components"])

    def test_runaway_script_busy_watchdog(self) -> None:
        # A raised callback budget deliberately lets the independent automation watchdog observe the spin.
        # Short bounded requests poll for Busy; no passing outcome depends on a sleep or exact elapsed duration.
        client, _ = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Scripting": {"CallbackBudgetMs": 10000}}})
        client.call("play.start", {"lockstep": True})
        observer = self.connect(self.editors[-1], "watchdog-observer")
        finished = threading.Event()
        outcome: list[Exception] = []

        def spin() -> None:
            try:
                client.call("script.eval", {"context": "play", "code": "while true do end"}, timeout=60.0)
            except Exception as error:
                outcome.append(error)
            finally:
                finished.set()

        thread = threading.Thread(target=spin)
        thread.start()
        busy = None
        deadline = time.monotonic() + 45.0
        try:
            while busy is None and not finished.is_set() and time.monotonic() < deadline:
                try:
                    observer.call("session.info", timeout=0.25)
                except TimeoutError:
                    continue
                except engine_client.EngineError as error:
                    if error.code != engine_client.BUSY:
                        raise
                    busy = error
        finally:
            thread.join(timeout=60.0)
        self.assertFalse(thread.is_alive(), "script watchdog did not terminate the callback")
        self.assertIsNotNone(busy, "automation watchdog never reported Busy during the raised-budget callback")
        self.assertEqual(len(outcome), 1, outcome)
        self.assertIsInstance(outcome[0], engine_client.EngineError)
        self.assertEqual(outcome[0].code, engine_client.SCRIPT_ERROR)
        self.assertEqual(outcome[0].data["scriptError"]["kind"], "timeout")
        self.assertTrue(observer.call("session.info"))
        self.assertTrue(any(row["kind"] == "timeout" for row in observer.call("script.errors")["errors"]))


if __name__ == "__main__":
    unittest.main()
