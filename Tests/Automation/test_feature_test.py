"""Projects/FeatureTest as a whole (Docs/Decisions/0022-m14-contract.md decision 8): the committed scaffolds, run in the
order and with the seeds of Scaffold/Scaffolds.json from Scaffold/Baseline/, reproduce the project byte for byte. The lead
owns this test; it needs every stream's scaffold, so it is unskipped at integration."""

from __future__ import annotations

import subprocess
import sys
import unittest

from harness import REPOSITORY_ROOT, AutomationTestCase, automation_config


class FeatureTestScaffoldTests(AutomationTestCase):
    @unittest.skip("contract stub: ADR 0022 decision 8 (lead, at integration)")
    def test_scaffolds_reproduce_the_project_byte_for_byte(self) -> None:
        # Formats.jsonl imports an .hdr, which needs the GPU environment baker.
        if not self.require_gpu():
            return
        process = subprocess.run(
            [sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Scaffold.py"), "--check", "--config", automation_config()],
            capture_output=True, text=True, timeout=1800.0, check=False)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)


if __name__ == "__main__":
    unittest.main()
