"""Optimistic concurrency (Docs/Architecture.md §13.4 "ifRevision", Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class ConcurrencyTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_if_revision_conflict(self) -> None:
        client, _ = self.open_editor_with_scene()
        revision = client.call("session.info")["_meta"]["revision"]
        created = client.call("entity.create", {"name": "First", "ifRevision": revision})
        current = created["_meta"]["revision"]
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("entity.create", {"name": "Second", "ifRevision": revision})
        self.assert_engine_error(raised.exception, engine_client.CONFLICT, "Conflict")
        self.assertEqual(raised.exception.data["currentRevision"], current)
        self.assertEqual(client.call("scene.query", {"where": {"name": "Second"}})["total"], 0)


if __name__ == "__main__":
    unittest.main()
