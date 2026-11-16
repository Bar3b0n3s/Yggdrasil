"""M31's per-entity material parameters through automation (Docs/Decisions/0046-m31-material-parameters.md
decisions 1, 4 and 8; World Tree's brief 5): the MaterialParameters component is ordinary component data, which the
generic entity methods create, read, update and undo, whose ranges refuse what the renderer cannot draw and which needs
a MeshRenderer; and, in a rendering editor, 200 instances of one mesh with 200 different tints draw in as few draws as
200 untinted ones (stats.get), because the parameters ride in the instance records.

Every wait here is a bounded observation wait (the displayed views report a frame after the edit that changed them),
never a timing gate.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from typing import Any

from harness import AutomationTestCase, engine_client

OBSERVATION_SECONDS = 60.0
# The built-in cube's triangles (six faces of two each).
CUBE_TRIANGLES = 12
FIELDS = {"Tint", "Emissive", "EmissiveStrength", "UVOffset", "UVScroll", "DissolveAmount", "DissolveEdgeColor",
          "DissolveEdgeStrength", "DissolveEdgeWidth", "DissolveNoiseScale", "Opacity"}


def parameters_of(client: engine_client.EngineClient, path: str) -> dict[str, Any]:
    """The MaterialParameters component of the entity at `path`."""
    entity = client.call("entity.get", {"entity": path, "components": ["MaterialParameters"]})["entity"]
    return entity["components"]["MaterialParameters"]


def update_parameters(client: engine_client.EngineClient, path: str, values: dict[str, Any]) -> dict[str, Any]:
    """entity.update of the MaterialParameters of the entity at `path` with the merge patch `values`."""
    return client.call("entity.update", {"entity": path, "components": {"MaterialParameters": values}})


class MaterialParametersTests(AutomationTestCase):

    def test_component_round_trips_through_the_entity_methods_and_one_undo(self) -> None:
        client, _ = self.open_editor_with_scene()
        schema = client.call("component.schema", {"name": "MaterialParameters"})
        fields = {field["name"]: field for field in schema["fields"]}
        self.assertEqual(set(fields), FIELDS)
        self.assertEqual(fields["UVScroll"]["unit"], "1/s")
        client.call("entity.create", {"name": "Lava", "components": {
            "MeshRenderer": {"Mesh": "engine://Meshes/Plane"},
            "MaterialParameters": {"Tint": [1, 0.5, 0.25], "UVScroll": [0.1, 0.05], "Emissive": [1, 0.4, 0.1],
                                   "EmissiveStrength": 2}}})
        lava = parameters_of(client, "/Lava")
        self.assertEqual(lava["Tint"], [1, 0.5, 0.25])
        self.assertEqual(lava["EmissiveStrength"], 2)
        self.assertEqual(lava["Opacity"], 1)
        self.assertEqual(lava["DissolveAmount"], 0)

        update_parameters(client, "/Lava", {"DissolveAmount": 0.5, "Opacity": 0.25})
        updated = parameters_of(client, "/Lava")
        self.assertEqual((updated["DissolveAmount"], updated["Opacity"]), (0.5, 0.25))
        self.assertEqual(updated["Tint"], [1, 0.5, 0.25], "a merge patch keeps the fields it does not name")
        client.call("edit.undo")
        restored = parameters_of(client, "/Lava")
        self.assertEqual((restored["DissolveAmount"], restored["Opacity"]), (0, 1))

    def test_out_of_range_values_and_a_missing_mesh_renderer_are_refused(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Wall", "components": {
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}, "MaterialParameters": {}}})
        refused_values = (("Opacity", 1.5), ("DissolveAmount", -0.25), ("DissolveNoiseScale", 0), ("Tint", [2, 0, 0]),
                          ("EmissiveStrength", -1))
        for field, value in refused_values:
            with self.subTest(field=field):
                with self.assertRaises(engine_client.EngineError) as refused:
                    update_parameters(client, "/Wall", {field: value})
                # An out-of-range value is an invalid parameter, located at the field (a colour at its channel).
                self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS, "InvalidArgument")
                pointer = refused.exception.data["location"]["pointer"]
                self.assertTrue(pointer.startswith(f"/components/MaterialParameters/{field}"), pointer)
        self.assertEqual(parameters_of(client, "/Wall")["Opacity"], 1)
        # The overrides apply to a MeshRenderer's materials, so the component requires one.
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("entity.create", {"name": "Bare", "components": {"MaterialParameters": {"Opacity": 0.5}}})
        self.assertIn("MeshRenderer", str(refused.exception))

    def test_tinted_instances_of_one_mesh_draw_in_as_few_draws_as_untinted_ones(self) -> None:
        # World Tree's brief 5, "200 tinted instances draw in as few draws as untinted ones (stats.get)": the opaque
        # passes record one draw for the 200 cubes, tinted or not, so the tints never split the instanced draw.
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("entity.create", {"name": "Camera", "components": {
            "Transform": {"Translation": [0, 12, 16], "EulerAngles": [-38, 0, 0]},
            "Camera": {"Primary": True, "Clear": "Color"}}})
        client.call("viewport.camera", {"position": [0, 12, 16], "target": [0, 0, 0]})
        # 200 cubes of one mesh and material, their MaterialParameters at the defaults (neutral) to start with.
        ops = [{"method": "entity.create", "params": {"name": f"Cube{index}", "components": {
            "Transform": {"Translation": [-9.5 + (index % 20), 0.4, -4.5 + (index // 20)], "Scale": [0.6, 0.6, 0.6]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}, "MaterialParameters": {}}}} for index in range(200)]
        client.call("edit.batch", {"label": "Cubes", "ops": ops})
        cubes = 200 * CUBE_TRIANGLES

        def draws(view: dict[str, Any]) -> tuple[int, int]:
            rows = {row["name"]: row for row in view["passes"]}
            entry = rows.get("ForwardOpaque", {"drawCalls": -1, "triangles": -1})
            return entry["drawCalls"], entry["triangles"]

        def wait_for(ready: Callable[[dict[str, Any]], bool], after: int = -1) -> list[dict[str, Any]]:
            """The displayed views of stats.get once one of them, of a frame after `after`, satisfies `ready`."""
            deadline = time.monotonic() + OBSERVATION_SECONDS
            while True:
                stats = client.call("stats.get")
                views = [view for view in stats["views"]
                         if view["available"] and int(view["frame"]) > after and ready(view)]
                if views:
                    return views
                if time.monotonic() >= deadline:
                    self.fail(f"no displayed view reached the expected state within {OBSERVATION_SECONDS:.0f} s: "
                              f"{stats}")

        for view in wait_for(lambda view: draws(view)[1] == cubes):
            self.assertEqual(draws(view), (1, cubes), view)

        tints = [{"method": "entity.update", "params": {"entity": f"/Cube{index}", "components": {
            "MaterialParameters": {"Tint": [round(1 - index / 199, 4), round(index / 199, 4), 0.5]}}}}
            for index in range(200)]
        client.call("edit.batch", {"label": "Tints", "ops": tints})
        edited = max((int(view["frame"]) for view in client.call("stats.get")["views"] if view["available"]),
                     default=-1)
        for view in wait_for(lambda view: draws(view)[1] == cubes, edited):
            self.assertEqual(draws(view), (1, cubes), view)
        self.assertEqual(parameters_of(client, "/Cube199")["Tint"], [0, 1, 0.5])
