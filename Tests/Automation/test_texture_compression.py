"""Texture compression through automation (Docs/Decisions/0034-m23-contract.md decisions 13 and 18, M23's
R-TEXC-BC): the Compression import setting of TextureImporter version 2, Auto by default, read and written with
asset.getImportSettings and asset.setImportSettings like every import setting, and a compressed texture's
content-browser preview, which shows its decoded texels."""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client, read_json
from test_environment import png_pixels

TEXTURE_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "Assets" / "Textures"

# editor.screenshot calls that drive the content browser's frames until both previews exist (test_thumbnails.py).
MAX_FRAMES = 24


class TextureCompressionTests(AutomationTestCase):
    def test_compression_defaults_to_auto_and_is_an_undoable_import_setting(self) -> None:
        client, root = self.open_editor_with_scene()
        shutil.copy(TEXTURE_FIXTURES / "Rgba.png", root / "Assets" / "Rgba.png")
        client.call("project.refreshAssets")
        info = client.call("asset.info", {"asset": "Assets/Rgba.png"})
        self.assertEqual(info["importer"], "Texture")
        self.assertEqual(info["importerVersion"], 2)
        self.assertEqual(info["settings"]["Compression"], "Auto")

        changed = client.call("asset.setImportSettings",
                              {"asset": "Assets/Rgba.png", "settings": {"Compression": "none"}})
        self.assertEqual(changed["settings"]["Compression"], "None")
        self.assertEqual(read_json(root / "Assets" / "Rgba.png.meta")["Settings"]["Compression"], "None")
        client.call("asset.reimport", {"asset": "Assets/Rgba.png"})
        reimported = client.call("asset.info", {"asset": "Assets/Rgba.png"})
        self.assertEqual(reimported["settings"]["Compression"], "None")
        self.assertEqual(reimported["diagnostics"], [])

        client.call("edit.undo")
        restored = client.call("asset.getImportSettings", {"asset": "Assets/Rgba.png"})
        self.assertEqual(restored["settings"]["Compression"], "Auto")

        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("asset.setImportSettings",
                        {"asset": "Assets/Rgba.png", "settings": {"Compression": "Lossy"}})
        self.assertIn("Lossy", str(refused.exception))
        kept = client.call("asset.getImportSettings", {"asset": "Assets/Rgba.png"})
        self.assertEqual(kept["settings"]["Compression"], "Auto")

    def test_a_compressed_texture_previews_its_decoded_texels(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        for name in ("Compressed.png", "Plain.png"):
            shutil.copy(TEXTURE_FIXTURES / "Rgba.png", root / "Assets" / name)
        client.call("project.refreshAssets")
        client.call("asset.setImportSettings", {"asset": "Assets/Plain.png", "settings": {"Compression": "None"}})
        compressed = client.call("asset.info", {"asset": "Assets/Compressed.png"})["asset"]["id"]
        plain = client.call("asset.info", {"asset": "Assets/Plain.png"})["asset"]["id"]

        directory = root / "Library/Cache/Thumbnails"
        previews: dict[str, list[Path]] = {}
        for _ in range(MAX_FRAMES):
            client.call("editor.screenshot", {"maxDimension": 1280})
            previews = {asset: sorted(directory.glob(f"{asset}-128-*.png")) for asset in (compressed, plain)}
            if all(previews.values()):
                break
        self.assertTrue(all(previews.values()), f"the content browser did not preview both textures: {previews}")

        # The BC7 preview decodes its blocks: it stays within BC7's error of the uncompressed preview of the same image,
        # where blocks read as texels would be noise.
        width, height, compressed_pixels = png_pixels(previews[compressed][-1])
        self.assertEqual((width, height), (128, 128))
        _, _, plain_pixels = png_pixels(previews[plain][-1])
        differences = [abs(left - right) for left, right in zip(compressed_pixels, plain_pixels)]
        self.assertLess(sum(differences) / len(differences), 4.0)
        self.assertLessEqual(max(differences), 48)
        self.assertNotEqual(len(set(compressed_pixels)), 1, "a flat preview is not the texture")
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)


if __name__ == "__main__":
    unittest.main()
