"""M13 cooked scripting through a real exported executable and its feature-test entry point."""

from __future__ import annotations

import json
import subprocess
import unittest

from harness import EXIT_FAILED, EXIT_SUCCESS, AutomationTestCase, automation_config, engine_client
from tiny_game import TINY_GAME_NAME, TINY_SCENE, build_tiny_game, exported_executable


POWER_BEHAVIOUR = '''--!strict
local Powers = {}
function Powers.OnCreate(self: any)
	local random = Random.New(901)
	local hash = 2166136261
	for _ = 1, 96 do
		local base = random:Number(0.1, 20)
		local exponent = random:Number(-2, 3)
		local text = string.format("%.17g", base ^ exponent)
		for index = 1, #text do
			hash = bit32.bxor(bit32.lrotate(hash, 5), string.byte(text, index))
		end
	end
	self.Entity.Name = "Powers" .. tostring(hash)
end
function Powers.OnFixedUpdate(self: any)
	self.Entity.Transform:Translate(vector.create(1 / 60, 0, 0))
end
return Script.Define("Powers", Powers)
'''

POWER_SUITE = '''--!strict
return Test.Suite("Cooked", function()
	Test.Case("power corpus", function()
		local base = assert(tonumber("2"))
		local exponent = assert(tonumber("1.5"))
		Test.ExpectEqual(base ^ exponent, 2.8284271247461903)
		Test.ExpectEqual(2 ^ 1.5, 2.8284271247461903)
		Test.ExpectEqual(16 ^ 1.25, 32)
		Test.ExpectEqual(16 ^ 0.25, 2)
		Test.WaitTicks(4)
	end)
end)
'''


def export_for_testing(client: engine_client.EngineClient, config: str) -> dict:
    """project.export {config, testing: true} of the open tiny game (Docs/Decisions/0024-m15-contract.md decision 6):
    its Game.json admits --feature-test, and Assets/Tests/** is packaged whatever Export.Exclude says."""
    return client.call("project.export", {"config": config, "testing": True})


class ScriptModeTests(AutomationTestCase):
    def check_modes(self, configurations: tuple[str, ...]) -> dict[str, str]:
        """Compare one authored project in the editor and each requested, already-built runtime configuration."""
        if "Dist" in configurations and not self.require_gpu():
            return {}
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("script.write", {"path": "Assets/Scripts/Powers.luau", "source": POWER_BEHAVIOUR})
        client.call("entity.update", {"entity": "/Cube", "components": {
            "Script": {"Scripts": [{"Script": "Assets/Scripts/Powers.luau"}]}}})
        client.call("script.write", {"path": "Assets/Tests/Cooked.test.luau", "source": POWER_SUITE})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [{
            "Script": "Assets/Tests/Cooked.test.luau", "Scene": TINY_SCENE,
            "Modes": ["Editor", "Release", "Dist"]}]}}})
        client.call("scene.save")
        editor_result = engine_client.load_offloaded(client.call("test.run", timeout=120.0))
        self.assertTrue(editor_result["passed"], editor_result)
        self.assertEqual(len(editor_result["cases"]), 1)
        self.assertEqual(len(editor_result["suites"]), 1)
        expected = editor_result["suites"][0]["finalStateHash"]
        hashes = {"Editor": expected}
        for config in configurations:
            with self.subTest(config=config):
                # A testing export packages the suite below Assets/Tests/ although Export.Exclude leaves it out of a
                # plain export, and the runner loads the ordinary cooked script artifacts.
                executable = exported_executable(export_for_testing(client, config))
                working = self.directory / f"Cooked-{config}"
                working.mkdir()
                arguments = [str(executable), "--headless", "--feature-test", "--filter", "Cooked/power"]
                if config != "Dist":
                    arguments += ["--renderer", "none", "--user-data-dir", str(working / "UserData")]
                process = subprocess.run(arguments, cwd=working, stdin=subprocess.DEVNULL,
                                         capture_output=True, timeout=120.0, check=False)
                self.assertEqual(process.returncode, EXIT_SUCCESS,
                                 process.stderr.decode("utf-8", errors="replace"))
                # The exported runner's reports have fixed names below the working directory (ADR 0022 decision 7).
                mode = "Dist" if config == "Dist" else "Release"
                reports = sorted((working / "bin/TestResults").glob("*.json"))
                self.assertEqual(reports, [working / "bin/TestResults" / f"FeatureTest-{mode}.json"])
                result = json.loads(reports[0].read_text(encoding="utf-8"))
                self.assertTrue(result["passed"], result)
                self.assertEqual(result["mode"], mode)
                self.assertEqual(len(result["cases"]), 1)
                self.assertEqual(result["cases"][0]["status"], "passed")
                self.assertEqual(len(result["suites"]), 1)
                hashes[config] = result["suites"][0]["finalStateHash"]
                self.assertEqual(hashes[config], expected, hashes)
                self.assertTrue(reports[0].with_suffix(".xml").is_file())
        return hashes

    def test_exported_coverage_run_counts_cooked_loads_in_its_mode_report(self) -> None:
        # The Runtime host observes loads through its RuntimeAssetManager (IFeatureTestHost::GetAssetManager), so an
        # unfiltered --feature-test of a project with Testing.CoverageGates counts the cooked suite script by its pak
        # path and evaluates the six per-mode gates; the report is
        # <working directory>/bin/TestResults/FeatureTest-Release.json.
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("script.write", {"path": "Assets/Tests/Cooked.test.luau", "source": POWER_SUITE})
        client.call("project.setSettings", {"patch": {"Testing": {
            "CoverageGates": True, "Suites": [{"Script": "Assets/Tests/Cooked.test.luau", "Scene": TINY_SCENE}]}}})
        client.call("scene.save")
        executable = exported_executable(export_for_testing(client, automation_config()))
        working = self.directory / "Coverage"
        working.mkdir()
        arguments = [str(executable), "--headless", "--feature-test", "--renderer", "none", "--user-data-dir",
                     str(working / "UserData")]
        process = subprocess.run(arguments, cwd=working, stdin=subprocess.DEVNULL, capture_output=True, timeout=120.0,
                                 check=False)
        # The tiny game covers a sliver of the registry, so its gates fail and so does the run.
        self.assertEqual(process.returncode, EXIT_FAILED, process.stderr.decode("utf-8", errors="replace"))
        result = json.loads((working / "bin/TestResults/FeatureTest-Release.json").read_text(encoding="utf-8"))
        self.assertEqual(result["mode"], "Release")
        self.assertEqual([gate["gate"] for gate in result["coverage"]["gates"]], ["1", "1b", "2", "3", "4", "9"])
        self.assertTrue(all(gate["evaluated"] for gate in result["coverage"]["gates"]))
        self.assertFalse(result["passed"])
        # A Release report lists only the entries of its mode: no EditorOnly counter.
        self.assertTrue(all("Release" in counter["modes"] for counter in result["coverage"]["counters"]))
        script = next(counter for counter in result["coverage"]["counters"] if counter["id"] == "format:Script/.luau")
        self.assertGreaterEqual(script["count"], 1)
        self.assertTrue((working / "bin/TestResults/FeatureTest-Release.xml").is_file())

    def test_exported_feature_test_runs_cooked_scripts_with_the_editor_state_hash(self) -> None:
        # PreCommit supplies Debug; the CI automation stage supplies Release. The milestone's configuration audit
        # additionally calls check_modes with Debug, Release and Dist after building all three.
        self.check_modes((automation_config(),))


if __name__ == "__main__":
    unittest.main()
