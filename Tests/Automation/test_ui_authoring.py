"""The editor's minimal UI authoring through automation (Docs/Architecture.md §13.5; Docs/Decisions/0033-m22-contract.md
decisions 32 to 34): ui.createWidget, which the editor's Create UI menu submits, and the scene viewport's UI preview,
which viewport.setOptions turns on; the rect tool and the anchor presets submit entity.update."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class UiAuthoringTests(AutomationTestCase):
    """ui.createWidget and the UI preview option."""

    def test_create_widget_builds_each_kind_below_a_canvas_in_one_undo_step(self) -> None:
        client, _ = self.open_editor_with_scene()
        # Without a parent, a button goes below a new root canvas, created in the same undo step.
        button = client.call("ui.createWidget", {"kind": "Button", "text": "Play"})
        self.assertEqual(len(button["entities"]), 3)
        tree = client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})
        self.assertEqual([element["name"] for element in tree["elements"]], ["Canvas", "Button", "Label"])
        self.assertEqual(tree["elements"][1]["widget"], "Button")
        self.assertEqual(tree["elements"][2]["text"], "Play")
        created_by_kind: dict[str, list[str]] = {}
        kinds = (("Toggle", 3), ("Slider", 3), ("ScrollView", 2), ("TextInput", 3), ("Panel", 1), ("Image", 1),
                 ("Text", 1))
        for kind, count in kinds:
            created = client.call("ui.createWidget", {"kind": kind, "parent": "/Canvas"})
            self.assertEqual(len(created["entities"]), count, kind)
            created_by_kind[kind] = created["entities"]
        widgets = {element["name"]: element["widget"] for element in
                   client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"]}
        self.assertEqual(widgets["Toggle"], "Toggle")
        self.assertEqual(widgets["Slider"], "Slider")
        self.assertEqual(widgets["Scroll View"], "ScrollView")
        self.assertEqual(widgets["Text Input"], "TextInput")

        # Eight commands: one undo removes the text, the last widget (checked by its id, since the text input's child
        # is named "Text" too); six more leave only the first command's canvas, button and label, and the seventh
        # removes all three, so the new canvas took no undo step of its own.
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        ids = {element["id"] for element in
               client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"]}
        self.assertNotIn(created_by_kind["Text"][0], ids)
        self.assertTrue(set(created_by_kind["TextInput"]) <= ids)
        for _ in range(6):
            self.assertEqual(client.call("edit.undo")["undone"], 1)
        remaining = client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"]
        self.assertEqual([element["name"] for element in remaining], ["Canvas", "Button", "Label"])
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        self.assertEqual(client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"], [])

    def test_create_widget_builds_its_template_below_a_canvas_and_refuses_what_its_kind_cannot_take(self) -> None:
        client, _ = self.open_editor_with_scene()
        # A dry run reports the entities it would create and creates none.
        dry = client.call("ui.createWidget", {"kind": "Button", "dryRun": True})
        self.assertEqual(len(dry["entities"]), 3)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Canvas"}})["total"], 0)

        # Without a parent, a button goes below a new root canvas: the canvas first, then the button and its label.
        button = client.call("ui.createWidget", {"kind": "Button", "name": "Play", "text": "Play"})
        self.assertEqual(button["entity"]["path"], "/Canvas/Play")
        self.assertEqual(len(button["entities"]), 3)
        self.assertEqual(button["entities"][1], button["entity"]["id"])
        canvas = client.call("entity.get", {"entity": "/Canvas", "components": ["UiCanvas"]})
        self.assertEqual(canvas["entity"]["id"], button["entities"][0])
        label = client.call("entity.get", {"entity": "/Canvas/Play/Label", "components": ["UiText"]})
        self.assertEqual(label["entity"]["components"]["UiText"]["Text"], "Play")

        # A slider names its parts, and a scroll view its content.
        slider = client.call("ui.createWidget", {"kind": "Slider", "parent": "/Canvas"})
        parts = client.call("entity.get", {"entity": "/Canvas/Slider", "components": ["UiSlider"]})
        self.assertEqual(parts["entity"]["components"]["UiSlider"]["Fill"], slider["entities"][1])
        self.assertEqual(parts["entity"]["components"]["UiSlider"]["Handle"], slider["entities"][2])

        # Refusals: a label on a kind without one, an empty name, an unknown parent and an index past the roots.
        for params, code, pointer in (({"kind": "Image", "text": "Logo"}, engine_client.INVALID_PARAMS, "/text"),
                                      ({"kind": "Text", "name": ""}, engine_client.INVALID_PARAMS, "/name"),
                                      ({"kind": "Text", "parent": "/Nowhere"}, engine_client.NOT_FOUND, None),
                                      ({"kind": "Canvas", "index": 99}, engine_client.INVALID_PARAMS, "/index")):
            with self.assertRaises(engine_client.EngineError, msg=str(params)) as refused:
                client.call("ui.createWidget", params)
            self.assert_engine_error(refused.exception, code)
            if pointer is not None:
                self.assertEqual(refused.exception.issues[0]["pointer"], pointer)

        # One undo takes the slider back; another the button with the canvas created for it.
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Slider"}})["total"], 0)
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Canvas"}})["total"], 0)

    def test_the_ui_preview_option_round_trips_through_viewport_set_options(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.assertFalse(client.call("viewport.setOptions", {})["uiPreview"])
        self.assertTrue(client.call("viewport.setOptions", {"uiPreview": True})["uiPreview"])
        # Other members leave it as it is.
        self.assertTrue(client.call("viewport.setOptions", {"grid": False})["uiPreview"])
        self.assertFalse(client.call("viewport.setOptions", {"uiPreview": False})["uiPreview"])


if __name__ == "__main__":
    unittest.main()
