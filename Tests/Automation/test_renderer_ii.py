"""M9 visual queries, displayed-view statistics and capture-local annotation acceptance."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, AutomationTestCase, engine_client
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game, exported_executable


class RendererIITests(AutomationTestCase):
    """Exercise the public wire protocol in the editor and an exported Runtime."""

    def make_scene(self, client: engine_client.EngineClient) -> None:
        client.call("entity.create", {"name": "Cube", "components": {
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}, "BoxCollider": {}}})
        client.call("entity.create", {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 0, 5]}, "Camera": {"Primary": True}}})
        client.call("viewport.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})

    def shoot(self, client: engine_client.EngineClient, **options: Any) -> bytes:
        result = client.call("viewport.screenshot", {
            "view": "scene", "width": 320, "height": 180, **options})
        return Path(result["path"]).read_bytes()

    def test_stats_report_pass_timings(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.make_scene(client)
        before = client.call("scene.tree")["scene"]
        stats = client.call("stats.get")
        self.assertEqual(stats["entities"], 2)
        self.assertEqual(stats["bodies"], 0)
        self.assertEqual(stats["voices"], 0)
        self.assertFalse(stats["scriptAvailable"])
        self.assertEqual(stats["memoryAllocationCount"], 0)
        self.assertEqual([view["name"] for view in stats["views"]], ["scene", "game"])
        self.assertTrue(all(not view["gpuAvailable"] for view in stats["views"]))
        self.assertEqual(client.call("scene.tree")["scene"], before)
        if not self.require_gpu():
            return
        rendered, _ = self.open_editor_with_scene(name="RenderedProject", renderer="vulkan")
        self.make_scene(rendered)
        deadline = time.monotonic() + 60
        while True:
            observed = rendered.call("stats.get")
            ready = [view for view in observed["views"] if view["gpuAvailable"]]
            if ready or time.monotonic() >= deadline:
                break
        self.assertTrue(ready, observed)
        for view in ready:
            self.assertTrue(view["frame"].isdigit())
            self.assertTrue(view["gpuFrame"].isdigit())
            self.assertLessEqual(int(view["gpuFrame"]), int(view["frame"]))
            self.assertTrue(view["passes"])
            self.assertTrue(any(entry["gpuAvailable"] for entry in view["passes"]))
            for entry in view["passes"]:
                self.assertGreaterEqual(entry["cpuMilliseconds"], 0)
                self.assertGreaterEqual(entry["gpuMilliseconds"], 0)
        self.shoot(rendered, width=73, height=51)
        following = rendered.call("stats.get")
        self.assertEqual([view["name"] for view in following["views"]], ["scene", "game"])
        self.assertTrue(all((view["width"], view["height"]) != (73, 51)
                            for view in following["views"]))

    def test_viewport_pick_deterministic(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.make_scene(client)
        selected = client.call("edit.getSelection")["selection"]
        for view in ("scene", "game"):
            params = {"view": view, "x": 320, "y": 180}
            first = client.call("viewport.pick", params)
            self.assertEqual(first["raycast"]["entity"]["name"], "Cube")
            for _ in range(3):
                self.assertEqual(client.call("viewport.pick", params)["raycast"], first["raycast"])
        client.call("play.start", {"lockstep": True, "paused": True})
        client.call("entity.update", {"entity": "/Cube", "target": "play",
                                      "components": {"Transform": {"Translation": [0, 0, -2]}}})
        moved = client.call("viewport.pick", {"view": "game", "x": 320, "y": 180})
        original = client.call("viewport.pick", {"view": "game", "x": 320, "y": 180, "target": "edit"})
        self.assertGreater(moved["raycast"]["distance"], original["raycast"]["distance"] + 1.9)
        client.call("play.stop")
        self.assertEqual(client.call("edit.getSelection")["selection"], selected)

    def test_scene_raycast_exported_runtime(self) -> None:
        editor = self.connect(self.start_editor())
        self.create_project(editor, TINY_GAME_NAME)
        build_tiny_game(editor)
        executable = exported_executable(export_tiny_game(editor))
        editor.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS)
        game = engine_client.launch_editor(executable,
                                          ["--headless", "--automation", "--paused", "--renderer", "none"],
                                          self.directory / "VisualRuntime", TINY_GAME_NAME)
        self.editors.append(game)
        client = self.connect(game)
        ray = {"origin": [0, 0, 5], "direction": [0, 0, -3], "maxDistance": 10}
        hit = client.call("scene.raycast", ray)
        self.assertTrue(hit["hit"])
        self.assertEqual(hit["entity"]["name"], "Cube")
        self.assertAlmostEqual(hit["distance"], 4.5)
        self.assertFalse(client.call("scene.raycast", {**ray, "layerMask": 0})["hit"])
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("scene.raycast", {**ray, "direction": [0, 0, 0]})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(raised.exception.issues[0]["pointer"], "/direction")
        stats = client.call("stats.get")
        self.assertEqual(stats["entities"], 3)
        self.assertEqual([view["name"] for view in stats["views"]], ["game"])
        self.assertFalse(stats["views"][0]["gpuAvailable"])
        # Explicit Runtime label IDs resolve before a renderer-none capture reports Unsupported.
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("viewport.screenshot", {"view": "game", "annotate": {"labels": [hit["entity"]["id"]]}})
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED)
        client.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())

    def test_screenshot_annotations_are_capture_local(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.make_scene(client)
        clean = self.shoot(client)
        before = client.call("scene.tree")["scene"]
        annotated = self.shoot(client, annotate={"labels": "all", "bounds": True,
                                                "axes": True, "colliders": True})
        self.assertNotEqual(annotated, clean)
        self.assertEqual(self.shoot(client), clean)
        self.assertEqual(client.call("scene.tree")["scene"], before)
        for options, pointer in (({"axes": 1}, "/annotate/axes"),
                                 ({"unexpected": True}, "/annotate/unexpected")):
            with self.assertRaises(engine_client.EngineError) as raised:
                self.shoot(client, annotate=options)
            self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
            self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        self.assertEqual(self.shoot(client), clean)

    def test_viewport_pick_camera_clip_interval(self) -> None:
        client, _ = self.open_editor_with_scene()
        # Width 640/height 360, x=455 gives positive off-axis x; derive the ray analytically for a 90 degree FOV.
        x, y = 455, 179
        dx = (2 * (x + 0.5) / 640 - 1) * 640 / 360
        dy = 1 - 2 * (y + 0.5) / 360
        client.call("entity.create", {"name": "Camera", "components": {
            "Camera": {"Primary": True, "VerticalFov": 90, "NearClip": 1, "FarClip": 10}}})
        for name, depth in (("Near", 0.5), ("Far", 9.5)):
            client.call("entity.create", {"name": name, "components": {
                "Transform": {"Translation": [dx * depth, dy * depth, -depth], "Scale": [0.3, 0.3, 0.1]},
                "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})
        pixel = {"view": "game", "x": x, "y": y}
        hit = client.call("viewport.pick", pixel)["raycast"]
        self.assertEqual(hit["entity"]["name"], "Far")
        self.assertGreater(hit["distance"], 10)
        client.call("entity.update", {"entity": "/Camera", "components": {
            "Camera": {"Projection": "Orthographic", "OrthographicSize": 9.5}}})
        hit = client.call("viewport.pick", pixel)["raycast"]
        self.assertEqual(hit["entity"]["name"], "Far")
        self.assertAlmostEqual(hit["distance"], 8.45, places=4)

    def test_annotation_label_selection_and_explicit_ids(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.make_scene(client)
        selection = client.call("edit.select", {"entities": ["/Cube"]})["selection"]
        clean = self.shoot(client)
        selected = self.shoot(client, annotate={"labels": "selection"})
        self.assertNotEqual(selected, clean)
        self.assertEqual(self.shoot(client, annotate={"labels": "selected"}), selected)
        self.assertEqual(self.shoot(client, annotate={"labels": ["/Cube"]}), selected)
        self.assertEqual(self.shoot(client, annotate={"labels": [selection[0]["id"]]}), selected)
        self.assertEqual(self.shoot(client, annotate={"labels": [selection[0]["id"][:6]]}), selected)
        self.assertEqual(self.shoot(client, annotate={"labels": []}), clean)
        with self.assertRaises(engine_client.EngineError) as raised:
            self.shoot(client, annotate={"labels": ["/Cube", "/Missing"]})
        self.assert_engine_error(raised.exception, engine_client.NOT_FOUND)
        self.assertEqual(raised.exception.issues[0]["pointer"], "/annotate/labels/1")
        self.assertEqual(client.call("edit.getSelection")["selection"], selection)

    def test_render_validation_warnings(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.make_scene(client)
        client.call("entity.create", {"name": "Environment", "components": {"Environment": {"Intensity": 0}}})
        warnings = client.call("project.validate", {"scope": "scene"})["diagnostics"]
        dark = [item for item in warnings if item["code"] == "RENDER_NO_LIGHTING"]
        self.assertEqual(len(dark), 1)
        self.assertEqual(dark[0]["severity"], "Warning")
        self.assertFalse(dark[0]["autoFixable"])
        for index in range(9):
            client.call("entity.create", {"name": f"Spot{index}", "components": {"SpotLight": {"CastShadows": True}}})
        client.call("scene.save")
        report = client.call("project.validate", {"scope": "project"})["diagnostics"]
        budget = [item for item in report if item["code"] == "RENDER_SPOT_SHADOW_BUDGET"]
        self.assertEqual(len(budget), 1)
        self.assertFalse(any(item["code"] == "RENDER_NO_LIGHTING" for item in report))
        repeated = client.call("project.validate", {"scope": "project"})["diagnostics"]
        self.assertEqual([item["id"] for item in repeated if item["code"] == "RENDER_SPOT_SHADOW_BUDGET"],
                         [budget[0]["id"]])

    def test_renderer_debug_outputs_and_wireframe(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.make_scene(client)
        with self.assertRaises(engine_client.EngineError) as raised:
            self.shoot(client, debugView="AO")
        self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED)
        with self.assertRaises(engine_client.EngineError) as raised:
            self.shoot(client, debugView="not-a-view")
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
        self.assertEqual(raised.exception.issues[0]["pointer"], "/debugView")
        if not self.require_gpu():
            return
        rendered, _ = self.open_editor_with_scene(name="RenderedProject", renderer="vulkan")
        self.make_scene(rendered)
        rendered.call("entity.create", {"name": "Sun", "components": {"DirectionalLight": {"CastShadows": True}}})
        lit = self.shoot(rendered)
        for mode in ("AO", "ShadowCascades", "Overdraw"):
            image = self.shoot(rendered, debugView=mode)
            self.assertNotEqual(image, lit)
            self.assertEqual(self.shoot(rendered, debugView=mode), image)
        before = rendered.call("viewport.pick", {"view": "scene", "x": 320, "y": 180})["raycast"]
        rendered.call("viewport.setOptions", {"wireframe": True})
        self.assertEqual(rendered.call("viewport.pick", {"view": "scene", "x": 320, "y": 180})["raycast"], before)
        rendered.call("viewport.setOptions", {"wireframe": False})
        self.assertEqual(self.shoot(rendered), lit)
