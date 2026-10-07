"""Compact tool schemas (Docs/Architecture.md §13.8 "Compact tool schemas", Roadmap M4)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))

from engine_mcp import server  # noqa: E402

# The proxied tools of §13.8 that exist so far: M4's, M5's screenshot tools, registered at the M4/M5 merge
# (Docs/Decisions/0009-m5-decisions.md decision 33), and M6's asset, prefab and entity_bounds tools
# (Docs/Decisions/0010-m6-decisions.md decision 20). Script, play, input and test tools arrive with their milestones.
PROXIED_TOOLS = {
    "project_create", "project_open", "project_save", "project_get_settings", "project_set_settings",
    "project_validate",
    "scene_new", "scene_open", "scene_save", "scene_tree", "scene_query", "scene_diff", "entity_create", "entity_get",
    "entity_update", "entity_destroy", "entity_duplicate", "entity_reparent", "entity_bounds", "component_list",
    "component_schema", "edit_batch", "edit_undo", "edit_redo", "log_read", "docs_get", "viewport_screenshot",
    "editor_screenshot", "asset_list", "asset_import", "asset_create", "asset_set_properties", "asset_move",
    "asset_delete", "prefab_create", "prefab_instantiate", "prefab_apply",
}
MAX_SCHEMA_BYTES = 4096


def schema_keywords(value: object) -> set[str]:
    """Every member name used anywhere in a JSON value (so descriptions that mention a keyword do not count)."""
    names: set[str] = set()
    children: list[object] = []
    if isinstance(value, dict):
        names.update(str(name) for name in value)
        children = list(value.values())
    elif isinstance(value, list):
        children = value
    for child in children:
        names |= schema_keywords(child)
    return names


class SchemaTests(unittest.TestCase):
    def test_tool_schemas_are_compact(self) -> None:
        tools = server.load_catalog()
        self.assertEqual({tool.name for tool in tools}, PROXIED_TOOLS)
        for tool in tools:
            with self.subTest(tool=tool.name):
                text = json.dumps(tool.input_schema)
                keywords = schema_keywords(tool.input_schema)
                self.assertNotIn("$defs", keywords)
                self.assertNotIn("$ref", keywords)
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
