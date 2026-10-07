"""Launch or attach (Docs/Architecture.md §13.8 "Launch or attach", §4.13): two processes never write the same project.

editor_launch {project, create?, template?, headless?, renderer?}:
  1. A live session file whose projectPath names the project (engine_client.find_session): attach to that editor, whose
     automation server is listening (it runs with --automation or "Allow AI automation"). Nothing is spawned.
  2. Otherwise, when <Project>/Library/Editor.lock is held (an editor without automation): fail with
     EditorAlreadyOpen {pid} (the pid from the lock file) and the hint "enable Allow AI automation in the editor's
     Automation panel, then call editor_attach".
  3. Otherwise spawn bin/<cfg>-<sys>-<arch>/Editor/Editor(.exe) (Release preferred; a missing binary is an error naming
     "python Scripts/Build.py --config Release --project Editor") with --automation --project <path> (headless:
     --headless; renderer: --renderer none by default in M4), and supervise it. With create, the project is created
     first through project.create on a launcher-state editor.
editor_attach {project? | pid?}: attaches explicitly to a live session. The bridge never supervises or kills an editor
it attached to; editor_shutdown on an attached editor only disconnects.

Spawned editors get --user-data-dir with the launcher's user-data root (the OS's by default), so the bridge finds their
session files and crash reports where it looks.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

import engine_client
from engine_mcp.connection import EditorConnection
from engine_mcp.transcript import CLIENT_NAME

ALREADY_OPEN_HINT = "enable Allow AI automation in the editor's Automation panel, then call editor_attach"
PROJECT_FILE_EXTENSION = ".eproj"
RENDERERS = ("vulkan", "none")


@dataclasses.dataclass(eq=False)
class EditorAlreadyOpen(Exception):
    """An editor holds the project lock without a listening automation server."""

    pid: int

    def __str__(self) -> str:
        return f"an editor (pid {self.pid}) has the project open without automation: {ALREADY_OPEN_HINT}"

    def to_json(self) -> dict[str, Any]:
        """{"error": "EditorAlreadyOpen", "pid": pid, "hint": ALREADY_OPEN_HINT}."""
        return {"error": "EditorAlreadyOpen", "pid": self.pid, "hint": ALREADY_OPEN_HINT}


@dataclasses.dataclass(frozen=True)
class LaunchRequest:
    """editor_launch's arguments."""

    project: Path
    create: bool = False
    template: str = "Empty"
    headless: bool = True
    renderer: str = "none"


def find_project_file(project: Path) -> Path | None:
    """The .eproj `project` names: itself, or the only .eproj in the directory; None when there is none yet. Raises
    ValueError for a directory with several project files or a file that is not one."""
    if project.is_file():
        if project.suffix != PROJECT_FILE_EXTENSION:
            raise ValueError(f"{project} is not a {PROJECT_FILE_EXTENSION} project file")
        return project.resolve()
    if not project.is_dir():
        return None
    candidates = sorted(path for path in project.glob(f"*{PROJECT_FILE_EXTENSION}") if path.is_file())
    if len(candidates) > 1:
        names = ", ".join(path.name for path in candidates)
        raise ValueError(f"{project} holds several project files ({names}); name one of them")
    return candidates[0].resolve() if candidates else None


class Launcher:
    """Finds, starts and attaches to editors for one bridge process."""

    def __init__(self, repository_root: Path, user_data_root: Path | None = None,
                 configurations: tuple[str, ...] = ("Release", "Debug")) -> None:
        self.repository_root = repository_root
        self.user_data_root = user_data_root
        # The build configurations whose editor is spawned, in order of preference (§13.8: Release first).
        self.configurations = configurations

    @property
    def app_name(self) -> str:
        return engine_client.read_workspace_name(self.repository_root)

    def sessions_directory(self) -> Path:
        return engine_client.sessions_directory(self.app_name, self.user_data_root)

    def launch(self, request: LaunchRequest) -> EditorConnection:
        """editor_launch (see the module comment). Raises EditorAlreadyOpen, FileNotFoundError for a missing binary, or
        RuntimeError when the spawned editor exits before it listens (with its exit code: 3 for a locked or invalid
        project)."""
        if request.renderer not in RENDERERS:
            raise ValueError(f"renderer must be one of {', '.join(RENDERERS)} (got '{request.renderer}')")
        project = request.project.expanduser().resolve()
        project_file = find_project_file(project)
        if project_file is None and not request.create:
            raise FileNotFoundError(f"no project at {project}: pass create: true to create one")
        if project_file is not None:
            session = engine_client.find_session(self.sessions_directory(), project_file)
            if session is not None:
                return self.connect(session, supervised=False, editor=None)
            holder = engine_client.read_lock_holder(project_file.parent)
            if holder is not None:
                raise EditorAlreadyOpen(holder)
        executable = engine_client.find_editor_executable(self.repository_root, self.configurations)
        arguments = ["--automation", "--renderer", request.renderer, *(["--headless"] if request.headless else [])]
        if project_file is not None:
            arguments += ["--project", str(project_file)]
        editor = engine_client.launch_editor(executable, arguments, self.user_data_directory(), self.app_name)
        assert editor.session is not None
        try:
            connection = self.connect(editor.session, supervised=True, editor=editor)
        except BaseException:
            editor.kill()
            raise
        if project_file is None:
            self.create_project(connection, project, request.template)
        return connection

    def attach(self, project: Path | None = None, pid: int | None = None) -> EditorConnection:
        """editor_attach: the live session of `project` or of `pid` (exactly one, ValueError otherwise). Raises
        LookupError when no such session exists."""
        if (project is None) == (pid is None):
            raise ValueError("editor_attach takes exactly one of project and pid")
        sessions = self.sessions_directory()
        if project is not None:
            project_file = find_project_file(project.expanduser().resolve())
            if project_file is None:
                raise LookupError(f"no project at {project}")
            session = engine_client.find_session(sessions, project_file)
            if session is None:
                holder = engine_client.read_lock_holder(project_file.parent)
                detail = f" (pid {holder} has it open without automation: {ALREADY_OPEN_HINT})" if holder else ""
                raise LookupError(f"no editor with automation serves {project_file}{detail}")
        else:
            session = next((item for item in engine_client.list_sessions(sessions) if item.pid == pid), None)
            if session is None:
                raise LookupError(f"no editor with automation runs as pid {pid} (session files in {sessions})")
        return self.connect(session, supervised=False, editor=None)

    # ----------------------------------------------------------------------------------------------------------------

    def user_data_directory(self) -> Path:
        return self.user_data_root if self.user_data_root is not None else engine_client.default_user_data_root()

    def connect(self, session: engine_client.SessionInfo, supervised: bool,
                editor: engine_client.EditorProcess | None) -> EditorConnection:
        client = engine_client.EngineClient.connect_session(session, client_name=CLIENT_NAME,
                                                            call_log=engine_client.CallLog.from_environment())
        project_root = Path(session.project_path).parent if session.project_path else None
        return EditorConnection(project_root, supervised, client=client, editor=editor, session=session,
                                app_name=self.app_name)

    @staticmethod
    def create_project(connection: EditorConnection, project: Path, template: str) -> None:
        """project.create on the launcher-state editor just spawned; it also opens the project (ADR 0008 decision 13).
        A failure shuts the editor down again and is raised as RuntimeError."""
        response = connection.call("project.create", {"path": str(project), "name": project.name,
                                                      "template": template})
        if "error" in response:
            error = engine_client.error_from_response("project.create", response)
            connection.shutdown(force=True)
            raise RuntimeError(f"project.create failed: {error.detail} ({error.error_code})")
