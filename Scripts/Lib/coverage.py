"""The FeatureTest coverage reports and the gates Scripts/Test.py evaluates (Docs/Architecture.md §15.5, §15.6;
Docs/Decisions/0022-m14-contract.md decisions 2, 3, 6 and 7).

A report is the JSON TestRunResult the FeatureTestRunner writes (bin/TestResults/FeatureTest-<mode>.json, or beside the
editor's --junit path): "mode", "passed", "cancelled", "cases", "suites" and "coverage" with "registryFingerprint",
"counters" ([{"id", "kind", "modes", "count"}], sorted by id) and "gates" ([{"gate", "evaluated", "passed",
"missing"}], gates 1, 1b, 2, 3, 4 and 9 in that order). The feature suite checks one editor report; the cross-mode merge
(run by M15's export stage) checks the Editor, Release and Dist reports together. Gate 5 (automation methods) compares the
editor's method catalogue with the methods the Python suites called.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

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


def read_feature_report(path: Path) -> FeatureReport:
    """Reads and checks one report: UTF-8 JSON with the members above, a known mode, counters sorted by unique id and
    gates in PER_MODE_GATES order or none. Raises ValueError naming the file and the problem."""
    raise NotImplementedError("contract stub: read_feature_report (ADR 0022 decision 7)")


def evaluate_feature_report(report: FeatureReport) -> list[str]:
    """The failures of one editor run for the feature suite, empty when it passes: the run did not pass; a gate of
    PER_MODE_GATES is absent, unevaluated or failed (one "UNCOVERED gate <gate> (<mode>): <id>" line per missing id); a
    case row was skipped (FeatureTest allows no skipped case, decision 7)."""
    raise NotImplementedError("contract stub: evaluate_feature_report (ADR 0022 decision 7)")


MERGE_RULES = ("report", "passed", "skipped", "gate", "missing", "extra", "mismatch", "uncovered")


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
    differs by design."""
    raise NotImplementedError("contract stub: merge_coverage (ADR 0022 decision 6)")


def read_called_methods(path: Path) -> set[str]:
    """The method names the Python suites called (one per line, engine_client.CallLog); a missing file is no calls."""
    raise NotImplementedError("contract stub: read_called_methods (ADR 0022 decision 6)")


def uncovered_methods(registered: list[str], called: set[str]) -> list[str]:
    """Gate 5: every registered method (the --dump-reference Methods.json names, which leave out the debug.* test hooks)
    that was never called, sorted."""
    raise NotImplementedError("contract stub: uncovered_methods (ADR 0022 decision 6)")
