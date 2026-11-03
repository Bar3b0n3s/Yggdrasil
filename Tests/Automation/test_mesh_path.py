"""M23's mesh path as automation observes it (Docs/Decisions/0034-m23-contract.md decisions 2, 3, 5, 17 and 18):
instanced draws, the light clusters, point-light shadows and the LightCount view, through stats.get and
viewport.screenshot in a rendering editor and in an exported Runtime.

Every wait here is a bounded observation wait (the displayed views report a frame after the edit that changed them),
never a timing gate.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, engine_client
from test_environment import png_pixels
from tiny_game import TINY_GAME_NAME, TINY_SCENE, export_tiny_game, exported_executable

# The built-in cube's triangles (six faces of two each).
CUBE_TRIANGLES = 12
# The camera both displayed views look through: the scene view's (viewport.camera) and the primary Camera entity's.
CAMERA_POSITION = [0, 5, 9]
CAMERA_PITCH = -30
OBSERVATION_SECONDS = 60.0
# The LightCount view's palette (0 lights black, 1 to 2 blue, 3 to 4 cyan, 5 to 8 green, 9 to 16 yellow, 17 to 32
# orange, more red), each channel within one step.
LIGHT_COUNT_PALETTE = [(0, 0, 0), (0, 0, 255), (0, 255, 255), (0, 255, 0), (255, 255, 0), (255, 128, 0), (255, 0, 0)]


def grid_ops(count: int, mesh: str, prefix: str, y: float) -> list[dict[str, Any]]:
    """entity.create ops for a 4-wide grid of `count` entities drawing `mesh` with its default material."""
    return [{"method": "entity.create", "params": {"name": f"{prefix}{index}", "components": {
        "Transform": {"Translation": [-2.25 + 1.5 * (index % 4), y, -2.25 + 1.5 * (index // 4)],
                      "Scale": [0.8, 0.8, 0.8]},
        "MeshRenderer": {"Mesh": mesh}}}} for index in range(count)]


def camera_ops() -> list[dict[str, Any]]:
    """The primary camera at CAMERA_POSITION, looking down at the grid."""
    return [{"method": "entity.create", "params": {"name": "Camera", "components": {
        "Transform": {"Translation": CAMERA_POSITION, "EulerAngles": [CAMERA_PITCH, 0, 0]},
        "Camera": {"Primary": True, "Clear": "Color"}}}}]


def passes_of(view: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """A stats.get view's pass rows by name."""
    return {entry["name"]: entry for entry in view["passes"]}


def palette_index(pixels: bytes, width: int, x: int, y: int) -> int | None:
    """The LightCount palette entry of a pixel, or None for a colour outside the palette."""
    offset = (y * width + x) * 4
    colour = pixels[offset:offset + 3]
    for index, entry in enumerate(LIGHT_COUNT_PALETTE):
        if all(abs(colour[channel] - entry[channel]) <= 1 for channel in range(3)):
            return index
    return None


class MeshPathTests(AutomationTestCase):
    """Rendering editors and an exported Runtime; each test needs a Vulkan device."""

    def wait_for_views(self, client: engine_client.EngineClient,
                       ready: Callable[[dict[str, Any]], bool]) -> list[dict[str, Any]]:
        """The displayed views of stats.get once at least one available view satisfies `ready` (bounded by
        OBSERVATION_SECONDS; fails with the last observation otherwise)."""
        deadline = time.monotonic() + OBSERVATION_SECONDS
        while True:
            stats = client.call("stats.get")
            views = [view for view in stats["views"] if view["available"] and ready(view)]
            if views:
                return views
            if time.monotonic() >= deadline:
                self.fail(f"no displayed view reached the expected state within {OBSERVATION_SECONDS:.0f} s: {stats}")

    def open_grid_editor(self) -> engine_client.EngineClient:
        """A rendering editor on a new scene with the camera and a sun; the scene view looks through the camera."""
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("edit.batch", {"label": "Mesh path camera", "ops": camera_ops() + [
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-50, 30, 0]}, "DirectionalLight": {"Intensity": 2}}}}]})
        client.call("viewport.camera", {"position": CAMERA_POSITION, "target": [0, 0, 0]})
        return client

    def test_meshes_of_one_mesh_and_material_draw_as_one_instanced_draw(self) -> None:
        # R-INST (decision 2): the opaque passes record one draw per run of equal (pipeline, material, mesh, submesh)
        # keys, so 16 cubes are one ForwardOpaque and one DepthNormal draw call of all their triangles, a mesh's own
        # MeshRenderer flags (in the instance record) never split the run, and a second mesh adds one draw.
        if not self.require_gpu():
            return
        client = self.open_grid_editor()
        client.call("edit.batch", {"label": "Cubes", "ops": grid_ops(16, "engine://Meshes/Cube", "Cube", 0.4)})
        cubes = 16 * CUBE_TRIANGLES

        def draws(view: dict[str, Any], pass_name: str) -> tuple[int, int]:
            entry = passes_of(view).get(pass_name, {"drawCalls": -1, "triangles": -1})
            return entry["drawCalls"], entry["triangles"]

        for view in self.wait_for_views(client, lambda view: draws(view, "ForwardOpaque") == (1, cubes)):
            self.assertEqual(draws(view, "DepthNormal"), (1, cubes), view)

        client.call("entity.update", {"entity": "/Cube5", "components": {
            "MeshRenderer": {"ReceiveShadows": False, "CastShadows": False}}})
        client.call("entity.update", {"entity": "/Cube10", "components": {"Transform": {"Scale": [0.5, 1.6, 0.5]}}})
        # A frame that started after the edits (later than every frame recorded when they were done) still draws the
        # cubes as one draw.
        edited = max((int(view["frame"]) for view in client.call("stats.get")["views"] if view["available"]),
                     default=-1)
        self.wait_for_views(client, lambda view: int(view["frame"]) > edited
                            and draws(view, "ForwardOpaque") == (1, cubes))

        client.call("edit.batch", {"label": "Spheres", "ops": grid_ops(4, "engine://Meshes/Sphere", "Sphere", 1.4)})
        for view in self.wait_for_views(client, lambda view: draws(view, "ForwardOpaque")[0] == 2):
            forward = draws(view, "ForwardOpaque")
            self.assertGreater(forward[1], cubes, view)
            self.assertEqual(draws(view, "DepthNormal"), forward, view)
        client.call("scene.save")

    def test_light_clusters_pass_runs_only_while_local_lights_are_visible(self) -> None:
        # R-CLC (decisions 5 and 20): with directional lights alone every cluster would hold the whole list, so the
        # LightClusters pass is not recorded; a point light makes the frame cull its lights per cluster with one
        # dispatch, and removing it again drops the row.
        if not self.require_gpu():
            return
        client = self.open_grid_editor()
        client.call("edit.batch", {"label": "Cubes", "ops": grid_ops(8, "engine://Meshes/Cube", "Cube", 0.4)})
        for view in self.wait_for_views(client, lambda view: "ForwardOpaque" in passes_of(view)):
            self.assertNotIn("LightClusters", passes_of(view), view)

        client.call("entity.create", {"name": "Bulb", "components": {
            "Transform": {"Translation": [0, 1.5, 0]}, "PointLight": {"Range": 4}}})
        for view in self.wait_for_views(client, lambda view: "LightClusters" in passes_of(view)):
            clusters = passes_of(view)["LightClusters"]
            self.assertGreater(clusters["dispatches"], 0, view)
            self.assertEqual(clusters["drawCalls"], 0, view)
            self.assertEqual((view["shadowedPointLights"], view["droppedPointShadows"]), (0, 0), view)

        client.call("entity.destroy", {"entities": ["/Bulb"]})
        self.wait_for_views(client, lambda view: "ForwardOpaque" in passes_of(view)
                            and "LightClusters" not in passes_of(view))
        client.call("scene.save")

    def test_exported_runtime_renders_point_shadows_clusters_and_the_light_count_view(self) -> None:
        # The exported Runtime (the editor's configuration) renders M23's path as the editor does: four shadow-casting
        # point lights, the per-view budget, all shadowed with a PointShadows pass; the clusters cull the eight
        # short-range point lights; the cube grid is one instanced draw; and viewport.screenshot's LightCount view shows
        # the clusters' counts in the palette, deterministically, unlike the lit image.
        if not self.require_gpu():
            return
        editor = self.start_editor()
        client = self.connect(editor)
        self.create_project(client, TINY_GAME_NAME)
        client.call("scene.new", {"path": TINY_SCENE})
        # Two rows of four lights 3 m apart, each reaching 1.5 m: no surface is within reach of more than a few.
        lights = [{"method": "entity.create", "params": {"name": f"Lamp{index}", "components": {
            "Transform": {"Translation": [-4.5 + 3 * (index % 4), 1, -2 + 4 * (index // 4)]},
            "PointLight": {"Intensity": 4, "Range": 1.5, "CastShadows": index < 4}}}} for index in range(8)]
        client.call("edit.batch", {"label": "Mesh path game", "ops": camera_ops()
                                   + grid_ops(16, "engine://Meshes/Cube", "Cube", 0.4) + lights})
        client.call("project.setSettings", {"patch": {
            "StartScene": TINY_SCENE, "Window": {"Title": TINY_GAME_NAME, "Width": 320, "Height": 180},
            "Export": {"BuildScenes": [TINY_SCENE]}}})
        client.call("scene.save")
        executable = exported_executable(export_tiny_game(client))
        client.call("session.shutdown")
        self.assertEqual(editor.wait(), EXIT_SUCCESS, editor.output())

        game = engine_client.launch_editor(
            executable, ["--headless", "--automation", "--paused", "--expect-no-errors", *GPU_EDITOR_ARGUMENTS],
            self.directory / "Runtime", TINY_GAME_NAME)
        self.editors.append(game)
        runtime = self.connect(game)
        runtime.call("play.step", {"ticks": 1, "render": "last"})

        def rendered(view: dict[str, Any]) -> bool:
            passes = passes_of(view)
            return view["gpuAvailable"] and "PointShadows" in passes and "LightClusters" in passes

        view = self.wait_for_views(runtime, rendered)[0]
        self.assertEqual(view["name"], "game")
        self.assertEqual((view["shadowedPointLights"], view["droppedPointShadows"]), (4, 0), view)
        passes = passes_of(view)
        self.assertGreater(passes["PointShadows"]["drawCalls"], 0, view)
        self.assertGreater(passes["LightClusters"]["dispatches"], 0, view)
        self.assertEqual((passes["ForwardOpaque"]["drawCalls"], passes["ForwardOpaque"]["triangles"]),
                         (1, 16 * CUBE_TRIANGLES), view)

        # 640 x 360 pixels: ten by six 64-pixel tiles.
        size = {"view": "game", "width": 640, "height": 360}
        counted = runtime.call("viewport.screenshot", {**size, "debugView": "LightCount"})
        again = runtime.call("viewport.screenshot", {**size, "debugView": "lightcount"})
        lit = runtime.call("viewport.screenshot", size)
        self.assertEqual(Path(again["path"]).read_bytes(), Path(counted["path"]).read_bytes())
        self.assertNotEqual(Path(lit["path"]).read_bytes(), Path(counted["path"]).read_bytes())
        width, height, pixels = png_pixels(Path(counted["path"]))
        self.assertEqual((width, height), (640, 360))
        bands = {palette_index(pixels, width, x, y) for y in range(0, height, 4) for x in range(0, width, 4)}
        self.assertNotIn(None, bands)
        # The background is black; the clusters that hold lights hold no more than the scene's eight (green, 5 to 8, is
        # the highest band), and some hold at most four of them (blue or cyan): the clusters cull.
        self.assertIn(0, bands)
        self.assertTrue(bands - {0}, bands)
        self.assertLessEqual(max(bands), 3, bands)
        self.assertLessEqual(min(bands - {0}), 2, bands)
        runtime.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())
