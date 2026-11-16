"""M28's save data through automation (Docs/Decisions/0043-m28-game-services.md decision 8): editor play's store and the
savedata.get, savedata.set and savedata.clear methods, a live session that starts with it and commits to it, a recording
that records it and a replay that verifies whatever the machine's store holds, and a corrupt store moved aside."""

from __future__ import annotations

import json
import math
import unittest

from harness import AutomationTestCase, engine_client

# A Saver that starts at the height its save data says (so the state hash depends on the store) and steps one unit
# along X per fixed step while Space is held (so a recording's outcome depends on its input, which input.record stop
# checks).
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
        scripts = {"Scripts": [{"Script": "Assets/Scripts/Saver.luau"}]}
        client.call("entity.create", {"name": "Saver", "components": {"Script": scripts}})
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
        realm = client.call("script.eval", {"context": "play", "code": "return SaveData.Get('Realm')"})
        self.assertEqual(realm["value"], 2)
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
        # A large count and precise doubles, which a float would round (16,777,217 to 16,777,216): the replay's
        # header keeps the store's doubles exactly (ADR 0043 decision 7), and the recorded run reads them as stored.
        # -0 and magnitudes of 2^63 or more too (the W28.2 review's findings 2 and 3): the header keeps -0's sign and
        # writes 2^64 in a form the reader accepts.
        store = {"Realm": 2, "Ratio": 0.1, "Count": 16777217, "Stamp": 1731700000.123456, "Zero": -0.0,
                 "Huge": 1.8446744073709552e19, "Low": -9.223372036854778e18}
        client.call("savedata.set", {"values": store})
        client.call("input.record", {"action": "start", "seed": 42})
        client.call("play.step", {"ticks": 4, "render": "none",
                                  "input": [{"type": "key", "key": "Space", "state": "tap"}]})
        exact = ("SaveData.Get('Realm') == 2 and SaveData.Get('Ratio') == 0.1 and SaveData.Get('Count') == 16777217"
                 " and SaveData.Get('Stamp') == 1731700000.123456 and 1 / SaveData.Get('Zero') == -math.huge"
                 " and SaveData.Get('Huge') == 1.8446744073709552e19 and SaveData.Get('Low') == -9.223372036854778e18")
        client.call("input.record", {"action": "stop", "path": "Assets/Tests/Replays/Saved.replay",
                                     "expect": [{"tick": 4, "luau": exact}]})
        client.call("play.stop")
        replay = json.loads((root / "Assets" / "Tests" / "Replays" / "Saved.replay").read_text(encoding="utf-8"))
        self.assertEqual(replay["Version"], 4)
        self.assertEqual(replay["SaveData"], store)
        self.assertEqual(math.copysign(1.0, replay["SaveData"]["Zero"]), -1.0)
        # The machine's store holds something else now: the replay starts with its own and verifies at its hash.
        client.call("savedata.set", {"values": {"Realm": 9}})
        verified = client.call("input.replay", {"path": "Assets/Tests/Replays/Saved.replay", "verify": True,
                                                "strictHash": True})
        self.assertTrue(verified["passed"], json.dumps(verified))

    def test_a_read_only_editor_keeps_a_private_store_and_writes_no_store_file(self) -> None:
        # The W28.2 review's finding 1: a --read-only editor's play store lives in memory, so seeding it, a live
        # session's commit and clearing it never write Library/SaveData/Store.json, and the next read-only editor of the
        # project starts empty (a project opened next in the same editor: the EditorPlayController unit case).
        client, root = self.open_editor_with_scene()
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        store = root / "Library" / "SaveData" / "Store.json"
        reader = self.connect(self.start_editor(["--read-only"], project=root))
        reader.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        reader.call("savedata.set", {"values": {"Realm": 2}})
        reader.call("play.start")
        committed = reader.call("script.eval", {"context": "play",
                                                "code": "SaveData.Set('Best', 7) return { SaveData.Commit() }"})
        self.assertEqual(committed["value"], [True])
        reader.call("play.stop")
        self.assertEqual(reader.call("savedata.get")["values"], {"Best": 7, "Realm": 2})
        # The next play starts with the private store.
        reader.call("play.start")
        best = reader.call("script.eval", {"context": "play", "code": "return SaveData.Get('Best')"})
        self.assertEqual(best["value"], 7)
        reader.call("play.stop")
        self.assertFalse(store.exists())
        reader.call("savedata.clear")
        self.assertEqual(reader.call("savedata.get")["values"], {})
        reader.call("savedata.set", {"values": {"Realm": 3}})
        reader.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        self.assertFalse(store.exists())
        second = self.connect(self.start_editor(["--read-only"], project=root))
        self.assertEqual(second.call("savedata.get")["values"], {})
        self.assertFalse(store.exists())

    def test_a_save_data_value_nests_to_its_limit_and_no_deeper(self) -> None:
        # The W28.2 review's finding 4: a value nests at most 64 arrays and tables (MaxSaveDataDepth), far below the
        # readers' depth, so savedata.get, the store file and a replay header can always hold it.
        client, root = self.open_editor_with_scene()
        deep: object = []
        for _ in range(63):
            deep = {"Next": deep}
        client.call("savedata.set", {"values": {"Deep": deep}})
        self.assertEqual(client.call("savedata.get")["values"], {"Deep": deep})
        stored = json.loads((root / "Library" / "SaveData" / "Store.json").read_text(encoding="utf-8"))
        self.assertEqual(stored["Values"], {"Deep": deep})
        with self.assertRaises(engine_client.EngineError):
            client.call("savedata.set", {"values": {"Deeper": [deep]}})
        self.assertEqual(client.call("savedata.get")["values"], {"Deep": deep})

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
        load_error = client.call("script.eval", {"context": "play", "code": "return SaveData.GetLoadError()"})
        self.assertIsNone(load_error.get("value"))
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
