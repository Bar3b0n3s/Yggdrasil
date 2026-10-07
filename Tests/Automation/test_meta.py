"""Responses: the _meta diagnostics delta, bounded output and enum spelling (Docs/Architecture.md §13.4, Roadmap M4)."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client


class MetaTests(AutomationTestCase):
    def test_meta_reports_new_warning(self) -> None:
        client, root = self.open_editor_with_scene()
        # A scene file with an unknown component loads with one warning, which the next response reports.
        scene = json.loads((root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))
        scene["Entities"] = [{"ID": "5d1c9a7e33b04f12", "Name": "Gadget", "Parent": None, "Active": True, "Tags": [],
                              "Components": {"FutureThing": {"Level": 3}}}]
        scene["ComponentVersions"] = {"FutureThing": 1}
        (root / "Assets" / "Scenes" / "Odd.scene").write_text(json.dumps(scene, indent="\t") + "\n", encoding="utf-8")
        quiet = client.call("session.info")
        self.assertEqual(quiet["_meta"]["diagnostics"]["newWarnings"], 0)
        opened = client.call("scene.open", {"path": "Assets/Scenes/Odd.scene"})
        diagnostics = opened["_meta"]["diagnostics"]
        self.assertEqual(diagnostics["newWarnings"], 1)
        self.assertEqual(diagnostics["newErrors"], 0)
        self.assertIn("FutureThing", diagnostics["firstNew"][0]["message"])
        self.assertEqual(client.call("session.info")["_meta"]["diagnostics"]["newWarnings"], 0)

    def test_large_result_offloaded(self) -> None:
        client, root = self.open_editor_with_scene()
        ops = [{"method": "entity.create", "params": {"name": f"Cell{index:04}", "tags": ["Cell"]}}
               for index in range(600)]
        client.call("edit.batch", {"label": "Cells", "ops": ops})
        result = client.call("scene.get")
        self.assertTrue(result["truncated"])
        offloaded = Path(result["path"])
        self.assertTrue(offloaded.is_file())
        self.assertEqual(offloaded.parent, root / "Library" / "Automation" / "Out")
        self.assertEqual(len(json.loads(offloaded.read_text(encoding="utf-8"))["scene"]["Entities"]), 600)
        self.assertEqual(result["summary"]["scene"]["type"], "object")
        self.assertIn("_meta", result)
        self.assertEqual(len(engine_client.load_offloaded(result)["scene"]["Entities"]), 600)
        small = client.call("project.info")
        self.assertIs(engine_client.load_offloaded(small), small)

    def test_enum_values_case_insensitive_and_echoed_canonically(self) -> None:
        client, _ = self.open_editor_with_scene()
        created = client.call("entity.create", {"name": "Camera", "components": {
            "Camera": {"Projection": "orthographic", "Clear": "COLOR"},
            "RigidBody": {"Type": "kinematic", "MotionQuality": "linearcast"}}})
        entity = client.call("entity.get", {"entity": created["entity"]["id"]})["entity"]
        self.assertEqual(entity["components"]["Camera"]["Projection"], "Orthographic")
        self.assertEqual(entity["components"]["Camera"]["Clear"], "Color")
        self.assertEqual(entity["components"]["RigidBody"]["Type"], "Kinematic")
        self.assertEqual(entity["components"]["RigidBody"]["MotionQuality"], "LinearCast")
        tree = client.call("scene.tree", {"format": "JSON", "target": "EDIT"})
        self.assertEqual(tree["entities"][0]["name"], "Camera")
        history = client.call("edit.history", {"limit": 1})
        self.assertEqual(history["entries"][0]["origin"], "Agent")


if __name__ == "__main__":
    unittest.main()
