"""ui.tree through automation (Docs/Architecture.md §13.5; Docs/Decisions/0033-m22-contract.md decision 8): a scene's
UI laid out in scene pixels, in the editor's edit and play scenes and in an exported game's Runtime, which serves it as
part of its subset. Skipped by the M22 contract (C22.1); stream E un-skips them."""

from __future__ import annotations

import unittest

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game, exported_executable

SKIP_UI_TREE = "contract stub: ui.tree (Docs/Decisions/0033-m22-contract.md decision 8, stream E)"


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

    @unittest.skip(SKIP_UI_TREE)
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

    @unittest.skip(SKIP_UI_TREE)
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


if __name__ == "__main__":
    unittest.main()
