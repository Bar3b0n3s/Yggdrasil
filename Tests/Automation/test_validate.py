"""project.validate with selective fixes (Docs/Architecture.md §13.7, Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class ValidateTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_validate_fix_selected_ids_only(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Export": {"BuildScenes": ["Assets/Scenes/Main.scene",
                                                                                  "Assets/Scenes/Gone.scene"]}}})
        for name in ("CameraA", "CameraB"):
            client.call("entity.create", {"name": name, "components": {"Camera": {"Primary": True}}})
        report = client.call("project.validate")
        by_code = {diagnostic["code"]: diagnostic for diagnostic in report["diagnostics"]}
        self.assertIn("SCENE_MULTIPLE_PRIMARY_CAMERAS", by_code)
        self.assertIn("BUILD_SCENE_MISSING", by_code)
        self.assertTrue(by_code["BUILD_SCENE_MISSING"]["autoFixable"])
        target = by_code["BUILD_SCENE_MISSING"]["id"]
        # Ids are stable: validating again gives the same ones.
        again = {diagnostic["code"]: diagnostic["id"] for diagnostic in client.call("project.validate")["diagnostics"]}
        self.assertEqual(again["BUILD_SCENE_MISSING"], target)

        fixed = client.call("project.validate", {"fix": [target]})
        self.assertEqual(fixed["fixed"], [target])
        codes = {diagnostic["code"] for diagnostic in fixed["diagnostics"]}
        self.assertNotIn("BUILD_SCENE_MISSING", codes)
        self.assertIn("SCENE_MULTIPLE_PRIMARY_CAMERAS", codes)
        self.assertEqual(client.call("project.getSettings")["settings"]["Export"]["BuildScenes"],
                         ["Assets/Scenes/Main.scene"])
        # The fixes are one undoable command.
        client.call("edit.undo")
        self.assertEqual(len(client.call("project.getSettings")["settings"]["Export"]["BuildScenes"]), 2)
        by_code_fix = client.call("project.validate", {"fix": ["SCENE_MULTIPLE_PRIMARY_CAMERAS"]})
        remaining = {diagnostic["code"] for diagnostic in by_code_fix["diagnostics"]}
        self.assertNotIn("SCENE_MULTIPLE_PRIMARY_CAMERAS", remaining)


if __name__ == "__main__":
    unittest.main()
