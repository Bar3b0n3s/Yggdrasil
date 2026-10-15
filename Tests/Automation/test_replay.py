"""M13 recording, replay verification and cancellation over the real automation protocol."""

from __future__ import annotations

import json
import time
import unittest

from harness import AutomationTestCase, engine_client

# A Mover that steps one unit along X per fixed step while Space is held: a recording that presses Space has an outcome
# that depends on its input, which input.record stop's input check requires (M16 friction S2-01, FX-4).
KEY_MOVER = """local KeyMover = {}
function KeyMover.OnFixedUpdate(self: any, dt: number)
\tif Input.IsKeyDown("Space") then
\t\tself.Entity.Transform:Translate(vector.create(1, 0, 0))
\tend
end
return Script.Define("KeyMover", KeyMover)
"""


def add_key_mover(client: engine_client.EngineClient) -> None:
    """Adds the KeyMover script and its "Mover" entity to the open scene (unsaved)."""
    client.call("script.write", {"path": "Assets/Scripts/KeyMover.luau", "source": KEY_MOVER})
    client.call("entity.create", {"name": "Mover",
                                   "components": {"Script": {"Script": "Assets/Scripts/KeyMover.luau"}}})


class ReplayTests(AutomationTestCase):
    def record(self, client: engine_client.EngineClient, path: str, ticks: int = 4) -> dict:
        add_key_mover(client)
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

    def test_replay_leaves_the_session_paused_at_its_last_tick_for_inspection(self) -> None:
        # input.replay's documented end state (M16 friction S1-09): the session waits paused at the replay's last tick,
        # script.eval and scene.query read that state, and play.stop ends it.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Probe"})
        path = "Assets/Tests/Inspected.replay"
        self.record(client, path, ticks=6)
        client.call("play.stop")
        result = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertTrue(result["passed"])
        self.assertEqual(result["finalTick"], 6)
        state = client.call("play.state")
        self.assertEqual(state["state"], "Paused")
        self.assertEqual(state["tick"], 6)
        self.assertEqual(state["stateHash"], result["stateHash"])
        evaluated = client.call("script.eval", {"context": "play", "code":
                                                "return {Time.GetTick(), Scene.GetLoadParameters().level}"})
        self.assertEqual(evaluated["value"], [6, 7])
        found = client.call("scene.query", {"where": {"name": "Probe"}})
        self.assertEqual(len(found["entities"]), 1)
        self.assertEqual(client.call("play.state")["tick"], 6)  # inspecting never steps the session
        client.call("play.stop")
        self.assertEqual(client.call("play.state")["state"], "Edit")

    def test_replay_strict_hash(self) -> None:
        client, root = self.open_editor_with_scene()
        path = "Assets/Tests/Hash.replay"
        self.record(client, path)
        source = root / path
        document = json.loads(source.read_text(encoding="utf-8"))
        document["FinalStateHash"] = ("0000000000000000" if document["FinalStateHash"] != "0000000000000000"
                                      else "0000000000000001")
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

    def test_replay_disconnect_releases_session(self) -> None:
        # §13.2 "Disconnect" over the real transport: a client killed during a pending input.replay leaves no lockstep,
        # stepping hold or replay input suppression behind; the session waits paused where the replay stopped.
        client, root = self.open_editor_with_scene()
        path = "Assets/Tests/Long.replay"
        self.record(client, path)
        source = root / path
        document = json.loads(source.read_text(encoding="utf-8"))
        final_tick = 50_000_000  # far more ticks than the test can run, so the disconnect always lands mid-replay
        document["FinalTick"] = final_tick
        document["Expect"] = []
        source.write_text(json.dumps(document), encoding="utf-8")
        client.call("asset.reimport", {"asset": path})
        client.call("play.stop")  # the recording client owns lockstep until then
        editor = self.editors[-1]
        observer = self.connect(editor, "replay-observer")
        assert editor.session is not None
        doomed = engine_client.EngineClient.connect_session(editor.session, client_name="replay-doomed")
        doomed.connection.sendall(engine_client.encode_frame(json.dumps(
            {"jsonrpc": "2.0", "id": 1, "method": "input.replay", "params": {"path": path}}).encode("utf-8")))
        # Requests of different clients have no order: wait (bounded) until the replay owns and advances the session.
        deadline = time.monotonic() + 30.0
        state = observer.call("play.state")
        while not (state["lockstepOwner"] == "replay-doomed" and state["tick"] > 0) and time.monotonic() < deadline:
            state = observer.call("play.state")
        self.assertEqual(state["lockstepOwner"], "replay-doomed", state)
        doomed.connection.close()
        deadline = time.monotonic() + 30.0
        while state["lockstep"] and time.monotonic() < deadline:
            state = observer.call("play.state")
        self.assertFalse(state["lockstep"], state)
        self.assertEqual(state["state"], "Paused")
        self.assertLess(state["tick"], final_tick)
        # Nothing of the replay holds the session: another client steps it and injects input.
        stepped = observer.call("play.step", {"ticks": 1, "render": "none"})
        self.assertEqual(stepped["tick"], state["tick"] + 1)
        observer.call("input.inject", {"events": [{"type": "key", "key": "Space", "state": "tap"}]})
        self.assertFalse(observer.call("play.state")["lockstep"])

    def test_recording_invalidates_before_external_predicate_mutation(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Target"})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.waitFor", {"until": 'Scene.FindByName("Target").Name = "Changed"; return true',
                                     "timeoutTicks": 1})
        path = "Assets/Tests/Invalid.replay"
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("input.record", {"action": "stop", "path": path})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertFalse((root / path).exists())

    def test_record_refuses_a_recording_whose_outcome_does_not_depend_on_its_input(self) -> None:
        # M16 friction S2-01 (FX-4): a recording with input events is replayed without them before it is written; when
        # that run reproduces every checkpoint, the final hash and every expectation, nothing is written.
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 70, "render": "none", "input": [
            {"tick": 2, "type": "key", "key": "Space", "state": "tap"}]})
        path = "Assets/Tests/Idle.replay"
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("input.record", {"action": "stop", "path": path})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("does not depend on its input", refused.exception.detail)
        self.assertIn(path, refused.exception.detail)
        self.assertIn("restart", refused.exception.data["hint"])
        # M16 friction S3-01: the hint states the input check's real rule, not a trailing restart as such.
        self.assertIn("never shows in the world at a checkpoint", refused.exception.data["hint"])
        check = refused.exception.data["inputCheck"]
        self.assertTrue(check["passed"])
        self.assertTrue(check["withoutInput"])
        self.assertEqual(check["checkpointsChecked"], 1)
        self.assertFalse((root / path).exists())
        state = client.call("play.state")
        self.assertFalse(state["recording"])
        self.assertEqual((state["state"], state["tick"]), ("Paused", 70))
        client.call("play.stop")

        # The same input on a scene that reacts to it is written, with its checkpoint and where the check ended.
        add_key_mover(client)
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 70, "render": "none", "input": [
            {"tick": 2, "type": "key", "key": "Space", "state": "tap"}]})
        saved = client.call("input.record", {"action": "stop", "path": path})
        self.assertEqual((saved["events"], saved["checkpoints"], saved["inputCheckTick"]), (2, 1, 60))
        document = json.loads((root / path).read_text(encoding="utf-8"))
        self.assertEqual(document["Version"], 2)
        self.assertEqual([checkpoint["Tick"] for checkpoint in document["Checkpoints"]], [60])

    def test_replay_stops_at_the_first_checkpoint_that_differs(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 130, "render": "none"})
        path = "Assets/Tests/Checkpoints.replay"
        saved = client.call("input.record", {"action": "stop", "path": path})
        self.assertEqual((saved["events"], saved["checkpoints"], saved["inputCheckTick"]), (0, 2, 0))
        source = root / path
        document = json.loads(source.read_text(encoding="utf-8"))
        self.assertEqual([checkpoint["Tick"] for checkpoint in document["Checkpoints"]], [60, 120])
        verified = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertEqual((verified["passed"], verified["checkpointsChecked"]), (True, 2))
        recorded = document["Checkpoints"][0]["StateHash"]
        document["Checkpoints"][0]["StateHash"] = "0000000000000001" if recorded == "0000000000000000" else \
            "0000000000000000"
        source.write_text(json.dumps(document), encoding="utf-8")
        client.call("asset.reimport", {"asset": path})
        with self.assertRaises(engine_client.EngineError) as mismatch:
            client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assert_engine_error(mismatch.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("checkpoint tick 60", mismatch.exception.detail)
        outcome = mismatch.exception.data["replay"]
        self.assertTrue(outcome["checkpointFailed"])
        self.assertEqual((outcome["finalTick"], outcome["checkpointsChecked"]), (60, 1))
        self.assertEqual((outcome["stateHash"], outcome["expectedStateHash"]),
                         (recorded, document["Checkpoints"][0]["StateHash"]))
        self.assertEqual(client.call("play.state")["tick"], 60)

    def test_replay_without_input_applies_none_of_its_events(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Tests/Pressed.replay"
        saved = self.record(client, path, ticks=8)
        self.assertGreater(saved["inputCheckTick"], 0)
        client.call("play.stop")
        with self.assertRaises(engine_client.EngineError) as failed:
            client.call("input.replay", {"path": path, "verify": True, "strictHash": True, "withoutInput": True})
        outcome = failed.exception.data["replay"]
        self.assertTrue(outcome["withoutInput"])
        self.assertFalse(outcome["hashMatched"])
        self.assertEqual(outcome["expectedStateHash"], saved["stateHash"])
        moved = client.call("script.eval", {"context": "play", "code":
                                            "return Scene.FindByName('Mover').Transform.WorldPosition.x"})
        self.assertEqual(moved["value"], 0)
        replayed = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertFalse(replayed["withoutInput"])
        self.assertTrue(replayed["passed"])
        # Without verify nothing would be checked and the input check would always pass (M16 review R19).
        with self.assertRaises(engine_client.EngineError) as unverified:
            client.call("input.replay", {"path": path, "withoutInput": True})
        self.assert_engine_error(unverified.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertIn("withoutInput requires verify:true", unverified.exception.detail)

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
