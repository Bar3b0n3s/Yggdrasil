"""Navigation through automation (Docs/Decisions/0045-m30-navigation.md decisions 8 to 12; Roadmap M30, World Tree's
brief 1): navigation.bake, the editor's Bake Navigation action, builds the saved scene's navigation mesh from its static
colliders without a renderer, writes it beside the scene, assigns it in one undoable edit, and two bakes write
byte-identical files; project.validate reports a scene not baked and a bake made stale by a moved wall; a playing editor
and a scene without a Navigation component are refused; a play session's scripts query the baked mesh through the
Navigation namespace; and the navigation view (viewport.setOptions navMesh, viewport.screenshot annotate.navMesh) draws
the mesh's polygon edges."""

from __future__ import annotations

import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client
from test_environment import png_pixels


def navigation_codes(client: engine_client.EngineClient) -> list[str]:
    """The NAV_ codes project.validate reports, in its order."""
    report = client.call("project.validate")
    return [diagnostic["code"] for diagnostic in report["diagnostics"] if diagnostic["code"].startswith("NAV_")]


def build_arena(client: engine_client.EngineClient) -> None:
    """A floor with a pillar, a not walkable channel and a Navigation component of two agent types."""
    client.call("entity.create", {"name": "Floor", "components": {
        "Transform": {"Translation": [0, -0.5, 0]}, "BoxCollider": {"HalfExtents": [10, 0.5, 10]}}})
    client.call("entity.create", {"name": "Pillar", "components": {
        "Transform": {"Translation": [-4, 1.5, 0]}, "BoxCollider": {"HalfExtents": [0.5, 1.5, 0.5]}}})
    client.call("entity.create", {"name": "Lava", "components": {
        "Transform": {"Translation": [-3, 0, -5]}, "NavModifier": {"HalfExtents": [7, 0.5, 1]}}})
    client.call("entity.create", {"name": "Navigation", "components": {"Navigation": {"AgentTypes": [
        {"Name": "Small", "Radius": 0.4, "Height": 1.8, "MaxSlope": 45, "MaxStep": 0.4},
        {"Name": "Large", "Radius": 1.0, "Height": 2.0, "MaxSlope": 45, "MaxStep": 0.4}]}}})
    client.call("scene.save")


def baked_mesh(client: engine_client.EngineClient) -> str | None:
    """The NavMesh field of the scene's Navigation component."""
    entity = client.call("entity.get", {"entity": "/Navigation", "components": ["Navigation"]})["entity"]
    return entity["components"]["Navigation"]["NavMesh"]


class NavigationBakeTests(AutomationTestCase):
    def test_two_bakes_write_byte_identical_meshes_and_a_moved_wall_is_stale(self) -> None:
        # Decision 8: the bake writes Assets/Navigation/<the scene's path below Assets/ without .scene>.navmesh,
        # imports it and sets the Navigation component's NavMesh in one undo step; a second bake of the unchanged scene
        # changes no file and no component, so it records no step. Decision 9: project.validate reports
        # NAV_MESH_NOT_BAKED before the bake and NAV_MESH_STALE once a wall moves. The editor has no renderer: the bake
        # is CPU only.
        client, root = self.open_editor_with_scene()
        build_arena(client)
        self.assertEqual(navigation_codes(client), ["NAV_MESH_NOT_BAKED"])

        first = client.call("navigation.bake")
        self.assertEqual(first["path"], "Assets/Navigation/Scenes/Main.navmesh")
        self.assertTrue(first["changed"])
        self.assertEqual(first["asset"]["type"], "NavMesh")
        self.assertEqual([agent["name"] for agent in first["agents"]], ["Small", "Large"])
        self.assertTrue(all(agent["polygons"] > 0 for agent in first["agents"]))
        self.assertEqual(baked_mesh(client), first["asset"]["id"])
        bytes_after_first = (root / first["path"]).read_bytes()
        self.assertEqual(bytes_after_first[:4], b"ENAV")
        self.assertEqual(navigation_codes(client), [])

        second = client.call("navigation.bake")
        self.assertFalse(second["changed"])
        self.assertEqual(second["inputHash"], first["inputHash"])
        self.assertEqual((root / first["path"]).read_bytes(), bytes_after_first)

        # Moving a wall makes the bake stale, naming the scene.
        client.call("entity.update", {"entity": "/Pillar", "components": {"Transform": {"Translation": [-2, 1.5, 0]}}})
        report = client.call("project.validate")
        stale = [diagnostic for diagnostic in report["diagnostics"] if diagnostic["code"] == "NAV_MESH_STALE"]
        self.assertEqual(len(stale), 1, report["diagnostics"])
        self.assertIn("Main.scene", str(stale[0]))

        # Undo takes back the move, so the bake reads as current again; one more undo takes back the first bake's
        # assignment (the second bake recorded nothing).
        client.call("edit.undo")
        self.assertEqual(navigation_codes(client), [])
        client.call("edit.undo")
        self.assertIsNone(baked_mesh(client))

    def test_a_playing_editor_and_a_scene_without_navigation_cannot_bake(self) -> None:
        # Decision 8: the mesh is baked from the edit scene, so a playing editor is refused and nothing is written; a
        # scene without a Navigation component is NotFound, naming the component.
        client, root = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("navigation.bake")
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")
        build_arena(client)
        client.call("play.start", {"lockstep": True})
        with self.assertRaises(engine_client.EngineError) as playing:
            client.call("navigation.bake")
        self.assert_engine_error(playing.exception, engine_client.INVALID_STATE, "InvalidState")
        client.call("play.stop")
        self.assertFalse((root / "Assets" / "Navigation").exists())
        self.assertIsNone(baked_mesh(client))


class NavigationQueryTests(AutomationTestCase):
    def test_scripts_query_the_baked_mesh_in_play(self) -> None:
        # Decisions 10 and 11: the play session builds the scene's navigation from its baked mesh; a path from one side
        # of the not walkable channel to the other goes around its end and reaches its goal; the large agent type keeps
        # its larger clearance from the pillar; a raycast into the pillar stops short of it.
        client, _ = self.open_editor_with_scene()
        build_arena(client)
        client.call("navigation.bake")
        client.call("scene.save")
        client.call("play.start", {"lockstep": True})

        def evaluate(code: str) -> object:
            return client.call("script.eval", {"context": "play", "code": code})["value"]

        complete, corners = evaluate(
            "local path, complete = Navigation.FindPath(vector.create(-6, 0, -8), vector.create(-6, 0, -2)) "
            "return {complete, path}")
        self.assertTrue(complete)
        self.assertGreater(max(corner[0] for corner in corners), 4.0, corners)
        for agent, clearance in (("Small", 0.4), ("Large", 1.0)):
            point = evaluate(f"return Navigation.ClosestPoint(vector.create(-4, 0, 0), 3, '{agent}')")
            outside = max(abs(point[0] + 4.0) - 0.5, abs(point[2]) - 0.5)
            self.assertGreater(outside, clearance - 0.25, (agent, point))
        hit = evaluate("local hit = Navigation.Raycast(vector.create(-8, 0, 0), vector.create(-1, 0, 0)) "
                       "return {hit.Clear, hit.Point}")
        self.assertFalse(hit[0])
        self.assertLess(hit[1][0], -4.5)
        client.call("play.stop")


class NavigationViewTests(AutomationTestCase):
    def test_the_navigation_view_draws_the_baked_mesh(self) -> None:
        # Decision 12: viewport.setOptions' navMesh shows the scene view's navigation mesh (off by default), and
        # viewport.screenshot's annotate.navMesh draws the polygon edges into a capture, which changes the image only
        # where the mesh lies, and the same capture twice gives the same image.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        self.assertFalse(client.call("viewport.setOptions")["navMesh"])
        self.assertTrue(client.call("viewport.setOptions", {"navMesh": True})["navMesh"])
        self.assertFalse(client.call("viewport.setOptions", {"navMesh": False})["navMesh"])

        build_arena(client)
        client.call("navigation.bake")
        client.call("entity.create", {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 18, 0.001], "EulerAngles": [-90, 0, 0]},
            "Camera": {"Primary": True, "Clear": "Color", "ClearColor": [0, 0, 0]}}})
        size = {"view": "game", "width": 160, "height": 120}
        clean = Path(client.call("viewport.screenshot", size)["path"])
        annotated_params = {**size, "annotate": {"navMesh": True}}
        annotated = Path(client.call("viewport.screenshot", annotated_params)["path"])
        repeated = Path(client.call("viewport.screenshot", annotated_params)["path"])
        self.assertNotEqual(annotated.read_bytes(), clean.read_bytes())
        self.assertEqual(repeated.read_bytes(), annotated.read_bytes())
        width, height, before = png_pixels(clean)
        _, _, after = png_pixels(annotated)
        changed = sum(1 for pixel in range(0, width * height * 4, 4)
                      if before[pixel:pixel + 4] != after[pixel:pixel + 4])
        self.assertGreater(changed, 100)


if __name__ == "__main__":
    unittest.main()
