"""Environments through automation (Docs/Architecture.md §7.4 EnvironmentImporter, §7.5, §8.6; Roadmap M8 acceptance
test_environment_import_and_screenshot and test_renderer_none_environment_import_uses_cached_bake): a Radiance .hdr is
imported, baked on the GPU, lights the scene and shows as its skybox; a cooked bake is reused by an editor without a
device, and an environment no cache holds is Unsupported there with the hint to bake it once with a GPU.

Skipped skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9): stream B implements the baker and
the importer and removes the skips; the screenshot needs stream A's scene renderer and stream C's tonemap too.
"""

from __future__ import annotations

import math
import struct
import unittest
import zlib
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, AutomationTestCase, engine_client

GPU_HINT = "start the editor with a GPU once to bake this environment"


def write_hdr(path: Path, width: int, rgb: tuple[float, float, float]) -> None:
    """Writes a flat (uncompressed) Radiance RGBE image of `width` x width / 2 texels of radiance `rgb`."""
    height = width // 2

    def rgbe(color: tuple[float, float, float]) -> bytes:
        largest = max(color)
        if largest < 1e-32:
            return bytes(4)
        mantissa, exponent = math.frexp(largest)  # largest = mantissa * 2^exponent, mantissa in [0.5, 1)
        scale = mantissa * 256.0 / largest
        return bytes([int(color[0] * scale), int(color[1] * scale), int(color[2] * scale), exponent + 128])

    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode("ascii")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + rgbe(rgb) * (width * height))


def png_pixels(path: Path) -> tuple[int, int, bytes]:
    """The width, height and RGBA8 pixels (rows top first) of an 8-bit RGBA, non-interlaced PNG, the screenshots'
    format, with the five PNG row filters undone."""
    data = path.read_bytes()
    position = 8
    width = height = 0
    compressed = b""
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            compressed += body
        position += 12 + length
    raw = zlib.decompress(compressed)
    stride = width * 4
    rows = bytearray()
    previous = bytearray(stride)
    for row in range(height):
        start = row * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        for index in range(stride):
            left = line[index - 4] if index >= 4 else 0
            up = previous[index]
            upper_left = previous[index - 4] if index >= 4 else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - upper_left
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
                line[index] = (line[index] + predictor) & 0xFF
        rows += line
        previous = line
    return width, height, bytes(rows)


class EnvironmentTests(AutomationTestCase):
    """Importing, rendering and reusing environments."""

    def set_environment(self, client: engine_client.EngineClient, environment: str) -> None:
        """A primary camera clearing to the skybox, looking down -Z, and an Environment entity with `environment`."""
        ops: list[dict[str, Any]] = [
            {"method": "entity.create", "params": {"name": "Camera", "components": {
                "Transform": {"Translation": [0, 0, 0]}, "Camera": {"Primary": True, "Clear": "Skybox"}}}},
            {"method": "entity.create", "params": {"name": "World", "components": {
                "Environment": {"Environment": environment, "ShowSkybox": True}}}},
        ]
        client.call("edit.batch", {"label": "Environment", "ops": ops})

    @unittest.skip("contract stub: un-skipped by M8 stream B")
    def test_environment_import_and_screenshot(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        source = self.directory / "Sources" / "Orange.hdr"
        write_hdr(source, 64, (2.0, 0.5, 0.125))
        imported = client.call("asset.import", {"source": str(source), "destDir": "Assets/Environments"})
        environment = imported["assets"][0]
        self.assertEqual(environment["type"], "Environment")
        self.assertTrue((root / "Assets" / "Environments" / "Orange.hdr.meta").is_file())

        self.set_environment(client, environment["id"])
        shot = client.call("viewport.screenshot", {"view": "game", "width": 64, "height": 36})
        width, height, pixels = png_pixels(Path(shot["path"]))
        self.assertEqual((width, height), (64, 36))
        # The skybox fills the view with the environment's colour: orange after tonemapping (red above green above
        # blue).
        centre = ((height // 2) * width + width // 2) * 4
        red, green, blue = pixels[centre], pixels[centre + 1], pixels[centre + 2]
        self.assertGreater(red, green)
        self.assertGreater(green, blue)
        # Without the skybox the camera's clear colour shows instead.
        client.call("entity.update", {"entity": "/World", "components": {"Environment": {"ShowSkybox": False}}})
        hidden = client.call("viewport.screenshot", {"view": "game", "width": 64, "height": 36})
        self.assertNotEqual(Path(hidden["path"]).read_bytes(), Path(shot["path"]).read_bytes())
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    @unittest.skip("contract stub: un-skipped by M8 stream B")
    def test_renderer_none_environment_import_uses_cached_bake(self) -> None:
        if not self.require_gpu():
            return
        # A rendering editor imports and bakes the environment into the project's cache.
        client, root = self.open_editor_with_scene(renderer="vulkan")
        source = self.directory / "Sources" / "Orange.hdr"
        write_hdr(source, 64, (2.0, 0.5, 0.125))
        imported = client.call("asset.import", {"source": str(source), "destDir": "Assets/Environments"})
        environment = imported["assets"][0]
        self.set_environment(client, environment["id"])
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

        # An editor without a device opens the project: the cooked bake is served, so nothing is reported.
        client = self.connect(self.start_editor(project=root))
        report = client.call("project.validate", {"scope": "project"})
        codes = [diagnostic["code"] for diagnostic in report["diagnostics"]]
        self.assertNotIn("ASSET_IMPORT_FAILED", codes, report)
        info = client.call("asset.info", {"asset": environment["id"]})["asset"]
        self.assertEqual(info["type"], "Environment")

        # A second copy of the same bytes has the same cache key (§7.5) and is served from the bake as well.
        copy = self.directory / "Sources" / "Copy.hdr"
        copy.write_bytes(source.read_bytes())
        client.call("asset.import", {"source": str(copy), "destDir": "Assets/Environments"})
        report = client.call("project.validate", {"scope": "project"})
        self.assertNotIn("ASSET_IMPORT_FAILED", [diagnostic["code"] for diagnostic in report["diagnostics"]], report)

        # An environment no cache holds cannot be baked here: its diagnostic names the GPU hint.
        other = self.directory / "Sources" / "Blue.hdr"
        write_hdr(other, 64, (0.1, 0.2, 1.0))
        client.call("asset.import", {"source": str(other), "destDir": "Assets/Environments"})
        report = client.call("project.validate", {"scope": "project"})
        failed = [diagnostic for diagnostic in report["diagnostics"] if diagnostic["code"] == "ASSET_IMPORT_FAILED"]
        self.assertEqual(len(failed), 1, report)
        self.assertIn("Blue.hdr", failed[0]["file"])
        self.assertIn(GPU_HINT, failed[0]["hint"] + failed[0]["message"])
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())


if __name__ == "__main__":
    unittest.main()
