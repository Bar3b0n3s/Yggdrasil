"""ui.tree through automation (Docs/Architecture.md §13.5; Docs/Decisions/0033-m22-contract.md decision 8): a scene's
UI laid out in scene pixels, in the editor's edit and play scenes and in an exported game's Runtime, which serves it as
part of its subset."""

from __future__ import annotations

import unittest

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game, exported_executable


def create_ui(client: engine_client.EngineClient) -> None:
    """A canvas at two scene pixels per unit holding a panel (an image) with a label (a text) stretched over it."""
    panel_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 5], "Size": [40, 20]}
    label_rect = {"AnchorMin": [0, 0], "AnchorMax": [1, 1], "Size": [0, 0]}
    client.call("edit.batch", {"label": "Ui", "ops": [
        {"method": "entity.create", "params": {"name": "Hud", "components": {"UiCanvas": {"ScaleFactor": 2}}}},
        {"method": "entity.create", "params": {"name": "Panel", "parent": {"$ref": "0.entity.id"},
                                               "components": {"UiRect": panel_rect, "UiImage": {}}}},
        {"method": "entity.create", "params": {"name": "Label", "parent": {"$ref": "1.entity.id"},
                                               "components": {"UiRect": label_rect, "UiText": {"Text": "Score"}}}},
    ]})


class UiTreeTests(AutomationTestCase):
    """ui.tree in the editor and the Runtime."""

    def test_ui_tree_lists_the_edit_and_play_scene_ui_in_scene_pixels(self) -> None:
        client, _ = self.open_editor_with_scene()
        create_ui(client)
        listed = client.call("ui.tree", {"format": "Json", "width": 200, "height": 100})
        self.assertEqual((listed["width"], listed["height"]), (200, 100))
        names = [element["name"] for element in listed["elements"]]
        self.assertEqual(names, ["Hud", "Panel", "Label"])
        # The canvas's scale turns scene pixels into the canvas units UiRect takes; an element reports 0.
        self.assertEqual(listed["elements"][0]["scale"], 2)
        panel = listed["elements"][1]
        self.assertEqual(panel["scale"], 0)
        self.assertEqual(panel["rect"], {"x": 20, "y": 10, "width": 80, "height": 40})
        self.assertEqual(panel["components"], ["UiRect", "UiImage"])
        self.assertTrue(panel["visible"])
        self.assertEqual(listed["elements"][2]["text"], "Score")
        outline = client.call("ui.tree")["text"]
        self.assertIn('UiText("Score")', outline)

        # The play scene: the session's current layout, at its view (the project's window size in lockstep).
        window = client.call("project.getSettings")["settings"]["Window"]
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 1, "render": "none"})
        played = client.call("ui.tree", {"format": "Json", "target": "play"})
        self.assertEqual((played["width"], played["height"]), (window["Width"], window["Height"]))
        self.assertEqual(len(played["elements"]), 3)
        client.call("play.stop")

        with self.assertRaises(engine_client.EngineError) as not_canvas:
            client.call("ui.tree", {"canvas": "/Hud/Panel"})
        self.assert_engine_error(not_canvas.exception, engine_client.INVALID_PARAMS)

    def test_ui_tree_works_in_the_runtime(self) -> None:
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        create_ui(client)
        client.call("scene.save")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.editors[-1].wait()

        arguments = ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Automated", TINY_GAME_NAME)
        self.editors.append(game)
        runtime = self.connect(game)
        runtime.call("play.step", {"ticks": 1})
        listed = runtime.call("ui.tree", {"format": "Json"})
        self.assertEqual([element["name"] for element in listed["elements"]], ["Hud", "Panel", "Label"])
        self.assertTrue(listed["elements"][1]["visible"])
        runtime.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())


CLICK_COUNTER = """local T = {}
function T:OnCreate() self.Clicks = 0 end
function T:OnUiClick(element) self.Clicks += 1 self.Last = element.Name end
return Script.Define("ClickCounter", T)
"""


class UiInteractionTests(AutomationTestCase):
    """The UI's interaction through automation (Docs/Decisions/0033-m22-contract.md decisions 24, 27 and 31)."""

    def test_a_click_through_play_step_input_reaches_on_ui_click_and_ui_tree_reports_the_state(self) -> None:
        # A press and release injected through play.step over a button: the click bubbles from the button to the
        # canvas's behaviour in the next UiEvents phase, and ui.tree reports the play session's hover and focus.
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/ClickCounter.luau", "source": CLICK_COUNTER})
        button_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 10],
                       "Size": [100, 40]}
        client.call("edit.batch", {"label": "Ui", "ops": [
            {"method": "entity.create", "params": {"name": "Hud", "components": {
                "UiCanvas": {}, "Script": {"Scripts": [{"Script": "Assets/Scripts/ClickCounter.luau"}]}}}},
            {"method": "entity.create", "params": {"name": "Play", "parent": {"$ref": "0.entity.id"}, "components": {
                "UiRect": button_rect, "UiImage": {}, "UiButton": {}}}},
        ]})
        client.call("play.start", {"lockstep": True})
        press = {"type": "mouseButton", "button": "Left", "state": "tap", "position": [20, 20]}
        client.call("play.step", {"ticks": 3, "render": "none", "input": [press]})
        clicks = client.call("script.eval", {"context": "play", "entity": "/Hud",
                                             "code": "return {self.Clicks, self.Last}"})
        self.assertEqual(clicks["value"], [1, "Play"])
        played = client.call("ui.tree", {"format": "Json", "target": "play"})
        button = next(element for element in played["elements"] if element["name"] == "Play")
        self.assertEqual(button["widget"], "Button")
        self.assertTrue(button["hovered"])
        self.assertTrue(button["focused"])
        self.assertFalse(button["pressed"])
        client.call("play.stop")

    def test_ui_tree_reports_the_interaction_in_the_runtime(self) -> None:
        # The exported game's Runtime fills ui.tree's interaction members from its own play session: a button the held
        # pointer pressed is its widget, interactable, hovered, focused and pressed; the release leaves it hovered and
        # focused.
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        create_ui(client)
        play_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [60, 5], "Size": [40, 20]}
        client.call("entity.create", {"name": "Play", "parent": "/Hud", "components": {
            "UiRect": play_rect, "UiImage": {}, "UiButton": {}}})
        client.call("scene.save")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.editors[-1].wait()

        arguments = ["--headless", "--automation", "--paused", *GPU_EDITOR_ARGUMENTS]
        game = engine_client.launch_editor(executable, arguments, self.directory / "Interacted", TINY_GAME_NAME)
        self.editors.append(game)
        runtime = self.connect(game)
        # The canvas's scale is 2: the button covers scene pixels (120, 10) to (200, 50).
        runtime.call("input.inject", {"events": [
            {"type": "mouseButton", "button": "Left", "state": "down", "position": [150, 30]}]})
        runtime.call("play.step", {"ticks": 1, "render": "none"})
        held = next(element for element in runtime.call("ui.tree", {"format": "Json"})["elements"]
                    if element["name"] == "Play")
        self.assertEqual(held["widget"], "Button")
        self.assertTrue(held["interactable"])
        self.assertTrue(held["hovered"])
        self.assertTrue(held["focused"])
        self.assertTrue(held["pressed"])
        runtime.call("input.inject", {"events": [{"type": "mouseButton", "button": "Left", "state": "up"}]})
        runtime.call("play.step", {"ticks": 1, "render": "none"})
        released = next(element for element in runtime.call("ui.tree", {"format": "Json"})["elements"]
                        if element["name"] == "Play")
        self.assertTrue(released["hovered"])
        self.assertTrue(released["focused"])
        self.assertFalse(released["pressed"])
        runtime.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())


if __name__ == "__main__":
    unittest.main()
