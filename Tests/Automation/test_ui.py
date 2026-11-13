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


# Records the text input's callbacks (C22.3, decision 47) as "<callback>:<element>:<text>".
FORM_LOG = """local FormLog = {}
function FormLog.OnCreate(self: any)
\tself.Log = {}
end
function FormLog.OnUiTextChanged(self: any, element: Entity, text: string)
\ttable.insert(self.Log, "changed:" .. element.Name .. ":" .. text)
end
function FormLog.OnUiSubmit(self: any, element: Entity, text: string)
\ttable.insert(self.Log, "submit:" .. element.Name .. ":" .. text)
end
return Script.Define("FormLog", FormLog)
"""


class UiTreeTests(AutomationTestCase):
    """ui.tree in the editor and the Runtime."""

    def test_ui_tree_lists_the_edit_and_play_scene_ui_in_scene_pixels(self) -> None:
        client, _ = self.open_editor_with_scene()
        create_ui(client)
        listed = client.call("ui.tree", {"format": "Json", "width": 200, "height": 100})
        self.assertEqual((listed["width"], listed["height"]), (200, 100))
        names = [element["name"] for element in listed["elements"]]
        self.assertEqual(names, ["Hud", "Panel", "Label"])
        # The canvas's scale turns scene pixels into the canvas units UiRect takes; an element reports its canvas's
        # (M25's friction F11).
        self.assertEqual([element["scale"] for element in listed["elements"]], [2, 2, 2])
        panel = listed["elements"][1]
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

    def test_ui_tree_reports_each_text_box_and_whether_it_overflows(self) -> None:
        # M25's friction F02 (Docs/Decisions/0036-m25-tetris-pixel-art.md): each text's laid-out box in scene
        # pixels, so an agent checks that every text lies inside its rectangle; the outline marks a text that
        # overflows.
        client, _ = self.open_editor_with_scene()
        fits_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 10], "Size": [100, 20]}
        wide_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 40], "Size": [20, 20]}
        client.call("edit.batch", {"label": "Texts", "ops": [
            {"method": "entity.create", "params": {"name": "Hud", "components": {"UiCanvas": {}}}},
            {"method": "entity.create", "params": {"name": "Fits", "parent": {"$ref": "0.entity.id"}, "components": {
                "UiRect": fits_rect, "UiText": {"Text": "Score", "Size": 10, "Alignment": "Right", "Wrap": False}}}},
            {"method": "entity.create", "params": {"name": "Wide", "parent": {"$ref": "0.entity.id"}, "components": {
                "UiRect": wide_rect, "UiText": {"Text": "A label far wider than its box", "Size": 10, "Wrap": False}}}},
        ]})
        listed = client.call("ui.tree", {"format": "Json", "width": 200, "height": 100})
        elements = {element["name"]: element for element in listed["elements"]}
        fits = elements["Fits"]
        bounds = fits["textBounds"]
        self.assertFalse(fits["textOverflows"])
        self.assertGreater(bounds["width"], 0)
        self.assertAlmostEqual(bounds["x"] + bounds["width"], 110, places=3)
        self.assertGreaterEqual(bounds["x"], 10)
        self.assertEqual(bounds["y"], 10)
        wide = elements["Wide"]
        self.assertTrue(wide["textOverflows"])
        self.assertEqual(wide["textBounds"]["x"], 10)
        self.assertGreater(wide["textBounds"]["width"], 20)
        self.assertEqual(elements["Hud"]["textBounds"], {"x": 0, "y": 0, "width": 0, "height": 0})
        outline = client.call("ui.tree", {"width": 200, "height": 100})["text"]
        wide_line = next(line for line in outline.splitlines() if "Wide" in line)
        self.assertTrue(wide_line.endswith("overflows"), wide_line)
        fits_line = next(line for line in outline.splitlines() if "Fits" in line)
        self.assertNotIn("overflows", fits_line)

    def test_ui_tree_lists_inactive_elements_on_request(self) -> None:
        # M25's friction F10 (Docs/Decisions/0036-m25-tetris-pixel-art.md): includeInactive lists an inactive panel
        # and its text, laid out and measured as if active and marked inactive, so an agent checks a hidden game-over
        # panel's texts in the edit scene; the scene is only read.
        client, _ = self.open_editor_with_scene()
        panel_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 20], "Size": [80, 40]}
        title_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [0, 4], "Size": [80, 16]}
        client.call("edit.batch", {"label": "Panel", "ops": [
            {"method": "entity.create", "params": {"name": "Hud", "components": {"UiCanvas": {}}}},
            {"method": "entity.create", "params": {"name": "GameOver", "parent": {"$ref": "0.entity.id"},
                                                   "active": False,
                                                   "components": {"UiRect": panel_rect, "UiImage": {}}}},
            {"method": "entity.create", "params": {"name": "Title", "parent": {"$ref": "1.entity.id"}, "components": {
                "UiRect": title_rect,
                "UiText": {"Text": "GAME OVER", "Size": 10, "Alignment": "Center", "Wrap": False}}}},
        ]})
        listed = client.call("ui.tree", {"format": "Json", "width": 200, "height": 100})
        self.assertEqual([element["name"] for element in listed["elements"]], ["Hud"])
        revision = listed["scene"]["revision"]
        everything = client.call("ui.tree", {"format": "Json", "width": 200, "height": 100, "includeInactive": True})
        elements = {element["name"]: element for element in everything["elements"]}
        self.assertEqual(list(elements), ["Hud", "GameOver", "Title"])
        self.assertTrue(elements["Hud"]["active"])
        self.assertFalse(elements["GameOver"]["active"])
        title = elements["Title"]
        self.assertFalse(title["active"])
        self.assertEqual(title["rect"], {"x": 10, "y": 24, "width": 80, "height": 16})
        self.assertGreater(title["textBounds"]["width"], 0)
        self.assertFalse(title["textOverflows"])
        self.assertEqual(everything["scene"]["revision"], revision)
        outline = client.call("ui.tree", {"width": 200, "height": 100, "includeInactive": True})["text"]
        title_line = next(line for line in outline.splitlines() if "Title" in line)
        self.assertTrue(title_line.endswith("(10,24 80x16)  inactive"), title_line)
        self.assertFalse(client.call("entity.get", {"entity": "/Hud/GameOver"})["entity"]["active"])

    def test_ui_tree_works_in_the_runtime(self) -> None:
        # The exported Runtime serves ui.tree from its own asset manager and scene (the Dist Runtime holds no
        # automation, so the Release export is the one that serves it). M25's milestone review: its text measurement
        # (textBounds, textOverflows: the font loaded from the pak), the element scale, active and includeInactive
        # (a serializer copy of the running scene) answer as the editor's do.
        if not self.require_gpu():
            return
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        create_ui(client)
        notice_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [10, 30],
                       "Size": [60, 10]}
        client.call("edit.batch", {"label": "Notice", "ops": [
            {"method": "entity.create", "params": {"name": "Notice", "parent": "/Hud", "active": False, "components": {
                "UiRect": notice_rect, "UiText": {"Text": "Paused", "Size": 5, "Wrap": False}}}},
        ]})
        client.call("scene.save")
        size = {"format": "Json", "width": 200, "height": 100}
        edited = client.call("ui.tree", size)["elements"]
        edited_everything = client.call("ui.tree", {**size, "includeInactive": True})["elements"]
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
        self.assertEqual([element["scale"] for element in listed["elements"]], [2, 2, 2])
        self.assertTrue(all(element["active"] for element in listed["elements"]))
        label = listed["elements"][2]["textBounds"]
        self.assertGreater(label["width"], 0)
        self.assertGreater(label["height"], 0)

        # At the editor's resolution, every entry measures as the editor's did, the hidden one included.
        everything = runtime.call("ui.tree", {**size, "includeInactive": True})["elements"]
        self.assertEqual([element["name"] for element in everything], ["Hud", "Panel", "Label", "Notice"])
        notice = everything[3]
        self.assertFalse(notice["active"])
        self.assertGreater(notice["textBounds"]["width"], 0)
        for expected_list, answered_list in ((edited, runtime.call("ui.tree", size)["elements"]),
                                             (edited_everything, everything)):
            self.assertEqual(len(answered_list), len(expected_list))
            for expected, answered in zip(expected_list, answered_list):
                with self.subTest(element=expected["name"], inactive=len(expected_list) > 3):
                    for key in ("name", "active", "scale", "rect", "textOverflows"):
                        self.assertEqual(answered[key], expected[key], key)
                    for edge in ("x", "y", "width", "height"):
                        self.assertAlmostEqual(answered["textBounds"][edge], expected["textBounds"][edge], places=3)
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

    def test_typed_text_reaches_on_ui_text_changed_and_ui_tree_reports_it(self) -> None:
        # M22 C22.3: a press focuses a text field; Text events injected through play.step are latched by the
        # TextLatch phase and typed into it, OnUiTextChanged and OnUiSubmit (Enter) bubble to the canvas's behaviour
        # with the field's text, and ui.tree reports the field as a TextInput with its text.
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/FormLog.luau", "source": FORM_LOG})
        client.call("edit.batch", {"label": "Ui", "ops": [
            {"method": "entity.create", "params": {"name": "Form", "components": {
                "UiCanvas": {}, "Script": {"Scripts": [{"Script": "Assets/Scripts/FormLog.luau"}]}}}},
            {"method": "ui.createWidget", "params": {"kind": "TextInput", "parent": {"$ref": "0.entity.id"},
                                                     "name": "Name", "text": "Your name"}},
        ]})
        field = next(element for element in client.call("ui.tree", {"format": "Json"})["elements"]
                     if element["name"] == "Name")
        point = [field["rect"]["x"] + 4, field["rect"]["y"] + 4]
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 2, "render": "none", "input": [
            {"type": "mouseButton", "button": "Left", "state": "tap", "position": point}]})
        client.call("play.step", {"ticks": 2, "render": "none", "input": [{"type": "text", "text": "Ada"}]})
        client.call("play.step", {"ticks": 2, "render": "none", "input": [
            {"type": "key", "key": "Enter", "state": "tap"}]})
        log = client.call("script.eval", {"context": "play", "entity": "/Form", "code": "return self.Log"})
        self.assertEqual(log["value"], ["changed:Name:Ada", "submit:Name:Ada"])
        played = client.call("ui.tree", {"format": "Json", "target": "play"})
        name = next(element for element in played["elements"] if element["name"] == "Name")
        self.assertEqual(name["widget"], "TextInput")
        self.assertEqual(name["text"], "Ada")
        self.assertTrue(name["focused"])
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


class UiWorldCanvasTests(AutomationTestCase):
    """World canvases through automation (Docs/Decisions/0033-m22-contract.md decisions 49 to 53)."""

    def test_a_click_through_the_camera_reaches_a_world_canvas_button_and_ui_tree_reports_its_pixels(self) -> None:
        # M22 C22.3: a World canvas of 200 x 100 canvas pixels at 20 per world unit faces a primary camera 10 units in
        # front of it with a 90 degree field of view. ui.tree reports the canvas and its button in the canvas's own
        # pixels, whatever the scene resolution, and a press and release injected through play.step at the image's
        # centre reach the button along the camera's ray (the canvas's centre), so OnUiClick bubbles to the canvas's
        # behaviour.
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/ClickCounter.luau", "source": CLICK_COUNTER})
        button_rect = {"AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [80, 30],
                       "Size": [40, 40]}
        board = {"RenderMode": "World", "WorldWidth": 200, "WorldHeight": 100, "PixelsPerUnit": 20}
        client.call("edit.batch", {"label": "World UI", "ops": [
            {"method": "entity.create", "params": {"name": "Camera", "components": {
                "Transform": {"Translation": [0, 0, 10]}, "Camera": {"VerticalFov": 90, "Primary": True}}}},
            {"method": "entity.create", "params": {"name": "Board", "components": {
                "UiCanvas": board, "Script": {"Scripts": [{"Script": "Assets/Scripts/ClickCounter.luau"}]}}}},
            {"method": "entity.create", "params": {"name": "Play", "parent": {"$ref": "1.entity.id"}, "components": {
                "UiRect": button_rect, "UiImage": {}, "UiButton": {}}}},
        ]})
        listed = client.call("ui.tree", {"format": "Json", "width": 640, "height": 360})
        canvas, button = listed["elements"]
        self.assertTrue(canvas["worldCanvas"])
        self.assertEqual(canvas["rect"], {"x": 0, "y": 0, "width": 200, "height": 100})
        self.assertTrue(button["worldCanvas"])
        self.assertEqual(button["rect"], {"x": 80, "y": 30, "width": 40, "height": 40})
        self.assertIn("world 200x100", client.call("ui.tree")["text"])

        window = client.call("project.getSettings")["settings"]["Window"]
        centre = [window["Width"] / 2, window["Height"] / 2]
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 3, "render": "none", "input": [
            {"type": "mouseButton", "button": "Left", "state": "tap", "position": centre}]})
        clicks = client.call("script.eval", {"context": "play", "entity": "/Board",
                                             "code": "return {self.Clicks, self.Last}"})
        self.assertEqual(clicks["value"], [1, "Play"])
        played = client.call("ui.tree", {"format": "Json", "target": "play"})
        played_button = next(element for element in played["elements"] if element["name"] == "Play")
        self.assertTrue(played_button["worldCanvas"])
        self.assertTrue(played_button["hovered"])
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
