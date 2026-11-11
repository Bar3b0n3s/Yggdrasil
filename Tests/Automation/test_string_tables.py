"""String tables through automation (Docs/Architecture.md §6, §7.4, §13.5 asset.create {type: "StringTable"},
asset.getProperties and asset.setProperties; Docs/Decisions/0033-m22-contract.md decision 10): a table created with
its locales and entries, edited as registry JSON, read by a script through Loc.Get, and the located refusals of invalid
tables."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client, read_json

TABLE_PATH = "Assets/Text/Menu.stringtable"


def menu_values() -> dict[str, object]:
    """Two locales; "Menu.Start" in both, "Menu.Quit" in the fallback only."""
    return {
        "Locales": ["en", "fr"],
        "Entries": [
            {"Key": "Menu.Start", "Values": {"en": "Start", "fr": "Commencer"}},
            {"Key": "Menu.Quit", "Values": {"en": "Quit"}},
        ],
    }


class StringTableTests(AutomationTestCase):
    """asset.create, asset.getProperties and asset.setProperties for string tables, and Loc.Get in edit and play."""

    def test_create_edit_and_read_a_string_table_in_the_project_locale(self) -> None:
        client, root = self.open_editor_with_scene()
        created = client.call("asset.create", {"type": "StringTable", "path": TABLE_PATH, "values": menu_values()})
        self.assertEqual(created["asset"]["type"], "StringTable")
        self.assertEqual(read_json(root / "Assets" / "Text" / "Menu.stringtable")["Format"], "StringTable")
        self.assertEqual(read_json(root / "Assets" / "Text" / "Menu.stringtable.meta")["Importer"], "StringTable")
        values = client.call("asset.getProperties", {"asset": TABLE_PATH})["values"]
        self.assertEqual(values["Locales"], ["en", "fr"])
        patched = client.call("asset.setProperties", {"asset": TABLE_PATH, "values": {"Locales": ["en", "fr", "de"]}})
        self.assertEqual(patched["values"]["Locales"], ["en", "fr", "de"])
        client.call("edit.undo")

        # The project's locale reaches Loc.Get in an edit evaluation and in play.
        client.call("project.setSettings", {"patch": {"Localization": {"Locale": "fr-CA"}}})
        code = f'return Loc.Get(Assets.Load("{TABLE_PATH}"), "Menu.Start")'
        self.assertEqual(client.call("script.eval", {"code": code, "context": "edit"})["value"], "Commencer")
        client.call("play.start", {"lockstep": True})
        evaluated = client.call("script.eval", {"code": code, "context": "play"})
        self.assertEqual(evaluated["value"], "Commencer")
        client.call("play.stop")

    def test_invalid_string_tables_are_refused_at_the_offending_value(self) -> None:
        client, _ = self.open_editor_with_scene()
        values = menu_values()
        values["Entries"][1]["Key"] = "Menu.Start"  # type: ignore[index]
        with self.assertRaises(engine_client.EngineError) as duplicate:
            client.call("asset.create", {"type": "StringTable", "path": TABLE_PATH, "values": values})
        self.assert_engine_error(duplicate.exception, engine_client.VALIDATION_FAILED)
        self.assertEqual(duplicate.exception.issues[0]["pointer"], "/values/Entries/1/Key")


if __name__ == "__main__":
    unittest.main()
