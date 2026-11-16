"""Script field writes, reference remapping and session memory through automation."""

from __future__ import annotations

import copy
import unittest

from harness import AutomationTestCase, engine_client


class ScriptEntityTests(AutomationTestCase):
    def test_script_field_validation_and_duplicate_references(self) -> None:
        client, _ = self.open_editor_with_scene()
        path = "Assets/Scripts/Links.luau"
        client.call("script.write", {"path": path, "source": """
local Links = { Fields = {
    Target = Field.Entity(), Copies = Field.Array(Field.Entity()),
    Label = Field.String(), Amount = Field.Number(1, {Min = 0, Max = 5}),
} }
return Script.Define("Links", Links)
"""})
        client.call("entity.create", {"name": "Root"})
        child = client.call("entity.create", {"name": "Child", "parent": "/Root"})["entity"]["id"]
        assigned = {"Target": child, "Copies": [child], "Label": child, "Amount": 2}
        client.call("entity.update", {"entity": "/Root", "components": {"Script": {"Scripts": [{
            "Script": path, "Fields": assigned,
        }]}}})
        # A slot given Fields alone borrows the slot's Script handle; a list write replaces the slot's Fields whole.
        patched = client.call("entity.update", {"entity": "/Root", "components": {
            "Script": {"Scripts": [{"Fields": dict(assigned, Amount=3)}]},
        }})
        self.assertEqual(patched["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Amount"], 3)
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("entity.update", {"entity": "/Root", "components": {
                "Script": {"Scripts": [{"Fields": {"Amount": 6}}]},
            }})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(invalid.exception.data["issues"][0]["pointer"], "/components/Script/Scripts/0/Fields/Amount")
        self.assertNotIn("no script is assigned", invalid.exception.data["detail"])
        copy = client.call("entity.duplicate", {"entities": ["/Root"]})["entities"][0]["id"]
        fields = client.call("entity.get", {"entity": copy})["entity"]["components"]["Script"]["Scripts"][0]["Fields"]
        self.assertNotEqual(fields["Target"], child)
        self.assertEqual(fields["Copies"], [fields["Target"]])
        self.assertEqual(fields["Label"], child)
        self.assertEqual(fields["Amount"], 3)
        client.call("edit.undo")
        client.call("edit.redo")
        restored = client.call("entity.get", {"entity": copy})["entity"]["components"]["Script"]
        self.assertEqual(restored["Scripts"][0]["Fields"], fields)

    def test_script_slots_are_edited_as_a_list_and_overridden_per_slot(self) -> None:
        # Docs/Decisions/0031-m20-contract.md decisions 17 and 19 (S-MULTI): the inspector adds, removes and reorders
        # script slots by writing the whole list through entity.update (an array replaces the list), one undo step each,
        # and scene.tree names every slot's script. In a prefab instance an edit of one slot is recorded for that slot
        # ("Scripts/<slot>/<member>") and reverted by that key.
        client, _ = self.open_editor_with_scene()
        handles = {}
        for name in ("Aim", "Fire"):
            written = client.call("script.write", {
                "path": f"Assets/Scripts/{name}.luau",
                "source": f'return Script.Define("{name}", {{Fields = {{Amount = Field.Number(1)}}}})',
            })
            handles[name] = written["script"]["id"]
        client.call("entity.create", {"name": "Turret", "components": {"Script": {"Scripts": [
            {"Script": handles["Aim"], "Fields": {"Amount": 2}}, {"Script": handles["Fire"], "ExecutionOrder": 5}]}}})

        def read_slots(entity: str = "/Turret") -> list:
            components = client.call("entity.get", {"entity": entity, "components": ["Script"]})["entity"]["components"]
            return components["Script"]["Scripts"]

        def write_slots(slots: list, entity: str = "/Turret") -> None:
            client.call("entity.update", {"entity": entity, "components": {"Script": {"Scripts": slots}}})

        undo_entries = len(client.call("edit.history")["entries"])
        slots = read_slots()
        write_slots([slots[1], slots[0]])
        self.assertEqual([slot["Script"] for slot in read_slots()], [handles["Fire"], handles["Aim"]])
        self.assertEqual(read_slots()[1]["Fields"], {"Amount": 2})
        self.assertEqual(read_slots()[0]["ExecutionOrder"], 5)
        write_slots(read_slots() + [{}])
        self.assertEqual(len(read_slots()), 3)
        tree = client.call("scene.tree")["text"]
        self.assertIn("Script(Scripts/Fire.luau, Scripts/Aim.luau, none)", tree)
        write_slots(read_slots()[1:2])
        self.assertEqual([slot["Script"] for slot in read_slots()], [handles["Aim"]])
        self.assertEqual(len(client.call("edit.history")["entries"]), undo_entries + 3)
        client.call("edit.undo", {"steps": 3})
        self.assertEqual([slot["Script"] for slot in read_slots()], [handles["Aim"], handles["Fire"]])

        # An instance of a prefab with both slots records an edit of slot 1's fields for slot 1 alone.
        client.call("prefab.create", {"entity": "/Turret", "path": "Assets/Prefabs/Turret.prefab",
                                      "replaceWithInstance": True})
        slots = read_slots()
        slots[1]["Fields"] = {"Amount": 4}
        write_slots(slots)
        overrides = client.call("entity.get", {"entity": "/Turret"})["entity"]["components"]["Prefab"]["Overrides"]
        self.assertEqual([(entry["Component"], entry["Field"]) for entry in overrides],
                         [("Script", "Scripts/1/Fields")])
        self.assertEqual(overrides[0]["Value"], {"Amount": 4})
        reverted = client.call("prefab.revert", {"instance": "/Turret", "overrides": [
            {"path": overrides[0]["Path"], "kind": "Field", "component": "Script", "field": "Scripts/1/Fields"}]})
        self.assertEqual(reverted["removedOverrides"], 1)
        self.assertEqual(read_slots()[1].get("Fields", {}), {})
        self.assertEqual(read_slots()[0]["Fields"], {"Amount": 2})

    def test_an_empty_script_slot_entry_leaves_its_slot_unchanged(self) -> None:
        # World Tree's brief 19: a slot entry without a Script patches the slot at its index and keeps what it omits,
        # so {} leaves its slot unchanged as {"Fields": {}} does, with dryRun and without, and a prefab instance
        # records only the patched slot's changed fields. A shorter list still removes the slots past its end.
        client, _ = self.open_editor_with_scene()
        names = ("Controller", "Combat", "Health", "Sap")
        handles = {}
        fields = "{Amount = Field.Number(1), Clip = Field.Number(0)}"
        for name in names:
            written = client.call("script.write", {
                "path": f"Assets/Scripts/{name}.luau",
                "source": f'return Script.Define("{name}", {{Fields = {fields}}})',
            })
            handles[name] = written["script"]["id"]
        client.call("entity.create", {"name": "Warden", "components": {"Script": {"Scripts": [
            {"Script": handles[name], "Fields": {"Amount": index + 2}, "ExecutionOrder": index + 1}
            for index, name in enumerate(names)]}}})
        client.call("prefab.create", {"entity": "/Warden", "path": "Assets/Prefabs/Warden.prefab",
                                      "replaceWithInstance": True})

        def read_slots() -> list:
            entity = client.call("entity.get", {"entity": "/Warden", "components": ["Script"]})["entity"]
            return entity["components"]["Script"]["Scripts"]

        before = read_slots()
        expected = copy.deepcopy(before)
        expected[0]["Fields"]["Clip"] = 7
        patch = {"Script": {"Scripts": [{"Fields": {"Clip": 7}}, {}, {}, {}]}}
        for dry_run in (True, False):
            with self.subTest(dry_run=dry_run):
                params = {"entity": "/Warden", "components": patch}
                if dry_run:
                    params["dryRun"] = True
                components = client.call("entity.update", params)["entity"]["components"]
                self.assertEqual(components["Script"]["Scripts"], expected)
                self.assertEqual([(entry["Component"], entry["Field"], entry["Value"])
                                  for entry in components["Prefab"]["Overrides"]],
                                 [("Script", "Scripts/0/Fields", {"Clip": 7})])
                self.assertEqual(read_slots(), expected if not dry_run else before)

        shorter = client.call("entity.update", {"entity": "/Warden", "dryRun": True,
                                                "components": {"Script": {"Scripts": [{}, {}]}}})
        self.assertEqual(shorter["entity"]["components"]["Script"]["Scripts"], expected[:2])

    def test_stats_reports_script_memory_only_with_a_live_vm(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.assertFalse(client.call("stats.get")["scriptAvailable"])
        client.call("play.start", {"lockstep": True})
        before = client.call("play.state")["tick"]
        stats = client.call("stats.get")
        self.assertTrue(stats["scriptAvailable"])
        self.assertGreater(stats["scriptHeapBytes"], 0)
        self.assertGreater(stats["scriptSoftLimitBytes"], stats["scriptHeapBytes"])
        self.assertGreater(stats["scriptHardLimitBytes"], stats["scriptSoftLimitBytes"])
        self.assertEqual(client.call("play.state")["tick"], before)
        client.call("play.stop")
        self.assertFalse(client.call("stats.get")["scriptAvailable"])


if __name__ == "__main__":
    unittest.main()
