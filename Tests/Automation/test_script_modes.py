"""M13 cooked scripting through a real exported executable and its feature-test entry point."""

from __future__ import annotations

import json
import subprocess
import unittest

from harness import EXIT_SUCCESS, AutomationTestCase, automation_config, engine_client
from tiny_game import TINY_GAME_NAME, TINY_SCENE, build_tiny_game, export_tiny_game, exported_executable


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
            "Script": {"Script": "Assets/Scripts/Powers.luau"}}})
        client.call("script.write", {"path": "Assets/Tests/Cooked.test.luau", "source": POWER_SUITE})
        client.call("project.setSettings", {"patch": {"Export": {"Exclude": []}, "Testing": {"Suites": [{
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
                executable = exported_executable(export_tiny_game(client, config))
                # M15 adds the testing-export switch. This fixture enables the manifest admission flag
                # without altering either pak, so the runner must load the ordinary cooked script artifacts.
                manifest_path = executable.parent / "Game.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest["Testing"] = True
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                working = self.directory / f"Cooked-{config}"
                working.mkdir()
                arguments = [str(executable), "--headless", "--feature-test", "--filter", "Cooked/power"]
                if config != "Dist":
                    arguments += ["--renderer", "none", "--user-data-dir", str(working / "UserData")]
                process = subprocess.run(arguments, cwd=working, stdin=subprocess.DEVNULL,
                                         capture_output=True, timeout=120.0, check=False)
                self.assertEqual(process.returncode, EXIT_SUCCESS,
                                 process.stderr.decode("utf-8", errors="replace"))
                reports = list((working / "bin/TestResults").glob("Runtime-*.json"))
                self.assertEqual(len(reports), 1)
                result = json.loads(reports[0].read_text(encoding="utf-8"))
                self.assertTrue(result["passed"], result)
                self.assertEqual(result["mode"], "Dist" if config == "Dist" else "Release")
                self.assertEqual(len(result["cases"]), 1)
                self.assertEqual(result["cases"][0]["status"], "passed")
                self.assertEqual(len(result["suites"]), 1)
                hashes[config] = result["suites"][0]["finalStateHash"]
                self.assertEqual(hashes[config], expected, hashes)
                self.assertTrue(reports[0].with_suffix(".xml").is_file())
        return hashes

    def test_exported_feature_test_runs_cooked_scripts_with_the_editor_state_hash(self) -> None:
        # PreCommit supplies Debug; the CI automation stage supplies Release. The milestone's configuration audit
        # additionally calls check_modes with Debug, Release and Dist after building all three.
        self.check_modes((automation_config(),))


if __name__ == "__main__":
    unittest.main()
