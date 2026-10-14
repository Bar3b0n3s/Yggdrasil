"""Testing exports through automation and the export suite's pure parts (Docs/Architecture.md §7.6, §14.1 to §14.3,
§15.1 T3 and T3d; Roadmap M15; Docs/Decisions/0024-m15-contract.md decisions 6, 9 and 10, stream B). The FeatureTest
testing export in Release and Dist is the export stage's (Scripts/Test.py --suite export); these tests use the tiny game.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config, engine_client
from tiny_game import TINY_GAME_NAME, build_tiny_game

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import exports  # noqa: E402  (Scripts/ is on sys.path above)


def write_feature_report(path: Path, mode: str, suites: list[dict]) -> Path:
    """A minimal FeatureTest TestRunResult with `suites` rows."""
    document = {"mode": mode, "passed": True, "cancelled": False, "cases": [], "suites": suites,
                "coverage": {"registryFingerprint": "0000000000000000", "counters": [], "gates": []},
                "jsonPath": "", "junitPath": "", "recordingPath": ""}
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


class TestingExportParamTests(AutomationTestCase):
    """project.export's testing param (moved from test_export.py by the contract; stream B replaces the refusal)."""

    def test_project_export_testing_param(self) -> None:
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        # Until stream B lands, a testing export is refused at /testing (ADR 0012 decision 14). Stream B turns this into
        # the acceptance: {"testing": true} exports with Game.json Testing true and the report's testing fields.
        with self.assertRaises(engine_client.EngineError) as refused:
            client.call("project.export", {"config": automation_config(), "testing": True})
        self.assert_engine_error(refused.exception, engine_client.UNSUPPORTED)
        self.assertEqual(refused.exception.issues[0]["pointer"], "/testing")


class TestingExportTests(AutomationTestCase):
    @unittest.skip("contract stub: testing exports (ADR 0024 decision 6, stream B)")
    def test_testing_export_packages_tests_and_says_testing(self) -> None:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("script.write", {"path": "Assets/Tests/Smoke.test.luau", "source":
                                     "--!strict\nreturn Test.Suite(\"Smoke\", function()\n"
                                     "\tTest.Case(\"passes\", function() Test.Expect(true, \"true\") end)\nend)\n"})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [
            {"Script": "Assets/Tests/Smoke.test.luau", "Scene": "Assets/Scenes/Main.scene"}]}}})
        report = client.call("project.export", {"config": automation_config(), "testing": True})
        self.assertTrue(report["testing"])
        self.assertEqual(report["testSuiteCount"], 1)
        self.assertEqual(report["replayCount"], 0)
        output = Path(report["outputDirectory"])
        self.assertTrue(output.is_relative_to(root / "Build"))
        self.assertTrue(output.parent.name.endswith("-Testing"), output)
        manifest = json.loads((output / "Game.json").read_text(encoding="utf-8")) if sys.platform != "darwin" else \
            json.loads((output / f"{TINY_GAME_NAME}.app" / "Contents" / "Resources" / "Game.json").read_text(encoding="utf-8"))
        self.assertTrue(manifest["Testing"])
        plain = client.call("project.export", {"config": automation_config(), "testing": False})
        self.assertFalse(plain["testing"])

    @unittest.skip("contract stub: testing exports (ADR 0024 decisions 6 and 9, stream B)")
    def test_testing_export_runs_its_suite_in_the_exported_runtime(self) -> None:
        client = self.connect(self.start_editor())
        self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("script.write", {"path": "Assets/Tests/Smoke.test.luau", "source":
                                     "--!strict\nreturn Test.Suite(\"Smoke\", function()\n"
                                     "\tTest.Case(\"passes\", function() Test.Expect(true, \"true\") end)\nend)\n"})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [
            {"Script": "Assets/Tests/Smoke.test.luau", "Scene": "Assets/Scenes/Main.scene"}]}}})
        report = client.call("project.export", {"config": automation_config(), "testing": True})
        package = Path(report["outputDirectory"])
        with tempfile.TemporaryDirectory(prefix="TestingExportRun-") as working:
            code, stderr = self.run_exported(Path(report["executable"]), Path(working))
            self.assertEqual(code, 0, stderr)
            result = json.loads((Path(working) / "bin" / "TestResults" / "FeatureTest-Release.json").read_text(
                encoding="utf-8"))
        self.assertTrue(result["passed"])
        self.assertTrue((exports.testing_user_data(package, TINY_GAME_NAME) / "Logs" / f"{TINY_GAME_NAME}.log").is_file())

    def run_exported(self, executable: Path, working: Path) -> tuple[int, str]:
        """Runs the exported executable's --feature-test headless without a device in `working`."""
        completed = subprocess.run([str(executable), "--headless", "--renderer", "none", "--feature-test"], cwd=working,
                                   stdin=subprocess.DEVNULL, capture_output=True, timeout=300.0, check=False)
        return completed.returncode, completed.stderr.decode("utf-8", errors="replace")


class ExportsLibTests(unittest.TestCase):
    """Scripts/Lib/exports.py: the parts of the export and determinism suites that need no build."""

    @unittest.skip("contract stub: exports (ADR 0024 decision 9, stream B)")
    def test_package_paths(self) -> None:
        package = exports.package_directory("Dist")
        self.assertEqual(package, REPOSITORY_ROOT / "bin" / "Exports" / "Dist" / "FeatureTest")
        executable = exports.package_executable(package)
        if sys.platform == "win32":
            self.assertEqual(executable, package / "FeatureTest.exe")
        elif sys.platform == "darwin":
            self.assertEqual(executable, package / "FeatureTest.app" / "Contents" / "MacOS" / "FeatureTest")
        else:
            self.assertEqual(executable, package / "FeatureTest")
        self.assertEqual(exports.testing_user_data(package), package / "bin" / "TestUserData" / "FeatureTest")

    @unittest.skip("contract stub: exports (ADR 0024 decision 10, stream B)")
    def test_replay_hashes_come_from_every_replay_row_with_a_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ExportsLib-") as directory:
            report = write_feature_report(Path(directory) / "FeatureTest-Dist.json", "Dist", [
                {"suite": "replay:Assets/Tests/Replays/Determinism.replay", "passed": True,
                 "finalStateHash": "2a75dca847e98e59"},
                {"suite": "replay:Assets/Tests/Replays/Input.replay", "passed": False, "finalStateHash": "ff0e3f70e3200f87"},
                {"suite": "replay:Assets/Tests/Replays/Broken.replay", "passed": False, "finalStateHash": ""},
                {"suite": "Api.Entity", "passed": True, "finalStateHash": "0123456789abcdef"},
            ])
            # A failed row keeps its observed hash, so the determinism stage can name the drift.
            self.assertEqual(exports.read_replay_hashes(report),
                             {"Assets/Tests/Replays/Determinism.replay": "2a75dca847e98e59",
                              "Assets/Tests/Replays/Input.replay": "ff0e3f70e3200f87"})
            broken = Path(directory) / "Broken.json"
            broken.write_text("[]", encoding="utf-8")
            with self.assertRaises(ValueError):
                exports.read_replay_hashes(broken)

    @unittest.skip("contract stub: exports (ADR 0024 decisions 9 and 10, stream B)")
    def test_a_report_counts_only_when_newer_than_every_input(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ExportsLib-") as directory:
            root = Path(directory)
            report = root / "FeatureTest-Release.json"
            runtime = root / "Runtime.exe"
            runtime.write_text("runtime", encoding="utf-8")
            self.assertIn("missing", exports.stale_report_reason(report, [runtime]) or "")
            report.write_text("{}", encoding="utf-8")
            os.utime(runtime, (1_000_000, 1_000_000))
            os.utime(report, (2_000_000, 2_000_000))
            self.assertIsNone(exports.stale_report_reason(report, [runtime, root / "Absent.exe"]))
            os.utime(runtime, (3_000_000, 3_000_000))
            reason = exports.stale_report_reason(report, [runtime])
            self.assertIsNotNone(reason)
            self.assertIn("Runtime.exe", reason or "")
        inputs = exports.report_inputs("Dist", "exported")
        self.assertTrue(any(path.parent.name == "Runtime" and path.stem == "Runtime" for path in inputs))
        self.assertTrue(any(path.parent.name == "Editor" and path.stem == "Editor" for path in inputs))
        self.assertTrue(any("Library" not in path.parts and path.suffix == ".replay" for path in inputs))
        self.assertFalse(any("Library" in path.relative_to(REPOSITORY_ROOT).parts for path in inputs))
        with self.assertRaises(ValueError):
            exports.report_inputs("Dist", "other")

    @unittest.skip("contract stub: exports (ADR 0024 decision 10, stream B)")
    def test_determinism_runs_are_compared_with_the_committed_hash_and_each_other(self) -> None:
        replay = "Assets/Tests/Replays/Determinism.replay"
        committed = {replay: "2a75dca847e98e59"}
        same = {run: {replay: "2a75dca847e98e59"} for run in exports.DETERMINISM_RUNS}
        self.assertEqual(exports.compare_determinism_runs(committed, same), {replay: []})
        drifted = dict(same)
        drifted["Exported Dist"] = {replay: "%016x" % zlib.crc32(b"drift")}
        failures = exports.compare_determinism_runs(committed, drifted)[replay]
        self.assertTrue(any(line.startswith("Exported Dist: ") for line in failures), failures)
        self.assertTrue(any(line.startswith("runs differ: ") for line in failures), failures)
        missing = dict(same)
        missing["Exported Release"] = {}
        self.assertEqual(exports.compare_determinism_runs(committed, missing)[replay], ["Exported Release: no hash"])


if __name__ == "__main__":
    unittest.main()
