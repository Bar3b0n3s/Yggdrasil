"""The bridge's connection to one editor (Docs/Architecture.md §13.8): framing, handshake and calls through
Tools/Automation/engine_client.py, each call bounded by its method's timeoutSeconds from the catalogue (default 60),
every request written to the transcript first so its line number travels as params._meta.transcriptLine, and crash
detection: when the editor the bridge launched exits unexpectedly, the next call fails with EditorCrashed {exitCode,
lastLogLines (50), crashReportPath, autosaveAvailable}.
"""

from __future__ import annotations

import dataclasses
import time
from pathlib import Path
from typing import Any

import engine_client
from engine_mcp.transcript import UNRECORDED_METHODS, Transcript

DEFAULT_TIMEOUT_SECONDS = 60.0
LAST_LOG_LINES = 50
# How long a supervised editor whose connection broke gets to exit before the break counts as a connection failure.
EXIT_GRACE_SECONDS = 5.0
# How long editor_shutdown waits for a launched editor to exit after session.shutdown before killing it.
SHUTDOWN_TIMEOUT_SECONDS = 30.0
# Methods that change which project the editor has open, and so which transcript the bridge writes.
_PROJECT_METHODS = frozenset({"project.create", "project.open"})


@dataclasses.dataclass(eq=False)
class EditorCrashed(Exception):
    """The supervised editor died (§13.8 "Resilience")."""

    exit_code: int | None
    last_log_lines: list[str]
    crash_report_path: str
    autosave_available: bool

    def __str__(self) -> str:
        report = f"; crash report {self.crash_report_path}" if self.crash_report_path else ""
        return f"the editor exited unexpectedly with code {self.exit_code}{report}"

    def to_json(self) -> dict[str, Any]:
        """{"error": "EditorCrashed", "exitCode", "lastLogLines", "crashReportPath", "autosaveAvailable"}."""
        return {"error": "EditorCrashed", "exitCode": self.exit_code, "lastLogLines": list(self.last_log_lines),
                "crashReportPath": self.crash_report_path, "autosaveAvailable": self.autosave_available,
                "hint": "call editor_launch to start the editor again"}


class EditorConnection:
    """One connected editor: launched (supervised) or attached (never killed, §13.8)."""

    def __init__(self, project_root: Path | None, supervised: bool, client: engine_client.EngineClient | None = None,
                 editor: engine_client.EditorProcess | None = None, session: engine_client.SessionInfo | None = None,
                 app_name: str = "") -> None:
        self.project_root = project_root
        self.supervised = supervised
        self.transcript: Transcript | None = Transcript.open(project_root) if project_root is not None else None
        self.client = client
        # The launched editor (supervised connections only) and the session file it was reached through.
        self.editor = editor
        self.session = session
        # The application name, which names the editor's crash report folder under its user-data root.
        self.app_name = app_name
        self.shutdown_requested = False
        self.crashed: EditorCrashed | None = None

    def call(self, method: str, params: dict[str, Any] | None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """Writes the request to the transcript, sends it with its transcript line, writes the response summary and
        returns the response message. Raises EditorCrashed when a supervised editor has died, ConnectionClosed
        otherwise."""
        self.check_editor()
        if self.client is None:
            raise engine_client.ConnectionClosed("no editor is connected: call editor_launch or editor_attach")
        had_project = self.transcript is not None
        params = self.prepare_params(method, dict(params or {}))
        transcript = self.transcript if method not in UNRECORDED_METHODS else None
        request_id = self.client.next_id
        line = transcript.append_request(request_id, method, params) if transcript is not None else None
        try:
            response = self.client.request(method, params, transcript_line=line, timeout=timeout)
        except (engine_client.ConnectionClosed, engine_client.ProtocolError) as error:
            failure = self.connection_failed(error)
            if transcript is not None and line is not None:
                transcript.append_response(request_id, line, _bridge_error(request_id, failure))
            if failure is error:
                raise
            raise failure from error
        except TimeoutError as error:
            if transcript is not None and line is not None:
                transcript.append_response(request_id, line, _bridge_error(request_id, error, "Timeout"))
            raise
        if transcript is not None and line is not None:
            transcript.append_response(request_id, line, response)
        self.after_response(method, response, had_project)
        return response

    def is_alive(self) -> bool:
        """Whether the connection is open and, for a supervised editor, its process runs."""
        if self.client is None or self.crashed is not None:
            return False
        return self.editor is None or self.editor.process.poll() is None

    def disconnect(self) -> None:
        """Closes the connection; an attached editor keeps running."""
        if self.client is not None:
            self.client.close()
            self.client = None

    def shutdown(self, save: bool = False, force: bool = False) -> dict[str, Any]:
        """editor_shutdown: a launched editor gets session.shutdown {save, force} and is waited for (killed after
        SHUTDOWN_TIMEOUT_SECONDS); an attached one is only disconnected (§13.8)."""
        if not self.supervised or self.editor is None:
            self.disconnect()
            return {"disconnected": True, "shutDown": False, "reason": "the editor was attached, so it keeps running"}
        result: dict[str, Any] = {"disconnected": True, "shutDown": True}
        if self.editor.process.poll() is None and self.client is not None:
            response = self.call("session.shutdown", {"save": save, "force": force})
            if "error" in response:
                return {"disconnected": False, "shutDown": False, "response": response}
            result["response"] = response
            try:
                result["exitCode"] = self.editor.wait(SHUTDOWN_TIMEOUT_SECONDS)
            except TimeoutError:
                self.editor.kill()
                result["exitCode"] = self.editor.process.returncode
                result["killed"] = True
        else:
            result["exitCode"] = self.editor.process.returncode
        self.disconnect()
        return result

    def describe(self) -> dict[str, Any]:
        """editor_status: what the bridge is connected to."""
        pid = self.editor.process.pid if self.editor is not None else (self.session.pid if self.session else None)
        status: dict[str, Any] = {
            "connected": self.is_alive(),
            "supervised": self.supervised,
            "pid": pid,
            "port": self.session.port if self.session is not None else None,
            "project": self.project_root.as_posix() if self.project_root is not None else "",
            "transcript": self.transcript.path.as_posix() if self.transcript is not None else "",
        }
        if self.client is not None and self.client.hello:
            status["engineVersion"] = self.client.hello.get("engineVersion", "")
            status["protocolVersion"] = self.client.hello.get("protocolVersion", "")
        if self.crashed is not None:
            status["crashed"] = self.crashed.to_json()
        return status

    # ----------------------------------------------------------------------------------------------------------------

    def check_editor(self) -> None:
        """Raises EditorCrashed (again) once a supervised editor has died unexpectedly."""
        if self.crashed is not None:
            raise self.crashed
        if self.editor is not None and self.editor.process.poll() is not None:
            if self.shutdown_requested:
                self.disconnect()
                raise engine_client.ConnectionClosed("the editor has shut down: call editor_launch to start it again")
            self.crashed = self.make_crashed()
            self.disconnect()
            raise self.crashed

    def connection_failed(self, error: Exception) -> Exception:
        """What a broken connection means: EditorCrashed when the supervised editor died, else the error itself."""
        self.disconnect()
        if self.editor is None:
            return error
        deadline = time.monotonic() + EXIT_GRACE_SECONDS
        while self.editor.process.poll() is None and time.monotonic() < deadline:
            time.sleep(engine_client.POLL_INTERVAL_SECONDS)
        if self.editor.process.poll() is None or self.shutdown_requested:
            return error
        self.crashed = self.make_crashed()
        return self.crashed

    def make_crashed(self) -> EditorCrashed:
        assert self.editor is not None
        self.editor.join_readers()
        lines = list(self.editor.output_tail)[-LAST_LOG_LINES:]
        return EditorCrashed(exit_code=self.editor.process.returncode, last_log_lines=lines,
                             crash_report_path=self.find_crash_report(), autosave_available=self.autosave_available())

    def find_crash_report(self) -> str:
        """The newest crash report the editor wrote after it started (§4.13: <UserData>/<AppName>/Crashes/)."""
        if self.editor is None or not self.app_name:
            return ""
        directory = self.editor.user_data_dir / self.app_name / "Crashes"
        try:
            reports = [path for path in directory.iterdir() if path.is_file()]
        except OSError:
            return ""
        recent = [path for path in reports if path.stat().st_mtime >= self.editor.started_at - 1.0]
        if not recent:
            return ""
        return max(recent, key=lambda path: path.stat().st_mtime).as_posix()

    def autosave_available(self) -> bool:
        """Whether the project has an autosave to recover (§4.13: Library/Autosave/, from M10)."""
        if self.project_root is None:
            return False
        autosave = self.project_root / "Library" / "Autosave"
        try:
            return autosave.is_dir() and any(path.is_file() for path in autosave.rglob("*"))
        except OSError:
            return False

    def prepare_params(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Makes a relative project path absolute (against the bridge's working directory) before project.create or
        project.open, so the bridge knows which project's transcript the request belongs to; with no project yet, the
        transcript of the project being created or opened starts with this request, pending (kept in memory) until the
        editor confirms the project (transcript.py)."""
        if method not in _PROJECT_METHODS or not isinstance(params.get("path"), str) or not params["path"]:
            return params
        path = Path(params["path"]).expanduser()
        if not path.is_absolute():
            path = path.resolve()
            params["path"] = str(path)
        if self.transcript is None:
            root = path.parent if path.suffix == ".eproj" else path
            self.transcript = Transcript.open(root, pending=True)
            self.project_root = root
        return params

    def after_response(self, method: str, response: dict[str, Any], had_project: bool) -> None:
        result = response.get("result")
        if not isinstance(result, dict):
            if method in _PROJECT_METHODS and not had_project:
                # The editor still has no project: the next project call chooses the transcript again. The refused request
                # was kept in memory only and is dropped, so nothing is written to a directory that is not a project.
                if self.transcript is not None:
                    self.transcript.discard_pending()
                self.transcript = None
                self.project_root = None
            return
        if method == "session.shutdown":
            self.shutdown_requested = True
        project = result.get("project")
        if method in _PROJECT_METHODS and isinstance(project, dict) and isinstance(project.get("root"), str):
            root = Path(project["root"])
            if self.project_root is None or not _same_path(root, self.project_root):
                self.project_root = root
                self.transcript = Transcript.open(root)
            elif self.transcript is not None:
                self.transcript.flush_pending()


def _same_path(first: Path, second: Path) -> bool:
    try:
        return first.resolve() == second.resolve() or first.samefile(second)
    except OSError:
        return False


def _bridge_error(request_id: int, error: Exception, error_code: str = "") -> dict[str, Any]:
    """A response for the transcript when the bridge got none (the connection broke or the call timed out)."""
    if isinstance(error, EditorCrashed):
        error_code = "EditorCrashed"
    code = engine_client.TIMEOUT if error_code == "Timeout" else engine_client.INTERNAL
    return {"jsonrpc": "2.0", "id": request_id,
            "error": {"code": code, "message": str(error),
                      "data": {"errorCode": error_code or "ConnectionClosed", "detail": str(error)}}}
