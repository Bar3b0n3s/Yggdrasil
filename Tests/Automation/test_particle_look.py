"""The particle look through automation (Docs/Decisions/0048-m33-particle-look.md decisions 1, 7 and 8): a ParticleLook
beside a ParticleEmitter, with a flipbook sprite sheet cut from an imported texture, created, read and updated through
entity.create, entity.get and entity.update; its schema through component.schema; its rules refused, located, as the
inspector refuses them; and its emitter's particles playing with it, counted through script.eval."""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
FLAME_PNG = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Particles" / "Flame.png"


def look_of(client: engine_client.EngineClient, path: str) -> dict[str, Any]:
    """The ParticleLook component of the entity at `path`."""
    entity = client.call("entity.get", {"entity": path, "components": ["ParticleLook"]})["entity"]
    return entity["components"]["ParticleLook"]


class ParticleLookTests(AutomationTestCase):
    """entity.* and component.schema over the ParticleLook component."""

    def create_flipbook(self, client: engine_client.EngineClient) -> str:
        """Imports the flame fixture and cuts it into a sheet of 16 frames; returns the sheet's path."""
        source_dir = self.directory / "Downloads"
        source_dir.mkdir()
        shutil.copyfile(FLAME_PNG, source_dir / "Flame.png")
        imported = client.call("asset.import", {"source": str(source_dir / "Flame.png"), "destDir": "Assets/Textures",
                                                "settings": {"Compression": "None"}})
        frames = [{"Rect": {"X": 32 * (index % 4), "Y": 32 * (index // 4), "Width": 32, "Height": 32}}
                  for index in range(16)]
        client.call("asset.create", {"type": "SpriteSheet", "path": "Assets/Flame.spritesheet",
                                     "values": {"Texture": imported["asset"]["id"], "Frames": frames}})
        return "Assets/Flame.spritesheet"

    def test_a_look_is_created_read_and_updated_beside_its_emitter(self) -> None:
        client, _ = self.open_editor_with_scene()
        sheet = self.create_flipbook(client)
        client.call("entity.create", {"name": "Flame", "components": {
            "ParticleEmitter": {"Rate": 20, "Blend": "Additive"},
            "ParticleLook": {"SpriteSheet": sheet, "FrameCount": 8, "FrameMode": "Random", "StartRotation": [-30, 30],
                             "Spin": [10, 90], "Alignment": "Flat", "SoftDistance": 0.5, "Intensity": 6}}})
        look = look_of(client, "/Flame")
        self.assertEqual(look["FrameCount"], 8, look)
        self.assertEqual(look["FrameMode"], "Random")
        self.assertEqual(look["StartRotation"], [-30, 30])
        self.assertEqual(look["Alignment"], "Flat")
        self.assertEqual(look["Intensity"], 6)
        # The defaults draw as an emitter without a look.
        self.assertEqual(look["FirstFrame"], 0)
        self.assertEqual(look["FrameCycles"], 1)
        self.assertFalse(look["BlendFrames"])

        client.call("entity.update", {"entity": "/Flame", "components": {
            "ParticleLook": {"Alignment": "Velocity", "VelocityStretch": 0.25}}})
        self.assertEqual(look_of(client, "/Flame")["Alignment"], "Velocity")
        self.assertEqual(look_of(client, "/Flame")["VelocityStretch"], 0.25)

        schema = client.call("component.schema", {"name": "ParticleLook"})
        fields = [field["name"] for field in schema["fields"]]
        self.assertEqual(fields, ["SpriteSheet", "FirstFrame", "FrameCount", "FrameMode", "FrameCycles", "BlendFrames",
                                  "StartRotation", "Spin", "Alignment", "VelocityStretch", "SoftDistance", "Intensity"],
                         schema)
        by_name = {field["name"]: field for field in schema["fields"]}
        self.assertEqual(by_name["Alignment"]["enumValues"], ["Camera", "Velocity", "Flat"])
        self.assertEqual(by_name["FrameMode"]["enumValues"], ["OverLife", "Random"])
        self.assertEqual(by_name["Spin"]["unit"], "deg/s")

        # The emitter plays with its look: the session steps its particles, angles and frames.
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 10, "render": "none"})
        code = 'return Scene.FindByName("Flame").ParticleEmitter:GetParticleCount()'
        count = client.call("script.eval", {"context": "play", "code": code})
        self.assertGreater(count["value"], 0, count)
        client.call("play.stop")

    def test_a_reversed_range_and_a_look_without_an_emitter_are_refused(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Sparks", "components": {"ParticleEmitter": {}, "ParticleLook": {}}})
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("entity.update", {"entity": "/Sparks", "components": {"ParticleLook": {"Spin": [90, -90]}}})
        self.assertIn("Spin", str(refused.exception.data), refused.exception.data)
        self.assertEqual(look_of(client, "/Sparks")["Spin"], [0, 0])
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("entity.create", {"name": "Lonely", "components": {"ParticleLook": {"Intensity": 2}}})
        self.assertIn("ParticleEmitter", str(missing.exception), missing.exception.data)


if __name__ == "__main__":
    unittest.main()
