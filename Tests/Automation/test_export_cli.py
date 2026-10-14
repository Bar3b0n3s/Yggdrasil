"""`Editor --export` and Scripts/Export.py (Docs/Architecture.md §2.3, §12.1, §14.2; Docs/Decisions/0024-m15-contract.md
decision 3, stream A): the one-shot export of a project from the command line, its report file and exit codes. The
exports use the Runtime of the configuration the suite's editor was built in (harness.automation_config).
"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from harness import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, REPOSITORY_ROOT, AutomationTestCase, automation_config
from tiny_game import TINY_GAME_NAME, build_tiny_game


class ExportCommandLineTests(AutomationTestCase):
    def make_tiny_game(self) -> Path:
        client = self.connect(self.start_editor())
        root = self.create_project(client, TINY_GAME_NAME)
        build_tiny_game(client)
        client.call("session.shutdown")
        self.editors[-1].wait()
        return root

    @unittest.skip("contract stub: Editor --export (ADR 0024 decision 3, stream A)")
    def test_export_cli_writes_the_package_and_its_report(self) -> None:
        root = self.make_tiny_game()
        output = self.directory / "Packages" / TINY_GAME_NAME
        report_path = self.directory / "Report.json"
        code, stderr = self.run_editor(["--headless", "--renderer", "none", "--project", str(root), "--export", str(output),
                                        "--config", automation_config(), "--export-report", str(report_path)], timeout=600.0)
        self.assertEqual(code, EXIT_SUCCESS, stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertTrue(report["success"])
        self.assertEqual(Path(report["result"]["outputDirectory"]), output)
        self.assertTrue(Path(report["result"]["executable"]).is_file())
        self.assertFalse((root / "Build").exists())

        # A validation failure exits 1 and still writes the report, with the error and its issues.
        client = self.connect(self.start_editor(project=root))
        client.call("project.setSettings", {"patch": {"StartScene": ""}})
        client.call("session.shutdown")
        self.editors[-1].wait()
        code, stderr = self.run_editor(["--headless", "--renderer", "none", "--project", str(root), "--export", str(output),
                                        "--config", automation_config(), "--export-report", str(report_path)], timeout=600.0)
        self.assertEqual(code, EXIT_FAILED, stderr)
        failed = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertFalse(failed["success"])
        self.assertEqual([issue["pointer"] for issue in failed["error"]["issues"]], ["/StartScene"])

    @unittest.skip("contract stub: Editor --export (ADR 0024 decision 3, stream A)")
    def test_export_cli_exits_3_when_the_project_does_not_open_or_is_locked(self) -> None:
        root = self.make_tiny_game()
        output = self.directory / "Locked" / TINY_GAME_NAME
        report_path = self.directory / "Report.json"
        code, stderr = self.run_editor(["--headless", "--renderer", "none", "--project", str(self.directory / "Missing.eproj"),
                                        "--export", str(output), "--config", automation_config(), "--export-report",
                                        str(report_path)], timeout=600.0)
        self.assertEqual(code, EXIT_INIT_FAILED, stderr)
        self.assertFalse(json.loads(report_path.read_text(encoding="utf-8"))["success"])
        # A serving editor holds the project's lock: the one-shot export cannot open it.
        self.connect(self.start_editor(project=root))
        code, stderr = self.run_editor(["--headless", "--renderer", "none", "--project", str(root), "--export", str(output),
                                        "--config", automation_config()], timeout=600.0)
        self.assertEqual(code, EXIT_INIT_FAILED, stderr)
        self.assertFalse(output.exists())
        # A missing build output also exits 3 (Exporter::IsMissingBuildOutput, ExporterTests); it cannot be staged here,
        # because Editor --export always exports from the checkout's bin directory.

    @unittest.skip("contract stub: Editor --export-root (ADR 0024 decision 3, stream A)")
    def test_export_root_moves_project_export_out_of_the_project(self) -> None:
        root = self.make_tiny_game()
        exports = self.directory / "ExportRoot"
        client = self.connect(self.start_editor(project=root, arguments=["--export-root", str(exports)]))
        report = client.call("project.export", {"config": automation_config()})
        output = Path(report["outputDirectory"])
        self.assertTrue(output.is_relative_to(exports), output)
        self.assertTrue(Path(report["executable"]).is_file())
        self.assertFalse((root / "Build").exists())

    @unittest.skip("contract stub: Scripts/Export.py (ADR 0024 decision 3, stream A)")
    def test_export_script_wraps_the_cli(self) -> None:
        root = self.make_tiny_game()
        output = self.directory / "ScriptPackage"
        completed = subprocess.run([sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Export.py"), "--project", str(root),
                                    "--config", automation_config(), "--out", str(output), "--editor-config", automation_config(),
                                    "--renderer", "none", "--user-data-dir", str(self.user_data), "--json"],
                                   capture_output=True, text=True, timeout=600.0, check=False)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stdout + completed.stderr)
        document = json.loads(completed.stdout)
        self.assertTrue(document["success"])
        self.assertEqual(Path(document["result"]["outputDirectory"]), output)


if __name__ == "__main__":
    unittest.main()
