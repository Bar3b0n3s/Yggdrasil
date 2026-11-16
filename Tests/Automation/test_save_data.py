"""M28's save data through automation (Docs/Decisions/0043-m28-game-services.md decision 8): editor play's store and the
savedata.get, savedata.set and savedata.clear methods, a live session that starts with it and commits to it, a recording
that records it and a replay that verifies whatever the machine's store holds, and a corrupt store moved aside."""

from __future__ import annotations

import json
import unittest

from harness import AutomationTestCase, engine_client

# A Saver that starts at the height its save data says (so the state hash depends on the store) and steps one unit along X
# per fixed step while Space is held (so a recording's outcome depends on its input, which input.record stop checks).
SAVER = """local Saver = {}
function Saver.OnCreate(self: any)
\tlocal realm = SaveData.Get("Realm")
\tself.Entity.Transform:Translate(vector.create(0, if type(realm) == "number" then realm else 0, 0))
end
function Saver.OnFixedUpdate(self: any, dt: number)
\tif Input.IsKeyDown("Space") then
\t\tself.Entity.Transform:Translate(vector.create(1, 0, 0))
\tend
end
return Script.Define("Saver", Saver)
"""


class SaveDataTests(AutomationTestCase):
    def add_saver(self, client: engine_client.EngineClient) -> None:
        client.call("script.write", {"path": "Assets/Scripts/Saver.luau", "source": SAVER})
        client.call("entity.create", {"name": "Saver",
                                       "components": {"Script": {"Scripts": [{"Script": "Assets/Scripts/Saver.luau"}]}}})
        client.call("scene.save")

    def test_save_data_methods_seed_read_merge_and_clear_the_play_store(self) -> None:
        client, root = self.open_editor_with_scene()
        empty = client.call("savedata.get")
        self.assertEqual((empty["values"], empty["count"], empty["loadError"]), ({}, 0, ""))
        written = client.call("savedata.set", {"values": {"Realm": 2, "Options": {"Volume": 0.5}}})
        self.assertEqual(written["count"], 2)
        self.assertEqual(client.call("savedata.get")["values"], {"Options": {"Volume": 0.5}, "Realm": 2})
        merged = client.call("savedata.set", {"values": {"Realm": None, "Name": "Ash"}, "merge": True})
        self.assertEqual(merged["count"], 2)
        self.assertEqual(client.call("savedata.get")["values"], {"Name": "Ash", "Options": {"Volume": 0.5}})
        stored = json.loads((root / "Library" / "SaveData" / "Store.json").read_text(encoding="utf-8"))
        self.assertEqual((stored["Format"], stored["Version"]), ("SaveData", 1))
        self.assertEqual(stored["Values"], {"Name": "Ash", "Options": {"Volume": 0.5}})
        self.assertEqual(client.call("savedata.clear")["count"], 0)
        self.assertEqual(client.call("savedata.get")["values"], {})
        for params in ({"values": [1, 2]}, {"values": {"": 1}}, {"values": {"Key": "x" * 1_100_000}}):
            with self.subTest(params=list(params["values"])[:1]):
                with self.assertRaises(engine_client.EngineError):
                    client.call("savedata.set", params)
        self.assertEqual(client.call("savedata.get")["values"], {})
        with self.assertRaises(engine_client.EngineError) as no_session:
            client.call("savedata.get", {"target": "Play"})
        self.assert_engine_error(no_session.exception, engine_client.INVALID_STATE, "InvalidState")

    def test_live_play_starts_with_the_play_store_and_commit_writes_it(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("savedata.set", {"values": {"Realm": 2}})
        client.call("play.start")
        self.assertEqual(client.call("script.eval", {"context": "play", "code": "return SaveData.Get('Realm')"})["value"], 2)
        committed = client.call("script.eval", {"context": "play",
                                                "code": "SaveData.Set('Best', 12.5) return { SaveData.Commit() }"})
        self.assertEqual(committed["value"], [True])
        self.assertEqual(client.call("savedata.get", {"target": "Play"})["values"], {"Best": 12.5, "Realm": 2})
        client.call("play.stop")
        self.assertEqual(client.call("savedata.get")["values"], {"Best": 12.5, "Realm": 2})
        # A lockstep session keeps its store in memory: it starts empty and its commit writes nothing.
        client.call("play.start", {"lockstep": True})
        self.assertEqual(client.call("savedata.get", {"target": "Play"})["values"], {})
        client.call("script.eval", {"context": "play", "code": "SaveData.Set('Best', 1) return SaveData.Commit()"})
        client.call("play.stop")
        self.assertEqual(client.call("savedata.get")["values"]["Best"], 12.5)
        stored = json.loads((root / "Library" / "SaveData" / "Store.json").read_text(encoding="utf-8"))
        self.assertEqual(stored["Values"], {"Best": 12.5, "Realm": 2})

    def test_a_recording_records_the_play_store_and_its_replay_verifies_whatever_the_store_holds(self) -> None:
        client, root = self.open_editor_with_scene()
        self.add_saver(client)
        client.call("savedata.set", {"values": {"Realm": 2, "Ratio": 0.1}})
        client.call("input.record", {"action": "start", "seed": 42})
        client.call("play.step", {"ticks": 4, "render": "none", "input": [{"type": "key", "key": "Space", "state": "tap"}]})
        client.call("input.record", {"action": "stop", "path": "Assets/Tests/Replays/Saved.replay",
                                     "expect": [{"tick": 4, "luau": "SaveData.Get('Realm') == 2"}]})
        client.call("play.stop")
        replay = json.loads((root / "Assets" / "Tests" / "Replays" / "Saved.replay").read_text(encoding="utf-8"))
        self.assertEqual(replay["Version"], 4)
        self.assertEqual(replay["SaveData"]["Realm"], 2)
        self.assertAlmostEqual(replay["SaveData"]["Ratio"], 0.1, places=6)
        # The machine's store holds something else now: the replay starts with its own and verifies at its hash.
        client.call("savedata.set", {"values": {"Realm": 9}})
        verified = client.call("input.replay", {"path": "Assets/Tests/Replays/Saved.replay", "verify": True,
                                                "strictHash": True})
        self.assertTrue(verified["passed"], json.dumps(verified))

    def test_a_corrupt_play_store_is_moved_aside_and_reported(self) -> None:
        client, root = self.open_editor_with_scene()
        directory = root / "Library" / "SaveData"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "Store.json").write_bytes(b'{"Format": "SaveData", "Version": 1, "Values": {"Realm"')
        read = client.call("savedata.get")
        self.assertEqual(read["values"], {})
        self.assertIn("Store.corrupt-1.json", read["loadError"])
        self.assertTrue((directory / "Store.corrupt-1.json").is_file())
        self.assertFalse((directory / "Store.json").exists())
        client.call("play.start")
        self.assertEqual(client.call("script.eval", {"context": "play", "code": "return SaveData.GetLoadError()"})["value"],
                         None)
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
