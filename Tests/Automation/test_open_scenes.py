"""Multiple open scenes through automation (Docs/Decisions/0031-m20-contract.md decisions 13 to 16 and 23, E-SCENES)."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import GpuRecoveryTestCase, engine_client

MAIN = "Assets/Scenes/Main.scene"
SECOND = "Assets/Scenes/Second.scene"


def entity_names(tree: dict) -> list[str]:
    """The names of a scene.tree {format: "json"} result's entities, in canonical order."""
    return [entity["name"] for entity in tree["entities"]]


def autosaved_scenes(root: Path) -> dict[str, str]:
    """The scene payload of the newest autosave generation of each scene of the project at `root`, by its scene path."""
    autosave = root / "Library" / "Autosave"
    manifest = json.loads((autosave / "Manifest.json").read_text(encoding="utf-8"))
    scenes: dict[str, str] = {}
    # The manifest lists the newest generation first.
    for entry in manifest["Generations"]:
        generation = autosave / entry["Generation"]
        path = json.loads((generation / "Metadata.json").read_text(encoding="utf-8"))["ScenePath"]
        scenes.setdefault(path, (generation / "Scene.json").read_text(encoding="utf-8"))
    return scenes


class OpenSceneTests(GpuRecoveryTestCase):
    """GpuRecoveryTestCase for its device-loss helpers (M18's loss tests), which only the device-loss test uses."""

    def test_list_activate_and_close_the_open_scenes(self) -> None:
        # Decision 14: scene.new and scene.open with keepOpen add an open scene and make it active; scene.list reports
        # them in tab order; scene.activate switches by path or id; scene.close needs save or discardChanges for a dirty
        # scene and activates its neighbour.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "InMain"})
        second = client.call("scene.new", {"path": SECOND, "keepOpen": True})
        self.assertEqual(second["scene"]["path"], SECOND)
        scenes = client.call("scene.list")["scenes"]
        self.assertEqual([entry["scene"]["path"] for entry in scenes], [MAIN, SECOND])
        self.assertEqual([entry["active"] for entry in scenes], [False, True])
        self.assertTrue(scenes[0]["scene"]["dirty"])
        self.assertEqual(scenes[1]["id"], second["id"])

        client.call("scene.activate", {"scene": MAIN})
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), ["InMain"])
        client.call("scene.activate", {"scene": second["id"]})
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), [])
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("scene.activate", {"scene": "Assets/Scenes/Missing.scene"})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")

        with self.assertRaises(engine_client.EngineError) as dirty:
            client.call("scene.close", {"scene": MAIN})
        self.assert_engine_error(dirty.exception, engine_client.INVALID_STATE, "InvalidState")
        closed = client.call("scene.close", {"scene": MAIN, "save": True})
        self.assertEqual(closed["file"], MAIN)
        self.assertIn("InMain", (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))
        self.assertEqual(closed["activeId"], second["id"])
        self.assertEqual([entry["scene"]["path"] for entry in client.call("scene.list")["scenes"]], [SECOND])

        # scene.open with keepOpen opens the saved Main in a new tab at the end, the active one; Second keeps its
        # unsaved changes.
        client.call("entity.create", {"name": "InSecond"})
        reopened = client.call("scene.open", {"path": MAIN, "keepOpen": True})
        self.assertEqual(reopened["scene"]["path"], MAIN)
        scenes = client.call("scene.list")["scenes"]
        self.assertEqual([entry["scene"]["path"] for entry in scenes], [SECOND, MAIN])
        self.assertEqual([entry["active"] for entry in scenes], [False, True])
        self.assertEqual(scenes[1]["id"], reopened["id"])
        self.assertTrue(scenes[0]["scene"]["dirty"])
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), ["InMain"])

        # reload reads the open Second again in its own tab, dropping its changes, and makes it active.
        reloaded = client.call("scene.open", {"path": SECOND, "reload": True, "discardChanges": True})
        self.assertEqual(reloaded["scene"]["path"], SECOND)
        scenes = client.call("scene.list")["scenes"]
        self.assertEqual([entry["scene"]["path"] for entry in scenes], [SECOND, MAIN])
        self.assertEqual([entry["active"] for entry in scenes], [True, False])
        self.assertEqual(scenes[0]["id"], reloaded["id"])
        self.assertFalse(scenes[0]["scene"]["dirty"])
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), [])

    def test_requests_without_a_scene_target_address_the_active_scene(self) -> None:
        # Decision 16 (Roadmap M20 acceptance "transcripts without scene targets behave as before"): with another scene
        # open, an M17-style transcript without targets edits, undoes and saves the active scene; a request with a
        # target addresses its scene and reports that scene's "_meta".
        client, root = self.open_editor_with_scene()
        main_text = (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8")
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("entity.create", {"name": "Board"})
        client.call("entity.create", {"name": "Piece", "parent": "/Board"})
        client.call("edit.undo")
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), ["Board"])
        self.assertEqual(client.call("scene.save")["file"], SECOND)
        self.assertEqual((root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"), main_text)

        created = client.call("entity.create", {"name": "Elsewhere", "scene": MAIN})
        self.assertTrue(created["_meta"]["dirty"])
        self.assertFalse(client.call("session.info")["_meta"]["dirty"])
        self.assertEqual(len(client.call("edit.history", {"scene": MAIN})["entries"]), 1)
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json", "scene": MAIN})), ["Elsewhere"])
        with self.assertRaises(engine_client.EngineError) as untargeted:
            client.call("asset.list", {"scene": MAIN})
        self.assert_engine_error(untargeted.exception, engine_client.INVALID_PARAMS, "InvalidArgument")

    def test_a_batch_resolves_its_references_in_its_target_scene(self) -> None:
        # Decision 16 and ADR 0028's ledger (Roadmap M20 acceptance "a batch $ref and an entity path resolve in the
        # targeted scene"): every op of a batch with a target runs in that scene; an op cannot carry its own target.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Board"})
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("entity.create", {"name": "Board"})
        client.call("edit.batch", {"scene": MAIN, "label": "Pieces", "ops": [
            {"method": "entity.create", "params": {"name": "Piece", "parent": "/Board"}},
            {"method": "entity.update", "params": {"entity": {"$ref": "0.entity.id"}, "name": "Renamed"}},
        ]})
        main_tree = client.call("scene.tree", {"format": "json", "scene": MAIN})
        self.assertEqual(entity_names(main_tree), ["Board", "Renamed"])
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), ["Board"])

        # The merge with M19 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"; Docs/Decisions/0030-m19-contract.md
        # decisions 7 and 8): a named $ref and an entity path inside component data (a Field.Entity() value) resolve in
        # the target scene too, never in the active scene, which holds the same path with another id.
        script = "Assets/Scripts/Hud.luau"
        client.call("script.write", {"path": script, "source": """
local Hud = { Fields = { Score = Field.Entity() } }
return Script.Define("Hud", Hud)
"""})
        client.call("entity.create", {"name": "Game"})
        active_score = client.call("entity.create", {"name": "ScoreText", "parent": "/Game"})["entity"]["id"]
        hud = client.call("edit.batch", {"scene": MAIN, "label": "Hud", "ops": [
            {"method": "entity.create", "params": {"name": "Game"}, "name": "game"},
            {"method": "entity.create", "params": {"name": "ScoreText", "parent": {"$ref": "game.entity.id"}}},
            {"method": "entity.create", "params": {"name": "Hud", "components": {
                "Script": {"Scripts": [{"Script": script, "Fields": {"Score": "/Game/ScoreText"}}]}}}},
        ]})
        main_score = hud["results"][1]["entity"]["id"]
        self.assertEqual(hud["results"][1]["entity"]["path"], "/Game/ScoreText")
        self.assertNotEqual(main_score, active_score)
        hud_entity = client.call("entity.get", {"scene": MAIN, "entity": "/Hud"})["entity"]
        self.assertEqual(hud_entity["components"]["Script"]["Scripts"][0]["Fields"]["Score"], main_score)
        active_tree = client.call("scene.tree", {"format": "json"})
        self.assertEqual(sorted(entity_names(active_tree)), ["Board", "Game", "ScoreText"])

        with self.assertRaises(engine_client.EngineError) as op_target:
            client.call("edit.batch", {"ops": [
                {"method": "entity.create", "params": {"name": "Stray", "scene": MAIN}},
            ]})
        self.assert_engine_error(op_target.exception, engine_client.INVALID_PARAMS, "InvalidArgument")

    def test_a_prefab_update_rebuilds_every_open_scene(self) -> None:
        # Decision 15 (Roadmap M20 acceptance "a prefab update rebuilds every open scene"): an applied edit reaches the
        # instances of another open scene through an entry of that scene's own history, so undo there reverts only it.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp"})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp"})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Lamp.prefab", "name": "Copy"})
        entries = len(client.call("edit.history")["entries"])

        client.call("scene.activate", {"scene": MAIN})
        client.call("entity.update", {"entity": "/Lamp/Bulb", "name": "Globe"})
        client.call("prefab.apply", {"instance": "/Lamp"})
        second_tree = client.call("scene.tree", {"format": "json", "scene": SECOND})
        self.assertEqual(entity_names(second_tree), ["Copy", "Globe"])
        self.assertEqual(len(client.call("edit.history", {"scene": SECOND})["entries"]), entries + 1)

        client.call("edit.undo", {"scene": SECOND})
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json", "scene": SECOND})), ["Copy", "Bulb"])
        self.assertEqual(entity_names(client.call("scene.tree", {"format": "json"})), ["Lamp", "Globe"])

    def test_autosave_recovers_every_open_scene(self) -> None:
        # Decision 15 (Roadmap M20 acceptance "autosave is isolated per open scene"): the autosave before play writes
        # one generation per dirty open scene, and after a crash project.open {recover} opens each of them again,
        # dirty, without touching the source files.
        client, root = self.open_editor_with_scene()
        main_text = (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8")
        client.call("entity.create", {"name": "InMain"})
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("entity.create", {"name": "InSecond"})
        client.call("play.start", {"lockstep": True})
        client.close()
        self.editors[-1].kill()

        reopened = self.connect(self.start_editor())
        opened = reopened.call("project.open", {"path": str(root), "recover": True})
        self.assertTrue(opened["recovered"])
        scenes = reopened.call("scene.list")["scenes"]
        self.assertEqual(sorted(entry["scene"]["path"] for entry in scenes), [MAIN, SECOND])
        self.assertTrue(all(entry["scene"]["dirty"] for entry in scenes))
        self.assertEqual(entity_names(reopened.call("scene.tree", {"format": "json", "scene": SECOND})), ["InSecond"])
        self.assertEqual(entity_names(reopened.call("scene.tree", {"format": "json", "scene": MAIN})), ["InMain"])
        self.assertEqual((root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"), main_text)

    def test_device_loss_with_two_open_scenes_autosaves_and_recovers_both(self) -> None:
        # Decision 23 (Roadmap M20 acceptance "an injected device loss with two open scenes autosaves both and recovers
        # both views"): M18's recovery writes the autosave its fatal hook would write before it recovers, which holds
        # one generation per dirty open scene, and after it each open scene's view renders what it rendered before the
        # loss.
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene("DeviceLossOpenScenes", renderer="vulkan")
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
        client.call("entity.create",
                    {"name": "InMain", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("scene.new", {"path": SECOND, "keepOpen": True})
        client.call("entity.create",
                    {"name": "InSecond", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        before = {scene: self.shoot(client, {"scene": scene}) for scene in (MAIN, SECOND)}
        self.assertNotEqual(before[MAIN], before[SECOND])
        self.assertFalse((root / "Library" / "Autosave" / "Manifest.json").exists())
        cursor = client.call("events.read", {"cursor": "end"})["nextCursor"]

        # The loss is injected with Second active: the autosave before the recovery writes both dirty scenes.
        self.assertTrue(client.call("debug.deviceLost")["queued"])
        events = self.wait_for_device_loss_events(client, cursor, 1)
        self.assertEqual(len(events), 1, events)
        self.assertEqual(client.call("stats.get")["deviceLosses"], 1)
        autosaved = autosaved_scenes(root)
        self.assertEqual(sorted(autosaved), [MAIN, SECOND])
        self.assertIn('"InMain"', autosaved[MAIN])
        self.assertNotIn('"InSecond"', autosaved[MAIN])
        self.assertIn('"InSecond"', autosaved[SECOND])

        # Both scenes stay open with their unsaved changes, and each one's view renders again in its tab (the editor's
        # scene view, after a frame newer than the one held when it became active), and the same image as before.
        scenes = client.call("scene.list")["scenes"]
        self.assertEqual([entry["scene"]["path"] for entry in scenes], [MAIN, SECOND])
        self.assertTrue(all(entry["scene"]["dirty"] for entry in scenes))
        for scene in (MAIN, SECOND):
            client.call("scene.activate", {"scene": scene})
            held = self.wait_for_rendered_frame(client, -1)
            self.wait_for_rendered_frame(client, held)
            self.assertEqual(self.shoot(client, {"scene": scene}), before[scene], scene)
        self.assertEqual(len(self.device_loss_events(client, cursor)), 1)
        # The two-scene recovery printed no GPU warning or error (the milestone review, T3): the crash below ends the
        # editor without its clean-exit check.
        self.assert_no_gpu_messages(self.editors[-1])

        # The autosave the loss wrote recovers both scenes after a crash.
        client.close()
        self.editors[-1].kill()
        reopened = self.connect(self.start_editor())
        self.assertTrue(reopened.call("project.open", {"path": str(root), "recover": True})["recovered"])
        recovered = reopened.call("scene.list")["scenes"]
        self.assertEqual(sorted(entry["scene"]["path"] for entry in recovered), [MAIN, SECOND])
        self.assertEqual(entity_names(reopened.call("scene.tree", {"format": "json", "scene": MAIN})), ["InMain"])
        self.assertEqual(entity_names(reopened.call("scene.tree", {"format": "json", "scene": SECOND})), ["InSecond"])


if __name__ == "__main__":
    unittest.main()
