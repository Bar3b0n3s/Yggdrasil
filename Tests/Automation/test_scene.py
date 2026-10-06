"""Scene lifecycle through automation (Docs/Architecture.md §13.5 scene.*, Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class SceneTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_scene_open_dirty_requires_save_or_discard(self) -> None:
        client, root = self.open_editor_with_scene()
        client.call("scene.new", {"path": "Assets/Scenes/Other.scene", "discardChanges": True})
        client.call("entity.create", {"name": "Unsaved"})
        self.assertTrue(client.call("session.info")["_meta"]["dirty"])

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("scene.open", {"path": "Assets/Scenes/Main.scene"})
        self.assert_engine_error(refused.exception, engine_client.INVALID_STATE, "InvalidState")
        with self.assertRaises(engine_client.EngineError) as both:
            client.call("scene.open", {"path": "Assets/Scenes/Main.scene", "save": True, "discardChanges": True})
        self.assert_engine_error(both.exception, engine_client.INVALID_PARAMS)

        opened = client.call("scene.open", {"path": "Assets/Scenes/Main.scene", "save": True})
        self.assertEqual(opened["scene"]["path"], "Assets/Scenes/Main.scene")
        self.assertIn("Unsaved", (root / "Assets" / "Scenes" / "Other.scene").read_text(encoding="utf-8"))

        client.call("entity.create", {"name": "Dropped"})
        client.call("scene.open", {"path": "Assets/Scenes/Other.scene", "discardChanges": True})
        self.assertNotIn("Dropped", (root / "Assets" / "Scenes" / "Main.scene").read_text(encoding="utf-8"))
        self.assertFalse(client.call("session.info")["_meta"]["dirty"])


if __name__ == "__main__":
    unittest.main()
