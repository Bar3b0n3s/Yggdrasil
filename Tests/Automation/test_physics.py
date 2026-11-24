"""Physics through automation (Docs/Architecture.md §5.6 Simulate, §9, §13.5 "physics.bodyInfo", §13.7
PHYSICS_ADJACENT_STATIC_BODIES; Roadmap M11 acceptance): a Simulate session drops a ball, physics.bodyInfo reports its
velocity, sleeping state and contacts, and project.validate reports adjacent implicit static bodies and fixes them with
a Static RigidBody on their common parent. test_simulate_mode_ball_falls, test_body_info_reports_contacts and
test_validate_reports_adjacent_static_bodies_and_fix_adds_parent_body are the Roadmap acceptance tests;
test_body_info_needs_a_play_session_and_a_body covers physics.bodyInfo's errors.
test_belt_carries_a_character_that_reports_its_ground_collider covers M36's ColliderSurface (Docs/Decisions/
0053-m36-contact-surfaces.md): physics.bodyInfo's groundCollider, a surface velocity written through entity.update, and
PHYSICS_SURFACE_UNUSED; test_body_info_reports_a_squeezed_character covers its W36.2 squeeze (physics.bodyInfo's
squeezed and squeezedBy). test_joint_info_reads_a_joint_in_play reads physics.jointInfo (M37,
Docs/Decisions/0054-m37-joints.md decision 10) and its errors. test_body_info_reports_a_characters_last_move reads
physics.bodyInfo's lastMove after a CharacterController:MoveFree (M48, Docs/Decisions/
0066-m48-characters-off-the-ground-and-on-light-bodies.md decision 3).
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

    def test_belt_carries_a_character_that_reports_its_ground_collider(self) -> None:
        client, _ = self.open_editor_with_scene()
        # One static compound: a marble slab and a belt beside it, the belt's surface moving along +X.
        client.call("entity.create", {"name": "Level", "components": {"RigidBody": {"Type": "Static"}}})
        client.call("entity.create", {"name": "Marble", "parent": "/Level", "components": {
            "Transform": {"Translation": [0, -0.5, -3]}, "BoxCollider": {"HalfExtents": [10, 0.5, 1.5]}}})
        client.call("entity.create", {"name": "Belt", "parent": "/Level", "components": {
            "Transform": {"Translation": [0, -0.5, 0]}, "BoxCollider": {"HalfExtents": [10, 0.5, 1.5]},
            "ColliderSurface": {"Friction": 0.8, "SurfaceVelocity": [2, 0, 0]}}})
        client.call("entity.create", {"name": "Walker", "components": {
            "Transform": {"Translation": [-5, 0.05, 0]}, "CharacterController": {}}})

        # A surface on an entity without a collider applies to nothing.
        client.call("entity.create", {"name": "Loose", "components": {"ColliderSurface": {}}})
        report = client.call("project.validate", {"scope": "scene"})
        unused = [item for item in report["diagnostics"] if item["code"] == "PHYSICS_SURFACE_UNUSED"]
        self.assertEqual(len(unused), 1, report["diagnostics"])
        self.assertEqual(unused[0]["severity"], "Warning")
        self.assertFalse(unused[0]["autoFixable"])

        # Simulate: no script moves the character, and the belt carries it about 2 m in a second.
        client.call("play.start", {"mode": "simulate", "lockstep": True})
        client.call("play.step", {"ticks": 70, "render": "none"})
        walker = client.call("physics.bodyInfo", {"entity": "/Walker"})
        self.assertTrue(walker["grounded"])
        self.assertEqual(walker["groundCollider"]["path"], "/Level/Belt")
        self.assertGreater(walker["position"][0], -3.5)
        level = client.call("physics.bodyInfo", {"entity": "/Level/Belt"})
        self.assertEqual(level["groundCollider"]["id"], "")

        # The belt stops through entity.update in the play scene: the character stops with it, still on the belt.
        client.call("entity.update", {"entity": "/Level/Belt", "target": "play", "components": {
            "ColliderSurface": {"SurfaceVelocity": [0, 0, 0]}}})
        client.call("play.step", {"ticks": 2, "render": "none"})
        stopped = client.call("physics.bodyInfo", {"entity": "/Walker"})["position"][0]
        client.call("play.step", {"ticks": 30, "render": "none"})
        still = client.call("physics.bodyInfo", {"entity": "/Walker"})["position"][0]
        self.assertAlmostEqual(still, stopped, delta=0.01)
        client.call("play.stop")

    def test_body_info_reports_a_squeezed_character(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Floor", "components": {
            "Transform": {"Translation": [0, -0.5, 0]}, "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": [10, 0.5, 10]}}})
        client.call("entity.create", {"name": "Pinned", "components": {
            "Transform": {"Translation": [0, 0.02, 0]}, "CharacterController": {}}})
        client.call("entity.create", {"name": "Press", "components": {
            "Transform": {"Translation": [0, 2.35, 0]}, "RigidBody": {"Type": "Kinematic"},
            "BoxCollider": {"HalfExtents": [1, 0.25, 1]}}})
        client.call("play.start", {"mode": "simulate", "lockstep": True})
        client.call("play.step", {"ticks": 10, "render": "none"})
        standing = client.call("physics.bodyInfo", {"entity": "/Pinned"})
        self.assertFalse(standing["squeezed"])
        self.assertEqual(standing["squeezedBy"]["id"], "")

        # The press comes down at 1 m/s, a tick at a time, until it holds the character against the floor.
        squeezed = None
        for tick in range(40):
            client.call("entity.update", {"entity": "/Press", "target": "play", "components": {
                "Transform": {"Translation": [0, 2.35 - (tick + 1) / 60, 0]}}})
            client.call("play.step", {"ticks": 1, "render": "none"})
            info = client.call("physics.bodyInfo", {"entity": "/Pinned"})
            if info["squeezed"]:
                squeezed = info
                break
        self.assertIsNotNone(squeezed)
        assert squeezed is not None
        self.assertEqual(squeezed["squeezedBy"]["path"], "/Press")
        client.call("play.stop")

    def test_body_info_reports_a_characters_last_move(self) -> None:
        client, _ = self.open_editor_with_scene()
        # A swimmer in open air with nothing under it, and a ball, which is no character.
        client.call("entity.create", {"name": "Swimmer", "components": {
            "Transform": {"Translation": [0, 10, 0]}, "CharacterController": {}}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [5, 10, 0]}, "RigidBody": {"Type": "Dynamic"},
            "SphereCollider": {"Radius": 0.5}}})
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 1, "render": "none"})
        self.assertEqual(client.call("physics.bodyInfo", {"entity": "/Swimmer"})["lastMove"], "Walk")

        # A free move in the air, consumed by the next step: the swimmer rises at the velocity it asked for, with no
        # gravity.
        client.call("script.eval", {"context": "play", "code":
                                    "Scene.FindByName('Swimmer').CharacterController:MoveFree(vector.create(0, 1, 0))"})
        client.call("play.step", {"ticks": 1, "render": "none"})
        swimmer = client.call("physics.bodyInfo", {"entity": "/Swimmer"})
        self.assertEqual(swimmer["lastMove"], "Free")
        self.assertAlmostEqual(swimmer["linearVelocity"][1], 1.0, places=4)

        # Without another call the next update walks: the swimmer falls on from the velocity it had.
        client.call("play.step", {"ticks": 1, "render": "none"})
        walked = client.call("physics.bodyInfo", {"entity": "/Swimmer"})
        self.assertEqual(walked["lastMove"], "Walk")
        self.assertLess(walked["linearVelocity"][1], 1.0)
        self.assertEqual(client.call("physics.bodyInfo", {"entity": "/Ball"})["lastMove"], "")
        client.call("play.stop")

    def test_joint_info_reads_a_joint_in_play(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("edit.batch", {"label": "Joints", "ops": [
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "Transform": {"Translation": [0, 5, 0]}, "RigidBody": {"Type": "Static"},
                "BoxCollider": {"HalfExtents": [0.1, 0.1, 0.1]}}}},
            {"method": "entity.create", "params": {"name": "Bob", "components": {
                "Transform": {"Translation": [0, 3, 0]}, "RigidBody": {"Type": "Dynamic"},
                "SphereCollider": {"Radius": 0.1},
                "DistanceJoint": {"ConnectedBody": "/Post", "MinDistance": 0, "MaxDistance": 2}}}},
            {"method": "entity.create", "params": {"name": "Plain"}}]})

        # Joints exist only in a play session.
        with self.assertRaises(engine_client.EngineError) as not_playing:
            client.call("physics.jointInfo", {"entity": "/Bob"})
        self.assert_engine_error(not_playing.exception, engine_client.INVALID_STATE, "InvalidState")

        client.call("play.start", {"mode": "simulate", "lockstep": True})
        client.call("play.step", {"ticks": 30, "render": "none"})
        info = client.call("physics.jointInfo", {"entity": "/Bob"})
        self.assertEqual(info["entity"]["name"], "Bob")
        self.assertEqual(info["type"], "Distance")
        self.assertEqual(info["body"]["name"], "Bob")
        self.assertEqual(info["connectedBody"]["name"], "Post")
        self.assertTrue(info["created"])
        self.assertFalse(info["broken"])
        self.assertAlmostEqual(info["distance"], 2.0, delta=0.05)
        self.assertGreater(info["force"], 1.0)
        self.assertEqual(info["motor"], "Off")
        self.assertEqual(info["tick"], 30)

        # An entity without a joint component holds no joint.
        with self.assertRaises(engine_client.EngineError) as no_joint:
            client.call("physics.jointInfo", {"entity": "/Plain"})
        self.assert_engine_error(no_joint.exception, engine_client.NOT_FOUND, "NotFound")
        self.assertEqual(no_joint.exception.issues[0]["pointer"], "/entity")
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
