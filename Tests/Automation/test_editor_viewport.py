"""Headless editor camera and viewport preferences through the real host."""

from __future__ import annotations

import math
import unittest

from harness import AutomationTestCase, engine_client


class EditorViewportTests(AutomationTestCase):
    """Viewport session changes preserve scene content, history and revision."""

    def test_viewport_camera_get_set_and_invalid_pose(self) -> None:
        client, _ = self.open_editor_with_scene()
        initial = client.call("viewport.camera")
        self.assertEqual(initial["position"], [0, 3, 10])
        self.assertEqual(initial["target"], [0, 0, 0])
        moved = client.call("viewport.camera", {"position": [4, 5, 9]})
        self.assertEqual(moved["position"], [4, 5, 9])
        self.assertEqual(moved["target"], initial["target"])
        aimed = client.call("viewport.camera", {"target": [1, 2, 3]})
        self.assertEqual(aimed["position"], moved["position"])
        self.assertEqual(aimed["target"], [1, 2, 3])
        for target in ([4, 5, 9], [4, 8, 9]):
            with self.assertRaises(engine_client.EngineError) as refused:
                client.call("viewport.camera", {"target": target})
            self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
            unchanged = client.call("viewport.camera")
            self.assertEqual(unchanged["position"], aimed["position"])
            self.assertEqual(unchanged["target"], aimed["target"])

    def test_viewport_frame_fits_entities(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Left", "components": {"Transform": {"Translation": [-8, -2, 1]}}})
        client.call("entity.create", {"name": "Right", "components": {"Transform": {"Translation": [12, 6, 3]}}})
        before = client.call("viewport.camera")
        framed = client.call("viewport.frame", {"entities": ["/Left", "/Right"]})
        self.assertEqual(framed["target"], [2, 2, 2])
        self.assertEqual({entity["name"] for entity in framed["entities"]}, {"Left", "Right"})
        distance = math.dist(framed["position"], framed["target"])
        self.assertGreater(distance, 1)
        forward = [(b - a) / distance for a, b in zip(framed["position"], framed["target"], strict=True)]
        horizontal_length = math.hypot(forward[0], forward[2])
        right = [-forward[2] / horizontal_length, 0, forward[0] / horizontal_length]
        up = [right[1] * forward[2] - right[2] * forward[1],
              right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0]]
        for point in ([-8, -2, 1], [12, 6, 3]):
            offset = [a - b for a, b in zip(point, framed["position"], strict=True)]
            depth = sum(a * b for a, b in zip(offset, forward, strict=True))
            self.assertGreater(depth, 0)
            x = abs(sum(a * b for a, b in zip(offset, right, strict=True)))
            y = abs(sum(a * b for a, b in zip(offset, up, strict=True)))
            self.assertLessEqual(x, depth * math.tan(math.radians(30)) * 16 / 9 / 1.1 + 1e-4)
            self.assertLessEqual(y, depth * math.tan(math.radians(30)) / 1.1 + 1e-4)
        self.assertNotEqual(framed["position"], before["position"])
        for entities in ([], ["/Left", "/Missing"]):
            with self.assertRaises(engine_client.EngineError):
                client.call("viewport.frame", {"entities": entities})
            unchanged = client.call("viewport.camera")
            self.assertEqual(unchanged["position"], framed["position"])
            self.assertEqual(unchanged["target"], framed["target"])

    def test_viewport_set_options_preserves_unspecified_members(self) -> None:
        client, _ = self.open_editor_with_scene()
        expected = {"grid": True, "gizmos": True, "colliders": False, "icons": True, "wireframe": False}
        defaults = client.call("viewport.setOptions")
        for key, value in expected.items():
            self.assertEqual(defaults[key], value)
        for key in expected:
            expected[key] = not expected[key]
            changed = client.call("viewport.setOptions", {key: expected[key]})
            for member, value in expected.items():
                self.assertEqual(changed[member], value)
        with self.assertRaises(engine_client.EngineError):
            client.call("viewport.setOptions", {"grid": "invalid"})
        unchanged = client.call("viewport.setOptions")
        for key, value in expected.items():
            self.assertEqual(unchanged[key], value)

    def test_viewport_controls_without_renderer(self) -> None:
        client, _ = self.open_editor_with_scene(renderer="none")
        client.call("entity.create", {"name": "Focus"})
        client.call("scene.save")
        before = client.call("session.info")["_meta"]
        history = client.call("edit.history")["entries"]
        client.call("viewport.camera", {"position": [5, 4, 8], "target": [1, 0, 0]})
        client.call("viewport.frame", {"entities": ["/Focus"]})
        client.call("viewport.setOptions", {"wireframe": True, "grid": False})
        after = client.call("session.info")["_meta"]
        self.assertEqual(after["revision"], before["revision"])
        self.assertFalse(after["dirty"])
        self.assertEqual(client.call("edit.history")["entries"], history)


if __name__ == "__main__":
    unittest.main()
