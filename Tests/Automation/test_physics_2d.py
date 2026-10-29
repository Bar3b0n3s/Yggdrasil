"""2D physics on Jolt through automation (Docs/Decisions/0032-m21-contract.md decisions 8 to 13 and 15; Roadmap M21's 2D
physics, R-2D-PHYS)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase

SKIP_PHYSICS_2D = ("contract stub: the 2D world, its system, its callbacks and the physics2d methods "
                   "(ADR 0032 decisions 8 to 15, streams P1 and P2)")

RECORDER = """local T = {}
function T:OnCollisionEnter2D(other, contact) Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":" .. other.Name) end
return Script.Define("Recorder", T)
"""


class Physics2DTests(AutomationTestCase):
    @unittest.skip(SKIP_PHYSICS_2D)
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

    @unittest.skip(SKIP_PHYSICS_2D)
    def test_2d_contacts_reach_scripts_and_a_body_stays_in_its_plane(self) -> None:
        # Decisions 9, 10 and 13 (Roadmap M21 "2D contact events reach scripts in canonical order", "a 2D body never
        # leaves its plane"): a falling ball enters the floor once, both sides are called in UUID order, and the ball's
        # Z never changes.
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
        self.assertEqual(sorted(entity["name"] for entity in traces), ["Trace:Ball:Floor", "Trace:Floor:Ball"])
        ball = client.call("entity.get", {"entity": "/Ball", "target": "play"})["entity"]["components"]
        self.assertEqual(ball["Transform"]["Translation"][2], 0.25)
        client.call("play.stop")

    @unittest.skip(SKIP_PHYSICS_2D)
    def test_an_entity_in_both_worlds_is_a_validation_error(self) -> None:
        # Decision 12: PHYSICS2D_MIXED_WORLDS.
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Mixed", "components": {
            "RigidBody": {}, "SphereCollider": {}, "RigidBody2D": {}, "CircleCollider2D": {}}})
        report = client.call("project.validate", {"scope": "Scene"})
        codes = [diagnostic["code"] for diagnostic in report["diagnostics"]]
        self.assertIn("PHYSICS2D_MIXED_WORLDS", codes)


if __name__ == "__main__":
    unittest.main()
