"""MCP conformance through the official SDK's client (Docs/Architecture.md §15.7, Roadmap M4): the catalogue tools are
listed without an editor, editor_launch starts a headless editor, entity_create and scene_tree work, a killed editor
makes the next call return EditorCrashed, and the generated .mcp.json starts the bridge with no python on PATH.

These tests run in the bridge's virtual environment (Scripts/Test.py starts them with Tools/MCP/.venv's interpreter),
where the mcp package is installed from Tools/MCP/requirements.lock.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))

from engine_mcp import server  # noqa: E402


class ConformanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineMcpConformance-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        # Keep the projects these tests create out of any real transcript, for this test only: the patch restores the
        # environment afterwards, so later test modules never see a path into a removed directory.
        environment = mock.patch.dict(os.environ, {"ENGINE_MCP_TRANSCRIPT": str(self.directory / "BuildLog.jsonl")})
        environment.start()
        self.addCleanup(environment.stop)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_sdk_client_lists_catalog_tools_without_editor(self) -> None:
        names = self.list_tool_names()
        self.assertTrue(set(server.LOCAL_TOOL_NAMES) <= names)
        self.assertTrue({tool.name for tool in server.load_catalog()} <= names)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_sdk_client_launches_an_editor_and_calls_tools(self) -> None:
        results = self.run_session([
            ("editor_launch", {"project": str(self.directory / "Game"), "create": True, "template": "Empty"}),
            ("scene_new", {"path": "Assets/Scenes/Main.scene"}),
            ("entity_create", {"name": "Board"}),
            ("scene_tree", {}),
        ])
        self.assertIn("Board", results[-1]["structuredContent"]["text"])
        self.assertIn("_meta", results[-1]["text"])

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_killed_editor_reports_editor_crashed(self) -> None:
        results = self.run_session([
            ("editor_launch", {"project": str(self.directory / "Game"), "create": True}),
            ("engine_call", {"method": "session.info", "params": {}}),
            ("$kill", {}),
            ("scene_tree", {}),
        ])
        crashed = results[-1]["structuredContent"]
        self.assertEqual(crashed["error"], "EditorCrashed")
        self.assertIn("exitCode", crashed)
        self.assertLessEqual(len(crashed["lastLogLines"]), 50)

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_generated_mcp_json_needs_no_python_on_path(self) -> None:
        configuration = json.loads((REPOSITORY_ROOT / ".mcp.json").read_text(encoding="utf-8"))
        engine = configuration["mcpServers"]["engine"]
        self.assertEqual(engine["type"], "stdio")
        self.assertTrue(Path(engine["command"]).is_absolute())
        self.assertEqual(Path(engine["args"][0]).resolve(), (MCP_ROOT / "run.py").resolve())
        names = self.list_tool_names(command=engine["command"], arguments=engine["args"], environment={"PATH": ""})
        self.assertIn("editor_launch", names)

    def list_tool_names(self, command: str | None = None, arguments: list[str] | None = None,
                        environment: dict[str, str] | None = None) -> set[str]:
        """Starts the bridge (by default with this interpreter and run.py) through the SDK's stdio client and returns
        the names of the tools it lists."""
        raise NotImplementedError("contract stub: ConformanceTests.list_tool_names (M4 stream D)")

    def run_session(self, calls: list[tuple[str, dict[str, object]]]) -> list[dict[str, object]]:
        """Runs `calls` in one bridge session through the SDK client ("$kill" kills the launched editor) and returns
        each result as {"structuredContent": ..., "text": ...}."""
        raise NotImplementedError("contract stub: ConformanceTests.run_session (M4 stream D)")


if __name__ == "__main__":
    unittest.main()
