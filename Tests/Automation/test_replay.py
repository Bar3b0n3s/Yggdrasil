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


# A Booster that steps one unit along X per fixed step while Space is held or while its own table says so: a script
# table that script.eval can write without any engine call, so nothing invalidates the recording.
BOOSTER = """local Booster = {}
function Booster.OnFixedUpdate(self: any, dt: number)
\tif Input.IsKeyDown("Space") or self.Boost then
\t\tself.Entity.Transform:Translate(vector.create(1, 0, 0))
\tend
end
return Script.Define("Booster", Booster)
"""


# What input.replay {withoutInput: true}'s failures say instead of the divergence hint (ReplayMethods.cpp).
WITHOUT_INPUT_NOTE = ("the replay ran without its input events (withoutInput), which a replay that depends on its "
                      "input is expected to fail")


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
        # M17 friction F03: a state hash that differs says what changes the world and how to re-record.
        self.assertIn("the world differs from the recording", mismatch.exception.data["hint"])
        # M17 review D-06/R-03: the hint names the method engine_client calls, never the MCP tool.
        self.assertIn('re-record it with test.run {record} (the game-building skill, "Re-recording replays")',
                      mismatch.exception.data["hint"])
        self.assertNotIn("test_run", mismatch.exception.data["hint"])
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

    @unittest.skip("contract stub: input.replay continuePastCheckpoint (Docs/Decisions/0030-m19-contract.md decision "
                   "3, A-F29; streams A and B, lifted by the integration)")
    def test_replay_continue_past_checkpoint_counts_every_failed_checkpoint(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 3 (A-F29): continuePastCheckpoint plays on past every checkpoint
        # whose hash differs to the final tick and compares the final hash too; the replay still fails, counting the
        # failed checkpoints and naming the first, and the session waits at the final tick. It needs strictHash.
        client, _ = self.open_editor_with_scene()
        add_key_mover(client)
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        # Space is held from tick 50: checkpoints 60 and 120 and the final hash differ from a run without input.
        client.call("play.step", {"ticks": 130, "render": "none", "input": [
            {"tick": 50, "type": "key", "key": "Space", "state": "down"}]})
        path = "Assets/Tests/Held.replay"
        saved = client.call("input.record", {"action": "stop", "path": path})
        self.assertEqual(saved["checkpoints"], 2)
        client.call("play.stop")
        with self.assertRaises(engine_client.EngineError) as failed:
            client.call("input.replay", {"path": path, "verify": True, "strictHash": True, "withoutInput": True,
                                         "continuePastCheckpoint": True})
        self.assert_engine_error(failed.exception, engine_client.VALIDATION_FAILED, "Validation")
        outcome = failed.exception.data["replay"]
        self.assertFalse(outcome["passed"])
        self.assertTrue(outcome["checkpointFailed"])
        self.assertEqual((outcome["checkpointsChecked"], outcome["checkpointsFailed"],
                          outcome["firstFailedCheckpointTick"]), (2, 2, 60))
        self.assertEqual((outcome["finalTick"], outcome["hashMatched"]), (130, False))
        self.assertEqual(client.call("play.state")["tick"], 130)
        client.call("play.stop")
        with self.assertRaises(engine_client.EngineError) as unstrict:
            client.call("input.replay", {"path": path, "verify": True, "continuePastCheckpoint": True})
        self.assert_engine_error(unstrict.exception, engine_client.INVALID_PARAMS, "InvalidArgument")

    @unittest.skip("contract stub: the replay start fingerprint's changes (Docs/Decisions/0030-m19-contract.md "
                   "decision 2, A-FPRINT; streams A and B, lifted by the integration)")
    def test_replay_names_the_script_and_material_that_changed_since_the_recording(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 2 (A-FPRINT; ADR 0027 friction F13): a replay records its start
        # fingerprint, so a playback whose hash differs names what changed since the recording in its result's changes,
        # and its error's message ends with the first.
        client, _ = self.open_editor_with_scene()
        add_key_mover(client)
        client.call("asset.create", {"type": "Material", "path": "Assets/Materials/Red.material",
                                     "values": {"Roughness": 0.45}})
        client.call("entity.create", {"name": "Box", "components": {"MeshRenderer": {
            "Mesh": "engine://Meshes/Cube", "Materials": ["Assets/Materials/Red.material"]}}})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 70, "render": "none", "input": [
            {"tick": 3, "type": "key", "key": "Space", "state": "down"}]})
        path = "Assets/Tests/Changed.replay"
        client.call("input.record", {"action": "stop", "path": path})
        client.call("play.stop")
        # The mover now moves two units per tick, so the hash differs; the material changes too.
        client.call("script.write", {"path": "Assets/Scripts/KeyMover.luau",
                                     "source": KEY_MOVER.replace("vector.create(1, 0, 0)", "vector.create(2, 0, 0)")})
        client.call("asset.setProperties", {"asset": "Assets/Materials/Red.material", "values": {"Roughness": 0.9}})
        with self.assertRaises(engine_client.EngineError) as changed:
            client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assert_engine_error(changed.exception, engine_client.VALIDATION_FAILED, "Validation")
        changes = changed.exception.data["replay"]["changes"]
        self.assertTrue(any(line.startswith("Script Assets/Scripts/KeyMover.luau (") for line in changes), changes)
        self.assertTrue(any(line.startswith("Material Assets/Materials/Red.material (") for line in changes), changes)
        self.assertIn("; changed since the recording: ", changed.exception.detail)
        client.call("play.stop")

    @unittest.skip("contract stub: input.record's final-hash warning (Docs/Decisions/0030-m19-contract.md decision 3, "
                   "A-F29; streams A and B, lifted by the integration)")
    def test_record_warns_when_only_the_final_hash_does_not_depend_on_the_input(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 3 (A-F29; ADR 0026 friction F29): a recording whose input shows
        # at its checkpoints but whose end state a reset restores passes the input check and is written, with one
        # warning in warnings that its final hash alone does not depend on its input; a recording whose final state
        # shows its input gets none.
        client, root = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/Resetter.luau", "source":
                    "local Resetter = {}\nfunction Resetter.OnFixedUpdate(self: any, dt: number)\n"
                    "\tif Input.IsKeyDown(\"Space\") then self.Entity.Transform:Translate(vector.create(1, 0, 0)) end\n"
                    "\tif Time.GetTick() == 125 then self.Entity.Transform.Translation = vector.create(0, 0, 0) end\n"
                    "end\nreturn Script.Define(\"Resetter\", Resetter)\n"})
        client.call("entity.create", {"name": "Resetter",
                                       "components": {"Script": {"Script": "Assets/Scripts/Resetter.luau"}}})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 130, "render": "none", "input": [
            {"tick": 10, "type": "key", "key": "Space", "state": "down"},
            {"tick": 70, "type": "key", "key": "Space", "state": "up"}]})
        path = "Assets/Tests/Reset.replay"
        saved = client.call("input.record", {"action": "stop", "path": path})
        self.assertTrue((root / path).exists())
        self.assertEqual(saved["inputCheckTick"], 60)
        self.assertEqual(len(saved["warnings"]), 1, saved["warnings"])
        self.assertIn(path, saved["warnings"][0])
        self.assertIn("does not depend on its input", saved["warnings"][0])
        self.assertEqual(client.call("play.state")["tick"], 130)
        client.call("play.stop")

        add_key_mover(client)
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 130, "render": "none", "input": [
            {"tick": 100, "type": "key", "key": "Space", "state": "down"}]})
        plain = client.call("input.record", {"action": "stop", "path": "Assets/Tests/Plain.replay"})
        self.assertEqual(plain["warnings"], [])
        client.call("play.stop")

    def test_record_refuses_a_recording_changed_by_a_script_table_write(self) -> None:
        # Docs/Decisions/0027-m17-contract.md decision 3, A1 (M17 maps GTF-01, GDF-03, GAF-09): a script.eval that
        # writes only a script table during a recording calls no engine API, so nothing invalidates the recording, and
        # Stop played it only without its input, which the boosted world fails too, so it wrote a replay that its own
        # input does not reproduce. Stop now replays the recording with its input first and refuses it there.
        client, root = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/Booster.luau", "source": BOOSTER})
        client.call("entity.create", {"name": "Booster",
                                       "components": {"Script": {"Script": "Assets/Scripts/Booster.luau"}}})
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 10, "render": "none", "input": [
            {"tick": 2, "type": "key", "key": "Space", "state": "tap"}]})
        client.call("script.eval", {"context": "play", "entity": "/Booster", "code": "self.Boost = true; return true"})
        client.call("play.step", {"ticks": 120, "render": "none"})
        path = "Assets/Tests/Boosted.replay"
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("input.record", {"action": "stop", "path": path})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "Validation")
        detail = refused.exception.detail
        self.assertIn(f"refusing to write '{path}'", detail)
        self.assertIn("replayed with its input, the recording does not reproduce itself", detail)
        self.assertIn("checkpoint tick 60", detail)
        self.assertIn("neither recorded nor detected", refused.exception.data["hint"])
        verification = refused.exception.data["verification"]
        self.assertTrue(verification["checkpointFailed"])
        self.assertFalse(verification["withoutInput"])
        self.assertEqual(verification["finalTick"], 60)
        self.assertIn(verification["stateHash"], detail)
        self.assertIn(verification["expectedStateHash"], detail)
        # M17 review C-01: the recording was made moments before, so its stop reason blames no change since then.
        self.assertEqual(verification["stopReason"],
                         f"the state hash at checkpoint tick 60 is {verification['stateHash']}, "
                         f"but the recording's is {verification['expectedStateHash']}")
        self.assertNotIn("changed since it was recorded", json.dumps(refused.exception.data))
        self.assertFalse((root / path).exists())
        state = client.call("play.state")
        self.assertFalse(state["recording"])
        self.assertFalse(state["lockstep"])
        self.assertEqual((state["state"], state["tick"]), ("Paused", 60))
        client.call("play.stop")

        # The same play without the evaluation reproduces itself, fails without its input and is written.
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 130, "render": "none", "input": [
            {"tick": 2, "type": "key", "key": "Space", "state": "tap"}]})
        saved = client.call("input.record", {"action": "stop", "path": path})
        self.assertEqual((saved["events"], saved["checkpoints"], saved["inputCheckTick"]), (2, 2, 60))
        self.assertTrue((root / path).exists())
        verified = client.call("input.replay", {"path": path, "verify": True, "strictHash": True})
        self.assertTrue(verified["passed"])
        self.assertEqual(verified["stateHash"], saved["stateHash"])

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
        self.assertIn("(the look's included)", mismatch.exception.data["hint"])
        outcome = mismatch.exception.data["replay"]
        self.assertTrue(outcome["checkpointFailed"])
        self.assertIn("(the look's included)", outcome["stopReason"])
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
        self.assert_engine_error(failed.exception, engine_client.VALIDATION_FAILED, "Validation")
        # The input check's failure is the expected outcome for a replay with input events: no divergence hint asks for
        # a re-recording (M17 friction F03 belongs to a playback with the input), and the message says what ran.
        self.assertEqual(failed.exception.detail, "replay verification failed at tick 8; " + WITHOUT_INPUT_NOTE)
        self.assertNotIn("hint", failed.exception.data)
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

        # A longer replay fails without its input at its first checkpoint, whose stop reason names no divergence causes.
        client.call("play.stop")
        long_path = "Assets/Tests/PressedLong.replay"
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 70, "render": "none", "input": [
            {"tick": 2, "type": "key", "key": "Space", "state": "tap"}]})
        client.call("input.record", {"action": "stop", "path": long_path})
        client.call("play.stop")
        with self.assertRaises(engine_client.EngineError) as diverged:
            client.call("input.replay", {"path": long_path, "verify": True, "strictHash": True, "withoutInput": True})
        self.assert_engine_error(diverged.exception, engine_client.VALIDATION_FAILED, "Validation")
        self.assertIn("checkpoint tick 60", diverged.exception.detail)
        self.assertTrue(diverged.exception.detail.endswith("; " + WITHOUT_INPUT_NOTE))
        self.assertNotIn("hint", diverged.exception.data)
        check = diverged.exception.data["replay"]
        self.assertTrue(check["checkpointFailed"])
        self.assertIn("the replay ran without its input events", check["stopReason"])
        self.assertNotIn("(the look's included)", check["stopReason"])

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
