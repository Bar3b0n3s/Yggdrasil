"""The exporter through automation (Docs/Architecture.md §7.6, §14.1, §14.2, §13.5 project.export; Roadmap M7
acceptance): the package layout, the app-local CRT, deterministic paks and the smoke test. The exports use the Runtime
of the configuration the suite's editor was built in (harness.automation_config), which Scripts/Build.py builds with
its Redist DLLs.

Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 14): stream D implements and
registers project.export and removes the skips.
"""

from __future__ import annotations

import hashlib
import json
import sys
import unittest
from pathlib import Path

from harness import AutomationTestCase, automation_config, engine_client, runtime_executable
from tiny_game import TINY_GAME_NAME, build_tiny_game, export_tiny_game

CRT_FILES = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")


class ExportTests(AutomationTestCase):
    """Exports of the tiny game."""

    def open_tiny_game(self) -> tuple[engine_client.EngineClient, Path]:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        return client, root

    @unittest.skip("contract stub: un-skipped by M7 stream D")
    def test_export_tiny_project(self) -> None:
        client, root = self.open_tiny_game()
        report = export_tiny_game(client)
        output = Path(report["outputDirectory"])
        self.assertTrue(output.is_relative_to(root / "Build"))
        executable = Path(report["executable"])
        self.assertEqual(executable.parent, output)
        self.assertEqual(executable.stem, TINY_GAME_NAME)
        self.assertTrue(executable.is_file())
        self.assertTrue((output / "Data" / "Engine.pak").is_file())
        self.assertTrue((output / "Data" / "Game.pak").is_file())
        if sys.platform == "win32":
            for name in CRT_FILES:
                self.assertTrue((output / name).is_file(), name)
        manifest = json.loads((output / "Game.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["Format"], "GameManifest")
        self.assertEqual(manifest["Name"], TINY_GAME_NAME)
        self.assertEqual([pak["Path"] for pak in manifest["Paks"]], ["Data/Engine.pak", "Data/Game.pak"])
        for pak in manifest["Paks"]:
            self.assertRegex(pak["XXH64"], r"^[0-9a-f]{16}$")
        self.assertGreater(report["gameAssetCount"], 0)
        listed = {entry["path"] for entry in report["files"]}
        self.assertIn("Game.json", listed)
        self.assertIn("Data/Game.pak", listed)
        self.assertFalse(report["smokeTestRan"])

    @unittest.skip("contract stub: un-skipped by M7 stream D")
    def test_export_is_deterministic(self) -> None:
        client, _ = self.open_tiny_game()
        first = export_tiny_game(client)
        output = Path(first["outputDirectory"])
        digests = {name: hashlib.sha256((output / "Data" / name).read_bytes()).hexdigest()
                   for name in ("Engine.pak", "Game.pak")}
        manifest = (output / "Game.json").read_bytes()
        second = export_tiny_game(client)
        self.assertEqual(second["outputDirectory"], first["outputDirectory"])
        for name, digest in digests.items():
            self.assertEqual(hashlib.sha256((output / "Data" / name).read_bytes()).hexdigest(), digest, name)
        self.assertEqual((output / "Game.json").read_bytes(), manifest)

    @unittest.skip("contract stub: un-skipped by M7 stream D")
    def test_export_smoke_test_runs_the_exported_game(self) -> None:
        # The suite's editor has no device (--renderer none), so a Debug or Release smoke test passes --renderer none.
        client, _ = self.open_tiny_game()
        report = export_tiny_game(client, smoke_test=True)
        self.assertTrue(report["smokeTestRan"])
        self.assertEqual(report["smokeTestExitCode"], 0)
        # The smoke test ran on the staged package: nothing it wrote is in the package.
        output = Path(report["outputDirectory"])
        self.assertEqual({entry.name for entry in output.iterdir()} - set(CRT_FILES),
                         {Path(report["executable"]).name, "Game.json", "Data"})

    @unittest.skip("contract stub: un-skipped by M7 stream D")
    def test_export_dist_smoke_test_without_a_device_is_reported_not_run(self) -> None:
        # A Dist Runtime has no --renderer none and exits 3 without a device, so an editor without one cannot run the
        # Dist smoke test: the export succeeds and says so (Exporter.h step 6). Without a Dist build the export names
        # the build command instead; either way nothing is accepted and ignored.
        client, _ = self.open_tiny_game()
        if not runtime_executable("Dist").is_file():
            with self.assertRaises(engine_client.EngineError) as missing:
                export_tiny_game(client, "Dist", smoke_test=True)
            self.assert_engine_error(missing.exception, engine_client.NOT_FOUND)
            self.assertIn("python Scripts/Build.py --config Dist --project Runtime", missing.exception.detail)
            return
        report = export_tiny_game(client, "Dist", smoke_test=True)
        self.assertFalse(report["smokeTestRan"])
        self.assertTrue(any("smoke test" in warning for warning in report["warnings"]), report["warnings"])

    @unittest.skip("contract stub: un-skipped by M7 stream D")
    def test_export_validates_the_project_and_its_params(self) -> None:
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        # No StartScene yet: validation fails and nothing is written.
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("project.export", {"config": automation_config()})
        self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED)
        refusals = (({"config": "Release", "outDir": "Assets/Game"}, engine_client.INVALID_PARAMS, "/outDir"),
                    ({"config": "Release", "testing": True}, engine_client.UNSUPPORTED, "/testing"))
        for params, code, pointer in refusals:
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("project.export", params)
                self.assert_engine_error(refused.exception, code)
                self.assertEqual(refused.exception.issues[0]["pointer"], pointer)


if __name__ == "__main__":
    unittest.main()
