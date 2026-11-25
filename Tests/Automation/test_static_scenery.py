"""Static scenery through automation (Docs/Decisions/0061-m43-static-scenery.md decision 3; Docs/Architecture.md
§5.2, §8.3, §13.7): a scenery root made Static through entity.update makes its subtree's meshes the static set. A
lockstep session's tick in which nothing moves recomputes no world matrix and visits no entity (stats.get's
transformsUpdated and entitiesVisited), and the frame after it rebuilds no static cell (staticCellRebuilds), as for the
same scenery without Static; a write to a static entity of the play scene recomputes its matrices and rebuilds its one
cell at the next extracted frame. The script readout (Application.GetStats) counts the static items in every mode; a
rendering editor's Scene view reports them, its cells and its dynamic items (staticItems, staticCells,
staticCellsVisible, dynamicItems). The two sessions agree on every position; their state hashes differ by the
component.

The Scene view renders at the editor's frames, so the rendering test waits, bounded by FRAME_WAIT_SECONDS, for the frame
after a change to reach stats.get; no passing outcome depends on how long that takes.
"""

from __future__ import annotations

import time
import unittest
from typing import Any, Callable

from harness import AutomationTestCase, engine_client

FRAME_WAIT_SECONDS = 30.0
PAIRS = 12
SEED = 4300
# The rocks lie in four static cells of 32 m: x in {-6, -2} or {2, 6} and z in {-4} or {0, 4}.
CELLS = 4


def scenery_ops() -> list[dict[str, Any]]:
    """edit.batch ops of a camera, a shadowed sun and a scenery root over PAIRS built-in-mesh pairs (a rock and its mesh
    child each)."""
    ops: list[dict[str, Any]] = [
        {"method": "entity.create", "params": {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 4, 14], "EulerAngles": [-15, 0, 0]}, "Camera": {"Primary": True}}}},
        {"method": "entity.create", "params": {"name": "Sun", "components": {
            "Transform": {"EulerAngles": [-50, 30, 0]}, "DirectionalLight": {"Intensity": 2, "CastShadows": True}}}},
        {"method": "entity.create", "params": {"name": "Island"}},
    ]
    for index in range(PAIRS):
        ops.append({"method": "entity.create", "params": {"name": f"Rock{index}", "parent": "/Island", "components": {
            "Transform": {"Translation": [-6 + (index % 4) * 4, 0, -4 + (index // 4) * 4]}}}})
        ops.append({"method": "entity.create", "params": {
            "name": "Shape", "parent": f"/Island/Rock{index}", "components": {
                "Transform": {"Translation": [0, 0.5, 0], "EulerAngles": [0, index * 20, 0]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}}})
    return ops


# The session's world positions of the rocks' shapes and its readout's static items.
POSITIONS_CODE = (
    "local out = {}\n"
    f"for index = 0, {PAIRS - 1} do\n"
    "  local shape = Scene.FindByPath(string.format('/Island/Rock%d/Shape', index))\n"
    "  local p = shape.Transform.WorldPosition\n"
    "  table.insert(out, string.format('%.4f %.4f %.4f', p.x, p.y, p.z))\n"
    "end\n"
    "return { Positions = table.concat(out, ';'), StaticItems = Application.GetStats().StaticItems }")


def scene_view(client: engine_client.EngineClient) -> dict[str, Any]:
    """stats.get's entry of the view named "scene"."""
    return next(view for view in client.call("stats.get")["views"] if view["name"] == "scene")


class StaticSceneryTests(AutomationTestCase):
    """The counters in a headless editor without a renderer (render extraction runs for the game view at every frame the
    session extracts), and the Scene view's rows in a rendering one."""

    def run_session(self, client: engine_client.EngineClient) -> dict[str, Any]:
        """A lockstep session of the open scene: 30 ticks, the last extracted, then one more that is; stats.get, the
        positions and the readout after it, then a write to the first rock of the play scene and one more tick."""
        client.call("play.start", {"lockstep": True, "seed": SEED})
        try:
            client.call("play.step", {"ticks": 30, "render": "last"})
            client.call("play.step", {"ticks": 1, "render": "last"})
            steady = client.call("stats.get")
            read = client.call("script.eval", {"context": "play", "code": POSITIONS_CODE})["value"]
            state = client.call("play.state")
            client.call("entity.update", {"entity": "/Island/Rock0", "target": "play",
                                          "components": {"Transform": {"Translation": [-6, 1, -4]}}})
            client.call("play.step", {"ticks": 1, "render": "last"})
            moved = client.call("stats.get")
        finally:
            client.call("play.stop", {})
        return {"steady": steady, "moved": moved, "positions": read["Positions"], "staticItems": read["StaticItems"],
                "hash": state["stateHash"]}

    def test_a_static_root_makes_its_meshes_static_and_an_unchanged_tick_costs_nothing(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("edit.batch", {"label": "Scenery", "ops": scenery_ops()})
        dynamic = self.run_session(client)
        self.assertEqual(dynamic["staticItems"], 0)
        self.assertEqual(dynamic["steady"]["staticCellRebuilds"], 0)
        self.assertEqual(dynamic["moved"]["staticCellRebuilds"], 0)

        client.call("entity.update", {"entity": "/Island", "components": {"Static": {"Scope": "Subtree"}}})
        self.assertEqual(client.call("entity.get", {"entity": "/Island"})["entity"]["components"]["Static"]["Scope"],
                         "Subtree")
        static = self.run_session(client)
        self.assertEqual(static["staticItems"], PAIRS)
        # Nothing moves: no matrix, no visit, no cell rebuilt, with or without Static.
        for session in (dynamic, static):
            self.assertEqual(session["steady"]["transformsUpdated"], 0)
            self.assertEqual(session["steady"]["entitiesVisited"], 0)
        self.assertEqual(static["steady"]["staticCellRebuilds"], 0)
        # The rock and its shape moved: their two matrices, and the one cell that holds the shape rebuilt.
        self.assertEqual(static["moved"]["transformsUpdated"], 2)
        self.assertEqual(dynamic["moved"]["transformsUpdated"], 2)
        self.assertEqual(static["moved"]["staticCellRebuilds"], 1)
        self.assertEqual(static["positions"], dynamic["positions"])
        self.assertNotEqual(static["hash"], dynamic["hash"])  # the Static component is scene data

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

    def test_the_scene_view_reports_the_static_set_it_draws(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("edit.batch", {"label": "Scenery", "ops": scenery_ops()})
        client.call("viewport.camera", {"position": [0, 6, 18], "target": [0, 0, 0]})
        dynamic = self.wait_for_scene_view(client, lambda view: view["dynamicItems"] == PAIRS, "the dynamic items")
        self.assertEqual(dynamic["staticItems"], 0)
        self.assertEqual(dynamic["staticCells"], 0)

        client.call("entity.update", {"entity": "/Island", "components": {"Static": {}}})
        static = self.wait_for_scene_view(client, lambda view: view["staticItems"] == PAIRS, "the static items")
        self.assertEqual(static["dynamicItems"], 0)
        self.assertEqual(static["staticCells"], CELLS)
        self.assertEqual(static["staticCellsVisible"], CELLS)

        # Looking away, the view culls every cell whole.
        client.call("viewport.camera", {"position": [0, 6, 18], "target": [0, 6, 40]})
        away = self.wait_for_scene_view(client, lambda view: view["staticCellsVisible"] == 0, "every cell culled")
        self.assertEqual(away["staticCells"], CELLS)
        self.assertEqual(away["staticItems"], PAIRS)


if __name__ == "__main__":
    unittest.main()
