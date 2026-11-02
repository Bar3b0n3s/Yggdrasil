"""The editor's minimal UI authoring through automation (Docs/Architecture.md §13.5; Docs/Decisions/0033-m22-contract.md
decisions 32 to 34): ui.createWidget, which the editor's Create UI menu submits, and the scene viewport's UI preview,
which viewport.setOptions turns on; the rect tool and the anchor presets submit entity.update."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class UiAuthoringTests(AutomationTestCase):
    """ui.createWidget and the UI preview option."""

    @unittest.skip("contract stub: Docs/Decisions/0033-m22-contract.md decision 32 (stream D)")
    def test_create_widget_builds_each_kind_below_a_canvas_in_one_undo_step(self) -> None:
        client, _ = self.open_editor_with_scene()
        # Without a parent, a button goes below a new root canvas, created in the same undo step.
        button = client.call("ui.createWidget", {"kind": "Button", "text": "Play"})
        self.assertEqual(len(button["entities"]), 3)
        tree = client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})
        self.assertEqual([element["name"] for element in tree["elements"]], ["Canvas", "Button", "Label"])
        self.assertEqual(tree["elements"][1]["widget"], "Button")
        self.assertEqual(tree["elements"][2]["text"], "Play")
        for kind, count in (("Toggle", 3), ("Slider", 3), ("ScrollView", 2), ("Panel", 1), ("Image", 1), ("Text", 1)):
            created = client.call("ui.createWidget", {"kind": kind, "parent": "/Canvas"})
            self.assertEqual(len(created["entities"]), count, kind)
        widgets = {element["name"]: element["widget"] for element in
                   client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"]}
        self.assertEqual(widgets["Toggle"], "Toggle")
        self.assertEqual(widgets["Slider"], "Slider")
        self.assertEqual(widgets["Scroll View"], "ScrollView")

        # Seven commands: one undo removes the text, the last widget; five more leave only the first command's canvas,
        # button and label, and the sixth removes all three, so the new canvas took no undo step of its own.
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        names = [element["name"] for element in client.call("scene.query", {"where": {"name": "Text"}})["entities"]]
        self.assertEqual(names, [])
        for _ in range(5):
            self.assertEqual(client.call("edit.undo")["undone"], 1)
        remaining = client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"]
        self.assertEqual([element["name"] for element in remaining], ["Canvas", "Button", "Label"])
        self.assertEqual(client.call("edit.undo")["undone"], 1)
        self.assertEqual(client.call("ui.tree", {"format": "Json", "width": 320, "height": 180})["elements"], [])

    @unittest.skip("contract stub: Docs/Decisions/0033-m22-contract.md decision 33 (stream D)")
    def test_the_ui_preview_option_round_trips_through_viewport_set_options(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.assertFalse(client.call("viewport.setOptions", {})["uiPreview"])
        self.assertTrue(client.call("viewport.setOptions", {"uiPreview": True})["uiPreview"])
        # Other members leave it as it is.
        self.assertTrue(client.call("viewport.setOptions", {"grid": False})["uiPreview"])
        self.assertFalse(client.call("viewport.setOptions", {"uiPreview": False})["uiPreview"])


if __name__ == "__main__":
    unittest.main()
