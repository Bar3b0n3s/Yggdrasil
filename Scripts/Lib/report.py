"""Step results, console output, --json documents and JUnit summaries shared by the developer scripts.

Conventions for every script:
- Human-readable progress goes to stdout. With --json it goes to stderr instead, and stdout carries exactly one JSON
  document, so `Script.py --json > result.json` always parses.
- Exit codes follow Architecture §4.1 where they apply: 0 success, 1 failed, 2 usage error (bad command line),
  3 initialization failed (a required tool or file is missing), 5 timeout.
"""

from __future__ import annotations

import dataclasses
import datetime
import enum
import json
import os
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any, TextIO

EXIT_SUCCESS = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_INIT_FAILED = 3
EXIT_TIMEOUT = 5


class Status(str, enum.Enum):
    PASSED = "passed"
    FAILED = "failed"
    TIMEOUT = "timeout"
    WARNING = "warning"  # completed, with something the reader should look at; not a failure
    SKIPPED = "skipped"  # deliberately not run, with the reason in the detail
    NOT_AVAILABLE = "not-available"  # requested, but that suite or stage does not exist yet in this milestone
    NOT_RUN = "not-run"  # not reached because an earlier step failed


FAILING_STATUSES = frozenset({Status.FAILED, Status.TIMEOUT})

_LABELS = {
    Status.PASSED: "passed",
    Status.FAILED: "FAILED",
    Status.TIMEOUT: "TIMEOUT",
    Status.WARNING: "warning",
    Status.SKIPPED: "skipped",
    Status.NOT_AVAILABLE: "NOT AVAILABLE YET",
    Status.NOT_RUN: "not run",
}


@dataclasses.dataclass
class Step:
    name: str
    status: Status
    detail: str = ""
    duration: float = 0.0
    exit_code: int | None = None  # a specific process exit code for a failing step (default: 1, or 5 for timeouts)
    data: dict[str, Any] = dataclasses.field(default_factory=dict)

    @property
    def failed(self) -> bool:
        return self.status in FAILING_STATUSES

    def to_json(self) -> dict[str, Any]:
        document: dict[str, Any] = {
            "name": self.name,
            "status": self.status.value,
            "detail": self.detail,
            "durationSeconds": round(self.duration, 3),
        }
        if self.exit_code is not None:
            document["exitCode"] = self.exit_code
        document.update(self.data)
        return document


def label(status: Status) -> str:
    return _LABELS[status]


def configure_stdio() -> None:
    """Line-buffered stdout/stderr that never raise on unencodable characters; UTF-8 when redirected (CI logs, the
    scripts that run other scripts), whatever the Windows code page."""
    for stream in (sys.stdout, sys.stderr):
        try:
            if stream.isatty():
                stream.reconfigure(errors="replace", line_buffering=True)  # type: ignore[attr-defined]
            else:
                stream.reconfigure(  # type: ignore[attr-defined]
                    encoding="utf-8", errors="replace", line_buffering=True
                )
        except (AttributeError, ValueError, OSError):
            pass


class Console:
    """Human-readable output: stdout normally, stderr in --json mode."""

    def __init__(self, json_mode: bool = False) -> None:
        self.json_mode = json_mode
        self.stream: TextIO = sys.stderr if json_mode else sys.stdout

    def print(self, text: str = "") -> None:
        self.stream.write(text + "\n")
        self.stream.flush()

    def heading(self, text: str) -> None:
        self.print(f"==== {text} ====")

    def result(self, step: Step) -> None:
        detail = f": {step.detail}" if step.detail else ""
        self.print(f"[{label(step.status)}] {step.name}{detail}")
        annotation = github_error_annotation(step)
        if annotation:
            # Workflow commands are read from stdout. Annotations are public through the check-runs API, unlike job
            # logs, which need a signed-in user, so a CI failure stays diagnosable for everyone.
            sys.stdout.write(annotation + "\n")
            sys.stdout.flush()

    def summary(self, title: str, steps: list[Step]) -> None:
        if not steps:
            return
        name_width = min(max(len(step.name) for step in steps), 60)
        label_width = max(len(label(step.status)) for step in steps)
        self.print()
        self.print(title)
        for step in steps:
            duration = f"{step.duration:7.1f} s" if step.duration >= 0.05 else " " * 9
            detail = step.detail.splitlines()[0] if step.detail else ""
            status = label(step.status)
            self.print(f"  {status:<{label_width}}  {step.name:<{name_width}}  {duration}  {detail}".rstrip())


_ANNOTATION_MESSAGE_LIMIT = 8000  # characters; the tail is kept, GitHub truncates longer messages anyway


def _escape_workflow_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _escape_workflow_property(text: str) -> str:
    return _escape_workflow_data(text).replace(":", "%3A").replace(",", "%2C")


def github_error_annotation(step: Step) -> str | None:
    """The `::error` workflow command for a failing step when running under GitHub Actions, else None. The message holds
    the step detail and the tail of the step's output, so a failure is diagnosable from the run's annotations alone."""
    if os.environ.get("GITHUB_ACTIONS") != "true" or not step.failed:
        return None
    tail = str(step.data.get("outputTail", "")).strip()
    message = f"{step.detail}\n\n{tail}" if tail else step.detail
    if len(message) > _ANNOTATION_MESSAGE_LIMIT:
        message = "...\n" + message[-_ANNOTATION_MESSAGE_LIMIT:]
    return f"::error title={_escape_workflow_property(step.name)}::{_escape_workflow_data(message)}"


def overall_exit_code(steps: list[Step]) -> int:
    """0 when no step failed, else the code of the first failing step (1 by default, 5 for a timeout)."""
    for step in steps:
        if step.failed:
            if step.exit_code:
                return step.exit_code
            return EXIT_TIMEOUT if step.status == Status.TIMEOUT else EXIT_FAILED
    return EXIT_SUCCESS


def emit_json(document: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(document, indent=2) + "\n")
    sys.stdout.flush()


def write_junit(path: Path, suite_name: str, steps: list[Step]) -> None:
    """Write `steps` as one JUnit test suite (one test case per step), for CI result viewers."""
    not_run = (Status.SKIPPED, Status.NOT_AVAILABLE, Status.NOT_RUN)
    suite = ElementTree.Element(
        "testsuite",
        {
            "name": suite_name,
            "tests": str(len(steps)),
            "failures": str(sum(1 for step in steps if step.status == Status.FAILED)),
            "errors": str(sum(1 for step in steps if step.status == Status.TIMEOUT)),
            "skipped": str(sum(1 for step in steps if step.status in not_run)),
            "time": f"{sum(step.duration for step in steps):.3f}",
            "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
    )
    for step in steps:
        case = ElementTree.SubElement(
            suite, "testcase", {"classname": suite_name, "name": step.name, "time": f"{step.duration:.3f}"}
        )
        if step.status == Status.FAILED:
            ElementTree.SubElement(case, "failure", {"message": step.detail or "failed"}).text = step.detail
        elif step.status == Status.TIMEOUT:
            ElementTree.SubElement(case, "error", {"message": step.detail or "timed out"}).text = step.detail
        elif step.status in not_run:
            message = f"{label(step.status)}: {step.detail}" if step.detail else label(step.status)
            ElementTree.SubElement(case, "skipped", {"message": message})
        elif step.detail:
            ElementTree.SubElement(case, "system-out").text = step.detail
    root = ElementTree.Element("testsuites")
    root.append(suite)
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    ElementTree.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
    temporary.replace(path)
