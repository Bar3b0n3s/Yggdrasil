"""Prefabs through automation (Docs/Architecture.md §5.5, §13.5 prefab.*, Roadmap M6)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client, read_json

BRICK_SCRIPT = """--!strict
local Brick = {}

Brick.Fields = {
\tPoints = Field.Integer(1, { Min = 1 }),
}

return Script.Define("Brick", Brick)
"""
BRICK_PREFAB = "Assets/Prefabs/Brick.prefab"


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

    def test_instantiate_components_are_instance_overrides_that_a_strict_editor_opens(self) -> None:
        # The M16 dry run's brick grid (S1-17, S1-04): one prefab, each row with its own Points, placed by one batch of
        # prefab.instantiate {components} without an entity.update per brick. The values are the instances' overrides,
        # project.validate reports nothing, and a --strict editor opens the saved project.
        client, root = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/Brick.luau", "source": BRICK_SCRIPT})
        client.call("entity.create", {"name": "Brick", "components": {
            "Script": {"Script": "Assets/Scripts/Brick.luau", "Fields": {"Points": 1}},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("prefab.create", {"entity": "/Brick", "path": BRICK_PREFAB})
        client.call("entity.destroy", {"entities": ["/Brick"]})
        ops = [{"method": "prefab.instantiate", "params": {
            "prefab": BRICK_PREFAB, "name": f"Row{points}", "transform": {"Translation": [0, points, 0]},
            "components": {"Script": {"Fields": {"Points": points}}, "MeshRenderer": {"CastShadows": points == 2}}}}
            for points in (2, 3)]
        client.call("edit.batch", {"label": "Rows", "ops": ops})
        row = client.call("entity.get", {"entity": "/Row3"})["entity"]["components"]
        self.assertEqual(row["Script"]["Fields"], {"Points": 3})
        self.assertFalse(row["MeshRenderer"]["CastShadows"])
        overrides = row["Prefab"]["Overrides"]
        self.assertIn({"Points": 3}, [entry["Value"] for entry in overrides if entry["Field"] == "Fields"])
        self.assertIn("CastShadows", [entry["Field"] for entry in overrides])

        # A $ref to a member the result does not have names the members it has (S1-07): prefab.instantiate's root is
        # "entity", prefab.create's "instance".
        with self.assertRaises(engine_client.EngineError) as wrong_member:
            client.call("edit.batch", {"ops": [
                {"method": "prefab.instantiate", "params": {"prefab": BRICK_PREFAB, "name": "Row4"}},
                {"method": "entity.update", "params": {"entity": {"$ref": "0.instance.id"}, "name": "Renamed"}}]})
        self.assert_engine_error(wrong_member.exception, engine_client.INVALID_PARAMS)
        hints = [wrong_member.exception.data.get("hint", "")] + [issue.get("hint", "")
                                                               for issue in wrong_member.exception.issues]
        self.assertTrue(any("'entity', 'prefab', 'undoIndex'" in hint for hint in hints), wrong_member.exception.data)

        client.call("scene.save")
        document = read_json(root / "Assets" / "Scenes" / "Main.scene")
        self.assertEqual([entity["Name"] for entity in document["Entities"]], ["Row2", "Row3"])
        report = client.call("project.validate")
        self.assertEqual([d for d in report["diagnostics"] if d["code"].startswith(("SCRIPT_", "PREFAB_"))], [])
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)

        code, output = self.run_editor(["--headless", "--renderer", "none", "--strict", "--read-only",
                                        "--check-scripts", "--project", str(root)])
        self.assertEqual(code, 0, output)
        self.assertNotIn("REFLECTION_VARIANT_UNRESOLVED", output)

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
