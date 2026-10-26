"""M13 script authoring, evaluation, diagnostics and watchdog automation acceptance."""

from __future__ import annotations

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
            "name": "Owner", "components": {"Script": {"Scripts": [{"Script": path}]}},
        })
        # A slot given Fields alone borrows the slot's Script handle (Script version 2: a list of slots).
        patch = {"entity": "/Owner", "components": {"Script": {"Scripts": [{"Fields": {
            "Amount": 3, "Mode": "mOvInG", "References": [[path]],
        }}]}}}
        before = client.call("entity.get", {"entity": "/Owner"})["entity"]
        client.call("entity.update", dict(patch, dryRun=True))
        self.assertEqual(client.call("entity.get", {"entity": "/Owner"})["entity"], before)
        client.call("edit.batch", {"ops": [{"method": "entity.update", "params": patch}]})
        component = client.call("entity.get", {"entity": "/Owner"})["entity"]["components"]["Script"]
        self.assertEqual(component["Scripts"][0]["Script"], script)
        self.assertEqual(component["Scripts"][0]["Fields"], {
            "Amount": 3, "Mode": "Moving", "References": [[script]],
        })
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("entity.update", {"entity": "/Owner", "components": {
                "Script": {"Scripts": [{"Fields": {"Amount": 6}}]},
            }})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(invalid.exception.data["issues"][0]["pointer"],
                         "/components/Script/Scripts/0/Fields/Amount")
        with self.assertRaises(engine_client.EngineError) as malformed:
            client.call("entity.update", {"entity": "/Owner", "components": {
                "Script": {"Scripts": [{"Fields": [1]}]},
            }})
        self.assert_engine_error(malformed.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(malformed.exception.data["issues"][0]["pointer"],
                         "/components/Script/Scripts/0/Fields")
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

    def test_script_errors_keep_bounded_messages_and_report_evictions(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Fault.luau"
        client.call("script.write", {"path": path, "source": (
            'local Fault = {}\n'
            'function Fault:OnStart() error(self.Entity.Name .. string.rep("x", 20000), 0) end\n'
            'return Script.Define("Fault", Fault)\n')})
        # More distinct errors than the editor's history keeps (MaxScriptErrorEntries, 256), each far over 8 KiB.
        client.call("edit.batch", {"label": "Faults", "ops": [
            {"method": "entity.create", "params": {"name": f"Fault{index}",
                                                   "components": {"Script": {"Scripts": [{"Script": path}]}}}}
            for index in range(300)]})
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        client.call("play.start", {"lockstep": True, "pauseOnError": False})
        client.call("play.step", {"ticks": 1})
        # The page is larger than the inline result limit, so the editor offloads it to a file.
        errors = engine_client.load_offloaded(client.call("script.errors", {"since": cursor, "limit": 1000}))
        client.call("play.stop")
        self.assertTrue(errors["evicted"])
        self.assertEqual(len(errors["errors"]), 256)
        for error in errors["errors"]:
            self.assertLessEqual(len(error["message"].encode("utf-8")), 8192)
            self.assertRegex(error["message"], r"^Fault\d+x+ \[truncated: 2000\d bytes\]$")
        follow = client.call("script.errors", {"since": errors["nextCursor"]})
        self.assertFalse(follow["evicted"])
        self.assertFalse(follow["errors"])

    def test_script_errors_give_the_entity_path_while_the_scene_holds_the_entity(self) -> None:
        # The entity summary's path ("the entity's path from the scene root") was always empty. It is read with the
        # errors, in the scene that reads address (the play scene while playing, else the edit scene), and is empty once
        # that scene no longer holds the entity.
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Faulty.luau"
        client.call("script.write", {"path": path, "source": (
            'local Faulty = {}\n'
            'function Faulty:OnStart() error("board fault", 0) end\n'
            'return Script.Define("Faulty", Faulty)\n')})
        client.call("entity.create", {"name": "Game"})
        board = client.call("entity.create", {"name": "Board", "parent": "/Game",
                                              "components": {"Script": {"Scripts": [{"Script": path}]}}})["entity"]
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]

        def read_entity() -> dict:
            errors = client.call("script.errors", {"since": cursor})["errors"]
            self.assertEqual(len(errors), 1, errors)
            return errors[0]["entity"]

        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 1})
        self.assertEqual(read_entity(), {"id": board["id"], "name": "Board", "path": "/Game/Board"})
        client.call("entity.destroy", {"entities": ["/Game/Board"], "target": "play"})
        self.assertEqual(read_entity(), {"id": board["id"], "name": "Board", "path": ""})
        client.call("play.stop")
        self.assertEqual(read_entity()["path"], "/Game/Board")
        client.call("entity.destroy", {"entities": ["/Game/Board"]})
        self.assertEqual(read_entity(), {"id": board["id"], "name": "Board", "path": ""})

    def test_scene_reload_keeps_the_played_copy_and_get_current_names_the_scene(self) -> None:
        # ADR 0031 decision 5 (ADR 0026 P06): Scene.Load of the played scene reloads its saved file, dropping the edits
        # made since the save. Scene.Reload restarts from the play copy, unsaved edits included, with new parameters,
        # and Scene.GetCurrent names the open scene's asset, in an edit evaluation (which refuses Scene.Reload) and in
        # play.
        client, _ = self.open_editor_with_scene()
        client.call("scene.save")
        current = 'return Scene.GetCurrent() == Assets.Load("Assets/Scenes/Main.scene")'
        self.assertIs(client.call("script.eval", {"context": "edit", "code": current})["value"], True)
        with self.assertRaises(engine_client.EngineError) as readonly:
            client.call("script.eval", {"context": "edit", "code": "Scene.Reload()"})
        self.assert_engine_error(readonly.exception, engine_client.SCRIPT_ERROR, "Script")
        client.call("entity.create", {"name": "Unsaved"})
        client.call("play.start", {"lockstep": True, "parameters": {"round": 1}})
        self.assertIs(client.call("script.eval", {"context": "play", "code": current})["value"], True)
        client.call("script.eval", {"context": "play",
                                    "code": 'Scene.CreateEntity("Spawned"); Scene.Reload({round = 2})'})
        client.call("play.step", {"ticks": 1})
        state = client.call("script.eval", {"context": "play", "code": (
            'return {unsaved = Scene.FindByName("Unsaved") ~= nil, spawned = Scene.FindByName("Spawned") ~= nil, '
            'round = Scene.GetLoadParameters().round, '
            'current = Scene.GetCurrent() == Assets.Load("Assets/Scenes/Main.scene")}')})["value"]
        self.assertEqual(state, {"unsaved": True, "spawned": False, "round": 2, "current": True})
        client.call("play.stop")

    def test_wait_for_predicate(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"lockstep": True, "parameters": {"level": 4}, "pauseOnError": False})
        result = client.call("play.waitFor",
                             {"until": 'Time.GetTick() >= 3 and { level = Scene.GetLoadParameters().level }',
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
                "Script": {"Scripts": [{"Script": "Assets/Scripts/Library.luau"}]}}})
        self.assert_engine_error(rejected.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("SCRIPT_NOT_A_BEHAVIOUR", str(rejected.exception.data))
        self.assertNotIn("Script", client.call("entity.get", {"entity": "/Target"})["entity"]["components"])

    def test_missing_required_module_is_a_located_finding(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/Board.luau", "source": "return {Value = 1}"})
        for request in ("./Missing", "./Bord"):
            with self.subTest(request=request):
                source = f'local Board = require("{request}")\nreturn Script.Define("Game", {{}})\n'
                written = client.call("script.write", {"path": "Assets/Scripts/Game.luau", "source": source})
                self.assertTrue(any(row["severity"] == "Error" for row in written["diagnostics"]), written)
                # Checking the whole project reports every script with the unresolved require among the findings.
                checked = client.call("script.check", {})
                self.assertFalse(checked["passed"])
                self.assertEqual(checked["paths"], ["Assets/Scripts/Board.luau", "Assets/Scripts/Game.luau"])
                self.assertTrue(any(row["file"].startswith("Assets/Scripts/") and row["severity"] == "Error"
                                    for row in checked["diagnostics"]), checked)
                report = client.call("project.validate", {})
                self.assertIn("diagnostics", report, report)

    def test_looping_type_function_is_refused_and_never_hangs_the_editor(self) -> None:
        # Review finding M23 (Architecture §11.9): Luau ran a user-defined type function's body in its analysis VM with
        # no bound, so this script.write never returned and its import hung the editor. The type checker refuses the
        # declaration before analysis, located at its name, in the written script and in a module it requires.
        client, _ = self.open_editor_with_scene()
        spin = "type function Spin(t)\n\twhile true do end\n\treturn t\nend\n"
        client.call("script.write", {"path": "Assets/Scripts/Types.luau",
                                     "source": "--!strict\nexport " + spin + "return {}\n"})
        written = client.call("script.write", {"path": "Assets/Scripts/Spin.luau", "source": "--!strict\n" + spin
                                               + "local value: Spin<number> = 1\nreturn value\n"})
        refusal = next(row for row in written["diagnostics"] if row["severity"] == "Error")
        self.assertEqual((refusal["file"], refusal["line"], refusal["column"], refusal["code"]),
                         ("Assets/Scripts/Spin.luau", 2, 15, "SCRIPT_TYPE_ERROR"), written)
        self.assertIn("type function 'Spin' is not supported", refusal["message"])
        checked = client.call("script.check", {})
        self.assertFalse(checked["passed"])
        self.assertTrue(any(row["file"] == "Assets/Scripts/Types.luau" and row["line"] == 2
                            and row["code"] == "SCRIPT_TYPE_ERROR" for row in checked["diagnostics"]), checked)

    def test_script_create_names_test_suites_after_the_file(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Tests/Tetris.test.luau"
        created = client.call("script.create", {"path": path, "template": "Test"})
        self.assertFalse(created["diagnostics"], created)
        self.assertIn('Test.Suite("Tetris",', client.call("script.read", {"path": path})["source"])

    def test_edit_eval_ignores_a_dangling_script_reference(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Ball.luau"
        client.call("script.write", {"path": path, "source": 'return Script.Define("Ball", {})'})
        client.call("entity.create", {"name": "Ball", "components": {"Script": {"Scripts": [{"Script": path}]}}})
        client.call("asset.delete", {"asset": path})
        # The trashed behaviour only affects its own entity; every other evaluation still runs.
        self.assertEqual(client.call("script.eval", {"context": "edit", "code": "1 + 2"})["value"], 3)
        self.assertEqual(client.call("script.eval",
                                     {"context": "edit", "code": 'Scene.FindByName("Ball") ~= nil'})["value"], True)

    def test_hot_reload_locates_failures_and_ignores_unloaded_failed_suites(self) -> None:
        client, _ = self.open_editor_with_scene()
        module = "Assets/Scripts/Board.luau"
        path = "Assets/Scripts/Game.luau"
        suite = "Assets/Tests/Board.test.luau"
        source = '''local Board = require("./Board")
local Game = { Fields = { Reloads = Field.Number(0) } }
function Game:Value() return Board.Value end
function Game:OnHotReload() self.Reloads += 1 end
return Script.Define("Game", Game)
'''
        suite_source = ('local Board = require("../Scripts/Board")\n'
                        'return Test.Suite("Board", function()\n'
                        '\tTest.Case("value", function() Test.Expect(Board.Value > 0) end)\n'
                        'end)\n')
        client.call("script.write", {"path": module, "source": "return {Value = 1}"})
        client.call("script.write", {"path": path, "source": source})
        client.call("script.write", {"path": suite, "source": suite_source})
        client.call("entity.create", {"name": "Game", "components": {"Script": {"Scripts": [{"Script": path}]}}})
        client.call("play.start", {"paused": True})
        probe = 'local s = Scene.FindByName("Game"):GetScript(); return {s:Value(), s.Reloads}'
        self.assertEqual(self.eval_play(client, probe), [1, 0])
        # The suite requires Board, but it is neither loaded in play nor attached: its failed import is not live.
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        client.call("script.write", {"path": suite,
                                     "source": suite_source.replace("Test.Expect(Board.Value > 0)", "local = 1")})
        self.wait_for_compile_error(client, cursor, suite)
        client.call("script.write", {"path": module, "source": "return {Value = 10}"})
        self.wait_for_play_value(client, probe, [10, 1])
        # A syntax error on line 3 and a load-time failure on line 4 are reported where they are; old code keeps
        # running.
        for broken, line, column in (
                ('local Board = require("./Board")\n-- edited\nlocal = 1\n', 3, 7),
                ('local Board = require("./Board")\nlocal Game = {}\n\nGame.Tick = Time.GetTick()\n'
                 'return Script.Define("Game", Game)\n', 4, None)):
            with self.subTest(line=line):
                cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
                client.call("script.write", {"path": path, "source": broken})
                error = self.wait_for_compile_error(client, cursor, path)
                self.assertEqual(error["line"], line)
                if column is not None:
                    self.assertEqual(error["column"], column)
                self.assertGreater(error["column"], 0)
                self.assertEqual(self.eval_play(client, probe), [10, 1])

    def eval_play(self, client: engine_client.EngineClient, code: str) -> object:
        return client.call("script.eval", {"context": "play", "code": code})["value"]

    def wait_for_play_value(self, client: engine_client.EngineClient, code: str, expected: object) -> None:
        # Polls the editor's safe-point publication; the deadline only bounds a failure.
        deadline = time.monotonic() + 30.0
        while True:
            value = self.eval_play(client, code)
            if value == expected:
                return
            if time.monotonic() >= deadline:
                self.fail(f"editor never published {expected!r}; last value was {value!r}")

    def wait_for_compile_error(self, client: engine_client.EngineClient, cursor: object, script: str) -> dict:
        deadline = time.monotonic() + 30.0
        while True:
            errors = client.call("script.errors", {"since": cursor})["errors"]
            compile_errors = [row for row in errors if row["kind"] == "compile" and row["script"] == script]
            if compile_errors:
                return compile_errors[0]
            if time.monotonic() >= deadline:
                self.fail("the failed script import never reached script.errors")

    def poll_until_answered(self, client: engine_client.EngineClient, method: str,
                            deadline: float) -> tuple[dict | None, engine_client.EngineError | None]:
        """`method`'s result once the editor answers it, or the first Busy error; short bounded requests until
        `deadline` (a bound on a failure only, never asserted on)."""
        while time.monotonic() < deadline:
            try:
                return client.call(method, timeout=0.25), None
            except TimeoutError:
                continue
            except engine_client.EngineError as error:
                if error.code != engine_client.BUSY:
                    raise
                return None, error
        return None, None

    def test_runaway_script_busy_watchdog(self) -> None:
        # §13.2: a gameplay callback that never returns freezes the editor's frame loop outside any request; the I/O
        # thread then answers Busy naming the callback from the phase marker. The watchdog's threshold is 5 s of
        # wall-clock time without a pump, so the spin must outlive it: the raised 10 s callback budget arranges that.
        # Like test_busy_watchdog_reports_phase (ADR 0008 decision 15) this is a documented wall-clock exception; it
        # has no sleep, and its other waits are short bounded requests whose deadline only bounds a failure.
        client, _ = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Scripting": {"CallbackBudgetMs": 10000}}})
        client.call("script.write", {"path": "Assets/Scripts/Spin.luau", "source":
                                     "local Spin = {}\nfunction Spin:OnFixedUpdate() while true do end end\n"
                                     "return Script.Define(\"Spin\", Spin)\n"})
        client.call("entity.create", {"name": "Spinner", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Spin.luau"}]}}})
        observer = self.connect(self.editors[-1], "watchdog-observer")
        # Ordinary play: the editor's own loop runs OnFixedUpdate after this request returns.
        client.call("play.start")
        deadline = time.monotonic() + 45.0
        busy: engine_client.EngineError | None = None
        while busy is None and time.monotonic() < deadline:
            _, busy = self.poll_until_answered(observer, "session.info", deadline)
        self.assertIsNotNone(busy, "automation watchdog never reported Busy during the raised-budget callback")
        assert busy is not None
        self.assertEqual(busy.data["phase"], "Script:Spin.OnFixedUpdate")
        self.assertGreaterEqual(busy.data["stalledMilliseconds"], 5000)
        # The script watchdog ends the callback at its budget and disables the instance; the editor answers again.
        errors: list[dict] = []
        while not errors and time.monotonic() < deadline:
            answered, _ = self.poll_until_answered(observer, "script.errors", deadline)
            errors = [row for row in (answered or {}).get("errors", []) if row["kind"] == "timeout"]
        self.assertTrue(errors, "the runaway callback was not stopped by its budget")
        self.assertEqual((errors[0]["script"], errors[0]["callback"]), ("Assets/Scripts/Spin.luau", "OnFixedUpdate"))
        self.assertTrue(observer.call("session.info"))


if __name__ == "__main__":
    unittest.main()
