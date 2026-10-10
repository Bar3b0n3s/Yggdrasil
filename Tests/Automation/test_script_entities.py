"""Script field writes, reference remapping and session memory through automation."""

from __future__ import annotations

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
        client.call("entity.update", {"entity": "/Root", "components": {"Script": {
            "Script": path, "Fields": {"Target": child, "Copies": [child], "Label": child, "Amount": 2},
        }}})
        patched = client.call("entity.update", {"entity": "/Root", "components": {
            "Script": {"Fields": {"Amount": 3}},
        }})
        self.assertEqual(patched["entity"]["components"]["Script"]["Fields"]["Amount"], 3)
        with self.assertRaises(engine_client.EngineError) as invalid:
            client.call("entity.update", {"entity": "/Root", "components": {
                "Script": {"Fields": {"Amount": 6}},
            }})
        self.assert_engine_error(invalid.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
        self.assertEqual(invalid.exception.data["issues"][0]["pointer"], "/components/Script/Fields/Amount")
        self.assertNotIn("no script is assigned", invalid.exception.data["detail"])
        copy = client.call("entity.duplicate", {"entities": ["/Root"]})["entities"][0]["id"]
        fields = client.call("entity.get", {"entity": copy})["entity"]["components"]["Script"]["Fields"]
        self.assertNotEqual(fields["Target"], child)
        self.assertEqual(fields["Copies"], [fields["Target"]])
        self.assertEqual(fields["Label"], child)
        self.assertEqual(fields["Amount"], 3)
        client.call("edit.undo")
        client.call("edit.redo")
        self.assertEqual(client.call("entity.get", {"entity": copy})["entity"]["components"]["Script"]["Fields"], fields)

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
