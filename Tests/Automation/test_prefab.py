"""Prefabs through automation (Docs/Architecture.md §5.5, §13.5 prefab.*, Roadmap M6)."""

from __future__ import annotations

import json
import unittest
from typing import Any

from harness import AutomationTestCase, engine_client, read_json

BRICK_SCRIPT = """--!strict
local Brick = {}

Brick.Fields = {
\tPoints = Field.Integer(1, { Min = 1 }),
}

return Script.Define("Brick", Brick)
"""
BRICK_PREFAB = "Assets/Prefabs/Brick.prefab"
SKIP_REMOVE_ENTITY = ("contract stub: RemoveEntity records a deleted prefab member "
                      "(ADR 0031 decision 7, streams A1 and B)")
SKIP_APPLY_LEVEL = ("contract stub: live nested prefabs and the prefab.apply level "
                    "(ADR 0031 decisions 8 and 9, streams A2 and B)")


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

    def test_destroying_a_prefab_member_is_refused_with_a_hint(self) -> None:
        # E-PFB-REFUSE (ADR 0028 decision 16): a deleted member came back with its instance's next prefab update, so the
        # edit scene refuses it, alone or as a batch op, and names the ways out: deactivate it or unpack the instance.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp"})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("entity.create", {"name": "Other"})
        before = client.call("scene.tree", {"format": "json"})["entities"]
        undo_entries = len(client.call("edit.history")["entries"])

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("entity.destroy", {"entities": ["/Other", "/Lamp/Bulb"]})
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        pointers = [issue["pointer"] for issue in refused.exception.issues]
        self.assertEqual(pointers, ["/entities/1"], refused.exception.data)
        hint = refused.exception.data.get("hint", "")
        self.assertIn("prefab.unpack", hint)
        self.assertIn("entity.update", hint)
        with self.assertRaises(engine_client.EngineError) as batched:
            client.call("edit.batch", {"ops": [{"method": "entity.destroy", "params": {"entities": ["/Other"]}},
                                               {"method": "entity.destroy", "params": {"entities": ["/Lamp/Bulb"]}}]})
        self.assert_engine_error(batched.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertEqual(client.call("scene.tree", {"format": "json"})["entities"], before)
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries)

        # The ways out, called exactly as the hint writes them: a deactivated member stays so across an update of its
        # instance (here by applying another instance of the prefab), and an unpacked instance's entities are ordinary
        # ones.
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Lamp.prefab", "name": "Lamp2"})
        client.call("entity.update", hinted_params(hint, "entity.update"))
        self.assertEqual(client.call("prefab.apply", {"instance": "/Lamp2"})["updatedInstances"], 2)
        self.assertFalse(client.call("entity.get", {"entity": "/Lamp/Bulb"})["entity"]["active"])
        self.assertTrue(client.call("entity.get", {"entity": "/Lamp2/Bulb"})["entity"]["active"])
        client.call("prefab.unpack", hinted_params(hint, "prefab.unpack"))
        destroyed = client.call("entity.destroy", {"entities": ["/Lamp/Bulb"]})["destroyed"]
        self.assertEqual(len(destroyed), 1)

    def test_replacing_a_prefab_member_with_an_instance_is_refused(self) -> None:
        # The other removal of a member (ADR 0028 decision 16): its instance lost it to the new instance, whose root
        # kept its id, and every later update of the instance failed. Saving the member as a prefab without replacing it
        # works.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp"})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp"})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("prefab.create", {"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab",
                                          "replaceWithInstance": True})
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        pointers = [issue["pointer"] for issue in refused.exception.issues]
        self.assertEqual(pointers, ["/entity"], refused.exception.data)
        self.assertEqual(hinted_params(refused.exception.data.get("hint", ""), "prefab.unpack"), {"instance": "/Lamp"})
        self.assertFalse((root / "Assets" / "Prefabs" / "Bulb.prefab").exists())
        components = client.call("entity.get", {"entity": "/Lamp/Bulb"})["entity"]["components"]
        self.assertIn("PrefabLink", components)
        self.assertNotIn("Prefab", components)

        client.call("prefab.create", {"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab"})
        self.assertTrue((root / "Assets" / "Prefabs" / "Bulb.prefab").is_file())

    @unittest.skip(SKIP_REMOVE_ENTITY)
    def test_destroying_a_prefab_member_records_its_deletion(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decision 7 (E-PFB-DEL): RemoveEntity replaces the setup's refusal (the
        # two refusal tests above, which stream A1 rewrites into this one). Deleting a member records one RemoveEntity
        # on its instance root in one undo step, alone or as a batch op; the member stays deleted when the prefab
        # updates; and a member replaced by an instance of a new prefab becomes a user child of the outer instance.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp"})
        client.call("entity.create", {"name": "Shade", "parent": "/Lamp"})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Lamp.prefab", "name": "Lamp2"})
        undo_entries = len(client.call("edit.history")["entries"])

        destroyed = client.call("entity.destroy", {"entities": ["/Lamp/Bulb"]})["destroyed"]
        self.assertEqual(len(destroyed), 1)
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries + 1)
        overrides = client.call("entity.get", {"entity": "/Lamp"})["entity"]["components"]["Prefab"]["Overrides"]
        self.assertEqual([entry["Kind"] for entry in overrides], ["RemoveEntity"])
        self.assertEqual(len(overrides[0]["Path"]), 1)

        # An update of the prefab (applying the other instance's edit) keeps the member deleted.
        client.call("entity.update", {"entity": "/Lamp2", "components": {"MeshRenderer": {"CastShadows": False}}})
        self.assertEqual(client.call("prefab.apply", {"instance": "/Lamp2"})["updatedInstances"], 2)
        names = [entity["name"] for entity in client.call("scene.tree", {"format": "json"})["entities"]]
        self.assertEqual(names, ["Lamp", "Shade", "Lamp2", "Bulb", "Shade"])

        # As a batch op, and replacing a member with an instance of a new prefab, also record deletions.
        client.call("edit.batch", {"ops": [{"method": "entity.destroy", "params": {"entities": ["/Lamp2/Bulb"]}}]})
        replaced = client.call("prefab.create", {"entity": "/Lamp/Shade", "path": "Assets/Prefabs/Shade.prefab",
                                                 "replaceWithInstance": True})
        self.assertEqual(replaced["instance"]["path"], "/Lamp/Shade")
        self.assertTrue((root / "Assets" / "Prefabs" / "Shade.prefab").is_file())
        lamp = client.call("entity.get", {"entity": "/Lamp", "components": "all"})["entity"]["components"]
        self.assertEqual([entry["Kind"] for entry in lamp["Prefab"]["Overrides"]], ["RemoveEntity", "RemoveEntity"])
        shade = client.call("entity.get", {"entity": "/Lamp/Shade", "components": "all"})["entity"]["components"]
        self.assertIn("Prefab", shade)
        self.assertEqual(shade["PrefabLink"]["InstanceRoot"], replaced["instance"]["id"])

    @unittest.skip(SKIP_APPLY_LEVEL)
    def test_apply_writes_to_the_level_the_caller_picks(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decisions 8 and 9 (E-PFB-NEST): a Room prefab created around a Lamp
        # instance keeps it live. prefab.apply writes an edit of a lamp inside a room to the level the caller picks: at
        # the Lamp level Lamp.prefab changes and Room.prefab is rewritten; without a level the outermost prefab, Room,
        # receives it.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Room"})
        client.call("entity.create", {"name": "Lamp", "parent": "/Room"})
        client.call("entity.create", {"name": "Bulb", "parent": "/Room/Lamp",
                                      "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("prefab.create", {"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.create", {"entity": "/Room", "path": "Assets/Prefabs/Room.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Room.prefab", "name": "Room2"})
        nested = client.call("entity.get", {"entity": "/Room/Lamp", "components": "all"})["entity"]["components"]
        self.assertIn("Prefab", nested)
        self.assertIsNotNone(nested["PrefabLink"]["OuterPrefabEntityID"])

        client.call("entity.update", {"entity": "/Room/Lamp/Bulb",
                                      "components": {"MeshRenderer": {"CastShadows": False}}})
        applied = client.call("prefab.apply", {"instance": "/Room/Lamp", "level": "/Room/Lamp"})
        self.assertEqual(applied["prefab"]["path"], "Assets/Prefabs/Lamp.prefab")
        self.assertEqual([prefab["path"] for prefab in applied["updatedPrefabs"]], ["Assets/Prefabs/Room.prefab"])
        bulb = client.call("entity.get", {"entity": "/Room2/Lamp/Bulb", "components": ["MeshRenderer"]})
        self.assertFalse(bulb["entity"]["components"]["MeshRenderer"]["CastShadows"])
        lamp_prefab = read_json(root / "Assets" / "Prefabs" / "Lamp.prefab")
        bulbs = [entity for entity in lamp_prefab["Entities"] if entity["Name"] == "Bulb"]
        self.assertFalse(bulbs[0]["Components"]["MeshRenderer"]["CastShadows"])

        client.call("entity.update", {"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": False}}})
        outermost = client.call("prefab.apply", {"instance": "/Room"})
        self.assertEqual(outermost["prefab"]["path"], "Assets/Prefabs/Room.prefab")
        self.assertEqual(outermost["updatedPrefabs"], [])
        lamp_prefab = read_json(root / "Assets" / "Prefabs" / "Lamp.prefab")
        bulbs = [entity for entity in lamp_prefab["Entities"] if entity["Name"] == "Bulb"]
        self.assertTrue(bulbs[0]["Components"]["MeshRenderer"]["Visible"])

        with self.assertRaises(engine_client.EngineError) as wrong_level:
            client.call("prefab.apply", {"instance": "/Room", "level": "/Room2/Lamp"})
        self.assert_engine_error(wrong_level.exception, engine_client.INVALID_PARAMS)

    def test_prefab_errors_and_dry_run(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Plain"})
        with self.assertRaises(engine_client.EngineError) as not_instance:
            client.call("prefab.apply", {"instance": "/Plain"})
        self.assert_engine_error(not_instance.exception, engine_client.INVALID_PARAMS)
        result = client.call("prefab.create", {"entity": "/Plain", "path": "Assets/Plain.prefab", "dryRun": True})
        self.assertTrue(result["dryRun"])
        self.assertFalse((root / "Assets" / "Plain.prefab").exists())


def hinted_params(hint: str, method: str) -> dict[str, Any]:
    """The params of the call `hint` writes as "<method> {...}" (a refusal's way out, written to be copied)."""
    start = hint.index(f"{method} ") + len(method) + 1
    params, _ = json.JSONDecoder().raw_decode(hint, start)
    if not isinstance(params, dict):
        raise AssertionError(f"the hint's {method} call has no params object: {hint}")
    return params


if __name__ == "__main__":
    unittest.main()
