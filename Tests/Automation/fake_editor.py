"""A stand-in for the editor's automation server, for the tests of the clients themselves (Tools/Automation/
engine_client.py and the MCP bridge): it speaks the wire protocol of Docs/Architecture.md §13.2 (framing, the
session.hello handshake with its token check and the close after three failures, JSON-RPC responses with "_meta") and
answers each method with a handler the test registers. It never replaces the real editor in the scenario tests: those
start Editor --headless through harness.py.

Helpers for the session files and the project lock the clients read are here too, so the client tests need no editor.
"""

from __future__ import annotations

import dataclasses
import json
import os
import socket
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any, Callable

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))

import engine_client  # noqa: E402  (the path above makes the shared client importable)

TOKEN = "0123456789abcdef" * 4
# The interpreter that runs stand-in processes. In a Windows virtual environment sys.executable is a launcher that runs
# the base interpreter as a child process, so a process started through it has another pid than the one that writes
# session files and takes locks, and killing it leaves that child running; the base interpreter has neither problem.
PYTHON = Path(getattr(sys, "_base_executable", sys.executable))
MAX_AUTH_FAILURES = 3


class FakeError(Exception):
    """Raised by a handler: answered as a JSON-RPC error with data {errorCode, detail, issues, **extra, _meta}."""

    def __init__(self, code: int, error_code: str, detail: str, extra: dict[str, Any] | None = None) -> None:
        super().__init__(detail)
        self.code = code
        self.error_code = error_code
        self.detail = detail
        self.extra = extra or {}


class CloseConnection(Exception):
    """Raised by a handler: the server closes the connection without answering."""


Handler = Callable[[dict[str, Any]], dict[str, Any]]


@dataclasses.dataclass
class ReceivedRequest:
    """One request the fake server read, after the handshake."""

    client: str
    method: str
    params: dict[str, Any]
    request_id: Any


class FakeEditor:
    """A TCP server on 127.0.0.1 that answers like an editor's automation server. Thread-safe for the tests' needs:
    handlers run on one thread per connection."""

    def __init__(self, handlers: dict[str, Handler] | None = None, token: str = TOKEN) -> None:
        self.token = token
        self.handlers: dict[str, Handler] = dict(handlers or {})
        self.requests: list[ReceivedRequest] = []
        # Sent before every response, as a pending operation's $/progress notifications are.
        self.notifications: list[dict[str, Any]] = []
        self.revision = 0
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.connections: list[socket.socket] = []
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.accepter = threading.Thread(target=self.accept_loop, daemon=True, name="fake-editor-accept")

    def __enter__(self) -> FakeEditor:
        self.accepter.start()
        return self

    def __exit__(self, *exception: object) -> None:
        self.stop()

    def stop(self) -> None:
        """Stops listening and closes every connection."""
        self.stopping.set()
        self.listener.close()
        with self.lock:
            connections = list(self.connections)
        for connection in connections:
            _close(connection)
        if self.accepter.is_alive():
            self.accepter.join(timeout=10.0)

    def close_connections(self) -> None:
        """Closes every connection, as an editor that dies does, and keeps listening."""
        with self.lock:
            connections, self.connections = list(self.connections), []
        for connection in connections:
            _close(connection)

    def accept_loop(self) -> None:
        while not self.stopping.is_set():
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with self.lock:
                self.connections.append(connection)
            threading.Thread(target=self.serve, args=(connection,), daemon=True, name="fake-editor-client").start()

    def serve(self, connection: socket.socket) -> None:
        reader = engine_client.FrameReader()
        client_name = ""
        failures = 0
        try:
            while True:
                payload = reader.next_payload()
                if payload is None:
                    data = connection.recv(65536)
                    if not data:
                        return
                    reader.feed(data)
                    continue
                message = json.loads(payload.decode("utf-8"))
                method = message.get("method", "")
                params = message.get("params", {})
                request_id = message.get("id")
                if not client_name:
                    error = self.check_hello(method, params)
                    if error is None:
                        client_name = params["client"]["name"]
                        self.send(connection, {"jsonrpc": "2.0", "id": request_id, "result": self.hello_result()})
                        continue
                    failures += 1
                    self.send(connection, self.error_response(request_id, error))
                    if failures >= MAX_AUTH_FAILURES:
                        return
                    continue
                with self.lock:
                    self.requests.append(ReceivedRequest(client_name, method, params, request_id))
                response = self.dispatch(method, params, request_id)
                if response is not None and request_id is not None:
                    for notification in self.notifications:
                        self.send(connection, notification)
                    self.send(connection, response)
        except CloseConnection:
            return
        except (OSError, ValueError, engine_client.ProtocolError):
            return
        finally:
            _close(connection)

    def check_hello(self, method: str, params: dict[str, Any]) -> FakeError | None:
        if method != "session.hello":
            return FakeError(engine_client.UNAUTHORIZED, "PermissionDenied", "session.hello must be the first request")
        if params.get("token") != self.token:
            return FakeError(engine_client.UNAUTHORIZED, "PermissionDenied", "invalid token")
        client = params.get("client")
        if not isinstance(client, dict) or not client.get("name"):
            return FakeError(engine_client.INVALID_PARAMS, "InvalidArgument", "client.name is missing")
        return None

    def hello_result(self) -> dict[str, Any]:
        return {"protocolVersion": engine_client.PROTOCOL_VERSION, "engineVersion": "0.1.0", "clientId": 1,
                "capabilities": ["dryRun", "ifRevision", "batch"], "project": {"open": False, "name": "",
                                                                              "projectFile": "", "readOnly": False},
                "_meta": self.meta()}

    def meta(self) -> dict[str, Any]:
        return {"revision": self.revision, "dirty": False, "playState": "Edit",
                "diagnostics": {"newErrors": 0, "newWarnings": 0, "newScriptErrors": 0, "logCursor": "0",
                                "firstNew": []}}

    def dispatch(self, method: str, params: dict[str, Any], request_id: Any) -> dict[str, Any] | None:
        handler = self.handlers.get(method)
        if handler is None:
            return self.error_response(request_id, FakeError(engine_client.METHOD_NOT_FOUND, "NotFound",
                                                             f"no method '{method}'"))
        try:
            result = handler(params)
        except FakeError as error:
            return self.error_response(request_id, error)
        return {"jsonrpc": "2.0", "id": request_id, "result": {**result, "_meta": self.meta()}}

    def error_response(self, request_id: Any, error: FakeError) -> dict[str, Any]:
        data = {"errorCode": error.error_code, "detail": error.detail, "issues": [], **error.extra}
        if error.code != engine_client.UNAUTHORIZED:
            data["_meta"] = self.meta()
        return {"jsonrpc": "2.0", "id": request_id, "error": {"code": error.code, "message": error.detail,
                                                             "data": data}}

    @staticmethod
    def send(connection: socket.socket, message: dict[str, Any]) -> None:
        connection.sendall(engine_client.encode_frame(json.dumps(message).encode("utf-8")))

    def write_session_file(self, directory: Path, project_path: str = "", pid: int | None = None) -> Path:
        """A session file naming this server, as a listening editor writes it (§13.2); the pid defaults to this
        process, which is alive."""
        pid = os.getpid() if pid is None else pid
        directory.mkdir(parents=True, exist_ok=True)
        content = {"pid": pid, "port": self.port, "token": self.token, "protocolVersion": "1.0",
                   "engineVersion": "0.1.0", "projectPath": project_path, "headless": True,
                   "startedAt": "2026-10-06T12:00:00Z"}
        path = directory / f"{pid}.json"
        path.write_text(json.dumps(content, indent="\t") + "\n", encoding="utf-8")
        return path


def _close(connection: socket.socket) -> None:
    try:
        connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    connection.close()


# The lock holder (Engine/Source/Engine/Platform/ProjectLock.h): exclusive LockFileEx on one byte far past the pid text
# on Windows, flock on POSIX; then the pid text; then "locked" on standard output, and the lock is held until standard
# input closes.
_LOCK_HOLDER_SCRIPT = """
import os, sys
sys.path.insert(0, sys.argv[2])
import engine_client
path = sys.argv[1]
os.makedirs(os.path.dirname(path), exist_ok=True)
if sys.platform == "win32":
    import msvcrt
    file = open(path, "a+b")
    engine_client.windows_lock_byte(msvcrt.get_osfhandle(file.fileno()), exclusive=True, wait=False)
else:
    import fcntl
    file = open(path, "a+b")
    fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
with open(path, "r+b") as text:
    text.truncate(0)
    text.write(f"{os.getpid()}\\n".encode("ascii"))
print("locked", flush=True)
sys.stdin.read()
"""


def hold_project_lock(project_root: Path) -> subprocess.Popen[str]:
    """A child process that holds <project_root>/Library/Editor.lock the way an editor does, until its standard input is
    closed (release_project_lock)."""
    lock_file = project_root / engine_client.LOCK_FILE_RELATIVE_PATH
    process = subprocess.Popen([str(PYTHON), "-c", _LOCK_HOLDER_SCRIPT, str(lock_file),
                                str(REPOSITORY_ROOT / "Tools" / "Automation")],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert process.stdout is not None
    line = process.stdout.readline()
    if line.strip() != "locked":
        process.kill()
        process.wait()
        raise RuntimeError(f"the lock holder failed (exit code {process.poll()}): {line!r}")
    return process


def release_project_lock(holder: subprocess.Popen[str]) -> None:
    """Ends a hold_project_lock child, which releases the lock and leaves its pid text behind (a stale lock file)."""
    if holder.stdin is not None:
        holder.stdin.close()
    holder.wait(timeout=30.0)
    if holder.stdout is not None:
        holder.stdout.close()
