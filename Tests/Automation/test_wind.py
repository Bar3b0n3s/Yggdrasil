"""Wind through automation (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3F, 3G, 3H and
3J; Docs/Architecture.md §5.3, §6.5, §8.3 "Wind"): a Material whose wind block asset.create writes bends a row of reeds
in the Scene view's screenshot once a Wind made through entity.create blows over them (an edit-time image shows the lean
at time 0), and two screenshots of one state are byte-identical; a calm Wind renders the bytes of no Wind, and so does
the block disabled through asset.setProperties; a second Wind and a Direction without a horizontal part are refused;
entity.get reads every field back; and Wind:Sample through script.eval gives the velocities Scene/Wind.h's
double-precision model gives, rounded once to floats.

Every call here is a direct request and its answer; nothing waits on time.
"""

from __future__ import annotations

import struct
import unittest
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client

SHOT = {"view": "scene", "width": 160, "height": 90}
REED_MATERIAL = "Assets/Materials/Reed.material"
REED_WIND = {"Enabled": True, "Bend": 0.5, "Branch": 0, "Flutter": 0, "Stiffness": 0.1}
GALE = {"Direction": [1, 0, 0.25], "Strength": 20, "GustStrength": 6, "GustFrequency": 0.25, "Turbulence": 0.3}
# SampleWind of GALE at a point and time, in double precision, rounded to floats (Scripting/Bindings/Wind.cpp).
GALE_SAMPLES = (
    (([3, 0, 4], 1.75), (24.07280731201172, 0.0, 7.862316608428955)),
    (([-12.5, 2, 30], 47.25), (19.06694793701172, 0.0, 6.31603479385376)),
)


def as_float(value: float) -> float:
    """`value` rounded to a 32-bit float, the precision of a Luau vector."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


class WindTests(AutomationTestCase):
    """A rendering editor; each test needs a Vulkan device."""

    def shoot(self, client: engine_client.EngineClient) -> bytes:
        """The PNG bytes of a scene-view screenshot at SHOT's size."""
        return Path(client.call("viewport.screenshot", SHOT)["path"]).read_bytes()

    def wind(self, client: engine_client.EngineClient) -> dict[str, Any]:
        """The Gale entity's Wind as entity.get reads it."""
        return client.call("entity.get", {"entity": "/Gale", "components": ["Wind"]})["entity"]["components"]["Wind"]

    def test_wind_bends_wind_materials_and_samples_through_automation(self) -> None:
        if not self.require_gpu():
            return
        client, _ = self.open_editor_with_scene(renderer="vulkan")
        client.call("asset.create", {"type": "Material", "path": REED_MATERIAL,
                                     "values": {"BaseColor": [0.6, 0.62, 0.3, 1], "Roughness": 0.7, "Wind": REED_WIND}})
        reeds = [{"method": "entity.create", "params": {"name": f"Reed{index}", "components": {
            "Transform": {"Translation": [-2 + index, 0, 0], "Scale": [0.2, 4, 0.2]},
            "MeshRenderer": {"Mesh": "engine://Meshes/Cube", "Materials": [REED_MATERIAL]}}}} for index in range(5)]
        client.call("edit.batch", {"label": "Reeds", "ops": [
            *reeds,
            {"method": "entity.create", "params": {"name": "Sun", "components": {
                "Transform": {"EulerAngles": [-40, 20, 0]}, "DirectionalLight": {"Intensity": 2}}}},
            {"method": "entity.create", "params": {"name": "Post", "components": {
                "PostProcess": {"AntiAliasing": "None", "BloomEnabled": False, "SsaoEnabled": False}}}}]})
        client.call("viewport.camera", {"position": [0, 1.5, 7], "target": [0, 1, 0]})
        still = self.shoot(client)

        # A gale leans the reeds, already at an edit-time image's time 0.
        client.call("entity.create", {"name": "Gale", "components": {"Wind": GALE}})
        swept = self.shoot(client)
        self.assertNotEqual(swept, still)
        self.assertEqual(self.shoot(client), swept)
        read = self.wind(client)
        for field, value in GALE.items():
            if field == "Direction":
                for axis in range(3):
                    self.assertAlmostEqual(read[field][axis], value[axis], places=6)
            else:
                self.assertAlmostEqual(read[field], value, places=6)

        # Wind:Sample reads the component at the given time, in an edit-time evaluation too.
        for (position, time), expected in GALE_SAMPLES:
            code = (f"return Scene.FindByName(\"Gale\").Wind:Sample(vector.create({position[0]}, {position[1]}, "
                    f"{position[2]}), {time})")
            value = client.call("script.eval", {"code": code, "context": "Edit"})["value"]
            self.assertEqual([as_float(component) for component in value], list(expected), code)

        # A second Wind and a Direction without a horizontal part are refused; the scene keeps its one wind.
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.create", {"name": "SecondGale", "components": {"Wind": {}}})
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Gale", "components": {"Wind": {"Direction": [0, 1, 0]}}})
        with self.assertRaises(engine_client.EngineError):
            client.call("entity.update", {"entity": "/Gale", "components": {"Wind": {"Turbulence": 2}}})
        self.assertEqual(self.shoot(client), swept)

        # A calm wind displaces nothing: the image of no Wind, byte for byte.
        client.call("entity.update", {"entity": "/Gale", "components": {"Wind": {"Strength": 0, "GustStrength": 0}}})
        self.assertEqual(self.shoot(client), still)

        # Back in the gale, the material's block disabled leaves the reeds straight.
        client.call("entity.update", {"entity": "/Gale", "components": {"Wind": {"Strength": 20, "GustStrength": 6}}})
        self.assertEqual(self.shoot(client), swept)
        client.call("asset.setProperties",
                    {"asset": REED_MATERIAL, "values": {"Wind": {**REED_WIND, "Enabled": False}}})
        self.assertEqual(self.shoot(client), still)
        client.call("scene.save")


if __name__ == "__main__":
    unittest.main()
