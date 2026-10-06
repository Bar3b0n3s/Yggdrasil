"""Standard-library client of the editor's automation protocol (Docs/Architecture.md §13.1-§13.4, §13.8).

One module shared by the MCP bridge (Tools/MCP/engine_mcp/connection.py) and the Python automation suite
(Tests/Automation), so both speak the protocol the same way:

  - framing: LSP-style "Content-Length: N\\r\\n\\r\\n<UTF-8 JSON>" (encode_frame, FrameReader);
  - sessions: <UserData>/<AppName>/Automation/Sessions/<pid>.json written by a listening editor (§13.2), read with
    list_sessions and find_session; files of dead processes are ignored;
  - the project lock: <Project>/Library/Editor.lock names the holder's pid (§4.13, read_lock_holder);
  - connections: EngineClient.connect performs session.hello with the session token, then call() sends requests and
    raises EngineError for error responses;
  - launching: find_editor_executable and launch_editor start a headless editor and wait for its session file;
  - method coverage (§15.6 gate 5): every call() is recorded by a CallLog; with the environment variable
    ENGINE_AUTOMATION_COVERAGE=<file> each called method name is appended to that file, one per line, which
    Scripts/Test.py's automation suite compares with the registered methods.

Every function here is a contract stub of the M4 contract task (Roadmap rule 3) until stream D implements it: it raises
NotImplementedError with a message starting "contract stub", which Scripts/Lint.py rejects outside contract mode.
"""

from __future__ import annotations

import dataclasses
import os
import socket
import subprocess
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = "1.0"
CLIENT_NAME = "engine-client"
CLIENT_VERSION = "1"
DEFAULT_TIMEOUT_SECONDS = 60.0
# How long launch_editor waits for a new editor's session file.
LAUNCH_TIMEOUT_SECONDS = 60.0
COVERAGE_ENVIRONMENT_VARIABLE = "ENGINE_AUTOMATION_COVERAGE"
MAX_FRAME_PAYLOAD_BYTES = 64 * 1024 * 1024

# JSON-RPC error codes (§13.3), as in Engine/Source/Engine/Automation/Protocol/JsonRpc.h.
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL = -32000
NOT_FOUND = -32001
INVALID_STATE = -32002
VALIDATION_FAILED = -32003
CONFLICT = -32004
TIMEOUT = -32005
SCRIPT_ERROR = -32006
BUSY = -32007
UNAUTHORIZED = -32008
UNSUPPORTED = -32009
CANCELLED = -32010


class EngineError(Exception):
    """An error response: `code` is the JSON-RPC code, `error_code` the engine ErrorCode name (data.errorCode), `detail`
    the engine message (data.detail) and `data` the whole error.data object (issues, failedOp, currentRevision,
    _meta)."""

    def __init__(self, method: str, code: int, message: str, data: dict[str, Any]) -> None:
        super().__init__(f"{method}: {message} ({code})")
        self.method = method
        self.code = code
        self.message = message
        self.data = data
        self.error_code = str(data.get("errorCode", ""))
        self.detail = str(data.get("detail", message))

    @property
    def issues(self) -> list[dict[str, Any]]:
        return list(self.data.get("issues", []))


class ConnectionClosed(Exception):
    """The editor closed the connection (or never accepted it)."""


class ProtocolError(Exception):
    """The editor sent bytes that are not a well-formed frame or JSON-RPC message."""


@dataclasses.dataclass(frozen=True)
class SessionInfo:
    """One session file (§13.2), as written by a listening editor."""

    pid: int
    port: int
    token: str
    protocol_version: str
    engine_version: str
    project_path: str  # absolute .eproj path with '/' separators; empty in the launcher state
    headless: bool
    started_at: str
    path: Path  # the session file itself


def encode_frame(payload: bytes) -> bytes:
    """b"Content-Length: <len>\\r\\n\\r\\n" + payload."""
    raise NotImplementedError("contract stub: engine_client.encode_frame (M4 stream D)")


class FrameReader:
    """Incremental decoder of one connection's byte stream (the client side of FrameDecoder): feed() received bytes,
    next_payload() returns each complete payload or None when more bytes are needed; malformed headers or a payload
    over MAX_FRAME_PAYLOAD_BYTES raise ProtocolError."""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, data: bytes) -> None:
        raise NotImplementedError("contract stub: FrameReader.feed (M4 stream D)")

    def next_payload(self) -> bytes | None:
        raise NotImplementedError("contract stub: FrameReader.next_payload (M4 stream D)")


class CallLog:
    """The methods a client called (§15.6 gate 5). With `path` (default: $ENGINE_AUTOMATION_COVERAGE) each recorded
    method is also appended to that file, so the counts of every test process add up."""

    def __init__(self, path: Path | None = None) -> None:
        self.path = path
        self.methods: list[str] = []

    @classmethod
    def from_environment(cls) -> CallLog:
        """A log writing to $ENGINE_AUTOMATION_COVERAGE when it is set."""
        raise NotImplementedError("contract stub: CallLog.from_environment (M4 stream D)")

    def record(self, method: str) -> None:
        raise NotImplementedError("contract stub: CallLog.record (M4 stream D)")


def read_workspace_name(repository_root: Path) -> str:
    """The workspace name premake5.lua defines (`WorkspaceName = "<name>"`), which the Editor and Tests use as their
    application name (ENGINE_PRODUCT_NAME) and so as the user-data folder name."""
    raise NotImplementedError("contract stub: engine_client.read_workspace_name (M4 stream D)")


def default_user_data_root() -> Path:
    """The OS user-data root (Engine/Source/Engine/Platform/Paths.h): %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or
    ~/.local/share on Linux, ~/Library/Application Support on macOS."""
    raise NotImplementedError("contract stub: engine_client.default_user_data_root (M4 stream D)")


def sessions_directory(app_name: str, user_data_root: Path | None = None) -> Path:
    """<user_data_root or default_user_data_root()>/<app_name>/Automation/Sessions."""
    raise NotImplementedError("contract stub: engine_client.sessions_directory (M4 stream D)")


def is_process_alive(pid: int) -> bool:
    """Whether a process with `pid` runs (Windows: OpenProcess and GetExitCodeProcess through ctypes, never os.kill,
    which terminates the process there; POSIX: os.kill(pid, 0))."""
    raise NotImplementedError("contract stub: engine_client.is_process_alive (M4 stream D)")


def list_sessions(directory: Path) -> list[SessionInfo]:
    """The session files of live editors in `directory`, sorted by pid; unreadable files and files of dead processes are
    skipped (§13.2 "stale files (dead pid) are ignored")."""
    raise NotImplementedError("contract stub: engine_client.list_sessions (M4 stream D)")


def find_session(directory: Path, project_file: Path) -> SessionInfo | None:
    """The live session whose projectPath names `project_file` (compared as resolved paths, case-insensitively on
    Windows and macOS), or None."""
    raise NotImplementedError("contract stub: engine_client.find_session (M4 stream D)")


def read_lock_holder(project_root: Path) -> int | None:
    """The pid in <project_root>/Library/Editor.lock when another process holds the lock (§4.13, §13.8
    EditorAlreadyOpen); None when the file is missing or no process holds the lock (a crashed editor's stale pid
    text)."""
    raise NotImplementedError("contract stub: engine_client.read_lock_holder (M4 stream D)")


def wait_for_lock_holder(project_root: Path, process: subprocess.Popen[bytes] | None = None,
                         timeout: float = LAUNCH_TIMEOUT_SECONDS) -> int:
    """Polls read_lock_holder (with short sleeps against a time.monotonic() deadline) until a process holds the
    project's lock and returns its pid. Raises TimeoutError after `timeout` seconds, and RuntimeError (with the exit
    code and the tail of its standard error) when `process`, the editor expected to take the lock, exits first."""
    raise NotImplementedError("contract stub: engine_client.wait_for_lock_holder (M4 stream D)")


class EngineClient:
    """One authenticated connection to an editor. Not thread-safe: one request at a time."""

    def __init__(self, connection: socket.socket, call_log: CallLog | None = None) -> None:
        self.connection = connection
        self.call_log = call_log
        self.reader = FrameReader()
        self.next_id = 1
        self.hello: dict[str, Any] = {}

    @classmethod
    def connect(cls, port: int, token: str, client_name: str = CLIENT_NAME, client_version: str = CLIENT_VERSION,
                timeout: float = DEFAULT_TIMEOUT_SECONDS, call_log: CallLog | None = None) -> EngineClient:
        """Connects to 127.0.0.1:`port` and performs session.hello; the hello result is kept in `hello`. Raises
        ConnectionClosed, EngineError (a bad token is UNAUTHORIZED) or TimeoutError."""
        raise NotImplementedError("contract stub: EngineClient.connect (M4 stream D)")

    @classmethod
    def connect_session(cls, session: SessionInfo, client_name: str = CLIENT_NAME,
                        call_log: CallLog | None = None) -> EngineClient:
        """connect() with the session file's port and token."""
        raise NotImplementedError("contract stub: EngineClient.connect_session (M4 stream D)")

    def request(self, method: str, params: dict[str, Any] | None = None, transcript_line: int | None = None,
                timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """Sends one request (params._meta.transcriptLine when given) and returns the whole response message. Records
        the method in the call log. Raises ConnectionClosed, ProtocolError or TimeoutError."""
        raise NotImplementedError("contract stub: EngineClient.request (M4 stream D)")

    def call(self, method: str, params: dict[str, Any] | None = None, transcript_line: int | None = None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """request(), returning the result object (with its "_meta") or raising EngineError for an error response."""
        raise NotImplementedError("contract stub: EngineClient.call (M4 stream D)")

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        """Sends a notification (no id, no response)."""
        raise NotImplementedError("contract stub: EngineClient.notify (M4 stream D)")

    def send_raw(self, data: bytes) -> None:
        """Sends bytes as they are (security tests: HTTP probes, oversized frames, bad tokens)."""
        raise NotImplementedError("contract stub: EngineClient.send_raw (M4 stream D)")

    def receive_message(self, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """The next message from the editor. Raises ConnectionClosed when the editor closes the connection, in an
        orderly way (end of stream) or by a reset (ConnectionResetError and ConnectionAbortedError are mapped to it),
        TimeoutError when nothing arrives within `timeout` (the connection is still open), and ProtocolError for
        malformed frames."""
        raise NotImplementedError("contract stub: EngineClient.receive_message (M4 stream D)")

    def close(self) -> None:
        """Closes the connection (the editor then cancels this client's pending operations, §13.2)."""
        raise NotImplementedError("contract stub: EngineClient.close (M4 stream D)")

    def __enter__(self) -> EngineClient:
        return self

    def __exit__(self, *exception: object) -> None:
        self.close()


def connect_raw(port: int, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> socket.socket:
    """A TCP connection to 127.0.0.1:`port` without any handshake (security tests)."""
    raise NotImplementedError("contract stub: engine_client.connect_raw (M4 stream D)")


def find_editor_executable(repository_root: Path, configurations: tuple[str, ...] = ("Release", "Debug")) -> Path:
    """bin/<Config>-<system>-<arch>/Editor/Editor(.exe) of the first configuration that is built (§13.8: the bridge
    prefers Release). Raises FileNotFoundError naming `python Scripts/Build.py --config Release --project Editor`."""
    raise NotImplementedError("contract stub: engine_client.find_editor_executable (M4 stream D)")


@dataclasses.dataclass
class EditorProcess:
    """An editor started by launch_editor."""

    process: subprocess.Popen[bytes]
    session: SessionInfo | None
    user_data_dir: Path

    def connect(self, client_name: str = CLIENT_NAME, call_log: CallLog | None = None) -> EngineClient:
        """EngineClient.connect_session on this editor's session."""
        raise NotImplementedError("contract stub: EditorProcess.connect (M4 stream D)")

    def wait(self, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> int:
        """Waits for the process to exit and returns its exit code."""
        raise NotImplementedError("contract stub: EditorProcess.wait (M4 stream D)")

    def kill(self) -> None:
        """Kills the process (crash tests) and waits for it."""
        raise NotImplementedError("contract stub: EditorProcess.kill (M4 stream D)")


def launch_editor(executable: Path, arguments: list[str], user_data_dir: Path, app_name: str,
                  wait_for_session: bool = True, environment: dict[str, str] | None = None) -> EditorProcess:
    """Starts `executable` with `arguments` plus --user-data-dir=<user_data_dir> (argument list, never a shell) and,
    with `wait_for_session`, waits up to LAUNCH_TIMEOUT_SECONDS for its session file in
    sessions_directory(app_name, user_data_dir). An editor that exits first is reported with its exit code and the tail
    of its standard error in a RuntimeError."""
    raise NotImplementedError("contract stub: engine_client.launch_editor (M4 stream D)")


def _environment_coverage_path() -> Path | None:
    value = os.environ.get(COVERAGE_ENVIRONMENT_VARIABLE, "")
    return Path(value) if value else None
