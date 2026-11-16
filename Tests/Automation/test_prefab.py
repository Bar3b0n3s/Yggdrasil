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
GAME_FLOW_SCRIPT = """--!strict
local GameFlow = {}

GameFlow.Fields = {
	Menus = Field.Entity(),
	Warden = Field.Entity(),
}

return Script.Define("GameFlow", GameFlow)
"""
HUD_SCRIPT = """--!strict
local Hud = {}

Hud.Fields = {
\tBar = Field.Entity(),
}

return Script.Define("Hud", Hud)
"""


def tree_names(tree: dict[str, Any]) -> list[str]:
    """The names of the entities of a scene.tree {format: "json"} result, depth first."""
    return [entity["name"] for entity in tree["entities"]]


def hinted_params(hint: str, method: str) -> dict[str, object]:
    """The params of the call to `method` that `hint` gives to copy, written as "<method> {...}"."""
    start = hint.find(f"{method} {{")
    if start < 0:
        raise AssertionError(f"the hint gives no {method} call: {hint!r}")
    params, _ = json.JSONDecoder().raw_decode(hint, start + len(method) + 1)
    if not isinstance(params, dict):
        raise AssertionError(f"the {method} call in the hint has no params object: {hint!r}")
    return params


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
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Brick.luau", "Fields": {"Points": 1}}]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("prefab.create", {"entity": "/Brick", "path": BRICK_PREFAB})
        client.call("entity.destroy", {"entities": ["/Brick"]})
        ops = [{"method": "prefab.instantiate", "params": {
            "prefab": BRICK_PREFAB, "name": f"Row{points}", "transform": {"Translation": [0, points, 0]},
            "components": {"Script": {"Scripts": [{"Fields": {"Points": points}}]},
                           "MeshRenderer": {"CastShadows": points == 2}}}}
            for points in (2, 3)]
        client.call("edit.batch", {"label": "Rows", "ops": ops})
        row = client.call("entity.get", {"entity": "/Row3"})["entity"]["components"]
        self.assertEqual(row["Script"]["Scripts"][0]["Fields"], {"Points": 3})
        self.assertFalse(row["MeshRenderer"]["CastShadows"])
        overrides = row["Prefab"]["Overrides"]
        self.assertIn({"Points": 3}, [entry["Value"] for entry in overrides if entry["Field"] == "Scripts/0/Fields"])
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

    def test_rebuild_keeps_overrides_valid_together_and_reports_what_it_drops(self) -> None:
        # M35 (Docs/Decisions/0050-m35-pause-entity-state.md decisions 9 to 11; World Tree's brief 13): a rebuild writes
        # an instance's overrides of one component together, and the methods that rebuild report each override they
        # drop; a script-field patch at instantiation keeps the slot's other references on the instance's own members.
        client, _ = self.open_editor_with_scene()
        options = "Assets/Prefabs/Options.prefab"

        def anchors(min_anchor: list[float], max_anchor: list[float] | None = None) -> dict[str, Any]:
            rect: dict[str, Any] = {"AnchorMin": min_anchor}
            if max_anchor is not None:
                rect["AnchorMax"] = max_anchor
            return {"UiRect": rect}

        client.call("entity.create", {"name": "Options"})
        centred = anchors([0.5, 0.5], [0.5, 0.5])
        client.call("entity.create", {"name": "Panel", "parent": "/Options", "components": centred})
        client.call("prefab.create", {"entity": "/Options", "path": options, "replaceWithInstance": True})
        for name in ("Second", "Third"):
            client.call("prefab.instantiate", {"prefab": options, "name": name})
        client.call("entity.update", {"entity": "/Options/Panel", "components": anchors([0, 0], [0, 0])})
        client.call("entity.update", {"entity": "/Second/Panel", "components": anchors([0.375, 0.375])})
        client.call("scene.save", {"path": "Assets/Scenes/Title.scene"})
        client.call("entity.update", {"entity": "/Third/Panel", "components": anchors([0.25, 0.25], [0.25, 0.25])})
        applied = client.call("prefab.apply", {"instance": "/Third"})
        second = client.call("entity.get", {"entity": "/Second"})["entity"]["id"]
        dropped = [(row["code"], row["entity"]) for row in applied["rebuildDiagnostics"]]
        self.assertEqual(dropped, [("PREFAB_STALE_OVERRIDE", second)])
        reopen = {"path": "Assets/Scenes/Title.scene", "reload": True, "discardChanges": True}
        opened = client.call("scene.open", reopen)
        stale = [row["entity"] for row in opened["diagnostics"] if row["code"] == "PREFAB_STALE_OVERRIDE"]
        self.assertEqual(stale, [second])
        panel = client.call("entity.get", {"entity": "/Options/Panel", "components": ["UiRect"]})
        rect = panel["entity"]["components"]["UiRect"]
        self.assertEqual((rect["AnchorMin"], rect["AnchorMax"]), ([0, 0], [0, 0]))

        # The patch of one script field names the instance's own Menus, and nothing is logged.
        client.call("script.write", {"path": "Assets/Scripts/GameFlow.luau", "source": GAME_FLOW_SCRIPT})
        client.call("entity.create", {"name": "Game"})
        menus = client.call("entity.create", {"name": "Menus", "parent": "/Game"})["entity"]["id"]
        slot = {"Script": "Assets/Scripts/GameFlow.luau", "Fields": {"Menus": menus}}
        client.call("entity.update", {"entity": "/Game", "components": {"Script": {"Scripts": [slot]}}})
        client.call("prefab.create", {"entity": "/Game", "path": "Assets/Prefabs/GameUi.prefab"})
        warden = client.call("entity.create", {"name": "Warden"})["entity"]["id"]
        patch = {"Script": {"Scripts": [{"Fields": {"Warden": "/Warden"}}]}}
        game_ui = "Assets/Prefabs/GameUi.prefab"
        hub = client.call("prefab.instantiate", {"prefab": game_ui, "name": "Hub", "components": patch})
        diagnostics = hub["_meta"]["diagnostics"]
        self.assertEqual((diagnostics["newWarnings"], diagnostics["newErrors"]), (0, 0))
        fields = client.call("entity.get", {"entity": "/Hub"})["entity"]["components"]["Script"]["Scripts"][0]["Fields"]
        self.assertEqual(fields["Menus"], client.call("entity.get", {"entity": "/Hub/Menus"})["entity"]["id"])
        self.assertEqual(fields["Warden"], warden)

    def test_destroying_a_prefab_member_records_its_deletion(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decision 7 (E-PFB-DEL): RemoveEntity replaces the setup's refusal
        # (ADR 0028 decision 16), whose two tests this one rewrites. Deleting a member records one RemoveEntity on its
        # instance root in one undo step, alone or as a batch op; the member stays deleted when the prefab updates; and
        # a member replaced by an instance of a new prefab becomes a user child of the outer instance, while saving one
        # without replacing it leaves it a member.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp"})
        client.call("entity.create", {"name": "Shade", "parent": "/Lamp"})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Lamp.prefab", "name": "Lamp2"})
        bulb_link = client.call("entity.get", {"entity": "/Lamp/Bulb", "components": ["PrefabLink"]})
        bulb_prefab_id = bulb_link["entity"]["components"]["PrefabLink"]["PrefabEntityID"]
        undo_entries = len(client.call("edit.history")["entries"])

        destroyed = client.call("entity.destroy", {"entities": ["/Lamp/Bulb"]})["destroyed"]
        self.assertEqual(len(destroyed), 1)
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries + 1)
        overrides = client.call("entity.get", {"entity": "/Lamp"})["entity"]["components"]["Prefab"]["Overrides"]
        self.assertEqual([entry["Kind"] for entry in overrides], ["RemoveEntity"])
        self.assertEqual(overrides[0]["Path"], [bulb_prefab_id])

        # An update of the prefab (applying the other instance's edit) keeps the member deleted.
        client.call("entity.update", {"entity": "/Lamp2", "components": {"MeshRenderer": {"CastShadows": False}}})
        self.assertEqual(client.call("prefab.apply", {"instance": "/Lamp2"})["updatedInstances"], 2)
        names = [entity["name"] for entity in client.call("scene.tree", {"format": "json"})["entities"]]
        self.assertEqual(names, ["Lamp", "Shade", "Lamp2", "Bulb", "Shade"])

        # As a batch op, and replacing a member with an instance of a new prefab, also record deletions; saving a member
        # as a prefab without replacing it leaves it a member.
        client.call("edit.batch", {"ops": [{"method": "entity.destroy", "params": {"entities": ["/Lamp2/Bulb"]}}]})
        lamp2 = client.call("entity.get", {"entity": "/Lamp2"})["entity"]["components"]
        self.assertEqual([entry["Kind"] for entry in lamp2["Prefab"]["Overrides"]], ["RemoveEntity"])
        client.call("prefab.create", {"entity": "/Lamp/Shade", "path": "Assets/Prefabs/ShadeCopy.prefab"})
        copied = client.call("entity.get", {"entity": "/Lamp/Shade", "components": "all"})["entity"]["components"]
        self.assertIn("PrefabLink", copied)
        self.assertNotIn("Prefab", copied)
        replaced = client.call("prefab.create", {"entity": "/Lamp/Shade", "path": "Assets/Prefabs/Shade.prefab",
                                                 "replaceWithInstance": True})
        self.assertEqual(replaced["instance"]["path"], "/Lamp/Shade")
        self.assertTrue((root / "Assets" / "Prefabs" / "Shade.prefab").is_file())
        lamp = client.call("entity.get", {"entity": "/Lamp", "components": "all"})["entity"]["components"]
        self.assertEqual([entry["Kind"] for entry in lamp["Prefab"]["Overrides"]], ["RemoveEntity", "RemoveEntity"])
        shade = client.call("entity.get", {"entity": "/Lamp/Shade", "components": "all"})["entity"]["components"]
        self.assertIn("Prefab", shade)
        self.assertEqual(shade["PrefabLink"]["InstanceRoot"], replaced["instance"]["id"])

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

    def test_nested_instances_revert_within_and_unpack_one_level(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decision 8 (E-PFB-NEST): prefab.revert on a nested instance root removes
        # the overrides its outermost instance records within it; prefab.unpack refuses a nested root with the call that
        # unpacks its outermost instance, which leaves the lamp an outermost instance keeping its state.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Room", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("entity.create", {"name": "Lamp", "parent": "/Room"})
        client.call("entity.create", {"name": "Bulb", "parent": "/Room/Lamp",
                                      "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        client.call("prefab.create", {"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.create", {"entity": "/Room", "path": "Assets/Prefabs/Room.prefab",
                                      "replaceWithInstance": True})
        client.call("entity.update", {"entity": "/Room", "components": {"MeshRenderer": {"CastShadows": False}}})
        client.call("entity.update", {"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": False}}})

        reverted = client.call("prefab.revert", {"instance": "/Room/Lamp"})
        self.assertEqual(reverted["removedOverrides"], 1)
        room = client.call("entity.get", {"entity": "/Room", "components": "all"})["entity"]["components"]
        self.assertEqual([entry["Field"] for entry in room["Prefab"]["Overrides"]], ["CastShadows"])
        bulb = client.call("entity.get", {"entity": "/Room/Lamp/Bulb", "components": ["MeshRenderer"]})
        self.assertTrue(bulb["entity"]["components"]["MeshRenderer"]["Visible"])

        client.call("entity.update", {"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": False}}})
        with self.assertRaises(engine_client.EngineError) as nested:
            client.call("prefab.unpack", {"instance": "/Room/Lamp"})
        self.assert_engine_error(nested.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertEqual(hinted_params(nested.exception.data.get("hint", ""), "prefab.unpack"), {"instance": "/Room"})
        client.call("prefab.unpack", hinted_params(nested.exception.data.get("hint", ""), "prefab.unpack"))
        lamp = client.call("entity.get", {"entity": "/Room/Lamp", "components": "all"})["entity"]["components"]
        self.assertIsNone(lamp["PrefabLink"]["OuterPrefabEntityID"])
        self.assertEqual([entry["Field"] for entry in lamp["Prefab"]["Overrides"]], ["Visible"])

    def test_apply_level_and_revert_keys_by_path(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decisions 9 and 10: a top-level instance is its own only level, so
        # applying at it is the default apply, and updatedPrefabs is empty when no prefab nests the written one;
        # another entity as the level is refused at /level. prefab.revert's keys name an override by its path; the
        # version 1 key prefabEntityId is an unknown member.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        client.call("entity.create", {"name": "Bulb", "parent": "/Lamp",
                                      "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        client.call("prefab.create", {"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/Lamp.prefab", "name": "Lamp2"})
        client.call("entity.update", {"entity": "/Lamp2/Bulb", "components": {"MeshRenderer": {"CastShadows": False}}})
        undo_entries = len(client.call("edit.history")["entries"])

        with self.assertRaises(engine_client.EngineError) as wrong_level:
            client.call("prefab.apply", {"instance": "/Lamp2", "level": "/Lamp"})
        self.assert_engine_error(wrong_level.exception, engine_client.INVALID_PARAMS)
        self.assertEqual([issue["pointer"] for issue in wrong_level.exception.issues], ["/level"])
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries)

        applied = client.call("prefab.apply", {"instance": "/Lamp2", "level": "/Lamp2"})
        self.assertEqual(applied["prefab"]["path"], "Assets/Prefabs/Lamp.prefab")
        self.assertEqual(applied["updatedInstances"], 2)
        self.assertEqual(applied["updatedPrefabs"], [])
        bulbs = [entity for entity in read_json(root / "Assets" / "Prefabs" / "Lamp.prefab")["Entities"]
                 if entity["Name"] == "Bulb"]
        self.assertFalse(bulbs[0]["Components"]["MeshRenderer"]["CastShadows"])

        client.call("entity.update", {"entity": "/Lamp/Bulb",
                                      "components": {"MeshRenderer": {"CastShadows": True, "Visible": False}}})
        link = client.call("entity.get", {"entity": "/Lamp/Bulb", "components": ["PrefabLink"]})
        path = [link["entity"]["components"]["PrefabLink"]["PrefabEntityID"]]
        with self.assertRaises(engine_client.EngineError) as old_key:
            client.call("prefab.revert", {"instance": "/Lamp", "overrides": [{"prefabEntityId": path[0]}]})
        self.assert_engine_error(old_key.exception, engine_client.INVALID_PARAMS)
        reverted = client.call("prefab.revert", {"instance": "/Lamp", "overrides": [
            {"path": path, "kind": "field", "component": "MeshRenderer", "field": "Visible"}]})
        self.assertEqual(reverted["removedOverrides"], 1)
        bulb = client.call("entity.get", {"entity": "/Lamp/Bulb", "components": ["MeshRenderer"]})
        self.assertTrue(bulb["entity"]["components"]["MeshRenderer"]["Visible"])
        self.assertTrue(bulb["entity"]["components"]["MeshRenderer"]["CastShadows"])

    def test_apply_adds_an_entity_added_below_the_instance(self) -> None:
        # M27 friction F04 (Docs/Decisions/0038-m27-rolling-ball.md): a BottomBar added below a HUD instance never
        # reached HUD.prefab, though prefab.apply reported success, so the HUD instances of the other levels never got
        # it. prefab.apply adds it to the prefab at its place: every instance, in every open scene, gains it, the
        # applied instance holds it once as a member with a new id (addedEntities), and one undo step restores
        # everything. An entity outside the instance that references an added entity makes the apply refuse.
        client, root = self.open_editor_with_scene()
        client.call("entity.create", {"name": "HUD"})
        client.call("entity.create", {"name": "TopBar", "parent": "/HUD"})
        client.call("entity.create", {"name": "Score", "parent": "/HUD"})
        client.call("prefab.create", {"entity": "/HUD", "path": "Assets/Prefabs/HUD.prefab",
                                      "replaceWithInstance": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/HUD.prefab", "name": "HUD2"})
        client.call("scene.new", {"path": "Assets/Scenes/Level2.scene", "keepOpen": True})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/HUD.prefab", "name": "HUD"})
        client.call("scene.activate", {"scene": "Assets/Scenes/Main.scene"})
        level2 = {"scene": "Assets/Scenes/Level2.scene"}
        prefab_file = root / "Assets" / "Prefabs" / "HUD.prefab"
        prefab_before = prefab_file.read_bytes()

        # The builder's batch: the new bar below the instance, then the apply.
        undo_entries = len(client.call("edit.history")["entries"])
        batch = client.call("edit.batch", {"label": "Bottom bar", "ops": [
            {"method": "entity.create", "params": {"name": "BottomBar", "parent": "/HUD", "index": 1}},
            {"method": "entity.create", "params": {"name": "Lives", "parent": "/HUD/BottomBar"}},
            {"method": "prefab.apply", "params": {"instance": "/HUD"}}]})
        bottom = batch["results"][0]["entity"]
        applied = batch["results"][2]
        self.assertEqual([entity["path"] for entity in applied["addedEntities"]], ["/HUD/BottomBar"])
        self.assertNotEqual(applied["addedEntities"][0]["id"], bottom["id"])
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries + 1)
        hud = ["TopBar", "BottomBar", "Lives", "Score"]
        self.assertEqual(tree_names(client.call("scene.tree", {"format": "json"})), ["HUD", *hud, "HUD2", *hud])
        self.assertEqual(tree_names(client.call("scene.tree", {"format": "json", **level2})), ["HUD", *hud])
        added = client.call("entity.get", {"entity": "/HUD/BottomBar", "components": "all"})["entity"]
        self.assertEqual(added["id"], applied["addedEntities"][0]["id"])
        self.assertIn("PrefabLink", added["components"])
        self.assertIn("BottomBar", [entity["Name"] for entity in read_json(prefab_file)["Entities"]])

        # One undo step undoes the batch: the prefab file and every instance as before, the bar gone again.
        client.call("edit.undo")
        self.assertEqual(prefab_file.read_bytes(), prefab_before)
        self.assertEqual(tree_names(client.call("scene.tree", {"format": "json"})),
                         ["HUD", "TopBar", "Score", "HUD2", "TopBar", "Score"])
        self.assertEqual(tree_names(client.call("scene.tree", {"format": "json", **level2})),
                         ["HUD", "TopBar", "Score"])
        client.call("edit.redo")
        self.assertEqual(client.call("entity.get", {"entity": "/HUD2/BottomBar/Lives"})["entity"]["name"], "Lives")

        # A Banner added below HUD2 that the HUD's script references from outside HUD2 is refused, naming the referrer.
        client.call("script.write", {"path": "Assets/Scripts/Hud.luau", "source": HUD_SCRIPT})
        banner = client.call("entity.create", {"name": "Banner", "parent": "/HUD2"})["entity"]
        client.call("entity.create", {"name": "Watcher", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Hud.luau", "Fields": {"Bar": banner["id"]}}]}}})
        prefab_applied = prefab_file.read_bytes()
        with self.assertRaises(engine_client.EngineError) as referenced:
            client.call("prefab.apply", {"instance": "/HUD2"})
        self.assert_engine_error(referenced.exception, engine_client.INVALID_STATE, "InvalidState")
        watcher = client.call("entity.get", {"entity": "/Watcher"})["entity"]
        self.assertEqual(referenced.exception.data.get("location", {}).get("entity"), watcher["id"])
        self.assertEqual(prefab_file.read_bytes(), prefab_applied)
        self.assertEqual(client.call("entity.get", {"entity": "/HUD2/Banner"})["entity"]["id"], banner["id"])

    def test_scene_open_reports_the_instances_rebuilt_from_a_changed_prefab(self) -> None:
        # M27 friction F14 (Docs/Decisions/0038-m27-rolling-ball.md): after prefab_apply gave HUD.prefab a BottomBar in
        # Level 1, scene_open of Level 2 returned dirty: true with no diagnostics or repairs, so nothing said why the
        # freshly opened scene was unsaved. Its result now lists each instance the load rebuilt with a change, with its
        # prefab; a scene opened with its prefabs as saved lists none and opens clean.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "HUD"})
        client.call("entity.create", {"name": "TopBar", "parent": "/HUD"})
        client.call("prefab.create", {"entity": "/HUD", "path": "Assets/Prefabs/HUD.prefab",
                                      "replaceWithInstance": True})
        client.call("scene.save")
        client.call("scene.new", {"path": "Assets/Scenes/Level2.scene"})
        client.call("prefab.instantiate", {"prefab": "Assets/Prefabs/HUD.prefab", "name": "HUD"})
        client.call("scene.save")
        unchanged = client.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        self.assertEqual(unchanged["rebuiltInstances"], [])
        self.assertFalse(unchanged["scene"]["dirty"])

        client.call("entity.create", {"name": "BottomBar", "parent": "/HUD"})
        client.call("prefab.apply", {"instance": "/HUD"})
        client.call("scene.save")
        opened = client.call("scene.open", {"path": "Assets/Scenes/Level2.scene"})
        self.assertTrue(opened["scene"]["dirty"])
        self.assertEqual([(rebuilt["instance"]["path"], rebuilt["prefab"]["path"])
                          for rebuilt in opened["rebuiltInstances"]], [("/HUD", "Assets/Prefabs/HUD.prefab")])
        self.assertEqual(tree_names(client.call("scene.tree", {"format": "json"})), ["HUD", "TopBar", "BottomBar"])
        client.call("scene.save")
        reloaded = client.call("scene.open", {"path": "Assets/Scenes/Level2.scene", "reload": True})
        self.assertEqual(reloaded["rebuiltInstances"], [])
        self.assertFalse(reloaded["scene"]["dirty"])

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
