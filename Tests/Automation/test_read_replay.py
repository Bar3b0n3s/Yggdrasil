"""M19 input.readReplay over the real automation protocol (Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD,
stream B): a replay's events read back in play.step's input form, so a replay re-records from its own events. Its own
file, so that stream B owns it while stream A owns test_replay.py within W19.1 (decision 15)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

# A Mover that steps one unit along X per fixed step while Space is held, so a recording that presses Space depends on
# its input and passes input.record stop's input check (M16 friction S2-01, FX-4).
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


class ReadReplayTests(AutomationTestCase):
    @unittest.skip("contract stub: input.readReplay (Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD, "
                   "stream B)")
    def test_read_replay_returns_events_that_play_step_replays(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 14 (A-RREAD; ADR 0027 decision 12's deferral): input.readReplay
        # reads a replay back, its events in play.step's input form with absolute ticks, so a replay made with
        # input.record is re-recorded from its own events and reproduces its final hash.
        client, root = self.open_editor_with_scene()
        add_key_mover(client)
        client.call("scene.save")
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": 130, "render": "none", "input": [
            {"tick": 70, "type": "key", "key": "Space", "state": "down"},
            {"tick": 90, "type": "key", "key": "Space", "state": "up"}]})
        saved = client.call("input.record", {"action": "stop", "path": "Assets/Tests/Read.replay",
                                             "expect": [{"tick": 130, "luau": "Time.GetTick() == 130"}]})
        client.call("play.stop")
        read = client.call("input.readReplay", {"path": "Assets/Tests/Read.replay"})
        self.assertEqual(read["path"], "Assets/Tests/Read.replay")
        self.assertEqual(read["finalTick"], 130)
        self.assertEqual(read["finalStateHash"], saved["stateHash"])
        self.assertEqual(read["eventCount"], 2)
        self.assertEqual(read["events"][0], {"tick": 70, "type": "Key", "key": "Space", "state": "Down"})
        self.assertEqual(read["nextCursor"], "")
        self.assertEqual(read["expect"], [{"tick": 130, "luau": "Time.GetTick() == 130"}])
        self.assertEqual(len(read["checkpoints"]), 2)
        self.assertEqual(read["header"]["Scene"]["Path"], "Assets/Scenes/Main.scene")
        self.assertTrue(read["hasFingerprint"])
        client.call("input.record", {"action": "start"})
        client.call("play.step", {"ticks": read["finalTick"], "render": "none", "input": read["events"]})
        again = client.call("input.record", {"action": "stop", "path": "Assets/Tests/Again.replay",
                                             "expect": read["expect"]})
        self.assertEqual(again["stateHash"], saved["stateHash"])
        self.assertTrue((root / "Assets/Tests/Again.replay").exists())
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
