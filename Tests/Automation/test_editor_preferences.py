"""Editor startup with unusable preferences (user://Editor.json): the editor runs with the defaults, reports the problem
and leaves the file for the user to repair (Editor/Private/EditorHostPreferences.h). The user's "Allow native code"
preference lives in the same file and only there (Docs/Decisions/0030-m19-contract.md decision 29)."""

from __future__ import annotations

import json
import unittest

from harness import AutomationTestCase, app_name, automation_config, engine_client


class EditorPreferencesTests(AutomationTestCase):
    def test_unreadable_preferences_do_not_stop_the_editor(self) -> None:
        path = self.user_data / app_name() / "Editor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        # A newer editor's file (after a downgrade) and a damaged one.
        for text in ('{"Format": "EditorPreferences", "Version": 2, "AllowAiAutomation": true}',
                     '{"Format": "EditorPref'):
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

    def test_allow_native_code_is_read_from_the_user_preferences_and_kept_by_the_editor(self) -> None:
        # Decision 29: the editor reads AllowNativeCode from the user's Editor.json when a project opens, a file it
        # cannot read leaves native code off, and the editor's own writes of the file (the recent projects) keep the
        # preference as the user left it. The export of a game with Native/ shows which way it was read: while native
        # code is off it is refused first (PermissionDenied, decision 33); while it is on, a Native/ without a source is
        # NotFound.
        path = self.user_data / app_name() / "Editor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        cases = (('{"Format": "EditorPreferences", "Version": 1, "AllowNativeCode": true, "Future": {"Kept": [1, 2]}}',
                  "NotFound", True),
                 ('{"Format": "EditorPreferences", "Version": 1, "AllowNativeCode": false}', "PermissionDenied", False),
                 ('{"Format": "EditorPreferences", "Version": 1}', "PermissionDenied", None),
                 ('{"Format": "EditorPreferences", "Version": 2, "AllowNativeCode": true}', "PermissionDenied", None))
        for text, refusal, kept in cases:
            with self.subTest(text=text):
                path.write_text(text, encoding="utf-8")
                editor = self.start_editor()
                client = self.connect(editor)
                name = f"NativePreference{len(self.editors)}"
                root = self.create_project(client, name)
                (root / "Native").mkdir()
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("project.export", {"config": automation_config()})
                self.assertEqual(refused.exception.error_code, refusal, str(refused.exception))
                self.assertFalse((root / "Build").exists())
                client.call("session.shutdown", {"force": True})
                self.assertEqual(editor.wait(), 0, editor.output())
                if kept is None and '"Version": 2' in text:
                    # A file this editor cannot read is left for the user to repair.
                    self.assertEqual(path.read_text(encoding="utf-8"), text)
                    continue
                if kept is None:
                    # A file without the member gets none from the editor's own writes.
                    self.assertNotIn("AllowNativeCode", path.read_text(encoding="utf-8"))
                    continue
                document = json.loads(path.read_text(encoding="utf-8"))
                self.assertIs(document["AllowNativeCode"], kept)
                self.assertIn(str(root / f"{name}.eproj").replace("\\", "/"),
                              [entry.replace("\\", "/") for entry in document["RecentProjects"]])
                if "Future" in text:
                    self.assertEqual(document["Future"], {"Kept": [1, 2]})


if __name__ == "__main__":
    unittest.main()
