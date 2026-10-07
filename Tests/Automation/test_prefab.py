"""Prefabs through automation (Docs/Architecture.md §5.5, §13.5 prefab.*, Roadmap M6)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client, read_json


class PrefabTests(AutomationTestCase):
    def test_prefab_create_instantiate_apply_revert_unpack(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Cell", "components": {
            "Transform": {"Scale": [0.95, 0.95, 0.95]}, "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        created = client.call("prefab.create", {"entity": "/Cell", "path": "Assets/Prefabs/Cell.prefab",
                                                "replaceWithInstance": True})
        self.assertEqual(created["prefab"]["type"], "Prefab")
        self.assertEqual(read_json(root / "Assets" / "Prefabs" / "Cell.prefab")["Format"], "Prefab")
        self.assertTrue((root / "Assets" / "Prefabs" / "Cell.prefab.meta").is_file())

        second = client.call("prefab.instantiate", {"prefab": created["prefab"]["id"], "name": "Cell2",
                                                    "transform": {"Translation": [2, 0, 0]}})
        self.assertEqual(second["entity"]["path"], "/Cell2")

        # An override applied to the prefab reaches every instance.
        client.call("entity.update", {"entity": "/Cell2", "components": {"MeshRenderer": {"CastShadows": False}}})
        applied = client.call("prefab.apply", {"instance": "/Cell2"})
        self.assertEqual(applied["updatedInstances"], 2)
        first = client.call("entity.get", {"entity": "/Cell", "components": ["MeshRenderer"]})
        self.assertFalse(first["entity"]["components"]["MeshRenderer"]["CastShadows"])

        # Revert clears the instance's own overrides.
        client.call("entity.update", {"entity": "/Cell", "components": {"MeshRenderer": {"Visible": False}}})
        reverted = client.call("prefab.revert", {"instance": "/Cell"})
        self.assertGreaterEqual(reverted["removedOverrides"], 1)
        first = client.call("entity.get", {"entity": "/Cell", "components": ["MeshRenderer"]})
        self.assertTrue(first["entity"]["components"]["MeshRenderer"]["Visible"])

        # Unpack keeps the entities and drops the links.
        client.call("prefab.unpack", {"instance": "/Cell2"})
        unpacked = client.call("entity.get", {"entity": "/Cell2", "components": "all"})
        self.assertNotIn("PrefabLink", unpacked["entity"]["components"])

        # Every step was one undo entry.
        history = client.call("edit.history")["entries"]
        labels = [entry["label"] for entry in history]
        self.assertTrue(all(label.startswith("[agent] ") for label in labels))
        client.call("edit.undo", {"steps": len(history)})
        self.assertEqual([entity["name"] for entity in client.call("scene.tree", {"format": "json"})["entities"]], [])

    def test_prefab_errors_and_dry_run(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Plain"})
        with self.assertRaises(engine_client.EngineError) as not_instance:
            client.call("prefab.apply", {"instance": "/Plain"})
        self.assert_engine_error(not_instance.exception, engine_client.INVALID_PARAMS)
        result = client.call("prefab.create", {"entity": "/Plain", "path": "Assets/Plain.prefab", "dryRun": True})
        self.assertTrue(result["dryRun"])
        self.assertFalse((root / "Assets" / "Plain.prefab").exists())


if __name__ == "__main__":
    unittest.main()
