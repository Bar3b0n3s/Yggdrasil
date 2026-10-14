"""The Release perf capture, bin/TestResults/Perf.json (Docs/Architecture.md §15.1 T6, §15.8; Roadmap M15;
Docs/Decisions/0024-m15-contract.md decision 16). Stream D implements this module and Scripts/Perf.py, which CI.py's
hardening stage runs first. Wall-clock numbers are noisy, so the capture never fails on a number: an over-budget metric
makes the step a warning, and only a capture that cannot run fails (ADR 0008 decision 15: no passing outcome may depend on
how long anything takes).

Perf.json (canonical JSON, two-space indent, keys sorted, LF):
  {"format": "PerfCapture", "version": 1, "commit": "<git HEAD>", "config": "Release",
   "host": {"system", "machine", "device", "deviceClass", "renderer"},
   "metrics": [MetricSummary as JSON, sorted by name],
   "scenes": [{"scene", "ticks",
               "frameMs": {...}, the play.step time per tick with render "Every" (MetricSummary as JSON),
               "simulationMs": {...}, the same with render "None",
               "gpuPassMs": {"<pass>": ms}, stats.get's GPU pass times at the end (empty without timings),
               "memory": {"privateStart", "privateEnd", "privatePeak", "residentPeak"} in bytes (Lib/process.py)},
              sorted by scene],
   "warnings": ["..."]}
The metrics are "playCopy/1000Entities" (ms, the median of 5 play.start/play.stop cycles), "replay/Determinism/tickMs"
and, per scene, "scene/<Name>/frameMs" and "scene/<Name>/simulationMs", the same summaries as the scene rows.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

PERF_FORMAT = "PerfCapture"
PERF_VERSION = 1
PERF_REPORT_NAME = "Perf.json"
# Tracked budgets, in the metric's unit (§5.6: "1,000 entities copy in under 30 ms (Release)"; ADR 0012 handed it to this
# capture). Exceeding one is a warning.
BUDGETS = {"playCopy/1000Entities": 30.0}


@dataclasses.dataclass(frozen=True)
class MetricSummary:
    """One metric over its samples; JSON keys name, unit, samples, mean, median, p95, max, budget (null without one),
    overBudget (the median exceeds the budget)."""

    name: str
    unit: str
    samples: int
    mean: float
    median: float
    p95: float  # nearest-rank
    maximum: float
    budget: float | None
    over_budget: bool


def summarize(name: str, unit: str, values: list[float]) -> MetricSummary:
    """The summary of `values` (at least one), with BUDGETS' budget for `name` if any. Raises ValueError for no values or
    a non-finite one."""
    raise NotImplementedError("contract stub: perf (ADR 0024 decision 16, stream D)")


def metric_to_json(metric: MetricSummary) -> dict[str, Any]:
    """The JSON object of the module docstring's metrics entries."""
    raise NotImplementedError("contract stub: perf (ADR 0024 decision 16, stream D)")


def write_perf_report(path: Path, capture: dict[str, Any]) -> None:
    """Writes `capture` (the document of the module docstring) canonically and atomically, creating the directory.
    Raises ValueError when it lacks a member or has the wrong format or version."""
    raise NotImplementedError("contract stub: perf (ADR 0024 decision 16, stream D)")


def read_perf_report(path: Path) -> dict[str, Any]:
    """Reads and checks a Perf.json. Raises ValueError naming the file and the problem."""
    raise NotImplementedError("contract stub: perf (ADR 0024 decision 16, stream D)")
