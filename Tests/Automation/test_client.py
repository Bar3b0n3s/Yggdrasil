"""The shared protocol client itself (Tools/Automation/engine_client.py; Docs/Architecture.md §13.2, §13.8, §4.13):
framing, the handshake and calls against a stand-in server, session files, the project lock and launching. These tests
need no editor build; their calls never count towards method coverage (no CallLog writes to the coverage file)."""

from __future__ import annotations

import contextlib
import inspect
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from unittest import mock

import fake_editor
from harness import REPOSITORY_ROOT, engine_client


class FramingTests(unittest.TestCase):
    def test_frames_decode_in_any_split(self) -> None:
        payloads = [b'{"jsonrpc":"2.0","id":1,"result":{}}', "{\"text\": \"éè\"}".encode("utf-8"), b"{}"]
        stream = b"".join(engine_client.encode_frame(payload) for payload in payloads)
        self.assertTrue(stream.startswith(b"Content-Length: 36\r\n\r\n" + payloads[0]))
        for chunk_size in (1, 3, len(stream)):
            reader = engine_client.FrameReader()
            decoded: list[bytes] = []
            for start in range(0, len(stream), chunk_size):
                reader.feed(stream[start:start + chunk_size])
                while (payload := reader.next_payload()) is not None:
                    decoded.append(payload)
            self.assertEqual(decoded, payloads, chunk_size)
            self.assertEqual(reader.buffer, bytearray())

    def test_content_type_is_accepted_after_content_length(self) -> None:
        reader = engine_client.FrameReader()
        reader.feed(b"content-length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}")
        self.assertEqual(reader.next_payload(), b"{}")

    def test_malformed_headers_raise_protocol_error(self) -> None:
        cases = {
            "http": b"HTTP/1.1 200 OK\r\n\r\n",
            "no length": b"Content-Type: text/plain\r\n\r\n",
            "repeated": b"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
            "oversized": f"Content-Length: {engine_client.MAX_FRAME_PAYLOAD_BYTES + 1}\r\n\r\n".encode("ascii"),
            "long header": b"X" * (engine_client.MAX_FRAME_HEADER_BYTES + 1),
        }
        for name, data in cases.items():
            with self.subTest(case=name):
                reader = engine_client.FrameReader()
                reader.feed(data)
                with self.assertRaises(engine_client.ProtocolError):
                    reader.next_payload()


# A request's bound in the progress tests, in the fake clock's seconds; each report arrives most of a bound after the
# last one (PROGRESS_GAP), so four of them outlast the bound twice over. The socket waits what is left of it in
# real seconds, so it is generous: only a failure waits that long.
PROGRESS_BOUND = 1000.0
PROGRESS_GAP = 0.75 * PROGRESS_BOUND
# Bounds the stand-in editor's wait for the client to give up (only a failure lasts this long).
GIVE_UP_WAIT_SECONDS = 60.0


class ConnectionTests(unittest.TestCase):
    def start_server(self, handlers: dict[str, fake_editor.Handler]) -> fake_editor.FakeEditor:
        server = fake_editor.FakeEditor(handlers)
        server.__enter__()
        self.addCleanup(server.stop)
        return server

    def test_connect_says_hello_and_calls(self) -> None:
        server = self.start_server({"scene.tree": lambda params: {"text": "Main.scene", "echo": params}})
        call_log = engine_client.CallLog()
        with engine_client.EngineClient.connect(server.port, server.token, client_name="tests",
                                                call_log=call_log) as client:
            self.assertEqual(client.hello["protocolVersion"], engine_client.PROTOCOL_VERSION)
            result = client.call("scene.tree", {"depth": 2}, transcript_line=12)
            self.assertEqual(result["text"], "Main.scene")
            self.assertEqual(result["echo"], {"depth": 2, "_meta": {"transcriptLine": 12}})
            self.assertIn("_meta", result)
            response = client.request("scene.tree")
            self.assertEqual(response["id"], 3)
        self.assertEqual(call_log.methods, ["session.hello", "scene.tree", "scene.tree"])
        self.assertEqual([request.client for request in server.requests], ["tests", "tests"])

    def test_error_responses_raise_engine_error(self) -> None:
        def conflict(params: dict[str, Any]) -> dict[str, Any]:
            raise fake_editor.FakeError(engine_client.CONFLICT, "Conflict", "the scene changed",
                                        {"currentRevision": 7, "issues": [{"pointer": "/ifRevision", "message": "x"}]})

        server = self.start_server({"entity.create": conflict})
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            with self.assertRaises(engine_client.EngineError) as raised:
                client.call("entity.create", {"name": "A"})
            error = raised.exception
            self.assertEqual((error.code, error.error_code, error.detail), (engine_client.CONFLICT, "Conflict",
                                                                            "the scene changed"))
            self.assertEqual(error.data["currentRevision"], 7)
            self.assertEqual(error.issues[0]["pointer"], "/ifRevision")
            with self.assertRaises(engine_client.EngineError) as unknown:
                client.call("no.such")
            self.assertEqual(unknown.exception.code, engine_client.METHOD_NOT_FOUND)

    def test_bad_token_is_unauthorized(self) -> None:
        server = self.start_server({})
        with self.assertRaises(engine_client.EngineError) as raised:
            engine_client.EngineClient.connect(server.port, "f" * 64)
        self.assertEqual(raised.exception.code, engine_client.UNAUTHORIZED)

    def test_closed_connection_and_timeouts(self) -> None:
        release = threading.Event()

        def slow(params: dict[str, Any]) -> dict[str, Any]:
            release.wait(10.0)
            return {}

        def close(params: dict[str, Any]) -> dict[str, Any]:
            raise fake_editor.CloseConnection()

        server = self.start_server({"debug.stall": slow, "session.shutdown": close})
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            with self.assertRaises(TimeoutError):
                client.call("debug.stall", timeout=0.2)
            release.set()
            # The late answer to the timed-out request is skipped by id.
            server.handlers["session.info"] = lambda params: {"pid": 1}
            self.assertEqual(client.call("session.info")["pid"], 1)
            with self.assertRaises(engine_client.ConnectionClosed):
                client.call("session.shutdown")
        with self.assertRaises(engine_client.ConnectionClosed):
            client.call("session.info")

    def test_notifications_before_the_response_are_skipped(self) -> None:
        server = self.start_server({"play.step": lambda params: {"tick": 600}})
        server.notifications.append({"jsonrpc": "2.0", "method": "$/progress", "params": {"done": 300}})
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            self.assertEqual(client.request("play.step")["result"]["tick"], 600)
            client.notify("play.step", {"ticks": 1})
            client.send_raw(engine_client.encode_frame(b'{"jsonrpc":"2.0","id":"named","method":"play.step"}'))
            self.assertEqual(client.receive_message(timeout=10.0)["method"], "$/progress")
            self.assertEqual(client.receive_message(timeout=10.0)["id"], "named")
        self.assertEqual([request.request_id for request in server.requests], [2, None, "named"])

    def test_a_request_waits_as_long_as_its_progress_advances(self) -> None:
        # Skerries' brief S20: a recording that outlasted test.run's 900 s ended with Timeout although the editor went
        # on and finished. Each $/progress whose work advanced moves the deadline, so the call outlives its bound.
        server = self.start_server({})
        consumed = threading.Semaphore(0)

        def run(params: dict[str, Any]) -> dict[str, Any]:
            for work in range(1, 5):
                server.send_progress(work, f"recording Record.Hops, run 1 (recorded): tick {work * 60}",
                                     stage="record")
                # The next report, and the answer, go only once the client has read this one, so none waits unread in
                # its buffer, where a deadline that never moved would not be checked against it (bounded: a client that
                # gave up never reads it, and only that failure waits this long).
                if not consumed.acquire(timeout=GIVE_UP_WAIT_SECONDS):
                    raise fake_editor.CloseConnection()
            return {"passed": True}

        server.handlers["test.run"] = run
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            clock = fake_editor.FakeClock()
            client.clock = clock
            started = clock.now
            reports: list[dict[str, Any]] = []

            def progress(report: dict[str, Any]) -> None:
                reports.append(report)
                clock.advance(PROGRESS_GAP)
                consumed.release()

            result = client.call("test.run", {}, timeout=PROGRESS_BOUND, progress=progress)
            self.assertTrue(result["passed"])
            self.assertGreater(clock.now - started, 2 * PROGRESS_BOUND, "the call outlived its bound")
            # The callback heard every report of the request, as the editor sent it.
            self.assertEqual([report["work"] for report in reports], [1, 2, 3, 4])
            self.assertEqual(reports[-1]["message"], "recording Record.Hops, run 1 (recorded): tick 240")
            self.assertEqual({report["id"] for report in reports}, {server.requests[-1].request_id})

    def test_a_request_whose_progress_stops_advancing_times_out(self) -> None:
        # A run that still reports but no longer advances (the same work) is cut off once its bound passes without an
        # advance, naming its last report.
        server = self.start_server({})
        gave_up = threading.Event()

        def run(params: dict[str, Any]) -> dict[str, Any]:
            server.send_progress(5, "suite Board: tick 300")
            for _ in range(3):
                server.send_progress(5, "suite Board: tick 300")
            # Answers only once the client gave up (bounded), so nothing but the bound can end the call.
            gave_up.wait(GIVE_UP_WAIT_SECONDS)
            return {"passed": True}

        server.handlers["test.run"] = run
        server.handlers["session.info"] = lambda params: {"pid": 1}
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            clock = fake_editor.FakeClock()
            client.clock = clock
            heard: list[int] = []

            def progress(report: dict[str, Any]) -> None:
                heard.append(report["work"])
                clock.advance(PROGRESS_GAP)

            with self.assertRaises(TimeoutError) as raised:
                client.call("test.run", {}, timeout=PROGRESS_BOUND, progress=progress)
            gave_up.set()
            self.assertIn("test.run: its work has not advanced for 1000.0 s", str(raised.exception))
            self.assertIn("suite Board: tick 300", str(raised.exception))
            self.assertEqual(heard[:2], [5, 5])
            # The connection still serves: the late reports and answer of the timed-out request are skipped by id.
            client.clock = time.monotonic
            self.assertEqual(client.call("session.info", timeout=30.0)["pid"], 1)

    def test_progress_of_another_request_never_moves_the_deadline(self) -> None:
        # Reports of an earlier request that timed out on this side (still running in the editor) are not this
        # request's.
        server = self.start_server({})
        gave_up = threading.Event()

        def run(params: dict[str, Any]) -> dict[str, Any]:
            for work in range(1, 4):
                server.send_progress(work, "replay Assets/Tests/Replays/Old.replay: tick 1 of 600", id=-1)
            gave_up.wait(GIVE_UP_WAIT_SECONDS)
            return {"passed": True}

        server.handlers["input.replay"] = run
        with engine_client.EngineClient.connect(server.port, server.token) as client:
            clock = fake_editor.FakeClock()
            client.clock = clock
            original = engine_client.EngineClient.receive_message
            heard: list[dict[str, Any]] = []

            def receive_and_advance(this: engine_client.EngineClient, timeout: float = 60.0) -> dict[str, Any]:
                message = original(this, timeout)
                if message.get("method") == engine_client.PROGRESS_METHOD:
                    clock.advance(PROGRESS_GAP)
                return message

            with mock.patch.object(engine_client.EngineClient, "receive_message", receive_and_advance), \
                    self.assertRaises(TimeoutError) as raised:
                client.call("input.replay", {"path": "Assets/Tests/Replays/New.replay"}, timeout=PROGRESS_BOUND,
                            progress=heard.append)
            gave_up.set()
            self.assertIn("input.replay: no response from the editor within 1000.0 s", str(raised.exception))
            self.assertEqual(heard, [])

    def test_unreachable_port_is_connection_closed(self) -> None:
        server = self.start_server({})
        port = server.port
        server.stop()
        with self.assertRaises(engine_client.ConnectionClosed):
            engine_client.connect_raw(port, timeout=5.0)

    def test_call_log_appends_to_the_coverage_file(self) -> None:
        directory = make_directory(self)
        log = engine_client.CallLog(directory / "Coverage" / "Called.txt")
        log.record("scene.tree")
        log.record("entity.create")
        self.assertEqual((directory / "Coverage" / "Called.txt").read_text(encoding="utf-8"),
                         "scene.tree\nentity.create\n")
        with mock.patch.dict(os.environ, {engine_client.COVERAGE_ENVIRONMENT_VARIABLE: str(directory / "Env.txt")}):
            self.assertEqual(engine_client.CallLog.from_environment().path, directory / "Env.txt")


class SessionAndLockTests(unittest.TestCase):
    def test_sessions_of_live_editors_are_listed(self) -> None:
        directory = make_directory(self)
        with fake_editor.FakeEditor() as server:
            project = directory / "Game" / "Game.eproj"
            live = server.write_session_file(directory, project_path=project.as_posix())
            dead_pid = finished_process_pid()
            server.write_session_file(directory, pid=dead_pid)
            (directory / "123.json").write_text("{not json", encoding="utf-8")
            (directory / "456.json").write_text(json.dumps({"pid": 456}), encoding="utf-8")
            sessions = engine_client.list_sessions(directory)
            self.assertEqual([session.path for session in sessions], [live])
            self.assertEqual(sessions[0].port, server.port)
            self.assertEqual(sessions[0].project_path, project.as_posix())
            self.assertEqual(engine_client.find_session(directory, project), sessions[0])
            if sys.platform in ("win32", "darwin"):
                self.assertEqual(engine_client.find_session(directory, Path(str(project).upper())), sessions[0])
            self.assertIsNone(engine_client.find_session(directory, directory / "Other" / "Other.eproj"))
            with engine_client.EngineClient.connect_session(sessions[0]) as client:
                self.assertEqual(client.hello["engineVersion"], "0.1.0")
        self.assertEqual(engine_client.list_sessions(directory / "Missing"), [])

    def test_a_session_file_is_listed_while_the_rename_that_publishes_it_is_in_flight(self) -> None:
        # Docs/Decisions/0040-session-file-sharing.md: on Windows the editor's rename keeps a handle with DELETE access
        # open on the file under its new name until the rename returns. A client polling for the file can read it in
        # that moment; Python's open(), which does not share deletion, failed there with PermissionError, so the live
        # editor was not listed (and the MCP bridge's editor_launch, finding its project lock held, refused with
        # EditorAlreadyOpen instead of attaching).
        directory = make_directory(self)
        with fake_editor.FakeEditor() as server:
            staged = server.write_session_file(directory / "Staging")
            published = directory / staged.name
            with rename_keeping_handle_open(staged, published):
                sessions = engine_client.list_sessions(directory)
                self.assertEqual([session.path for session in sessions], [published])
                self.assertEqual(sessions[0].port, server.port)
                self.assertEqual(json.loads(engine_client.read_shared_bytes(published))["pid"], os.getpid())
        with self.assertRaises(FileNotFoundError):
            engine_client.read_shared_bytes(directory / "Missing.json")

    def test_process_liveness(self) -> None:
        self.assertTrue(engine_client.is_process_alive(os.getpid()))
        self.assertFalse(engine_client.is_process_alive(finished_process_pid()))
        self.assertFalse(engine_client.is_process_alive(0))

    def test_lock_holder_is_reported_while_the_lock_is_held(self) -> None:
        project = make_directory(self) / "Game"
        self.assertIsNone(engine_client.read_lock_holder(project))
        holder = fake_editor.hold_project_lock(project)
        try:
            self.assertEqual(engine_client.read_lock_holder(project), holder.pid)
            self.assertEqual(engine_client.wait_for_lock_holder(project, timeout=10.0), holder.pid)
        finally:
            fake_editor.release_project_lock(holder)
        # The pid text stays behind, but nobody holds the lock (a crashed editor's stale file).
        self.assertEqual((project / engine_client.LOCK_FILE_RELATIVE_PATH).read_text(encoding="ascii").strip(),
                         str(holder.pid))
        self.assertIsNone(engine_client.read_lock_holder(project))

    def test_waiting_for_the_lock_fails_when_the_editor_exits_or_time_runs_out(self) -> None:
        project = make_directory(self) / "Game"
        script = "import sys; sys.stderr.write('no project\\n'); sys.exit(3)"
        exiting = subprocess.Popen([str(fake_editor.PYTHON), "-c", script], stderr=subprocess.PIPE)
        exiting.wait()
        with self.assertRaises(RuntimeError) as raised:
            engine_client.wait_for_lock_holder(project, exiting, timeout=10.0)
        self.assertIn("code 3", str(raised.exception))
        self.assertIn("no project", str(raised.exception))
        if exiting.stderr is not None:
            exiting.stderr.close()
        with self.assertRaises(TimeoutError):
            engine_client.wait_for_lock_holder(project, timeout=0.2)


class LaunchTests(unittest.TestCase):
    # The slowest start of an editor measured to its session file: a --strict Debug editor opening FeatureTest while
    # sibling worktrees loaded the machine (32 s unloaded; Docs/Decisions/0040-session-file-sharing.md).
    LOADED_EDITOR_START_SECONDS = 134.0

    # A stand-in "editor": a Python process that writes a lot of output (the pipes must be drained), then its session
    # file into the --user-data-dir it was given, then waits to be killed.
    EDITOR_SCRIPT = (
        "import json, os, sys, time\n"
        "root = sys.argv[-1].split('=', 1)[1]\n"
        "sys.stderr.write('log line\\n' * 20000)\n"
        "sys.stderr.flush()\n"
        "directory = os.path.join(root, sys.argv[1], 'Automation', 'Sessions')\n"
        "os.makedirs(directory, exist_ok=True)\n"
        "session = {'pid': os.getpid(), 'port': 5, 'token': 't', 'protocolVersion': '1.0', 'engineVersion': '0.1.0',\n"
        "           'projectPath': '', 'headless': True, 'startedAt': 'now'}\n"
        "path = os.path.join(directory, f'{os.getpid()}.json')\n"
        "with open(path + '.tmp', 'w') as file:\n"
        "    file.write(json.dumps(session))\n"
        "os.replace(path + '.tmp', path)\n"
        "time.sleep(120)\n"
    )

    def test_launch_waits_for_the_session_file(self) -> None:
        user_data = make_directory(self) / "UserData"
        editor = engine_client.launch_editor(fake_editor.PYTHON, ["-c", self.EDITOR_SCRIPT, "App"], user_data, "App")
        try:
            assert editor.session is not None
            self.assertEqual(editor.session.pid, editor.process.pid)
            self.assertEqual(editor.user_data_dir, user_data.resolve())
        finally:
            editor.kill()
        self.assertIsNotNone(editor.process.returncode)
        self.assertEqual(editor.output_tail[-1], "log line")

    def test_the_launch_wait_is_a_hang_bound_with_headroom_over_a_loaded_start(self) -> None:
        self.assertGreaterEqual(engine_client.LAUNCH_TIMEOUT_SECONDS, 2 * self.LOADED_EDITOR_START_SECONDS)
        self.assertEqual(inspect.signature(engine_client.wait_for_lock_holder).parameters["timeout"].default,
                         engine_client.LAUNCH_TIMEOUT_SECONDS)
        # An editor that never writes its session file fails the launch once the bound has passed, naming it.
        user_data = make_directory(self) / "UserData"
        with mock.patch.object(engine_client, "LAUNCH_TIMEOUT_SECONDS", 1.0):
            with self.assertRaises(TimeoutError) as raised:
                engine_client.launch_editor(fake_editor.PYTHON, ["-c", "import time; time.sleep(120)"], user_data,
                                            "App")
        self.assertIn("wrote no session file within 1 s", str(raised.exception))

    def test_launch_reports_an_editor_that_exits_first(self) -> None:
        user_data = make_directory(self) / "UserData"
        script = "import sys; print('starting'); sys.stderr.write('is locked by process 42\\n'); sys.exit(3)"
        with self.assertRaises(RuntimeError) as raised:
            engine_client.launch_editor(fake_editor.PYTHON, ["-c", script], user_data, "App")
        self.assertIn("code 3", str(raised.exception))
        self.assertIn("is locked by process 42", str(raised.exception))

    def test_missing_editor_names_the_build_command(self) -> None:
        with self.assertRaises(FileNotFoundError) as raised:
            engine_client.find_editor_executable(make_directory(self))
        self.assertIn("python Scripts/Build.py --config Release --project Editor", str(raised.exception))

    def test_workspace_name_and_user_data_root(self) -> None:
        directory = make_directory(self)
        (directory / "premake5.lua").write_text('-- the workspace\nWorkspaceName = "Sample"\n', encoding="utf-8")
        self.assertEqual(engine_client.read_workspace_name(directory), "Sample")
        self.assertTrue(engine_client.read_workspace_name(REPOSITORY_ROOT))
        root = engine_client.default_user_data_root()
        self.assertTrue(root.is_absolute())
        self.assertEqual(engine_client.sessions_directory("App", directory),
                         directory / "App" / "Automation" / "Sessions")


def make_directory(test: unittest.TestCase) -> Path:
    """A temporary directory removed after `test`."""
    directory = Path(tempfile.mkdtemp(prefix="EngineClient-")).resolve()
    test.addCleanup(shutil.rmtree, directory, ignore_errors=True)
    return directory


@contextlib.contextmanager
def rename_keeping_handle_open(source: Path, target: Path) -> Iterator[None]:
    """Renames `source` to `target` the way every rename runs on Windows (MoveFileExW's included): through a handle with
    DELETE access, sharing everything, whose rename (SetFileInformationByHandle, FileRenameInfo) takes effect before the
    handle closes. The handle stays open for the with-block: the moment between a rename taking effect and its return,
    which a loaded machine stretches, held as long as the test needs. Elsewhere a descriptor of `source` stays open
    across a plain rename instead."""
    if sys.platform != "win32":
        with source.open("rb"):
            os.replace(source, target)
            yield
        return
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                     wintypes.DWORD, wintypes.HANDLE)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.SetFileInformationByHandle.argtypes = (wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD)
    kernel32.SetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    name = str(target)

    class RenameInformation(ctypes.Structure):  # FILE_RENAME_INFO with room for the whole name
        _fields_ = [("ReplaceIfExists", wintypes.BOOLEAN), ("RootDirectory", wintypes.HANDLE),
                    ("FileNameLength", wintypes.DWORD), ("FileName", wintypes.WCHAR * (len(name) + 1))]

    delete, synchronize, share_all, open_existing, normal, file_rename_info = 0x10000, 0x100000, 0x7, 3, 0x80, 3
    handle = kernel32.CreateFileW(str(source), delete | synchronize, share_all, None, open_existing, normal, None)
    if handle is None or handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        information = RenameInformation(True, None, len(name) * ctypes.sizeof(wintypes.WCHAR), name)
        if not kernel32.SetFileInformationByHandle(handle, file_rename_info, ctypes.byref(information),
                                                   ctypes.sizeof(information)):
            raise ctypes.WinError(ctypes.get_last_error())
        yield
    finally:
        kernel32.CloseHandle(handle)


def finished_process_pid() -> int:
    """The pid of a process that has exited (and been reaped)."""
    process = subprocess.Popen([str(fake_editor.PYTHON), "-c", "pass"])
    process.wait()
    deadline = time.monotonic() + 10.0
    while engine_client.is_process_alive(process.pid) and time.monotonic() < deadline:
        time.sleep(engine_client.POLL_INTERVAL_SECONDS)
    return process.pid


if __name__ == "__main__":
    unittest.main()
