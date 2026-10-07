"""The always-on transcript (Docs/Architecture.md §13.8 "Transcript", §13.12): every request the bridge sends and a
summary of its response, appended as JSONL to <ProjectDir>/Automation/BuildLog.jsonl of the project it launched or
attached to. Nothing depends on remembering an environment variable; ENGINE_MCP_TRANSCRIPT=<path> only redirects the
file (Python tests keep fixtures clean with it).

Line format (shared with EditorCore/Automation/TranscriptLog.h, Docs/Decisions/0008-m4-decisions.md decision 11),
camelCase, one object per line, appended and never rewritten:
  {"type": "request", "time": "<UTC ISO 8601>", "client": "<name>", "id": <id>, "method": "<method>", "params": {...}}
  {"type": "response", "time": "...", "client": "<name>", "id": <id>, "requestLine": <line of the request>, "ok": true,
   "summary": "<one line>"}          (or "ok": false with "error": {"code", "errorCode", "detail"})
The 1-based line number of each request line is passed to the editor as params._meta.transcriptLine, which provenance
stores. Session-level methods (session.hello) are not written: they carry the token.

Line numbers stay right when several writers share the file (two bridges attached to one editor): every append takes an
exclusive OS lock on the file (the project lock's primitives: LockFileEx on one far byte on Windows, flock on POSIX),
counts the lines another writer added since, and only then writes.

The transcript of a project that is being created or opened (editor_launch {create: true}, project.create,
project.open) is pending until the editor confirms the project: its lines are kept in memory, numbered after the lines the
project's transcript already holds, and written once the directory holds a project file (.eproj). The bridge creates
nothing before that: project.create accepts only a new or empty directory, so an Automation/ folder written first would
make it fail, and a refused call must leave nothing behind in a directory that is not a project.
"""

from __future__ import annotations

import contextlib
import datetime
import json
import os
import sys
from pathlib import Path
from typing import Any, BinaryIO, Iterator

TRANSCRIPT_RELATIVE_PATH = Path("Automation") / "BuildLog.jsonl"
PROJECT_FILE_PATTERN = "*.eproj"
TRANSCRIPT_ENVIRONMENT_VARIABLE = "ENGINE_MCP_TRANSCRIPT"
CLIENT_NAME = "engine-mcp"
# Methods whose params must never reach a file.
UNRECORDED_METHODS = frozenset({"session.hello"})
MAX_SUMMARY_CHARACTERS = 200
# Result members a summary names first, when present.
_NOTABLE_MEMBERS = ("entity", "entities", "scene", "project", "undoIndex", "undone", "redone", "total", "changedFiles",
                    "savedFiles", "createdFiles", "fixed", "diagnostics", "results", "destroyed", "selection")


def transcript_path(project_root: Path) -> Path:
    """$ENGINE_MCP_TRANSCRIPT when set, else <project_root>/Automation/BuildLog.jsonl."""
    redirected = os.environ.get(TRANSCRIPT_ENVIRONMENT_VARIABLE, "")
    if redirected:
        return Path(redirected)
    return project_root / TRANSCRIPT_RELATIVE_PATH


class Transcript:
    """Appends to one transcript file. Counts its existing lines when opened, so line numbers continue across bridge
    restarts; a file whose last line lacks its newline (a torn write) is completed with one before appending."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.line_count = 0
        # Bytes of the file already counted in line_count.
        self.counted_size = 0
        # The project directory whose project file must exist before the file is written (None: write at once), and the
        # lines kept until then.
        self.pending_root: Path | None = None
        self.pending_lines: list[bytes] = []

    @classmethod
    def open(cls, project_root: Path, pending: bool = False) -> Transcript:
        """The transcript of `project_root` (transcript_path), with Automation/ created. With `pending` (a project being
        created or opened), or for a directory that does not exist, nothing is created: lines are kept in memory until
        flush_pending finds the project file (see the module comment). A redirected transcript is never pending."""
        transcript = cls(transcript_path(project_root))
        redirected = bool(os.environ.get(TRANSCRIPT_ENVIRONMENT_VARIABLE, ""))
        if not redirected and (pending or not project_root.is_dir()):
            transcript.pending_root = project_root
            transcript.line_count = _count_lines(transcript.path)
            return transcript
        transcript.path.parent.mkdir(parents=True, exist_ok=True)
        with transcript.locked() as file:
            transcript.catch_up(file)
        return transcript

    def append_request(self, request_id: int | str, method: str, params: dict[str, Any]) -> int:
        """Appends a request line (flushed before returning) and returns its 1-based line number."""
        if method in UNRECORDED_METHODS:
            raise ValueError(f"{method} carries the session token and is never written to the transcript")
        return self.append({"type": "request", "time": utc_timestamp(), "client": CLIENT_NAME, "id": request_id,
                            "method": method, "params": params})

    def append_response(self, request_id: int | str, request_line: int, response: dict[str, Any]) -> None:
        """Appends the response line of the request at `request_line`, with summarize(response)."""
        line: dict[str, Any] = {"type": "response", "time": utc_timestamp(), "client": CLIENT_NAME, "id": request_id,
                                "requestLine": request_line, "ok": "error" not in response,
                                "summary": summarize(response)}
        error = response.get("error")
        if isinstance(error, dict):
            data = error.get("data") if isinstance(error.get("data"), dict) else {}
            line["error"] = {"code": error.get("code"), "errorCode": data.get("errorCode", ""),
                             "detail": data.get("detail", error.get("message", ""))}
        self.append(line)

    def append(self, line: dict[str, Any]) -> int:
        """Appends one JSON line and returns its 1-based number."""
        encoded = json.dumps(line, ensure_ascii=False).encode("utf-8") + b"\n"
        if self.pending_root is not None:
            self.pending_lines.append(encoded)
            self.line_count += 1
            return self.line_count
        with self.locked() as file:
            self.catch_up(file)
            file.seek(0, os.SEEK_END)
            file.write(encoded)
            file.flush()
            os.fsync(file.fileno())
            self.counted_size += len(encoded)
            self.line_count += 1
            return self.line_count

    @property
    def is_pending(self) -> bool:
        """Whether lines are kept in memory until the project exists."""
        return self.pending_root is not None

    def flush_pending(self) -> bool:
        """Writes the kept lines once the pending project directory holds a project file, and from then on writes at once;
        returns whether the transcript is written to its file now. The kept lines were numbered after the lines the file
        held when the transcript was opened; a file that meanwhile got lines (the editor writes no transcript, so only
        another bridge could) keeps them, and the kept lines follow."""
        if self.pending_root is None:
            return True
        if not _holds_project_file(self.pending_root):
            return False
        self.pending_root = None
        lines, self.pending_lines = self.pending_lines, []
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.locked() as file:
            self.counted_size = 0
            self.line_count = 0
            self.catch_up(file)
            file.seek(0, os.SEEK_END)
            for encoded in lines:
                file.write(encoded)
            file.flush()
            os.fsync(file.fileno())
            self.counted_size += sum(len(encoded) for encoded in lines)
            self.line_count += len(lines)
        return True

    def discard_pending(self) -> None:
        """Drops the kept lines of a project that was never confirmed (a refused project.create or project.open): nothing
        is written to a directory that is not a project."""
        self.pending_lines = []

    def catch_up(self, file: BinaryIO) -> None:
        """Counts the lines written since the last count (by this or another writer) and completes a torn last line."""
        file.seek(0, os.SEEK_END)
        size = file.tell()
        if size < self.counted_size:  # replaced by a shorter file: count it again
            self.counted_size = 0
            self.line_count = 0
        if size == self.counted_size:
            return
        file.seek(self.counted_size)
        added = file.read(size - self.counted_size)
        self.line_count += added.count(b"\n")
        if not added.endswith(b"\n"):
            file.seek(0, os.SEEK_END)
            file.write(b"\n")
            file.flush()
            self.line_count += 1
            size += 1
        self.counted_size = size

    @contextlib.contextmanager
    def locked(self) -> Iterator[BinaryIO]:
        """The file opened for reading and appending under an exclusive OS lock."""
        with self.path.open("a+b") as file:
            _lock(file)
            try:
                yield file
            finally:
                _unlock(file)


def _count_lines(path: Path) -> int:
    """The lines of an existing transcript as catch_up counts them (a torn last line counts as complete), without creating
    or locking anything; 0 when there is no file."""
    try:
        data = path.read_bytes()
    except OSError:
        return 0
    return data.count(b"\n") + (0 if not data or data.endswith(b"\n") else 1)


def _holds_project_file(root: Path) -> bool:
    """Whether `root` is a directory holding a project file."""
    try:
        return root.is_dir() and any(path.is_file() for path in root.glob(PROJECT_FILE_PATTERN))
    except OSError:
        return False


def summarize(response: dict[str, Any]) -> str:
    """One line for a response: the error's code and detail, or the result's notable members (ids, names, counts) and
    its _meta delta (revision, dirty, new errors and warnings), at most 200 characters."""
    error = response.get("error")
    if isinstance(error, dict):
        data = error.get("data") if isinstance(error.get("data"), dict) else {}
        detail = data.get("detail", error.get("message", ""))
        parts = [f"error {error.get('code')} {data.get('errorCode', '')}: {detail}"]
        if "failedOp" in data:
            parts.append(f"failedOp {data['failedOp']}")
        return _clip("; ".join(parts))
    result = response.get("result")
    if not isinstance(result, dict):
        return _clip(f"result {json.dumps(result)}")
    parts = []
    if result.get("truncated") is True and "path" in result:
        parts.append(f"offloaded to {result['path']}")
    if result.get("dryRun") is True:
        parts.append("dry run")
    keys = [key for key in _NOTABLE_MEMBERS if key in result]
    keys += [key for key in result if key not in keys and key not in ("_meta", "dryRun", "truncated", "path")]
    for key in keys:
        described = _describe_member(key, result[key])
        if described:
            parts.append(described)
    meta = result.get("_meta")
    if isinstance(meta, dict):
        parts.append(_describe_meta(meta))
    return _clip("; ".join(part for part in parts if part) or "ok")


def _describe_member(key: str, value: Any) -> str:
    if isinstance(value, dict):
        identity = [str(value[name]) for name in ("id", "name", "path") if isinstance(value.get(name), (str, int))]
        return f"{key} {' '.join(identity)}" if identity else ""
    if isinstance(value, list):
        ids = [str(item["id"]) for item in value[:3] if isinstance(item, dict) and isinstance(item.get("id"), str)]
        return f"{key} {len(value)}" + (f" ({', '.join(ids)}{', ...' if len(value) > 3 else ''})" if ids else "")
    if isinstance(value, bool):
        return f"{key} {'yes' if value else 'no'}"
    if isinstance(value, (int, float)):
        return f"{key} {value}"
    if isinstance(value, str) and value and len(value) <= 40 and "\n" not in value:
        return f"{key} {value}"
    return ""


def _describe_meta(meta: dict[str, Any]) -> str:
    parts = []
    if "revision" in meta:
        parts.append(f"rev {meta['revision']}")
    if meta.get("dirty"):
        parts.append("dirty")
    diagnostics = meta.get("diagnostics")
    if isinstance(diagnostics, dict):
        for key, label in (("newErrors", "errors"), ("newWarnings", "warnings"), ("newScriptErrors", "script errors")):
            if diagnostics.get(key):
                parts.append(f"+{diagnostics[key]} {label}")
    return " ".join(parts)


def _clip(text: str) -> str:
    text = " ".join(text.split())
    return text if len(text) <= MAX_SUMMARY_CHARACTERS else text[:MAX_SUMMARY_CHARACTERS - 3] + "..."


def utc_timestamp() -> str:
    """The current UTC time as ISO 8601 with milliseconds ("2026-10-06T12:34:56.789Z")."""
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def _lock(file: BinaryIO) -> None:
    if sys.platform == "win32":
        import msvcrt

        import engine_client

        engine_client.windows_lock_byte(msvcrt.get_osfhandle(file.fileno()), exclusive=True, wait=True)
    else:
        import fcntl

        fcntl.flock(file.fileno(), fcntl.LOCK_EX)


def _unlock(file: BinaryIO) -> None:
    if sys.platform == "win32":
        import msvcrt

        import engine_client

        engine_client.windows_unlock_byte(msvcrt.get_osfhandle(file.fileno()))
    else:
        import fcntl

        fcntl.flock(file.fileno(), fcntl.LOCK_UN)
