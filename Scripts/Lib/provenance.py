"""The provenance audit of the demo projects (Docs/Architecture.md §13.12, §13.4, §13.8, §2.3; Roadmap M16;
Docs/Decisions/0026-m16-contract.md decision 4, stream A), shared by Scripts/AuditProvenance.py and the games suite of
Scripts/Test.py.

A project passes when every engine-consumed file was written by the editor through automation, as the two committed
files prove; nothing under the gitignored Library/ is ever read:
  - Automation/Provenance.json (ProvenanceRecorder.h; ADR 0008 decision 10): {"Format": "Provenance", "Version": 1,
    "Entries": [{"Path", "XXH64", "Method", "RequestId", "Client", "TranscriptLine"}]}, PascalCase keys as the editor
    writes them (Architecture §13.4 and §13.12 spell them in lowercase until the lead amends them), entries sorted by
    the UTF-8 bytes of Path and unique;
  - Automation/BuildLog.jsonl (TranscriptLog.h, Tools/MCP/engine_mcp/transcript.py; ADR 0008 decision 11): one JSON
    object per line, 1-based, appended and never rewritten: request lines {"type": "request", "time", "client", "id",
    "method", "params"} and response lines {"type": "response", "time", "client", "id", "requestLine", "ok", "summary",
    "error"?}.

Engine-consumed files (enumerate_engine_files): the project root's *.eproj and .luaurc, and every file under Assets/
(.meta files included). AGENTS.md, README.md, .gitignore and Automation/** are exempt, and nothing else is audited.

File sources (FILE_MODES):
  - "index" (the acceptance mode, "from a fresh clone", and the default): the project's staged blobs, materialized into
    a temporary directory by materialize_index (git ls-files -z piped to git checkout-index -z --stdin -f --prefix),
    with .gitattributes' conversions applied as a checkout applies them; untracked and ignored files are absent, so a
    file that exists only in the working tree is not audited and a committed backup (*.bak) is;
  - "disk": the working tree as it is, *.bak files left out (the ignore rules keep them out of a commit); Library/ and
    Build/ hold no engine-consumed file by definition. The static checks and the self-test's fixtures use it.

Rules, for every engine-consumed file and every entry (each failure is a Finding with a stable code, FINDING_CODES):
  - Rule 1: the file has an entry ("entry-missing" otherwise), whose XXH64 (compared case-insensitively) is the
    seed-zero XXH64 of the file's bytes ("hash-mismatch" otherwise, naming both hashes and the entry's method and line).
  - Rule 2: the entry was written by an automation method: Method and Client are not "ui" ("written-by-ui"); its
    TranscriptLine names a line of BuildLog.jsonl ("transcript-line-missing" for a null line or one past the last line)
    that is a request line ("transcript-mismatch" otherwise) with the entry's RequestId (compared type-strictly: a
    boolean never equals an integer, an integer never equals a string), Method and Client ("transcript-mismatch" naming
    the member that differs). The one exception is §13.12's: an entry with Method "project.upgrade", Client "cli" and a
    null TranscriptLine passes when the transcript holds a project.upgrade request line of client "cli" (the editor
    records the line since M4, so only a legacy entry has none). The response line is not required: a rolled-back
    edit.batch writes files under a request whose response is not ok, and a killed bridge leaves no response.
  - Rule 3: no entry names a file that is absent from the audited files ("entry-dangling"), compared as exact
    case-sensitive strings, never by Path.exists, which ignores case on Windows and macOS; unless a request line after
    the entry's own line is an asset.delete or asset.move (top level or an edit.batch op) whose response is ok and whose
    "asset" or "path" param names the file, its source (the path without ".meta") or a directory above it. The editor
    forgets an entry when it deletes or moves the file, so this is the fallback the rule states, matched by path only.
  - The project and its two files: "project-missing" (the directory, or its root *.eproj, does not exist; in index mode,
    is not staged),
    "provenance-missing", "provenance-invalid" (load_provenance refused the document; no rule runs then),
    "transcript-missing", "transcript-invalid" (a line that is a JSON object but neither a well-formed request nor a
    well-formed response line, or a session.hello request line, which carries the session token and must never be
    committed).
Warnings (WARNING_CODES) never fail the audit:
  - "crlf": an engine-consumed text file (no NUL byte in its first 8,000 bytes) holds CR LF, which a fresh clone keeps
    only where .gitattributes marks the file -text;
  - "torn-line": a line that is not a JSON object: an unterminated last line, or a torn line the bridge completed with
    "\\n" (transcript.py). It becomes a finding with the same code when an entry's TranscriptLine names it.
MISSING_FILE_CODES are the findings that make AuditProvenance.py exit with 3 (a required file is missing).
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

PROVENANCE_FILE = "Automation/Provenance.json"
TRANSCRIPT_FILE = "Automation/BuildLog.jsonl"
PROVENANCE_FORMAT = "Provenance"
PROVENANCE_VERSION = 1
# The project files the audit never reads as engine-consumed (Architecture §13.12: documentation is exempt).
EXEMPT_FILES = ("AGENTS.md", "README.md")
FILE_MODES = ("index", "disk")
DEFAULT_FILE_MODE = "index"
# Rule order; a finding's code is one of these.
FINDING_CODES = (
    "project-missing",
    "provenance-missing",
    "provenance-invalid",
    "transcript-missing",
    "transcript-invalid",
    "entry-missing",
    "hash-mismatch",
    "written-by-ui",
    "transcript-line-missing",
    "transcript-mismatch",
    "torn-line",
    "entry-dangling",
)
WARNING_CODES = ("crlf", "torn-line")
MISSING_FILE_CODES = ("project-missing", "provenance-missing", "transcript-missing")
# AuditProvenance.py's exit codes for one project (Lib/report.py's conventions).
EXIT_PASSED = 0
EXIT_FINDINGS = 1
EXIT_MISSING_FILE = 3


@dataclasses.dataclass(frozen=True)
class ProvenanceEntry:
    """One entry of Automation/Provenance.json, in the editor's spelling (ProvenanceRecorder.h)."""

    path: str  # "Path": project-relative, "/" separators
    xxh64: str  # "XXH64": 16 hexadecimal digits as recorded
    method: str  # "Method": the automation method, "ui" for the windowed UI, "project.upgrade" for --upgrade
    request_id: int | str | None  # "RequestId": the JSON-RPC id as sent (never a boolean)
    client: str  # "Client": session.hello's client.name, "cli", "batch", "test" or "ui"
    transcript_line: int | None  # "TranscriptLine": the request's 1-based line in BuildLog.jsonl, or None


@dataclasses.dataclass(frozen=True)
class TranscriptLine:
    """One line of Automation/BuildLog.jsonl."""

    number: int  # 1-based
    raw: bytes  # the line's bytes without its b"\n"
    record: dict[str, Any] | None  # the line as a JSON object; None for a torn line (not a JSON object)
    terminated: bool  # False only for a last line without its b"\n"


@dataclasses.dataclass(frozen=True)
class Transcript:
    """Every line of one transcript, in order: lines[n - 1] is line n."""

    lines: tuple[TranscriptLine, ...]


@dataclasses.dataclass(frozen=True)
class Finding:
    """One failure or warning of an audit."""

    code: str  # one of FINDING_CODES, or WARNING_CODES for a warning
    path: str  # the project-relative file it concerns ("Assets/...", PROVENANCE_FILE, TRANSCRIPT_FILE; "": the project)
    detail: str  # one sentence: what is wrong, the transcript line where one applies, and how to repair it


@dataclasses.dataclass
class ProjectAudit:
    """The audit of one project. It passes when `findings` is empty; `warnings` never fail it."""

    project: Path  # the audited project directory, absolute (the working tree's, also in index mode)
    files: str  # the file source of FILE_MODES
    counts: dict[str, int]  # "files" (engine-consumed), "entries", "transcriptLines", "requests"
    findings: list[Finding]  # in FINDING_CODES order, then by path
    warnings: list[Finding]  # in WARNING_CODES order, then by path


def load_provenance(path: Path) -> list[ProvenanceEntry]:
    """The entries of the Provenance.json at `path`, in file order. Raises FileNotFoundError when it does not exist, and
    ValueError naming the file and a JSON pointer for anything ProvenanceRecorder::FromText refuses, read as strictly:
    invalid UTF-8 or JSON, a duplicate key, NaN or Infinity, a wrong Format or Version, a missing or unknown member, a
    mistyped member (a boolean RequestId included), an empty Path, an XXH64 that is not 16 hexadecimal digits, a
    TranscriptLine below 1, and paths that are not sorted by their UTF-8 bytes or not unique."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def load_transcript(path: Path) -> Transcript:
    """The lines of the BuildLog.jsonl at `path`, split on b"\\n" bytes only, never with str.splitlines: the bridge's
    json.dumps(ensure_ascii=False) may write U+2028, U+2029 or U+0085 raw inside a line, and they end no line. Line n is
    the n-th piece; a non-empty tail without a final b"\\n" is a last line with terminated False; an empty file has no
    line. Each line is parsed as strict JSON (duplicate keys, NaN and Infinity refused) into `record` when it is an
    object, else `record` is None (a torn line). Raises FileNotFoundError when the file does not exist."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def materialize_index(repository: Path, project: Path, destination: Path) -> Path:
    """Writes the staged blobs of every file below `project` (a directory inside the work tree of `repository`) into
    `destination`, as a fresh clone of the commit about to be made would hold them: `git -C <repository> ls-files -z --
    <project, repository-relative>` piped to `git -C <repository> checkout-index -z --stdin -f --prefix=<destination>/`,
    so .gitattributes' conversions are applied and untracked and ignored files are absent. Returns
    destination / <project, repository-relative>, which does not exist when no file of the project is staged. Raises
    ValueError when `project` is not inside `repository`, and RuntimeError naming the git command and its output when
    git is missing or fails."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def enumerate_engine_files(project_copy: Path, files: str = DEFAULT_FILE_MODE) -> list[str]:
    """The project-relative paths ("/" separators) of the engine-consumed files of `project_copy`, sorted by their UTF-8
    bytes: the root's *.eproj and .luaurc files and every file below Assets/. With `files` "disk" the *.bak files are
    left out; with "index" (`project_copy` is materialize_index's copy) every file counts, so a committed backup is
    audited. EXEMPT_FILES, .gitignore and Automation/** are never listed, and nothing below Library/ is ever read.
    Raises ValueError for a `files` value outside FILE_MODES."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def audit_project(project_dir: Path, files: str = DEFAULT_FILE_MODE) -> ProjectAudit:
    """Audits the project directory `project_dir` (its root holds the *.eproj) under the rules of the module docstring.
    With `files` "index" the repository is `git -C <project_dir> rev-parse --show-toplevel`, and the project's staged
    blobs are materialized into a temporary directory that is removed afterwards; every file is read from that copy,
    Automation/Provenance.json and Automation/BuildLog.jsonl included, so an unstaged change to them is ignored as an
    unstaged change to an asset is. The findings name project-relative paths either way. A project that is missing (in
    index mode: a directory that does not exist, checked before git runs, or one with no staged file or no staged root
    *.eproj), or whose Provenance.json or BuildLog.jsonl is missing (in the copy, in index mode), is a ProjectAudit with
    that finding (MISSING_FILE_CODES), not an exception. Raises ValueError for a `files` value outside FILE_MODES, and
    RuntimeError naming the git command and its output when git is missing or fails in index mode, a directory outside
    any git work tree included."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def audit_exit_code(audit: ProjectAudit) -> int:
    """EXIT_PASSED when `audit` has no finding, EXIT_MISSING_FILE when a finding's code is in MISSING_FILE_CODES, else
    EXIT_FINDINGS."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")


def audit_to_json(audit: ProjectAudit) -> dict[str, Any]:
    """The audit as AuditProvenance.py --json and the games suite's step data carry it: {"project": <repository-relative
    path, "/" separators, or the absolute path outside the repository>, "files": <file source>, "passed": <no finding>,
    "counts": {"files", "entries", "transcriptLines", "requests"}, "findings": [{"code", "path", "detail"}],
    "warnings": [{"code", "path", "detail"}]}."""
    raise NotImplementedError("contract stub: provenance audit (ADR 0026 decision 4, stream A)")
