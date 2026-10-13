"""The FeatureTest coverage reports and the gates Scripts/Test.py evaluates (Docs/Architecture.md §15.5, §15.6;
Docs/Decisions/0022-m14-contract.md decisions 2, 3, 6 and 7).

A report is the JSON TestRunResult the FeatureTestRunner writes (bin/TestResults/FeatureTest-<mode>.json, or beside the
editor's --junit path): "mode", "passed", "cancelled", "cases", "suites" and "coverage" with "registryFingerprint",
"counters" ([{"id", "kind", "modes", "count"}], sorted by id) and "gates" ([{"gate", "evaluated", "passed",
"missing"}], gates 1, 1b, 2, 3, 4 and 9 in that order). The feature suite checks one editor report; the cross-mode merge
(run by M15's export stage) checks the Editor, Release and Dist reports together. Gate 5 (automation methods) compares
the editor's method catalogue with the methods the Python suites called.
"""

from __future__ import annotations

import dataclasses
import json
from pathlib import Path
from typing import Any

MODES = ("Editor", "Release", "Dist")
PER_MODE_GATES = ("1", "1b", "2", "3", "4", "9")


@dataclasses.dataclass(frozen=True)
class CoverageCounter:
    """One TestCoverageCounter of a report."""

    id: str
    kind: str
    modes: tuple[str, ...]
    count: int


@dataclasses.dataclass(frozen=True)
class GateResult:
    """One TestCoverageGateResult of a report."""

    gate: str
    evaluated: bool
    passed: bool
    missing: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class FeatureReport:
    """The parts of a FeatureTest report the gates read."""

    path: Path
    mode: str
    passed: bool
    skipped_cases: tuple[str, ...]  # "<suite>/<case>" of every case row with status "skipped"
    counters: tuple[CoverageCounter, ...]
    gates: tuple[GateResult, ...]


def _member(path: Path, owner: dict[str, Any], key: str, kind: type | tuple[type, ...], where: str) -> Any:
    """`owner[key]` checked against `kind` (bool is never accepted as an int); ValueError naming the file otherwise."""
    if key not in owner:
        raise ValueError(f"{path}: {where} has no '{key}'")
    value = owner[key]
    kinds = kind if isinstance(kind, tuple) else (kind,)
    if isinstance(value, bool) and bool not in kinds:
        raise ValueError(f"{path}: {where}.{key} must be {'/'.join(k.__name__ for k in kinds)}, not a boolean")
    if not isinstance(value, kinds):
        raise ValueError(f"{path}: {where}.{key} must be {'/'.join(k.__name__ for k in kinds)}")
    return value


def _strings(path: Path, values: list[Any], where: str) -> tuple[str, ...]:
    if not all(isinstance(value, str) for value in values):
        raise ValueError(f"{path}: {where} must hold strings")
    return tuple(values)


def read_feature_report(path: Path) -> FeatureReport:
    """Reads and checks one report: UTF-8 JSON with the members above, a known mode, counters sorted by unique id and
    gates in PER_MODE_GATES order or none. Raises ValueError naming the file and the problem."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"{path}: cannot read the report: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: not UTF-8 JSON: {error}") from error
    if not isinstance(document, dict):
        raise ValueError(f"{path}: the report is not a JSON object")
    mode = _member(path, document, "mode", str, "the report")
    if mode not in MODES:
        raise ValueError(f"{path}: unknown mode '{mode}' (expected one of {', '.join(MODES)})")
    passed = _member(path, document, "passed", bool, "the report")
    _member(path, document, "cancelled", bool, "the report")
    _member(path, document, "suites", list, "the report")

    skipped: list[str] = []
    for index, row in enumerate(_member(path, document, "cases", list, "the report")):
        where = f"cases[{index}]"
        if not isinstance(row, dict):
            raise ValueError(f"{path}: {where} is not an object")
        suite = _member(path, row, "suite", str, where)
        name = _member(path, row, "case", str, where)
        if _member(path, row, "status", str, where) == "skipped":
            skipped.append(f"{suite}/{name}")

    coverage = _member(path, document, "coverage", dict, "the report")
    _member(path, coverage, "registryFingerprint", str, "coverage")
    counters: list[CoverageCounter] = []
    for index, item in enumerate(_member(path, coverage, "counters", list, "coverage")):
        where = f"coverage.counters[{index}]"
        if not isinstance(item, dict):
            raise ValueError(f"{path}: {where} is not an object")
        modes = _strings(path, _member(path, item, "modes", list, where), f"{where}.modes")
        unknown = [entry for entry in modes if entry not in MODES]
        if unknown:
            raise ValueError(f"{path}: {where}.modes names the unknown mode '{unknown[0]}'")
        count = _member(path, item, "count", int, where)
        if count < 0:
            raise ValueError(f"{path}: {where}.count is negative")
        counters.append(CoverageCounter(_member(path, item, "id", str, where), _member(path, item, "kind", str, where),
                                        modes, count))
    ids = [counter.id for counter in counters]
    for previous, current in zip(ids, ids[1:]):
        if previous == current:
            raise ValueError(f"{path}: the counter '{current}' is listed twice")
        if previous.encode("utf-8") > current.encode("utf-8"):
            raise ValueError(f"{path}: the counters are not sorted by id ('{previous}' before '{current}')")

    gates: list[GateResult] = []
    for index, item in enumerate(_member(path, coverage, "gates", list, "coverage")):
        where = f"coverage.gates[{index}]"
        if not isinstance(item, dict):
            raise ValueError(f"{path}: {where} is not an object")
        gates.append(GateResult(_member(path, item, "gate", str, where), _member(path, item, "evaluated", bool, where),
                                _member(path, item, "passed", bool, where),
                                _strings(path, _member(path, item, "missing", list, where), f"{where}.missing")))
    names = tuple(gate.gate for gate in gates)
    if names and names != PER_MODE_GATES:
        raise ValueError(f"{path}: the gates are {', '.join(names)}; a report lists none or exactly "
                         f"{', '.join(PER_MODE_GATES)} in that order")
    return FeatureReport(path, mode, passed, tuple(skipped), tuple(counters), tuple(gates))


def evaluate_feature_report(report: FeatureReport) -> list[str]:
    """The failures of one editor run for the feature suite, empty when it passes: the run did not pass; a gate of
    PER_MODE_GATES is absent, unevaluated or failed (one "UNCOVERED gate <gate> (<mode>): <id>" line per missing id); a
    case row was skipped (FeatureTest allows no skipped case, decision 7)."""
    failures: list[str] = []
    if not report.passed:
        failures.append(f"the {report.mode} run did not pass")
    gates = {gate.gate: gate for gate in report.gates}
    for name in PER_MODE_GATES:
        gate = gates.get(name)
        if gate is None:
            failures.append(f"gate {name} ({report.mode}) is absent: the project does not set Testing.CoverageGates")
        elif not gate.evaluated:
            failures.append(f"gate {name} ({report.mode}) was not evaluated: only an unfiltered, non-recording "
                            "run that completes evaluates the gates")
        elif not gate.passed:
            if gate.missing:
                failures.extend(f"UNCOVERED gate {name} ({report.mode}): {entry}" for entry in gate.missing)
            else:
                failures.append(f"gate {name} ({report.mode}) failed without naming an uncovered entry")
    failures.extend(f"skipped case ({report.mode}): {case} (FeatureTest permits no skipped case)"
                    for case in report.skipped_cases)
    return failures


MERGE_RULES = ("report", "passed", "skipped", "gate", "missing", "extra", "mismatch", "uncovered")


def _ordered(rule: str, lines: list[tuple[int, str]]) -> list[str]:
    """`lines` ((mode index, subject) pairs) of one rule as failure lines, by mode, then by subject byte-wise."""
    unique = sorted(set(lines), key=lambda line: (line[0], line[1].encode("utf-8")))
    return [f"merge: {rule}: {subject}" for _, subject in unique]


def merge_coverage(reports: list[FeatureReport]) -> list[str]:
    """The cross-mode union merge of §15.6, empty when it passes. Each failure is one line "merge: <rule>: <subject>",
    with the rules of MERGE_RULES in that order (ADR 0022 decision 6):

    report     a mode of MODES has no report or more than one, or a report names another mode   "<mode>"
    passed     a report did not pass                                                             "<mode>"
    skipped    a report has a skipped case row                                                   "<mode> <suite>/<case>"
    gate       a per-mode gate is absent, unevaluated or failed                                  "<mode> <gate>"
    missing    an Editor id whose modes contain the mode is absent from that mode's report       "<mode> <id>"
    extra      a Release or Dist id the Editor report lacks, or whose Editor modes exclude it    "<mode> <id>"
    mismatch   a Release or Dist counter's kind or modes differ from the Editor's                "<mode> <id>"
    uncovered  an Editor counter has count 0 in every report whose mode is in its modes          "<id>"

    Within a rule, lines are ordered by mode (MODES order), then by subject byte-wise. The Editor report holds every
    registered counter (every entry exists in the Editor). registryFingerprint is not compared: each mode's registry
    differs by design.

    `reports` are expected in MODES order (the --merge-coverage command's positional order): a report in another
    position than its mode's "names another mode". When the report rule fails, the comparisons are meaningless and no
    other rule is checked."""
    report_lines: list[tuple[int, str]] = []
    for index, mode in enumerate(MODES):
        count = sum(1 for report in reports if report.mode == mode)
        misplaced = index >= len(reports) or reports[index].mode != mode
        if count != 1 or misplaced:
            report_lines.append((index, mode))
    if report_lines:
        # A surplus report always duplicates a mode, so it is named by that mode's line.
        return _ordered("report", report_lines)

    ordered = list(reports)
    failures: list[str] = []
    failures += _ordered("passed", [(index, report.mode) for index, report in enumerate(ordered) if not report.passed])
    failures += _ordered("skipped", [(index, f"{report.mode} {case}") for index, report in enumerate(ordered)
                                     for case in report.skipped_cases])
    gate_lines: list[tuple[int, str]] = []
    for index, report in enumerate(ordered):
        gates = {gate.gate: gate for gate in report.gates}
        for name in PER_MODE_GATES:
            gate = gates.get(name)
            if gate is None or not gate.evaluated or not gate.passed:
                gate_lines.append((index, f"{report.mode} {name}"))
    failures += _ordered("gate", gate_lines)

    editor = {counter.id: counter for counter in ordered[0].counters}
    exported = {report.mode: {counter.id: counter for counter in report.counters} for report in ordered[1:]}
    missing_lines: list[tuple[int, str]] = []
    extra_lines: list[tuple[int, str]] = []
    mismatch_lines: list[tuple[int, str]] = []
    for index, mode in enumerate(MODES):
        if mode == "Editor":
            continue
        counters = exported[mode]
        for counter in editor.values():
            if mode in counter.modes and counter.id not in counters:
                missing_lines.append((index, f"{mode} {counter.id}"))
        for counter in counters.values():
            known = editor.get(counter.id)
            if known is None or mode not in known.modes:
                extra_lines.append((index, f"{mode} {counter.id}"))
            elif counter.kind != known.kind or tuple(counter.modes) != tuple(known.modes):
                mismatch_lines.append((index, f"{mode} {counter.id}"))
    failures += _ordered("missing", missing_lines)
    failures += _ordered("extra", extra_lines)
    failures += _ordered("mismatch", mismatch_lines)

    uncovered: list[tuple[int, str]] = []
    for counter in editor.values():
        total = 0
        for report in ordered:
            if report.mode not in counter.modes:
                continue
            found = counter if report.mode == "Editor" else exported[report.mode].get(counter.id)
            total += found.count if found is not None else 0
        if total == 0:
            uncovered.append((0, counter.id))
    failures += _ordered("uncovered", uncovered)
    return failures


def read_called_methods(path: Path) -> set[str]:
    """The method names the Python suites called (one per line, engine_client.CallLog); a missing file is no calls."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return set()
    return {line.strip() for line in text.splitlines() if line.strip()}


def uncovered_methods(registered: list[str], called: set[str]) -> list[str]:
    """Gate 5: every registered method (the --dump-reference Methods.json names, which leave out the debug.* test hooks)
    that was never called, sorted."""
    return sorted(set(registered) - called)
