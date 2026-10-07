"""The launcher state (Docs/Architecture.md §12.1, Roadmap M4): an editor without --project offers only session.*,
rpc.discover, docs.get, project.create and project.open."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

LAUNCHER_METHODS = ("docs.get", "project.create", "project.open", "rpc.discover", "session.hello", "session.info",
                    "session.shutdown")


class LauncherStateTests(AutomationTestCase):
    def test_launcher_state_allows_only_project_methods(self) -> None:
        editor = self.start_editor()
        client = self.connect(editor)
        self.assertFalse(client.hello["project"]["open"])
        client.call("session.info")
        client.call("docs.get")
        # The whole catalogue is over the offload threshold, so it arrives as a file (§13.4 bounded output).
        methods = engine_client.load_offloaded(client.call("rpc.discover"))["methods"]
        for method in methods:
            name = method["name"]
            self.assertEqual(method["availableInLauncher"], name in LAUNCHER_METHODS, name)
        for name in ("scene.tree", "entity.create", "project.info", "component.list", "log.read", "edit.undo", "debug.stall"):
            with self.assertRaises(engine_client.EngineError, msg=name) as raised:
                client.call(name, {})
            self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")
            self.assertIn("no project open", raised.exception.detail)
        self.create_project(client)
        self.assertTrue(client.call("project.info")["project"]["name"])
        self.assertTrue(client.call("session.info")["project"]["open"])


if __name__ == "__main__":
    unittest.main()
