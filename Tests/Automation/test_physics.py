"""Physics through automation (Docs/Architecture.md §5.6 Simulate, §9, §13.5 "physics.bodyInfo", §13.7
PHYSICS_ADJACENT_STATIC_BODIES; Roadmap M11 acceptance): a Simulate session drops a ball, physics.bodyInfo reports its
velocity, sleeping state and contacts, and project.validate reports adjacent implicit static bodies and fixes them with a
Static RigidBody on their common parent. test_simulate_mode_ball_falls, test_body_info_reports_contacts and
test_validate_reports_adjacent_static_bodies_and_fix_adds_parent_body are the Roadmap acceptance tests;
test_body_info_needs_a_play_session_and_a_body covers physics.bodyInfo's errors.
"""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class PhysicsTests(AutomationTestCase):
    """Bodies, contacts and physics diagnostics of a headless editor."""

    def create_ground_and_ball(self, client: engine_client.EngineClient, height: float = 5.0) -> None:
        """A static ground box whose top face is at y = 0, and a dynamic ball of radius 0.5 above it."""
        client.call("entity.create", {"name": "Ground", "components": {
            "Transform": {"Translation": [0, -0.5, 0]}, "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": [50, 0.5, 50]}}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0, height, 0]}, "RigidBody": {"Type": "Dynamic"},
            "SphereCollider": {"Radius": 0.5}}})

    def ball_height(self, client: engine_client.EngineClient, target: str = "play") -> float:
        """The ball's world height in the `target` scene."""
        entity = client.call("entity.get", {"entity": "/Ball", "components": ["Transform"], "target": target})["entity"]
        return float(entity["components"]["Transform"]["Translation"][1])

    def test_simulate_mode_ball_falls(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ground_and_ball(client)
        started = client.call("play.start", {"mode": "simulate", "lockstep": True})
        self.assertEqual(started["state"], "Simulate")
        self.assertEqual(started["mode"], "Simulate")
        start_height = self.ball_height(client)
        self.assertAlmostEqual(start_height, 5.0, places=4)

        # Half a second of free fall: 0.5 * 9.81 * 0.5^2 ~= 1.23 m.
        client.call("play.step", {"ticks": 30, "render": "none"})
        falling = self.ball_height(client)
        self.assertLess(falling, start_height - 1.0)
        # Then it lands and rests on the ground (radius 0.5, minus the penetration slop).
        stepped = client.call("play.step", {"ticks": 150, "render": "none"})
        self.assertRegex(stepped["stateHash"], r"^[0-9a-f]{16}$")
        self.assertAlmostEqual(self.ball_height(client), 0.5, delta=0.05)

        # Simulate never touched the edit scene (§5.6), and stopping returns to it unchanged.
        self.assertAlmostEqual(self.ball_height(client, "edit"), 5.0, places=4)
        client.call("play.stop")
        self.assertEqual(client.call("play.state")["state"], "Edit")

    def test_body_info_reports_contacts(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ground_and_ball(client, height=1.0)
        client.call("play.start", {"lockstep": True, "seed": 5})

        # A fifth of a second in, the ball falls (about 2 m/s), awake and touching nothing.
        client.call("play.step", {"ticks": 12, "render": "none"})
        falling = client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assertLess(falling["linearVelocity"][1], -1.0)
        self.assertFalse(falling["sleeping"])
        self.assertEqual(falling["contacts"], [])

        client.call("play.step", {"ticks": 48, "render": "none"})

        info = client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assertEqual(info["entity"]["name"], "Ball")
        self.assertEqual(info["body"]["id"], info["entity"]["id"])
        self.assertEqual(info["type"], "Dynamic")
        self.assertEqual(info["origin"], "RigidBody")
        self.assertEqual(info["layer"], "Default")
        # At rest on the ground.
        for axis in range(3):
            self.assertLess(abs(info["linearVelocity"][axis]), 0.05)
            self.assertLess(abs(info["angularVelocity"][axis]), 0.05)
        self.assertEqual(len(info["contacts"]), 1)
        contact = info["contacts"][0]
        self.assertEqual(contact["other"]["name"], "Ground")
        self.assertFalse(contact["isTrigger"])
        self.assertLessEqual(contact["sinceTick"], 60)

        # Resting long enough, it falls asleep and keeps its contact (sleeping never ends a pair).
        client.call("play.step", {"ticks": 120, "render": "none"})
        asleep = client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assertTrue(asleep["sleeping"])
        self.assertEqual([pair["other"]["name"] for pair in asleep["contacts"]], ["Ground"])

        # The ground's side of the same pair.
        ground = client.call("physics.bodyInfo", {"entity": "/Ground"})
        self.assertEqual(ground["type"], "Static")
        self.assertEqual([pair["other"]["name"] for pair in ground["contacts"]], ["Ball"])

        # Errors: an entity without a body, and Edit mode.
        client.call("entity.create", {"name": "Empty", "target": "play"})
        with self.assertRaises(engine_client.EngineError) as no_body:
            client.call("physics.bodyInfo", {"entity": "/Empty"})
        self.assert_engine_error(no_body.exception, engine_client.NOT_FOUND, "NotFound")
        client.call("play.stop")
        with self.assertRaises(engine_client.EngineError) as not_playing:
            client.call("physics.bodyInfo", {"entity": "/Ball"})
        self.assert_engine_error(not_playing.exception, engine_client.INVALID_STATE, "InvalidState")

    def test_validate_reports_adjacent_static_bodies_and_fix_adds_parent_body(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Track"})
        for x in (0, 1):
            client.call("entity.create", {"name": "Piece", "parent": "/Track", "components": {
                "Transform": {"Translation": [x, 0, 0]}, "BoxCollider": {}}})

        report = client.call("project.validate", {"scope": "scene"})
        adjacent = [item for item in report["diagnostics"] if item["code"] == "PHYSICS_ADJACENT_STATIC_BODIES"]
        self.assertEqual(len(adjacent), 1, report["diagnostics"])
        self.assertEqual(adjacent[0]["severity"], "Warning")
        self.assertTrue(adjacent[0]["autoFixable"])

        fixed = client.call("project.validate", {"scope": "scene", "fix": [adjacent[0]["id"]]})
        self.assertEqual(fixed["fixed"], [adjacent[0]["id"]])
        self.assertNotIn("PHYSICS_ADJACENT_STATIC_BODIES", [item["code"] for item in fixed["diagnostics"]])
        track = client.call("entity.get", {"entity": "/Track", "components": ["RigidBody"]})["entity"]
        self.assertEqual(track["components"]["RigidBody"]["Type"], "Static")

        # One undo step takes the fix back, and the warning returns.
        client.call("edit.undo")
        track = client.call("entity.get", {"entity": "/Track", "components": "all"})["entity"]
        self.assertNotIn("RigidBody", track["components"])
        again = client.call("project.validate", {"scope": "scene"})
        self.assertIn("PHYSICS_ADJACENT_STATIC_BODIES", [item["code"] for item in again["diagnostics"]])

    def test_body_info_needs_a_play_session_and_a_body(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Empty"})

        # Bodies exist only in a play session; the hint names Simulate, which needs no scripts.
        with self.assertRaises(engine_client.EngineError) as not_playing:
            client.call("physics.bodyInfo", {"entity": "/Empty"})
        self.assert_engine_error(not_playing.exception, engine_client.INVALID_STATE, "InvalidState")
        self.assertIn("simulate", str(not_playing.exception.data.get("hint", "")))

        started = client.call("play.start", {"mode": "simulate", "lockstep": True})
        self.assertEqual(started["state"], "Simulate")
        self.assertEqual(started["mode"], "Simulate")
        # An entity without a collider owns no body.
        with self.assertRaises(engine_client.EngineError) as no_body:
            client.call("physics.bodyInfo", {"entity": "/Empty"})
        self.assert_engine_error(no_body.exception, engine_client.NOT_FOUND, "NotFound")
        self.assertEqual(no_body.exception.issues[0]["pointer"], "/entity")
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("physics.bodyInfo", {"entity": "/Nobody"})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")
        with self.assertRaises(engine_client.EngineError) as no_entity:
            client.call("physics.bodyInfo", {})
        self.assert_engine_error(no_entity.exception, engine_client.INVALID_PARAMS)
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
