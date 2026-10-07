"""entity.bounds through automation: world AABBs (Docs/Architecture.md §13.5, Roadmap M6)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class EntityBoundsTests(AutomationTestCase):
    def test_entity_bounds_world_aabb(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Track", "components": {"Transform": {"Translation": [10, 0, 0]}}})
        client.call("entity.create", {"name": "Piece", "parent": "/Track", "components": {
            "Transform": {"Translation": [0, 1, 0], "Scale": [2, 1, 4], "EulerAngles": [0, 90, 0]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        result = client.call("entity.bounds", {"entities": ["/Track/Piece", "/Track"]})
        piece, track = result["bounds"]
        self.assertTrue(piece["hasBounds"])
        # A unit cube scaled 2 x 1 x 4 and turned 90 degrees about Y: 4 wide in X, 2 deep in Z, centred at (10, 1, 0).
        for actual, expected in zip(piece["min"], [8.0, 0.5, -1.0]):
            self.assertAlmostEqual(actual, expected, places=4)
        for actual, expected in zip(piece["max"], [12.0, 1.5, 1.0]):
            self.assertAlmostEqual(actual, expected, places=4)
        self.assertEqual(track["entity"]["path"], "/Track")
        self.assertEqual(track["min"], piece["min"])
        own = client.call("entity.bounds", {"entities": ["/Track"], "includeDescendants": False})["bounds"][0]
        self.assertFalse(own["hasBounds"])
        self.assertEqual(own["min"], [])
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("entity.bounds", {"entities": ["/Missing"]})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")


if __name__ == "__main__":
    unittest.main()
