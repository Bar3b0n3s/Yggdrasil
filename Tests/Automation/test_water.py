"""Water through automation (Docs/Decisions/0065-m47-water.md decision 3; Docs/Architecture.md §9.12, §13.5
"water.sample", §13.7): water.sample reads the surface of the water that holds each point, or a named water's own
surface, in the edit scene at time 0 or a given time and in the play scene at the world's time, and refuses bad points
(none, or beyond the world) and entities; and a rendering editor's Scene view draws the waters, whose surfaces, rows and
planar opt-in stats.get reports, and records the Underwater pass with its camera under the sea.

The Scene view renders at the editor's frames, so the rendering test waits, bounded by FRAME_WAIT_SECONDS, for the frame
after a change to reach stats.get; no passing outcome depends on how long that takes.
"""

from __future__ import annotations

import time
import unittest
from typing import Any, Callable

from harness import AutomationTestCase, engine_client

FRAME_WAIT_SECONDS = 30.0


def scene_view(client: engine_client.EngineClient) -> dict[str, Any]:
    """stats.get's entry of the view named "scene"."""
    return next(view for view in client.call("stats.get")["views"] if view["name"] == "scene")


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
        # A point beyond the physics' world (a coordinate past 1e9 m) is refused where it is, as the physics queries
        # refuse it.
        with self.assertRaises(engine_client.EngineError) as far:
            client.call("water.sample", {"points": [[0, 0, 0], [0, 0, 2e9]]})
        self.assert_engine_error(far.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(far.exception.issues[0]["pointer"], "/points/1")

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

    def wait_for_scene_view(self, client: engine_client.EngineClient, condition: Callable[[dict[str, Any]], bool],
                            what: str) -> dict[str, Any]:
        """The Scene view's stats once `condition` holds, failing after FRAME_WAIT_SECONDS."""
        deadline = time.monotonic() + FRAME_WAIT_SECONDS
        while True:
            view = scene_view(client)
            if view["available"] and condition(view):
                return view
            if time.monotonic() > deadline:
                self.fail(f"the Scene view did not report {what} within {FRAME_WAIT_SECONDS:.0f} s: {view}")

    def test_stats_report_the_drawn_waters_and_the_underwater_view(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.create_waters(client)
        # The pool's planar reflection, M42's opt-in on a water: its mirror is the pool's base plane.
        pool = client.call("entity.update", {"entity": "/Pool", "components": {"PlanarReflection": {}}})["entity"]["id"]
        client.call("entity.create", {"name": "Sun", "components": {
            "Transform": {"EulerAngles": [-40, 20, 0]}, "DirectionalLight": {"Intensity": 2}}})

        # Above both waters, looking at the pool: both surfaces drawn under their two rows, the pool's mirror in its
        # slot.
        client.call("viewport.camera", {"position": [4, 7, 10], "target": [10, 3, 0]})
        above = self.wait_for_scene_view(
            client, lambda view: view["waterSurfaces"] == 2 and view["planarReflections"] == 1,
            "the two waters and the pool's mirror drawn")
        self.assertFalse(above["underwater"])
        self.assertEqual(above["planarReflectionEntities"], [pool])
        rows = [entry["name"] for entry in above["passes"]]
        self.assertIn("WaterRefraction", rows)
        self.assertIn("Water", rows)
        self.assertIn("PlanarReflection0", rows)
        self.assertNotIn("Underwater", rows)

        # Under the sea, far from the pool: the Underwater pass records, and the pool's mirror, whose plane the camera
        # is under, is culled.
        client.call("viewport.camera", {"position": [-20, -4, 0], "target": [-30, -5, 0]})
        under = self.wait_for_scene_view(client, lambda view: view["underwater"], "the Underwater pass recorded")
        self.assertEqual(under["waterSurfaces"], 2)
        self.assertEqual(under["planarReflections"], 0)
        self.assertIn("Underwater", [entry["name"] for entry in under["passes"]])
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
