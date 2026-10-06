"""Atomic batches (Docs/Architecture.md §13.4 "Atomic batch", Roadmap M4)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class BatchTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_batch_rollback_reports_failed_op(self) -> None:
        client, _ = self.open_editor_with_scene()
        before = client.call("scene.get")["scene"]
        revision = client.call("session.info")["_meta"]["revision"]
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"label": "Broken", "ops": [
                {"method": "entity.create", "params": {"name": "A"}},
                {"method": "entity.create", "params": {"name": "B", "components": {"RigidBody": {"Mass": -1}}}},
            ]})
        self.assertEqual(raised.exception.data["failedOp"], 1)
        self.assertEqual(raised.exception.code, engine_client.INVALID_PARAMS)
        self.assertEqual(client.call("scene.get")["scene"], before)
        self.assertEqual(client.call("edit.history")["entries"], [])
        self.assertGreaterEqual(client.call("session.info")["_meta"]["revision"], revision)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_ref_substitution(self) -> None:
        client, _ = self.open_editor_with_scene()
        result = client.call("edit.batch", {"label": "Scaffold", "ops": [
            {"method": "entity.create", "params": {"name": "Game"}},
            {"method": "entity.create", "params": {"name": "Board", "parent": {"$ref": "0.entity.id"}}},
            {"method": "entity.get", "params": {"entity": {"$ref": "1.entity.id"}}},
        ]})
        game_id = result["results"][0]["entity"]["id"]
        self.assertEqual(result["results"][2]["entity"]["parent"], game_id)
        self.assertEqual(result["results"][1]["entity"]["path"], "/Game/Board")
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"ops": [
                {"method": "entity.create", "params": {"parent": {"$ref": "5.entity.id"}}},
            ]})
        self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)


if __name__ == "__main__":
    unittest.main()
