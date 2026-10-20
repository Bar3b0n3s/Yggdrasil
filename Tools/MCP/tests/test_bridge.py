"""The bridge's parts without an editor build (Docs/Architecture.md §13.8): result formatting, the catalogue reader, the
connection (transcript lines and their numbers, crash detection), launch-or-attach and the machine-wide locks a launch
takes, against the stand-in server of Tests/Automation/fake_editor.py and stand-in processes."""

from __future__ import annotations

import base64
import contextlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from collections.abc import Callable, Iterator
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
import lock_holder  # noqa: E402
from engine_mcp import server, transcript  # noqa: E402
from engine_mcp.connection import EditorConnection, EditorCrashed  # noqa: E402
from engine_mcp.launcher import EditorAlreadyOpen, Launcher, LaunchRequest  # noqa: E402
from Lib import locks  # noqa: E402  (engine_mcp puts Scripts/ on sys.path)

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
        # Launches against stand-ins take the machine-wide locks of a directory of their own, never the machine's.
        lock_holder.isolated_lock_directory(self)

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
        self.assertIn("result_read", text.splitlines()[0])

        _, text = server.format_result({"jsonrpc": "2.0", "id": 4, "result": {"text": "x" * 100000, "_meta": meta}})
        self.assertLessEqual(len(text.encode("utf-8")), server.MAX_TEXT_BYTES)
        self.assertIn("structuredContent", text)
        self.assertTrue(text.endswith('_meta: {"revision": 4, "dirty": true}'))

    def test_screenshot_results_return_image_content_and_name_the_file(self) -> None:
        png = self.directory / "1-2-00000001.png"
        png.write_bytes(b"\x89PNG\r\n\x1a\npixels")
        meta = {"revision": 1}
        shot = {"view": "Scene", "path": str(png), "mimeType": "image/png", "width": 320, "height": 180, "data": "",
                "_meta": meta}
        structured, text = server.format_result({"jsonrpc": "2.0", "id": 1, "result": shot})
        self.assertIn(f"Screenshot 320x180: {png}", text)
        self.assertEqual(server.image_content(structured), (base64.b64encode(png.read_bytes()).decode("ascii"),
                                                            "image/png"))

        # Inline data is used as it is and left out of the text summary.
        inline = dict(shot, data="aW5saW5l")
        structured, text = server.format_result({"jsonrpc": "2.0", "id": 2, "result": inline})
        self.assertNotIn("aW5saW5l", text)
        self.assertEqual(server.image_content(structured), ("aW5saW5l", "image/png"))

        # The base64 travels once: the structuredContent of the tool result leaves out the data the image block carries.
        content, image = server.tool_content(structured, False)
        self.assertEqual(image, ("aW5saW5l", "image/png"))
        self.assertNotIn("data", content)
        self.assertEqual((content["path"], content["mimeType"], content["_meta"]), (str(png), "image/png", meta))
        self.assertIn("data", structured)
        self.assertEqual(server.tool_content(structured, True), (structured, None))

        # Inline data the editor left out (a PNG over 30 KB): the image comes from the file.
        omitted = dict(shot, inlineOmitted=True)
        structured, _ = server.format_result({"jsonrpc": "2.0", "id": 3, "result": omitted})
        content, image = server.tool_content(structured, False)
        self.assertEqual(image, (base64.b64encode(png.read_bytes()).decode("ascii"), "image/png"))
        self.assertTrue(content["inlineOmitted"])

        # Other results, and a file that is gone, carry no image.
        self.assertIsNone(server.image_content({"path": str(png), "truncated": True}))
        self.assertIsNone(server.image_content(dict(shot, path=str(self.directory / "Missing.png"))))

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
                raise fake_editor.FakeError(engine_client.CONFLICT, "AlreadyExists",
                                            f"the directory is not empty: {names}")
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

    def test_a_refused_project_call_that_wrote_a_project_keeps_its_transcript_lines(self) -> None:
        # The milestone review's R05: project.create records the files it created before its open, so a create whose
        # open then failed left entries naming line 1, and the bridge dropped that line; the next request took its
        # number.
        def create(params: dict[str, Any]) -> dict[str, Any]:
            root = Path(params["path"])
            root.mkdir(exist_ok=True)
            (root / f"{params['name']}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
            raise fake_editor.FakeError(engine_client.INTERNAL, "IoError", "a .meta write failed after the create")

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            project = Path(params["path"])
            return {"project": {"name": project.name, "root": project.as_posix()}}

        editor = self.start_server({"project.create": create, "project.open": open_project})
        connection = EditorConnection(None, supervised=False, client=self.connect(editor))
        game = self.directory / "Game"
        refused = connection.call("project.create", {"path": str(game), "name": "Game"})
        self.assertIn("error", refused)
        self.assertIsNone(connection.transcript)
        self.assertIsNone(connection.project_root)
        path = game / transcript.TRANSCRIPT_RELATIVE_PATH
        lines = self.read_lines(path)
        self.assertEqual([(line["type"], line.get("method")) for line in lines],
                         [("request", "project.create"), ("response", None)])
        self.assertEqual((lines[1]["requestLine"], lines[1]["ok"], lines[1]["error"]["errorCode"]),
                         (1, False, "IoError"))
        self.assertEqual(editor.requests[0].params["_meta"], {"transcriptLine": 1})
        # The next project call continues the transcript instead of taking line 1 again.
        self.assertIn("result", connection.call("project.open", {"path": str(game)}))
        self.assertEqual(editor.requests[1].params["_meta"], {"transcriptLine": 3})
        self.assertEqual([line.get("method", line["type"]) for line in self.read_lines(path)],
                         ["project.create", "response", "project.open", "response"])

    def test_a_timed_out_project_call_writes_its_lines_once_the_project_exists(self) -> None:
        # The milestone review's R31: a project.open or project.create the bridge stopped waiting for left the
        # transcript pending for good, so every later line stayed in memory while the editor recorded provenance at it.
        release = threading.Event()
        created = threading.Event()

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            release.wait(60.0)
            project = Path(params["path"])
            return {"project": {"name": project.name, "root": project.as_posix()}}

        def create(params: dict[str, Any]) -> dict[str, Any]:
            # The project file appears only after the bridge stopped waiting.
            release.wait(60.0)
            root = Path(params["path"])
            root.mkdir(exist_ok=True)
            (root / f"{params['name']}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
            created.set()
            return {"project": {"name": params["name"], "root": root.as_posix()}, "createdFiles": []}

        editor = self.start_server({"project.open": open_project, "project.create": create,
                                    "scene.new": lambda params: {}})
        # An existing project's open: its project file is there, so the lines are written when the call times out.
        root = self.make_project()
        connection = EditorConnection(None, supervised=False, client=self.connect(editor))
        with self.assertRaises(TimeoutError):
            connection.call("project.open", {"path": str(root)}, timeout=0.2)
        path = root / transcript.TRANSCRIPT_RELATIVE_PATH
        self.assertEqual([(line["type"], line.get("method")) for line in self.read_lines(path)],
                         [("request", "project.open"), ("response", None)])
        self.assertEqual(self.read_lines(path)[1]["error"]["errorCode"], "Timeout")
        release.set()
        self.assertIn("result", connection.call("scene.new", {"path": "Assets/Scenes/Main.scene"}))
        self.assertEqual(editor.requests[1].params["_meta"], {"transcriptLine": 3})
        self.assertEqual([line.get("method", line["type"]) for line in self.read_lines(path)],
                         ["project.open", "response", "scene.new", "response"])

        # A create whose project file appears after the timeout: the lines are written at the next call.
        release.clear()
        game = self.directory / "New"
        creating = EditorConnection(None, supervised=False, client=self.connect(editor))
        with self.assertRaises(TimeoutError):
            creating.call("project.create", {"path": str(game), "name": "New"}, timeout=0.2)
        self.assertTrue(creating.transcript is not None and creating.transcript.is_pending)
        self.assertFalse((game / transcript.TRANSCRIPT_RELATIVE_PATH).exists())
        release.set()
        self.assertTrue(created.wait(60.0))
        self.assertIn("result", creating.call("scene.new", {"path": "Assets/Scenes/Main.scene"}))
        lines = self.read_lines(game / transcript.TRANSCRIPT_RELATIVE_PATH)
        self.assertEqual([line.get("method", line["type"]) for line in lines],
                         ["project.create", "response", "scene.new", "response"])
        self.assertEqual(editor.requests[-1].params["_meta"], {"transcriptLine": 3})

        # A create that timed out and never made its project: the next project call, for another directory, starts
        # that directory's transcript afresh, and nothing is written to the first one.
        release.clear()
        abandoned = self.directory / "Abandoned"
        retrying = EditorConnection(None, supervised=False, client=self.connect(editor))
        with self.assertRaises(TimeoutError):
            retrying.call("project.open", {"path": str(abandoned)}, timeout=0.2)
        release.set()
        other = self.make_project("Other")
        self.assertIn("result", retrying.call("project.open", {"path": str(other)}))
        self.assertEqual(editor.requests[-1].params["_meta"], {"transcriptLine": 1})
        self.assertFalse(abandoned.exists())
        self.assertEqual([line.get("method", line["type"])
                          for line in self.read_lines(other / transcript.TRANSCRIPT_RELATIVE_PATH)],
                         ["project.open", "response"])

    def test_a_timed_out_project_create_writes_its_lines_when_the_connection_closes(self) -> None:
        # The fix check's follow-up to R31: lines still pending when the connection ends without another call (an
        # attached editor's shutdown, a new launch or attach, the bridge's exit) were dropped although the editor had
        # recorded provenance at them.
        release = threading.Event()
        created = threading.Event()

        def create(params: dict[str, Any]) -> dict[str, Any]:
            release.wait(60.0)
            root = Path(params["path"])
            root.mkdir(exist_ok=True)
            (root / f"{params['name']}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
            created.set()
            return {"project": {"name": params["name"], "root": root.as_posix()}, "createdFiles": []}

        editor = self.start_server({"project.create": create})
        game = self.directory / "Closed"
        connection = EditorConnection(None, supervised=False, client=self.connect(editor))
        with self.assertRaises(TimeoutError):
            connection.call("project.create", {"path": str(game), "name": "Closed"}, timeout=0.2)
        self.assertTrue(connection.transcript is not None and connection.transcript.is_pending)
        release.set()
        self.assertTrue(created.wait(60.0))
        self.assertEqual(connection.shutdown()["shutDown"], False)
        lines = self.read_lines(game / transcript.TRANSCRIPT_RELATIVE_PATH)
        self.assertEqual([(line["type"], line.get("method")) for line in lines],
                         [("request", "project.create"), ("response", None)])

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

    def test_a_crash_with_an_autosave_points_to_recovery(self) -> None:
        crashed = EditorCrashed(exit_code=4, last_log_lines=[], crash_report_path="", autosave_available=True)
        self.assertIn("editor_launch with recover: true", crashed.to_json()["hint"])
        self.assertNotIn("recover", EditorCrashed(4, [], "", False).to_json()["hint"])


class ProjectPathTests(BridgeTestCase):
    """Docs/Decisions/0026-m16-contract.md, friction S1-16 and prediction P09: the committed transcript names a project
    inside the repository by its repository-relative path, and a relative project path means one below the repository
    root, whatever the bridge's working directory."""

    def make_repository(self) -> Path:
        repository = self.directory / "Repository"
        repository.mkdir()
        (repository / "premake5.lua").write_text('WorkspaceName = "Sample"\n', encoding="utf-8")
        return repository

    def enter_directory(self, directory: Path) -> None:
        """Runs the rest of the test with `directory` as the working directory."""
        previous = Path.cwd()
        os.chdir(directory)
        self.addCleanup(os.chdir, previous)

    def test_project_paths_are_recorded_relative_to_the_repository(self) -> None:
        repository = self.make_repository()
        root = repository / "Projects" / "Game"
        root.mkdir(parents=True)
        (root / "Game.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
        outside = self.make_project("Outside")

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            project = Path(params["path"])
            return {"project": {"name": project.name, "root": project.as_posix()}}

        editor = self.start_server({"project.open": open_project})
        self.enter_directory(outside)
        connection = EditorConnection(None, supervised=False, client=self.connect(editor), repository_root=repository)
        # A relative path is the repository's, not the working directory's; the editor gets it absolute.
        self.assertIn("result", connection.call("project.open", {"path": "Projects/Game"}))
        self.assertEqual(Path(editor.requests[0].params["path"]), root.resolve())
        self.assertIn("result", connection.call("project.open", {"path": str(root)}))
        self.assertEqual(Path(editor.requests[1].params["path"]), root)
        lines = self.read_lines(root / transcript.TRANSCRIPT_RELATIVE_PATH)
        self.assertEqual([line["params"]["path"] for line in lines if line["type"] == "request"],
                         ["Projects/Game", "Projects/Game"])
        self.assertNotIn(self.directory.as_posix(), (root / transcript.TRANSCRIPT_RELATIVE_PATH).read_text("utf-8"))
        # A project outside the repository is recorded as given.
        other = EditorConnection(None, supervised=False, client=self.connect(editor), repository_root=repository)
        self.assertIn("result", other.call("project.open", {"path": str(outside)}))
        recorded = self.read_lines(outside / transcript.TRANSCRIPT_RELATIVE_PATH)
        self.assertEqual(recorded[0]["params"]["path"], str(outside))

    def test_a_relative_project_resolves_against_the_repository_root(self) -> None:
        repository = self.make_repository()
        root = repository / "Tests" / "Projects" / "Game"
        root.mkdir(parents=True)
        (root / "Game.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
        editor = self.start_server({"session.info": lambda params: {"pid": os.getpid()}})
        launcher = Launcher(repository, self.directory / "UserData")
        editor.write_session_file(launcher.sessions_directory(), project_path=(root / "Game.eproj").as_posix())
        elsewhere = self.directory / "Elsewhere"
        elsewhere.mkdir()
        self.enter_directory(elsewhere)
        self.assertEqual(launcher.resolve_project(Path("Tests/Projects/Game")), root.resolve())
        connection = launcher.launch(LaunchRequest(project=Path("Tests/Projects/Game")))
        self.assertEqual(connection.project_root, root)
        connection.disconnect()
        bridge = server.Bridge(repository, [], launcher)
        for name, arguments in (("editor_launch", {"project": "Tests/Projects/Game"}),
                                ("editor_attach", {"project": "Tests/Projects/Game/Game.eproj"})):
            with self.subTest(tool=name):
                structured, text, is_error = bridge.call_tool(name, arguments)
                self.assertFalse(is_error, text)
                self.assertEqual(Path(structured["project"]), root)
                bridge.call_tool("editor_shutdown", {})
        self.assertFalse((elsewhere / "Tests").exists())


class ResultReadTests(BridgeTestCase):
    """Friction S1-01: result_read lets an agent with only the MCP tools read what the editor offloaded (or a test
    report), and nothing outside the offload folders."""

    def make_bridge(self, root: Path | None) -> server.Bridge:
        bridge = server.Bridge(REPOSITORY_ROOT, [], Launcher(REPOSITORY_ROOT, self.directory / "UserData"))
        if root is not None:
            bridge.connection = EditorConnection(root, supervised=False)
        return bridge

    def test_result_read_pages_an_offloaded_result_and_reads_a_pointer(self) -> None:
        root = self.make_project()
        out = root / server.OFFLOAD_DIRECTORY
        out.mkdir(parents=True)
        document = {"passed": False, "failedCases": ["Breakout/launch"],
                    "cases": [{"suite": "Breakout", "case": f"case {index}", "status": "passed",
                               "message": "é" * 40} for index in range(300)]}
        offloaded = out / "4242-1791244800-00000002.json"
        offloaded.write_bytes(json.dumps(document, ensure_ascii=False, indent=1).encode("utf-8"))
        bridge = self.make_bridge(root)

        text = offloaded.read_bytes().decode("utf-8")
        joined, offset, pages = "", 0, 0
        while True:
            structured, summary, is_error = bridge.call_tool("result_read", {"path": str(offloaded), "offset": offset,
                                                                             "length": 4096})
            self.assertFalse(is_error, summary)
            self.assertEqual(structured["offset"], offset)
            self.assertLessEqual(len(structured["content"].encode("utf-8")), 4096)
            self.assertIn(structured["content"], summary)
            joined += structured["content"]
            pages += 1
            offset = structured["nextOffset"]
            if offset == 0 or pages > 100:
                break
        self.assertEqual(joined, text)
        self.assertGreater(pages, 1)
        self.assertEqual(structured["totalBytes"], len(text.encode("utf-8")))

        structured, _, is_error = bridge.call_tool("result_read", {"path": offloaded.name, "pointer": "/cases/7/case"})
        self.assertFalse(is_error)
        self.assertEqual(structured["content"], "case 7")
        structured, _, is_error = bridge.call_tool("result_read", {"path": "Library/Automation/Out/" + offloaded.name,
                                                                   "pointer": "/failedCases"})
        self.assertFalse(is_error)
        self.assertEqual(json.loads(structured["content"]), ["Breakout/launch"])
        for pointer, error in (("/cases/300", "NotFound"), ("/nothing", "NotFound"), ("cases", "InvalidParams")):
            with self.subTest(pointer=pointer):
                structured, _, is_error = bridge.call_tool("result_read", {"path": str(offloaded), "pointer": pointer})
                self.assertTrue(is_error)
                self.assertEqual(structured["error"], error)

    def test_result_read_refuses_files_outside_the_offload_folders(self) -> None:
        root = self.make_project()
        out = root / server.OFFLOAD_DIRECTORY
        out.mkdir(parents=True)
        (out / "1-2-00000001.png").write_bytes(b"\x89PNG\r\n\x1a\n\xff\xfe")
        bridge = self.make_bridge(root)
        # A bare file name is looked for in the offload folders only once the location is checked before the file
        # system (the milestone review's R33): the project's Game.eproj is never touched, so it is NotFound there, and
        # never read.
        for path, error in ((str(root / "Game.eproj"), "PermissionDenied"),
                            ("Library/Automation/Out/../../../Game.eproj", "PermissionDenied"),
                            ("Game.eproj", "NotFound"),
                            (str(REPOSITORY_ROOT / "AGENTS.md"), "PermissionDenied"),
                            ("Library/Automation/Out/missing.json", "NotFound"),
                            ("1-2-00000001.png", "NotText")):
            with self.subTest(path=path):
                structured, text, is_error = bridge.call_tool("result_read", {"path": path})
                self.assertTrue(is_error, text)
                self.assertEqual(structured["error"], error)
        for arguments in ({}, {"path": "x.json", "offset": -1}, {"path": "x.json", "length": 0},
                          {"path": "x.json", "length": server.RESULT_PAGE_BYTES + 1}):
            with self.subTest(arguments=arguments):
                structured, _, is_error = bridge.call_tool("result_read", arguments)
                self.assertTrue(is_error)
                self.assertEqual(structured["error"], "InvalidParams")

    def test_result_read_touches_nothing_outside_the_offload_folders(self) -> None:
        # The milestone review's R33: result_read stat'ed any absolute path before its confinement check, so a UNC path
        # made Windows open an SMB connection (sending the user's credentials) and NotFound against PermissionDenied
        # told whether a file outside the folders existed. The location is now checked as text first.
        root = self.make_project()
        (root / server.OFFLOAD_DIRECTORY).mkdir(parents=True)
        outside = self.directory / "Secret.json"
        outside.write_text('{"secret": true}', encoding="utf-8")
        unc = "\\\\attacker.invalid\\share\\x.json" if os.name == "nt" else "//attacker.invalid/share/x.json"
        bridge = self.make_bridge(root)
        touched: list[str] = []
        real = {name: getattr(Path, name) for name in ("is_file", "exists", "stat", "resolve")}

        def recording(name: str) -> Any:
            def method(path: Path, *arguments: Any, **options: Any) -> Any:
                touched.append(str(path))
                return real[name](path, *arguments, **options)
            return method

        patches = [mock.patch.object(Path, name, recording(name)) for name in real]
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            for path in (unc, str(outside), str(self.directory / "Missing.json"), "Library/../Game.eproj"):
                with self.subTest(path=path):
                    structured, text, is_error = bridge.call_tool("result_read", {"path": path})
                    self.assertTrue(is_error, text)
                    # The same answer whether the outside file exists or not.
                    self.assertEqual(structured["error"], "PermissionDenied")
                    self.assertNotIn("secret", text)
        for path in touched:
            self.assertNotIn("attacker.invalid", path)
            self.assertNotEqual(Path(path), outside)
            self.assertNotEqual(Path(path), self.directory / "Missing.json")

    def test_result_read_refuses_a_link_inside_an_offload_folder_that_leads_outside(self) -> None:
        # The milestone review's R35: Out/evil.json leading to a file outside the folders is refused once resolved
        # (Path.resolve, never a lexical normalization). The link is a real symbolic link where this process may create
        # one; otherwise (Windows without the privilege; a junction links directories only) Path.resolve answers for it
        # as a link would, so a check that stopped resolving reads the stand-in file and fails here.
        root = self.make_project()
        out = root / server.OFFLOAD_DIRECTORY
        out.mkdir(parents=True)
        outside = self.directory / "Secret.json"
        outside.write_text('{"secret": true}', encoding="utf-8")
        link = out / "evil.json"
        try:
            os.symlink(outside, link)
            stand_in = contextlib.nullcontext()
        except OSError:
            link.write_text('{"stand-in": true}', encoding="utf-8")
            real_resolve = Path.resolve

            def resolve(path: Path, strict: bool = False) -> Path:
                return outside if os.path.normcase(str(path)) == os.path.normcase(str(link)) \
                    else real_resolve(path, strict)

            stand_in = mock.patch.object(Path, "resolve", resolve)
        bridge = self.make_bridge(root)
        with stand_in:
            for path in (str(link), "evil.json", "Library/Automation/Out/evil.json"):
                with self.subTest(path=path):
                    structured, text, is_error = bridge.call_tool("result_read", {"path": path})
                    self.assertTrue(is_error, text)
                    self.assertEqual(structured["error"], "PermissionDenied")
                    self.assertNotIn("secret", text)
                    self.assertNotIn(outside.as_posix(), text)

    def test_result_read_reads_the_user_data_offload_folder_without_a_project(self) -> None:
        bridge = self.make_bridge(None)
        out = bridge.launcher.sessions_directory().parent / "Out"
        out.mkdir(parents=True)
        (out / "7-8-00000001.json").write_text('{"methods": []}', encoding="utf-8")
        structured, text, is_error = bridge.call_tool("result_read", {"path": "7-8-00000001.json"})
        self.assertFalse(is_error, text)
        self.assertEqual(structured["content"], '{"methods": []}')
        self.assertEqual(structured["nextOffset"], 0)

    def test_pages_end_at_line_breaks_and_never_split_a_character(self) -> None:
        text = "".join(f"{line:02} ééé\n" for line in range(10))
        self.assertEqual(server.page_text(text, 0, 25), (text[:14], 0, 20, 100))
        self.assertEqual(server.page_text(text, 3, 4), ("éé", 3, 7, 100))
        self.assertEqual(server.page_text(text, 4, 2)[1], 5)
        self.assertEqual(server.page_text(text, 90, 100), (text[-7:], 90, 0, 100))
        self.assertEqual(server.resolve_json_pointer({"a/b": {"~": [1, 2]}}, "/a~1b/~0/1"), 2)


class EngineMethodsTests(BridgeTestCase):
    def test_engine_methods_reads_an_offloaded_catalogue(self) -> None:
        # The unfiltered rpc.discover is over the offload threshold, so the editor answers with the file it wrote
        # (ADR 0008 decision 31); a filtered one fits inline.
        methods = [{"name": "entity.create", "description": "Creates an entity.", "mutates": True, "params": {}},
                   {"name": "scene.tree", "description": "The hierarchy.", "mutates": False, "params": {}}]
        offloaded = self.directory / "1-2-00000001.json"
        offloaded.write_text(json.dumps({"methods": methods}), encoding="utf-8")

        def discover(params: dict[str, Any]) -> dict[str, Any]:
            if "domain" in params:
                return {"methods": [method for method in methods if method["name"].startswith(params["domain"] + ".")]}
            return {"path": offloaded.as_posix(), "truncated": True,
                    "summary": {"methods": {"type": "array", "count": 2}}}

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


class EngineCallTests(BridgeTestCase):
    def test_engine_call_waits_for_the_called_methods_own_timeout(self) -> None:
        # Docs/Decisions/0026-m16-contract.md decision 13: engine_call bounds a call by the method's own timeoutSeconds
        # as the editor declares it (rpc.discover {domain}, asked once per domain and never transcribed), then the
        # catalogue's, then 60 s, so a long project.upgrade or test.run through engine_call is not cut off at 60 s.
        root = self.make_project()
        discovered: list[dict[str, Any]] = []

        def discover(params: dict[str, Any]) -> dict[str, Any]:
            discovered.append(params)
            if params.get("domain") == "project":
                return {"methods": [{"name": "project.upgrade", "timeoutSeconds": 600},
                                    {"name": "project.info", "timeoutSeconds": 60}]}
            return {"methods": []}

        editor = self.start_server({"rpc.discover": discover,
                                    "project.upgrade": lambda params: {"changedFiles": [], "unchangedCount": 3},
                                    "scene.tree": lambda params: {"text": "Main"},
                                    "debug.echo": lambda params: {}})
        catalog = [server.CatalogTool(name="scene_tree", method="scene.tree", description="The hierarchy.",
                                      input_schema={"type": "object"}, mutates=False, supports_dry_run=False,
                                      timeout_seconds=90)]
        bridge = server.Bridge(REPOSITORY_ROOT, catalog, Launcher(REPOSITORY_ROOT, self.directory / "UserData"))
        connection = EditorConnection(root, supervised=False, client=self.connect(editor))
        bridge.connection = connection
        assert connection.client is not None
        sent: list[tuple[str, float]] = []
        original = connection.client.request

        def request(method: str, params: dict[str, Any] | None = None, transcript_line: int | None = None,
                    timeout: float = engine_client.DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
            sent.append((method, timeout))
            return original(method, params, transcript_line, timeout)

        with mock.patch.object(connection.client, "request", side_effect=request):
            for method in ("project.upgrade", "project.upgrade", "scene.tree", "debug.echo"):
                structured, text, is_error = bridge.call_tool("engine_call", {"method": method, "params": {}})
                self.assertFalse(is_error, text)
        calls = [(method, timeout) for method, timeout in sent if method != "rpc.discover"]
        self.assertEqual(calls, [("project.upgrade", 600.0), ("project.upgrade", 600.0), ("scene.tree", 90.0),
                                 ("debug.echo", float(server.DEFAULT_TIMEOUT_SECONDS))])
        self.assertEqual(discovered, [{"domain": "project"}, {"domain": "scene"}, {"domain": "debug"}])
        # Only the agent's calls are in the transcript, each request with its response.
        lines = self.read_lines(root / "Automation" / "BuildLog.jsonl")
        self.assertEqual([line["method"] for line in lines if line["type"] == "request"],
                         ["project.upgrade", "project.upgrade", "scene.tree", "debug.echo"])
        self.assertEqual(len(lines), 8)


class ArgumentCoercionTests(BridgeTestCase):
    """Docs/Decisions/0026-m16-contract.md, friction S3-02: a client whose tool list predates the current catalogue
    sends an argument its cached schema lacks as a string, which the editor refused ("expected boolean, got string
    /toc"). The bridge converts a string to the type the current catalogue declares when it is exactly that type's JSON
    literal, and forwards everything else unchanged for the editor to judge."""

    SCHEMA = {"type": "object", "additionalProperties": False, "properties": {
        "topic": {"type": "string"}, "toc": {"type": "boolean"}, "offset": {"type": "integer", "minimum": 0},
        "scale": {"type": "number"}, "where": {"type": "object"}, "select": {"type": "array"},
        "either": {"type": ["integer", "null"]}, "free": {"description": "any JSON value"},
        "target": {"enum": ["Edit", "Play"]}}}

    def make_bridge(self, editor: fake_editor.FakeEditor, root: Path) -> server.Bridge:
        catalog = [server.CatalogTool(name="docs_get", method="docs.get", description="Docs.", input_schema=self.SCHEMA,
                                      mutates=False, supports_dry_run=False, timeout_seconds=60)]
        bridge = server.Bridge(REPOSITORY_ROOT, catalog, Launcher(REPOSITORY_ROOT, self.directory / "UserData"))
        bridge.connection = EditorConnection(root, supervised=False, client=self.connect(editor))
        return bridge

    def test_a_typed_tool_call_whose_stringified_arguments_the_catalogue_types_reaches_the_editor_with_json_types(
            self) -> None:
        root = self.make_project()
        editor = self.start_server({"docs.get": lambda params: {"text": "ok"}})
        bridge = self.make_bridge(editor, root)
        structured, text, is_error = bridge.call_tool("docs_get", {
            "topic": "123", "toc": "true", "offset": "-0", "scale": "2.5e-1", "where": '{"name": "Ball"}',
            "select": '["Transform", 1]', "either": "null"})
        self.assertFalse(is_error, text)
        params = {key: value for key, value in editor.requests[0].params.items() if key != "_meta"}
        self.assertEqual(params, {"topic": "123", "toc": True, "offset": 0, "scale": 0.25, "where": {"name": "Ball"},
                                  "select": ["Transform", 1], "either": None})
        # The transcript records what the editor received.
        request = self.read_lines(root / transcript.TRANSCRIPT_RELATIVE_PATH)[0]
        self.assertEqual(request["params"], params)
        # An integer literal for a number stays an integer; "false" is a boolean too.
        bridge.call_tool("docs_get", {"toc": "false", "scale": "3", "either": "7"})
        self.assertEqual({key: value for key, value in editor.requests[1].params.items() if key != "_meta"},
                         {"toc": False, "scale": 3, "either": 7})

        # The committed catalogue's docs_get declares toc a boolean: S3's refused call goes through now.
        docs_get = next(tool for tool in server.load_catalog() if tool.name == "docs_get")
        self.assertEqual(server.coerce_arguments({"topic": "skills/game-building", "toc": "true"},
                                                 docs_get.input_schema),
                         {"topic": "skills/game-building", "toc": True})

    def test_a_string_that_is_not_the_declared_types_literal_is_forwarded_unchanged(self) -> None:
        root = self.make_project()
        editor = self.start_server({"docs.get": lambda params: {"text": "ok"}})
        bridge = self.make_bridge(editor, root)
        # The milestone review's R32: a number literal that overflows a double inside object or array text (it would
        # become Infinity, which is not JSON, in the request frame and the transcript) and an integer literal too long
        # for Python's int() are forwarded unchanged too.
        too_long = "1" * 5000
        cases = [{"toc": "True"}, {"toc": "yes"}, {"toc": " true"}, {"toc": "1"},
                 {"offset": "1.0"}, {"offset": "01"}, {"offset": "1e2"}, {"offset": "+1"}, {"offset": " 1"},
                 {"scale": "NaN"}, {"scale": "Infinity"}, {"scale": "1e400"}, {"scale": ".5"}, {"scale": "0x10"},
                 {"where": "[1]"}, {"where": "{bad}"}, {"where": '{"a": NaN}'}, {"select": "{}"},
                 {"either": "none"}, {"topic": "true"}, {"free": "true"}, {"target": "1"}, {"unknown": "true"},
                 {"where": '{"x": 1e400}'}, {"select": "[-1e999]"}, {"where": '{"x": [1, 2E+309]}'},
                 {"offset": too_long}, {"scale": too_long}, {"either": too_long}, {"select": f"[{too_long}]"}]
        for index, arguments in enumerate(cases):
            with self.subTest(arguments=arguments):
                structured, text, is_error = bridge.call_tool("docs_get", arguments)
                self.assertFalse(is_error, text)
                forwarded = {key: value for key, value in editor.requests[index].params.items() if key != "_meta"}
                self.assertEqual(forwarded, arguments)
        # Every request line the transcript holds is strict JSON (no Infinity).
        for line in (root / transcript.TRANSCRIPT_RELATIVE_PATH).read_text(encoding="utf-8").splitlines():
            json.loads(line, parse_constant=lambda name: self.fail(f"{name} in the transcript: {line[:80]}"))
        # Arguments that already have their JSON types are never touched.
        typed = {"toc": True, "offset": 3, "where": {"name": "x"}, "select": ["a"]}
        self.assertEqual(server.coerce_arguments(typed, self.SCHEMA), typed)
        # A schema without properties converts nothing.
        self.assertEqual(server.coerce_arguments({"toc": "true"}, {"type": "object"}), {"toc": "true"})

    def test_the_conversion_follows_the_catalogue_a_launch_reloaded(self) -> None:
        # "The current catalogue": a parameter the reloaded catalogue declares is converted from then on.
        path = self.directory / "catalog.json"
        entry = {"name": "input_replay", "method": "input.replay", "description": "Replays.", "mutates": False,
                 "supportsDryRun": False, "timeoutSeconds": 600,
                 "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}}
        path.write_text(json.dumps({"Format": "McpCatalog", "Version": 1, "Tools": [entry]}), encoding="utf-8")
        tools, digest = server.read_catalog(path)
        root = self.make_project()
        editor = self.start_server({"input.replay": lambda params: {"passed": True}})
        bridge = server.Bridge(REPOSITORY_ROOT, tools, Launcher(REPOSITORY_ROOT, self.directory / "UserData"), path,
                               digest)
        bridge.connection = EditorConnection(root, supervised=False, client=self.connect(editor))
        bridge.call_tool("input_replay", {"path": "Assets/A.replay", "withoutInput": "true"})
        self.assertEqual(editor.requests[0].params["withoutInput"], "true")
        entry["inputSchema"]["properties"]["withoutInput"] = {"type": "boolean"}
        path.write_text(json.dumps({"Format": "McpCatalog", "Version": 1, "Tools": [entry]}), encoding="utf-8")
        self.assertTrue(bridge.reload_catalog())
        bridge.call_tool("input_replay", {"path": "Assets/A.replay", "withoutInput": "true"})
        self.assertIs(editor.requests[1].params["withoutInput"], True)


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

    def test_editor_attach_transcribes_a_session_info_naming_the_editor(self) -> None:
        # Docs/Decisions/0026-m16-contract.md, friction S2-10: an attach sent no request, so nothing in the transcript
        # showed where an attached session began; and S2-03: nothing reported that the project had no open scene.
        root = self.make_project()
        project = {"open": True, "name": "Game", "projectFile": (root / "Game.eproj").as_posix()}
        info: dict[str, Any] = {"pid": os.getpid(), "renderer": "vulkan", "scene": None, "project": project}
        editor = self.start_server({"session.info": lambda params: dict(info)})
        launcher, sessions = self.make_launcher()
        editor.write_session_file(sessions, project_path=(root / "Game.eproj").as_posix())
        bridge = server.Bridge(REPOSITORY_ROOT, [], launcher)
        structured, text, is_error = bridge.call_tool("editor_attach", {"project": str(root)})
        self.assertFalse(is_error, text)
        path = root / transcript.TRANSCRIPT_RELATIVE_PATH
        lines = self.read_lines(path)
        self.assertEqual([(line["type"], line.get("method")) for line in lines],
                         [("request", "session.info"), ("response", None)])
        for part in (f"pid {os.getpid()}", "project Game", "renderer vulkan"):
            self.assertIn(part, lines[1]["summary"])
        self.assertIsNone(structured["session"]["scene"])
        self.assertIn("no scene is open: scene_open", text)

        # editor_status asks again: the open scene, once there is one.
        info["scene"] = {"path": "Assets/Scenes/Main.scene", "name": "Main", "revision": 3, "dirty": True,
                         "entityCount": 2}
        status, _, _ = bridge.call_tool("editor_status", {})
        self.assertEqual(status["session"]["scene"], info["scene"])
        self.assertFalse(bridge.call_tool("editor_shutdown", {})[2])

        # editor_launch attaching to the editor that serves the project begins the same way; the new connection's
        # request ids start again, so only the line tells the two session.info requests apart.
        structured, text, is_error = bridge.call_tool("editor_launch", {"project": str(root)})
        self.assertFalse(is_error, text)
        self.assertIn("scene Assets/Scenes/Main.scene (unsaved changes)", text)
        self.assertEqual(structured["session"]["scene"]["path"], "Assets/Scenes/Main.scene")
        lines = self.read_lines(path)
        requests = [line for line in lines if line["type"] == "request"]
        self.assertEqual([line["method"] for line in requests], ["session.info"] * 3)
        self.assertEqual(requests[2]["id"], requests[0]["id"])
        bridge.call_tool("editor_shutdown", {})

        # An editor that refuses session.info is still attached; the response line records why.
        refusing = self.start_server({})
        other = self.make_project("Other")
        other_launcher = Launcher(REPOSITORY_ROOT, self.directory / "OtherUserData")
        refusing.write_session_file(other_launcher.sessions_directory(),
                                    project_path=(other / "Other.eproj").as_posix())
        connection = other_launcher.attach(project=other)
        self.assertTrue(connection.is_alive())
        self.assertIsNone(connection.session_info)
        refused = self.read_lines(other / transcript.TRANSCRIPT_RELATIVE_PATH)
        self.assertEqual((refused[0]["method"], refused[1]["ok"]), ("session.info", False))
        connection.disconnect()

    def test_editor_launch_transcribes_a_session_info_after_opening_the_project_and_reports_the_scene(self) -> None:
        # Docs/Decisions/0026-m16-contract.md, friction S3-06: a launch opened the project and sent no session.info, so
        # editor_launch said nothing about the open scene (none: opening a project opens none), unlike an attach.
        root = self.make_project()
        project = {"open": True, "name": "Game", "projectFile": (root / "Game.eproj").as_posix()}
        info: dict[str, Any] = {"pid": 4242, "renderer": "none", "scene": None, "project": project}

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            return {"project": {"name": "Game", "root": root.as_posix()}, "warnings": [], "recoveryAvailable": False,
                    "recovered": False}

        def create_project(params: dict[str, Any]) -> dict[str, Any]:
            created = Path(params["path"])
            created.mkdir(exist_ok=True)
            (created / f"{params['name']}.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
            return {"project": {"name": params["name"], "root": created.as_posix()}, "createdFiles": []}

        editor = self.start_server({"project.open": open_project, "project.create": create_project,
                                    "session.info": lambda params: dict(info)})
        launcher, _ = self.make_launcher()
        spawned = mock.Mock()

        def connect(session: Any, supervised: bool, editor_process: Any = None, **_: Any) -> EditorConnection:
            return EditorConnection(None, supervised=True, client=self.connect(editor), repository_root=REPOSITORY_ROOT)

        bridge = server.Bridge(REPOSITORY_ROOT, [], launcher)
        new = self.directory / "New"
        with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                mock.patch.object(engine_client, "launch_editor", return_value=spawned), \
                mock.patch.object(launcher, "connect", side_effect=connect):
            for arguments, path in (({"project": str(root)}, root),
                                    ({"project": str(new), "create": True}, new)):
                with self.subTest(arguments=arguments):
                    structured, text, is_error = bridge.call_tool("editor_launch", arguments)
                    self.assertFalse(is_error, text)
                    self.assertIsNone(structured["session"]["scene"])
                    self.assertEqual(structured["session"]["pid"], 4242)
                    self.assertIn("no scene is open: scene_open", text)
                    lines = self.read_lines(path / transcript.TRANSCRIPT_RELATIVE_PATH)
                    opened = "project.create" if arguments.get("create") else "project.open"
                    self.assertEqual([(line["type"], line.get("method")) for line in lines],
                                     [("request", opened), ("response", None), ("request", "session.info"),
                                      ("response", None)])
                    self.assertEqual(lines[3]["requestLine"], 3)
                    self.assertFalse(bridge.call_tool("editor_shutdown", {})[2])
        self.assertEqual([request.method for request in editor.requests],
                         ["project.open", "session.info", "project.create", "session.info"])
        spawned.kill.assert_not_called()

        # A session.info the editor refuses leaves the launch connected (the response line records why); one whose
        # connection breaks ends the editor the launch spawned.
        refusing = self.start_server({"project.open": open_project})
        refused = EditorConnection(None, supervised=True, client=self.connect(refusing),
                                   repository_root=REPOSITORY_ROOT)
        with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                mock.patch.object(engine_client, "launch_editor", return_value=spawned), \
                mock.patch.object(launcher, "connect", return_value=refused):
            connection = launcher.launch(LaunchRequest(project=root))
        self.assertIs(connection, refused)
        self.assertIsNone(connection.session_info)
        self.assertTrue(connection.is_alive())
        connection.disconnect()
        broken = EditorConnection(None, supervised=True, client=self.connect(editor), repository_root=REPOSITORY_ROOT)
        with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                mock.patch.object(engine_client, "launch_editor", return_value=spawned), \
                mock.patch.object(launcher, "connect", return_value=broken), \
                mock.patch.object(Launcher, "read_session_info", side_effect=engine_client.ConnectionClosed("gone")):
            with self.assertRaises(engine_client.ConnectionClosed):
                launcher.launch(LaunchRequest(project=root))
        spawned.kill.assert_called_once()
        self.assertIsNone(broken.client)

    def test_a_launch_whose_project_call_raises_kills_the_editor_it_spawned(self) -> None:
        # The milestone review's R30: only a raising session.info ended the spawned editor; a project.open or
        # project.create that raised instead of answering left it running, unsupervised and holding the project lock.
        root = self.make_project()
        launcher, _ = self.make_launcher()

        def close(params: dict[str, Any]) -> dict[str, Any]:
            raise fake_editor.CloseConnection()

        cases = [("project.open closes the connection", False, None, engine_client.ConnectionClosed),
                 ("project.open times out", False, TimeoutError("project.open did not answer within 60 s"),
                  TimeoutError),
                 ("project.create breaks the protocol", True, engine_client.ProtocolError("a frame that is not JSON"),
                  engine_client.ProtocolError)]
        for name, create, raised, expected in cases:
            with self.subTest(case=name):
                spawned = mock.Mock()
                editor = self.start_server({"project.open": close, "project.create": close})
                connection = EditorConnection(None, supervised=True, client=self.connect(editor),
                                              repository_root=REPOSITORY_ROOT)
                method = "create_project" if create else "open_project"
                patched = (mock.patch.object(Launcher, method, side_effect=raised) if raised is not None
                           else contextlib.nullcontext())
                with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                        mock.patch.object(engine_client, "launch_editor", return_value=spawned), \
                        mock.patch.object(launcher, "connect", return_value=connection), patched:
                    with self.assertRaises(expected):
                        launcher.launch(LaunchRequest(project=self.directory / "New" if create else root,
                                                      create=create))
                spawned.kill.assert_called_once()
                self.assertIsNone(connection.client)

    def test_local_tools_read_booleans_strictly(self) -> None:
        # The milestone review's R34: the bridge's own tools applied Python truthiness, so the string "false" (a
        # client with a stale schema stringifies arguments, F30) meant true: editor_shutdown {force: "false"} forced.
        root = self.make_project()
        launcher, _ = self.make_launcher()
        bridge = server.Bridge(REPOSITORY_ROOT, [], launcher)
        requests: list[LaunchRequest] = []

        def launch(request: LaunchRequest) -> EditorConnection:
            requests.append(request)
            return EditorConnection(root, supervised=False)

        with mock.patch.object(launcher, "launch", side_effect=launch):
            structured, text, is_error = bridge.call_tool("editor_launch", {
                "project": str(root), "create": "false", "recover": "false", "headless": "true"})
            self.assertFalse(is_error, text)
            self.assertEqual((requests[-1].create, requests[-1].recover, requests[-1].headless), (False, False, True))
            bridge.connection = None
            for arguments in ({"create": "False"}, {"recover": 1}, {"headless": "yes"}):
                with self.subTest(arguments=arguments):
                    structured, text, is_error = bridge.call_tool("editor_launch", {"project": str(root), **arguments})
                    self.assertTrue(is_error)
                    self.assertEqual(structured["error"], "InvalidParams")
                    self.assertIn(next(iter(arguments)), structured["message"])
        self.assertEqual(len(requests), 1)
        connection = mock.Mock()
        connection.shutdown.return_value = {"disconnected": True, "shutDown": True}
        bridge.connection = connection
        structured, text, is_error = bridge.call_tool("editor_shutdown", {"save": "false", "force": "false"})
        self.assertFalse(is_error, text)
        connection.shutdown.assert_called_once_with(save=False, force=False)
        bridge.connection = connection
        structured, text, is_error = bridge.call_tool("editor_shutdown", {"force": "maybe"})
        self.assertTrue(is_error)
        self.assertEqual(structured["error"], "InvalidParams")
        connection.shutdown.assert_called_once()

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

    def test_recover_never_attaches_to_an_editor_serving_the_project(self) -> None:
        root = self.make_project()
        editor = self.start_server({"session.info": lambda params: {"pid": os.getpid()}})
        launcher, sessions = self.make_launcher()
        editor.write_session_file(sessions, project_path=(root / "Game.eproj").as_posix())
        with self.assertRaises(RuntimeError) as raised:
            launcher.launch(LaunchRequest(project=root, recover=True))
        self.assertIn("Recover prompt", str(raised.exception))
        self.assertEqual(editor.requests, [])

    def test_open_project_reports_recovery_and_a_refused_recovery_shuts_the_editor_down(self) -> None:
        root = self.make_project()
        opened: list[dict[str, Any]] = []

        def open_project(params: dict[str, Any]) -> dict[str, Any]:
            opened.append(params)
            if len(opened) > 1:
                raise fake_editor.FakeError(engine_client.CONFLICT, "Conflict", "the scene changed after its autosave")
            return {"project": {"name": "Game", "root": root.as_posix()}, "warnings": [], "recoveryAvailable": True,
                    "recovered": params["recover"]}

        editor = self.start_server({"project.open": open_project})
        connection = EditorConnection(None, supervised=False, client=self.connect(editor))
        Launcher.open_project(connection, root / "Game.eproj", True)
        self.assertEqual((opened[0]["path"], opened[0]["recover"]), (str(root / "Game.eproj"), True))
        self.assertEqual(connection.recovery, {"recoveryAvailable": True, "recovered": True})
        self.assertTrue(connection.describe()["recovered"])
        refused = EditorConnection(None, supervised=False, client=self.connect(editor))
        with self.assertRaises(RuntimeError) as raised:
            Launcher.open_project(refused, root / "Game.eproj", True)
        self.assertIn("Conflict", str(raised.exception))
        self.assertIsNone(refused.recovery)
        self.assertIsNone(refused.client)

    def test_editor_launch_passes_recover_and_reports_the_outcome(self) -> None:
        root = self.make_project()
        editor = self.start_server({})
        launcher, _ = self.make_launcher()
        bridge = server.Bridge(REPOSITORY_ROOT, [], launcher)
        requests: list[LaunchRequest] = []
        for recovery, expected in (({"recoveryAvailable": True, "recovered": True}, "recovered its autosave"),
                                   ({"recoveryAvailable": True, "recovered": False}, "recover: true")):
            with self.subTest(recovered=recovery["recovered"]):
                connection = EditorConnection(root, supervised=True, client=self.connect(editor))
                connection.recovery = recovery

                def launch(request: LaunchRequest, launched: EditorConnection = connection) -> EditorConnection:
                    requests.append(request)
                    return launched

                with mock.patch.object(launcher, "launch", side_effect=launch):
                    structured, text, is_error = bridge.call_tool(
                        "editor_launch", {"project": str(root), "recover": recovery["recovered"]})
                self.assertFalse(is_error, text)
                self.assertEqual(requests[-1].recover, recovery["recovered"])
                self.assertEqual(structured["recovered"], recovery["recovered"])
                self.assertTrue(structured["recoveryAvailable"])
                self.assertIn(expected, text)
                bridge.connection = None
                connection.disconnect()

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


class MachineLockTests(BridgeTestCase):
    """editor_launch and the machine-wide locks of Scripts/Lib/locks.py (Docs/Architecture.md §2.3; ADR 0028 decision
    16): a rendering editor holds the GPU lock from its launch until it exits, however it exits; every launch waits for
    a running gate; a launch under a process that holds the GPU lock runs under that hold. The spawned editor is a
    stand-in process, so the lock follows a real process's lifetime; the editor's answers come from the stand-in
    server."""

    def setUp(self) -> None:
        super().setUp()
        self.root = self.make_project()
        project = {"name": "Game", "root": self.root.as_posix()}
        self.editor = self.start_server({
            "project.open": lambda params: {"project": project, "warnings": [], "recoveryAvailable": False,
                                            "recovered": False},
            "session.info": lambda params: {"pid": 4242, "renderer": "vulkan", "scene": None}})
        self.launcher = Launcher(REPOSITORY_ROOT, self.directory / "UserData", lock_wait_seconds=0.0)
        self.spawned: list[engine_client.EditorProcess] = []
        self.real_launch_editor = engine_client.launch_editor

    def spawn_stand_in(self, *_: object, **__: object) -> engine_client.EditorProcess:
        """launch_editor's stand-in: a real process that waits to be killed (it writes no session file)."""
        editor = self.real_launch_editor(fake_editor.PYTHON, ["-c", EDITOR_STAND_IN], self.directory / "UserData",
                                         "App", wait_for_session=False)
        editor.session = mock.Mock()
        self.addCleanup(editor.kill)
        self.spawned.append(editor)
        return editor

    @contextlib.contextmanager
    def stand_ins(self, launch_editor: Callable[..., engine_client.EditorProcess] | None = None) -> Iterator[None]:
        """The launcher's editor binary, spawn and connection replaced: the spawn by `launch_editor` (default:
        spawn_stand_in), the connection by one to the stand-in server."""
        connection = EditorConnection(None, supervised=True, client=self.connect(self.editor),
                                      repository_root=REPOSITORY_ROOT)
        with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                mock.patch.object(engine_client, "launch_editor", side_effect=launch_editor or self.spawn_stand_in), \
                mock.patch.object(self.launcher, "connect", return_value=connection):
            yield

    def launch(self, renderer: str) -> EditorConnection:
        with self.stand_ins():
            return self.launcher.launch(LaunchRequest(project=self.root, renderer=renderer))

    def end_editor(self, editor: engine_client.EditorProcess) -> None:
        """Kills `editor` and waits, bounded, for the launcher's thread that releases its lock on its exit."""
        editor.kill()
        for thread in threading.enumerate():
            if thread.name == f"gpu-lock-{editor.process.pid}":
                thread.join(lock_holder.DEADLINE_SECONDS)
                self.assertFalse(thread.is_alive())

    def gpu_holders(self) -> list[int]:
        return [holder.pid for holder in locks.holders(locks.GPU)]

    def test_a_rendering_editor_holds_the_gpu_lock_until_it_exits(self) -> None:
        connection = self.launch("vulkan")
        self.addCleanup(connection.disconnect)
        holders = locks.holders(locks.GPU)
        self.assertEqual([holder.pid for holder in holders], [os.getpid()])
        self.assertIn("MCP bridge", holders[0].command)
        with lock_holder.LockHolder(locks.GPU) as other:
            self.assertEqual(other.wait_for("unavailable", "ready"), "unavailable gpu 3")
        self.end_editor(self.spawned[0])
        self.assertEqual(self.gpu_holders(), [])
        with lock_holder.LockHolder(locks.GPU) as other:
            other.ready()

    def test_an_editor_without_rendering_takes_no_gpu_lock(self) -> None:
        connection = self.launch("none")
        self.addCleanup(connection.disconnect)
        self.assertIsNone(self.spawned[0].process.poll())
        self.assertEqual(self.gpu_holders(), [])

    def test_a_launch_fails_naming_the_holder_while_a_gpu_run_or_a_gate_runs(self) -> None:
        for lock, renderer in ((locks.GPU, "vulkan"), (locks.GATE, "vulkan"), (locks.GATE, "none")):
            with self.subTest(lock=lock, renderer=renderer), lock_holder.LockHolder(lock) as holder:
                holder.ready()
                with self.assertRaises(locks.LockUnavailable) as raised:
                    self.launch(renderer)
                self.assertEqual(raised.exception.lock, lock)
                self.assertIn(f"pid {holder.pid}", str(raised.exception))
                self.assertEqual(self.spawned, [], "nothing is spawned")
                bridge = server.Bridge(REPOSITORY_ROOT, [], self.launcher)
                with self.stand_ins():
                    structured, text, is_error = bridge.call_tool("editor_launch", {"project": str(self.root),
                                                                                    "renderer": renderer})
                self.assertTrue(is_error)
                self.assertEqual(structured["error"], "LockUnavailable")
                self.assertIn(f"pid {holder.pid}", text)
                self.assertEqual(self.spawned, [])
                # Unlocked by the holder itself: the next subtest takes the same lock at once, and the system may
                # release a killed process's locks only after a delay.
                self.assertEqual(holder.release(), 0)

    def test_a_launch_under_a_process_holding_the_gpu_lock_runs_under_its_hold(self) -> None:
        # The automation suite's Test.py holds the GPU lock while its bridge tests launch rendering editors.
        with lock_holder.LockHolder(locks.GPU) as parent:
            parent.ready()
            record = locks.read_record(locks.lock_paths(locks.GPU)[0], locks.GPU)
            self.assertIsNotNone(record)
            os.environ[locks.HELD_LOCKS_VARIABLE] = f"{locks.GPU}={record.token if record else ''}"
            connection = self.launch("vulkan")
            self.addCleanup(connection.disconnect)
            self.assertEqual(self.gpu_holders(), [parent.pid])
            self.end_editor(self.spawned[0])
            self.assertEqual(self.gpu_holders(), [parent.pid], "the parent's hold outlives the editor")

    def test_a_launch_that_fails_releases_the_gpu_lock(self) -> None:
        def exits_first(*_: object, **__: object) -> engine_client.EditorProcess:
            raise RuntimeError("Editor exited with code 3 before it listened for automation")

        with self.stand_ins(launch_editor=exits_first), self.assertRaises(RuntimeError):
            self.launcher.launch(LaunchRequest(project=self.root, renderer="vulkan"))
        self.assertEqual(self.gpu_holders(), [])

        # A project.open that fails kills the spawned editor, whose exit releases the lock.
        def refuse(params: dict[str, Any]) -> dict[str, Any]:
            raise fake_editor.FakeError(engine_client.CONFLICT, "Locked", "the project is locked")

        self.editor = self.start_server({"project.open": refuse})
        with self.assertRaises(RuntimeError):
            self.launch("vulkan")
        self.end_editor(self.spawned[0])
        self.assertEqual(self.gpu_holders(), [])


def finished_pid() -> int:
    """The pid of a process that has exited."""
    process = subprocess.Popen([str(fake_editor.PYTHON), "-c", "pass"])
    process.wait()
    return process.pid


if __name__ == "__main__":
    unittest.main()
