"""MCP conformance through the official SDK's client (Docs/Architecture.md §15.7, Roadmap M4): the catalogue tools are
listed without an editor, a handshake-era client is told to fetch the tool list again once it has initialized (§13.8
"Tool list refresh"), editor_launch starts a headless editor, entity_create, scene_tree and asset_get_properties work,
a killed editor makes the next call return EditorCrashed, and the generated .mcp.json starts the bridge with no python
on PATH. A rendering editor's viewport_screenshot returns an image content block (Roadmap M5;
Docs/Decisions/0009-m5-decisions.md decision 33); that test needs a Vulkan device, which it probes for like the
automation suite's rendering tests (Tests/Automation/harness.py, require_gpu).

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

import base64

import anyio
from mcp import Client, StdioServerParameters

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tests" / "Automation"))

import engine_client  # noqa: E402
import fake_editor  # noqa: E402
import harness  # noqa: E402
import lock_holder  # noqa: E402
from engine_mcp import server  # noqa: E402
from Lib import locks  # noqa: E402  (engine_mcp puts Scripts/ on sys.path)

# The bridge's settings, which the SDK's stdio client (it passes only an allow-listed environment) must hand on: the
# machine-wide locks' held tokens and directory among them, so a bridge that Test.py (holding the GPU lock) or CI.py
# (holding every lock) starts runs its editor launches under that hold instead of waiting for it (Scripts/Lib/locks.py).
BRIDGE_ENVIRONMENT_VARIABLES = ("ENGINE_MCP_TRANSCRIPT", "ENGINE_MCP_USER_DATA_DIR", "ENGINE_AUTOMATION_CONFIG",
                                "ENGINE_AUTOMATION_COVERAGE", locks.HELD_LOCKS_VARIABLE, locks.LOCK_DIRECTORY_VARIABLE)
SESSION_TIMEOUT_SECONDS = 300.0
NOTIFICATION_TIMEOUT_SECONDS = 30.0
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

    def test_the_bridge_inherits_the_machine_locks_its_starter_holds(self) -> None:
        # The review's blocker: the SDK starts the server with its default environment and env only, so without the
        # held tokens a bridge under Test.py's GPU hold waited for that hold (its own grandparent's) on a rendering
        # launch, and under CI.py's gate on every launch, until the session timed out.
        directory = lock_holder.isolated_lock_directory(self)
        with lock_holder.LockHolder(locks.GPU) as holder:
            holder.ready()
            record = locks.read_record(locks.lock_paths(locks.GPU)[0], locks.GPU)
            self.assertIsNotNone(record)
            os.environ[locks.HELD_LOCKS_VARIABLE] = f"{locks.GPU}={record.token if record else ''}"
            environment = self.bridge_environment(None)
            self.assertEqual(environment.get(locks.HELD_LOCKS_VARIABLE), os.environ[locks.HELD_LOCKS_VARIABLE])
            self.assertEqual(environment.get(locks.LOCK_DIRECTORY_VARIABLE), str(directory))
            # A process started with that environment alone, as the SDK starts the bridge, runs under the hold.
            child = lock_holder.LockHolder(locks.GPU, environment={**self.sdk_default_environment(), **environment})
            try:
                self.assertEqual(child.wait_for("held", "unavailable"), "held gpu exclusive inherited")
                child.ready()
            finally:
                child.release()
                child.stop()
            holder.release()

    @staticmethod
    def sdk_default_environment() -> dict[str, str]:
        """The environment the MCP SDK's stdio client gives a server before its env (mcp.client.stdio)."""
        from mcp.client.stdio import get_default_environment  # noqa: PLC0415  (the SDK's own allow-list)

        return get_default_environment()

    def test_sdk_client_lists_catalog_tools_without_editor(self) -> None:
        names = self.list_tool_names()
        self.assertTrue(set(server.LOCAL_TOOL_NAMES) <= names)
        self.assertTrue({tool.name for tool in server.load_catalog()} <= names)

    def test_sdk_client_is_told_to_fetch_the_tool_list_after_initializing(self) -> None:
        # The stdio bridge declares tools.listChanged, and a handshake-era client (one that opens with initialize) that
        # kept the tool list of an earlier bridge process fetches it again (Docs/Decisions/0026-m16-contract.md, P10;
        # test_tool_list.py covers the rest of the refresh in process).
        parameters = StdioServerParameters(command=sys.executable, args=[str(MCP_ROOT / "run.py")],
                                           env=self.bridge_environment(None), cwd=str(REPOSITORY_ROOT))

        async def session() -> tuple[bool | None, list[str]]:
            methods: list[str] = []
            arrived = anyio.Event()

            async def record(message: Any) -> None:
                method = getattr(message, "method", None)
                if isinstance(method, str):
                    methods.append(method)
                    arrived.set()

            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(parameters, mode="legacy", message_handler=record, cache=None) as client:
                    # Bounds only a notification that never comes (the assertions then report that none arrived).
                    with anyio.move_on_after(NOTIFICATION_TIMEOUT_SECONDS):
                        await arrived.wait()
                    return client.server_capabilities.tools.list_changed, methods

        list_changed, methods = anyio.run(session)
        self.assertTrue(list_changed)
        self.assertEqual(methods, ["notifications/tools/list_changed"])

    def test_sdk_client_launches_an_editor_and_calls_tools(self) -> None:
        results = self.run_session([
            ("editor_launch", {"project": str(self.directory / "Game"), "create": True, "template": "Empty"}),
            ("scene_new", {"path": "Assets/Scenes/Main.scene"}),
            ("entity_create", {"name": "Board"}),
            ("scene_tree", {}),
            ("engine_methods", {}),
            ("asset_create", {"type": "Material", "path": "Assets/Materials/Red.material",
                              "values": {"Roughness": 0.25}}),
            ("asset_get_properties", {"asset": "Assets/Materials/Red.material"}),
        ])
        self.assertIn("Board", results[3]["structuredContent"]["text"])
        self.assertIn("_meta", results[3]["text"])
        # Every registered method, read from the file the editor offloads the unfiltered catalogue to.
        self.assertFalse(results[4]["isError"], results[4]["text"])
        names = {method["name"] for method in results[4]["structuredContent"]["methods"]}
        self.assertGreaterEqual(len(names), MIN_EDITOR_METHODS)
        self.assertTrue({"entity.create", "rpc.discover", "project.upgrade"} <= names, sorted(names))
        # M17 friction F11: a material is read through its own tool, not engine_call.
        self.assertFalse(results[5]["isError"], results[5]["text"])
        self.assertFalse(results[6]["isError"], results[6]["text"])
        self.assertEqual(results[6]["structuredContent"]["asset"]["path"], "Assets/Materials/Red.material")
        self.assertEqual(results[6]["structuredContent"]["values"]["Roughness"], 0.25)

    def test_sdk_client_receives_an_image_from_viewport_screenshot(self) -> None:
        reason = harness.probe_rendering_editor()
        if reason is not None:
            if os.environ.get(harness.REQUIRE_GPU_VARIABLE) == "1":
                self.fail(f"no usable Vulkan device for a rendering editor ({harness.REQUIRE_GPU_VARIABLE}=1): "
                          f"{reason}")
            print(f"{harness.NO_DEVICE_PREFIX}{self.id()}: {reason}", file=sys.stderr, flush=True)
            return
        # The scene view renders the open scene (Docs/Decisions/0012-m7-decisions.md decision 9), so one is created
        # first.
        results = self.run_session([
            ("editor_launch", {"project": str(self.directory / "Game"), "create": True, "renderer": "vulkan"}),
            ("scene_new", {"path": "Assets/Scenes/Main.scene"}),
            ("viewport_screenshot", {"view": "scene", "maxDimension": 64}),
            ("viewport_screenshot", {"view": "scene", "maxDimension": 64, "inline": True}),
        ])
        self.assertFalse(results[1]["isError"], results[1]["text"])
        for shot in results[2:]:
            self.assertFalse(shot["isError"], shot["text"])
            self.assertEqual((shot["structuredContent"]["width"], shot["structuredContent"]["height"]), (64, 36))
            self.assertIn(shot["structuredContent"]["path"], shot["text"])
            self.assertEqual(len(shot["images"]), 1)
            image = shot["images"][0]
            self.assertEqual(image["mimeType"], "image/png")
            png = base64.b64decode(image["data"], validate=True)
            self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
            self.assertEqual(png, Path(shot["structuredContent"]["path"]).read_bytes())
        # The inline base64 travels once, as the image content block.
        self.assertNotIn("data", results[3]["structuredContent"])
        self.assertFalse(results[3]["structuredContent"]["inlineOmitted"])

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
        each result as {"structuredContent": ..., "text": ..., "isError": ..., "images": [{mimeType, data}]}. At the
        end, editor_shutdown {force: true} shuts down a launched editor that still runs (and only disconnects from an
        attached one)."""
        parameters = StdioServerParameters(command=sys.executable, args=[str(MCP_ROOT / "run.py")],
                                           env=self.bridge_environment(None, count_coverage), cwd=str(REPOSITORY_ROOT))

        async def call(client: Any, name: str, arguments: dict[str, object]) -> dict[str, Any]:
            result = await client.call_tool(name, arguments)
            text = "\n".join(block.text for block in result.content if getattr(block, "type", "") == "text")
            images = [{"mimeType": block.mime_type, "data": block.data} for block in result.content
                      if getattr(block, "type", "") == "image"]
            return {"structuredContent": result.structured_content, "text": text, "isError": result.is_error,
                    "images": images}

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
