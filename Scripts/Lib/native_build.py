"""The native build of a game's module (Docs/Decisions/0030-m19-contract.md decision 30, C-NATIVE-STATIC): what
Scripts/BuildNative.py (stream N-A) and the games suite's native preparation (decision 35) share. The editor runs
BuildNative.py by fixed paths with fixed arguments (EditorCore/Native/NativeBuild.h); its C++ ListNativeSources and
ComputeNativeSourceHash compute what list_sources and source_hash compute here, which a test pins
(Tests/Automation/test_build_native.py and Tests/Source/EditorCore/Native/NativeBuildTests.cpp hash the same files to
the same value), and its ParseNativeBuildRecord reads the record write_record writes. Windows backends only
(Docs/Decisions/0028-backlog-program.md decision 10): the build itself runs on Windows; these helpers are pure Python.
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import struct
from pathlib import Path
from typing import Any

from .xxh64 import xxh64_hex

# The project directory of a game's module and the kinds of file it compiles (decision 26).
NATIVE_DIRECTORY = "Native"
SOURCE_SUFFIXES = (".cpp", ".h")
# The build record every build writes into its output directory (decision 30).
RECORD_FILE = "NativeBuild.json"
RECORD_FORMAT = "NativeBuild"
RECORD_VERSION = 1
# The interface every module compiles against, repository-relative, part of the source hash.
API_HEADER = "Engine/Source/Engine/Native/NativeApi.h"
# The name the source hash gives NativeApi.h: its path below the Engine include root, as a source includes it.
API_HEADER_HASH_NAME = "Engine/Native/NativeApi.h"
SOURCE_HASH_PREFIX = "EngineNativeSources 1"

KINDS = ("Module", "Runtime")
CONFIGURATIONS = ("Debug", "Release", "Dist")
SEVERITIES = ("Error", "Warning")
# The build's whole output, beside the record, and the outputs of each kind (Scripts/Premake/NativeModule.lua).
LOG_FILE = "Build.log"
MODULE_LIBRARY = "NativeModule.dll"
RUNTIME_EXECUTABLE = "Runtime.exe"
# The workspace premake generates and the objects MSBuild compiles, below the output directory.
WORKSPACE_DIRECTORY = "Workspace"
INTERMEDIATE_DIRECTORY = "Intermediate"
WORKSPACE_INPUTS_FILE = "Inputs.json"

# The diagnostic codes of a build (NativeBuildDiagnostic::Code, EditorCore/Native/NativeBuild.h). FORMAT_WARNING is
# always of severity Warning: a source that is not clang-format clean, which fails nothing (Scripts/BuildNative.py).
LINT_ERROR = "NATIVE_LINT_ERROR"
FORMAT_WARNING = "NATIVE_FORMAT_WARNING"
COMPILE_ERROR = "NATIVE_COMPILE_ERROR"
COMPILE_WARNING = "NATIVE_COMPILE_WARNING"
LINK_ERROR = "NATIVE_LINK_ERROR"
BUILD_ERROR = "NATIVE_BUILD_ERROR"
CODES = (LINT_ERROR, FORMAT_WARNING, COMPILE_ERROR, COMPILE_WARNING, LINK_ERROR, BUILD_ERROR)

RECORD_KEYS = ("Format", "Version", "Kind", "Configuration", "SourceHash", "Sources", "Succeeded", "Output", "Log",
               "Diagnostics")
DIAGNOSTIC_KEYS = ("Severity", "Code", "File", "Line", "Column", "Message")
_HASH_PATTERN = re.compile(r"^[0-9a-f]{16}$")
_UINT32_LIMIT = 1 << 32

# An MSVC or MSBuild diagnostic line, without MSBuild's "<node>>" prefix and its trailing " [<project file>]":
#   <file>(<line>[,<column>]): [fatal ]error|warning <code>: <message>
#   <tool or object> : [fatal ]error|warning <code>: <message>        (the linker, MSBuild)
_LOCATED_DIAGNOSTIC = re.compile(
    r"^(?P<file>.+?)\((?P<line>\d+)(?:,(?P<column>\d+))?(?:,\d+,\d+)?\)\s*:\s*(?:fatal\s+)?"
    r"(?P<kind>error|warning)\s+(?P<code>[A-Za-z]+\d+)\s*:\s*(?P<message>.*)$")
_TOOL_DIAGNOSTIC = re.compile(
    r"^(?P<file>.*?)\s*:\s*(?:fatal\s+)?(?P<kind>error|warning)\s+(?P<code>[A-Za-z]+\d+)\s*:\s*(?P<message>.*)$")
_PROJECT_SUFFIX = re.compile(r"\s*\[[^\[\]]+\.vcxproj\]\s*$", re.IGNORECASE)
_NODE_PREFIX = re.compile(r"^\s*\d+(?::\d+)?>")


class RecordError(ValueError):
    """A build record that is not what a build writes."""


@dataclasses.dataclass(frozen=True)
class Diagnostic:
    """One finding of a build (NativeBuildDiagnostic): located in a source when `file` is not empty."""

    severity: str
    code: str
    file: str = ""
    line: int = 0
    column: int = 0
    message: str = ""

    def to_json(self) -> dict[str, Any]:
        return {"Severity": self.severity, "Code": self.code, "File": self.file, "Line": self.line,
                "Column": self.column, "Message": self.message}

    def describe(self) -> str:
        where = self.file
        if where and self.line:
            where += f":{self.line}" + (f":{self.column}" if self.column else "")
        return f"{where + ': ' if where else ''}{self.code}: {self.message}"


@dataclasses.dataclass
class BuildRecord:
    """NativeBuild.json (decision 30; NativeBuildRecord in EditorCore/Native/NativeBuild.h). `source_hash` is empty
    when the build failed before it could hash the sources."""

    kind: str
    configuration: str
    source_hash: str
    sources: list[str]
    succeeded: bool
    output: str
    log: str
    diagnostics: list[Diagnostic]

    def to_json(self) -> dict[str, Any]:
        return {"Format": RECORD_FORMAT, "Version": RECORD_VERSION, "Kind": self.kind,
                "Configuration": self.configuration, "SourceHash": self.source_hash, "Sources": list(self.sources),
                "Succeeded": self.succeeded, "Output": self.output, "Log": self.log,
                "Diagnostics": [diagnostic.to_json() for diagnostic in self.diagnostics]}


# --------------------------------------------------------------------------------------------------------------------
# Sources and their hash
# --------------------------------------------------------------------------------------------------------------------


def list_sources(project_root: Path) -> list[str]:
    """Every .cpp and .h file below <project_root>/Native/, project-relative with forward slashes ("Native/Spin.cpp"),
    sorted by their UTF-8 bytes; empty when the project has no Native/ directory. Raises OSError when a directory cannot
    be listed."""
    native = project_root / NATIVE_DIRECTORY
    if not native.is_dir():
        return []

    def fail(error: OSError) -> None:
        raise error

    sources: list[str] = []
    for directory, _, names in os.walk(native, onerror=fail):
        for name in names:
            if name.endswith(SOURCE_SUFFIXES):
                sources.append((Path(directory) / name).relative_to(project_root).as_posix())
    return sorted(sources, key=lambda source: source.encode("utf-8"))


def _hashed_file(name: str, data: bytes) -> bytes:
    return name.encode("utf-8") + b"\n" + struct.pack("<Q", len(data)) + data


def source_hash(project_root: Path, configuration: str, api_header: Path) -> str:
    """The source hash of a module build (decision 30), as 16 lowercase hex digits: XXH64 (seed 0, Lib/xxh64.py) over
    the UTF-8 text "EngineNativeSources 1\\n<configuration>\\n", then for `api_header` and for each of list_sources'
    files in order: its path ("Engine/Native/NativeApi.h" for the header, the project-relative path for a source),
    "\\n", its size as 8 little-endian bytes and its bytes. `configuration` is "Debug", "Release" or "Dist". A build is
    current when its record's SourceHash equals the value now. Raises FileNotFoundError for a missing header."""
    data = bytearray(f"{SOURCE_HASH_PREFIX}\n{configuration}\n".encode("utf-8"))
    data += _hashed_file(API_HEADER_HASH_NAME, api_header.read_bytes())
    for source in list_sources(project_root):
        data += _hashed_file(source, (project_root / source).read_bytes())
    return xxh64_hex(bytes(data))


# --------------------------------------------------------------------------------------------------------------------
# The build record
# --------------------------------------------------------------------------------------------------------------------


def _json_scalar(value: Any) -> str:
    if isinstance(value, bool) or value is None:
        return json.dumps(value)
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    raise TypeError(f"{value!r} is not a scalar of a build record")


def _canonical(value: Any, indent: int = 0) -> str:
    """The engine's canonical JSON layout (Core/Json/JsonWriter.h, JsonStyle::Pretty): tabs, scalar arrays on one line,
    objects and other arrays expanded."""
    pad = "\t" * indent
    inner = "\t" * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{}"
        members = [f"{inner}{json.dumps(key, ensure_ascii=False)}: {_canonical(item, indent + 1)}"
                   for key, item in value.items()]
        return "{\n" + ",\n".join(members) + "\n" + pad + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        if all(not isinstance(item, (dict, list)) for item in value):
            return "[" + ", ".join(_json_scalar(item) for item in value) + "]"
        return "[\n" + ",\n".join(inner + _canonical(item, indent + 1) for item in value) + "\n" + pad + "]"
    return _json_scalar(value)


def record_text(record: BuildRecord) -> str:
    """The record as a build writes it: canonical JSON with a final newline."""
    return _canonical(record.to_json()) + "\n"


def write_record(directory: Path, record: BuildRecord) -> Path:
    """Writes <directory>/NativeBuild.json through a temporary file; returns its path."""
    path = directory / RECORD_FILE
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(record_text(record), encoding="utf-8", newline="\n")
    os.replace(temporary, path)
    return path


def _require(condition: bool, pointer: str, message: str) -> None:
    if not condition:
        raise RecordError(f"{pointer or '/'}: {message}")


def _is_plain_file_name(name: str) -> bool:
    return name not in ("", ".", "..") and "/" not in name and "\\" not in name and ":" not in name


def parse_record(text: str) -> BuildRecord:
    """Reads a build record strictly, as ParseNativeBuildRecord does (EditorCore/Native/NativeBuild.h): exactly the
    members write_record writes, of their types. SourceHash and Output are empty only in the record of a failed build:
    SourceHash when the build failed before it could hash the sources (not Windows, or a source it could not read), so
    the editor still reads why it failed. Raises RecordError (a ValueError) naming the JSON pointer of the first
    problem, or for text that is not JSON."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise RecordError(f"not JSON: {error}") from None
    _require(isinstance(document, dict), "", "expected an object")
    unknown = [key for key in document if key not in RECORD_KEYS]
    _require(not unknown, f"/{unknown[0]}" if unknown else "", "unknown member")
    for key in RECORD_KEYS:
        _require(key in document, "", f"missing required field '{key}'")
    _require(document["Format"] == RECORD_FORMAT, "/Format", f"expected '{RECORD_FORMAT}'")
    _require(type(document["Version"]) is int and document["Version"] == RECORD_VERSION, "/Version",
             f"expected {RECORD_VERSION}")
    _require(document["Kind"] in KINDS, "/Kind", f"expected one of {', '.join(KINDS)}")
    _require(document["Configuration"] in CONFIGURATIONS, "/Configuration",
             f"expected one of {', '.join(CONFIGURATIONS)}")
    _require(isinstance(document["Succeeded"], bool), "/Succeeded", "expected a boolean")
    hash_text = document["SourceHash"]
    _require(isinstance(hash_text, str) and (bool(_HASH_PATTERN.match(hash_text))
                                             or (hash_text == "" and not document["Succeeded"])),
             "/SourceHash", "expected 16 lowercase hex digits (empty only for a failed build)")
    sources = document["Sources"]
    _require(isinstance(sources, list) and all(isinstance(source, str) for source in sources), "/Sources",
             "expected an array of strings")
    for key in ("Output", "Log"):
        _require(isinstance(document[key], str), f"/{key}", "expected a string")
    succeeded = document["Succeeded"]
    output = document["Output"]
    _require(_is_plain_file_name(output) if succeeded else output == "" or _is_plain_file_name(output), "/Output",
             "expected the output's file name (empty only for a failed build)")
    _require(_is_plain_file_name(document["Log"]), "/Log", "expected the log's file name")
    entries = document["Diagnostics"]
    _require(isinstance(entries, list), "/Diagnostics", "expected an array")
    diagnostics: list[Diagnostic] = []
    for index, entry in enumerate(entries):
        pointer = f"/Diagnostics/{index}"
        _require(isinstance(entry, dict), pointer, "expected an object")
        unknown = [key for key in entry if key not in DIAGNOSTIC_KEYS]
        _require(not unknown, f"{pointer}/{unknown[0]}" if unknown else pointer, "unknown member")
        for key in DIAGNOSTIC_KEYS:
            _require(key in entry, pointer, f"missing required field '{key}'")
        _require(entry["Severity"] in SEVERITIES, f"{pointer}/Severity", f"expected one of {', '.join(SEVERITIES)}")
        for key in ("Code", "File", "Message"):
            _require(isinstance(entry[key], str), f"{pointer}/{key}", "expected a string")
        for key in ("Line", "Column"):
            value = entry[key]
            _require(type(value) is int and 0 <= value < _UINT32_LIMIT, f"{pointer}/{key}",
                     "expected an unsigned 32-bit integer")
        diagnostics.append(Diagnostic(entry["Severity"], entry["Code"], entry["File"], entry["Line"], entry["Column"],
                                      entry["Message"]))
    return BuildRecord(document["Kind"], document["Configuration"], document["SourceHash"], list(sources), succeeded,
                       output, document["Log"], diagnostics)


def read_record(directory: Path) -> BuildRecord:
    """<directory>/NativeBuild.json, read strictly (parse_record). Raises OSError or RecordError."""
    return parse_record((directory / RECORD_FILE).read_text(encoding="utf-8"))


# --------------------------------------------------------------------------------------------------------------------
# The build's diagnostics and the workspace's inputs
# --------------------------------------------------------------------------------------------------------------------


def diagnostic_file(path_text: str, project_root: Path, repository_root: Path) -> str:
    """How a diagnostic names the file it is located in: project-relative for the game's files ("Native/Spin.cpp"),
    API_HEADER_HASH_NAME for the module's interface, empty for any other file (whose path the message keeps)."""
    try:
        path = Path(path_text.strip()).resolve()
        if path == (repository_root / API_HEADER).resolve():
            return API_HEADER_HASH_NAME
        return path.relative_to(project_root.resolve()).as_posix()
    except (OSError, ValueError):
        return ""


def parse_tool_diagnostic(line: str, project_root: Path, repository_root: Path) -> Diagnostic | None:
    """The diagnostic of one MSBuild, compiler or linker output line, or None for any other line. Compiler errors are
    NATIVE_COMPILE_ERROR and its warnings NATIVE_COMPILE_WARNING, linker errors and warnings (LNK) NATIVE_LINK_ERROR:
    all of severity Error, since the module's code builds with warnings as errors (decision 30). Anything else
    (MSBuild's MSB diagnostics, a missing tool) is NATIVE_BUILD_ERROR of its own severity: an MSBuild warning is about
    where or how the build runs (MSB8029: an output directory below %TEMP%), not about the module, and fails nothing."""
    text = _PROJECT_SUFFIX.sub("", _NODE_PREFIX.sub("", line)).strip()
    match = _LOCATED_DIAGNOSTIC.match(text) or _TOOL_DIAGNOSTIC.match(text)
    if match is None:
        return None
    tool_code = match.group("code")
    severity = "Error"
    if tool_code.upper().startswith("LNK"):
        code = LINK_ERROR
    elif tool_code.upper().startswith("C") and tool_code[1:].isdigit():
        code = COMPILE_WARNING if match.group("kind") == "warning" else COMPILE_ERROR
    else:
        code = BUILD_ERROR
        severity = "Warning" if match.group("kind") == "warning" else "Error"
    groups = match.groupdict()
    located = groups.get("line") is not None
    file = diagnostic_file(match.group("file"), project_root, repository_root) if located else ""
    message = f"{tool_code}: {match.group('message').strip()}"
    if located and not file:
        message = f"{match.group('file').strip()}({groups['line']}): {message}"
    elif not located and match.group("file").strip():
        message = f"{match.group('file').strip()}: {message}"
    return Diagnostic(severity, code, file, int(groups["line"]) if located else 0,
                      int(groups["column"]) if located and groups.get("column") else 0, message)


def write_workspace_inputs(path: Path, inputs: dict[str, Any]) -> Path:
    """Writes Scripts/Premake/NativeModule.lua's Inputs.json (its keys are documented there); returns `path`."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(inputs, indent="\t", ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    return path
