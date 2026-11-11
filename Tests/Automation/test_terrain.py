"""Terrain through automation (Docs/Decisions/0037-m26-contract.md decisions 20 and 23; Roadmap M26's terrain with
sculpt and paint tools): terrain.importHeightmap converts a 16-bit PNG or RAW heightmap, and terrain.edit, the terrain
tools' one action, sculpts and paints with a stroke whose bytes equal the editor's and which one undo restores."""

from __future__ import annotations

import struct
import unittest
from pathlib import Path
from typing import Any

from harness import AutomationTestCase, engine_client

SKIP_IMPORT = "contract stub: terrain.importHeightmap (ADR 0037 decision 20, stream T-A)"
SKIP_EDIT = "contract stub: terrain.edit (ADR 0037 decision 23, stream T-E)"


def ramp_raw(side: int) -> bytes:
    """A RAW heightmap of `side` x `side` little-endian 16-bit samples rising along +X."""
    return b"".join(struct.pack("<H", (x * 65535) // (side - 1)) for _ in range(side) for x in range(side))


def import_heightmap(client: engine_client.EngineClient, root: Path, name: str, raw: bytes) -> dict[str, Any]:
    """Writes `raw` as Heightmaps/<name>.raw and converts it into Assets/Terrain/<name>.heightfield."""
    (root / "Heightmaps").mkdir(exist_ok=True)
    (root / "Heightmaps" / f"{name}.raw").write_bytes(raw)
    return client.call("terrain.importHeightmap",
                       {"source": f"Heightmaps/{name}.raw", "path": f"Assets/Terrain/{name}.heightfield"})


def terrain_of(client: engine_client.EngineClient, path: str = "/Terrain") -> dict[str, Any]:
    """The Terrain component of the entity at `path`."""
    return client.call("entity.get", {"entity": path})["components"]["Terrain"]


class TerrainTests(AutomationTestCase):
    @unittest.skip(SKIP_IMPORT)
    def test_a_raw_heightmap_imports_as_a_heightfield(self) -> None:
        # Decision 20: a square RAW of 16-bit samples, at most 1025 a side, becomes a .heightfield bit for bit.
        client, root = self.open_editor_with_scene()
        result = import_heightmap(client, root, "Ramp", ramp_raw(33))
        self.assertEqual(result["resolution"], 33)
        self.assertEqual(result["asset"]["type"], "Heightfield")
        self.assertTrue((root / "Assets" / "Terrain" / "Ramp.heightfield").is_file())
        with self.assertRaises(engine_client.EngineError) as refused:
            import_heightmap(client, root, "Odd", ramp_raw(33)[:-2])
        self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS, "InvalidArgument")

    @unittest.skip(SKIP_EDIT)
    def test_a_stroke_rewrites_the_heightfield_and_one_undo_restores_it(self) -> None:
        # Decision 23: terrain.edit applies one stroke through EditTerrain, the editor tools' one call, rewriting the
        # heightfield's source; the same stroke twice from the same bytes gives the same bytes, and undo restores them.
        client, root = self.open_editor_with_scene()
        import_heightmap(client, root, "Flat", bytes(33 * 33 * 2))
        client.call("entity.create", {"name": "Terrain", "components": {
            "Terrain": {"Heightfield": "Assets/Terrain/Flat.heightfield", "Size": [32, 32]}}})
        source = root / "Assets" / "Terrain" / "Flat.heightfield"
        before = source.read_bytes()
        stroke = {"entity": "/Terrain", "tool": "Raise", "points": [[16, 0, 16], [18, 0, 16]], "radius": 4,
                  "strength": 0.5}
        edited = client.call("terrain.edit", stroke)
        self.assertEqual(edited["path"], "Assets/Terrain/Flat.heightfield")
        self.assertGreater(edited["changed"], 0)
        after = source.read_bytes()
        self.assertNotEqual(after, before)
        client.call("edit.undo")
        self.assertEqual(source.read_bytes(), before)
        client.call("terrain.edit", stroke)
        self.assertEqual(source.read_bytes(), after)

    @unittest.skip(SKIP_EDIT)
    def test_painting_makes_a_linear_uncompressed_splat_map(self) -> None:
        # Decisions 20 and 23: the first paint of a terrain without a splat map makes one beside its heightfield,
        # named after the terrain's entity, imported as a data texture (Usage Linear and Compression None, so its
        # painted bytes are sampled as they are), and assigns it in the same undoable edit. A second terrain on the
        # same heightfield gets a splat map of its own and leaves the first one's bytes alone.
        client, root = self.open_editor_with_scene()
        import_heightmap(client, root, "Flat", bytes(17 * 17 * 2))
        ids: dict[str, str] = {}
        for name in ("Terrain", "Other"):
            created = client.call("entity.create", {"name": name, "components": {
                "Terrain": {"Heightfield": "Assets/Terrain/Flat.heightfield"}}})
            ids[name] = created["entity"]["id"]
        stroke = {"tool": "Paint", "layer": 1, "points": [[8, 0, 8]], "radius": 6}
        splat_maps = []
        for name in ("Terrain", "Other"):
            painted = client.call("terrain.edit", {"entity": f"/{name}", **stroke})
            self.assertTrue(painted["createdSplatMap"])
            self.assertEqual(painted["path"], f"Assets/Terrain/Flat.{ids[name]}.splat.png")
            settings = client.call("asset.getImportSettings", {"asset": painted["path"]})["settings"]
            self.assertEqual(settings["Usage"], "Linear")
            self.assertEqual(settings["Compression"], "None")
            self.assertIsNotNone(terrain_of(client, f"/{name}")["SplatMap"])
            splat_maps.append((root / painted["path"], (root / painted["path"]).read_bytes()))
        self.assertNotEqual(splat_maps[0][0], splat_maps[1][0])
        self.assertEqual(splat_maps[0][0].read_bytes(), splat_maps[0][1])
        client.call("edit.undo")
        self.assertIsNone(terrain_of(client, "/Other")["SplatMap"])
        self.assertIsNotNone(terrain_of(client)["SplatMap"])
        client.call("edit.undo")
        self.assertIsNone(terrain_of(client)["SplatMap"])


if __name__ == "__main__":
    unittest.main()
