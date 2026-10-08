"""The exporter through automation (Docs/Architecture.md §7.6, §14.1, §14.2, §13.5 project.export; Roadmap M7
acceptance): the package layout, the app-local CRT, deterministic paks, the output directory and the smoke test. The
exports use the Runtime of the configuration the suite's editor was built in (harness.automation_config), which
Scripts/Build.py builds with its Redist DLLs. The exported game's own behaviour is test_runtime.py's.
"""

from __future__ import annotations

import hashlib
import json
import sys
import unittest
from pathlib import Path
from typing import Any

from harness import (EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, automation_config, engine_client,
                     runtime_executable)
from tiny_game import TINY_GAME_NAME, TINY_SCENE, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game

CRT_FILES = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")
# The hint of an environment that needs a bake no cache holds (Docs/Decisions/0013-m8-decisions.md decision 9).
ENVIRONMENT_GPU_HINT = "start the editor with a GPU once to bake this environment"


def platform_name() -> str:
    """Exporter::GetPlatformName: the host, the only platform an export targets."""
    if sys.platform == "win32":
        return "Windows"
    if sys.platform == "darwin":
        return "macOS"
    return "Linux"


def environment_warnings(report: dict[str, Any]) -> list[str]:
    """The warnings of an export report that mention environments."""
    return [warning for warning in report["warnings"] if "environment" in warning.lower()]


class ExportTests(AutomationTestCase):
    """Exports of the tiny game."""

    def open_tiny_game(self) -> tuple[engine_client.EngineClient, Path]:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        return client, root

    def test_export_tiny_project(self) -> None:
        client, root = self.open_tiny_game()
        report = export_tiny_game(client)
        output = Path(report["outputDirectory"])
        self.assertTrue(output.is_relative_to(root / "Build"))
        self.assertEqual(output, root / "Build" / f"{platform_name()}-{automation_config()}" / TINY_GAME_NAME)
        self.assertEqual(report["config"], automation_config())
        executable = Path(report["executable"])
        self.assertEqual(executable.parent, output)
        self.assertEqual(executable.stem, TINY_GAME_NAME)
        self.assertTrue(executable.is_file())
        self.assertEqual(executable.read_bytes(), runtime_executable(automation_config()).read_bytes())
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
        self.assertEqual(manifest["StartScene"], client.call("asset.info", {"asset": TINY_SCENE})["asset"]["id"])
        self.assertEqual((manifest["Window"]["Width"], manifest["Window"]["Height"]), (WINDOW_WIDTH, WINDOW_HEIGHT))
        self.assertFalse(manifest["Testing"])
        self.assertGreater(report["gameAssetCount"], 0)
        self.assertGreater(report["engineEntryCount"], 0)
        listed = {entry["path"]: entry for entry in report["files"]}
        self.assertEqual(list(listed), sorted(listed))
        self.assertIn("Game.json", listed)
        self.assertIn("Data/Game.pak", listed)
        for path, entry in listed.items():
            self.assertEqual(entry["size"], (output / path).stat().st_size, path)
            self.assertRegex(entry["hash"], r"^[0-9a-f]{16}$")
        self.assertFalse(report["smokeTestRan"])
        # Nothing but the output directory is left beside it: the staging directory became the output.
        self.assertEqual([entry.name for entry in output.parent.iterdir()], [TINY_GAME_NAME])

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
        self.assertEqual(second["files"], first["files"])

    def test_export_into_out_dir_replaces_the_earlier_export(self) -> None:
        client, root = self.open_tiny_game()
        output = root / "Build" / "Shipping" / TINY_GAME_NAME
        output.mkdir(parents=True)
        (output / "Old.txt").write_text("an earlier export", encoding="utf-8")
        report = client.call("project.export", {"config": automation_config(), "outDir": "Build/Shipping/TinyGame"})
        self.assertEqual(Path(report["outputDirectory"]), output)
        self.assertFalse((output / "Old.txt").exists())
        self.assertTrue(Path(report["executable"]).is_file())
        self.assertEqual([entry.name for entry in output.parent.iterdir()], [TINY_GAME_NAME])

    def test_export_smoke_test_runs_the_exported_game(self) -> None:
        # The suite's editor has no device (--renderer none), so a Debug or Release smoke test passes --renderer none.
        client, _ = self.open_tiny_game()
        report = export_tiny_game(client, smoke_test=True)
        self.assertTrue(report["smokeTestRan"])
        self.assertEqual(report["smokeTestExitCode"], 0)
        # The smoke test ran on the staged package: nothing it wrote is in the package, and its user-data directory is
        # gone too.
        output = Path(report["outputDirectory"])
        self.assertEqual({entry.name for entry in output.iterdir()} - set(CRT_FILES),
                         {Path(report["executable"]).name, "Game.json", "Data"})
        self.assertEqual([entry.name for entry in output.parent.iterdir()], [TINY_GAME_NAME])

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

    def test_export_with_renderer_none_uses_prebaked_engine_cache(self) -> None:
        # §7.5, §7.6 (Roadmap M8; Docs/Decisions/0013-m8-decisions.md decision 9): without a device and without a bake,
        # Engine.pak leaves the built-in environments out with a warning, and an export whose scenes reference one fails
        # with the GPU hint; once a GPU run of --bake-engine-assets filled the engine cooked cache, an editor without a
        # device exports them from it.
        if not self.require_gpu():
            return
        client, root = self.open_tiny_game()  # --renderer none, this test's empty engine cache directory
        unbaked = export_tiny_game(client)
        self.assertTrue(environment_warnings(unbaked), unbaked["warnings"])
        client.call("entity.update", {"entity": "/Camera", "components": {"Camera": {"Clear": "Skybox"}}})
        client.call("entity.create", {"name": "World", "components": {
            "Environment": {"Environment": "engine://Environments/Studio"}}})
        client.call("scene.save")
        with self.assertRaises(engine_client.EngineError) as raised:
            export_tiny_game(client)
        self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED)
        refusal = json.dumps(raised.exception.issues)
        self.assertIn("engine://Environments/Studio", refusal)
        self.assertIn(ENVIRONMENT_GPU_HINT, refusal)
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), EXIT_SUCCESS, self.editors[-1].output())

        bake = ["--headless", "--renderer", "vulkan", *GPU_EDITOR_ARGUMENTS, "--bake-engine-assets"]
        code, output = self.run_editor(bake, timeout=600.0)
        self.assertEqual(code, EXIT_SUCCESS, output)
        self.assertIn("Engine assets:", output)
        engine_cache = self.user_data / "EngineCache"
        self.assertTrue(any(path.is_dir() for path in engine_cache.glob("0000000000000201")), output)

        client = self.connect(self.start_editor(project=root))  # --renderer none, the same engine cache directory
        baked = export_tiny_game(client)
        self.assertFalse(environment_warnings(baked), baked["warnings"])
        # Engine.pak holds the two built-in environments (Studio and Sky) besides what the unbaked export had.
        self.assertEqual(baked["engineEntryCount"], unbaked["engineEntryCount"] + 2)

    def test_export_validates_the_project_and_its_params(self) -> None:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        client.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        # No StartScene yet: validation fails and nothing is written.
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("project.export", {"config": automation_config()})
        self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED)
        self.assertEqual([issue["pointer"] for issue in raised.exception.issues], ["/StartScene"])
        self.assertFalse((root / "Build").exists())
        refusals = (({"config": "Release", "outDir": "Assets/Game"}, engine_client.INVALID_PARAMS, "/outDir"),
                    ({"config": "Release", "outDir": "Build"}, engine_client.INVALID_PARAMS, "/outDir"),
                    ({"config": "Release", "testing": True}, engine_client.UNSUPPORTED, "/testing"))
        for params, code, pointer in refusals:
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("project.export", params)
                self.assert_engine_error(refused.exception, code)
                self.assertEqual(refused.exception.issues[0]["pointer"], pointer)


if __name__ == "__main__":
    unittest.main()
