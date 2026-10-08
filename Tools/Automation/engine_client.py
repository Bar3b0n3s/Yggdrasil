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
"""

from __future__ import annotations

import collections
import dataclasses
import functools
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import IO, Any

PROTOCOL_VERSION = "1.0"
CLIENT_NAME = "engine-client"
CLIENT_VERSION = "1"
DEFAULT_TIMEOUT_SECONDS = 60.0
# How long launch_editor waits for a new editor's session file.
LAUNCH_TIMEOUT_SECONDS = 60.0
COVERAGE_ENVIRONMENT_VARIABLE = "ENGINE_AUTOMATION_COVERAGE"
# The one build configuration whose editor the tests and the MCP bridge start (Release, then Debug, when unset).
CONFIG_ENVIRONMENT_VARIABLE = "ENGINE_AUTOMATION_CONFIG"
DEFAULT_CONFIGURATIONS = ("Release", "Debug")
MAX_FRAME_PAYLOAD_BYTES = 64 * 1024 * 1024
# The longest header section accepted from the editor, the blank line included (FrameDecoder's MaxFrameHeaderBytes).
MAX_FRAME_HEADER_BYTES = 1024
# Lines of an editor's console output kept by launch_editor (EditorProcess.output_tail).
OUTPUT_TAIL_LINES = 200
# How often the polling helpers look again.
POLL_INTERVAL_SECONDS = 0.05

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

# The project lock (Engine/Source/Engine/Platform/ProjectLock.h): Windows locks one byte at this offset, far past the
# pid text, so readers are never refused; POSIX uses flock on the whole file.
LOCK_FILE_RELATIVE_PATH = Path("Library") / "Editor.lock"
WINDOWS_LOCK_OFFSET = 0x7FFF_FFFF_FFFF_FFFE
_WORKSPACE_NAME_PATTERN = re.compile(r'^WorkspaceName\s*=\s*"([^"\n]+)"', re.MULTILINE)
_CONTENT_LENGTH_PATTERN = re.compile(rb"^content-length:[ \t]*([0-9]{1,20})[ \t]*$", re.IGNORECASE)
_CONTENT_TYPE_PATTERN = re.compile(rb"^content-type:.*$", re.IGNORECASE)


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


# --------------------------------------------------------------------------------------------------------------------
# Framing
# --------------------------------------------------------------------------------------------------------------------


def encode_frame(payload: bytes) -> bytes:
    """b"Content-Length: <len>\\r\\n\\r\\n" + payload."""
    return b"Content-Length: " + str(len(payload)).encode("ascii") + b"\r\n\r\n" + payload


class FrameReader:
    """Incremental decoder of one connection's byte stream (the client side of FrameDecoder): feed() received bytes,
    next_payload() returns each complete payload or None when more bytes are needed; malformed headers or a payload
    over MAX_FRAME_PAYLOAD_BYTES raise ProtocolError."""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, data: bytes) -> None:
        self.buffer += data

    def next_payload(self) -> bytes | None:
        end = self.buffer.find(b"\r\n\r\n")
        if end < 0:
            if len(self.buffer) > MAX_FRAME_HEADER_BYTES:
                raise ProtocolError(f"frame header longer than {MAX_FRAME_HEADER_BYTES} bytes")
            return None
        if end + 4 > MAX_FRAME_HEADER_BYTES:
            raise ProtocolError(f"frame header longer than {MAX_FRAME_HEADER_BYTES} bytes")
        length: int | None = None
        for line in bytes(self.buffer[:end]).split(b"\r\n"):
            match = _CONTENT_LENGTH_PATTERN.match(line)
            if match is not None and length is None:
                length = int(match.group(1))
            elif match is not None:
                raise ProtocolError("frame header repeats Content-Length")
            elif length is None or not _CONTENT_TYPE_PATTERN.match(line):
                raise ProtocolError(f"malformed frame header line {line[:80]!r}")
        if length is None:
            raise ProtocolError("frame header without Content-Length")
        if length > MAX_FRAME_PAYLOAD_BYTES:
            raise ProtocolError(f"frame payload of {length} bytes exceeds {MAX_FRAME_PAYLOAD_BYTES}")
        start = end + 4
        if len(self.buffer) < start + length:
            return None
        payload = bytes(self.buffer[start:start + length])
        del self.buffer[:start + length]
        return payload


# --------------------------------------------------------------------------------------------------------------------
# Method coverage
# --------------------------------------------------------------------------------------------------------------------


class CallLog:
    """The methods a client called (§15.6 gate 5). With `path` (default: $ENGINE_AUTOMATION_COVERAGE) each recorded
    method is also appended to that file, so the counts of every test process add up."""

    def __init__(self, path: Path | None = None) -> None:
        self.path = path
        self.methods: list[str] = []
        # Clients on several threads may share one log (the watchdog test); their lines must not interleave.
        self.lock = threading.Lock()

    @classmethod
    def from_environment(cls) -> CallLog:
        """A log writing to $ENGINE_AUTOMATION_COVERAGE when it is set."""
        return cls(_environment_coverage_path())

    def record(self, method: str) -> None:
        with self.lock:
            self.methods.append(method)
            if self.path is not None:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                with self.path.open("a", encoding="utf-8", newline="\n") as file:
                    file.write(method + "\n")


# --------------------------------------------------------------------------------------------------------------------
# Session files, processes and the project lock
# --------------------------------------------------------------------------------------------------------------------


def read_workspace_name(repository_root: Path) -> str:
    """The workspace name premake5.lua defines (`WorkspaceName = "<name>"`), which the Editor and Tests use as their
    application name (ENGINE_PRODUCT_NAME) and so as the user-data folder name."""
    script = repository_root / "premake5.lua"
    match = _WORKSPACE_NAME_PATTERN.search(script.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"{script} does not define the workspace name (WorkspaceName = \"...\")")
    return match.group(1)


def default_user_data_root() -> Path:
    """The OS user-data root (Engine/Source/Engine/Platform/Paths.h): %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or
    ~/.local/share on Linux, ~/Library/Application Support on macOS."""
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA", "")
        return Path(local) if local and Path(local).is_absolute() else Path.home() / "AppData" / "Local"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support"
    data_home = os.environ.get("XDG_DATA_HOME", "")
    # The XDG Base Directory specification ignores a relative value.
    return Path(data_home) if data_home and Path(data_home).is_absolute() else Path.home() / ".local" / "share"


def sessions_directory(app_name: str, user_data_root: Path | None = None) -> Path:
    """<user_data_root or default_user_data_root()>/<app_name>/Automation/Sessions."""
    root = user_data_root if user_data_root is not None else default_user_data_root()
    return root / app_name / "Automation" / "Sessions"


def is_process_alive(pid: int) -> bool:
    """Whether a process with `pid` runs (Windows: OpenProcess and WaitForSingleObject through ctypes, never os.kill,
    which terminates the process there; POSIX: os.kill(pid, 0))."""
    if pid <= 0:
        return False
    if sys.platform == "win32":
        return _windows_process_alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # it exists, owned by another user
    return True


def _parse_session(path: Path) -> SessionInfo | None:
    """A session file's content, or None when it is unreadable or malformed."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    types = {"pid": int, "port": int, "token": str, "protocolVersion": str, "engineVersion": str, "projectPath": str,
             "headless": bool, "startedAt": str}
    for key, expected in types.items():
        value = data.get(key)
        # bool is an int in Python; a numeric member must not be a boolean.
        if not isinstance(value, expected) or (expected is int and isinstance(value, bool)):
            return None
    if path.stem != str(data["pid"]) or not 0 < data["port"] < 65536:
        return None
    return SessionInfo(pid=data["pid"], port=data["port"], token=data["token"],
                       protocol_version=data["protocolVersion"], engine_version=data["engineVersion"],
                       project_path=data["projectPath"], headless=data["headless"], started_at=data["startedAt"],
                       path=path)


def list_sessions(directory: Path) -> list[SessionInfo]:
    """The session files of live editors in `directory`, sorted by pid; unreadable files and files of dead processes are
    skipped (§13.2 "stale files (dead pid) are ignored")."""
    if not directory.is_dir():
        return []
    sessions = [session for session in (_parse_session(path) for path in directory.glob("*.json")) if session]
    return sorted((session for session in sessions if is_process_alive(session.pid)), key=lambda item: item.pid)


def _comparable_path(path: Path) -> str:
    """A resolved path in the form two paths naming the same file share: case-folded where the file system usually
    ignores case (Windows, macOS)."""
    text = str(path.resolve())
    if sys.platform == "win32":
        return os.path.normcase(text)
    if sys.platform == "darwin":
        return text.casefold()
    return text


def find_session(directory: Path, project_file: Path) -> SessionInfo | None:
    """The live session whose projectPath names `project_file` (compared as resolved paths, case-insensitively on
    Windows and macOS), or None."""
    wanted = _comparable_path(project_file)
    for session in list_sessions(directory):
        if session.project_path and _comparable_path(Path(session.project_path)) == wanted:
            return session
    return None


def _read_pid_text(lock_file: Path) -> int:
    """The pid the lock file names; 0 while it names none (the holder writes it right after locking)."""
    try:
        text = lock_file.read_text(encoding="ascii", errors="replace").strip()
    except OSError:
        return 0
    return int(text) if text.isdigit() and int(text) > 0 else 0


def read_lock_holder(project_root: Path) -> int | None:
    """The pid in <project_root>/Library/Editor.lock when another process holds the lock (§4.13, §13.8
    EditorAlreadyOpen); None when the file is missing or no process holds the lock (a crashed editor's stale pid
    text). While a holder has locked the file but not yet written its pid, the result is 0."""
    lock_file = project_root / LOCK_FILE_RELATIVE_PATH
    if not lock_file.is_file():
        return None
    held = _windows_lock_held(lock_file) if sys.platform == "win32" else _posix_lock_held(lock_file)
    if held is None or not held:
        return None
    return _read_pid_text(lock_file)


def wait_for_lock_holder(project_root: Path, process: subprocess.Popen[bytes] | None = None,
                         timeout: float = LAUNCH_TIMEOUT_SECONDS) -> int:
    """Polls read_lock_holder (with short sleeps against a time.monotonic() deadline) until a process holds the
    project's lock and returns its pid. Raises TimeoutError after `timeout` seconds, and RuntimeError (with the exit
    code and the tail of its standard error) when `process`, the editor expected to take the lock, exits first."""
    deadline = time.monotonic() + timeout
    while True:
        holder = read_lock_holder(project_root)
        if holder:
            return holder
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"the editor (pid {process.pid}) exited with code {process.returncode} before it took "
                               f"the lock of {project_root}: {_process_output_tail(process)}")
        if time.monotonic() >= deadline:
            raise TimeoutError(f"no process took the lock of {project_root} within {timeout:.0f} s")
        time.sleep(POLL_INTERVAL_SECONDS)


# --------------------------------------------------------------------------------------------------------------------
# Connections
# --------------------------------------------------------------------------------------------------------------------


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
        client = cls(connect_raw(port, timeout), call_log)
        try:
            client.hello = client.call("session.hello", {
                "token": token,
                "protocolVersion": PROTOCOL_VERSION,
                "client": {"name": client_name, "version": client_version},
            }, timeout=timeout)
        except BaseException:
            client.close()
            raise
        return client

    @classmethod
    def connect_session(cls, session: SessionInfo, client_name: str = CLIENT_NAME,
                        call_log: CallLog | None = None) -> EngineClient:
        """connect() with the session file's port and token."""
        return cls.connect(session.port, session.token, client_name=client_name, call_log=call_log)

    def request(self, method: str, params: dict[str, Any] | None = None, transcript_line: int | None = None,
                timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """Sends one request (params._meta.transcriptLine when given) and returns the whole response message. Records
        the method in the call log. Raises ConnectionClosed, ProtocolError or TimeoutError."""
        request_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method,
                   "params": _with_transcript_line(params, transcript_line)}
        if self.call_log is not None:
            self.call_log.record(method)
        self.send_raw(encode_frame(json.dumps(message, ensure_ascii=False).encode("utf-8")))
        deadline = time.monotonic() + timeout
        while True:
            response = self.receive_message(max(0.0, deadline - time.monotonic()))
            # Notifications ($/progress) carry no id; a response with another id answers an earlier request that timed
            # out on this side.
            if response.get("id") == request_id and ("result" in response or "error" in response):
                return response

    def call(self, method: str, params: dict[str, Any] | None = None, transcript_line: int | None = None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """request(), returning the result object (with its "_meta") or raising EngineError for an error response."""
        response = self.request(method, params, transcript_line, timeout)
        if "error" in response:
            raise error_from_response(method, response)
        result = response["result"]
        if not isinstance(result, dict):
            raise ProtocolError(f"{method}: the result is not an object: {result!r}")
        return result

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        """Sends a notification (no id, no response)."""
        if self.call_log is not None:
            self.call_log.record(method)
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        self.send_raw(encode_frame(json.dumps(message, ensure_ascii=False).encode("utf-8")))

    def send_raw(self, data: bytes) -> None:
        """Sends bytes as they are (security tests: HTTP probes, oversized frames, bad tokens)."""
        try:
            self.connection.sendall(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError) as error:
            raise ConnectionClosed(f"the editor closed the connection: {error}") from error
        except OSError as error:
            if self.connection.fileno() < 0:
                raise ConnectionClosed("the connection is closed") from error
            raise

    def receive_message(self, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """The next message from the editor. Raises ConnectionClosed when the editor closes the connection, in an
        orderly way (end of stream) or by a reset (ConnectionResetError and ConnectionAbortedError are mapped to it),
        TimeoutError when nothing arrives within `timeout` (the connection is still open), and ProtocolError for
        malformed frames."""
        deadline = time.monotonic() + timeout
        while True:
            payload = self.reader.next_payload()
            if payload is not None:
                try:
                    message = json.loads(payload.decode("utf-8"))
                except (UnicodeDecodeError, ValueError) as error:
                    raise ProtocolError(f"a frame payload is not UTF-8 JSON: {error}") from error
                if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                    raise ProtocolError(f"not a JSON-RPC 2.0 message: {payload[:200]!r}")
                return message
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no message from the editor within {timeout:.1f} s")
            if self.connection.fileno() < 0:
                raise ConnectionClosed("the connection is closed")
            self.connection.settimeout(remaining)
            try:
                data = self.connection.recv(65536)
            except socket.timeout:
                raise TimeoutError(f"no message from the editor within {timeout:.1f} s") from None
            except (ConnectionResetError, ConnectionAbortedError) as error:
                raise ConnectionClosed(f"the editor reset the connection: {error}") from error
            if not data:
                raise ConnectionClosed("the editor closed the connection")
            self.reader.feed(data)

    def close(self) -> None:
        """Closes the connection (the editor then cancels this client's pending operations, §13.2)."""
        try:
            self.connection.close()
        except OSError:
            pass

    def __enter__(self) -> EngineClient:
        return self

    def __exit__(self, *exception: object) -> None:
        self.close()


def load_offloaded(result: dict[str, Any]) -> dict[str, Any]:
    """The full result of a call: an offloaded result ({"path", "truncated": true, "summary"}, Docs/Architecture.md
    §13.4 bounded output) read from the file it names, any other result unchanged."""
    if result.get("truncated") is not True or not isinstance(result.get("path"), str):
        return result
    full = json.loads(Path(result["path"]).read_text(encoding="utf-8"))
    if not isinstance(full, dict):
        raise ProtocolError(f"the offloaded result {result['path']} is not an object")
    return full


def error_from_response(method: str, response: dict[str, Any]) -> EngineError:
    """The EngineError of an error response."""
    error = response.get("error")
    if not isinstance(error, dict) or not isinstance(error.get("code"), int):
        raise ProtocolError(f"{method}: malformed error response {response!r}")
    data = error.get("data")
    return EngineError(method, error["code"], str(error.get("message", "")), data if isinstance(data, dict) else {})


def _with_transcript_line(params: dict[str, Any] | None, transcript_line: int | None) -> dict[str, Any]:
    result = dict(params or {})
    if transcript_line is not None:
        meta = result.get("_meta")
        result["_meta"] = {**(meta if isinstance(meta, dict) else {}), "transcriptLine": transcript_line}
    return result


def connect_raw(port: int, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> socket.socket:
    """A TCP connection to 127.0.0.1:`port` without any handshake (security tests)."""
    try:
        connection = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except (ConnectionRefusedError, ConnectionResetError, ConnectionAbortedError) as error:
        raise ConnectionClosed(f"the editor on port {port} refused the connection: {error}") from error
    except socket.timeout:
        raise TimeoutError(f"connecting to port {port} took longer than {timeout:.1f} s") from None
    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return connection


# --------------------------------------------------------------------------------------------------------------------
# Launching editors
# --------------------------------------------------------------------------------------------------------------------


def output_directory_name(config: str) -> str:
    """bin/<Config>-<system>-<arch> as premake names it (Scripts/Lib/paths.py output_directory_name)."""
    if sys.platform == "win32":
        return f"{config}-windows-x86_64"
    if sys.platform == "darwin":
        return f"{config}-macosx-AARCH64"
    return f"{config}-linux-x86_64"


def configurations_from_environment() -> tuple[str, ...]:
    """The build configurations whose editor is started, in order of preference: ENGINE_AUTOMATION_CONFIG alone when set,
    else Release, then Debug (§13.8: Release first)."""
    config = os.environ.get(CONFIG_ENVIRONMENT_VARIABLE, "")
    return (config,) if config else DEFAULT_CONFIGURATIONS


def find_editor_executable(repository_root: Path, configurations: tuple[str, ...] = DEFAULT_CONFIGURATIONS) -> Path:
    """bin/<Config>-<system>-<arch>/Editor/Editor(.exe) of the first configuration that is built (§13.8: the bridge
    prefers Release). Raises FileNotFoundError naming `python Scripts/Build.py --config Release --project Editor`."""
    name = "Editor.exe" if sys.platform == "win32" else "Editor"
    candidates = [repository_root / "bin" / output_directory_name(config) / "Editor" / name
                  for config in configurations]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    tried = ", ".join(str(candidate) for candidate in candidates)
    config = configurations[0] if configurations else "Release"
    raise FileNotFoundError(f"no editor is built ({tried}): run python Scripts/Build.py --config {config} --project "
                            f"Editor")


@dataclasses.dataclass
class EditorProcess:
    """An editor started by launch_editor. Its console output (standard output and error) is drained by background
    threads, so the editor never blocks on a full pipe; the last OUTPUT_TAIL_LINES lines are kept."""

    process: subprocess.Popen[bytes]
    session: SessionInfo | None
    user_data_dir: Path
    output_tail: collections.deque[str] = dataclasses.field(
        default_factory=lambda: collections.deque(maxlen=OUTPUT_TAIL_LINES))
    started_at: float = dataclasses.field(default_factory=time.time)
    readers: list[threading.Thread] = dataclasses.field(default_factory=list)

    def connect(self, client_name: str = CLIENT_NAME, call_log: CallLog | None = None) -> EngineClient:
        """EngineClient.connect_session on this editor's session."""
        if self.session is None:
            raise ConnectionClosed(f"the editor (pid {self.process.pid}) wrote no session file: it does not listen")
        return EngineClient.connect_session(self.session, client_name=client_name, call_log=call_log)

    def wait(self, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> int:
        """Waits for the process to exit and returns its exit code."""
        try:
            exit_code = self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            raise TimeoutError(f"the editor (pid {self.process.pid}) did not exit within {timeout:.0f} s") from None
        self.join_readers()
        return exit_code

    def kill(self) -> None:
        """Kills the process (crash tests) and waits for it."""
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.join_readers()

    def output(self) -> str:
        """The kept tail of the editor's console output."""
        return "\n".join(self.output_tail)

    def join_readers(self) -> None:
        """Waits until the output readers have drained the pipes of the exited process."""
        for reader in self.readers:
            reader.join(timeout=10.0)


def _drain(stream: IO[bytes], tail: collections.deque[str]) -> None:
    """Reads `stream` to its end, keeping its last lines in `tail`."""
    with stream:
        for line in iter(stream.readline, b""):
            tail.append(line.decode("utf-8", errors="replace").rstrip("\r\n"))


class CapturedProcess(subprocess.Popen[bytes]):
    """A process started by launch_editor; its EditorProcess drains the console output into `output_tail`."""

    output_tail: collections.deque[str]


def _process_output_tail(process: subprocess.Popen[bytes], lines: int = 20) -> str:
    """The end of an exited process's console output: what launch_editor kept, or its captured standard error."""
    if isinstance(process, CapturedProcess):
        # The output readers finish shortly after the process exits.
        time.sleep(POLL_INTERVAL_SECONDS)
        return "\n".join(list(process.output_tail)[-lines:]) or "(no output)"
    if process.stderr is None:
        return "(no output captured)"
    try:
        text = process.stderr.read().decode("utf-8", errors="replace")
    except (OSError, ValueError):
        return "(output already consumed)"
    return "\n".join(text.splitlines()[-lines:]) or "(no output)"


def launch_editor(executable: Path, arguments: list[str], user_data_dir: Path, app_name: str,
                  wait_for_session: bool = True, environment: dict[str, str] | None = None) -> EditorProcess:
    """Starts `executable` with `arguments` plus --user-data-dir=<user_data_dir> (argument list, never a shell) and,
    with `wait_for_session`, waits up to LAUNCH_TIMEOUT_SECONDS for its session file in
    sessions_directory(app_name, user_data_dir). An editor that exits first is reported with its exit code and the tail
    of its standard error in a RuntimeError. Its standard input is empty and its output is captured, never inherited:
    the MCP bridge speaks MCP on its own standard streams."""
    user_data_dir = user_data_dir.resolve()
    command = [str(executable), *arguments, f"--user-data-dir={user_data_dir}"]
    process = CapturedProcess(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=environment)
    editor = EditorProcess(process=process, session=None, user_data_dir=user_data_dir)
    process.output_tail = editor.output_tail
    for stream in (process.stdout, process.stderr):
        if stream is not None:
            reader = threading.Thread(target=_drain, args=(stream, editor.output_tail), daemon=True,
                                      name=f"editor-{process.pid}-output")
            reader.start()
            editor.readers.append(reader)
    if not wait_for_session:
        return editor

    session_file = sessions_directory(app_name, user_data_dir) / f"{process.pid}.json"
    deadline = time.monotonic() + LAUNCH_TIMEOUT_SECONDS
    while True:
        session = _parse_session(session_file) if session_file.is_file() else None
        if session is not None:
            editor.session = session
            return editor
        if process.poll() is not None:
            editor.join_readers()
            raise RuntimeError(f"{executable.name} exited with code {process.returncode} before it listened for "
                               f"automation:\n{editor.output()}")
        if time.monotonic() >= deadline:
            editor.kill()
            raise TimeoutError(f"{executable.name} wrote no session file within {LAUNCH_TIMEOUT_SECONDS:.0f} s "
                               f"({session_file}):\n{editor.output()}")
        time.sleep(POLL_INTERVAL_SECONDS)


# --------------------------------------------------------------------------------------------------------------------
# Operating-system helpers
# --------------------------------------------------------------------------------------------------------------------


@functools.lru_cache(maxsize=None)
def _windows_kernel32() -> Any:
    """kernel32 through ctypes, with the signatures used here (Windows only; loaded once)."""
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    overlapped = ctypes.POINTER(_windows_overlapped_type())
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.LockFileEx.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                    overlapped)
    kernel32.LockFileEx.restype = wintypes.BOOL
    kernel32.UnlockFileEx.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, overlapped)
    kernel32.UnlockFileEx.restype = wintypes.BOOL
    return kernel32


@functools.lru_cache(maxsize=None)
def _windows_overlapped_type() -> Any:
    """The OVERLAPPED structure, built on first use so this module imports on every platform."""
    import ctypes
    from ctypes import wintypes

    class Overlapped(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t), ("Offset", wintypes.DWORD),
                    ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]

    return Overlapped


def _windows_lock_overlapped() -> Any:
    """An OVERLAPPED naming WINDOWS_LOCK_OFFSET."""
    return _windows_overlapped_type()(0, 0, WINDOWS_LOCK_OFFSET & 0xFFFF_FFFF, WINDOWS_LOCK_OFFSET >> 32, None)


def windows_lock_byte(handle: int, exclusive: bool, wait: bool) -> bool:
    """Locks the project-lock byte (WINDOWS_LOCK_OFFSET) of the open file `handle`; False when another handle holds a
    conflicting lock and `wait` is false (ERROR_LOCK_VIOLATION). Raises OSError for other failures."""
    import ctypes

    kernel32 = _windows_kernel32()
    overlapped = _windows_lock_overlapped()
    flags = (0x2 if exclusive else 0) | (0 if wait else 0x1)  # LOCKFILE_EXCLUSIVE_LOCK, LOCKFILE_FAIL_IMMEDIATELY
    if kernel32.LockFileEx(handle, flags, 0, 1, 0, ctypes.byref(overlapped)):
        return True
    error = ctypes.get_last_error()
    if error == 33 and not wait:  # ERROR_LOCK_VIOLATION
        return False
    raise ctypes.WinError(error)


def windows_unlock_byte(handle: int) -> None:
    """Releases windows_lock_byte's lock."""
    import ctypes

    _windows_kernel32().UnlockFileEx(handle, 0, 1, 0, ctypes.byref(_windows_lock_overlapped()))


def _windows_lock_held(lock_file: Path) -> bool | None:
    """Whether another handle holds the project lock (a shared probe conflicts only with the holder's exclusive lock);
    None when the file cannot be opened."""
    import ctypes

    kernel32 = _windows_kernel32()
    generic_read, share_all, open_existing, normal = 0x80000000, 0x7, 3, 0x80
    handle = kernel32.CreateFileW(str(lock_file), generic_read, share_all, None, open_existing, normal, None)
    if handle is None or handle == ctypes.c_void_p(-1).value:
        return None
    try:
        if windows_lock_byte(handle, exclusive=False, wait=False):
            windows_unlock_byte(handle)
            return False
        return True
    finally:
        kernel32.CloseHandle(handle)


def _posix_lock_held(lock_file: Path) -> bool | None:
    """Whether another process holds flock on the file; None when it cannot be opened."""
    import fcntl

    try:
        descriptor = os.open(lock_file, os.O_RDONLY)
    except OSError:
        return None
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        return False
    finally:
        os.close(descriptor)


def _windows_process_alive(pid: int) -> bool:
    import ctypes

    kernel32 = _windows_kernel32()
    synchronize, query_limited_information = 0x00100000, 0x1000
    handle = kernel32.OpenProcess(synchronize | query_limited_information, False, pid)
    if not handle:
        # Access denied means the process exists (another user's or a protected one); anything else means it does not.
        return ctypes.get_last_error() == 5
    try:
        return kernel32.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT: still running
    finally:
        kernel32.CloseHandle(handle)


def _environment_coverage_path() -> Path | None:
    value = os.environ.get(COVERAGE_ENVIRONMENT_VARIABLE, "")
    return Path(value) if value else None
