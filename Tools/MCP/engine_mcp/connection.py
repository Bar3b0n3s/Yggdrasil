"""The bridge's connection to one editor (Docs/Architecture.md §13.8): framing, handshake and calls through
Tools/Automation/engine_client.py, each call bounded by its method's timeoutSeconds (from the catalogue for a tool, and
for engine_call as the editor's rpc.discover declares it, method_timeout; default 60),
every request written to the transcript first so its line number travels as params._meta.transcriptLine, and crash
detection: when the editor the bridge launched exits unexpectedly, the next call fails with EditorCrashed {exitCode,
lastLogLines (50), crashReportPath, autosaveAvailable}.

The transcript is committed with the game, so it names no machine path where it can avoid one
(Docs/Decisions/0026-m16-contract.md, friction S1-16): the path of a project.create or project.open inside the
repository is recorded relative to the repository root (the editor still receives the absolute path), and a relative
path an agent passes is resolved against the repository root, never the bridge's working directory.
"""

from __future__ import annotations

import dataclasses
import time
from pathlib import Path
from typing import Any

import engine_client
from engine_mcp.autosave import has_published_autosave
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
        """{"error": "EditorCrashed", "exitCode", "lastLogLines", "crashReportPath", "autosaveAvailable", "hint"}."""
        hint = ("call editor_launch with recover: true to start the editor again and adopt the autosave"
                if self.autosave_available else "call editor_launch to start the editor again")
        return {"error": "EditorCrashed", "exitCode": self.exit_code, "lastLogLines": list(self.last_log_lines),
                "crashReportPath": self.crash_report_path, "autosaveAvailable": self.autosave_available,
                "hint": hint}


class EditorConnection:
    """One connected editor: launched (supervised) or attached (never killed, §13.8)."""

    def __init__(self, project_root: Path | None, supervised: bool, client: engine_client.EngineClient | None = None,
                 editor: engine_client.EditorProcess | None = None, session: engine_client.SessionInfo | None = None,
                 app_name: str = "", repository_root: Path | None = None) -> None:
        self.project_root = project_root
        self.supervised = supervised
        # The repository the bridge serves: relative project paths are resolved against it, and project paths inside it
        # are recorded relative to it (transcript_params). None: the working directory, and paths recorded as given.
        self.repository_root = repository_root
        self.transcript: Transcript | None = Transcript.open(project_root) if project_root is not None else None
        self.client = client
        # The launched editor (supervised connections only) and the session file it was reached through.
        self.editor = editor
        self.session = session
        # The application name, which names the editor's crash report folder under its user-data root.
        self.app_name = app_name
        self.shutdown_requested = False
        self.crashed: EditorCrashed | None = None
        # The editor's {recoveryAvailable, recovered} when editor_launch opened the project through project.open (§13.8
        # "Resilience"); None when it opened another way.
        self.recovery: dict[str, bool] | None = None
        # The session.info result that began the session (Launcher.read_session_info: first thing after an attach, right
        # after a launch's project.create or project.open); None until then, or when the editor refused it.
        self.session_info: dict[str, Any] | None = None
        # The timeoutSeconds of each method the editor declared, by domain, as method_timeout asked for them.
        self.method_timeouts: dict[str, dict[str, float]] = {}

    def call(self, method: str, params: dict[str, Any] | None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """Writes the request to the transcript, sends it with its transcript line, writes the response summary and
        returns the response message. Raises EditorCrashed when a supervised editor has died, ConnectionClosed
        otherwise."""
        self.check_editor()
        if self.client is None:
            raise engine_client.ConnectionClosed("no editor is connected: call editor_launch or editor_attach")
        self.resolve_pending()
        # Whether the editor had confirmed a project before this call: a transcript still pending belongs to a project
        # the editor has not confirmed (a project.create or project.open that timed out, resolve_pending).
        had_project = self.transcript is not None and not self.transcript.is_pending
        params = self.prepare_params(method, dict(params or {}))
        transcript = self.transcript if method not in UNRECORDED_METHODS else None
        request_id = self.client.next_id
        line = (transcript.append_request(request_id, method, self.transcript_params(method, params))
                if transcript is not None else None)
        try:
            response = self.client.request(method, params, transcript_line=line, timeout=timeout)
        except (engine_client.ConnectionClosed, engine_client.ProtocolError) as error:
            failure = self.connection_failed(error)
            if transcript is not None and line is not None:
                transcript.append_response(request_id, line, _bridge_error(request_id, failure))
            self.resolve_pending()
            if failure is error:
                raise
            raise failure from error
        except TimeoutError as error:
            if transcript is not None and line is not None:
                transcript.append_response(request_id, line, _bridge_error(request_id, error, "Timeout"))
            # The editor may still finish the project.create or project.open and record provenance at its line, so
            # the lines are written as soon as the project file exists: now, or at the start of a later call.
            self.resolve_pending()
            raise
        if transcript is not None and line is not None:
            transcript.append_response(request_id, line, response)
        self.after_response(method, response, had_project)
        return response

    def method_timeout(self, method: str) -> float | None:
        """The timeoutSeconds the connected editor declares for `method` (Docs/Decisions/0026-m16-contract.md decision
        13), from rpc.discover {domain}, asked once per domain and kept for the connection. That lookup is the bridge's
        own read, not an agent's call, so it is not transcribed. None when the editor cannot tell: no connection, a
        method without a domain or that the editor does not have, or a discovery that failed (the call itself then
        reports the broken connection or the crash)."""
        domain, separator, _ = method.partition(".")
        if self.client is None or not separator or not domain:
            return None
        if domain not in self.method_timeouts:
            try:
                response = self.client.request("rpc.discover", {"domain": domain})
            except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError):
                return None
            result = response.get("result")
            if not isinstance(result, dict):
                return None
            try:
                catalogue = engine_client.load_offloaded(result)
            except (OSError, ValueError, engine_client.ProtocolError):
                return None
            timeouts: dict[str, float] = {}
            for entry in catalogue.get("methods", []):
                if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
                    continue
                seconds = entry.get("timeoutSeconds")
                if isinstance(seconds, (int, float)) and not isinstance(seconds, bool) and seconds > 0:
                    timeouts[entry["name"]] = float(seconds)
            self.method_timeouts[domain] = timeouts
        return self.method_timeouts[domain].get(method)

    def is_alive(self) -> bool:
        """Whether the connection is open and, for a supervised editor, its process runs."""
        if self.client is None or self.crashed is not None:
            return False
        return self.editor is None or self.editor.process.poll() is None

    def disconnect(self) -> None:
        """Closes the connection; an attached editor keeps running. A transcript still pending after a project.create or
        project.open the bridge stopped waiting for is resolved first, so its lines reach the file when the project
        exists by now (the attached shutdown, a new launch or attach, and the bridge's exit all end here)."""
        self.resolve_pending()
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
        if self.recovery is not None:
            status.update(self.recovery)
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
        """Whether the project has a complete published recovery candidate (§4.13; final validation is the editor's)."""
        return self.project_root is not None and has_published_autosave(self.project_root)

    def resolve_pending(self) -> None:
        """Writes a pending transcript's lines once its directory holds a project file (Transcript.flush_pending). The
        transcript stays pending after a project.create or project.open the bridge stopped waiting for (a timeout or a
        broken connection), since the editor may still finish it and record provenance at its line; every later call
        and every such failure resolves it again, so those lines are never kept in memory only for good."""
        if self.transcript is not None and self.transcript.is_pending:
            self.transcript.flush_pending()

    def prepare_params(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Makes a relative project path absolute (against the repository root, or the working directory without one)
        before project.create or project.open, so the editor and the bridge agree on the project and the bridge knows
        which project's transcript the request belongs to; with no project yet, the transcript of the project being
        created or opened starts with this request, pending (kept in memory) until the editor confirms the project
        (transcript.py). A transcript still pending for another directory (an earlier project.create or project.open
        that timed out, whose directory still holds no project file, so the editor recorded nothing there) is dropped
        first."""
        if method not in _PROJECT_METHODS or not isinstance(params.get("path"), str) or not params["path"]:
            return params
        path = Path(params["path"]).expanduser()
        if not path.is_absolute():
            path = (self.repository_root / path if self.repository_root is not None else path).resolve()
            params["path"] = str(path)
        root = path.parent if path.suffix == ".eproj" else path
        if (self.transcript is not None and self.transcript.is_pending and self.project_root is not None
                and not _same_path(root, self.project_root)):
            self.transcript.discard_pending()
            self.transcript = None
            self.project_root = None
        if self.transcript is None:
            self.transcript = Transcript.open(root, pending=True)
            self.project_root = root
        return params

    def transcript_params(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """The params as the transcript records them: those the editor receives, except that a project.create or
        project.open path inside the repository is recorded relative to the repository root, with '/' separators."""
        path = params.get("path")
        if method not in _PROJECT_METHODS or self.repository_root is None or not isinstance(path, str) or not path:
            return params
        relative = relative_path(Path(path), self.repository_root)
        return params if relative is None else {**params, "path": relative}

    def after_response(self, method: str, response: dict[str, Any], had_project: bool) -> None:
        result = response.get("result")
        if not isinstance(result, dict):
            if method in _PROJECT_METHODS and not had_project:
                # The editor still has no project: the next project call chooses the transcript again. A refused request
                # whose directory holds a project file keeps its lines, since the editor may have written and recorded
                # files under its line before refusing (project.create records what it created before its open, an
                # open records the .meta files its scan wrote before a later failure; the milestone review's R05).
                # Otherwise its lines were kept in memory only and are dropped, so nothing is written to a directory
                # that is not a project.
                if self.transcript is not None and not self.transcript.flush_pending():
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


def relative_path(path: Path, root: Path) -> str | None:
    """`path` relative to `root` with '/' separators ("." for the root itself), or None when it lies outside it (both
    resolved, so a symbolic link counts where it leads)."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except (OSError, ValueError):
        return None


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
