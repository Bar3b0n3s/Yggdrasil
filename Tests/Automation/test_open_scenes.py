"""Multiple open scenes through automation (Docs/Decisions/0031-m20-contract.md decisions 13 to 16, E-SCENES)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

MAIN = "Assets/Scenes/Main.scene"
SECOND = "Assets/Scenes/Second.scene"


def entity_names(tree: dict) -> list[str]:
    """The names of a scene.tree {format: "json"} result's entities, in canonical order."""
    return [entity["name"] for entity in tree["entities"]]


class OpenSceneTests(AutomationTestCase):
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


if __name__ == "__main__":
    unittest.main()
