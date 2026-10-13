"""Editor startup with unusable preferences (user://Editor.json): the editor runs with the defaults, reports the problem
and leaves the file for the user to repair (Editor/Private/EditorHostPreferences.h)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, app_name


class EditorPreferencesTests(AutomationTestCase):
    def test_unreadable_preferences_do_not_stop_the_editor(self) -> None:
        path = self.user_data / app_name() / "Editor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        # A newer editor's file (after a downgrade) and a damaged one.
        for text in ('{"Format": "EditorPreferences", "Version": 2, "AllowAiAutomation": true}', '{"Format": "EditorPref'):
            with self.subTest(text=text):
                path.write_text(text, encoding="utf-8")
                editor = self.start_editor()
                client = self.connect(editor)
                self.assertTrue(client.call("session.info")["headless"])
                root = self.create_project(client, f"Preferences{len(self.editors)}")
                self.assertTrue((root / f"Preferences{len(self.editors)}.eproj").exists())
                client.call("session.shutdown", {"force": True})
                self.assertEqual(editor.wait(), 0, editor.output())
                self.assertIn("Editor preferences:", editor.output())
                self.assertEqual(path.read_text(encoding="utf-8"), text)


if __name__ == "__main__":
    unittest.main()
