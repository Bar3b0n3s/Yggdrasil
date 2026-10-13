"""The headless content browser generates real thumbnails via its normal safe points, and a reopened project reuses
them (EditorCore/Thumbnails/ThumbnailCache.h)."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from harness import AutomationTestCase, engine_client
from test_environment import png_pixels

# editor.screenshot calls that drive the content browser's frames: queue, load, render and upload need a few each.
MAX_FRAMES = 24


class ThumbnailsTests(AutomationTestCase):
    def capture_until_thumbnail(self, client: engine_client.EngineClient, directory: Path, asset: str) -> list[Path]:
        """Drives UI frames until the 128 px image of `asset` exists; returns the matching images."""
        images: list[Path] = []
        for _ in range(MAX_FRAMES):
            client.call("editor.screenshot", {"maxDimension": 1280})
            images = sorted(directory.glob(f"{asset}-128-*.png"))
            if images:
                break
        return images

    def assert_rendered_material(self, image: Path) -> None:
        """The preview is a lit sphere in the material's red on a different background, never a placeholder."""
        width, height, pixels = png_pixels(image)
        self.assertEqual((width, height), (128, 128))
        colors = {pixels[index:index + 3] for index in range(0, len(pixels), 4)}
        self.assertGreater(len(colors), 16, "a flat image is not a rendered preview")

        def average(left: int, top: int, size: int) -> tuple[float, float, float]:
            sums = [0, 0, 0]
            for y in range(top, top + size):
                for x in range(left, left + size):
                    offset = (y * width + x) * 4
                    for channel in range(3):
                        sums[channel] += pixels[offset + channel]
            return sums[0] / (size * size), sums[1] / (size * size), sums[2] / (size * size)

        # Lighting and a specular highlight desaturate the BaseColor (0.8, 0.1, 0.2), but red stays dominant.
        red, green, blue = average(40, 40, 48)
        self.assertGreater(red, 40.0, f"the sphere's centre is not lit: {(red, green, blue)}")
        self.assertGreater(red, green + 30.0, f"the centre does not show the red BaseColor: {(red, green, blue)}")
        self.assertGreater(red, blue + 20.0, f"the centre does not show the red BaseColor: {(red, green, blue)}")
        corner = average(0, 0, 8)
        self.assertGreater(abs(red - corner[0]) + abs(green - corner[1]) + abs(blue - corner[2]), 30.0,
                           f"the background {corner} is not distinct from the sphere {(red, green, blue)}")

    def test_thumbnails_generated_headless(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        asset = client.call("asset.create", {
            "type": "Material", "path": "Assets/Preview.material",
            "values": {"BaseColor": [0.8, 0.1, 0.2, 1.0]},
        })["asset"]["id"]
        directory = root / "Library/Cache/Thumbnails"
        images = self.capture_until_thumbnail(client, directory, asset)
        self.assertEqual(len(images), 1, "the headless content browser did not generate the material preview")
        self.assert_rendered_material(images[0])
        manifest = json.loads((directory / f"{asset}-128.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["Format"], "ThumbnailManifest")
        self.assertIn(asset, manifest["Inputs"])
        self.assertTrue(images[0].name.endswith(f"-{manifest['Digest']}.png"))
        before = {path: path.read_bytes() for path in images}
        client.call("asset.list")
        client.call("editor.screenshot", {"maxDimension": 1280})
        self.assertEqual({path: path.read_bytes() for path in directory.glob(f"{asset}-128-*.png")}, before)
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)

        # Another session of the same project reuses the image: nothing is rendered or written again.
        stamp = images[0].stat().st_mtime_ns
        reopened = self.connect(self.start_editor(renderer="vulkan", project=root))
        for _ in range(MAX_FRAMES // 2):
            reopened.call("editor.screenshot", {"maxDimension": 1280})
        self.assertEqual(sorted(directory.glob("*.png")), images)
        self.assertEqual(images[0].read_bytes(), before[images[0]])
        self.assertEqual(images[0].stat().st_mtime_ns, stamp)
        reopened.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)


if __name__ == "__main__":
    unittest.main()
