"""M38's session locale through automation (Docs/Decisions/0055-m38-runtime-locale.md decisions 3, 5, 6 and 7):
play.start's locale, which ui.tree then measures the texts in, play.state's locale as Loc.SetLocale changes it,
input.record's locale that the replay's header records and input.replay plays in, and test.run's locale for the suites
whose entry names none."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

TABLE = "Assets/Localization/Menu.stringtable"

# A caption that draws the table's Title into its UiText when it starts and again when the locale changes, so the play
# scene's texts follow the session's locale.
CAPTION = """local Caption = {}
function Caption.Draw(self: any)
\tself.Entity.UiText.Text = Loc.Get(Assets.Load("Assets/Localization/Menu.stringtable"), "Title")
end
function Caption.OnStart(self: any) Caption.Draw(self) end
function Caption.OnLocaleChanged(self: any, locale: string, previous: string) Caption.Draw(self) end
function Caption.OnFixedUpdate(self: any)
\tif Input.IsKeyPressed("L") then Loc.SetLocale(if Loc.GetLocale() == "ar" then "de" else "ar") end
end
return Script.Define("Caption", Caption)
"""

SUITE = """return Test.Suite("Strings", function()
\tTest.Case("reads its locale", function()
\t\tTest.ExpectEqual(Loc.GetLocale(), Scene.GetLoadParameters().Expected)
\tend)
end)
"""


class LocaleTests(AutomationTestCase):
    def add_caption(self, client: engine_client.EngineClient) -> None:
        client.call("asset.create", {"type": "StringTable", "path": TABLE, "values": {
            "Locales": ["en", "de", "ar"],
            "Entries": [{"Key": "Title", "Values": {"en": "World Tree", "de": "Weltenbaum", "ar": "شجرة العالم"}}]}})
        client.call("script.write", {"path": "Assets/Scripts/Caption.luau", "source": CAPTION})
        client.call("entity.create", {"name": "Menu", "components": {"UiCanvas": {}}})
        client.call("entity.create", {"name": "Title", "parent": "/Menu", "components": {
            "UiRect": {"Size": [400, 60]}, "UiText": {"Text": "", "Size": 32},
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Caption.luau"}]}}})
        client.call("scene.save")

    @staticmethod
    def title_text(client: engine_client.EngineClient) -> str:
        tree = client.call("ui.tree", {"target": "Play", "format": "Json"})
        for element in tree["elements"]:
            if element["path"] == "/Menu/Title":
                return element["text"]
        raise AssertionError("ui.tree lists no /Menu/Title")

    def test_play_start_locale_starts_the_session_in_it_and_ui_tree_measures_its_texts(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.add_caption(client)
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("play.start", {"locale": "de_DE"})
        self.assertEqual(refused.exception.code, engine_client.INVALID_PARAMS)
        self.assertEqual(refused.exception.issues[0]["pointer"], "/locale")
        self.assertEqual(client.call("play.state")["state"], "Edit")
        started = client.call("play.start", {"locale": "de", "lockstep": True})
        self.assertEqual(started["locale"], "de")
        client.call("play.step", {"ticks": 1, "render": "none"})
        self.assertEqual(self.title_text(client), "Weltenbaum")
        # Loc.SetLocale from an evaluation: OnLocaleChanged redraws the caption before it returns, and play.state
        # follows.
        client.call("script.eval", {"context": "play", "code": "Loc.SetLocale('ar')"})
        self.assertEqual(client.call("play.state")["locale"], "ar")
        self.assertEqual(self.title_text(client), "شجرة العالم")
        client.call("play.stop")
        # Absent, the project's locale; "" the empty locale, whose lookups read the fallback.
        self.assertEqual(client.call("play.start", {"lockstep": True})["locale"],
                         client.call("project.getSettings")["settings"]["Localization"]["Locale"])
        client.call("play.stop")
        self.assertEqual(client.call("play.start", {"locale": "", "lockstep": True})["locale"], "")
        client.call("play.step", {"ticks": 1, "render": "none"})
        self.assertEqual(self.title_text(client), "World Tree")

    def test_a_recording_starts_in_its_locale_and_its_replay_plays_in_it(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.add_caption(client)
        client.call("input.record", {"action": "Start", "locale": "de"})
        client.call("play.step", {"ticks": 90, "render": "none",
                                  "input": [{"tick": 30, "type": "Key", "key": "L", "state": "Tap"}]})
        self.assertEqual(client.call("play.state")["locale"], "ar")
        client.call("input.record", {"action": "Stop", "path": "Assets/Tests/German.replay"})
        read = client.call("input.readReplay", {"path": "Assets/Tests/German.replay"})
        self.assertEqual(read["header"]["Locale"], "de")
        replayed = client.call("input.replay",
                               {"path": "Assets/Tests/German.replay", "verify": True, "strictHash": True})
        self.assertTrue(replayed["passed"])
        # The replay started in its header's locale, then its input switched it, as recorded.
        self.assertEqual(client.call("play.state")["locale"], "ar")

    def test_test_run_locale_starts_the_suites_that_name_none_in_it(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Tests/Strings.test.luau", "source": SUITE})
        inherits = {"Script": "Assets/Tests/Strings.test.luau", "Scene": "", "Modes": ["Editor"],
                    "Parameters": {"Expected": "ar"}}
        german = {**inherits, "Locale": "de", "Parameters": {"Expected": "de"}}
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [inherits, german]}}})
        result = client.call("test.run", {"locale": "ar"})
        self.assertTrue(result["passed"], result)
        self.assertEqual([(row["suite"], row["locale"]) for row in result["suites"]],
                         [("Strings", "ar"), ("Strings (de)", "de")])
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("test.run", {"locale": ""})
        self.assertEqual(refused.exception.issues[0]["pointer"], "/locale")


if __name__ == "__main__":
    unittest.main()
