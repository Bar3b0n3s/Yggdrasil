"""The bridge's parts without an editor build (Docs/Architecture.md §13.8): result formatting, the catalogue reader, the
connection (transcript lines and their numbers, crash detection) and launch-or-attach, against the stand-in server of
Tests/Automation/fake_editor.py and stand-in processes."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tests" / "Automation"))

import engine_client  # noqa: E402
import fake_editor  # noqa: E402
from engine_mcp import server, transcript  # noqa: E402
from engine_mcp.connection import EditorConnection, EditorCrashed  # noqa: E402
from engine_mcp.launcher import EditorAlreadyOpen, Launcher, LaunchRequest  # noqa: E402

# A stand-in for a launched editor's process: console output, then it waits to be killed.
EDITOR_STAND_IN = "import time\nfor index in range(80):\n    print(f'log {index}', flush=True)\ntime.sleep(120)\n"


class BridgeTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineBridge-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        # No transcript redirection, and no method coverage: calls answered by the stand-in server prove nothing about
        # the editor's methods (§15.6 gate 5 counts only calls an editor answered).
        excluded = (transcript.TRANSCRIPT_ENVIRONMENT_VARIABLE, engine_client.COVERAGE_ENVIRONMENT_VARIABLE)
        environment = {key: value for key, value in os.environ.items() if key not in excluded}
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)

    def start_server(self, handlers: dict[str, fake_editor.Handler]) -> fake_editor.FakeEditor:
        editor = fake_editor.FakeEditor(handlers)
        editor.__enter__()
        self.addCleanup(editor.stop)
        return editor

    def connect(self, editor: fake_editor.FakeEditor) -> engine_client.EngineClient:
        client = engine_client.EngineClient.connect(editor.port, editor.token, client_name="engine-mcp")
        self.addCleanup(client.close)
        return client

    def make_project(self, name: str = "Game") -> Path:
        root = self.directory / name
        root.mkdir()
        (root / f"{name}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
        return root

    @staticmethod
    def read_lines(path: Path) -> list[dict[str, Any]]:
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class FormatTests(BridgeTestCase):
    def test_results_carry_structured_content_and_the_meta_delta(self) -> None:
        meta = {"revision": 4, "dirty": True}
        structured, text = server.format_result({"jsonrpc": "2.0", "id": 1,
                                                 "result": {"text": "Main.scene  rev 4", "scene": {"name": "Main"},
                                                            "_meta": meta}})
        self.assertEqual(structured["_meta"], meta)
        self.assertTrue(text.startswith("Main.scene  rev 4"))
        self.assertIn('_meta: {"revision": 4, "dirty": true}', text)

        error = {"code": -32602, "message": "Invalid params",
                 "data": {"errorCode": "InvalidArgument", "detail": "bad field", "failedOp": 1, "_meta": meta,
                          "issues": [{"pointer": "/components/RigidBody/Mas", "message": "unknown field 'Mas'",
                                      "hint": "did you mean 'Mass'?"}]}}
        structured, text = server.format_result({"jsonrpc": "2.0", "id": 2, "error": error})
        self.assertEqual((structured["error"], structured["code"]), ("InvalidArgument", -32602))
        self.assertIn("did you mean 'Mass'?", text)
        self.assertIn('"failedOp": 1', text)
        self.assertIn("_meta", text)

        offloaded = {"path": "C:/Game/Library/Automation/Out/1-2-00000001.json", "truncated": True,
                     "summary": {"scene": {"type": "object"}}, "_meta": meta}
        _, text = server.format_result({"jsonrpc": "2.0", "id": 3, "result": offloaded})
        self.assertIn("Out/1-2-00000001.json", text.splitlines()[0])

        _, text = server.format_result({"jsonrpc": "2.0", "id": 4, "result": {"text": "x" * 100000, "_meta": meta}})
        self.assertLessEqual(len(text.encode("utf-8")), server.MAX_TEXT_BYTES)
        self.assertIn("structuredContent", text)
        self.assertTrue(text.endswith('_meta: {"revision": 4, "dirty": true}'))

    def test_catalog_reader_checks_format_and_entries(self) -> None:
        self.assertIsInstance(server.load_catalog(), list)
        cases = {
            "format": {"Format": "Other", "Version": 1, "Tools": []},
            "version": {"Format": "McpCatalog", "Version": 2, "Tools": []},
            "entry": {"Format": "McpCatalog", "Version": 1, "Tools": [{"name": "scene_tree"}]},
            "local name": {"Format": "McpCatalog", "Version": 1, "Tools": [
                {"name": "engine_call", "method": "x.y", "description": "d", "inputSchema": {}, "mutates": False,
                 "supportsDryRun": False, "timeoutSeconds": 60}]},
        }
        for name, document in cases.items():
            with self.subTest(case=name):
                path = self.directory / f"{name}.json"
                path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(ValueError):
                    server.load_catalog(path)


class TranscriptLockingTests(BridgeTestCase):
    def test_two_writers_number_lines_consistently(self) -> None:
        root = self.make_project()
        path = root / transcript.TRANSCRIPT_RELATIVE_PATH
        path.parent.mkdir(parents=True)
        path.write_bytes(b'{"type": "request"}\n{"torn": ')
        first = transcript.Transcript.open(root)
        second = transcript.Transcript.open(root)
        self.assertEqual(first.append_request(1, "scene.tree", {}), 3)
        self.assertEqual(second.append_request(1, "scene.tree", {}), 4)
        first.append_response(1, 3, {"jsonrpc": "2.0", "id": 1, "result": {}})
        self.assertEqual(second.append_request(2, "entity.create", {"name": "A"}), 6)
        self.assertEqual(len(path.read_text(encoding="utf-8").splitlines()), 6)
        with self.assertRaises(ValueError):
            first.append_request(3, "session.hello", {"token": "secret"})
        self.assertNotIn("secret", path.read_text(encoding="utf-8"))


class ConnectionTests(BridgeTestCase):
    def test_calls_are_written_to_the_transcript_with_their_line(self) -> None:
        root = self.make_project()
        editor = self.start_server({"entity.create": lambda params: {"entity": {"id": "5d1c9a7e33b04f12"},
                                                                     "params": params}})
        client = self.connect(editor)
        connection = EditorConnection(root, supervised=False, client=client)
        response = connection.call("entity.create", {"name": "Board"})
        self.assertEqual(response["result"]["params"]["_meta"], {"transcriptLine": 1})
        failed = connection.call("no.such", {})
        self.assertIn("error", failed)
        connection.disconnect()
        lines = self.read_lines(root / "Automation" / "BuildLog.jsonl")
        self.assertEqual([line["type"] for line in lines], ["request", "response", "request", "response"])
        self.assertEqual(lines[0]["params"], {"name": "Board"})
        self.assertEqual(lines[0]["id"], response["id"])
        self.assertEqual(lines[1]["requestLine"], 1)
        self.assertIn("5d1c9a7e33b04f12", lines[1]["summary"])
        self.assertEqual((lines[3]["ok"], lines[3]["error"]["code"]), (False, engine_client.METHOD_NOT_FOUND))
        self.assertEqual(editor.requests[1].params["_meta"], {"transcriptLine": 3})

    def test_a_created_project_gets_the_transcript_of_its_first_request(self) -> None:
        def create(params: dict[str, Any]) -> dict[str, Any]:
            # As the editor does: a new or empty directory only, and the project file is what makes it a project.
            root = Path(params["path"])
            if root.exists() and any(root.iterdir()):
                names = sorted(path.name for path in root.iterdir())
                raise fake_editor.FakeError(engine_client.CONFLICT, "AlreadyExists", f"the directory is not empty: {names}")
            root.mkdir(exist_ok=True)
            (root / f"{params['name']}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
            return {"project": {"name": params["name"], "root": root.as_posix()}, "createdFiles": []}

        editor = self.start_server({"project.create": create, "scene.new": lambda params: {}})
        client = self.connect(editor)
        connection = EditorConnection(None, supervised=False, client=client)
        # A refused call leaves nothing in a directory that is not a project.
        other = self.directory / "Other"
        other.mkdir()
        (other / "notes.txt").write_text("not a project", encoding="utf-8")
        refused = connection.call("project.create", {"path": str(other), "name": "Other"})
        self.assertIn("error", refused)
        self.assertIsNone(connection.transcript)
        self.assertEqual(sorted(path.name for path in other.iterdir()), ["notes.txt"])
        # An existing empty directory is accepted: the bridge created nothing in it before the editor's check.
        game = self.directory / "Game"
        game.mkdir()
        created = connection.call("project.create", {"path": str(game), "name": "Game"})
        self.assertIn("result", created, created)
        connection.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        lines = self.read_lines(game / "Automation" / "BuildLog.jsonl")
        self.assertEqual([line.get("method", line["type"]) for line in lines],
                         ["project.create", "response", "scene.new", "response"])
        self.assertEqual(editor.requests[1].params["_meta"], {"transcriptLine": 1})
        self.assertEqual(editor.requests[2].params["_meta"], {"transcriptLine": 3})
        self.assertEqual(connection.project_root, game)

    def test_an_opened_project_continues_its_transcript_and_a_refused_open_writes_nothing(self) -> None:
        root = self.make_project()
        path = root / transcript.TRANSCRIPT_RELATIVE_PATH
        path.parent.mkdir(parents=True)
        path.write_bytes(b'{"type": "request"}\n{"type": "response"}\n')

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            project = Path(params["path"])
            if not any(project.glob("*.eproj")):
                raise fake_editor.FakeError(engine_client.NOT_FOUND, "NotFound", f"no project file in {project}")
            return {"project": {"name": project.name, "root": project.as_posix()}}

        editor = self.start_server({"project.open": open_project})
        client = self.connect(editor)
        connection = EditorConnection(None, supervised=False, client=client)
        plain = self.directory / "Plain"
        plain.mkdir()
        self.assertIn("error", connection.call("project.open", {"path": str(plain)}))
        self.assertEqual(list(plain.iterdir()), [])
        self.assertIn("result", connection.call("project.open", {"path": str(root)}))
        self.assertEqual(editor.requests[1].params["_meta"], {"transcriptLine": 3})
        lines = self.read_lines(path)
        self.assertEqual([line.get("method", line["type"]) for line in lines],
                         ["request", "response", "project.open", "response"])
        self.assertEqual(lines[3]["requestLine"], 3)

    def test_a_dead_supervised_editor_is_reported_as_crashed(self) -> None:
        user_data = self.directory / "UserData"
        crashes = user_data / "App" / "Crashes"
        process = engine_client.launch_editor(fake_editor.PYTHON, ["-c", EDITOR_STAND_IN], user_data, "App",
                                              wait_for_session=False)
        self.addCleanup(process.kill)

        def crash(params: dict[str, Any]) -> dict[str, Any]:
            crashes.mkdir(parents=True)
            (crashes / "Crash-1.txt").write_text("report", encoding="utf-8")
            process.kill()
            raise fake_editor.CloseConnection()

        editor = self.start_server({"session.info": lambda params: {"pid": process.process.pid}, "debug.crash": crash})
        client = self.connect(editor)
        connection = EditorConnection(self.make_project(), supervised=True, client=client, editor=process,
                                      app_name="App")
        self.assertIn("result", connection.call("session.info", {}))
        self.assertTrue(connection.is_alive())
        deadline = time.monotonic() + 30.0
        while len(process.output_tail) < 80 and time.monotonic() < deadline:
            time.sleep(engine_client.POLL_INTERVAL_SECONDS)
        with self.assertRaises(EditorCrashed) as raised:
            connection.call("debug.crash", {})
        crashed = raised.exception.to_json()
        self.assertEqual(crashed["error"], "EditorCrashed")
        self.assertEqual(crashed["exitCode"], process.process.returncode)
        self.assertEqual(len(crashed["lastLogLines"]), 50)
        self.assertEqual(crashed["lastLogLines"][-1], "log 79")
        self.assertTrue(crashed["crashReportPath"].endswith("Crashes/Crash-1.txt"))
        self.assertFalse(crashed["autosaveAvailable"])
        self.assertFalse(connection.is_alive())
        # Every later call reports the crash again, until editor_launch starts a new editor.
        with self.assertRaises(EditorCrashed):
            connection.call("session.info", {})
        lines = self.read_lines(connection.transcript.path) if connection.transcript else []
        self.assertEqual(lines[-1]["error"]["errorCode"], "EditorCrashed")


class EngineMethodsTests(BridgeTestCase):
    def test_engine_methods_reads_an_offloaded_catalogue(self) -> None:
        # The unfiltered rpc.discover is over the offload threshold, so the editor answers with the file it wrote (ADR 0008
        # decision 31); a filtered one fits inline.
        methods = [{"name": "entity.create", "description": "Creates an entity.", "mutates": True, "params": {}},
                   {"name": "scene.tree", "description": "The hierarchy.", "mutates": False, "params": {}}]
        offloaded = self.directory / "1-2-00000001.json"
        offloaded.write_text(json.dumps({"methods": methods}), encoding="utf-8")

        def discover(params: dict[str, Any]) -> dict[str, Any]:
            if "domain" in params:
                return {"methods": [method for method in methods if method["name"].startswith(params["domain"] + ".")]}
            return {"path": offloaded.as_posix(), "truncated": True, "summary": {"methods": {"type": "array", "count": 2}}}

        editor = self.start_server({"rpc.discover": discover})
        bridge = server.Bridge(REPOSITORY_ROOT, [], Launcher(REPOSITORY_ROOT, self.directory / "UserData"))
        bridge.connection = EditorConnection(None, supervised=False, client=self.connect(editor))
        structured, text, is_error = bridge.call_tool("engine_methods", {})
        self.assertFalse(is_error, text)
        self.assertEqual([method["name"] for method in structured["methods"]], ["entity.create", "scene.tree"])
        self.assertTrue(structured["methods"][0]["mutates"])
        self.assertIn("revision", structured["_meta"])
        structured, _, is_error = bridge.call_tool("engine_methods", {"domain": "scene"})
        self.assertFalse(is_error)
        self.assertEqual([method["name"] for method in structured["methods"]], ["scene.tree"])


class LauncherTests(BridgeTestCase):
    def make_launcher(self) -> tuple[Launcher, Path]:
        launcher = Launcher(REPOSITORY_ROOT, self.directory / "UserData")
        return launcher, launcher.sessions_directory()

    def test_launch_attaches_to_the_session_serving_the_project(self) -> None:
        root = self.make_project()
        editor = self.start_server({"session.info": lambda params: {"pid": os.getpid()}})
        launcher, sessions = self.make_launcher()
        editor.write_session_file(sessions, project_path=(root / "Game.eproj").as_posix())
        connection = launcher.launch(LaunchRequest(project=root))
        self.assertFalse(connection.supervised)
        self.assertEqual(connection.project_root, root)
        self.assertEqual(connection.call("session.info", {})["result"]["pid"], os.getpid())
        self.assertEqual(connection.shutdown()["shutDown"], False)
        for attached in (launcher.attach(pid=os.getpid()), launcher.attach(project=root / "Game.eproj")):
            self.assertEqual(attached.session.pid if attached.session else None, os.getpid())
            attached.disconnect()
        with self.assertRaises(ValueError):
            launcher.attach()
        with self.assertRaises(LookupError):
            launcher.attach(pid=finished_pid())
        self.assertTrue((root / "Automation" / "BuildLog.jsonl").is_file())

    def test_a_held_lock_without_a_session_is_editor_already_open(self) -> None:
        root = self.make_project()
        launcher, _ = self.make_launcher()
        holder = fake_editor.hold_project_lock(root)
        try:
            with self.assertRaises(EditorAlreadyOpen) as raised:
                launcher.launch(LaunchRequest(project=root / "Game.eproj"))
            self.assertEqual(raised.exception.pid, holder.pid)
            self.assertIn("Allow AI automation", raised.exception.to_json()["hint"])
            with self.assertRaises(LookupError) as lookup:
                launcher.attach(project=root)
            self.assertIn(str(holder.pid), str(lookup.exception))
        finally:
            fake_editor.release_project_lock(holder)

    def test_missing_projects_and_binaries_are_reported(self) -> None:
        launcher, _ = self.make_launcher()
        with self.assertRaises(FileNotFoundError):
            launcher.launch(LaunchRequest(project=self.directory / "Missing"))
        with self.assertRaises(ValueError):
            launcher.launch(LaunchRequest(project=self.directory / "Missing", create=True, renderer="metal"))
        repository = self.directory / "Repository"
        repository.mkdir()
        (repository / "premake5.lua").write_text('WorkspaceName = "Sample"\n', encoding="utf-8")
        with self.assertRaises(FileNotFoundError) as raised:
            Launcher(repository, self.directory / "UserData").launch(
                LaunchRequest(project=self.directory / "New", create=True))
        self.assertIn("python Scripts/Build.py --config Release --project Editor", str(raised.exception))


def finished_pid() -> int:
    """The pid of a process that has exited."""
    process = subprocess.Popen([str(fake_editor.PYTHON), "-c", "pass"])
    process.wait()
    return process.pid


if __name__ == "__main__":
    unittest.main()
