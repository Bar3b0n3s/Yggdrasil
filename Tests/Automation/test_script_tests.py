"""M13 test discovery, structured results and verified test-run recording."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client


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

    def test_record_captures_test_inject_and_parameters(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Input": {"Actions": {
            "Jump": {"Type": "Button", "Bindings": ["Key.Space"]}}}}})
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
