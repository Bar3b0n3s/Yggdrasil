"""The always-on transcript (Docs/Architecture.md §13.8 "Transcript", §13.12): every request the bridge sends and a
summary of its response, appended as JSONL to <ProjectDir>/Automation/BuildLog.jsonl of the project it launched or
attached to. Nothing depends on remembering an environment variable; ENGINE_MCP_TRANSCRIPT=<path> only redirects the
file (Python tests keep fixtures clean with it).

Line format (shared with EditorCore/Automation/ProvenanceRecorder.h, Docs/Decisions/0008-m4-decisions.md decision 11),
camelCase, one object per line, appended and never rewritten:
  {"type": "request", "time": "<UTC ISO 8601>", "client": "<name>", "id": <id>, "method": "<method>", "params": {...}}
  {"type": "response", "time": "...", "client": "<name>", "id": <id>, "requestLine": <line of the request>, "ok": true,
   "summary": "<one line>"}          (or "ok": false with "error": {"code", "errorCode", "detail"})
The 1-based line number of each request line is passed to the editor as params._meta.transcriptLine, which provenance
stores. Session-level methods (session.hello) are not written: they carry the token.

Contract stubs (M4 stream D).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

TRANSCRIPT_RELATIVE_PATH = Path("Automation") / "BuildLog.jsonl"
TRANSCRIPT_ENVIRONMENT_VARIABLE = "ENGINE_MCP_TRANSCRIPT"
CLIENT_NAME = "engine-mcp"
# Methods whose params must never reach a file.
UNRECORDED_METHODS = frozenset({"session.hello"})


def transcript_path(project_root: Path) -> Path:
    """$ENGINE_MCP_TRANSCRIPT when set, else <project_root>/Automation/BuildLog.jsonl."""
    raise NotImplementedError("contract stub: transcript.transcript_path (M4 stream D)")


class Transcript:
    """Appends to one transcript file. Counts its existing lines when opened, so line numbers continue across bridge
    restarts; a file whose last line lacks its newline (a torn write) is completed with one before appending."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.line_count = 0

    @classmethod
    def open(cls, project_root: Path) -> Transcript:
        """The transcript of `project_root` (transcript_path), with Automation/ created."""
        raise NotImplementedError("contract stub: Transcript.open (M4 stream D)")

    def append_request(self, request_id: int | str, method: str, params: dict[str, Any]) -> int:
        """Appends a request line (flushed before returning) and returns its 1-based line number."""
        raise NotImplementedError("contract stub: Transcript.append_request (M4 stream D)")

    def append_response(self, request_id: int | str, request_line: int, response: dict[str, Any]) -> None:
        """Appends the response line of the request at `request_line`, with summarize(response)."""
        raise NotImplementedError("contract stub: Transcript.append_response (M4 stream D)")


def summarize(response: dict[str, Any]) -> str:
    """One line for a response: the error's code and detail, or the result's notable members (ids, names, counts) and
    its _meta delta (revision, dirty, new errors and warnings), at most 200 characters."""
    raise NotImplementedError("contract stub: transcript.summarize (M4 stream D)")
