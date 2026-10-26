"""M19 input.readReplay over the real automation protocol (Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD,
stream B): a replay's events read back in play.step's input form, so a replay re-records from its own events. Its own
file, so that stream B owns it while stream A owns test_replay.py within W19.1 (decision 15)."""

from __future__ import annotations

import json
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

    def test_read_replay_pages_an_upgraded_replay_and_refuses_bad_requests(self) -> None:
        # Decision 14: the file's own version is reported and the document comes back migrated (a version 2 replay has
        # no fingerprint); the events come a page at a time; a missing replay, a path outside Assets/ and a cursor this
        # replay never gave are refused, located at their param.
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        scene = client.call("asset.list", {"dir": "Assets/Scenes"})["assets"][0]
        events = [{"Tick": tick, "Type": "Key", "Key": "Space", "State": "Down" if tick % 2 == 0 else "Up"}
                  for tick in range(5)]
        document = {"Format": "Replay", "Version": 2, "Scene": {"Handle": scene["id"], "Path": scene["path"]},
                    "Parameters": {}, "Seed": 7, "FixedHz": 60, "EngineVersion": "0.1.0", "Config": "Editor",
                    "Events": events, "Expect": [], "Checkpoints": [], "FinalTick": 10,
                    "FinalStateHash": "0123456789abcdef"}
        replay = root / "Assets/Tests/Old.replay"
        replay.parent.mkdir(parents=True, exist_ok=True)
        replay.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")

        first = client.call("input.readReplay", {"path": "project://Assets/Tests/Old.replay", "limit": 2})
        self.assertEqual(first["path"], "Assets/Tests/Old.replay")
        self.assertEqual(first["formatVersion"], 2)
        self.assertFalse(first["hasFingerprint"])
        self.assertEqual(first["header"]["Seed"], 7)
        self.assertEqual(first["eventCount"], 5)
        self.assertEqual([event["tick"] for event in first["events"]], [0, 1])
        pages = [first]
        while pages[-1]["nextCursor"]:
            pages.append(client.call("input.readReplay", {"path": "Assets/Tests/Old.replay", "limit": 2,
                                                          "cursor": pages[-1]["nextCursor"]}))
        self.assertEqual([event["tick"] for page in pages for event in page["events"]], [0, 1, 2, 3, 4])
        self.assertEqual(pages[-1]["events"][0], {"tick": 4, "type": "Key", "key": "Space", "state": "Down"})

        refused = {"missing": ({"path": "Assets/Tests/Missing.replay"}, "NotFound", "/path"),
                   "outside": ({"path": "Library/Old.replay"}, "InvalidArgument", "/path"),
                   "cursor": ({"path": "Assets/Tests/Old.replay", "cursor": "5"}, "InvalidArgument", "/cursor")}
        for label, (params, error_code, pointer) in refused.items():
            with self.subTest(label), self.assertRaises(engine_client.EngineError) as caught:
                client.call("input.readReplay", params)
            self.assertEqual(caught.exception.error_code, error_code)
            self.assertEqual(caught.exception.data.get("location", {}).get("pointer"), pointer)


if __name__ == "__main__":
    unittest.main()
