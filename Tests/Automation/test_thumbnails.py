"""The headless content browser generates real thumbnails via its normal safe points."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase
from test_screenshot import png_size


class ThumbnailsTests(AutomationTestCase):
    def test_thumbnails_generated_headless(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        asset = client.call("asset.create", {
            "type": "Material", "path": "Assets/Preview.material",
            "values": {"BaseColor": [0.8, 0.1, 0.2, 1.0]},
        })["asset"]["id"]
        # Each capture constructs a fresh UI frame. The first queues the visible asset, the following
        # safe point loads/renders it; a first load changes its version and the next frame queues that version.
        for _ in range(4):
            client.call("editor.screenshot", {"maxDimension": 1280})
        thumbnails = sorted((root / "Library/Cache").glob(f"thumbnail-*-{asset}-*-128.png"))
        self.assertTrue(thumbnails, "the headless content browser did not generate the material preview")
        self.assertTrue(all(png_size(path) == (128, 128) for path in thumbnails))
        before = {path: path.read_bytes() for path in thumbnails}
        client.call("asset.list")
        client.call("editor.screenshot", {"maxDimension": 1280})
        self.assertEqual({path: path.read_bytes() for path in thumbnails}, before)
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), 0)


if __name__ == "__main__":
    unittest.main()
