"""The tool list's refresh (Docs/Architecture.md §13.8 "Tool list refresh"; Docs/Decisions/0026-m16-contract.md,
predicted friction P10), through the official SDK's client on its in-memory transport: the server declares
tools.listChanged in both protocol eras, a handshake-era client is told to fetch the tool list again once it has
initialized, and editor_launch and editor_attach reread a changed catalog.json, report it (failed calls too) and notify
the client (on the session of a handshake-era connection, on the subscriptions/listen stream of a 2026-07-28 one), also
when the client cancelled the call or it raised. The editor they connect to is the stand-in server of
Tests/Automation/fake_editor.py, reached through its session file as a listening editor's is."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any, Callable
from unittest import mock

import anyio
import mcp_types as types
from mcp import Client, MCPError
from mcp.client.subscriptions import ToolsListChanged
from mcp_types.version import MODERN_PROTOCOL_VERSIONS

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tests" / "Automation"))

import engine_client  # noqa: E402
import fake_editor  # noqa: E402
from engine_mcp import server, transcript  # noqa: E402
from engine_mcp.launcher import Launcher  # noqa: E402

# Bound a session that never ends and a wait for a notification that never comes (the assertions then report what
# arrived); no passing outcome depends on them.
SESSION_TIMEOUT_SECONDS = 120.0
NOTIFICATION_TIMEOUT_SECONDS = 30.0
TOOL_LIST_CHANGED = "notifications/tools/list_changed"
# A proxied tool the committed catalogue does not have, added to the test's copy of it.
NEW_TOOL = {"name": "debug_probe", "method": "debug.probe", "description": "Probes the editor.",
            "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False}, "mutates": False,
            "supportsDryRun": False, "timeoutSeconds": 60}


class UnexpectedFailure(Exception):
    """An exception Bridge.call_tool does not turn into an error result, so it leaves the server's tool handler."""


class NotificationRecorder:
    """The message handler of an SDK client: records the method of every server notification, so a test can wait for
    the next one."""

    def __init__(self) -> None:
        self.methods: list[str] = []
        self.arrived: anyio.Event | None = None

    async def record(self, message: Any) -> None:
        if isinstance(message, Exception) or not isinstance(getattr(message, "method", None), str):
            return
        self.methods.append(message.method)
        if self.arrived is not None:
            self.arrived.set()

    async def wait_for(self, count: int) -> None:
        """Returns once `count` notifications have arrived, or after NOTIFICATION_TIMEOUT_SECONDS."""
        with anyio.move_on_after(NOTIFICATION_TIMEOUT_SECONDS):
            while len(self.methods) < count:
                self.arrived = anyio.Event()
                await self.arrived.wait()


class ToolListTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineToolList-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        # No transcript redirection, and no method coverage: calls answered by the stand-in server prove nothing about
        # the editor's methods (§15.6 gate 5 counts only calls an editor answered).
        excluded = (transcript.TRANSCRIPT_ENVIRONMENT_VARIABLE, engine_client.COVERAGE_ENVIRONMENT_VARIABLE)
        environment = {key: value for key, value in os.environ.items() if key not in excluded}
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        # The bridge reads a copy of the committed catalogue, which the tests change as GenerateDocs.py would.
        self.catalog_path = self.directory / "catalog.json"
        self.catalog_path.write_bytes(server.CATALOG_PATH.read_bytes())
        self.launcher = Launcher(REPOSITORY_ROOT, self.directory / "UserData")
        self.editor = fake_editor.FakeEditor({"session.info": lambda params: {"pid": os.getpid()}})
        self.editor.__enter__()
        self.addCleanup(self.editor.stop)
        self.project = self.directory / "Game"
        self.project.mkdir()
        (self.project / "Game.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
        # editor_launch attaches to the editor serving the project; editor_attach {pid} finds it too.
        self.editor.write_session_file(self.launcher.sessions_directory(),
                                       project_path=(self.project / "Game.eproj").as_posix())

    def make_bridge(self) -> server.Bridge:
        catalog, digest = server.read_catalog(self.catalog_path)
        return server.Bridge(REPOSITORY_ROOT, catalog, self.launcher, self.catalog_path, digest)

    def change_catalog(self, change: Callable[[dict[str, Any]], None]) -> None:
        """Rewrites the catalogue with `change` applied to its document."""
        document = json.loads(self.catalog_path.read_text(encoding="utf-8"))
        change(document)
        self.catalog_path.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")

    def test_the_server_declares_list_changed_in_both_protocol_eras(self) -> None:
        mcp_server, _ = server._build_server(self.make_bridge())
        # The options the stdio server initializes with, and the SDK's in-memory transport too.
        self.assertTrue(mcp_server.create_initialization_options().capabilities.tools.list_changed)

        async def session() -> list[tuple[str, bool | None]]:
            eras = []
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                for mode in ("legacy", "auto"):
                    async with Client(mcp_server, mode=mode) as client:
                        eras.append((client.protocol_version, client.server_capabilities.tools.list_changed))
            return eras

        (handshake_version, handshake_flag), (modern_version, modern_flag) = anyio.run(session)
        self.assertNotIn(handshake_version, MODERN_PROTOCOL_VERSIONS)
        self.assertTrue(handshake_flag)
        # A 2026-07-28 connection learns it from server/discover, which offers it with subscriptions/listen.
        self.assertIn(modern_version, MODERN_PROTOCOL_VERSIONS)
        self.assertTrue(modern_flag)

    def test_a_handshake_client_is_told_to_fetch_the_tool_list_after_initializing(self) -> None:
        # A client that kept the tool list of an earlier bridge process (P10) fetches it again.
        mcp_server, _ = server._build_server(self.make_bridge())
        recorder = NotificationRecorder()

        async def session() -> set[str]:
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(mcp_server, mode="legacy", message_handler=recorder.record, cache=None) as client:
                    await recorder.wait_for(1)
                    return {tool.name for tool in (await client.list_tools()).tools}

        names = anyio.run(session)
        self.assertEqual(recorder.methods, [TOOL_LIST_CHANGED])
        self.assertTrue({"editor_launch", "docs_get"} <= names)

    def test_launch_and_attach_reload_a_changed_catalog_and_notify_a_handshake_client(self) -> None:
        bridge = self.make_bridge()
        mcp_server, _ = server._build_server(bridge)
        recorder = NotificationRecorder()
        launch = {"project": str(self.project)}

        def add_tool(document: dict[str, Any]) -> None:
            document["Tools"].append(NEW_TOOL)

        def describe_docs_get(document: dict[str, Any]) -> None:
            next(tool for tool in document["Tools"] if tool["name"] == "docs_get")["description"] = "Reads a page."

        async def session() -> dict[str, Any]:
            seen: dict[str, Any] = {}
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(mcp_server, mode="legacy", message_handler=recorder.record, cache=None) as client:
                    await recorder.wait_for(1)
                    seen["before"] = {tool.name for tool in (await client.list_tools()).tools}
                    seen["unchanged"] = await client.call_tool("editor_launch", launch)
                    await client.call_tool("editor_shutdown", {})
                    self.change_catalog(add_tool)
                    seen["launched"] = await client.call_tool("editor_launch", launch)
                    await recorder.wait_for(2)
                    seen["after launch"] = {tool.name for tool in (await client.list_tools()).tools}
                    await client.call_tool("editor_shutdown", {})
                    self.change_catalog(describe_docs_get)
                    seen["attached"] = await client.call_tool("editor_attach", {"pid": os.getpid()})
                    await recorder.wait_for(3)
                    tools = (await client.list_tools()).tools
                    seen["after attach"] = next(tool for tool in tools if tool.name == "docs_get").description
                    await client.call_tool("editor_shutdown", {})
            return seen

        seen = anyio.run(session)
        unchanged = seen["unchanged"]
        self.assertFalse(unchanged.is_error, unchanged.content)
        self.assertNotIn("toolListChanged", unchanged.structured_content)
        self.assertNotIn("debug_probe", seen["before"])
        for result in (seen["launched"], seen["attached"]):
            self.assertFalse(result.is_error, result.content)
            self.assertTrue(result.structured_content["toolListChanged"])
            text = "\n".join(block.text for block in result.content if isinstance(block, types.TextContent))
            self.assertIn(TOOL_LIST_CHANGED, text)
        self.assertEqual(seen["after launch"], seen["before"] | {"debug_probe"})
        self.assertEqual(seen["after attach"], "Reads a page.")
        # One notification after initializing and one for each changed catalogue; none for the unchanged one.
        self.assertEqual(recorder.methods, [TOOL_LIST_CHANGED] * 3)
        self.assertEqual(bridge.catalog_revision, 2)

    def test_a_cancelled_or_raising_attach_still_notifies_the_client(self) -> None:
        # The client cancels an attach (Escape during a slow launch) while its worker thread runs: the thread still
        # reloads the changed catalogue and connects, and the client still hears of the change. So does a client whose
        # attach raised after the reload.
        bridge = self.make_bridge()
        mcp_server, _ = server._build_server(bridge)
        recorder = NotificationRecorder()
        entered = threading.Event()
        release = threading.Event()
        self.addCleanup(release.set)
        attach = self.launcher.attach

        def held_attach(**arguments: Any) -> Any:
            entered.set()
            release.wait(SESSION_TIMEOUT_SECONDS)
            return attach(**arguments)

        def failing_attach(**arguments: Any) -> Any:
            raise UnexpectedFailure("the launcher failed")

        async def session() -> dict[str, Any]:
            seen: dict[str, Any] = {}
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(mcp_server, mode="legacy", message_handler=recorder.record, cache=None) as client:
                    await recorder.wait_for(1)
                    self.change_catalog(lambda document: document["Tools"].append(NEW_TOOL))
                    with mock.patch.object(self.launcher, "attach", held_attach):
                        async with anyio.create_task_group() as group:
                            group.start_soon(client.call_tool, "editor_attach", {"pid": os.getpid()})
                            await anyio.to_thread.run_sync(entered.wait, SESSION_TIMEOUT_SECONDS)
                            group.cancel_scope.cancel()
                        # The server reads the client's cancellation before this request, which it answers without
                        # waiting for the cancelled call; only then does the attach go on.
                        await client.list_tools()
                        release.set()
                        seen["status"] = await client.call_tool("editor_status", {})
                    await recorder.wait_for(2)
                    seen["after cancel"] = list(recorder.methods)
                    await client.call_tool("editor_shutdown", {})
                    self.change_catalog(lambda document: document["Tools"].remove(NEW_TOOL))
                    # The SDK answers a handler that raised with an error, and logs it.
                    with (mock.patch.object(self.launcher, "attach", failing_attach),
                          self.assertLogs("mcp", level="ERROR"),
                          self.assertRaisesRegex(MCPError, "the launcher failed")):
                        await client.call_tool("editor_attach", {"pid": os.getpid()})
                    await recorder.wait_for(3)
                    seen["names"] = {tool.name for tool in (await client.list_tools()).tools}
            return seen

        seen = anyio.run(session)
        status = seen["status"]
        self.assertFalse(status.is_error, status.content)
        self.assertIn("session", status.structured_content)
        self.assertEqual(seen["after cancel"], [TOOL_LIST_CHANGED] * 2)
        self.assertEqual(recorder.methods, [TOOL_LIST_CHANGED] * 3)
        self.assertEqual(bridge.catalog_revision, 2)
        self.assertNotIn("debug_probe", seen["names"])

    def test_a_failed_launch_or_attach_reports_what_its_reload_found(self) -> None:
        bridge = self.make_bridge()

        def toggle_tool(document: dict[str, Any]) -> None:
            if NEW_TOOL in document["Tools"]:
                document["Tools"].remove(NEW_TOOL)
            else:
                document["Tools"].append(NEW_TOOL)

        def call(name: str, arguments: dict[str, Any]) -> tuple[dict[str, Any], str]:
            structured, text, is_error = bridge.call_tool(name, arguments)
            self.assertTrue(is_error, text)
            return structured, text

        # Failures the tools return themselves, one the launcher raises, and one while an editor is connected.
        cases = {
            "launch without a project": ("editor_launch", {}, "InvalidParams"),
            "attach to no editor": ("editor_attach", {"pid": os.getpid() + 1}, "LookupError"),
            "attach while connected": ("editor_attach", {"pid": os.getpid()}, "InvalidState"),
        }
        for revision, (case, (name, arguments, error)) in enumerate(cases.items(), start=1):
            with self.subTest(case=case):
                if error == "InvalidState":
                    self.assertFalse(bridge.call_tool("editor_attach", {"pid": os.getpid()})[2])
                self.change_catalog(toggle_tool)
                structured, text = call(name, arguments)
                self.assertEqual(structured["error"], error)
                self.assertTrue(structured["toolListChanged"])
                self.assertNotIn("catalogError", structured)
                self.assertIn(TOOL_LIST_CHANGED, text)
                self.assertEqual(bridge.catalog_revision, revision)
        bridge.call_tool("editor_shutdown", {})

        # A catalogue that cannot be reloaded is reported by a failed launch too.
        self.catalog_path.write_text("{", encoding="utf-8")
        with self.assertLogs("engine_mcp", level="WARNING"):
            structured, text = call("editor_launch", {})
        self.assertEqual(structured["error"], "InvalidParams")
        self.assertNotIn("toolListChanged", structured)
        self.assertIn("cannot read the tool catalogue", structured["catalogError"])
        self.assertIn("the tool list is unchanged", text)

    def test_a_2026_client_listening_for_tool_changes_hears_a_reloaded_catalog(self) -> None:
        mcp_server, _ = server._build_server(self.make_bridge())

        async def session() -> tuple[str, bool | None, Any, Any, Any]:
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(mcp_server) as client:
                    async with client.listen(tools_list_changed=True) as subscription:
                        self.change_catalog(lambda document: document["Tools"].append(NEW_TOOL))
                        attached = await client.call_tool("editor_attach", {"pid": os.getpid()})
                        event = None
                        with anyio.move_on_after(NOTIFICATION_TIMEOUT_SECONDS):
                            event = await subscription.__anext__()
                    listed = await client.list_tools()
                    await client.call_tool("editor_shutdown", {})
                    return client.protocol_version, subscription.honored.tools_list_changed, attached, event, listed

        version, honored, attached, event, listed = anyio.run(session)
        self.assertIn(version, MODERN_PROTOCOL_VERSIONS)
        self.assertTrue(honored)
        self.assertFalse(attached.is_error, attached.content)
        self.assertTrue(attached.structured_content["toolListChanged"])
        self.assertIsInstance(event, ToolsListChanged)
        self.assertIn("debug_probe", {tool.name for tool in listed.tools})
        # The listing went out immediately stale, so the client never serves the tool list from its cache.
        self.assertIn("ttl_ms", listed.model_fields_set)
        self.assertEqual(listed.ttl_ms, 0)

    def test_reload_ignores_unchanged_tools_and_keeps_those_of_an_unreadable_catalog(self) -> None:
        bridge = self.make_bridge()
        tools = bridge.catalog
        # The same bytes with a new modification time, and the same tools written differently, change nothing.
        data = self.catalog_path.read_bytes()
        self.catalog_path.write_bytes(data)
        later = self.catalog_path.stat().st_mtime + 10.0
        os.utime(self.catalog_path, (later, later))
        self.assertFalse(bridge.reload_catalog())
        self.catalog_path.write_text(json.dumps(json.loads(data)) + "\n", encoding="utf-8")
        self.assertFalse(bridge.reload_catalog())
        self.assertIs(bridge.catalog, tools)
        self.assertEqual(bridge.catalog_revision, 0)

        # A catalogue that cannot be read or parsed keeps the listed tools; the launch or attach says why.
        cases = {"invalid JSON": "{", "another version": json.dumps({"Format": "McpCatalog", "Version": 2})}
        for name, content in cases.items():
            with self.subTest(case=name):
                self.catalog_path.write_text(content, encoding="utf-8")
                with self.assertLogs("engine_mcp", level="WARNING"):
                    structured, text, is_error = bridge.call_tool("editor_attach", {"pid": os.getpid()})
                self.assertFalse(is_error, text)
                self.assertNotIn("toolListChanged", structured)
                self.assertIn("cannot read the tool catalogue" if name == "invalid JSON" else "catalogue version",
                              structured["catalogError"])
                self.assertIn("the tool list is unchanged", text)
                self.assertIs(bridge.catalog, tools)
                bridge.call_tool("editor_shutdown", {})
        self.catalog_path.unlink()
        with self.assertLogs("engine_mcp", level="WARNING"):
            self.assertFalse(bridge.reload_catalog())
        self.assertIn("cannot read the tool catalogue", bridge.catalog_error)

        # It is read again at the next launch or attach.
        self.catalog_path.write_bytes(data)
        self.change_catalog(lambda document: document["Tools"].append(NEW_TOOL))
        structured, text, is_error = bridge.call_tool("editor_attach", {"pid": os.getpid()})
        self.assertFalse(is_error, text)
        self.assertTrue(structured["toolListChanged"])
        self.assertNotIn("catalogError", structured)
        self.assertEqual(bridge.catalog_error, "")
        self.assertEqual(list(bridge.catalog), [*tools, "debug_probe"])
        self.assertEqual(bridge.catalog_revision, 1)
        bridge.call_tool("editor_shutdown", {})


if __name__ == "__main__":
    unittest.main()
