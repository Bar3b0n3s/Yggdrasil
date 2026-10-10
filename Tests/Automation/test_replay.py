"""M13 recording, replay verification and cancellation over the real automation protocol."""

from __future__ import annotations

import json
import time
import unittest

from harness import AutomationTestCase, engine_client


class ReplayTests(AutomationTestCase):
    def record(self, client: engine_client.EngineClient, path: str, ticks: int = 4) -> dict:
        client.call("scene.save")
        client.call("input.record", {"action": "start", "seed": 42, "parameters": {"level": 7}})
        client.call("play.step", {"ticks": ticks, "render": "none", "input": [
            {"type": "key", "key": "Space", "state": "tap"}]})
        return client.call("input.record", {"action": "stop", "path": path, "expect": [
            {"tick": 0, "luau": "Scene.GetLoadParameters().level == 7"},
            {"tick": ticks, "luau": f"Time.GetTick() == {ticks}"}]})

    def test_record_requires_tick_zero(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 2})
        before = client.call("play.state")
        with self.assertRaises(engine_client.EngineError) as rejected:
            client.call("input.record", {"action": "start"})
        self.assert_engine_error(rejected.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertEqual(client.call("play.state")["stateHash"], before["stateHash"])
        restarted = client.call("input.record", {"action": "start", "restart": True})
        self.assertEqual(restarted["tick"], 0)
        self.assertTrue(client.call("play.state")["recording"])
        for params in ({"action": "stop", "path": "../Escape.replay"},
                       {"action": "stop", "path": "Assets/Tests/Invalid.replay", "expect": [
                           {"tick": 1, "luau": "true"}]}):
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError):
                    client.call("input.record", params)
                self.assertTrue(client.call("play.state")["recording"])

    def test_record_and_verify_replay(self) -> None:
        client, root = self.open_editor_with_scene()
        path = "Assets/Tests/Verified.replay"
        saved = self.record(client, path)
        document = json.loads((root / path).read_text(encoding="utf-8"))
        self.assertEqual(document["Parameters"], {"level": 7})
        self.assertEqual(document["Seed"], 42)
        self.assertEqual(document["FinalTick"], 4)
        self.assertEqual([(row["Tick"], row["State"]) for row in document["Events"]], [(0, "Down"), (1, "Up")])
        result = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertTrue(result["passed"])
        self.assertTrue(result["hashMatched"])
        self.assertEqual(result["stateHash"], saved["stateHash"])
        self.assertTrue(all(row["satisfied"] for row in result["expect"]))
        state = client.call("play.state")
        self.assertFalse(state["lockstep"])
        self.assertEqual(state["state"], "Paused")
        self.assertEqual(state["tick"], 4)

    def test_replay_strict_hash(self) -> None:
        client, root = self.open_editor_with_scene()
        path = "Assets/Tests/Hash.replay"
        self.record(client, path)
        source = root / path
        document = json.loads(source.read_text(encoding="utf-8"))
        document["FinalStateHash"] = "0000000000000000" if document["FinalStateHash"] != "0000000000000000" else "0000000000000001"
        source.write_text(json.dumps(document), encoding="utf-8")
        client.call("asset.reimport", {"asset": path})
        with self.assertRaises(engine_client.EngineError) as mismatch:
            client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assert_engine_error(mismatch.exception, engine_client.VALIDATION_FAILED, "Validation")
        outcome = mismatch.exception.data["replay"]
        self.assertTrue(outcome["hashChecked"])
        self.assertFalse(outcome["hashMatched"])
        self.assertEqual(outcome["expectedStateHash"], document["FinalStateHash"])
        self.assertEqual(outcome["finalTick"], 4)
        self.assertEqual(len(outcome["expect"]), 2)
        relaxed = client.call("input.replay", {"path": path, "verify": True})
        self.assertTrue(relaxed["passed"])
        self.assertFalse(relaxed["hashChecked"])

    def test_record_disconnect_cancels_without_publishing(self) -> None:
        client, root = self.open_editor_with_scene()
        observer = self.connect(self.editors[-1], "record-observer")
        client.call("input.record", {"action": "start"})
        client.close()
        deadline = time.monotonic() + 30.0
        state = observer.call("play.state")
        while (state["lockstep"] or state["recording"]) and time.monotonic() < deadline:
            state = observer.call("play.state")
        self.assertFalse(state["lockstep"])
        self.assertFalse(state["recording"])
        self.assertEqual(state["state"], "Paused")
        self.assertEqual(list((root / "Assets").rglob("*.replay")), [])

    def test_recording_invalidates_before_external_predicate_mutation(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Target"})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.waitFor", {"until": 'Scene.FindByName("Target").Name = "Changed"; return true', "timeoutTicks": 1})
        path = "Assets/Tests/Invalid.replay"
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("input.record", {"action": "stop", "path": path})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertFalse((root / path).exists())

    def test_replay_faults_preserve_both_embedded_locations(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("input.record", {"action": "start"})
        expectation = {"tick": 0, "luau": 'return error("embedded failure", 0)'}
        path = "Assets/Tests/Errors.replay"
        client.call("input.record", {"action": "stop", "path": path, "expect": [expectation, expectation]})
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        with self.assertRaises(engine_client.EngineError) as failure:
            client.call("input.replay", {"path": path, "verify": True})
        outcomes = failure.exception.data["replay"]["expect"]
        self.assertEqual([row["jsonPointer"] for row in outcomes], ["/Expect/0/Luau", "/Expect/1/Luau"])
        first = client.call("script.errors", {"since": cursor, "limit": 1})
        second = client.call("script.errors", {"since": first["nextCursor"], "limit": 1})
        self.assertEqual(first["errors"][0]["jsonPointer"], "/Expect/0/Luau")
        self.assertEqual(second["errors"][0]["jsonPointer"], "/Expect/1/Luau")
        self.assertNotEqual(first["nextCursor"], second["nextCursor"])
        self.assertFalse(client.call("script.errors", {"since": second["nextCursor"]})["errors"])


if __name__ == "__main__":
    unittest.main()
