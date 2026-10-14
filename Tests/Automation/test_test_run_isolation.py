"""M13 test-run isolation of host state between suites (Docs/Architecture.md §11.10)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class TestRunIsolationTests(AutomationTestCase):
    def test_test_run_suites_start_from_the_cursor_baseline(self) -> None:
        # Input.SetCursorMode reaches the editor's real window; a suite that locks the cursor must not leave it locked
        # for the next suite, whatever the suite order or filter.
        client, _ = self.open_editor_with_scene()
        client.call("script.write", {"path": "Assets/Tests/Locks.test.luau", "source":
                                     'return Test.Suite("Locks", function()\n'
                                     'Test.Case("lock", function() Input.SetCursorMode("Locked")\n'
                                     'Test.ExpectEqual(Input.GetCursorMode(), "Locked") end)\nend)'})
        client.call("script.write", {"path": "Assets/Tests/Reads.test.luau", "source":
                                     'return Test.Suite("Reads", function()\n'
                                     'Test.Case("default", function() '
                                     'Test.ExpectEqual(Input.GetCursorMode(), "Normal") end)\n'
                                     'end)'})
        client.call("project.setSettings", {"patch": {"Testing": {"Suites": [
            {"Script": "Assets/Tests/Locks.test.luau", "Scene": "", "Modes": ["Editor"]},
            {"Script": "Assets/Tests/Reads.test.luau", "Scene": "", "Modes": ["Editor"]}]}}})
        result = engine_client.load_offloaded(client.call("test.run", timeout=120.0))
        rows = {(row["suite"], row["case"]): row for row in result["cases"]}
        self.assertEqual(rows[("Locks", "lock")]["status"], "passed", rows)
        self.assertEqual(rows[("Reads", "default")]["status"], "passed", rows)
        self.assertTrue(result["passed"], result)
        # The same suite passes alone, so the outcome no longer depends on what ran before it.
        alone = engine_client.load_offloaded(client.call("test.run", {"filter": "Reads/"}, timeout=120.0))
        self.assertTrue(alone["passed"], alone)


if __name__ == "__main__":
    unittest.main()
