"""2D physics on Jolt through automation (Docs/Decisions/0032-m21-contract.md decisions 8 to 13, 15 and 27; Roadmap
M21's 2D physics, R-2D-PHYS)."""

from __future__ import annotations

import time
import unittest
from pathlib import Path

from harness import AutomationTestCase
from test_environment import png_pixels

RECORDER = """local T = {}
function T:OnCollisionEnter2D(other, contact) Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":" .. other.Name) end
return Script.Define("Recorder", T)
"""


class Physics2DTests(AutomationTestCase):
    def test_physics2d_queries_report_the_2d_colliders(self) -> None:
        # Decision 15: physics2d.raycast and physics2d.overlap read the play session's 2D world.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Wall", "components": {
            "Transform": {"Translation": [3, 0, 0]},
            "RigidBody2D": {"Type": "Static"}, "BoxCollider2D": {"HalfExtents": [0.5, 2]}}})
        client.call("play.start", {"lockstep": True, "mode": "simulate"})
        ray = client.call("physics2d.raycast", {"origin": [0, 1], "direction": [1, 0], "maxDistance": 10})
        self.assertEqual(len(ray["hits"]), 1)
        self.assertEqual(ray["hits"][0]["entity"]["name"], "Wall")
        self.assertAlmostEqual(ray["hits"][0]["distance"], 2.5, places=3)
        overlap = client.call("physics2d.overlap", {"shape": "Box", "center": [3, 0], "halfExtents": [0.1, 0.1]})
        self.assertEqual([entity["name"] for entity in overlap["entities"]], ["Wall"])
        client.call("play.stop")

    def test_2d_contacts_reach_scripts_and_a_body_stays_in_its_plane(self) -> None:
        # Decisions 9, 10 and 13 (Roadmap M21 "2D contact events reach scripts in canonical order", "a 2D body never
        # leaves its plane"): a falling ball enters the floor once, both sides are called, the lower UUID first (each
        # call creates its trace entity, which scene.query lists in canonical order), and the ball's Z never changes.
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Scripts/Recorder.luau", "source": RECORDER})
        script = {"Script": {"Scripts": [{"Script": "Assets/Scripts/Recorder.luau"}]}}
        client.call("entity.create", {"name": "Floor", "components": {
            "Transform": {"Translation": [0, -0.5, 0]},
            "RigidBody2D": {"Type": "Static"}, "BoxCollider2D": {"HalfExtents": [5, 0.5]}, **script}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0, 2, 0.25]},
            "RigidBody2D": {}, "CircleCollider2D": {"Radius": 0.25}, **script}})
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 90})
        traces = client.call("scene.query", {"where": {"name": "Trace:*"}, "target": "play"})["entities"]
        ids = {name: client.call("entity.get", {"entity": f"/{name}", "target": "play"})["entity"]["id"]
               for name in ("Floor", "Ball")}
        first, second = sorted(("Floor", "Ball"), key=lambda name: ids[name])
        self.assertEqual([entity["name"] for entity in traces], [f"Trace:{first}:{second}", f"Trace:{second}:{first}"])
        ball = client.call("entity.get", {"entity": "/Ball", "target": "play"})["entity"]["components"]
        self.assertEqual(ball["Transform"]["Translation"][2], 0.25)
        client.call("play.stop")

    def test_an_entity_in_both_worlds_is_a_validation_error(self) -> None:
        # Decision 12: PHYSICS2D_MIXED_WORLDS.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Mixed", "components": {
            "RigidBody": {}, "SphereCollider": {}, "RigidBody2D": {}, "CircleCollider2D": {}}})
        report = client.call("project.validate", {"scope": "Scene"})
        codes = [diagnostic["code"] for diagnostic in report["diagnostics"]]
        self.assertIn("PHYSICS2D_MIXED_WORLDS", codes)

    def test_physics2d_body_info_reports_a_2d_body(self) -> None:
        # C21.2, decision 27: physics.bodyInfo in the plane; a gathered child reports its owner's body, and the resting
        # crate's contact with the floor is listed.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Floor", "components": {
            "Transform": {"Translation": [0, -0.5, 0]},
            "RigidBody2D": {"Type": "Static"}, "BoxCollider2D": {"HalfExtents": [5, 0.5]}}})
        client.call("entity.create", {"name": "Crate", "components": {
            "Transform": {"Translation": [1, 0.5, 0]}, "RigidBody2D": {}, "BoxCollider2D": {}}})
        client.call("entity.create", {"name": "Lid", "parent": "/Crate", "components": {
            "Transform": {"Translation": [0, 0.6, 0]}, "BoxCollider2D": {"HalfExtents": [0.5, 0.1]}}})
        client.call("play.start", {"lockstep": True, "mode": "simulate"})
        client.call("play.step", {"ticks": 30})
        info = client.call("physics2d.bodyInfo", {"entity": "/Crate/Lid"})
        self.assertEqual(info["body"]["name"], "Crate")
        self.assertEqual(info["type"], "Dynamic")
        self.assertEqual([collider["name"] for collider in info["colliders"]], ["Crate", "Lid"])
        self.assertEqual(len(info["position"]), 2)
        self.assertAlmostEqual(info["position"][0], 1.0, places=2)
        self.assertEqual([contact["other"]["name"] for contact in info["contacts"]], ["Floor"])
        self.assertEqual(info["tick"], 30)
        client.call("play.stop")

    def test_the_2d_collider_overlay_draws_in_screenshots_and_the_scene_view(self) -> None:
        # C21.2, decision 28 over the wire (M21's milestone review): viewport.screenshot {annotate: {colliders: true}}
        # draws a BoxCollider2D's rectangle and a painted TilemapCollider's merged boxes in the plane, capture-local and
        # the same every time, and the scene view's Colliders option draws them in the view's own frames.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Crate", "components": {
            "Transform": {"Translation": [0, 1, 0]},
            "RigidBody2D": {"Type": "Static"}, "BoxCollider2D": {"HalfExtents": [2, 1]}}})
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Sheets/Tiles.spritesheet",
                                     "values": {"Frames": [{"Rect": {"Width": 16, "Height": 16}}]}})
        client.call("asset.create", {"type": "Tilemap", "path": "Assets/Tilemaps/Ground.tilemap", "values": {
            "ChunkSize": 4, "Palette": [{"Sheet": "Assets/Sheets/Tiles.spritesheet", "Solid": True}]}})
        client.call("tilemap.paint", {"tilemap": "Assets/Tilemaps/Ground.tilemap",
                                      "cells": [{"x": x, "y": -4, "tile": 1} for x in range(-3, 3)]})
        client.call("entity.create", {"name": "Ground", "components": {
            "TilemapCollider": {"Tilemap": "Assets/Tilemaps/Ground.tilemap"}}})
        client.call("viewport.setOptions", {"mode2D": True, "grid": False, "icons": False, "colliders": False})
        client.call("viewport.camera", {"position": [0, 0, 500], "target": [0, 0, 0], "orthographicSize": 5})

        size = {"view": "scene", "width": 320, "height": 180}
        annotated_params = {**size, "annotate": {"colliders": True}}
        clean = Path(client.call("viewport.screenshot", size)["path"])
        annotated = Path(client.call("viewport.screenshot", annotated_params)["path"])
        repeated = Path(client.call("viewport.screenshot", annotated_params)["path"])
        self.assertNotEqual(annotated.read_bytes(), clean.read_bytes())
        self.assertEqual(repeated.read_bytes(), annotated.read_bytes())

        # 18 pixels a unit (half height 5 over 180 pixels), the origin at the view's centre: every pixel the overlay
        # changed lies on the crate's rectangle [-2, 2] x [0, 2] or on one of the tilemap's two boxes, one a chunk,
        # [-3, 0] x [-4, -3] and [0, 3] x [-4, -3], within two pixels, and both colliders are drawn.
        width, height, before = png_pixels(clean)
        _, _, after = png_pixels(annotated)
        rectangles = {"Crate": [(-2.0, 0.0, 2.0, 2.0)], "Ground": [(-3.0, -4.0, 0.0, -3.0), (0.0, -4.0, 3.0, -3.0)]}
        tolerance = 2.0 / 18.0

        def on_outline(x: float, y: float, rectangle: tuple[float, float, float, float]) -> bool:
            left, bottom, right, top = rectangle
            inside_x = left - tolerance <= x <= right + tolerance
            inside_y = bottom - tolerance <= y <= top + tolerance
            near_vertical = min(abs(x - left), abs(x - right)) <= tolerance and inside_y
            near_horizontal = min(abs(y - bottom), abs(y - top)) <= tolerance and inside_x
            return near_vertical or near_horizontal

        drawn = {name: 0 for name in rectangles}
        for row in range(height):
            for column in range(width):
                offset = (row * width + column) * 4
                if before[offset:offset + 4] == after[offset:offset + 4]:
                    continue
                x = (column + 0.5 - width / 2) / 18.0
                y = (height / 2 - (row + 0.5)) / 18.0
                owners = [name for name, boxes in rectangles.items() if any(on_outline(x, y, box) for box in boxes)]
                self.assertTrue(owners,
                                f"pixel ({column}, {row}), world ({x:.2f}, {y:.2f}), is on no collider's outline")
                for name in owners:
                    drawn[name] += 1
        self.assertGreater(drawn["Crate"], 50)
        self.assertGreater(drawn["Ground"], 50)

        # The Colliders option: the scene view's overlays draw more once it is set.
        def overlay_draws() -> int:
            # The Overlays pass of a scene-view frame rendered after this call: a poll of the editor, its deadline only
            # bounding a failure.
            frame = client.call("stats.get")["views"][0]["frame"]
            after = int(frame) + 1 if frame else -1
            deadline = time.monotonic() + 60
            while True:
                view = client.call("stats.get")["views"][0]
                if view["available"] and int(view["frame"]) > after:
                    return sum(entry["drawCalls"] for entry in view["passes"] if entry["name"] == "Overlays")
                if time.monotonic() > deadline:
                    self.fail(f"the scene view rendered no frame after frame {after} within 60 s: {view}")

        without = overlay_draws()
        client.call("viewport.setOptions", {"colliders": True})
        with_colliders = overlay_draws()
        self.assertGreater(with_colliders, without)

    def test_stats_count_the_bodies_of_both_worlds(self) -> None:
        # M21's milestone review: stats.get's bodies (the Stats panel's and the hardening soak's count) counts the 2D
        # world's bodies with the 3D world's.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Cube", "components": {
            "Transform": {"Translation": [0, 0, 10]}, "RigidBody": {"Type": "Static"}, "BoxCollider": {}}})
        for index in range(3):
            client.call("entity.create", {"name": f"Crate{index}", "components": {
                "Transform": {"Translation": [3 * index, 0, 0]},
                "RigidBody2D": {"Type": "Static"}, "BoxCollider2D": {}}})
        self.assertEqual(client.call("stats.get")["bodies"], 0)
        client.call("play.start", {"lockstep": True, "mode": "simulate"})
        self.assertEqual(client.call("stats.get")["bodies"], 4)
        client.call("play.stop")
        self.assertEqual(client.call("stats.get")["bodies"], 0)

    def test_stats_count_the_awake_bodies_of_both_worlds_and_the_live_particles(self) -> None:
        # M28 (Docs/Decisions/0043-m28-game-services.md decision 4): stats.get's activeBodies (both worlds' awake
        # bodies) and particles (the session's live particles after its last Particles phase), the counters the readout
        # shares.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Ground", "components": {
            "Transform": {"Translation": [0, -2, 0]}, "RigidBody": {"Type": "Static"}, "BoxCollider": {}}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0, 5, 0]}, "RigidBody": {"Type": "Dynamic"}, "SphereCollider": {}}})
        client.call("entity.create", {"name": "Crate", "components": {
            "Transform": {"Translation": [10, 5, 0]}, "RigidBody2D": {"Type": "Dynamic"}, "BoxCollider2D": {}}})
        client.call("entity.create", {"name": "Sparks", "components": {
            "Transform": {}, "ParticleEmitter": {"Rate": 120}}})
        stats = client.call("stats.get", {"views": False})
        self.assertEqual((stats["activeBodies"], stats["particles"]), (0, 0))
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 30})
        stats = client.call("stats.get", {"views": False})
        # The two falling bodies are awake; the static ground never is.
        self.assertEqual(stats["bodies"], 3)
        self.assertEqual(stats["activeBodies"], 2)
        self.assertGreater(stats["particles"], 0)
        client.call("play.stop")
        stats = client.call("stats.get", {"views": False})
        self.assertEqual((stats["activeBodies"], stats["particles"]), (0, 0))


if __name__ == "__main__":
    unittest.main()
