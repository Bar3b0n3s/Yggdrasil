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

Contract stubs (M4 stream D).
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

from engine_mcp.connection import EditorConnection

ALREADY_OPEN_HINT = "enable Allow AI automation in the editor's Automation panel, then call editor_attach"


@dataclasses.dataclass
class EditorAlreadyOpen(Exception):
    """An editor holds the project lock without a listening automation server."""

    pid: int

    def to_json(self) -> dict[str, Any]:
        """{"error": "EditorAlreadyOpen", "pid": pid, "hint": ALREADY_OPEN_HINT}."""
        raise NotImplementedError("contract stub: EditorAlreadyOpen.to_json (M4 stream D)")


@dataclasses.dataclass(frozen=True)
class LaunchRequest:
    """editor_launch's arguments."""

    project: Path
    create: bool = False
    template: str = "Empty"
    headless: bool = True
    renderer: str = "none"


class Launcher:
    """Finds, starts and attaches to editors for one bridge process."""

    def __init__(self, repository_root: Path, user_data_root: Path | None = None) -> None:
        self.repository_root = repository_root
        self.user_data_root = user_data_root

    def launch(self, request: LaunchRequest) -> EditorConnection:
        """editor_launch (see the module comment). Raises EditorAlreadyOpen, FileNotFoundError for a missing binary, or
        RuntimeError when the spawned editor exits before it listens (with its exit code: 3 for a locked or invalid
        project)."""
        raise NotImplementedError("contract stub: Launcher.launch (M4 stream D)")

    def attach(self, project: Path | None = None, pid: int | None = None) -> EditorConnection:
        """editor_attach: the live session of `project` or of `pid` (exactly one, ValueError otherwise). Raises
        LookupError when no such session exists."""
        raise NotImplementedError("contract stub: Launcher.attach (M4 stream D)")
