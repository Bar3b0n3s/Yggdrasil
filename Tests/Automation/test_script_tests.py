"""M13 test discovery, structured results and verified test-run recording."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client

SKIP_DIRTY_SCENE = "contract stub: test.run refuses a dirty open scene it plays (ADR 0027 decision 3, A2, stream A)"


class ScriptTestRunnerTests(AutomationTestCase):
    def configure_suite(self, client: engine_client.EngineClient, source: str) -> None:
        client.call("script.write", {"path": "Assets/Tests/Automation.test.luau", "source": source})
        client.call("scene.save")
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [{
            "Script": "Assets/Tests/Automation.test.luau", "Scene": "Assets/Scenes/Main.scene",
            "Modes": ["Editor"], "Parameters": {"level": 3}}]}}})

    def test_test_run_reports_failures_with_locations(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.configure_suite(client, 'return Test.Suite("Failures", function()\n'
                             'Test.Case("two failures", function()\n'
                             'Test.Expect(false, "first")\n'
                             'Test.Expect(false, "second")\nend)\n'
                             'Test.Case("continues", function() Test.Expect(true) end)\nend)')
        listed = client.call("test.list")
        cases = listed["suites"][0]["cases"]
        self.assertEqual([case["name"] for case in cases], ["two failures", "continues"])
        self.assertEqual(cases[0]["line"], 2)
        result = engine_client.load_offloaded(client.call("test.run", timeout=120.0))
        self.assertFalse(result["passed"])
        self.assertEqual([case["status"] for case in result["cases"]], ["failed", "passed"])
        failed = result["cases"][0]
        self.assertEqual((failed["message"], failed["file"], failed["line"]),
                         ("first", "Assets/Tests/Automation.test.luau", 3))
        self.assertEqual([row["line"] for row in failed["failures"]], [3, 4])
        self.assertTrue(Path(result["jsonPath"]).is_file())
        self.assertTrue(Path(result["junitPath"]).is_file())
        saved = json.loads(Path(result["jsonPath"]).read_text(encoding="utf-8"))
        self.assertEqual(saved["cases"], result["cases"])
        self.assertIn("second", Path(result["junitPath"]).read_text(encoding="utf-8"))
        self.assertEqual(client.call("play.state")["state"], "Edit")

    @unittest.skip(SKIP_DIRTY_SCENE)
    def test_test_run_refuses_a_dirty_open_scene_that_a_selected_suite_uses(self) -> None:
        # Docs/Decisions/0027-m17-contract.md decision 3, A2 (M17 map GTF-04): test.run plays the saved scene file, so a
        # run while the suite's scene was open with unsaved changes silently tested what the editor no longer showed.
        client, _ = self.open_editor_with_scene()
        self.configure_suite(client, 'return Test.Suite("Dirty", function()\n'
                             'Test.Case("sees the edit", function()\n'
                             'Test.Expect(Scene.FindByName("Unsaved") ~= nil)\n'
                             'end)\n'
                             'end)')
        client.call("entity.create", {"name": "Unsaved"})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("test.run", timeout=120.0)
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertIn("'Assets/Scenes/Main.scene' is open with unsaved changes", refused.exception.detail)
        self.assertIn("the selected suite 'Dirty' plays its saved file", refused.exception.detail)
        self.assertEqual(refused.exception.data["hint"], "scene.save first; test runs play the saved scene file")
        self.assertEqual(client.call("play.state")["state"], "Edit")
        # Saved, the suite plays what the editor shows.
        client.call("scene.save")
        result = engine_client.load_offloaded(client.call("test.run", timeout=120.0))
        self.assertTrue(result["passed"])
        self.assertEqual([case["status"] for case in result["cases"]], ["passed"])

    def test_test_run_returns_inline_and_names_the_failing_case(self) -> None:
        # Docs/Decisions/0026-m16-contract.md, friction S1-02: the coverage counters made every result too large to
        # send, and the offloaded summary hid the failures. Without coverage asked for, a run comes back inline.
        client, _ = self.open_editor_with_scene()
        cases = "".join(f'Test.Case("case {index}", function() Test.Expect(true) end)\n' for index in range(19))
        self.configure_suite(client, 'return Test.Suite("Inline", function()\n' + cases
                             + 'Test.Case("fails", function() Test.Expect(false, "expected failure") end)\nend)')
        result = client.call("test.run", timeout=120.0)
        self.assertNotIn("truncated", result)
        self.assertFalse(result["passed"])
        self.assertEqual(result["failedCases"], ["Inline/fails"])
        counts = result["counts"]
        self.assertEqual((counts["total"], counts["passed"], counts["failed"]), (20, 19, 1))
        self.assertEqual(result["coverage"]["counters"], [])
        report = json.loads(Path(result["jsonPath"]).read_text(encoding="utf-8"))
        self.assertTrue(report["coverage"]["counters"])
        self.assertEqual(report["cases"], result["cases"])
        covered = engine_client.load_offloaded(client.call("test.run", {"coverage": True}, timeout=120.0))
        self.assertEqual(covered["coverage"]["counters"], report["coverage"]["counters"])

    def test_reports_outlive_later_runs_and_offloaded_results(self) -> None:
        # Friction S1-03: an offloaded result once took the name of the previous run's JSON report and replaced it
        # (project:// left a .bak), and shutting down deleted the reports.
        editor = self.start_editor()
        client = self.connect(editor)
        root = self.create_project(client)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        self.configure_suite(client, 'return Test.Suite("Reports", function()\n'
                             'Test.Case("first", function() Test.Expect(true) end)\nend)')
        first = client.call("test.run", timeout=120.0)
        first_report = Path(first["jsonPath"]).read_bytes()
        discovered = client.call("rpc.discover")
        self.assertTrue(discovered["truncated"])
        second = client.call("test.run", timeout=120.0)
        names = [Path(path).name for path in (first["junitPath"], first["jsonPath"], discovered["path"],
                                               second["junitPath"], second["jsonPath"])]
        self.assertEqual(len(set(names)), 5, names)
        self.assertEqual(Path(first["jsonPath"]).read_bytes(), first_report)
        self.assertEqual(json.loads(first_report.decode("utf-8"))["cases"], first["cases"])
        out = root / "Library" / "Automation" / "Out"
        self.assertEqual(sorted(path.name for path in out.iterdir() if path.name.endswith(".bak")), [])
        client.call("session.shutdown", {"force": True})
        self.assertEqual(editor.wait(), 0)
        # The offloaded result goes with its editor; the reports stay.
        self.assertFalse(Path(discovered["path"]).exists())
        for path in (first["jsonPath"], first["junitPath"], second["jsonPath"], second["junitPath"]):
            self.assertTrue(Path(path).is_file(), path)

    def test_test_run_record_writes_replay(self) -> None:
        client, root = self.open_editor_with_scene()
        self.configure_suite(client, 'return Test.Suite("Recorded", function()\n'
                             'Test.Case("wait", function() Test.WaitTicks(3) end)\nend)')
        path = "Assets/Tests/Replays/Recorded.replay"
        result = engine_client.load_offloaded(client.call(
            "test.run", {"filter": "rEcOrDeD/wAiT", "record": path}, timeout=120.0))
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["recordingPath"], path)
        self.assertTrue((root / path).is_file())
        playback = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertTrue(playback["hashMatched"])
        self.assertEqual(playback["stateHash"], result["suites"][0]["finalStateHash"])
        # M16 friction S3-03: the recording block describes the replay written; with no input there is nothing to check.
        recorded = json.loads((root / path).read_text(encoding="utf-8"))
        self.assertEqual(result["recording"], {
            "path": path, "formatVersion": recorded["Version"], "ticks": recorded["FinalTick"], "events": 0,
            "checkpoints": 0, "finalStateHash": recorded["FinalStateHash"],
            "inputCheck": {"ran": False, "divergedAtTick": 0}})
        plain = engine_client.load_offloaded(client.call("test.run", {"filter": "Recorded"}, timeout=120.0))
        self.assertIsNone(plain["recording"])

    def test_record_captures_test_inject_and_parameters(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Input": {"Actions": {
            "Jump": {"Type": "Button", "Bindings": ["Key.Space"]}}}}})
        # The scene reacts to the injected key, so the recording's outcome depends on its input, which the recording's
        # input check requires (M16 friction S2-01, FX-4).
        client.call("script.write", {"path": "Assets/Scripts/Hopper.luau", "source":
                    "local Hopper = {}\nfunction Hopper.OnFixedUpdate(self: any, dt: number)\n"
                    "\tif Input.IsKeyDown(\"A\") then\n\t\tself.Entity.Transform:Translate(vector.create(0, 1, 0))\n"
                    "\tend\nend\nreturn Script.Define(\"Hopper\", Hopper)\n"})
        client.call("entity.create", {"name": "Hopper",
                                      "components": {"Script": {"Script": "Assets/Scripts/Hopper.luau"}}})
        self.configure_suite(client, 'return Test.Suite("Inputs", function()\n'
                             'Test.Case("inject", function()\n'
                             'Test.ExpectEqual(Scene.GetLoadParameters().level, 3)\n'
                             'Test.InjectKey("A", "Tap")\n'
                             'Test.InjectAction("Jump", "Tap")\n'
                             'Test.WaitTicks(4)\nend)\nend)')
        path = "Assets/Tests/Replays/Inputs.replay"
        result = engine_client.load_offloaded(client.call(
            "test.run", {"record": path}, timeout=120.0))
        self.assertTrue(result["passed"], result)
        recorded = json.loads((root / path).read_text(encoding="utf-8"))
        self.assertEqual(recorded["Parameters"], {"level": 3})
        events = recorded["Events"]
        keys = [event for event in events if event["Type"] == "Key"]
        actions = [event for event in events if event["Type"] == "Action"]
        self.assertEqual([(event["Key"], event["State"]) for event in keys], [("A", "Down"), ("A", "Up")])
        self.assertEqual([(event["Name"], event["State"]) for event in actions], [("Jump", "Down"), ("Jump", "Up")])
        self.assertEqual(keys[1]["Tick"], keys[0]["Tick"] + 1)
        self.assertTrue(client.call("input.replay", {"path": path, "verify": True, "strictHash": True})["passed"])
        # M16 friction S3-03: the recording block; the run without input differed only at the final tick (no checkpoint
        # in a recording this short), where the Hopper had not moved.
        recording = result["recording"]
        self.assertEqual((recording["path"], recording["events"], recording["checkpoints"], recording["ticks"]),
                         (path, len(events), len(recorded["Checkpoints"]), recorded["FinalTick"]))
        self.assertEqual(recording["finalStateHash"], recorded["FinalStateHash"])
        self.assertEqual(recording["inputCheck"], {"ran": True, "divergedAtTick": recorded["FinalTick"]})

    def test_test_run_rejects_missing_selection_without_writing(self) -> None:
        client, root = self.open_editor_with_scene()
        self.configure_suite(client, 'return Test.Suite("Only", function() Test.Case("pass", function() end) end)')
        path = "Assets/Tests/Replays/Unwritten.replay"
        with self.assertRaises(engine_client.EngineError) as selection:
            client.call("test.run", {"filter": "no matching case", "record": path})
        self.assert_engine_error(selection.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertFalse((root / path).exists())
        self.assertEqual(client.call("play.state")["state"], "Edit")


if __name__ == "__main__":
    unittest.main()
