#!/usr/bin/env python3
"""Generate the seeded coverage-gap fixtures of the FeatureTest gates (Docs/Decisions/0022-m14-contract.md decision 6).

It owns every file under Tests/Data/CoverageGaps except the directories Gate6/, Gate7/ and Gate8/, which belong to
the validation and description gates' owner (stream D) and are authored by hand; any other file there that it does
not write is reported as extra and, without --check, removed:
- Gate<g>/<Mode>/FeatureTest-<Mode>.json and Expected.json for each per-mode gate g of 1, 1b, 2, 3, 4 and 9 and each
  mode of Editor, Release and Dist: a complete TestRunResult JSON (TestRunResultToJson's members) with a small
  synthetic counter set, and {"Format": "CoverageGap", "Version": 1, "Gate", "Mode", "Missing"}.
- Gate5/Methods.json (a --dump-reference method catalogue), CalledMethods.txt (the calls of the Python suites, one per
  line, in call order) and Expected.json ({"Format": "CoverageGap", "Version": 1, "Gate": "5", "Missing"}).
- Merge/<Case>/FeatureTest-{Editor,Release,Dist}.json and Expected.json ({"Format": "CoverageGap", "Version": 1,
  "Failures"}) for the merge cases of decision 6: Covered, Passed, SkippedCase, Unevaluated, Missing, Extra, Mismatch
  and Uncovered.

Every report is one shared run (the Api.Physics suite in Scripting.scene, one passing case) plus its counters. A
report's "coverage.gates" is computed here from its "coverage.counters" by the rules of EvaluateCoverageGates
(Engine/Source/Engine/Testing/CoverageGates.cpp), so the invariant of decision 6 (a report's gates equal the evaluation
of its counters, which the C++ acceptance test asserts) holds by construction. "passed" follows the FeatureTestRunner:
no case row failed, every suite passed and, when the gates were evaluated, every gate passed. A per-mode gap's
Expected.json is the evaluation of its report (Missing is the failing gate's list); generation stops with an error
unless the report fails exactly its own gate and passes the other five. Gate 5's Missing is every catalogued method
never called.

The exceptions, each deliberate:
- Merge/Unevaluated's Editor report is a run that did not evaluate its gates (a filtered run: FinishCoverage writes
  MakeUnevaluatedCoverageGates and leaves "passed" to the rows), so its gates are not the evaluation of its counters
  and it still passes; the merge's gate rule alone must fail it.
- Merge/Extra's Release report lists an Editor-only counter, which a real Release run drops; the evaluation ignores it
  (a counter outside the report's mode never counts) and the merge's extra rule must name it.
- The merge cases' Failures are written as data: they are what Scripts/Lib/coverage.py merge_coverage must report.

Run from anywhere: without arguments it writes the files; `--check` compares a fresh generation with the committed
files and writes nothing; `--json` prints a machine-readable report on stdout ({"status", "check", "differences",
"extra", "errors"}) and the human-readable lines on stderr.

Exit codes: 0 success, 1 --check found a difference or an extra file, or a file could not be read, written or
removed, 2 usage error.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path
from typing import Any

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "CoverageGaps"
LABEL = "Tests/Data/CoverageGaps"
# The directories under OUTPUT_DIRECTORY that another owner writes; this generator never reads, reports or removes them.
FOREIGN_DIRECTORIES = frozenset({"Gate6", "Gate7", "Gate8"})

MODES = ("Editor", "Release", "Dist")
EDITOR_ONLY = ("Editor",)
PER_MODE_GATES = ("1", "1b", "2", "3", "4", "9")
NO_COUNTERS = "<no counters>"

# ---------------------------------------------------------------------------------------------------------------------
# Canonical JSON (the engine's JsonWriter form: tabs, LF, scalar arrays on one line, objects expanded, final newline)
# ---------------------------------------------------------------------------------------------------------------------


def format_scalar(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, int):
        return str(value)
    raise TypeError(f"{value!r}: the coverage fixtures hold only strings, integers, booleans and null")


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


def canonical(document: dict[str, Any]) -> bytes:
    return (write_json(document) + "\n").encode("utf-8")


def byte_sorted(values: Any) -> list[str]:
    """`values` sorted byte-wise by their UTF-8 form, as every id list of a report is."""
    return sorted(values, key=lambda value: value.encode("utf-8"))


# ---------------------------------------------------------------------------------------------------------------------
# Counters and the per-mode gate evaluation (EvaluateCoverageGates)
# ---------------------------------------------------------------------------------------------------------------------

# Each counter kind with its gate and id prefix (the table of CoverageGates.h).
KINDS: dict[str, tuple[str, str]] = {
    "component": ("1", "component:"),
    "field": ("1", "field:"),
    "enum-value": ("1b", "enum:"),
    "function": ("2", "api:"),
    "method": ("2", "api:"),
    "operator": ("2", "api:"),
    "constructor": ("2", "api:"),
    "property-read": ("2", "api:"),
    "property-write": ("2", "api:"),
    "argument-value": ("2", "api:"),
    "callback": ("3", "callback:"),
    "proxy-read": ("4", "proxy:"),
    "proxy-write": ("4", "proxy:"),
    "format": ("9", "format:"),
}
# Components, enum values and formats exist in every mode (ADR 0022 decision 2).
ALL_MODE_KINDS = frozenset({"component", "enum-value", "format"})


@dataclasses.dataclass(frozen=True)
class Counter:
    """One TestCoverageCounter without its id (a CounterSet maps the id to it)."""

    kind: str
    modes: tuple[str, ...]
    count: int


CounterSet = dict[str, Counter]


def default_kind(identifier: str) -> str:
    """The kind an id of the table's shape implies: a script API id is a function unless it names a property access or
    an argument value; a proxy id ends with its access."""
    prefix, _, member = identifier.partition(":")
    if prefix == "api":
        if "/argument" in member:
            return "argument-value"
        if member.endswith("/read"):
            return "property-read"
        if member.endswith("/write"):
            return "property-write"
        return "function"
    if prefix == "proxy":
        if member.endswith("/read"):
            return "proxy-read"
        if member.endswith("/write"):
            return "proxy-write"
        raise ValueError(f"proxy counter '{identifier}' must end with /read or /write")
    kinds = {"component": "component", "field": "field", "enum": "enum-value", "callback": "callback",
             "format": "format"}
    if prefix not in kinds:
        raise ValueError(f"counter '{identifier}' has no known id prefix")
    return kinds[prefix]


def counter(identifier: str, count: int, modes: tuple[str, ...] = MODES,
            kind: str | None = None) -> tuple[str, Counter]:
    """An (id, Counter) pair; `kind` defaults to default_kind(identifier)."""
    return identifier, Counter(kind or default_kind(identifier), modes, count)


def validate_counter(identifier: str, item: Counter) -> None:
    """ValidateCounter's rules: a known kind, its prefix and an entry name, run modes listed once each, the Editor among
    them, and all three for a component, enum value or format."""
    if item.kind not in KINDS:
        raise ValueError(f"coverage counter '{identifier}' has the unknown kind '{item.kind}'")
    prefix = KINDS[item.kind][1]
    if not identifier.startswith(prefix) or identifier == prefix:
        raise ValueError(f"coverage counter '{identifier}' of kind '{item.kind}' must start with '{prefix}'")
    if not item.modes or len(set(item.modes)) != len(item.modes) or any(mode not in MODES for mode in item.modes):
        raise ValueError(f"coverage counter '{identifier}' must list known run modes, each once")
    if "Editor" not in item.modes:
        raise ValueError(f"coverage counter '{identifier}' does not exist in the Editor")
    if item.kind in ALL_MODE_KINDS and len(item.modes) != len(MODES):
        raise ValueError(f"coverage counter '{identifier}' of kind '{item.kind}' must exist in every mode")


def evaluate_gates(counters: CounterSet, mode: str) -> list[dict[str, Any]]:
    """EvaluateCoverageGates for a run in `mode`: per gate of PER_MODE_GATES, in that order, the zero-count counters of
    its kinds whose modes contain `mode`, sorted; a gate without any counter available in `mode` fails with the single
    entry "<no counters>"; counters of other modes are ignored."""
    for identifier, item in counters.items():
        validate_counter(identifier, item)
    gates: list[dict[str, Any]] = []
    for gate in PER_MODE_GATES:
        available = [identifier for identifier, item in counters.items()
                     if KINDS[item.kind][0] == gate and mode in item.modes]
        missing = byte_sorted(identifier for identifier in available if counters[identifier].count == 0)
        if not available:
            missing = [NO_COUNTERS]
        gates.append({"gate": gate, "evaluated": True, "passed": not missing, "missing": missing})
    return gates


def unevaluated_gates() -> list[dict[str, Any]]:
    """MakeUnevaluatedCoverageGates: what a filtered, recording or cancelled run reports. Never a pass."""
    return [{"gate": gate, "evaluated": False, "passed": False, "missing": []} for gate in PER_MODE_GATES]


def counter_set(base: CounterSet, changes: list[tuple[str, Counter]] | None = None,
                removed: list[str] | None = None) -> CounterSet:
    """`base` with `changes` added (or replacing the counter of the same id) and the ids of `removed` left out."""
    result = dict(base)
    result.update(changes or [])
    for identifier in removed or []:
        if identifier not in result:
            raise ValueError(f"cannot remove the counter '{identifier}': it is not in the set")
        del result[identifier]
    return result


# ---------------------------------------------------------------------------------------------------------------------
# Reports (TestRunResultToJson)
# ---------------------------------------------------------------------------------------------------------------------

SUITE = "Api.Physics"
SCRIPT = "Assets/Tests/Api/Physics.test.luau"
SCENE = "Assets/Scenes/Scripting.scene"
SCENE_ID = "5d2c8a7e41b09f63"
FINAL_STATE_HASH = "93b4c2f07a1e6d58"
TICKS = 4
# Each mode's registry differs by design, so each has its own fingerprint (the merge never compares them).
FINGERPRINTS = {"Editor": "a1f3c9e2d4b60718", "Release": "b2e4d0f3c5a71829", "Dist": "c3d5e1a4b6f8293a"}


def case_row(name: str = "raycasts", status: str = "passed", message: str = "", line: int = 2,
             failed: bool = False) -> dict[str, Any]:
    """A case row of the Api.Physics suite; a `failed` row carries `message` as its one failure."""
    failures = [{"message": message, "file": SCRIPT, "line": line, "jsonPointer": ""}] if failed else []
    return {"suite": SUITE, "case": name, "status": status, "message": message, "file": SCRIPT, "line": line,
            "jsonPointer": "", "ticks": TICKS, "failures": failures, "quitCode": 0, "quitExpected": False}


def row_passes(row: dict[str, Any]) -> bool:
    """The FeatureTestRunner's Passed(row): no failure, and passed, skipped or an expected quit."""
    expected_quit = row["status"] == "quit" and row["quitExpected"]
    return not row["failures"] and (row["status"] in ("passed", "skipped") or expected_quit)


def make_report(mode: str, counters: CounterSet, cases: list[dict[str, Any]] | None = None,
                evaluated: bool = True) -> dict[str, Any]:
    """A complete TestRunResult of `mode`. Its gates are the evaluation of `counters`, or, for a run that did not
    evaluate them (`evaluated` false), the unevaluated gates; "passed" is computed as the runner computes it."""
    rows = cases if cases is not None else [case_row()]
    suite = {"suite": SUITE, "scene": SCENE, "sceneId": SCENE_ID, "script": SCRIPT, "mode": mode, "ticks": TICKS,
             "breaks": 0, "scriptErrors": 0, "finalStateHash": FINAL_STATE_HASH,
             "passed": all(row_passes(row) for row in rows)}
    gates = evaluate_gates(counters, mode) if evaluated else unevaluated_gates()
    cancelled = False
    passed = not cancelled and all(row_passes(row) for row in rows) and suite["passed"]
    if evaluated:
        passed = passed and all(gate["passed"] for gate in gates)
    ordered = [{"id": identifier, "kind": counters[identifier].kind, "modes": list(counters[identifier].modes),
                "count": counters[identifier].count} for identifier in byte_sorted(counters)]
    return {
        "mode": mode, "passed": passed, "cancelled": cancelled, "cases": rows, "suites": [suite],
        "coverage": {"registryFingerprint": FINGERPRINTS[mode], "counters": ordered, "gates": gates},
        "jsonPath": f"bin/TestResults/FeatureTest-{mode}.json", "junitPath": f"bin/TestResults/FeatureTest-{mode}.xml",
        "recordingPath": "",
    }


def gap_document(**members: Any) -> dict[str, Any]:
    return {"Format": "CoverageGap", "Version": 1, **members}


# Every report's counters start from this set: one covered entry of every per-mode gate.
BASE_COUNTERS: CounterSet = dict([
    counter("api:Physics.Raycast", 3),
    counter("callback:Behaviour.OnStart", 1),
    counter("component:Camera", 2),
    counter("enum:ProjectionType/Orthographic", 1),
    counter("field:Camera/Projection", 1),
    counter("format:Texture/.png", 1),
    counter("proxy:Camera.Primary/read", 4),
])

# ---------------------------------------------------------------------------------------------------------------------
# Per-mode seeded gaps: the counters each Gate<g>/<Mode> report adds to BASE_COUNTERS
# ---------------------------------------------------------------------------------------------------------------------

# Each Dist gap is an entry only the editor run covered; the Editor gaps of gates 1 (a field), 2, 3 and 4 include an
# Editor-only entry (decision 6).
GATE_GAPS: dict[str, dict[str, list[tuple[str, Counter]]]] = {
    "1": {
        "Editor": [counter("component:Text", 0), counter("field:Camera/EditorPreview", 0, EDITOR_ONLY),
                   counter("field:Camera/VerticalFov", 2)],
        "Release": [counter("component:Text", 1), counter("field:Camera/VerticalFov", 0)],
        "Dist": [counter("field:Text/Alignment", 0)],
    },
    "1b": {
        "Editor": [counter("enum:ProjectionType/Perspective", 0)],
        "Release": [counter("enum:Tonemapper/Aces", 1), counter("enum:Tonemapper/AgX", 0),
                    counter("enum:Tonemapper/Linear", 0)],
        "Dist": [counter("enum:BodyType/Kinematic", 0)],
    },
    "2": {
        "Editor": [counter("api:Input.SetCursorMode", 1),
                   counter("api:Input.SetCursorMode/argument1/CursorMode/Locked", 0),
                   counter("api:Test.ReloadScript", 0, EDITOR_ONLY)],
        "Release": [counter("api:Entity.Name/read", 5), counter("api:Entity.Name/write", 0)],
        "Dist": [counter("api:Physics.Raycast/argument3/QueryMode/All", 0)],
    },
    "3": {
        "Editor": [counter("callback:Behaviour.OnHotReload", 0, EDITOR_ONLY)],
        "Release": [counter("callback:Behaviour.OnTriggerExit", 0)],
        "Dist": [counter("callback:Behaviour.OnCollisionExit", 0)],
    },
    "4": {
        "Editor": [counter("proxy:Camera.EditorPreview/read", 0, EDITOR_ONLY),
                   counter("proxy:Camera.Primary/write", 0)],
        "Release": [counter("proxy:RigidBody.Mass/read", 2), counter("proxy:RigidBody.Mass/write", 0)],
        "Dist": [counter("proxy:AudioSource.Volume/read", 0)],
    },
    "9": {
        "Editor": [counter("format:Font/.otf", 0)],
        "Release": [counter("format:Audio/.mp3", 0), counter("format:Texture/.jpeg", 0),
                    counter("format:Texture/.jpg", 1)],
        "Dist": [counter("format:Environment/.hdr", 0)],
    },
}


def gate_gap_files() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for gate, modes in GATE_GAPS.items():
        for mode in MODES:
            report = make_report(mode, counter_set(BASE_COUNTERS, modes[mode]))
            failing = [result for result in report["coverage"]["gates"] if not result["passed"]]
            if [result["gate"] for result in failing] != [gate]:
                raise ValueError(f"Gate{gate}/{mode}: the report must fail gate {gate} alone, not "
                                 f"{[result['gate'] for result in failing]}")
            files[f"Gate{gate}/{mode}/FeatureTest-{mode}.json"] = canonical(report)
            files[f"Gate{gate}/{mode}/Expected.json"] = canonical(
                gap_document(Gate=gate, Mode=mode, Missing=failing[0]["missing"]))
    return files


# ---------------------------------------------------------------------------------------------------------------------
# Gate 5 (automation methods)
# ---------------------------------------------------------------------------------------------------------------------

GATE5_METHODS = ["entity.create", "entity.destroy", "physics.raycast", "play.start", "scene.save", "scene.tree"]
# The calls in call order, with a repeated call and a debug.* test hook (the catalogue leaves those out; never counted).
GATE5_CALLS = ["entity.create", "play.start", "entity.create", "scene.tree", "debug.stall"]


def gate5_files() -> dict[str, bytes]:
    catalogue = {"Format": "MethodCatalog", "Version": 1, "ProtocolVersion": "1.0", "Methods": [
        {"name": name, "description": f"The {name} method of the seeded gap."} for name in byte_sorted(GATE5_METHODS)]}
    missing = byte_sorted(set(GATE5_METHODS) - set(GATE5_CALLS))
    return {
        "Gate5/Methods.json": canonical(catalogue),
        "Gate5/CalledMethods.txt": "".join(f"{call}\n" for call in GATE5_CALLS).encode("utf-8"),
        "Gate5/Expected.json": canonical(gap_document(Gate="5", Missing=missing)),
    }


# ---------------------------------------------------------------------------------------------------------------------
# The cross-mode merge cases
# ---------------------------------------------------------------------------------------------------------------------

# Every merge report adds a covered per-frame callback; the Editor report also lists two Editor-only entries.
MERGE_COUNTERS = counter_set(BASE_COUNTERS, [counter("callback:Behaviour.OnUpdate", 6)])
MERGE_EDITOR_COUNTERS = counter_set(MERGE_COUNTERS, [counter("api:Test.ReloadScript", 1, EDITOR_ONLY),
                                                     counter("callback:Behaviour.OnHotReload", 1, EDITOR_ONLY)])


@dataclasses.dataclass(frozen=True)
class MergeCase:
    """One Merge/<Case>: per mode, the counters added or replaced, the ids removed and the case rows (default: the one
    passing row), the modes whose run did not evaluate its gates, and the failures merge_coverage reports."""

    name: str
    failures: list[str]
    changes: dict[str, list[tuple[str, Counter]]] = dataclasses.field(default_factory=dict)
    removed: dict[str, list[str]] = dataclasses.field(default_factory=dict)
    cases: dict[str, list[dict[str, Any]]] = dataclasses.field(default_factory=dict)
    unevaluated: tuple[str, ...] = ()


MERGE_CASES = [
    MergeCase("Covered", []),
    MergeCase("Passed", ["merge: passed: Release"],
              cases={"Release": [case_row(status="failed", message="expected 3, got 2", line=3, failed=True)]}),
    MergeCase("SkippedCase", ["merge: skipped: Editor Api.Physics/sweeps"],
              cases={"Editor": [case_row(), case_row("sweeps", "skipped", "not on this platform", 3)]}),
    # Override: the Editor run did not evaluate its gates (a filtered run), so its gates are the unevaluated ones rather
    # than the evaluation of its counters, and it still passes: only the gate rule fails, once per gate.
    MergeCase("Unevaluated", ["merge: gate: Editor 1", "merge: gate: Editor 1b", "merge: gate: Editor 2",
                              "merge: gate: Editor 3", "merge: gate: Editor 4", "merge: gate: Editor 9"],
              unevaluated=("Editor",)),
    MergeCase("Missing", ["merge: missing: Release callback:Behaviour.OnUpdate"],
              removed={"Release": ["callback:Behaviour.OnUpdate"]}),
    # The Release report keeps an Editor-only counter, which a real Release run drops; its evaluation ignores it.
    MergeCase("Extra", ["merge: extra: Release callback:Behaviour.OnHotReload",
                        "merge: extra: Dist api:Debug.DrawLine"],
              changes={"Release": [counter("callback:Behaviour.OnHotReload", 0, EDITOR_ONLY)],
                       "Dist": [counter("api:Debug.DrawLine", 1)]}),
    MergeCase("Mismatch", ["merge: mismatch: Release api:Physics.Raycast",
                           "merge: mismatch: Dist field:Camera/Projection"],
              changes={"Release": [counter("api:Physics.Raycast", 3, kind="method")],
                       "Dist": [counter("field:Camera/Projection", 1, ("Editor", "Dist"))]}),
    MergeCase("Uncovered", ["merge: passed: Editor", "merge: passed: Release", "merge: passed: Dist",
                            "merge: gate: Editor 2", "merge: gate: Editor 3", "merge: gate: Release 2",
                            "merge: gate: Dist 2", "merge: uncovered: api:Physics.Overlap",
                            "merge: uncovered: callback:Behaviour.OnHotReload"],
              changes={"Editor": [counter("api:Physics.Overlap", 0), counter("api:Physics.SphereCast", 0),
                                  counter("callback:Behaviour.OnHotReload", 0, EDITOR_ONLY)],
                       "Release": [counter("api:Physics.Overlap", 0), counter("api:Physics.SphereCast", 0)],
                       "Dist": [counter("api:Physics.Overlap", 0), counter("api:Physics.SphereCast", 2)]}),
]


def merge_files() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for case in MERGE_CASES:
        for mode in MODES:
            base = MERGE_EDITOR_COUNTERS if mode == "Editor" else MERGE_COUNTERS
            counters = counter_set(base, case.changes.get(mode), case.removed.get(mode))
            report = make_report(mode, counters, case.cases.get(mode), evaluated=mode not in case.unevaluated)
            files[f"Merge/{case.name}/FeatureTest-{mode}.json"] = canonical(report)
        files[f"Merge/{case.name}/Expected.json"] = canonical(gap_document(Failures=case.failures))
    return files


# ---------------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------------


def fixtures() -> dict[str, bytes]:
    """Every generated file, by its path relative to OUTPUT_DIRECTORY."""
    files: dict[str, bytes] = {}
    for part in (gate_gap_files(), gate5_files(), merge_files()):
        overlap = files.keys() & part.keys()
        if overlap:
            raise ValueError(f"generated twice: {byte_sorted(overlap)}")
        files.update(part)
    return files


def owned(path: Path) -> bool:
    """Whether `path` (under OUTPUT_DIRECTORY) is this generator's: anything outside the foreign directories."""
    return path.relative_to(OUTPUT_DIRECTORY).parts[0] not in FOREIGN_DIRECTORIES


def existing_files() -> set[str]:
    if not OUTPUT_DIRECTORY.is_dir():
        return set()
    return {path.relative_to(OUTPUT_DIRECTORY).as_posix() for path in OUTPUT_DIRECTORY.rglob("*")
            if path.is_file() and owned(path)}


@dataclasses.dataclass
class Outcome:
    """What generate did: the number of generated files, those that differed, the extra files and the errors (paths
    relative to the repository root)."""

    fixtures: int = 0
    differences: list[str] = dataclasses.field(default_factory=list)
    extra: list[str] = dataclasses.field(default_factory=list)
    errors: list[str] = dataclasses.field(default_factory=list)


def generate(check: bool) -> Outcome:
    """Writes (or, with `check`, compares) every fixture, then removes (or reports) every owned file it did not generate
    and, without `check`, every owned directory left empty."""
    outcome = Outcome()
    try:
        files = fixtures()
    except (TypeError, ValueError) as error:
        outcome.errors.append(f"generation: {error}")
        return outcome
    outcome.fixtures = len(files)
    for name in byte_sorted(files):
        path = OUTPUT_DIRECTORY / name
        try:
            current = path.read_bytes() if path.is_file() else None
            if current == files[name]:
                continue
            outcome.differences.append(f"{LABEL}/{name}")
            if not check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(files[name])
        except OSError as error:
            outcome.errors.append(f"{LABEL}/{name}: {error}")
    try:
        for name in byte_sorted(existing_files() - files.keys()):
            outcome.extra.append(f"{LABEL}/{name}")
            if not check:
                (OUTPUT_DIRECTORY / name).unlink()
        if not check and OUTPUT_DIRECTORY.is_dir():
            directories = [path for path in OUTPUT_DIRECTORY.rglob("*") if path.is_dir() and owned(path)]
            for directory in sorted(directories, key=lambda path: len(path.parts), reverse=True):
                if not any(directory.iterdir()):
                    directory.rmdir()
    except OSError as error:
        outcome.errors.append(f"{LABEL}: {error}")
    return outcome


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the seeded coverage-gap fixtures in Tests/Data/CoverageGaps (except Gate6 to Gate8).",
        epilog="Exit codes: 0 success, 1 --check found a difference or an extra file, or a file could not be read, "
               "written or removed, 2 usage error.",
    )
    parser.add_argument("--check", action="store_true", help="compare with the committed files and write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    outcome = generate(options.check)
    failed = bool(outcome.errors) or (options.check and bool(outcome.differences or outcome.extra))
    out = sys.stderr if options.json else sys.stdout
    changed = "differ" if options.check else "written"
    unknown = "extra" if options.check else "removed"
    for name in outcome.differences:
        print(f"  {name}: {'differs from a fresh generation' if options.check else 'written'}", file=out)
    for name in outcome.extra:
        print(f"  {name}: {unknown} (not generated)", file=out)
    for error in outcome.errors:
        print(f"  error: {error}", file=out)
    print(f"{'FAILED' if failed else 'passed'}: {outcome.fixtures} coverage-gap fixtures, {len(outcome.differences)} "
          f"{changed}, {len(outcome.extra)} {unknown}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check,
                    "differences": outcome.differences, "extra": outcome.extra, "errors": outcome.errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
