"""Water through automation (Docs/Decisions/0065-m47-water.md decision 3; Docs/Architecture.md §9.12, §13.5
"water.sample"): water.sample reads the surface of the water that holds each point, or a named water's own surface, in
the edit scene at time 0 or a given time and in the play scene at the world's time, and refuses bad points and entities.
"""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class WaterTests(AutomationTestCase):
    """water.sample against a headless editor."""

    def create_waters(self, client: engine_client.EngineClient) -> None:
        """A sea at y = 0 with one swell, a pool at y = 3 over 4 x 4 m around (10, 3, 0) and a rock without water."""
        client.call("entity.create", {"name": "Sea", "components": {
            "Water": {"Waves": [{"Wavelength": 8, "Amplitude": 0.5, "Steepness": 0.5}]}}})
        client.call("entity.create", {"name": "Pool", "components": {
            "Transform": {"Translation": [10, 3, 0]},
            "Water": {"Shape": "Rectangle", "Size": [4, 4], "Depth": 2, "Headroom": 2, "Current": [1, 0]}}})
        client.call("entity.create", {"name": "Rock", "components": {"Transform": {"Translation": [0, 5, 0]}}})

    def test_sample_reads_the_water_that_holds_each_point(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_waters(client)
        sampled = client.call("water.sample", {"points": [[10, 3.5, 0], [10, 2, 1], [40, -10, 0]]})
        self.assertEqual(sampled["time"], 0)
        dry, wet, sea = sampled["points"]
        # In the pool's volume above its surface: the pool, dry; under it: wet, with the current.
        self.assertEqual(dry["water"]["path"], "/Pool")
        self.assertAlmostEqual(dry["depth"], -0.5, places=5)
        self.assertFalse(dry["underwater"])
        self.assertEqual(wet["water"]["path"], "/Pool")
        self.assertTrue(wet["underwater"])
        self.assertAlmostEqual(wet["velocity"][0], 1.0, places=5)
        # Far out, 10 m down: the sea.
        self.assertEqual(sea["water"]["path"], "/Sea")
        self.assertTrue(sea["underwater"])
        self.assertGreater(sea["depth"], 9)

        own = client.call("water.sample", {"entity": "/Pool", "points": [[40, 0, 0]]})
        self.assertIsNone(own["points"][0]["water"])

        with self.assertRaises(engine_client.EngineError) as no_water:
            client.call("water.sample", {"entity": "/Rock", "points": [[0, 0, 0]]})
        self.assert_engine_error(no_water.exception, engine_client.INVALID_PARAMS)
        with self.assertRaises(engine_client.EngineError) as no_points:
            client.call("water.sample", {"points": []})
        self.assert_engine_error(no_points.exception, engine_client.INVALID_PARAMS)

    def test_sample_takes_the_world_time_in_play(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_waters(client)
        point = [1.5, 0, 2]
        at_zero = client.call("water.sample", {"points": [point]})["points"][0]["height"]
        later = client.call("water.sample", {"points": [point], "time": 1.5})
        self.assertAlmostEqual(later["time"], 1.5, places=5)
        at_later = later["points"][0]["height"]
        self.assertNotAlmostEqual(at_zero, at_later, places=3)

        client.call("play.start", {"lockstep": True, "seed": 2})
        client.call("play.step", {"ticks": 90, "render": "none"})
        playing = client.call("water.sample", {"points": [point]})
        self.assertAlmostEqual(playing["time"], 1.5, places=4)
        self.assertAlmostEqual(playing["points"][0]["height"], at_later, places=4)
        edit = client.call("water.sample", {"points": [point], "target": "Edit"})
        self.assertAlmostEqual(edit["points"][0]["height"], at_zero, places=6)
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
