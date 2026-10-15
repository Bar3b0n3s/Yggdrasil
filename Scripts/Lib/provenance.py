"""The provenance audit of the demo projects (Docs/Architecture.md §13.12, §13.4, §13.8, §2.3; Roadmap M16;
Docs/Decisions/0026-m16-contract.md decision 4, stream A), shared by Scripts/AuditProvenance.py and the games suite of
Scripts/Test.py.

A project passes when every engine-consumed file was written by the editor through automation, as the two committed
files prove; nothing under the gitignored Library/ is ever read:
  - Automation/Provenance.json (ProvenanceRecorder.h; ADR 0008 decision 10): {"Format": "Provenance", "Version": 1,
    "Entries": [{"Path", "XXH64", "Method", "RequestId", "Client", "TranscriptLine"}]}, PascalCase keys as the editor
    writes them (Architecture §13.4 and §13.12), entries sorted by the UTF-8 bytes of Path and unique;
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
    "asset" param names the file or its source (the path without ".meta"). The editor forgets an entry when it deletes
    or moves the file, so this is the fallback the rule states, matched by path only. A directory never excuses an entry
    below it: asset.delete removes only a folder that holds no files, so an entry below a deleted folder survives only
    when something else removed its file. Nor does asset.move's destination ("path"): a move removes only its source.
  - The project and its two files: "project-missing" (the directory, or its root *.eproj, does not exist; in index mode,
    is not staged), "provenance-missing", "provenance-invalid" (load_provenance refused the document),
    "transcript-missing" (the rules run only when both files were read), "transcript-invalid" (a line that is a JSON
    object but neither a well-formed request nor a well-formed response line, or a session.hello request line, which
    carries the session token and must never be committed).
  - In index mode only (INDEX_ONLY_CODES): "path-collision", for each staged file of the project whose path equals
    another staged path once both are NFC-normalized and case-folded. A case-insensitive file system (Windows, macOS)
    holds one file for all of them, so a clone there, and the materialized copy the rules read on such a machine, loses
    the others; the finding is reported on every platform, so the verdict never depends on the machine.
Warnings (WARNING_CODES) never fail the audit:
  - "crlf": an engine-consumed text file (no NUL byte in its first 8,000 bytes) holds CR LF, which a fresh clone keeps
    only where .gitattributes marks the file -text;
  - "torn-line": a line that is not a JSON object: an unterminated last line, or a torn line the bridge completed with
    "\\n" (transcript.py). It becomes a finding with the same code when an entry's TranscriptLine names it.
MISSING_FILE_CODES are the findings that make AuditProvenance.py exit with 3 (a required file is missing).
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import subprocess
import tempfile
import unicodedata
from collections.abc import Callable
from pathlib import Path
from typing import Any

from . import paths, xxh64

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
    "path-collision",
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
# The finding codes only index mode reports, so no seeded fixture (audited in disk mode) can hold them;
# Tests/Automation/test_audit_provenance.py builds them in a temporary repository instead.
INDEX_ONLY_CODES = ("path-collision",)
# AuditProvenance.py's exit codes for one project (Lib/report.py's conventions).
EXIT_PASSED = 0
EXIT_FINDINGS = 1
EXIT_MISSING_FILE = 3

# The members of a provenance document and of each of its entries (ProvenanceRecorder.cpp), in the editor's order.
_ROOT_MEMBERS = ("Format", "Version", "Entries")
_ENTRY_MEMBERS = ("Path", "XXH64", "Method", "RequestId", "Client", "TranscriptLine")
_HASH_PATTERN = re.compile(r"[0-9A-Fa-f]{16}")
# The members of a transcript line (ADR 0008 decision 11): a request's, a response's, and the response's optional one.
_REQUEST_MEMBERS = ("type", "time", "client", "id", "method", "params")
_RESPONSE_MEMBERS = ("type", "time", "client", "id", "requestLine", "ok", "summary")
_RESPONSE_OPTIONAL_MEMBERS = ("error",)
# Request methods whose params carry the session token, which must never reach a committed file (transcript.py).
_TOKEN_METHODS = frozenset({"session.hello"})
# The methods that remove a file from where provenance recorded it (rule 3's fallback), and the param naming it: the
# asset deleted, or the source a move takes away (never its destination, "path").
_REMOVING_METHODS = frozenset({"asset.delete", "asset.move"})
_REMOVED_PATH_PARAM = "asset"
_BATCH_METHOD = "edit.batch"
_UPGRADE_METHOD = "project.upgrade"
_CLI_CLIENT = "cli"
_UI = "ui"
_ASSETS_DIRECTORY = "Assets"
_LUAURC = ".luaurc"
_PROJECT_EXTENSION = ".eproj"
_BACKUP_EXTENSION = ".bak"
_META_EXTENSION = ".meta"
_PROJECT_SCHEME = "project://"
# A file is text when its first bytes hold no NUL (git's own heuristic looks at the first 8,000 bytes).
_TEXT_PROBE_BYTES = 8000
# Every git command the audit runs gets this long; a hung git is a failure, never a pass.
_GIT_TIMEOUT_SECONDS = 300.0
# The configuration materialize_index's git commands run with. Command-line values override the system, user and
# repository configuration, so the copy holds the staged blobs with only .gitattributes' conversions whatever the
# machine: no conversion of core.autocrlf (Git for Windows' system configuration sets it to true), LF for a file whose
# attributes say text without naming an eol (core.eol's default, native, is CR LF on Windows), no user-wide attributes
# file (core.attributesFile), and a symbolic link written as a file holding its target, which is the blob's content.
# _CHECKOUT_ENVIRONMENT adds GIT_ATTR_NOSYSTEM, so the system-wide $(prefix)/etc/gitattributes (Git for Windows ships
# one) cannot convert the copy either.
_CHECKOUT_CONFIGURATION = ("-c", "core.autocrlf=false", "-c", "core.eol=lf", "-c", f"core.attributesFile={os.devnull}",
                           "-c", "core.symlinks=false")
_CHECKOUT_ENVIRONMENT = {"GIT_ATTR_NOSYSTEM": "1"}
# The kinds of a transcript line (_classify_line).
_REQUEST = "request"
_RESPONSE = "response"
_TORN = "torn"
_INVALID = "invalid"
# ProjectAudit.counts' keys, in the --json document's order.
_COUNT_KEYS = ("files", "entries", "transcriptLines", "requests")


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


@dataclasses.dataclass(frozen=True)
class _Request:
    """A well-formed request line of a transcript."""

    number: int
    record: dict[str, Any]
    succeeded: bool  # a response line names it (its requestLine, with the same id and client) with "ok" true


def load_provenance(path: Path) -> list[ProvenanceEntry]:
    """The entries of the Provenance.json at `path`, in file order. Raises FileNotFoundError when it does not exist, and
    ValueError naming the file and a JSON pointer for anything ProvenanceRecorder::FromText refuses, read as strictly:
    invalid UTF-8 or JSON, a duplicate key, NaN or Infinity, a wrong Format or Version, a missing or unknown member, a
    mistyped member (a boolean RequestId included), an empty Path, an XXH64 that is not 16 hexadecimal digits, a
    TranscriptLine below 1, and paths that are not sorted by their UTF-8 bytes or not unique."""
    data = path.read_bytes()
    try:
        document = _parse_strict_json(data.decode("utf-8"))
    except ValueError as error:
        raise ValueError(f"{path}: not strict UTF-8 JSON: {error}") from None

    def refuse(pointer: str, message: str) -> ValueError:
        return ValueError(f"{path}: {message} (JSON pointer \"{pointer}\")")

    if not isinstance(document, dict):
        raise refuse("", "the document is not a JSON object")
    _check_members(document, _ROOT_MEMBERS, "", refuse)
    if document["Format"] != PROVENANCE_FORMAT:
        raise refuse("/Format", f"\"Format\" must be \"{PROVENANCE_FORMAT}\", not {_describe(document['Format'])}")
    if type(document["Version"]) is not int or document["Version"] != PROVENANCE_VERSION:
        raise refuse("/Version", f"\"Version\" must be {PROVENANCE_VERSION}, not {_describe(document['Version'])}")
    items = document["Entries"]
    if not isinstance(items, list):
        raise refuse("/Entries", "\"Entries\" must be an array")
    entries: list[ProvenanceEntry] = []
    previous: bytes | None = None
    for index, item in enumerate(items):
        pointer = f"/Entries/{index}"
        if not isinstance(item, dict):
            raise refuse(pointer, "an entry must be a JSON object")
        _check_members(item, _ENTRY_MEMBERS, pointer, refuse)
        entry_path, hash_text = item["Path"], item["XXH64"]
        method, request_id, client, line = item["Method"], item["RequestId"], item["Client"], item["TranscriptLine"]
        if not isinstance(entry_path, str) or not entry_path:
            raise refuse(f"{pointer}/Path", "\"Path\" must be a non-empty string")
        if not isinstance(hash_text, str) or not _HASH_PATTERN.fullmatch(hash_text):
            raise refuse(f"{pointer}/XXH64", f"\"XXH64\" must be 16 hexadecimal digits, not {_describe(hash_text)}")
        if not isinstance(method, str):
            raise refuse(f"{pointer}/Method", "\"Method\" must be a string")
        if request_id is not None and not _is_request_id(request_id):
            raise refuse(f"{pointer}/RequestId",
                         f"\"RequestId\" must be an integer, a string or null, not {_describe(request_id)}")
        if not isinstance(client, str):
            raise refuse(f"{pointer}/Client", "\"Client\" must be a string")
        if line is not None and (type(line) is not int or line < 1):
            raise refuse(f"{pointer}/TranscriptLine",
                         f"\"TranscriptLine\" must be a 1-based line number or null, not {_describe(line)}")
        encoded = entry_path.encode("utf-8")
        if previous is not None and not previous < encoded:
            raise refuse(f"{pointer}/Path", f"entries must be sorted by path without duplicates: \"{entry_path}\" "
                                            f"follows \"{previous.decode('utf-8')}\"")
        previous = encoded
        entries.append(ProvenanceEntry(entry_path, hash_text, method, request_id, client, line))
    return entries


def load_transcript(path: Path) -> Transcript:
    """The lines of the BuildLog.jsonl at `path`, split on b"\\n" bytes only, never with str.splitlines: the bridge's
    json.dumps(ensure_ascii=False) may write U+2028, U+2029 or U+0085 raw inside a line, and they end no line. Line n is
    the n-th piece; a non-empty tail without a final b"\\n" is a last line with terminated False; an empty file has no
    line. Each line is parsed as strict JSON (duplicate keys, NaN and Infinity refused) into `record` when it is an
    object, else `record` is None (a torn line). Raises FileNotFoundError when the file does not exist."""
    data = path.read_bytes()
    if not data:
        return Transcript(())
    pieces = data.split(b"\n")
    complete = pieces[-1] == b""
    if complete:
        pieces.pop()
    last = len(pieces) - 1
    return Transcript(tuple(TranscriptLine(index + 1, raw, _parse_line(raw), complete or index < last)
                            for index, raw in enumerate(pieces)))


def materialize_index(repository: Path, project: Path, destination: Path) -> Path:
    """Writes the staged blobs of every file below `project` (a directory inside the work tree of `repository`) into
    `destination`, as a fresh clone of the commit about to be made would hold them: `git -C <repository> ls-files -z --
    <project, repository-relative>` piped to `git -C <repository> checkout-index -z --stdin -f --prefix=<destination>/`,
    so .gitattributes' conversions are applied and untracked and ignored files are absent. Both commands run with
    _CHECKOUT_CONFIGURATION and _CHECKOUT_ENVIRONMENT, so the machine's git configuration (core.autocrlf, core.eol,
    core.attributesFile, core.symlinks, the system-wide gitattributes file) changes no byte of the copy: it holds the
    staged blobs with only the staged .gitattributes' conversions (and the repository's own $GIT_DIR/info/attributes,
    which a fresh clone does not have). Returns destination / <project, repository-relative>, which does not exist when
    no file of the project is staged. Raises ValueError when `project` is not inside `repository`, and RuntimeError
    naming the git command and its output when git is missing or fails."""
    return _materialize_index(repository, project, destination)[0]


def _materialize_index(repository: Path, project: Path, destination: Path) -> tuple[Path, list[str]]:
    """materialize_index's copy and the project-relative paths ("/" separators) of the staged files it was written
    from, as git ls-files listed them (path-collision compares them; the copy may hold fewer files on a case-insensitive
    file system)."""
    relative = _relative_inside(project, repository)
    listed = _git(["-C", str(repository), *_CHECKOUT_CONFIGURATION, "--literal-pathspecs", "ls-files", "-z", "--",
                   relative or "."], environment=_CHECKOUT_ENVIRONMENT)
    destination.mkdir(parents=True, exist_ok=True)
    prefix = destination.resolve().as_posix().rstrip("/") + "/"
    _git(["-C", str(repository), *_CHECKOUT_CONFIGURATION, "checkout-index", "-z", "--stdin", "-f",
          f"--prefix={prefix}"], listed, environment=_CHECKOUT_ENVIRONMENT)
    base = f"{relative}/" if relative else ""
    staged = [entry.removeprefix(base) for entry in listed.decode("utf-8", errors="surrogateescape").split("\0")
              if entry]
    return (destination / relative if relative else destination), staged


def enumerate_engine_files(project_copy: Path, files: str = DEFAULT_FILE_MODE) -> list[str]:
    """The project-relative paths ("/" separators) of the engine-consumed files of `project_copy`, sorted by their UTF-8
    bytes: the root's *.eproj and .luaurc files and every file below Assets/. With `files` "disk" the *.bak files are
    left out; with "index" (`project_copy` is materialize_index's copy) every file counts, so a committed backup is
    audited. EXEMPT_FILES, .gitignore and Automation/** are never listed, and nothing below Library/ is ever read.
    Raises ValueError for a `files` value outside FILE_MODES, and OSError when a directory cannot be listed."""
    _check_file_mode(files)
    found = [entry.name for entry in project_copy.iterdir() if entry.is_file() and _is_root_engine_file(entry.name)]
    assets = project_copy / _ASSETS_DIRECTORY
    if assets.is_dir():
        for directory, subdirectories, names in os.walk(assets, onerror=_raise_walk_error):
            subdirectories.sort()
            for name in names:
                path = Path(directory) / name
                if not path.is_file() or (files == "disk" and name.endswith(_BACKUP_EXTENSION)):
                    continue
                found.append(path.relative_to(project_copy).as_posix())
    return sorted(found, key=_utf8_key)


def audit_project(project_dir: Path, files: str = DEFAULT_FILE_MODE) -> ProjectAudit:
    """Audits the project directory `project_dir` (its root holds the *.eproj) under the rules of the module docstring.
    With `files` "index" the repository is `git -C <project_dir> rev-parse --show-toplevel`, and the project's staged
    blobs are materialized into a temporary directory that is removed afterwards; every file is read from that copy,
    Automation/Provenance.json and Automation/BuildLog.jsonl included, so an unstaged change to them is ignored as an
    unstaged change to an asset is, and staged paths that collide on a case-insensitive file system are path-collision
    findings. The findings name project-relative paths either way. A project that is missing (in
    index mode: a directory that does not exist, checked before git runs, or one with no staged file or no staged root
    *.eproj), or whose Provenance.json or BuildLog.jsonl is missing (in the copy, in index mode), is a ProjectAudit with
    that finding (MISSING_FILE_CODES), not an exception. Raises ValueError for a `files` value outside FILE_MODES, and
    RuntimeError naming the git command and its output when git is missing or fails in index mode, a directory outside
    any git work tree included, and naming the file when the engine-consumed files cannot be listed or read."""
    _check_file_mode(files)
    project = Path(project_dir).resolve()
    if files == "disk" or not project.is_dir():
        return _audit_copy(project, project, files)
    toplevel = _git(["-C", str(project), "rev-parse", "--show-toplevel"]).decode("utf-8").strip()
    if not toplevel:
        raise RuntimeError(f"git -C {project} rev-parse --show-toplevel named no work tree")
    with tempfile.TemporaryDirectory(prefix="AuditProvenance-", ignore_cleanup_errors=True) as temporary:
        try:
            copy, staged = _materialize_index(Path(toplevel), project, Path(temporary) / "Index")
        except ValueError as error:
            raise RuntimeError(f"git -C {project} rev-parse --show-toplevel named a work tree that does not hold the "
                               f"project: {error}") from None
        return _audit_copy(project, copy, files, staged)


def audit_exit_code(audit: ProjectAudit) -> int:
    """EXIT_PASSED when `audit` has no finding, EXIT_MISSING_FILE when a finding's code is in MISSING_FILE_CODES, else
    EXIT_FINDINGS."""
    if not audit.findings:
        return EXIT_PASSED
    if any(finding.code in MISSING_FILE_CODES for finding in audit.findings):
        return EXIT_MISSING_FILE
    return EXIT_FINDINGS


def audit_to_json(audit: ProjectAudit) -> dict[str, Any]:
    """The audit as AuditProvenance.py --json and the games suite's step data carry it: {"project": <repository-relative
    path, "/" separators, or the absolute path outside the repository>, "files": <file source>, "passed": <no finding>,
    "counts": {"files", "entries", "transcriptLines", "requests"}, "findings": [{"code", "path", "detail"}],
    "warnings": [{"code", "path", "detail"}]}."""
    return {
        "project": paths.display_path(audit.project),
        "files": audit.files,
        "passed": not audit.findings,
        "counts": {key: audit.counts.get(key, 0) for key in _COUNT_KEYS},
        "findings": [_finding_to_json(finding) for finding in audit.findings],
        "warnings": [_finding_to_json(finding) for finding in audit.warnings],
    }


# --------------------------------------------------------------------------------------------------------------------
# The audit of one project copy: the working tree in disk mode, materialize_index's copy in index mode
# --------------------------------------------------------------------------------------------------------------------


def _audit_copy(project: Path, copy: Path, files: str, staged: list[str] | None = None) -> ProjectAudit:
    """The audit of `copy`; `staged` (index mode) lists the project's staged paths the copy was written from."""
    counts = {key: 0 for key in _COUNT_KEYS}
    missing = _missing_project(project, copy, files)
    if missing:
        return ProjectAudit(project, files, counts, [Finding("project-missing", "", missing)], [])
    try:
        engine_files = enumerate_engine_files(copy, files)
    except OSError as error:
        raise RuntimeError(f"the files of {project} cannot be listed: {error}") from None
    contents: dict[str, bytes] = {}
    for relative in engine_files:
        try:
            contents[relative] = (copy / relative).read_bytes()
        except OSError as error:
            raise RuntimeError(f"{project / relative} cannot be read: {error}") from None
    counts["files"] = len(engine_files)

    findings: list[Finding] = _path_collisions(staged or [])
    entries: list[ProvenanceEntry] | None = None
    try:
        entries = load_provenance(copy / PROVENANCE_FILE)
        counts["entries"] = len(entries)
    except FileNotFoundError:
        findings.append(Finding("provenance-missing", PROVENANCE_FILE,
                                f"{PROVENANCE_FILE} does not exist: the editor writes it with every recorded write, "
                                "and it is committed with the project (the rules were not checked)"))
    except (OSError, ValueError) as error:
        # The reader names the file it read, which is a temporary copy in index mode.
        reason = str(error).removeprefix(f"{copy / PROVENANCE_FILE}: ")
        findings.append(Finding("provenance-invalid", PROVENANCE_FILE,
                                f"{PROVENANCE_FILE} is refused: {reason}; only the editor writes it (the rules were "
                                "not checked)"))
    transcript: Transcript | None = None
    try:
        transcript = load_transcript(copy / TRANSCRIPT_FILE)
    except FileNotFoundError:
        findings.append(Finding("transcript-missing", TRANSCRIPT_FILE,
                                f"{TRANSCRIPT_FILE} does not exist: the MCP bridge appends it with every call, and it "
                                "is committed with the project (the rules were not checked)"))
    except OSError as error:
        findings.append(Finding("transcript-invalid", TRANSCRIPT_FILE,
                                f"{TRANSCRIPT_FILE} cannot be read: {error} (the rules were not checked)"))

    kinds: dict[int, str] = {}
    requests: dict[int, _Request] = {}
    if transcript is not None:
        kinds, requests, invalid = _read_transcript(transcript)
        counts["transcriptLines"] = len(transcript.lines)
        counts["requests"] = len(requests)
        findings += invalid
    if entries is not None and transcript is not None:
        findings += _rule_1(engine_files, contents, entries)
        findings += _rule_2(entries, kinds, requests, len(transcript.lines))
        findings += _rule_3(engine_files, entries, requests, files)

    warnings = _crlf_warnings(engine_files, contents)
    if transcript is not None:
        warnings += _torn_line_warnings(transcript, kinds, entries or [])
    return ProjectAudit(project, files, counts, _sorted_findings(findings, FINDING_CODES),
                        _sorted_findings(warnings, WARNING_CODES))


def _missing_project(project: Path, copy: Path, files: str) -> str:
    """Why the project is missing, or "" when it is not: its directory, or its root *.eproj, does not exist (in index
    mode: is not staged)."""
    shown = paths.display_path(project)
    if not project.is_dir():
        return f"{shown} is not a directory: pass a game project's directory or its .eproj file"
    if not copy.is_dir():
        return (f"no file of {shown} is staged: index mode audits the staged blobs, as a fresh clone holds them (git "
                "add the project, or pass --files disk)")
    if not any(entry.is_file() and entry.name.endswith(_PROJECT_EXTENSION) and _is_root_engine_file(entry.name)
               for entry in copy.iterdir()):
        staged = " staged" if files == "index" else ""
        return f"{shown} holds no{staged} root *.eproj file, so it is not a game project"
    return ""


def _read_transcript(transcript: Transcript) -> tuple[dict[int, str], dict[int, _Request], list[Finding]]:
    """Each line's kind, the well-formed request lines with whether a response marked each ok, and the
    transcript-invalid findings."""
    kinds: dict[int, str] = {}
    findings: list[Finding] = []
    responses: dict[int, list[dict[str, Any]]] = {}
    for line in transcript.lines:
        kind, problem = _classify_line(line)
        kinds[line.number] = kind
        if kind == _INVALID:
            findings.append(Finding("transcript-invalid", TRANSCRIPT_FILE,
                                    f"line {line.number} of {TRANSCRIPT_FILE} is {problem} (ADR 0008 decision 11): "
                                    "only the MCP bridge and Editor --upgrade write the transcript"))
        elif kind == _RESPONSE and line.record is not None:
            responses.setdefault(line.record["requestLine"], []).append(line.record)
    requests: dict[int, _Request] = {}
    for line in transcript.lines:
        record = line.record
        if kinds[line.number] != _REQUEST or record is None:
            continue
        succeeded = any(response["ok"] is True and _same_value(response["id"], record["id"])
                        and response["client"] == record["client"] for response in responses.get(line.number, ()))
        requests[line.number] = _Request(line.number, record, succeeded)
    return kinds, requests, findings


def _rule_1(engine_files: list[str], contents: dict[str, bytes], entries: list[ProvenanceEntry]) -> list[Finding]:
    """Every engine-consumed file has an entry whose XXH64 is the file's hash."""
    findings: list[Finding] = []
    by_path = {entry.path: entry for entry in entries}
    for relative in engine_files:
        entry = by_path.get(relative)
        if entry is None:
            findings.append(Finding("entry-missing", relative,
                                    f"{relative} has no provenance entry (rule 1), so the editor did not write it: "
                                    "write it through an automation method, or remove it"))
            continue
        actual = xxh64.xxh64_hex(contents[relative])
        if entry.xxh64.lower() != actual:
            findings.append(Finding("hash-mismatch", relative,
                                    f"{relative} hashes to {actual}, but provenance records {entry.xxh64} from "
                                    f"{entry.method} {_at_line(entry)} (rule 1), so it changed outside the editor: "
                                    "repeat the change through automation (after an engine change to canonical "
                                    "output, Editor --headless --project <dir> --upgrade)"))
    return findings


def _rule_2(entries: list[ProvenanceEntry], kinds: dict[int, str], requests: dict[int, _Request],
            line_count: int) -> list[Finding]:
    """Every entry was written by an automation method whose request is at the entry's transcript line."""
    findings: list[Finding] = []
    cli_upgrade = any(request.record["method"] == _UPGRADE_METHOD and request.record["client"] == _CLI_CLIENT
                      for request in requests.values())
    for entry in entries:
        path, number = entry.path, entry.transcript_line
        if entry.method == _UI or entry.client == _UI:
            findings.append(Finding("written-by-ui", path,
                                    f"{path} was written by the editor's UI (method {entry.method}, client "
                                    f"{entry.client}) (rule 2): a demo's files are written through automation only, so "
                                    "repeat the change through an automation method"))
        elif number is None:
            if entry.method == _UPGRADE_METHOD and entry.client == _CLI_CLIENT:
                if not cli_upgrade:
                    findings.append(Finding("transcript-line-missing", path,
                                            f"{path} was written by project.upgrade (client cli) without a transcript "
                                            "line, and the transcript holds no project.upgrade request of client cli "
                                            "(rule 2): run Editor --headless --project <dir> --upgrade, which "
                                            "transcribes its request"))
            else:
                findings.append(Finding("transcript-line-missing", path,
                                        f"{path} was written by {entry.method} (client {entry.client}) without a "
                                        "transcript line (rule 2): --batch runs, raw engine_client sessions and builds "
                                        "with ENGINE_MCP_TRANSCRIPT set are not transcribed, so build the project "
                                        "through the MCP bridge"))
        elif number > line_count:
            findings.append(Finding("transcript-line-missing", path,
                                    f"{path}'s entry ({entry.method}) names transcript line {number}, but "
                                    f"{TRANSCRIPT_FILE} has {line_count} line(s) (rule 2): was the transcript "
                                    "truncated, or ENGINE_MCP_TRANSCRIPT set while building?"))
        elif kinds.get(number) == _TORN:
            findings.append(Finding("torn-line", path,
                                    f"{path}'s entry ({entry.method}) names transcript line {number}, which is torn "
                                    "(not a JSON object), so its request cannot be checked (rule 2): write the file "
                                    "again through automation, so that its entry names a complete request line"))
        elif number not in requests:
            what = "a response line" if kinds.get(number) == _RESPONSE else "not a well-formed request line"
            findings.append(Finding("transcript-mismatch", path,
                                    f"{path}'s entry ({entry.method}) names transcript line {number}, which is {what} "
                                    "(rule 2): was the transcript edited, or ENGINE_MCP_TRANSCRIPT set while "
                                    "building?"))
        else:
            findings += _compare_request(entry, requests[number].record)
    return findings


def _compare_request(entry: ProvenanceEntry, record: dict[str, Any]) -> list[Finding]:
    """A transcript-mismatch finding naming each member of `entry` that differs from its request line `record`."""
    differing = [name for name, recorded, transcribed in (("RequestId", entry.request_id, record["id"]),
                                                           ("Method", entry.method, record["method"]),
                                                           ("Client", entry.client, record["client"]))
                 if not _same_value(recorded, transcribed)]
    if not differing:
        return []
    return [Finding("transcript-mismatch", entry.path,
                    f"{entry.path}'s entry (method {entry.method}, request id {_describe(entry.request_id)}, client "
                    f"{entry.client}) differs in {', '.join(differing)} from the request at transcript line "
                    f"{entry.transcript_line} (method {record['method']}, id {_describe(record['id'])}, client "
                    f"{record['client']}) (rule 2): was the transcript or the provenance edited?")]


def _rule_3(engine_files: list[str], entries: list[ProvenanceEntry], requests: dict[int, _Request],
            files: str) -> list[Finding]:
    """No entry names a file that is not audited, unless a later successful asset.delete or asset.move takes it (or its
    source) away."""
    findings: list[Finding] = []
    audited = set(engine_files)
    by_folded_case = {relative.casefold(): relative for relative in engine_files}
    removals = _removals(requests)
    for entry in entries:
        if entry.path in audited:
            continue
        after = entry.transcript_line or 0
        if any(number > after and _names_file(value, entry.path) for number, value in removals):
            continue
        hints = []
        if files == "index":
            hints.append("index mode audits only staged files")
        similar = by_folded_case.get(entry.path.casefold())
        if similar is not None:
            hints.append(f"the audited file {similar} differs only in letter case")
        hint = f" ({'; '.join(hints)})" if hints else ""
        findings.append(Finding("entry-dangling", entry.path,
                                f"{entry.path} has a provenance entry ({entry.method} {_at_line(entry)}) but no "
                                f"audited file{hint}, and no later successful asset.delete or asset.move takes it or "
                                "its source away (rule 3; a deleted folder or a move's destination never does): "
                                "restore the file, or delete or move it through automation"))
    return findings


def _removals(requests: dict[int, _Request]) -> list[tuple[int, str]]:
    """(request line, asset param) of every successful asset.delete and asset.move, top level or an edit.batch op."""
    removals: list[tuple[int, str]] = []
    for number, request in sorted(requests.items()):
        if not request.succeeded:
            continue
        method, params = request.record["method"], request.record["params"]
        operations: list[tuple[Any, Any]] = [(method, params)]
        if method == _BATCH_METHOD and isinstance(params.get("ops"), list):
            operations = [(operation.get("method"), operation.get("params")) for operation in params["ops"]
                          if isinstance(operation, dict)]
        for operation_method, operation_params in operations:
            if (operation_method in _REMOVING_METHODS and isinstance(operation_params, dict)
                    and isinstance(operation_params.get(_REMOVED_PATH_PARAM), str)):
                removals.append((number, operation_params[_REMOVED_PATH_PARAM]))
    return removals


def _names_file(value: str, path: str) -> bool:
    """Whether the asset param `value` names the file `path` or its source (`path` without ".meta"), "project://"
    prefixes and "#<sub-asset>" suffixes ignored; never a directory above it (module docstring, rule 3)."""
    named = value.removeprefix(_PROJECT_SCHEME).split("#", 1)[0]
    if not named:
        return False
    return named in (path, path.removesuffix(_META_EXTENSION))


def _path_collisions(staged: list[str]) -> list[Finding]:
    """A path-collision finding for each staged path that equals another once both are NFC-normalized and case-folded
    (casefold, the full folding, so no pair a case-insensitive file system merges is missed)."""
    groups: dict[str, list[str]] = {}
    for relative in staged:
        groups.setdefault(unicodedata.normalize("NFC", relative).casefold(), []).append(relative)
    findings: list[Finding] = []
    for paths_alike in groups.values():
        if len(paths_alike) < 2:
            continue
        for relative in paths_alike:
            others = ", ".join(other for other in sorted(paths_alike, key=_utf8_key) if other != relative)
            findings.append(Finding("path-collision", relative,
                                    f"{relative} and the staged {others} differ only in letter case or Unicode "
                                    "normalization: a case-insensitive file system (Windows, macOS) holds one file for "
                                    "them, so a clone there loses all but one, and the copy the rules read may too; "
                                    "keep one name (rename or delete the others through automation) and stage the "
                                    "result"))
    return findings


def _crlf_warnings(engine_files: list[str], contents: dict[str, bytes]) -> list[Finding]:
    warnings: list[Finding] = []
    for relative in engine_files:
        data = contents[relative]
        if b"\0" not in data[:_TEXT_PROBE_BYTES] and b"\r\n" in data:
            warnings.append(Finding("crlf", relative,
                                    f"{relative} is text with CR LF line endings: a fresh clone keeps these bytes only "
                                    "where .gitattributes marks the file -text, as the demo projects' rules do, and "
                                    "otherwise its hash differs there (write LF text)"))
    return warnings


def _torn_line_warnings(transcript: Transcript, kinds: dict[int, str],
                        entries: list[ProvenanceEntry]) -> list[Finding]:
    referenced = {entry.transcript_line for entry in entries if entry.transcript_line is not None}
    warnings: list[Finding] = []
    for line in transcript.lines:
        if kinds.get(line.number) != _TORN or line.number in referenced:
            continue
        cut = "" if line.terminated else ", and the last line has no newline: a write was cut off"
        warnings.append(Finding("torn-line", TRANSCRIPT_FILE,
                                f"line {line.number} of {TRANSCRIPT_FILE} is torn (not a JSON object{cut}); no "
                                "provenance entry names it, and it stays because the transcript is append-only"))
    return warnings


# --------------------------------------------------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------------------------------------------------


def _check_file_mode(files: str) -> None:
    if files not in FILE_MODES:
        raise ValueError(f"unknown file source {files!r}: expected one of {', '.join(FILE_MODES)}")


def _is_root_engine_file(name: str) -> bool:
    """Whether a file at the project root is engine-consumed: a *.eproj (ProvenanceRecorder::IsRecordedPath) or the
    .luaurc the type checker reads."""
    return name == _LUAURC or (name.endswith(_PROJECT_EXTENSION) and len(name) > len(_PROJECT_EXTENSION))


def _raise_walk_error(error: OSError) -> None:
    raise error


def _utf8_key(relative: str) -> bytes:
    return relative.encode("utf-8", errors="surrogateescape")


def _relative_inside(project: Path, repository: Path) -> str:
    """`project`'s path relative to `repository` ("/" separators, "" for the repository itself), each part compared as
    the file system compares it (ignoring letter case on Windows). Raises ValueError when it is not inside."""
    project_parts = project.resolve().parts
    repository_parts = repository.resolve().parts
    if len(project_parts) < len(repository_parts) or any(
            os.path.normcase(inner) != os.path.normcase(outer)
            for inner, outer in zip(project_parts, repository_parts)):
        raise ValueError(f"{project} is not inside the repository {repository}")
    return "/".join(project_parts[len(repository_parts):])


def _git(arguments: list[str], stdin: bytes | None = None, environment: dict[str, str] | None = None) -> bytes:
    """git's standard output for `arguments` (fed `stdin`, with `environment` added to this process's environment);
    RuntimeError naming the command and its output when git is missing or fails."""
    command = ["git", *arguments]
    child_environment = {**os.environ, **environment} if environment else None
    shown = " ".join(command)
    try:
        if stdin is None:
            completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                       timeout=_GIT_TIMEOUT_SECONDS, check=False, env=child_environment)
        else:
            completed = subprocess.run(command, input=stdin, capture_output=True, timeout=_GIT_TIMEOUT_SECONDS,
                                       check=False, env=child_environment)
    except FileNotFoundError:
        raise RuntimeError(f"{shown}: git is not installed or not on PATH (python Scripts/Setup.py checks the "
                           "toolchain)") from None
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"{shown} did not finish within {_GIT_TIMEOUT_SECONDS:.0f} s") from None
    except OSError as error:
        raise RuntimeError(f"{shown} could not start: {error}") from None
    if completed.returncode != 0:
        output = (completed.stderr or completed.stdout).decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"{shown} exited with code {completed.returncode}: {output}")
    return completed.stdout


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {}
    for key, value in pairs:
        if key in document:
            raise ValueError(f"duplicate key \"{key}\"")
        document[key] = value
    return document


def _reject_constant(name: str) -> Any:
    raise ValueError(f"{name} is not a JSON number")


def _parse_finite_float(text: str) -> float:
    value = float(text)
    if value in (float("inf"), float("-inf")):
        raise ValueError(f"{text} overflows a double")
    return value


def _parse_strict_json(text: str) -> Any:
    """`text` as strict JSON, as JsonReader::Parse reads it: duplicate keys, NaN, Infinity, numbers that overflow a
    double and unpaired surrogate escapes are refused with ValueError (json.JSONDecodeError for a syntax error)."""
    try:
        document = json.loads(text, object_pairs_hook=_reject_duplicate_keys, parse_constant=_reject_constant,
                              parse_float=_parse_finite_float)
        json.dumps(document, ensure_ascii=False).encode("utf-8")
    except UnicodeEncodeError:
        raise ValueError("a string holds an unpaired surrogate escape") from None
    except RecursionError:
        raise ValueError("the document is nested too deeply") from None
    return document


def _parse_line(raw: bytes) -> dict[str, Any] | None:
    """A transcript line's JSON object, or None for a torn line (invalid UTF-8 or JSON, or not an object)."""
    try:
        record = _parse_strict_json(raw.decode("utf-8"))
    except ValueError:
        return None
    return record if isinstance(record, dict) else None


def _check_members(document: dict[str, Any], required: tuple[str, ...], pointer: str,
                   refuse: Callable[[str, str], ValueError]) -> None:
    """Raises refuse(...) for an unknown member of `document`, then for a missing one."""
    for key in document:
        if key not in required:
            raise refuse(f"{pointer}/{_escape_pointer(key)}", f"unknown member \"{key}\"")
    for key in required:
        if key not in document:
            raise refuse(f"{pointer}/{_escape_pointer(key)}", f"missing member \"{key}\"")


def _escape_pointer(key: str) -> str:
    return key.replace("~", "~0").replace("/", "~1")


def _classify_line(line: TranscriptLine) -> tuple[str, str]:
    """The kind of a transcript line (_REQUEST, _RESPONSE, _TORN or _INVALID) and, for an invalid one, why."""
    record = line.record
    if record is None:
        return _TORN, "not a JSON object"
    kind = record.get("type")
    if kind == _REQUEST:
        problem = _request_problem(record)
        if problem:
            return _INVALID, f"a malformed request line ({problem})"
        if record["method"] in _TOKEN_METHODS:
            return _INVALID, (f"a {record['method']} request line, which carries the session token and must never be "
                              "committed (the bridge never writes one)")
        return _REQUEST, ""
    if kind == _RESPONSE:
        problem = _response_problem(record)
        return (_INVALID, f"a malformed response line ({problem})") if problem else (_RESPONSE, "")
    return _INVALID, f"a JSON object that is neither a request nor a response line (\"type\" {_describe(kind)})"


def _request_problem(record: dict[str, Any]) -> str:
    """Why `record` is not a well-formed request line, or ""."""
    problem = _member_problem(record, _REQUEST_MEMBERS, ())
    if problem:
        return problem
    checks = (
        (isinstance(record["time"], str), "a \"time\" that is not a string"),
        (isinstance(record["client"], str), "a \"client\" that is not a string"),
        (_is_request_id(record["id"]), "an \"id\" that is not an integer or a string"),
        (isinstance(record["method"], str) and bool(record["method"]), "a \"method\" that is not a non-empty string"),
        (isinstance(record["params"], dict), "\"params\" that are not an object"),
    )
    return next((message for valid, message in checks if not valid), "")


def _response_problem(record: dict[str, Any]) -> str:
    """Why `record` is not a well-formed response line, or ""."""
    problem = _member_problem(record, _RESPONSE_MEMBERS, _RESPONSE_OPTIONAL_MEMBERS)
    if problem:
        return problem
    request_line = record["requestLine"]
    checks = (
        (isinstance(record["time"], str), "a \"time\" that is not a string"),
        (isinstance(record["client"], str), "a \"client\" that is not a string"),
        (_is_request_id(record["id"]), "an \"id\" that is not an integer or a string"),
        (type(request_line) is int and request_line >= 1, "a \"requestLine\" that is not a 1-based line number"),
        (isinstance(record["ok"], bool), "an \"ok\" that is not a boolean"),
        (isinstance(record["summary"], str), "a \"summary\" that is not a string"),
        (isinstance(record.get("error", {}), dict), "an \"error\" that is not an object"),
    )
    return next((message for valid, message in checks if not valid), "")


def _member_problem(record: dict[str, Any], required: tuple[str, ...], optional: tuple[str, ...]) -> str:
    unknown = [key for key in record if key not in required and key not in optional]
    if unknown:
        return f"an unknown member \"{unknown[0]}\""
    absent = [key for key in required if key not in record]
    return f"no \"{absent[0]}\" member" if absent else ""


def _is_request_id(value: Any) -> bool:
    """A JSON-RPC id as the editor and the bridge send it: an integer (never a boolean) or a string."""
    return type(value) is int or isinstance(value, str)


def _same_value(recorded: Any, transcribed: Any) -> bool:
    """Type-strict equality: a boolean never equals an integer, and an integer never equals a string."""
    return type(recorded) is type(transcribed) and recorded == transcribed


def _describe(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False)


def _at_line(entry: ProvenanceEntry) -> str:
    if entry.transcript_line is None:
        return "without a transcript line"
    return f"at transcript line {entry.transcript_line}"


def _sorted_findings(findings: list[Finding], codes: tuple[str, ...]) -> list[Finding]:
    """`findings` in `codes` order, then by path; stable, so the findings of one path keep their order."""
    return sorted(findings, key=lambda finding: (codes.index(finding.code), _utf8_key(finding.path)))


def _finding_to_json(finding: Finding) -> dict[str, str]:
    return {"code": finding.code, "path": finding.path, "detail": finding.detail}
