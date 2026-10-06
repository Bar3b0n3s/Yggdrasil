"""The bridge's connection to one editor (Docs/Architecture.md §13.8): framing, handshake and calls through
Tools/Automation/engine_client.py, each call bounded by its method's timeoutSeconds from the catalogue (default 60),
every request written to the transcript first so its line number travels as params._meta.transcriptLine, and crash
detection: when the editor the bridge launched exits unexpectedly, the next call fails with EditorCrashed {exitCode,
lastLogLines (50), crashReportPath, autosaveAvailable}.

Contract stubs (M4 stream D).
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

from engine_mcp.transcript import Transcript

DEFAULT_TIMEOUT_SECONDS = 60.0
LAST_LOG_LINES = 50


@dataclasses.dataclass
class EditorCrashed(Exception):
    """The supervised editor died (§13.8 "Resilience")."""

    exit_code: int | None
    last_log_lines: list[str]
    crash_report_path: str
    autosave_available: bool

    def to_json(self) -> dict[str, Any]:
        """{"error": "EditorCrashed", "exitCode", "lastLogLines", "crashReportPath", "autosaveAvailable"}."""
        raise NotImplementedError("contract stub: EditorCrashed.to_json (M4 stream D)")


class EditorConnection:
    """One connected editor: launched (supervised) or attached (never killed, §13.8)."""

    def __init__(self, project_root: Path | None, supervised: bool) -> None:
        self.project_root = project_root
        self.supervised = supervised
        self.transcript: Transcript | None = None

    def call(self, method: str, params: dict[str, Any] | None,
             timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
        """Writes the request to the transcript, sends it with its transcript line, writes the response summary and
        returns the response message. Raises EditorCrashed when a supervised editor has died, ConnectionClosed
        otherwise."""
        raise NotImplementedError("contract stub: EditorConnection.call (M4 stream D)")

    def is_alive(self) -> bool:
        """Whether the connection is open and, for a supervised editor, its process runs."""
        raise NotImplementedError("contract stub: EditorConnection.is_alive (M4 stream D)")

    def disconnect(self) -> None:
        """Closes the connection; an attached editor keeps running."""
        raise NotImplementedError("contract stub: EditorConnection.disconnect (M4 stream D)")
