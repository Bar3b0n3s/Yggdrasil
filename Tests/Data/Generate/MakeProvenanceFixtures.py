#!/usr/bin/env python3
"""Generate the provenance-audit fixtures (Docs/Architecture.md §13.12; Docs/Decisions/0026-m16-contract.md decision 6).

Owns Tests/Data/Provenance/ whole (a file it does not write is reported as extra and, without --check, removed):
- <Case>/Project/: a small game project as the MCP bridge builds it: Mini.eproj, .luaurc, .gitignore, AGENTS.md, a
  scene, a script and a material with their .meta files, Automation/Provenance.json (ProvenanceRecorder.h) and
  Automation/BuildLog.jsonl (the transcript, ADR 0008 decision 11);
- <Case>/Expected.json: {"Format": "ProvenanceAuditFixture", "Version": 1, "ExitCode": 0|1|3, "Findings": [{"Code",
  "Path"}], "Warnings": [{"Code", "Path"}]}, what `AuditProvenance.py --project <Case>/Project --files disk` reports;
- README.md, which describes every case.

Clean is a simulated bridge build that holds every passing behaviour of the audit (Scripts/Lib/provenance.py), so an
audit that refused one fails it: project.create at line 1, requests each with its response, a reconnect whose request
ids start again, an asset.delete inside an edit.batch, raw U+2028 and U+0085 inside a line, a failed edit.batch whose
rolled-back write is recorded, a request without a response (a killed bridge), a cli project.upgrade with its line and a
legacy cli project.upgrade entry without one. Every other case is Clean with one seeded defect and has exactly the
findings and warnings its Expected.json lists (AuditProvenance.py --self-test, a static check). A CR LF case is not
committed, since .gitattributes would normalize it; Tests/Automation/test_audit_provenance.py builds it in a copy.

Everything is deterministic: fixed times, ids and handles, and every XXH64 computed with Scripts/Lib/xxh64.py from the
bytes written. The scene, material, .meta and .eproj files are valid documents of their formats (the reference schema
test validates every authored file under Tests/Data), in canonical form except where a case edits one by hand; the
canonical-JSON test exempts Tests/Data/Provenance/, whose files are byte-hash inputs, never engine input. Run from the
repository root: without arguments it writes the files, `--check` compares and writes nothing, `--json` prints a
machine-readable report.

Exit codes: 0 fixtures generated or verified, 1 stale or extra files or an I/O failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import struct
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Tests" / "Data" / "Provenance"
sys.path.insert(0, str(ROOT / "Scripts"))

from Lib.xxh64 import xxh64_hex  # noqa: E402  (Scripts/ is on sys.path above)

PROJECT_NAME = "Mini"
PROJECT_FILE = "Mini.eproj"
PROVENANCE_FILE = "Automation/Provenance.json"
TRANSCRIPT_FILE = "Automation/BuildLog.jsonl"
# The project directory as the bridge sends it to project.create and project.open (the transcript records the params
# as sent, ADR 0008 decision 11).
PROJECT_DIRECTORY = "C:/Work/Mini"
TIME = "2026-01-01T00:00:00.000Z"
BRIDGE = "engine-mcp"
CLI = "cli"
MAIN_SCENE = "Assets/Scenes/Main.scene"
BALL_SCRIPT = "Assets/Scripts/Ball.luau"
RED_MATERIAL = "Assets/Materials/Red.material"
OLD_MATERIAL = "Assets/Materials/Old.material"
CLEAN = "Clean"
EXPECTED_FORMAT = "ProvenanceAuditFixture"
EXIT_PASSED = 0
EXIT_FINDINGS = 1
EXIT_MISSING_FILE = 3

# Raw U+2028 and U+0085 inside the script, so script.write's request line holds them unescaped (the bridge writes
# json.dumps(ensure_ascii=False)): neither ends a transcript line.
BALL_SOURCE = ("--!strict\n-- The ball bounces off the walls,\u2028the paddle and the bricks\u0085(see Main.scene).\n"
               "return {}\n")
DRAFT_SOURCE = "--!strict\n-- A draft, moved to Paddle.luau.\nreturn {}\n"
LATE_SOURCE = "--!strict\n-- Written after its only asset.delete.\nreturn {}\n"
SPARE_SOURCE = "--!strict\n-- Removed by hand; a later asset.move named its path only as the destination.\nreturn {}\n"

# ---------------------------------------------------------------------------------------------------------------------
# Canonical JSON (the JsonWriter form of Architecture §6: tabs, LF, scalar arrays on one line, final newline)
# ---------------------------------------------------------------------------------------------------------------------


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def format_number(value: float | int) -> str:
    """Integers as written; a float as the shortest decimal that reads back as the same float32 (JsonWriter's form)."""
    if isinstance(value, bool):
        raise TypeError("booleans are not numbers")
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        raise ValueError("non-finite number")
    single = float32(value)
    if single == int(single):
        return str(int(single))
    for digits in range(1, 10):
        text = f"{single:.{digits}g}"
        if float32(float(text)) == single:
            if "e" in text:
                raise ValueError(f"{text}: fixture floats are written without an exponent")
            return text
    raise ValueError(f"{value} has no short float32 form")


def format_scalar(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    return format_number(value)


def write_json(value: Any, indent: int = 0) -> str:
    pad = "\t" * indent
    inner = "\t" * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{}"
        members = [f"{inner}{json.dumps(key, ensure_ascii=False)}: {write_json(item, indent + 1)}"
                   for key, item in value.items()]
        return "{\n" + ",\n".join(members) + "\n" + pad + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        if all(not isinstance(item, (dict, list)) for item in value):
            return "[" + ", ".join(format_scalar(item) for item in value) + "]"
        return "[\n" + ",\n".join(inner + write_json(item, indent + 1) for item in value) + "\n" + pad + "]"
    return format_scalar(value)


def canonical(value: Any) -> bytes:
    return (write_json(value) + "\n").encode("utf-8")


# ---------------------------------------------------------------------------------------------------------------------
# The project's documents
# ---------------------------------------------------------------------------------------------------------------------


def project_document() -> dict[str, Any]:
    """The canonical ProjectSettings document of the Empty template after project.setSettings set the start scene."""
    return {
        "Format": "Project", "Version": 1, "Name": PROJECT_NAME, "StartScene": MAIN_SCENE,
        "Window": {"Title": "Game", "Width": 1600, "Height": 900, "VSync": True, "Fullscreen": False,
                   "Resizable": True},
        "Simulation": {"FixedHz": 60, "MaxStepsPerFrame": 5, "Seed": 0, "MaxEntities": 65536},
        "Physics": {"Gravity": [0, -9.81, 0], "Layers": ["Default"], "Collisions": [["Default", "Default"]]},
        "Input": {"Actions": {}},
        "Rendering": {"ShadowMapSize": 2048, "SsaoHalfResolution": False},
        "Scripting": {"MemoryLimitMB": 256, "CallbackBudgetMs": 1000, "PauseOnError": True,
                      "BlockPlayOnTypeErrors": False},
        "Export": {"BuildScenes": [], "Exclude": ["Assets/Tests/**"], "Icon": "", "Version": "1.0.0", "Company": ""},
        "Testing": {"Suites": [], "Replays": [], "CaseTimeoutTicks": 600, "SuiteTickLimit": 36000,
                    "CoverageGates": False},
    }


def entity(identifier: str, name: str, x: float, y: float) -> dict[str, Any]:
    return {"ID": identifier, "Name": name, "Parent": None, "Active": True, "Tags": [],
            "Components": {"Transform": {"Translation": [x, y, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1]}}}


def scene_document() -> dict[str, Any]:
    return {"Format": "Scene", "Version": 1, "Name": "Main", "Seed": 0, "ComponentVersions": {"Transform": 1},
            "Entities": [entity("a1a1000000000001", "Paddle", 0, -4), entity("a1a1000000000002", "Ball", 0, -3)]}


def material_document() -> dict[str, Any]:
    return {"Format": "Material", "Version": 1, "BaseColor": [0.8, 0.1, 0.1, 1], "BaseColorMap": None,
            "Metallic": 0, "Roughness": 0.5, "MetallicRoughnessMap": None, "NormalMap": None, "NormalScale": 1,
            "OcclusionMap": None, "OcclusionStrength": 1, "Emissive": [0, 0, 0], "EmissiveStrength": 1,
            "EmissiveMap": None, "AlphaMode": "Opaque", "AlphaCutoff": 0.5, "DoubleSided": False, "UVScale": [1, 1],
            "UVOffset": [0, 0]}


def meta_document(handle: str, kind: str) -> dict[str, Any]:
    return {"Format": "AssetMeta", "Version": 1, "Handle": handle, "Type": kind, "Importer": kind,
            "ImporterVersion": 1, "Settings": None, "SubAssets": []}


def project_files() -> dict[str, bytes]:
    """Clean's files outside Automation/, as the editor wrote them."""
    return {
        PROJECT_FILE: canonical(project_document()),
        ".luaurc": b'{\n\t"languageMode": "strict"\n}\n',
        ".gitignore": b"Library/\nBuild/\n*.bak\n",
        "AGENTS.md": (f"# {PROJECT_NAME}\n\nA seeded fixture of the provenance audit (Tests/Data/Generate/"
                      "MakeProvenanceFixtures.py). This file is documentation, which the audit exempts.\n").encode(),
        MAIN_SCENE: canonical(scene_document()),
        MAIN_SCENE + ".meta": canonical(meta_document("0a5e000000000001", "Scene")),
        BALL_SCRIPT: BALL_SOURCE.encode("utf-8"),
        BALL_SCRIPT + ".meta": canonical(meta_document("0a5e000000000002", "Script")),
        RED_MATERIAL: canonical(material_document()),
        RED_MATERIAL + ".meta": canonical(meta_document("0a5e000000000003", "Material")),
    }


# ---------------------------------------------------------------------------------------------------------------------
# Transcript lines and provenance entries
# ---------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Line:
    """One transcript line: a JSON object, or the raw bytes of a torn line. A response's requestLine is the line number
    of the request labelled `answers`, resolved when the transcript is written."""

    label: str
    record: dict[str, Any] | None = None
    raw: bytes = b""
    answers: str = ""
    minified: bool = False  # the editor's JsonWriter form (cli lines); the bridge writes json.dumps' default form
    terminated: bool = True


@dataclasses.dataclass
class Entry:
    """One provenance entry; `line` is a request label, an explicit line number or None. Its XXH64 is that of the
    file's bytes, or of `recorded` when the file does not exist or was changed after the write it records."""

    path: str
    method: str
    request_id: int | str | None
    client: str
    line: str | int | None
    recorded: bytes | None = None


@dataclasses.dataclass
class Fixture:
    name: str
    description: str
    files: dict[str, bytes]
    lines: list[Line]
    entries: list[Entry]
    exit_code: int = EXIT_PASSED
    findings: list[tuple[str, str]] = dataclasses.field(default_factory=list)
    warnings: list[tuple[str, str]] = dataclasses.field(default_factory=list)
    write_provenance: bool = True
    write_transcript: bool = True
    unsorted_entries: bool = False  # write the first two entries in the wrong order (a malformed Provenance.json)
    edits: dict[str, bytes] = dataclasses.field(default_factory=dict)  # files changed after their write was recorded

    def entry(self, path: str) -> Entry:
        return next(entry for entry in self.entries if entry.path == path)

    def index(self, label: str) -> int:
        return next(index for index, line in enumerate(self.lines) if line.label == label)


def request(label: str, request_id: int | str, method: str, params: dict[str, Any], client: str = BRIDGE) -> Line:
    return Line(label, {"type": "request", "time": TIME, "client": client, "id": request_id, "method": method,
                        "params": params}, minified=client == CLI)


def response(label: str, request_id: int | str, summary: str, client: str = BRIDGE,
             error: dict[str, Any] | None = None) -> Line:
    record: dict[str, Any] = {"type": "response", "time": TIME, "client": client, "id": request_id,
                              "requestLine": None, "ok": error is None, "summary": summary}
    if error is not None:
        record["error"] = error
    return Line(f"{label}/response", record, answers=label, minified=client == CLI)


def call(label: str, request_id: int | str, method: str, params: dict[str, Any], summary: str, client: str = BRIDGE,
         error: dict[str, Any] | None = None) -> list[Line]:
    """A request line and its response line."""
    return [request(label, request_id, method, params, client), response(label, request_id, summary, client, error)]


def op(method: str, params: dict[str, Any]) -> dict[str, Any]:
    return {"method": method, "params": params}


def clean() -> Fixture:
    lines = [
        *call("create", 2, "project.create", {"path": PROJECT_DIRECTORY, "template": "Empty"}, "project Mini"),
        *call("scene", 3, "scene.new", {"path": MAIN_SCENE}, "scene Main; rev 1"),
        *call("paddle", 4, "entity.create", {"name": "Paddle"}, "entity a1a1000000000001 Paddle; rev 2 dirty"),
        *call("red", 5, "asset.create", {"type": "material", "path": RED_MATERIAL},
              "asset 0a5e000000000003 Material Assets/Materials/Red.material"),
        *call("old", 6, "asset.create", {"type": "material", "path": OLD_MATERIAL},
              "asset 0a5e000000000004 Material Assets/Materials/Old.material"),
        *call("script", 7, "script.write", {"path": BALL_SCRIPT, "source": BALL_SOURCE},
              "path Assets/Scripts/Ball.luau"),
        *call("save", 8, "scene.save", {}, "scene Main; savedFiles 1; rev 2"),
        # The bridge restarted: a new connection, whose request ids start again (session.hello, id 1, is never written).
        *call("open", 2, "project.open", {"path": f"{PROJECT_DIRECTORY}/{PROJECT_FILE}"}, "project Mini"),
        *call("tidy", 3, "edit.batch", {"label": "Tidy", "ops": [op("asset.delete", {"asset": OLD_MATERIAL}),
                                                                op("entity.create", {"name": "Ball"})]},
              "results 2; undoIndex 3; rev 3 dirty"),
        # Op 1 fails, so op 0's write of Red.material is rolled back: the rollback rewrites the file under this request.
        *call("rollback", 4, "edit.batch",
              {"label": "Shine", "ops": [op("asset.setProperties", {"asset": RED_MATERIAL,
                                                                    "properties": {"Roughness": 0.2}}),
                                         op("entity.update", {"entity": "Missing", "patch": {"Name": "Brick"}})]},
              "error -32001 NotFound: no entity named 'Missing'; failedOp 1",
              error={"code": -32001, "errorCode": "NotFound", "detail": "no entity named 'Missing'"}),
        *call("settings", 5, "project.setSettings", {"patch": {"StartScene": MAIN_SCENE}}, "changedFiles 1; rev 3"),
        # The bridge was killed before this request's response.
        request("killed", 6, "scene.save", {}),
        # Editor --headless --project <dir> --upgrade appends its own lines.
        *call("upgrade", 1, "project.upgrade", {}, "changedFiles 1", client=CLI),
    ]
    entries = [
        Entry(".luaurc", "project.create", 2, BRIDGE, "create"),
        Entry(RED_MATERIAL, "edit.batch", 4, BRIDGE, "rollback"),
        # A legacy entry of project.upgrade without its line (§13.12's exception: the transcript holds the request).
        Entry(RED_MATERIAL + ".meta", "project.upgrade", None, CLI, None),
        Entry(MAIN_SCENE, "scene.save", 6, BRIDGE, "killed"),
        Entry(MAIN_SCENE + ".meta", "scene.save", 8, BRIDGE, "save"),
        Entry(BALL_SCRIPT, "script.write", 7, BRIDGE, "script"),
        Entry(BALL_SCRIPT + ".meta", "project.upgrade", 1, CLI, "upgrade"),
        Entry(PROJECT_FILE, "project.setSettings", 5, BRIDGE, "settings"),
    ]
    return Fixture(CLEAN, "A simulated bridge build holding every passing behaviour; no finding and no warning.",
                   project_files(), lines, entries)


# ---------------------------------------------------------------------------------------------------------------------
# The seeded cases: Clean with one defect each
# ---------------------------------------------------------------------------------------------------------------------


def hash_mismatch(fixture: Fixture) -> None:
    fixture.edits[RED_MATERIAL] = fixture.files[RED_MATERIAL].replace(b"\t", b"  ")
    fixture.findings = [("hash-mismatch", RED_MATERIAL)]


def entry_missing(fixture: Fixture) -> None:
    fixture.entries.remove(fixture.entry(MAIN_SCENE + ".meta"))
    fixture.findings = [("entry-missing", MAIN_SCENE + ".meta")]


def ui_write(fixture: Fixture) -> None:
    fixture.entries[fixture.entries.index(fixture.entry(RED_MATERIAL))] = Entry(RED_MATERIAL, "ui", None, "ui", None)
    fixture.findings = [("written-by-ui", RED_MATERIAL)]


def batch_write(fixture: Fixture) -> None:
    entry = fixture.entry(MAIN_SCENE)
    entry.request_id, entry.client, entry.line = 3, "batch", None
    fixture.findings = [("transcript-line-missing", MAIN_SCENE)]


def line_out_of_range(fixture: Fixture) -> None:
    fixture.entry(BALL_SCRIPT).line = 99
    fixture.findings = [("transcript-line-missing", BALL_SCRIPT)]


def line_not_request(fixture: Fixture) -> None:
    fixture.entry(BALL_SCRIPT).line = "script/response"
    fixture.findings = [("transcript-mismatch", BALL_SCRIPT)]


def method_mismatch(fixture: Fixture) -> None:
    fixture.entry(MAIN_SCENE + ".meta").method = "scene.new"
    fixture.findings = [("transcript-mismatch", MAIN_SCENE + ".meta")]


def id_mismatch(fixture: Fixture) -> None:
    fixture.entry(PROJECT_FILE).request_id = "5"
    fixture.findings = [("transcript-mismatch", PROJECT_FILE)]


def client_mismatch(fixture: Fixture) -> None:
    fixture.entry(BALL_SCRIPT).client = "other-agent"
    fixture.findings = [("transcript-mismatch", BALL_SCRIPT)]


def encode_line(line: Line, numbers: dict[str, int]) -> bytes:
    if line.record is None:
        return line.raw
    record = dict(line.record)
    if line.answers:
        record["requestLine"] = numbers[line.answers]
    separators = (",", ":") if line.minified else (", ", ": ")
    return json.dumps(record, ensure_ascii=False, separators=separators).encode("utf-8")


def torn_referenced_line(fixture: Fixture) -> None:
    index = fixture.index("script")
    numbers = {line.label: number for number, line in enumerate(fixture.lines, 1)}
    fixture.lines[index] = Line("script", raw=encode_line(fixture.lines[index], numbers)[:60])
    fixture.findings = [("torn-line", BALL_SCRIPT)]


def torn_unreferenced_line(fixture: Fixture) -> None:
    torn = b'{"type": "request", "time": "2026-01-01T00:00:00.000Z", "client": "engine-mcp", "id": 9, "meth'
    fixture.lines.insert(fixture.index("open"), Line("torn", raw=torn))
    fixture.warnings = [("torn-line", TRANSCRIPT_FILE)]


def dangling_entry(fixture: Fixture) -> None:
    fixture.entries += [Entry("Assets/Materials/Gone.material", "asset.create", 5, BRIDGE, "red", recorded=b"gone"),
                        Entry("Assets/Materials/red.material", "asset.create", 5, BRIDGE, "red",
                              recorded=fixture.files[RED_MATERIAL])]
    fixture.findings = [("entry-dangling", "Assets/Materials/Gone.material"),
                        ("entry-dangling", "Assets/Materials/red.material")]


def deleted_through_transcript(fixture: Fixture) -> None:
    draft, paddle, kept, late = ("Assets/Scripts/Draft.luau", "Assets/Scripts/Paddle.luau",
                                 "Assets/Materials/Kept.material", "Assets/Scripts/Late.luau")
    temp, spare, hand = "Assets/Old/Temp.material", "Assets/Scripts/Spare.luau", "Assets/Scripts/Hand.luau"
    not_found = {"code": -32001, "errorCode": "NotFound", "detail": "no asset at 'Assets/Materials/Kept.material'"}
    fixture.lines += [
        # Another bridge session after the upgrade.
        *call("draft", 2, "script.write", {"path": draft, "source": DRAFT_SOURCE}, "path Assets/Scripts/Draft.luau"),
        *call("move", 3, "asset.move", {"asset": draft, "path": paddle}, "asset 0a5e000000000005 Script"),
        *call("kept", 4, "asset.create", {"type": "material", "path": kept}, "asset 0a5e000000000006 Material"),
        *call("keep", 5, "asset.delete", {"asset": kept}, "error -32001 NotFound", error=not_found),
        *call("early", 6, "asset.delete", {"asset": late}, "asset 0a5e000000000007 Script"),
        *call("late", 7, "script.write", {"path": late, "source": LATE_SOURCE}, "path Assets/Scripts/Late.luau"),
        # Temp.material and its .meta were removed by hand; asset.delete then removed the folder, which held no files.
        *call("temp", 8, "asset.create", {"type": "material", "path": temp}, "asset 0a5e000000000008 Material"),
        *call("folder", 9, "asset.delete", {"asset": "Assets/Old"}, "removedFolders 1"),
        # Spare.luau was removed by hand; a hand-made Hand.luau (no entry, so the move recorded nothing) was then moved
        # to its path, and that file removed by hand too: the move names Spare.luau only as its destination.
        *call("spare", 10, "script.write", {"path": spare, "source": SPARE_SOURCE}, "path Assets/Scripts/Spare.luau"),
        *call("rename", 11, "asset.move", {"asset": hand, "path": spare}, "asset 0a5e000000000009 Script"),
    ]
    fixture.files[paddle] = DRAFT_SOURCE.encode("utf-8")
    fixture.files[paddle + ".meta"] = canonical(meta_document("0a5e000000000005", "Script"))
    fixture.entries += [
        # Deleted by the edit.batch op of line "tidy" (the file, then its source): passes.
        Entry(OLD_MATERIAL, "asset.create", 6, BRIDGE, "old", recorded=canonical(material_document())),
        Entry(OLD_MATERIAL + ".meta", "asset.create", 6, BRIDGE, "old",
              recorded=canonical(meta_document("0a5e000000000004", "Material"))),
        # Moved by a top-level asset.move: passes; its destination is recorded under the move.
        Entry(draft, "script.write", 2, BRIDGE, "draft", recorded=DRAFT_SOURCE.encode("utf-8")),
        Entry(draft + ".meta", "script.write", 2, BRIDGE, "draft",
              recorded=canonical(meta_document("0a5e000000000005", "Script"))),
        Entry(paddle, "asset.move", 3, BRIDGE, "move"),
        Entry(paddle + ".meta", "asset.move", 3, BRIDGE, "move"),
        # Its asset.delete failed, so nothing excuses the missing file.
        Entry(kept, "asset.create", 4, BRIDGE, "kept", recorded=canonical(material_document())),
        # Its only asset.delete precedes the write the entry records.
        Entry(late, "script.write", 7, BRIDGE, "late", recorded=LATE_SOURCE.encode("utf-8")),
        # Below a folder that asset.delete removed: an empty-folder delete excuses no file entry.
        Entry(temp, "asset.create", 8, BRIDGE, "temp", recorded=canonical(material_document())),
        Entry(temp + ".meta", "asset.create", 8, BRIDGE, "temp",
              recorded=canonical(meta_document("0a5e000000000008", "Material"))),
        # Named only as a later asset.move's destination: a move removes only its source.
        Entry(spare, "script.write", 10, BRIDGE, "spare", recorded=SPARE_SOURCE.encode("utf-8")),
        Entry(spare + ".meta", "script.write", 10, BRIDGE, "spare",
              recorded=canonical(meta_document("0a5e000000000009", "Script"))),
    ]
    fixture.findings = [("entry-dangling", kept), ("entry-dangling", temp), ("entry-dangling", temp + ".meta"),
                        ("entry-dangling", late), ("entry-dangling", spare), ("entry-dangling", spare + ".meta")]


def upgrade_without_transcript(fixture: Fixture) -> None:
    # The upgrade was driven through the bridge (client engine-mcp), so no cli project.upgrade request is in the
    # transcript and the legacy cli entry has nothing to stand on.
    index = fixture.index("upgrade")
    fixture.lines[index:index + 2] = call("upgrade", 7, "project.upgrade", {}, "changedFiles 1")
    entry = fixture.entry(BALL_SCRIPT + ".meta")
    entry.request_id, entry.client = 7, BRIDGE
    fixture.findings = [("transcript-line-missing", RED_MATERIAL + ".meta")]


def token_in_transcript(fixture: Fixture) -> None:
    hello = request("hello", 1, "session.hello", {"token": "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
                                                  "client": {"name": BRIDGE, "version": "1.0.0"}})
    fixture.lines.insert(fixture.index("open"), hello)
    fixture.findings = [("transcript-invalid", TRANSCRIPT_FILE)]


def malformed_line(fixture: Fixture) -> None:
    fixture.lines.insert(fixture.index("open"), Line("note", {"type": "note", "text": "moved the paddle by hand"}))
    fixture.findings = [("transcript-invalid", TRANSCRIPT_FILE)]


def invalid_provenance(fixture: Fixture) -> None:
    fixture.unsorted_entries = True
    fixture.exit_code = EXIT_FINDINGS
    fixture.findings = [("provenance-invalid", PROVENANCE_FILE)]


def missing_provenance(fixture: Fixture) -> None:
    fixture.write_provenance = False
    fixture.findings = [("provenance-missing", PROVENANCE_FILE)]


def missing_transcript(fixture: Fixture) -> None:
    fixture.write_transcript = False
    fixture.findings = [("transcript-missing", TRANSCRIPT_FILE)]


def missing_project(fixture: Fixture) -> None:
    fixture.entry(PROJECT_FILE).recorded = fixture.files.pop(PROJECT_FILE)
    fixture.findings = [("project-missing", "")]


# (name, description, mutation); the exit code follows from the findings unless the mutation sets it.
CASES: list[tuple[str, str, Callable[[Fixture], None]]] = [
    ("HashMismatch", "Red.material re-indented by hand after its write was recorded.", hash_mismatch),
    ("EntryMissing", "Main.scene.meta has no provenance entry.", entry_missing),
    ("UiWrite", "Red.material's entry was written by the editor's UI.", ui_write),
    ("BatchWrite", "Main.scene's entry comes from Editor --batch (client batch), which writes no transcript line.",
     batch_write),
    ("LineOutOfRange", "Ball.luau's entry names line 99 of a shorter transcript.", line_out_of_range),
    ("LineNotRequest", "Ball.luau's entry names its request's response line.", line_not_request),
    ("MethodMismatch", "Main.scene.meta's entry names scene.new; its line is scene.save.", method_mismatch),
    ("IdMismatch", "Mini.eproj's entry records the request id \"5\" (a string); its line has 5 (an integer).",
     id_mismatch),
    ("ClientMismatch", "Ball.luau's entry names another client than its line.", client_mismatch),
    ("TornReferencedLine", "script.write's request line is torn, and Ball.luau's entry names it.",
     torn_referenced_line),
    ("TornUnreferencedLine", "A torn line the next bridge completed with a newline; no entry names it (a warning).",
     torn_unreferenced_line),
    ("DanglingEntry", "Entries for a file that does not exist and for one whose name differs only in letter case.",
     dangling_entry),
    ("DeletedThroughTranscript", "Dangling entries: deleted by an edit.batch op and moved by asset.move (both pass), "
     "one whose asset.delete failed, one whose only asset.delete precedes its write, two below a folder an "
     "asset.delete removed and two named only as an asset.move's destination (all fail).", deleted_through_transcript),
    ("UpgradeWithoutTranscript", "A legacy cli project.upgrade entry without a line, and no cli project.upgrade "
     "request in the transcript (the upgrade ran through the bridge).", upgrade_without_transcript),
    ("TokenInTranscript", "A session.hello request line, whose token must never be committed.", token_in_transcript),
    ("MalformedLine", "A JSON object line that is neither a request nor a response line.", malformed_line),
    ("InvalidProvenance", "Provenance.json's entries are not sorted by path.", invalid_provenance),
    ("MissingProvenance", "No Automation/Provenance.json.", missing_provenance),
    ("MissingTranscript", "No Automation/BuildLog.jsonl.", missing_transcript),
    ("MissingProject", "No root .eproj, so the directory is not a game project.", missing_project),
]
MISSING_FILE_CODES = ("project-missing", "provenance-missing", "transcript-missing")


def fixtures() -> list[Fixture]:
    result = [clean()]
    for name, description, mutate in CASES:
        fixture = clean()
        fixture.name, fixture.description = name, description
        mutate(fixture)
        if any(code in MISSING_FILE_CODES for code, _ in fixture.findings):
            fixture.exit_code = EXIT_MISSING_FILE
        elif fixture.findings:
            fixture.exit_code = EXIT_FINDINGS
        result.append(fixture)
    return result


# ---------------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------------


def render(fixture: Fixture) -> dict[str, bytes]:
    """`fixture`'s files below Tests/Data/Provenance/<Case>/."""
    labels = [line.label for line in fixture.lines]
    if len(set(labels)) != len(labels):
        raise ValueError(f"{fixture.name}: a transcript label is used twice")
    numbers = {label: number for number, label in enumerate(labels, 1)}
    outputs = {f"Project/{path}": data for path, data in fixture.files.items()}
    outputs.update({f"Project/{path}": data for path, data in fixture.edits.items()})
    if fixture.write_transcript:
        outputs[f"Project/{TRANSCRIPT_FILE}"] = b"".join(
            encode_line(line, numbers) + (b"\n" if line.terminated else b"") for line in fixture.lines)
    if fixture.write_provenance:
        entries = sorted(fixture.entries, key=lambda entry: entry.path.encode("utf-8"))
        if fixture.unsorted_entries:
            entries[0], entries[1] = entries[1], entries[0]
        outputs[f"Project/{PROVENANCE_FILE}"] = canonical({"Format": "Provenance", "Version": 1, "Entries": [
            provenance_entry(fixture, entry, numbers) for entry in entries]})
    outputs["Expected.json"] = canonical({
        "Format": EXPECTED_FORMAT, "Version": 1, "ExitCode": fixture.exit_code,
        "Findings": [{"Code": code, "Path": path} for code, path in fixture.findings],
        "Warnings": [{"Code": code, "Path": path} for code, path in fixture.warnings]})
    return outputs


def provenance_entry(fixture: Fixture, entry: Entry, numbers: dict[str, int]) -> dict[str, Any]:
    recorded = entry.recorded if entry.recorded is not None else fixture.files.get(entry.path)
    if recorded is None:
        raise ValueError(f"{fixture.name}: the entry of {entry.path} names no file and no recorded bytes")
    if isinstance(entry.line, str):
        if entry.line not in numbers:
            raise ValueError(f"{fixture.name}: the entry of {entry.path} names the unknown line {entry.line}")
        line: int | None = numbers[entry.line]
    else:
        line = entry.line
    return {"Path": entry.path, "XXH64": xxh64_hex(recorded), "Method": entry.method, "RequestId": entry.request_id,
            "Client": entry.client, "TranscriptLine": line}


def readme(cases: list[Fixture]) -> bytes:
    rows = "\n".join(f"| `{fixture.name}` | {fixture.description} | {fixture.exit_code} |" for fixture in cases)
    return (
        "# Provenance audit fixtures\n\n"
        "Generated by `Tests/Data/Generate/MakeProvenanceFixtures.py` (Docs/Decisions/0026-m16-contract.md decision "
        "6); do not edit by hand. `<Case>/Project/` is a small game project with its committed\n"
        "`Automation/Provenance.json` and `Automation/BuildLog.jsonl`, and `<Case>/Expected.json` lists the findings "
        "and warnings that\n`python Scripts/AuditProvenance.py --project <Case>/Project --files disk` reports. "
        "`AuditProvenance.py --self-test` checks every case.\n\n"
        "| Case | Defect | Exit code |\n|---|---|---|\n" + rows + "\n").encode("utf-8")


def outputs() -> dict[str, bytes]:
    cases = fixtures()
    result = {"README.md": readme(cases)}
    for fixture in cases:
        result.update({f"{fixture.name}/{path}": data for path, data in render(fixture).items()})
    return result


def owned_files(root: Path) -> set[str]:
    if not root.is_dir():
        return set()
    return {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="Exit codes: 0 fixtures generated or verified, 1 stale or extra files or "
                                            "an I/O failure, 2 usage error.")
    parser.add_argument("--check", action="store_true", help="compare with the committed files; write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report")
    arguments = parser.parse_args()
    changed: list[str] = []
    extra: list[str] = []
    label = OUTPUT.relative_to(ROOT).as_posix()
    try:
        files = outputs()
        for relative, data in sorted(files.items()):
            path = OUTPUT / relative
            if not path.is_file() or path.read_bytes() != data:
                changed.append(f"{label}/{relative}")
                if not arguments.check:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
        for relative in sorted(owned_files(OUTPUT) - files.keys()):
            extra.append(f"{label}/{relative}")
            if not arguments.check:
                (OUTPUT / relative).unlink()
        if not arguments.check and OUTPUT.is_dir():
            for directory in sorted((entry for entry in OUTPUT.rglob("*") if entry.is_dir()),
                                    key=lambda entry: len(entry.parts), reverse=True):
                if not any(directory.iterdir()):
                    directory.rmdir()
    except (OSError, ValueError) as error:
        if arguments.json:
            print(json.dumps({"ok": False, "error": str(error)}))
        else:
            print(f"Provenance fixtures: {error}", file=sys.stderr)
        return 1
    success = not arguments.check or not (changed or extra)
    if arguments.json:
        print(json.dumps({"ok": success, "changed": changed, "extra": extra}))
    else:
        verb = "stale" if arguments.check else "written"
        removed = "extra" if arguments.check else "removed"
        print(f"Provenance fixtures: {len(changed)} {verb} file(s), {len(extra)} {removed} file(s)")
        for name in changed + extra:
            print(f"  {name}")
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
