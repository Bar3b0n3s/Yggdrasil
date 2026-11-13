"""Colour-grading LUTs through automation (Docs/Architecture.md §7.4; Docs/Decisions/0037-m26-contract.md decision 15,
stream X-B): an Adobe/IRIDAS .cube file imported with asset.import as a Lut asset by the Lut importer, the built-in
engine://Luts/Identity, and the located refusal of a .cube file the importer cannot read."""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client, read_json

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
WARM_CUBE = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Luts" / "Warm.cube"


class LutTests(AutomationTestCase):
    """asset.import of .cube files, and asset.info of the built-in identity LUT."""

    def test_cube_file_imports_as_a_lut_and_the_identity_lut_is_built_in(self) -> None:
        client, root = self.open_editor_with_scene()
        source_dir = self.directory / "Downloads"
        source_dir.mkdir()
        shutil.copyfile(WARM_CUBE, source_dir / "Warm.cube")
        imported = client.call("asset.import", {"source": str(source_dir / "Warm.cube"), "destDir": "Assets/Luts"})
        lut = imported["asset"]
        self.assertEqual(lut["type"], "Lut", imported)
        self.assertEqual(read_json(root / "Assets" / "Luts" / "Warm.cube.meta")["Importer"], "Lut")
        info = client.call("asset.info", {"asset": lut["id"]})
        self.assertEqual(info["importer"], "Lut")
        self.assertEqual(info["importerVersion"], 1)
        self.assertEqual(info["diagnostics"], [])
        # The LUT reads only its own source: no dependency files and no referenced assets.
        self.assertEqual(info["dependencyFiles"], [])

        # The built-in identity LUT (procedural, no importer) at its reserved handle.
        identity = client.call("asset.info", {"asset": "engine://Luts/Identity"})
        self.assertEqual(identity["asset"]["type"], "Lut", identity)
        self.assertEqual(identity["asset"]["id"], "0000000000000281", identity)
        self.assertEqual(identity["importer"], "", identity)

    def test_cube_file_the_importer_cannot_read_is_refused_at_its_line(self) -> None:
        # Decision 15: a keyword the importer does not read (GAMMA, on line 3) refuses the import, located at its line.
        client, _ = self.open_editor_with_scene()
        source_dir = self.directory / "Downloads"
        source_dir.mkdir()
        source = source_dir / "Curve.cube"
        source.write_bytes(b'TITLE "Curve"\nLUT_3D_SIZE 2\nGAMMA 2.2\n')
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.import", {"source": str(source), "destDir": "Assets/Luts"})
        self.assert_engine_error(refused.exception, engine_client.VALIDATION_FAILED, "ImportFailed")
        self.assertEqual(refused.exception.data["location"]["line"], 3, refused.exception.data)


if __name__ == "__main__":
    unittest.main()
