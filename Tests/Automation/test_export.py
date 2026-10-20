"""The exporter through automation (Docs/Architecture.md §7.6, §14.1, §14.2, §13.5 project.export; Roadmap M7
acceptance): the package layout, the app-local CRT, deterministic paks, the output directory and the smoke test. The
exports use the Runtime of the configuration the suite's editor was built in (harness.automation_config), which
Scripts/Build.py builds with its Redist DLLs. The exported game's own behaviour is test_runtime.py's.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import sys
import unittest
from pathlib import Path
from typing import Any

from harness import (EXIT_SUCCESS, GPU_EDITOR_ARGUMENTS, AutomationTestCase, automation_config, engine_client,
                     runtime_executable)
from tiny_game import TINY_GAME_NAME, TINY_SCENE, WINDOW_HEIGHT, WINDOW_WIDTH, build_tiny_game, export_tiny_game

CRT_FILES = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")
NOTICES_FILE = "THIRD_PARTY_NOTICES.txt"
# The hint of an environment that needs a bake no cache holds (Docs/Decisions/0013-m8-decisions.md decision 9).
ENVIRONMENT_GPU_HINT = "start the editor with a GPU once to bake this environment"
# The version strings Windows reads from an executable's VS_VERSIONINFO (§14.2 step 5).
VERSION_KEYS = ("CompanyName", "FileDescription", "FileVersion", "InternalName", "OriginalFilename", "ProductName",
                "ProductVersion")


def platform_name() -> str:
    """Exporter::GetPlatformName: the host, the only platform an export targets."""
    if sys.platform == "win32":
        return "Windows"
    if sys.platform == "darwin":
        return "macOS"
    return "Linux"


def package_resources(output: Path, name: str = TINY_GAME_NAME) -> Path:
    """Where Game.json, Data/ and the notices are in the package at `output`: the bundle's Contents/Resources on macOS
    (EditorCore/Export/AppBundle.h), the package directory elsewhere."""
    return output / f"{name}.app" / "Contents" / "Resources" if sys.platform == "darwin" else output


def read_version_strings(executable: Path) -> dict[str, str]:
    """The \\StringFileInfo\\040904B0 strings of a Windows executable, read with Windows' own GetFileVersionInfoW and
    VerQueryValueW (the resource probe of Docs/Decisions/0024-m15-contract.md decision 4); empty without a version
    resource."""
    import ctypes  # noqa: PLC0415  (Windows only)
    from ctypes import wintypes  # noqa: PLC0415

    version = ctypes.WinDLL("version")
    version.GetFileVersionInfoSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p)
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(wintypes.UINT))
    version.VerQueryValueW.restype = wintypes.BOOL
    size = version.GetFileVersionInfoSizeW(str(executable), None)
    if size == 0:
        return {}
    block = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(executable), 0, size, block):
        raise OSError(f"GetFileVersionInfoW failed for {executable}")
    strings: dict[str, str] = {}
    for key in VERSION_KEYS:
        value = ctypes.c_void_p()
        length = wintypes.UINT()
        found = version.VerQueryValueW(block, f"\\StringFileInfo\\040904B0\\{key}", ctypes.byref(value),
                                       ctypes.byref(length))
        if found and value.value and length.value:
            strings[key] = ctypes.wstring_at(value.value, length.value).split("\0", 1)[0]
    return strings


def environment_warnings(report: dict[str, Any]) -> list[str]:
    """The warnings of an export report that mention environments."""
    return [warning for warning in report["warnings"] if "environment" in warning.lower()]


def read_pak_toc(pak: Path) -> dict[str, Any]:
    """The TOC of the pak at `pak` (Engine/Asset/PakFormat.h): the 64-byte little-endian header's TocOffset and TocSize
    name the canonical JSON that ends the file."""
    data = pak.read_bytes()
    magic, _version, _count, toc_offset, toc_size, _toc_hash = struct.unpack_from("<8sIIQQQ", data, 0)
    if magic != b"ENGPAK01":
        raise ValueError(f"{pak} is not a pak")
    return json.loads(data[toc_offset:toc_offset + toc_size].decode("utf-8"))


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
        resources = package_resources(output)
        self.assertEqual(executable.stem, TINY_GAME_NAME)
        self.assertTrue(executable.is_file())
        runtime = runtime_executable(automation_config())
        if sys.platform == "win32":
            # The copied Runtime carries the project's version resource (§14.2 step 5; no Export.Icon, so no icon
            # group).
            self.assertEqual(executable.parent, output)
            self.assertEqual(read_version_strings(executable), {
                "FileDescription": TINY_GAME_NAME, "FileVersion": "1.0.0.0", "InternalName": TINY_GAME_NAME,
                "OriginalFilename": f"{TINY_GAME_NAME}.exe", "ProductName": TINY_GAME_NAME,
                "ProductVersion": "1.0.0.0"})
            self.assertEqual(read_version_strings(runtime), {})
            for name in CRT_FILES:
                self.assertTrue((output / name).is_file(), name)
        elif sys.platform == "darwin":
            # The output directory holds only the bundle (§14.1, AppBundle.h).
            self.assertEqual([entry.name for entry in output.iterdir()], [f"{TINY_GAME_NAME}.app"])
            self.assertEqual(executable, output / f"{TINY_GAME_NAME}.app" / "Contents" / "MacOS" / TINY_GAME_NAME)
            info = (output / f"{TINY_GAME_NAME}.app" / "Contents" / "Info.plist").read_text(encoding="utf-8")
            self.assertIn(f"<string>{TINY_GAME_NAME}</string>", info)
            for framework in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                self.assertTrue((output / f"{TINY_GAME_NAME}.app" / "Contents" / "Frameworks" / framework).is_file())
        else:
            # Linux executables carry no resources (§16): the Runtime is copied unchanged.
            self.assertEqual(executable.parent, output)
            self.assertEqual(executable.read_bytes(), runtime.read_bytes())
        self.assertTrue((resources / "Data" / "Engine.pak").is_file())
        self.assertTrue((resources / "Data" / "Game.pak").is_file())
        # The notices Scripts/Build.py generated beside the Runtime (Docs/Decisions/0024-m15-contract.md decision 8).
        self.assertEqual((resources / NOTICES_FILE).read_bytes(), (runtime.parent / NOTICES_FILE).read_bytes())
        manifest = json.loads((resources / "Game.json").read_text(encoding="utf-8"))
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
        prefix = f"{TINY_GAME_NAME}.app/Contents/Resources/" if sys.platform == "darwin" else ""
        self.assertIn(f"{prefix}Game.json", listed)
        self.assertIn(f"{prefix}Data/Game.pak", listed)
        self.assertIn(f"{prefix}{NOTICES_FILE}", listed)
        for path, entry in listed.items():
            self.assertEqual(entry["size"], (output / path).stat().st_size, path)
            self.assertRegex(entry["hash"], r"^[0-9a-f]{16}$")
        self.assertFalse(report["smokeTestRan"])
        # Docs/Decisions/0026-m16-contract.md, friction S2-06: neither an exit code nor a SmokeTest timing without one.
        self.assertIsNone(report["smokeTestExitCode"])
        self.assertNotIn("SmokeTest", [timing["phase"] for timing in report["phaseTimings"]])
        # Nothing but the output directory is left beside it: the staging directory became the output.
        self.assertEqual([entry.name for entry in output.parent.iterdir()], [TINY_GAME_NAME])

    def test_export_is_deterministic(self) -> None:
        client, _ = self.open_tiny_game()
        first = export_tiny_game(client)
        output = Path(first["outputDirectory"])
        resources = package_resources(output)
        digests = {name: hashlib.sha256((resources / "Data" / name).read_bytes()).hexdigest()
                   for name in ("Engine.pak", "Game.pak")}
        manifest = (resources / "Game.json").read_bytes()
        executable = Path(first["executable"]).read_bytes()
        second = export_tiny_game(client)
        self.assertEqual(second["outputDirectory"], first["outputDirectory"])
        for name, digest in digests.items():
            self.assertEqual(hashlib.sha256((resources / "Data" / name).read_bytes()).hexdigest(), digest, name)
        self.assertEqual((resources / "Game.json").read_bytes(), manifest)
        # The patched executable too (§14.2 step 5 writes no timestamp).
        self.assertEqual(Path(second["executable"]).read_bytes(), executable)
        self.assertEqual(second["files"], first["files"])

    def test_export_into_out_dir_replaces_the_earlier_export(self) -> None:
        # An earlier export is a directory whose Game.json loads as a game manifest (Docs/Decisions/0024-m15-contract.md
        # decision 2, the M15 review's M01), so the first export makes a real one there.
        client, root = self.open_tiny_game()
        output = root / "Build" / "Shipping" / TINY_GAME_NAME
        client.call("project.export", {"config": automation_config(), "outDir": "Build/Shipping/TinyGame"})
        self.assertTrue((package_resources(output) / "Game.json").is_file())
        (output / "Old.txt").write_text("an earlier export", encoding="utf-8")
        report = client.call("project.export", {"config": automation_config(), "outDir": "Build/Shipping/TinyGame"})
        self.assertEqual(Path(report["outputDirectory"]), output)
        self.assertFalse((output / "Old.txt").exists())
        self.assertTrue(Path(report["executable"]).is_file())
        self.assertEqual([entry.name for entry in output.parent.iterdir()], [TINY_GAME_NAME])

    def test_export_never_replaces_a_directory_without_an_earlier_export(self) -> None:
        # Docs/Decisions/0024-m15-contract.md decision 2: an export replaces its output directory as a whole, so an
        # existing one must be empty or hold an earlier export; anything else is refused before anything is written.
        client, root = self.open_tiny_game()
        output = root / "Build" / "Notes"
        output.mkdir(parents=True)
        (output / "Todo.txt").write_text("keep me", encoding="utf-8")
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.export", {"config": automation_config(), "outDir": "Build/Notes"})
        self.assert_engine_error(refused.exception, engine_client.INVALID_PARAMS)
        self.assertIn(output.as_posix(), refused.exception.detail.replace("\\", "/"))
        self.assertEqual((output / "Todo.txt").read_text(encoding="utf-8"), "keep me")
        self.assertEqual([entry.name for entry in output.parent.iterdir()], ["Notes"])
        # A Game.json that is not a game manifest proves no earlier export (the M15 review's M01).
        manifest = package_resources(output) / "Game.json"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text("{}", encoding="utf-8")
        with self.assertRaises(engine_client.EngineError) as unproven:
            client.call("project.export", {"config": automation_config(), "outDir": "Build/Notes"})
        self.assert_engine_error(unproven.exception, engine_client.INVALID_PARAMS)
        self.assertEqual((output / "Todo.txt").read_text(encoding="utf-8"), "keep me")
        self.assertEqual(manifest.read_text(encoding="utf-8"), "{}")
        # An empty directory is fine.
        shutil.rmtree(output)
        output.mkdir()
        report = client.call("project.export", {"config": automation_config(), "outDir": "Build/Notes"})
        self.assertEqual(Path(report["outputDirectory"]), output)

    def test_export_smoke_test_runs_the_exported_game(self) -> None:
        # The suite's editor has no device (--renderer none), so a Debug or Release smoke test passes --renderer none.
        client, _ = self.open_tiny_game()
        report = export_tiny_game(client, smoke_test=True)
        self.assertTrue(report["smokeTestRan"])
        self.assertEqual(report["smokeTestExitCode"], 0)
        self.assertIn("SmokeTest", [timing["phase"] for timing in report["phaseTimings"]])
        # The smoke test ran on the staged package: nothing it wrote is in the package, and its user-data directory is
        # gone too.
        output = Path(report["outputDirectory"])
        if sys.platform == "darwin":
            self.assertEqual({entry.name for entry in package_resources(output).iterdir()},
                             {"Game.json", "Data", NOTICES_FILE, "vulkan"})
        else:
            self.assertEqual({entry.name for entry in output.iterdir()} - set(CRT_FILES),
                             {Path(report["executable"]).name, "Game.json", "Data", NOTICES_FILE})
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
        # Friction S2-06: asked for but not run reads as not run, not as exit code 0.
        self.assertIsNone(report["smokeTestExitCode"])
        self.assertNotIn("SmokeTest", [timing["phase"] for timing in report["phaseTimings"]])

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

    @unittest.skip("contract stub: Engine.pak's built-ins pruned and the reachability report (Docs/Decisions/"
                   "0030-m19-contract.md decisions 19 and 20, B-PRUNE-ENG, stream D)")
    def test_engine_pak_holds_only_the_built_ins_the_game_reaches(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decisions 19 and 20 (B-PRUNE-ENG): Engine.pak holds the built-ins the
        # exported game asks for by handle and those its assets reach, here the Coin preset through a script field and
        # the Hit preset through a literal Assets.Load; the environments and the other presets stay out, and
        # project.export's reachability says why each built-in is there.
        client, _ = self.open_tiny_game()
        client.call("script.write", {"path": "Assets/Scripts/Sounds.luau", "source":
                    'local Sounds = {Fields = {Clip = Field.Asset("AudioClip")}}; '
                    'function Sounds.OnStart(self: any) local hit = Assets.Load("engine://Audio/Hit"); '
                    'if hit ~= nil and self.Clip ~= nil then Audio.PlayOneShot(hit) end end; '
                    'return Script.Define("Sounds", Sounds)'})
        client.call("entity.update", {"entity": "/Cube", "components": {
            "Script": {"Script": "Assets/Scripts/Sounds.luau", "Fields": {"Clip": "engine://Audio/Coin"}}}})
        client.call("scene.save")
        report = export_tiny_game(client)
        reachability = report["reachability"]
        self.assertFalse(reachability["pruned"])
        self.assertEqual(reachability["computedLoads"], [])
        builtins = {entry["path"]: entry for entry in reachability["engineBuiltins"]}
        self.assertEqual(list(builtins), ["engine://Audio/Coin", "engine://Audio/Hit", "engine://Fonts/Default",
                                          "engine://Textures/BlueNoise"])
        self.assertEqual(builtins["engine://Textures/BlueNoise"]["reason"], "Required")
        self.assertEqual(builtins["engine://Fonts/Default"]["reason"], "Required")
        self.assertEqual((builtins["engine://Audio/Coin"]["reason"], builtins["engine://Audio/Coin"]["from"]),
                         ("Dependency", TINY_SCENE))
        hit = builtins["engine://Audio/Hit"]
        self.assertEqual((hit["reason"], hit["from"], hit["line"]), ("LiteralLoad", "Assets/Scripts/Sounds.luau", 1))
        for pruned in ("engine://Audio/Win", "engine://Environments/Sky", "engine://Environments/Studio"):
            self.assertIn(pruned, reachability["prunedBuiltins"])
        self.assertEqual(environment_warnings(report), [])
        toc = read_pak_toc(package_resources(Path(report["outputDirectory"])) / "Data" / "Engine.pak")
        packaged = sorted(entry["Path"] for entry in toc["Entries"] if entry["Handle"] is not None)
        self.assertEqual(packaged, list(builtins))

    @unittest.skip("contract stub: Export.PruneUnreferenced and SCRIPT_COMPUTED_ASSET_LOAD (Docs/Decisions/"
                   "0030-m19-contract.md decision 21, B-PRUNE-GAME, stream R)")
    def test_pruned_export_packages_what_the_start_scene_reaches_and_runs(self) -> None:
        # Docs/Decisions/0030-m19-contract.md decision 21 (B-PRUNE-GAME): with Export.PruneUnreferenced,
        # project.validate names the computed Assets.Load, Game.pak holds only what the start scene reaches,
        # project.export reports why each asset is there and what it left out, and the pruned game passes its smoke
        # test.
        client, _ = self.open_tiny_game()
        for name in ("Used", "Unused"):
            client.call("asset.create", {"type": "Material", "path": f"Assets/Materials/{name}.material"})
        client.call("script.write", {"path": "Assets/Scripts/Loader.luau", "source":
                    'local Loader = {}; function Loader.OnStart(self: any) local name = "Unused"; '
                    'local look = Assets.Load("Assets/Materials/" .. name .. ".material"); '
                    'if look ~= nil then Log.Info("loaded") end end; return Script.Define("Loader", Loader)'})
        client.call("entity.update", {"entity": "/Cube", "components": {
            "MeshRenderer": {"Materials": ["Assets/Materials/Used.material"]},
            "Script": {"Script": "Assets/Scripts/Loader.luau"}}})
        client.call("scene.save")

        def computed_loads() -> list[dict[str, Any]]:
            return [diagnostic for diagnostic in client.call("project.validate", {})["diagnostics"]
                    if diagnostic["code"] == "SCRIPT_COMPUTED_ASSET_LOAD"]

        self.assertEqual(computed_loads(), [])
        client.call("project.setSettings", {"patch": {"Export": {"PruneUnreferenced": True}}})
        found = computed_loads()
        self.assertEqual(len(found), 1)
        self.assertEqual((found[0]["file"], found[0]["line"], found[0]["severity"]),
                         ("Assets/Scripts/Loader.luau", 1, "Warning"))

        report = export_tiny_game(client, smoke_test=True)
        reachability = report["reachability"]
        self.assertTrue(reachability["pruned"])
        packaged = {entry["path"]: entry for entry in reachability["gameAssets"]}
        self.assertEqual(list(packaged), ["Assets/Materials/Used.material", "Assets/Scenes/Main.scene",
                                          "Assets/Scripts/Loader.luau"])
        self.assertEqual(packaged["Assets/Scenes/Main.scene"]["reason"], "StartScene")
        for path in ("Assets/Materials/Used.material", "Assets/Scripts/Loader.luau"):
            self.assertEqual((packaged[path]["reason"], packaged[path]["from"]), ("Dependency", TINY_SCENE), path)
        self.assertEqual(reachability["prunedAssets"], ["Assets/Materials/Unused.material"])
        self.assertEqual([(load["script"], load["line"]) for load in reachability["computedLoads"]],
                         [("Assets/Scripts/Loader.luau", 1)])
        self.assertEqual(report["gameAssetCount"], 3)
        self.assertTrue(report["smokeTestRan"])
        self.assertEqual(report["smokeTestExitCode"], EXIT_SUCCESS)

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
        # The testing param's checks are test_testing_export.py's (stream B, Docs/Decisions/0024-m15-contract.md).
        refusals = (({"config": "Release", "outDir": "Assets/Game"}, engine_client.INVALID_PARAMS, "/outDir"),
                    ({"config": "Release", "outDir": "Build"}, engine_client.INVALID_PARAMS, "/outDir"))
        for params, code, pointer in refusals:
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as refused:
                    client.call("project.export", params)
                self.assert_engine_error(refused.exception, code)
                self.assertEqual(refused.exception.issues[0]["pointer"], pointer)


if __name__ == "__main__":
    unittest.main()
