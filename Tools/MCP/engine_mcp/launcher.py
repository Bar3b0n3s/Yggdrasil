"""Launch or attach (Docs/Architecture.md §13.8 "Launch or attach", §4.13): two processes never write the same project.

editor_launch {project, create?, template?, headless?, renderer?, recover?}:
  1. A live session file whose projectPath names the project (engine_client.find_session): attach to that editor, whose
     automation server is listening (it runs with --automation or "Allow AI automation"). Nothing is spawned. With
     recover this is an error: that editor has the project open, and only its own Recover prompt can adopt the autosave.
  2. Otherwise, when <Project>/Library/Editor.lock is held (an editor without automation): fail with
     EditorAlreadyOpen {pid} (the pid from the lock file) and the hint "enable Allow AI automation in the editor's
     Automation panel, then call editor_attach".
  3. Otherwise spawn bin/<cfg>-<sys>-<arch>/Editor/Editor(.exe) (Release preferred; a missing binary is an error naming
     "python Scripts/Build.py --config Release --project Editor") in the launcher state with --automation (headless:
     --headless; renderer: --renderer none by default, so a launch needs no GPU; "vulkan" serves viewport_screenshot and
     editor_screenshot, which a --renderer none editor answers with Unsupported), and supervise it. With create, the
     project is created through project.create (template: one of project.create's, Empty or Basic3D). An existing
     project opens through project.open {path, recover}, never through --project, so whatever opening it writes (the
     .meta files of new sources) is attributed to that transcribed request (§13.12 rule 2;
     Docs/Decisions/0026-m16-contract.md decision 11). project.open is also the one path that can adopt a recovery
     (§13.8 "Resilience"): the launch reports the editor's recoveryAvailable and recovered (a published autosave
     candidate, as after EditorCrashed {autosaveAvailable: true}), and recover: true installs the autosave as a dirty
     scene. Without recover a candidate is only reported, never adopted. After the project.create or project.open, the
     launch sends a transcribed session.info (Launcher.read_session_info; Docs/Decisions/0026-m16-contract.md, friction
     S3-06), so editor_launch reports the session with its open scene (none: opening a project opens no scene) as an
     attach does.
editor_attach {project? | pid?}: attaches explicitly to a live session. The bridge never supervises or kills an editor
it attached to; editor_shutdown on an attached editor only disconnects. Every attach, step 1's too, sends a transcribed
session.info first (Launcher.attach_session; Docs/Decisions/0026-m16-contract.md, friction S2-10), so the transcript
shows where each attached session begins: its first line names the editor's pid, project and renderer.
editor_status without a connection lists the editors that editor_attach {pid} can reach (Launcher.running_editors): one
per live session file, with its pid, project and renderer (the native test game's friction NT-04).

A relative project path names a path below the repository root ("Tests/Projects/Breakout"), whatever the bridge's
working directory (Launcher.resolve_project; Docs/Decisions/0026-m16-contract.md, friction S1-16 and prediction P09).

Spawned editors get --user-data-dir with the launcher's user-data root (the OS's by default), so the bridge finds their
session files and crash reports where it looks.

Machine-wide locks (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, Docs/Decisions/0028-backlog-program.md decision
16): before step 3 spawns an editor, the launch waits until no gate (CI.py) runs, and a rendering editor (renderer
"vulkan") takes the GPU lock, shared by every checkout of the repository, which the bridge holds from that launch until
the editor exits, however it exits (Launcher.release_when_exits), or until the bridge itself exits: an editor left
running with unsaved changes when the MCP client goes away holds none (Docs/Architecture.md §2.3). Under a process that
already holds the lock (the automation suite's Test.py, whose bridge tests launch rendering editors) the launch runs
under that hold and waits for nothing. The wait tries the lock without waiting (locks' no-wait acquisition) every
WAIT_POLL_SECONDS, so it can end between two tries when the launch's tool call is cancelled. While it waits, the
launcher's current activity (Activity) is the wait (LockWait: the lock, its holder's pid, worktree and command, when the
wait began and its bound), which the server's progress notifications carry to the client with the seconds waited
counted when each is sent (server.py, "Progress"); the bridge's log gets the same report when the wait starts, when the
holder changes and every locks.REPORT_SECONDS. A wait lasts at most the launcher's lock_wait_seconds (LOCK_WAIT_SECONDS
by default); then the launch fails with LockUnavailable naming the holder, and no editor is spawned. A launch whose tool
call the client cancels (the server sets the call's cancel event) ends its wait at the next try and raises
LaunchCancelled: it spawns nothing, and a lock it took as the call was cancelled is released again (a hold under an
ancestor's releases nothing of that hold), so the bridge is free for the next call at once
(Docs/Decisions/0036-m25-tetris-pixel-art.md, the milestone review). Attaching takes no lock: the attached editor is not
the bridge's.
"""

from __future__ import annotations

import dataclasses
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any, Callable

import engine_client
from engine_mcp.connection import EditorConnection
from engine_mcp.transcript import CLIENT_NAME
from Lib import locks

ALREADY_OPEN_HINT = "enable Allow AI automation in the editor's Automation panel, then call editor_attach"
PROJECT_FILE_EXTENSION = ".eproj"
RENDERERS = ("vulkan", "none")
# How long editor_launch waits for a running gate or GPU run before it fails naming the holder: 25 min, below the
# 30 min (1,800 s) a Claude Code client waits for a tool call that sends it nothing, so a client that asks for no
# progress notifications still receives the LockUnavailable that names the holder (Docs/Decisions/
# 0036-m25-tetris-pixel-art.md, friction F14).
LOCK_WAIT_SECONDS = 1500.0
# How long a launch's wait for a machine-wide lock pauses between two no-wait tries of it (locks' own poll interval),
# and so the longest it keeps waiting once its tool call is cancelled, besides one try's settle polls.
WAIT_POLL_SECONDS = locks.POLL_SECONDS
# How long editor_status waits for each running editor's connection and session.info (Launcher.running_editors).
PROBE_TIMEOUT_SECONDS = 5.0
LOGGER = logging.getLogger("engine_mcp")


def _unreported(message: str) -> None:
    """The reporter of locks' no-wait tries, which report nothing (Launcher.wait_for_lock reports the wait itself)."""


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
class LockWait:
    """editor_launch's wait for a machine-wide lock: what it waits for (locks' description: "gpu lock", "end of the
    running gate"), the lock in the way and its holders (pid, worktree, command, since), when the wait began
    (time.monotonic()) and its bound in seconds."""

    description: str
    lock: str
    holders: str
    started: float
    bound: float

    def describe(self, now: float) -> str:
        """The wait's report, worded as locks' own, with the seconds waited counted at `now` (time.monotonic())."""
        return (f"Waiting for the {self.description} ({now - self.started:.0f} s of at most {self.bound:.0f} s): the "
                f"{self.lock} lock is held by {self.holders}")


class Activity:
    """What the running tool call is doing beyond running: editor_launch's wait for a machine-wide lock while it waits,
    none otherwise. The worker thread that runs the call sets it, and the server's event loop reads it for the call's
    progress notifications, so a lock guards it."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._wait: LockWait | None = None

    def set(self, wait: LockWait | None) -> None:
        with self._lock:
            self._wait = wait

    def read(self, now: float | None = None) -> str:
        """The wait's report with the seconds waited counted at `now` (default: the current time.monotonic()), so each
        progress notification counts them when it is sent; "" when no wait runs."""
        with self._lock:
            wait = self._wait
        if wait is None:
            return ""
        return wait.describe(time.monotonic() if now is None else now)


class LaunchCancelled(Exception):
    """editor_launch's tool call was cancelled while the launch waited for a machine-wide lock: nothing was spawned, and
    a lock taken as it was cancelled was released again."""


@dataclasses.dataclass(frozen=True)
class LaunchRequest:
    """editor_launch's arguments."""

    project: Path
    create: bool = False
    template: str = "Empty"
    headless: bool = True
    renderer: str = "none"
    recover: bool = False


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
                 configurations: tuple[str, ...] = ("Release", "Debug"),
                 lock_wait_seconds: float = LOCK_WAIT_SECONDS) -> None:
        self.repository_root = repository_root
        self.user_data_root = user_data_root
        # The build configurations whose editor is spawned, in order of preference (§13.8: Release first).
        self.configurations = configurations
        # The bound of a launch's wait for the machine-wide locks (0: fail at once when one is held).
        self.lock_wait_seconds = lock_wait_seconds
        # The latest report of a launch's wait for those locks while it waits (take_machine_locks).
        self.activity = Activity()

    @property
    def app_name(self) -> str:
        return engine_client.read_workspace_name(self.repository_root)

    def sessions_directory(self) -> Path:
        return engine_client.sessions_directory(self.app_name, self.user_data_root)

    def resolve_project(self, project: Path) -> Path:
        """`project` as an absolute path: "~" expanded, and a relative path taken below the repository root, never the
        bridge's working directory (the MCP client decides that one)."""
        project = project.expanduser()
        return (project if project.is_absolute() else self.repository_root / project).resolve()

    def launch(self, request: LaunchRequest, cancelled: threading.Event | None = None) -> EditorConnection:
        """editor_launch (see the module comment); `cancelled` is set when the client cancels the tool call. Raises
        EditorAlreadyOpen, FileNotFoundError for a missing binary, locks.LockUnavailable when a gate or another GPU run
        kept the machine-wide locks for lock_wait_seconds, LaunchCancelled when `cancelled` was set while the launch
        waited for them (nothing is spawned then), or RuntimeError when the spawned editor exits before it listens (with
        its exit code), when
        project.create or project.open fails (a locked or invalid project, a refused recovery), or when recover asks to
        adopt an autosave that an editor already serving the project holds. Whatever else the project.create,
        project.open or session.info of a spawned editor raises (a timeout, a broken connection) propagates after that
        editor is killed, since no connection to it is returned."""
        if request.renderer not in RENDERERS:
            raise ValueError(f"renderer must be one of {', '.join(RENDERERS)} (got '{request.renderer}')")
        project = self.resolve_project(request.project)
        project_file = find_project_file(project)
        if project_file is None and not request.create:
            raise FileNotFoundError(f"no project at {project}: pass create: true to create one")
        if project_file is None and request.recover:
            raise ValueError(f"no project at {project} to recover: recover opens an existing project")
        if project_file is not None:
            session = engine_client.find_session(self.sessions_directory(), project_file)
            if session is not None:
                if request.recover:
                    raise RuntimeError(f"an editor (pid {session.pid}) already has {project_file} open; only its "
                                       "Recover prompt can adopt the autosave (attach without recover to use that "
                                       "editor)")
                return self.attach_session(session)
            holder = engine_client.read_lock_holder(project_file.parent)
            if holder is not None:
                raise EditorAlreadyOpen(holder)
        # A launcher-state editor, which creates or opens the project through a transcribed request, so every write of
        # the creation or the open (the .meta files the open's scan writes for new sources) is attributed to it.
        executable = engine_client.find_editor_executable(self.repository_root, self.configurations)
        arguments = ["--automation", "--renderer", request.renderer, *(["--headless"] if request.headless else [])]
        hold = self.take_machine_locks(request.renderer, project, cancelled)
        try:
            editor = engine_client.launch_editor(executable, arguments, self.user_data_directory(), self.app_name)
        except BaseException:
            if hold is not None:
                hold.release()
            raise
        if hold is not None:
            self.release_when_exits(editor, hold)
        assert editor.session is not None
        try:
            connection = self.connect(editor.session, supervised=True, editor=editor)
        except BaseException:
            editor.kill()
            raise
        try:
            if project_file is None:
                self.create_project(connection, project, request.template)
            else:
                self.open_project(connection, project_file, request.recover)
            self.read_session_info(connection)
        except BaseException:
            # The bridge never returns this connection, so nothing else would end the editor it spawned: a refused,
            # timed-out or broken project.create, project.open or session.info (the milestone review's R30).
            connection.disconnect()
            editor.kill()
            raise
        return connection

    def attach(self, project: Path | None = None, pid: int | None = None) -> EditorConnection:
        """editor_attach: the live session of `project` or of `pid` (exactly one, ValueError otherwise). Raises
        LookupError when no such session exists."""
        if (project is None) == (pid is None):
            raise ValueError("editor_attach takes exactly one of project and pid")
        sessions = self.sessions_directory()
        if project is not None:
            project_file = find_project_file(self.resolve_project(project))
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
        return self.attach_session(session)

    def running_editors(self) -> list[dict[str, Any]]:
        """The editors with automation running for this user (editor_status without a connection): one entry per live
        session file, by pid (engine_client.list_sessions, which leaves out the file of a pid that is gone), each
        {pid, project, headless, renderer}, `project` being the file's projectPath ("" in the launcher state). A session
        file does not hold the renderer, so each editor is asked for its session.info over a connection of its own,
        closed at once and never transcribed: it attaches nothing, and session.info writes nothing. An editor that
        cannot be reached or does not answer within PROBE_TIMEOUT_SECONDS has renderer None and probeError saying
        why."""
        editors: list[dict[str, Any]] = []
        for session in engine_client.list_sessions(self.sessions_directory()):
            entry: dict[str, Any] = {"pid": session.pid, "project": session.project_path, "headless": session.headless,
                                     "renderer": None}
            try:
                with engine_client.EngineClient.connect(session.port, session.token, client_name=CLIENT_NAME,
                                                        timeout=PROBE_TIMEOUT_SECONDS) as client:
                    info = client.call("session.info", {}, timeout=PROBE_TIMEOUT_SECONDS)
            except (OSError, engine_client.ConnectionClosed, engine_client.EngineError,
                    engine_client.ProtocolError) as error:
                entry["probeError"] = f"session.info failed: {error}"
            else:
                renderer = info.get("renderer")
                if isinstance(renderer, str):
                    entry["renderer"] = renderer
            editors.append(entry)
        return editors

    def attach_session(self, session: engine_client.SessionInfo) -> EditorConnection:
        """An attach (editor_attach, or editor_launch finding the editor that serves the project): the connection, then
        a transcribed session.info (Docs/Decisions/0026-m16-contract.md, friction S2-10), so every attach leaves a
        request line in the project's transcript, and the summary of its response names the editor's pid, project and
        renderer. The result is kept as EditorConnection.session_info; a session.info the editor refuses leaves the
        connection attached (the response line records why), and a connection that breaks is closed again and raised."""
        connection = self.connect(session, supervised=False, editor=None)
        try:
            self.read_session_info(connection)
        except BaseException:
            connection.disconnect()
            raise
        return connection

    @staticmethod
    def read_session_info(connection: EditorConnection) -> None:
        """The transcribed session.info that begins every attached or launched session (attach_session, and launch after
        its project.create or project.open), kept as EditorConnection.session_info: None when the editor refuses it,
        whose response line then records why. Raises what EditorConnection.call raises."""
        response = connection.call("session.info", {})
        result = response.get("result")
        connection.session_info = result if isinstance(result, dict) else None

    # ----------------------------------------------------------------------------------------------------------------

    def take_machine_locks(self, renderer: str, project: Path,
                           cancelled: threading.Event | None = None) -> locks.Hold | None:
        """Before an editor is spawned: the GPU lock for a rendering editor (the caller hands it to
        release_when_exits), or for any other only the wait until no gate runs (wait_for_lock). Raises
        locks.LockUnavailable after lock_wait_seconds, or LaunchCancelled once `cancelled` is set."""
        if renderer == "vulkan":
            command = f"MCP bridge (pid {os.getpid()}): a rendering editor for {project}"
            return self.wait_for_lock(f"{locks.GPU} lock", lambda: locks.acquire_gpu(0.0, _unreported, command),
                                      cancelled)
        return self.wait_for_lock("end of the running gate", lambda: locks.wait_for_gate(0.0, _unreported), cancelled)

    def wait_for_lock(self, description: str, attempt: Callable[[], locks.Hold | None],
                      cancelled: threading.Event | None) -> locks.Hold | None:
        """Tries `attempt` (one of locks' no-wait acquisitions, which raises LockUnavailable when the lock is held)
        every WAIT_POLL_SECONDS until it succeeds, lock_wait_seconds have passed (LockUnavailable naming the holders
        and the seconds waited) or `cancelled` is set (LaunchCancelled). A hold taken just as `cancelled` was set is
        released again, which releases nothing of an ancestor's hold (locks.Hold). While it waits, the wait is the
        current activity, and its report goes to the bridge's log when it starts, when the holders change and every
        locks.REPORT_SECONDS."""
        started = time.monotonic()
        logged: str | None = None
        next_log = started
        try:
            while True:
                if cancelled is not None and cancelled.is_set():
                    raise LaunchCancelled(f"editor_launch was cancelled while it waited for the {description}; "
                                          "nothing was spawned")
                try:
                    hold = attempt()
                except locks.LockUnavailable as error:
                    now = time.monotonic()
                    if now - started >= self.lock_wait_seconds:
                        raise locks.LockUnavailable(error.lock, error.holders, now - started,
                                                    timed_out=self.lock_wait_seconds > 0) from None
                    holders = "; ".join(holder.describe() for holder in error.holders) or "another process"
                    wait = LockWait(description, error.lock, holders, started, self.lock_wait_seconds)
                    self.activity.set(wait)
                    if holders != logged or now >= next_log:
                        LOGGER.info("%s", wait.describe(now))
                        logged = holders
                        next_log = now + locks.REPORT_SECONDS
                    pause = min(WAIT_POLL_SECONDS, max(0.0, started + self.lock_wait_seconds - now))
                    if cancelled is not None:
                        cancelled.wait(pause)
                    else:
                        time.sleep(pause)
                    continue
                if cancelled is not None and cancelled.is_set():
                    if hold is not None:
                        hold.release()
                    raise LaunchCancelled(f"editor_launch was cancelled as it took the {description}; nothing was "
                                          "spawned, and the lock was released")
                if logged is not None:
                    LOGGER.info("Took the %s after waiting %.0f s", description, time.monotonic() - started)
                return hold
        finally:
            self.activity.set(None)

    @staticmethod
    def release_when_exits(editor: engine_client.EditorProcess, hold: locks.Hold) -> threading.Thread:
        """Releases `hold` once the editor's process has exited, however it exits (editor_shutdown, a crash, a kill), on
        a daemon thread that waits for it; returns that thread."""

        def wait_then_release() -> None:
            try:
                editor.process.wait()
            finally:
                hold.release()

        watcher = threading.Thread(target=wait_then_release, daemon=True, name=f"gpu-lock-{editor.process.pid}")
        watcher.start()
        return watcher

    def user_data_directory(self) -> Path:
        return self.user_data_root if self.user_data_root is not None else engine_client.default_user_data_root()

    def connect(self, session: engine_client.SessionInfo, supervised: bool,
                editor: engine_client.EditorProcess | None) -> EditorConnection:
        client = engine_client.EngineClient.connect_session(session, client_name=CLIENT_NAME,
                                                            call_log=engine_client.CallLog.from_environment())
        project_root = Path(session.project_path).parent if session.project_path else None
        return EditorConnection(project_root, supervised, client=client, editor=editor, session=session,
                                app_name=self.app_name, repository_root=self.repository_root)

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

    @staticmethod
    def open_project(connection: EditorConnection, project_file: Path, recover: bool) -> None:
        """project.open {path, recover} on the launcher-state editor just spawned, transcribed like every call (the
        transcript of the project starts or continues with it); records the editor's recoveryAvailable and recovered on
        the connection (EditorConnection.recovery). A failure (a locked or invalid project; a stale or corrupt recovery
        with recover: Conflict, Parse or Validation) shuts the editor down again and is raised as RuntimeError."""
        response = connection.call("project.open", {"path": str(project_file), "recover": recover})
        if "error" in response:
            error = engine_client.error_from_response("project.open", response)
            connection.shutdown(force=True)
            raise RuntimeError(f"project.open failed: {error.detail} ({error.error_code})")
        result = response.get("result", {})
        connection.recovery = {"recoveryAvailable": result.get("recoveryAvailable") is True,
                               "recovered": result.get("recovered") is True}
