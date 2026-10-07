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
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import anyio
from mcp import Client, StdioServerParameters

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tests" / "Automation"))

import engine_client  # noqa: E402
import fake_editor  # noqa: E402
from engine_mcp import server  # noqa: E402

# The bridge's settings, which the SDK's stdio client (it passes only an allow-listed environment) must hand on.
BRIDGE_ENVIRONMENT_VARIABLES = ("ENGINE_MCP_TRANSCRIPT", "ENGINE_MCP_USER_DATA_DIR", "ENGINE_AUTOMATION_CONFIG",
                                "ENGINE_AUTOMATION_COVERAGE")
SESSION_TIMEOUT_SECONDS = 300.0
# The M4 editor registers this many methods at least (test hooks excluded, since editor_launch starts no test hooks).
MIN_EDITOR_METHODS = 36


class ConformanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineMcpConformance-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        # Keep the projects these tests create out of any real transcript, and the editors the bridge launches out of
        # the real user-data folder (session files, logs, crash reports), for this test only: the patch restores the
        # environment afterwards, so later test modules never see a path into a removed directory.
        self.user_data = self.directory / "UserData"
        environment = mock.patch.dict(os.environ, {"ENGINE_MCP_TRANSCRIPT": str(self.directory / "BuildLog.jsonl"),
                                                   "ENGINE_MCP_USER_DATA_DIR": str(self.user_data)})
        environment.start()
        self.addCleanup(environment.stop)

    def test_sdk_client_lists_catalog_tools_without_editor(self) -> None:
        names = self.list_tool_names()
        self.assertTrue(set(server.LOCAL_TOOL_NAMES) <= names)
        self.assertTrue({tool.name for tool in server.load_catalog()} <= names)

    def test_sdk_client_launches_an_editor_and_calls_tools(self) -> None:
        results = self.run_session([
            ("editor_launch", {"project": str(self.directory / "Game"), "create": True, "template": "Empty"}),
            ("scene_new", {"path": "Assets/Scenes/Main.scene"}),
            ("entity_create", {"name": "Board"}),
            ("scene_tree", {}),
            ("engine_methods", {}),
        ])
        self.assertIn("Board", results[3]["structuredContent"]["text"])
        self.assertIn("_meta", results[3]["text"])
        # Every registered method, read from the file the editor offloads the unfiltered catalogue to.
        self.assertFalse(results[4]["isError"], results[4]["text"])
        names = {method["name"] for method in results[4]["structuredContent"]["methods"]}
        self.assertGreaterEqual(len(names), MIN_EDITOR_METHODS)
        self.assertTrue({"entity.create", "rpc.discover", "project.upgrade"} <= names, sorted(names))

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

    def test_generated_mcp_json_needs_no_python_on_path(self) -> None:
        configuration = json.loads((REPOSITORY_ROOT / ".mcp.json").read_text(encoding="utf-8"))
        engine = configuration["mcpServers"]["engine"]
        self.assertEqual(engine["type"], "stdio")
        self.assertTrue(Path(engine["command"]).is_absolute())
        self.assertEqual(Path(engine["args"][0]).resolve(), (MCP_ROOT / "run.py").resolve())
        names = self.list_tool_names(command=engine["command"], arguments=engine["args"], environment={"PATH": ""})
        self.assertIn("editor_launch", names)

    def test_sdk_client_attaches_and_proxies_calls(self) -> None:
        # The bridge's own plumbing without an editor build: editor_attach finds a stand-in server through its session
        # file, and engine_call, editor_status and engine_methods reach it.
        handlers = {
            "session.info": lambda params: {"pid": os.getpid(), "readOnly": False},
            "rpc.discover": lambda params: {"methods": [{"name": "session.info", "description": "The session.",
                                                         "mutates": False, "params": {"type": "object"}}]},
        }
        with fake_editor.FakeEditor(handlers) as editor:
            sessions = engine_client.sessions_directory(engine_client.read_workspace_name(REPOSITORY_ROOT),
                                                        self.user_data)
            editor.write_session_file(sessions)
            results = self.run_session([
                ("editor_attach", {"pid": os.getpid()}),
                ("engine_call", {"method": "session.info", "params": {}}),
                ("engine_call", {"method": "no.such", "params": {}}),
                ("editor_status", {}),
                ("engine_methods", {}),
            ], count_coverage=False)
            clients = {request.client for request in editor.requests}
        self.assertFalse(results[0]["isError"], results[0])
        self.assertFalse(results[0]["structuredContent"]["supervised"])
        self.assertEqual(results[1]["structuredContent"]["pid"], os.getpid())
        self.assertIn("_meta", results[1]["text"])
        self.assertTrue(results[2]["isError"])
        self.assertEqual(results[2]["structuredContent"]["code"], engine_client.METHOD_NOT_FOUND)
        self.assertTrue(results[3]["structuredContent"]["connected"])
        self.assertEqual([method["name"] for method in results[4]["structuredContent"]["methods"]], ["session.info"])
        self.assertEqual(clients, {"engine-mcp"})
        # The attached editor's project is unknown (launcher state), so nothing reached a transcript.
        self.assertFalse((self.directory / "BuildLog.jsonl").exists())

    def bridge_environment(self, environment: dict[str, str] | None, count_coverage: bool = True) -> dict[str, str]:
        """The bridge's settings from this process's environment, then `environment` over them. Without
        `count_coverage` the bridge records no method coverage (calls a stand-in server answers prove nothing about the
        editor's methods)."""
        passed = {name: os.environ[name] for name in BRIDGE_ENVIRONMENT_VARIABLES if name in os.environ}
        if not count_coverage:
            passed.pop(engine_client.COVERAGE_ENVIRONMENT_VARIABLE, None)
        return {**passed, **(environment or {})}

    def list_tool_names(self, command: str | None = None, arguments: list[str] | None = None,
                        environment: dict[str, str] | None = None) -> set[str]:
        """Starts the bridge (by default with this interpreter and run.py) through the SDK's stdio client and returns
        the names of the tools it lists."""
        parameters = StdioServerParameters(command=command or sys.executable,
                                           args=arguments or [str(MCP_ROOT / "run.py")],
                                           env=self.bridge_environment(environment), cwd=str(REPOSITORY_ROOT))

        async def session() -> set[str]:
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(parameters) as client:
                    return {tool.name for tool in (await client.list_tools()).tools}

        return anyio.run(session)

    def run_session(self, calls: list[tuple[str, dict[str, object]]],
                    count_coverage: bool = True) -> list[dict[str, Any]]:
        """Runs `calls` in one bridge session through the SDK client ("$kill" kills the launched editor) and returns
        each result as {"structuredContent": ..., "text": ..., "isError": ...}. At the end, editor_shutdown {force:
        true} shuts down a launched editor that still runs (and only disconnects from an attached one)."""
        parameters = StdioServerParameters(command=sys.executable, args=[str(MCP_ROOT / "run.py")],
                                           env=self.bridge_environment(None, count_coverage), cwd=str(REPOSITORY_ROOT))

        async def call(client: Any, name: str, arguments: dict[str, object]) -> dict[str, Any]:
            result = await client.call_tool(name, arguments)
            text = "\n".join(block.text for block in result.content if getattr(block, "type", "") == "text")
            return {"structuredContent": result.structured_content, "text": text, "isError": result.is_error}

        async def session() -> tuple[list[dict[str, Any]], str]:
            results: list[dict[str, Any]] = []
            failure = ""
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(parameters) as client:
                    try:
                        for name, arguments in calls:
                            if name == "$kill":
                                status = await call(client, "editor_status", {})
                                pid = status["structuredContent"].get("pid")
                                if not isinstance(pid, int):
                                    failure = f"no launched editor to kill: {status['text']}"
                                    break
                                kill_process(pid)
                                # The editor is the bridge's child, so only the bridge can see it exit (on POSIX it
                                # stays a zombie until the bridge reaps it): ask until it reports the editor gone.
                                deadline = time.monotonic() + 30.0
                                while status["structuredContent"].get("connected", False) and not status["isError"]:
                                    if time.monotonic() > deadline:
                                        failure = f"the killed editor {pid} still runs: {status['text']}"
                                        break
                                    await anyio.sleep(engine_client.POLL_INTERVAL_SECONDS)
                                    status = await call(client, "editor_status", {})
                                continue
                            results.append(await call(client, name, arguments))
                            if name in ("editor_launch", "editor_attach") and results[-1]["isError"]:
                                failure = f"{name} failed: {results[-1]['text']}"
                                break
                    finally:
                        await call(client, "editor_shutdown", {"force": True})
            return results, failure

        # A failure is reported after the session ends: inside it, the SDK's task groups would wrap it in an exception
        # group, which unittest shows as an error instead of a failure.
        results, failure = anyio.run(session)
        if failure:
            self.fail(failure)
        return results


def kill_process(pid: int) -> None:
    """Kills `pid` the way a crash ends it: TerminateProcess on Windows (os.kill), SIGKILL on POSIX."""
    os.kill(pid, signal.SIGTERM if sys.platform == "win32" else signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
