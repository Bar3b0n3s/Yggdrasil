"""The Release perf capture, bin/TestResults/Perf.json (Docs/Architecture.md §15.1 T6, §15.8; Roadmap M15;
Docs/Decisions/0024-m15-contract.md decision 16). Scripts/Perf.py captures it; CI.py's hardening stage runs that first.
Wall-clock numbers are noisy, so the capture never fails on a number: an over-budget metric makes the step a warning,
and only a capture that cannot run fails (ADR 0008 decision 15: no passing outcome may depend on how long anything
takes). No baseline is committed (the numbers churn per machine).

Perf.json (canonical JSON, two-space indent, keys sorted, LF):
  {"format": "PerfCapture", "version": 1, "commit": "<git HEAD>", "config": "Release",
   "host": {"system", "machine", "device", "deviceClass", "renderer"},
   "metrics": [MetricSummary as JSON, sorted by name],
   "scenes": [{"scene", "ticks",
               "frameMs": {...}, the play.step time per tick with render "Every" (MetricSummary as JSON; null when the
                         capture ran without a device),
               "simulationMs": {...}, the same with render "None",
               "gpuPassMs": {"<pass>": ms}, stats.get's GPU pass times at the end (empty without timings),
               "memory": {"privateStart", "privateEnd", "privatePeak", "residentPeak"} in bytes (Lib/process.py; null
                         when the query failed)},
              sorted by scene],
   "warnings": ["..."]}
The metrics are "playCopy/1000Entities" (ms: per cycle, the round trip of a lockstep play.start of a generated
1,000-entity scene less that of a generated empty scene, so the session's fixed setup cost, which dominates a play.start
in FeatureTest (about 0.7 s on the reference machine, with or without a renderer), cancels and the difference is the
serializer copy of the entities that §5.6 budgets; the median of 5 cycles is compared with the budget),
"replay/Determinism/tickMs" (ms per tick of the 600-tick replay in a --renderer none editor) and, per scene,
"scene/<Name>/frameMs" and "scene/<Name>/simulationMs", the same summaries as the scene rows.
"""

from __future__ import annotations

import dataclasses
import json
import math
import os
import statistics
from pathlib import Path
from typing import Any

PERF_FORMAT = "PerfCapture"
PERF_VERSION = 1
PERF_REPORT_NAME = "Perf.json"
# Tracked budgets, in the metric's unit (§5.6: "1,000 entities copy in under 30 ms (Release)"; ADR 0012 handed it to
# this capture). Exceeding one is a warning.
BUDGETS = {"playCopy/1000Entities": 30.0}

_CAPTURE_MEMBERS = {"format": str, "version": int, "commit": str, "config": str, "host": dict, "metrics": list,
                    "scenes": list, "warnings": list}
_HOST_MEMBERS = ("system", "machine", "device", "deviceClass", "renderer")
_METRIC_MEMBERS = ("name", "unit", "samples", "mean", "median", "p95", "max", "budget", "overBudget")
_SCENE_MEMBERS = ("scene", "ticks", "frameMs", "simulationMs", "gpuPassMs", "memory")
_MEMORY_MEMBERS = ("privateStart", "privateEnd", "privatePeak", "residentPeak")


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
    """The summary of `values` (at least one), with BUDGETS' budget for `name` if any. Raises ValueError for no values
    or a non-finite one."""
    if not values:
        raise ValueError(f"metric {name} has no samples")
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"metric {name} has a non-finite sample")
    ordered = sorted(float(value) for value in values)
    median = float(statistics.median(ordered))
    budget = BUDGETS.get(name)
    return MetricSummary(
        name=name,
        unit=unit,
        samples=len(ordered),
        mean=math.fsum(ordered) / len(ordered),
        median=median,
        p95=ordered[max(math.ceil(0.95 * len(ordered)), 1) - 1],
        maximum=ordered[-1],
        budget=budget,
        over_budget=budget is not None and median > budget,
    )


def metric_to_json(metric: MetricSummary) -> dict[str, Any]:
    """The JSON object of the module docstring's metrics entries."""
    return {"name": metric.name, "unit": metric.unit, "samples": metric.samples, "mean": metric.mean,
            "median": metric.median, "p95": metric.p95, "max": metric.maximum, "budget": metric.budget,
            "overBudget": metric.over_budget}


def _check_metric(metric: Any, where: str) -> None:
    if not isinstance(metric, dict):
        raise ValueError(f"{where} is not an object")
    missing = [member for member in _METRIC_MEMBERS if member not in metric]
    if missing:
        raise ValueError(f"{where} lacks {', '.join(missing)}")
    if not isinstance(metric["name"], str) or not isinstance(metric["samples"], int) or metric["samples"] < 1:
        raise ValueError(f"{where} needs a name and at least one sample")


def _check_capture(capture: Any) -> None:
    """Raises ValueError naming the first member of the module docstring's document that `capture` lacks or has
    wrong."""
    if not isinstance(capture, dict):
        raise ValueError("the capture is not an object")
    for member, kind in _CAPTURE_MEMBERS.items():
        if member not in capture:
            raise ValueError(f"the capture lacks '{member}'")
        if not isinstance(capture[member], kind) or (kind is int and isinstance(capture[member], bool)):
            raise ValueError(f"'{member}' is not a {kind.__name__}")
    if capture["format"] != PERF_FORMAT:
        raise ValueError(f"format is '{capture['format']}', not '{PERF_FORMAT}'")
    if capture["version"] != PERF_VERSION:
        raise ValueError(f"version is {capture['version']}, not {PERF_VERSION}")
    missing = [member for member in _HOST_MEMBERS if not isinstance(capture["host"].get(member), str)]
    if missing:
        raise ValueError(f"host lacks {', '.join(missing)}")
    for index, metric in enumerate(capture["metrics"]):
        _check_metric(metric, f"metrics[{index}]")
    names = [metric["name"] for metric in capture["metrics"]]
    if names != sorted(names) or len(set(names)) != len(names):
        raise ValueError("metrics are not sorted by name or repeat one")
    for index, scene in enumerate(capture["scenes"]):
        where = f"scenes[{index}]"
        if not isinstance(scene, dict):
            raise ValueError(f"{where} is not an object")
        missing = [member for member in _SCENE_MEMBERS if member not in scene]
        if missing:
            raise ValueError(f"{where} lacks {', '.join(missing)}")
        for member in ("frameMs", "simulationMs"):
            if scene[member] is not None:
                _check_metric(scene[member], f"{where}.{member}")
        if not isinstance(scene["gpuPassMs"], dict):
            raise ValueError(f"{where}.gpuPassMs is not an object")
        if scene["memory"] is not None and (not isinstance(scene["memory"], dict)
                                            or any(member not in scene["memory"] for member in _MEMORY_MEMBERS)):
            raise ValueError(f"{where}.memory lacks {', '.join(_MEMORY_MEMBERS)}")
    scenes = [scene["scene"] for scene in capture["scenes"]]
    if scenes != sorted(scenes):
        raise ValueError("scenes are not sorted by scene")
    if not all(isinstance(warning, str) for warning in capture["warnings"]):
        raise ValueError("warnings are not all text")


def write_perf_report(path: Path, capture: dict[str, Any]) -> None:
    """Writes `capture` (the document of the module docstring) canonically and atomically, creating the directory.
    Raises ValueError when it lacks a member or has the wrong format or version."""
    _check_capture(capture)
    text = json.dumps(capture, indent=2, sort_keys=True, allow_nan=False) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(text.encode("utf-8"))
    temporary.replace(path)


def read_perf_report(path: Path) -> dict[str, Any]:
    """Reads and checks a Perf.json. Raises ValueError naming the file and the problem."""
    try:
        capture = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"{path}: cannot be read: {error}") from None
    except ValueError as error:
        raise ValueError(f"{path}: not JSON: {error}") from None
    try:
        _check_capture(capture)
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from None
    return capture
