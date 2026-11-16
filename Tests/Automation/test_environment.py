"""Environments through automation (Docs/Architecture.md §7.4 EnvironmentImporter, §7.5, §8.6; Roadmap M8 acceptance
test_environment_import_and_screenshot and test_renderer_none_environment_import_uses_cached_bake): a Radiance .hdr is
imported, baked on the GPU, lights the scene (its SH9 and specular cube on a white sphere) and shows as its skybox; a
cooked bake is reused by an editor without a device (under its own handle, and for a copy of the same bytes, which has
the same cache key) (Docs/Decisions/0013-m8-decisions.md decision 9), and since World Tree brief 14 (ADR 0042 decision
2) an environment no cache holds imports there with its bake deferred to the next editor with a GPU, a warning with the
hint to bake it once with a GPU and no asset error, so its scenes still play and test. M23
(Docs/Decisions/0034-m23-contract.md decisions 14 and 18): an OpenEXR .exr imports the same way (EnvironmentImporter
version 2), and its Compression import setting, Auto by default, stores the baked cubes as BC6H or, with None, as
RGBA16F, both showing the same skybox.
"""

from __future__ import annotations

import json
import math
import struct
import unittest
import zlib
from pathlib import Path
from typing import Any

from harness import EXIT_SUCCESS, AutomationTestCase, engine_client

GPU_HINT = "start the editor with a GPU once to bake this environment"


def read_meta_settings(path: Path) -> dict[str, Any]:
    """The Settings object of a .meta file."""
    settings: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))["Settings"]
    return settings


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


def write_exr(path: Path, width: int, rgb: tuple[float, float, float]) -> None:
    """Writes a single-part OpenEXR image of `width` x width / 2 texels of radiance `rgb`: half-float B, G and R
    scanlines, uncompressed (the OpenEXR file layout: magic number, version, header attributes, offset table,
    chunks)."""
    height = width // 2

    def attribute(name: str, kind: str, value: bytes) -> bytes:
        return name.encode("ascii") + b"\0" + kind.encode("ascii") + b"\0" + struct.pack("<i", len(value)) + value

    channels = b"".join(name + b"\0" + struct.pack("<iB3xii", 1, 0, 1, 1) for name in (b"B", b"G", b"R")) + b"\0"
    window = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header = (attribute("channels", "chlist", channels) + attribute("compression", "compression", b"\0")
              + attribute("dataWindow", "box2i", window) + attribute("displayWindow", "box2i", window)
              + attribute("lineOrder", "lineOrder", b"\0")
              + attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
              + attribute("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
              + attribute("screenWindowWidth", "float", struct.pack("<f", 1.0)) + b"\0")
    preamble = bytes([0x76, 0x2F, 0x31, 0x01]) + struct.pack("<I", 2) + header  # magic number; version 2, scanlines
    line = b"".join(struct.pack("<e", value) * width for value in (rgb[2], rgb[1], rgb[0]))
    chunk_size = 8 + len(line)
    table = b"".join(struct.pack("<Q", len(preamble) + 8 * height + row * chunk_size) for row in range(height))
    chunks = b"".join(struct.pack("<ii", row, len(line)) + line for row in range(height))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(preamble + table + chunks)


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

    def import_environment(self, client: engine_client.EngineClient, source: Path) -> dict[str, Any]:
        """asset.import of `source` into Assets/Environments; returns the imported asset's summary {id, path, type}."""
        imported = client.call("asset.import", {"source": str(source), "destDir": "Assets/Environments"})
        environment: dict[str, Any] = imported["asset"]
        self.assertEqual(environment["type"], "Environment", imported)
        return environment

    def test_environment_import_and_screenshot(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        source = self.directory / "Sources" / "Orange.hdr"
        write_hdr(source, 64, (2.0, 0.5, 0.125))
        environment = self.import_environment(client, source)
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

        # The baked SH9 and specular cube light the scene: a white sphere in front of the camera, over a neutral grey
        # clear colour, takes the environment's orange (red above green above blue, brighter than the clear colour) ...
        client.call("entity.update", {"entity": "/Camera", "components": {
            "Camera": {"ClearColor": [0.05, 0.05, 0.05]}}})
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0, 0, -3]}, "MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})
        lit = client.call("viewport.screenshot", {"view": "game", "width": 64, "height": 36})
        _, _, lit_pixels = png_pixels(Path(lit["path"]))
        red, green, blue = lit_pixels[centre], lit_pixels[centre + 1], lit_pixels[centre + 2]
        corner = 0  # the clear colour at the top-left pixel
        self.assertGreater(red, green, (red, green, blue))
        self.assertGreater(green, blue, (red, green, blue))
        self.assertGreater(red, lit_pixels[corner] + 32, (red, lit_pixels[corner]))
        # ... and without the environment map the neutral FallbackColor lights it instead, no longer orange.
        client.call("entity.update", {"entity": "/World", "components": {"Environment": {
            "Environment": None, "FallbackColor": [0.5, 0.5, 0.5]}}})
        fallback = client.call("viewport.screenshot", {"view": "game", "width": 64, "height": 36})
        _, _, fallback_pixels = png_pixels(Path(fallback["path"]))
        fallback_red, fallback_blue = fallback_pixels[centre], fallback_pixels[centre + 2]
        self.assertLess(abs(fallback_red - fallback_blue), (red - blue) // 2, (fallback_red, fallback_blue, red, blue))
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    def test_exr_environment_imports_and_shows_its_skybox_with_either_compression(self) -> None:
        if not self.require_gpu():
            return
        client, root = self.open_editor_with_scene(renderer="vulkan")
        source = self.directory / "Sources" / "Orange.exr"
        write_exr(source, 64, (2.0, 0.5, 0.125))
        environment = self.import_environment(client, source)
        info = client.call("asset.info", {"asset": environment["id"]})
        self.assertEqual(info["importer"], "Environment")
        self.assertEqual(info["importerVersion"], 2)
        self.assertEqual(info["settings"]["Compression"], "Auto")
        self.assertEqual(info["diagnostics"], [])

        self.set_environment(client, environment["id"])
        shots: list[bytes] = []
        for compression in ("Auto", "None"):
            if compression == "None":
                changed = client.call("asset.setImportSettings",
                                      {"asset": environment["id"], "settings": {"Compression": "None"}})
                self.assertEqual(changed["settings"]["Compression"], "None")
                meta = root / "Assets" / "Environments" / "Orange.exr.meta"
                self.assertEqual(read_meta_settings(meta)["Compression"], "None")
                client.call("asset.reimport", {"asset": environment["id"]})
                self.assertEqual(client.call("asset.info", {"asset": environment["id"]})["diagnostics"], [])
            shot = client.call("viewport.screenshot", {"view": "game", "width": 64, "height": 36})
            width, height, pixels = png_pixels(Path(shot["path"]))
            centre = ((height // 2) * width + width // 2) * 4
            red, green, blue = pixels[centre], pixels[centre + 1], pixels[centre + 2]
            # The skybox shows the environment's orange (red above green above blue), from BC6H and RGBA16F cubes alike.
            self.assertGreater(red, green, (compression, red, green, blue))
            self.assertGreater(green, blue, (compression, red, green, blue))
            shots.append(pixels)
        differences = [abs(left - right) for left, right in zip(shots[0], shots[1])]
        self.assertLessEqual(max(differences), 8)
        client.call("session.shutdown", {"force": True})
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    def test_renderer_none_environment_import_uses_cached_bake(self) -> None:
        if not self.require_gpu():
            return
        # A rendering editor imports and bakes the environment into the project's cache.
        client, root = self.open_editor_with_scene(renderer="vulkan")
        source = self.directory / "Sources" / "Orange.hdr"
        write_hdr(source, 64, (2.0, 0.5, 0.125))
        environment = self.import_environment(client, source)
        self.set_environment(client, environment["id"])
        client.call("scene.save")
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

        # An editor without a device opens the project. Even a reimport, which bypasses the cache, is served from the
        # cooked bake, since the bake cannot be redone here; nothing is reported.
        client = self.connect(self.start_editor(project=root))
        client.call("asset.reimport", {"asset": environment["id"]})
        info = client.call("asset.info", {"asset": environment["id"]})
        self.assertEqual(info["asset"]["type"], "Environment")
        self.assertEqual(info["state"], "Loaded", info)
        report = client.call("project.validate", {"scope": "project"})
        codes = [diagnostic["code"] for diagnostic in report["diagnostics"]]
        self.assertNotIn("ASSET_IMPORT_FAILED", codes, report)

        # A second copy of the same bytes has the same cache key (§7.5) and is served from the bake as well.
        copy = self.directory / "Sources" / "Copy.hdr"
        copy.write_bytes(source.read_bytes())
        copied = self.import_environment(client, copy)
        self.assertEqual(client.call("asset.info", {"asset": copied["id"]})["state"], "Loaded")
        report = client.call("project.validate", {"scope": "project"})
        self.assertNotIn("ASSET_IMPORT_FAILED", [diagnostic["code"] for diagnostic in report["diagnostics"]], report)

        # An environment no cache holds cannot be baked here: World Tree brief 14 defers its bake to the next editor
        # with a GPU, so the import succeeds with that warning and the GPU hint, and no asset error is reported.
        other = self.directory / "Sources" / "Blue.hdr"
        write_hdr(other, 64, (0.1, 0.2, 1.0))
        imported = client.call("asset.import", {"source": str(other), "destDir": "Assets/Environments"})
        deferred = [diagnostic for diagnostic in imported["diagnostics"]
                    if diagnostic["code"] == "ASSET_BAKE_DEFERRED"]
        self.assertEqual(len(deferred), 1, imported)
        self.assertEqual(deferred[0]["severity"], "Warning")
        self.assertEqual(deferred[0]["hint"], GPU_HINT)
        report = client.call("project.validate", {"scope": "project"})
        self.assertNotIn("ASSET_IMPORT_FAILED", [diagnostic["code"] for diagnostic in report["diagnostics"]], report)
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

    def test_renderer_none_environment_import_defers_its_bake_and_its_scene_plays_and_tests(self) -> None:
        # World Tree brief 14 (ADR 0042 decision 2): an editor without a graphics device imported an HDR it could not
        # bake as an asset error, which then refused play.start of every scene of the project. The import now succeeds
        # with a warning that defers the bake to the next editor with a GPU, and the scene that uses the environment
        # plays, records and replays here.
        client, root = self.open_editor_with_scene()
        source = self.directory / "Sources" / "Blue.hdr"
        write_hdr(source, 64, (0.1, 0.2, 1.0))
        imported = client.call("asset.import", {"source": str(source), "destDir": "Assets/Environments"})
        environment = imported["asset"]
        self.assertEqual(environment["type"], "Environment", imported)
        self.assertTrue((root / "Assets" / "Environments" / "Blue.hdr.meta").is_file())
        codes = [diagnostic["code"] for diagnostic in imported["diagnostics"]]
        self.assertEqual(codes, ["ASSET_BAKE_DEFERRED"], imported)
        warning = imported["diagnostics"][0]
        self.assertEqual(warning["severity"], "Warning")
        self.assertEqual(warning["path"], "Assets/Environments/Blue.hdr")
        self.assertIn("deferred to the next editor with a GPU", warning["message"])
        self.assertEqual(warning["hint"], GPU_HINT)
        info = client.call("asset.info", {"asset": environment["id"]})
        self.assertEqual(info["state"], "Unloaded", info)
        self.assertEqual([diagnostic["code"] for diagnostic in info["diagnostics"]], ["ASSET_BAKE_DEFERRED"], info)
        # The deferral is no asset error, nor a project.validate code: it describes this editor, not the project.
        report = client.call("project.validate", {"scope": "project"})
        report_codes = [diagnostic["code"] for diagnostic in report["diagnostics"]]
        self.assertNotIn("ASSET_IMPORT_FAILED", report_codes, report)
        self.assertNotIn("ASSET_BAKE_DEFERRED", report_codes, report)

        # The scene that uses the environment plays.
        self.set_environment(client, environment["id"])
        client.call("entity.create", {"name": "Ball", "components": {
            "Transform": {"Translation": [0, 3, -5]}, "RigidBody": {"Type": "Dynamic"},
            "SphereCollider": {"Radius": 0.5}}})
        client.call("scene.save")
        client.call("play.start", {"lockstep": True})
        stepped = client.call("play.step", {"ticks": 30})
        self.assertEqual(stepped["tick"], 30)
        client.call("play.stop")

        # Its suite runs and records a replay, which then verifies at its hashes.
        client.call("script.write", {"path": "Assets/Tests/Sky.test.luau", "source": (
            'return Test.Suite("Sky", function()\n'
            'Test.Case("the scene plays under the unbaked sky", function()\n'
            'Test.WaitTicks(30)\n'
            'Test.Expect(Scene.FindByName("World") ~= nil)\n'
            'Test.Expect(Scene.FindByName("Ball") ~= nil)\n'
            'end)\nend)')})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [
            {"Script": "Assets/Tests/Sky.test.luau", "Scene": "Assets/Scenes/Main.scene", "Modes": ["Editor"]}]}}})
        replay = "Assets/Tests/Replays/Sky.replay"
        recorded = engine_client.load_offloaded(client.call("test.run", {"record": replay}, timeout=120.0))
        self.assertTrue(recorded["passed"], recorded)
        self.assertTrue((root / replay).is_file())
        client.call("project.setSettings", {"patch": {"Testing": {"Replays": [replay]}}})
        verified = engine_client.load_offloaded(client.call("test.run", timeout=120.0))
        self.assertTrue(verified["passed"], verified)
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())


if __name__ == "__main__":
    unittest.main()
