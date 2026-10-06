"""Compact tool schemas (Docs/Architecture.md §13.8 "Compact tool schemas", Roadmap M4)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))

from engine_mcp import server  # noqa: E402

# The proxied tools of §13.8 that exist in M4 (entity_bounds arrives with M6; asset, prefab, script, play, input,
# viewport, editor and test tools with their milestones).
M4_TOOLS = {
    "project_create", "project_open", "project_save", "project_get_settings", "project_set_settings",
    "project_validate",
    "scene_new", "scene_open", "scene_save", "scene_tree", "scene_query", "scene_diff", "entity_create", "entity_get",
    "entity_update", "entity_destroy", "entity_duplicate", "entity_reparent", "component_list", "component_schema",
    "edit_batch", "edit_undo", "edit_redo", "log_read", "docs_get",
}
MAX_SCHEMA_BYTES = 4096


class SchemaTests(unittest.TestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_tool_schemas_are_compact(self) -> None:
        tools = server.load_catalog()
        self.assertEqual({tool.name for tool in tools}, M4_TOOLS)
        for tool in tools:
            with self.subTest(tool=tool.name):
                text = json.dumps(tool.input_schema)
                self.assertNotIn("$defs", text)
                self.assertNotIn("$ref", text)
                self.assertLess(len(text.encode("utf-8")), MAX_SCHEMA_BYTES)
                self.assertTrue(tool.description)
        entity_create = next(tool for tool in tools if tool.name == "entity_create")
        components = entity_create.input_schema["properties"]["components"]
        self.assertEqual(components["type"], "object")
        self.assertIn("component_schema", components["description"])
        for name, schema in server.local_tool_schemas().items():
            with self.subTest(tool=name):
                self.assertIn(name, server.LOCAL_TOOL_NAMES)
                self.assertEqual(schema["type"], "object")


if __name__ == "__main__":
    unittest.main()
