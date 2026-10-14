#!/usr/bin/env python3
"""Audit the provenance of the demo game projects (Docs/Architecture.md §13.12, §2.3; Roadmap M16 to M18;
Docs/Decisions/0026-m16-contract.md decisions 3 and 4).

A project passes when every engine-consumed file (its root *.eproj and .luaurc, and everything below Assets/) was
written by the editor through automation, as its committed Automation/Provenance.json and Automation/BuildLog.jsonl
prove: rule 1, each file has an entry whose XXH64 is the file's hash; rule 2, each entry was written by an automation
method, never "ui", whose request is at the entry's transcript line with the same id, method and client (or by
project.upgrade, itself in the transcript); rule 3, no entry names a file that does not exist unless the transcript
records its deletion or move. AGENTS.md, README.md and Automation/** are exempt, and nothing below the gitignored
Library/ is ever read. The rules, the finding codes and the warnings are those of Scripts/Lib/provenance.py.

--files index (the default, and the acceptance mode "from a fresh clone") audits the project's staged blobs,
materialized into a temporary directory with git ls-files and git checkout-index, so .gitattributes' conversions apply
and untracked and ignored files are absent; --files disk audits the working tree, *.bak files left out. --demos audits
every game project Scripts/Lib/games.py discovers below Projects/ and Tests/Projects/ (never Projects/FeatureTest), in
the same mode: in index mode only the staged projects, each project found on disk but not staged being named in a
passed step (it is not part of the commit), and in disk mode every project on disk. It passes with nothing to audit
when there is none, and a directory holding several *.eproj files fails its "provenance demos" step (exit code 1).
--self-test audits the seeded fixtures of Tests/Data/Provenance/<Case>/Project/ in disk mode and compares each with its
Expected.json; the static checks run it. The games suite audits the demos in index mode (ADR 0026 decision 9).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from Lib import games, paths, provenance
from Lib.report import (EXIT_INIT_FAILED, Console, Status, Step, configure_stdio, emit_json, overall_exit_code)

PROVENANCE_FIXTURES = paths.REPOSITORY_ROOT / "Tests" / "Data" / "Provenance"
# Findings printed per code and project; --json carries every one.
SHOWN_FINDINGS = 20


def resolve_project(argument: str) -> Path:
    """The project directory an --project argument names: the directory itself, or the parent of a *.eproj file; made
    absolute against the working directory. Whether it exists is the audit's question (project-missing)."""
    path = Path(argument).resolve()
    return path.parent if path.suffix == ".eproj" else path


def audit_step(project: Path, files: str, console: Console) -> tuple[Step, provenance.ProjectAudit | None]:
    """One project's audit as a step: passed with its counts, or failed with its first findings printed (exit code 1, or
    3 when a required file is missing); git missing or failing in index mode is exit code 3."""
    name = f"provenance {paths.display_path(project)}"
    try:
        audit = provenance.audit_project(project, files)
    except RuntimeError as error:
        return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED), None
    code = provenance.audit_exit_code(audit)
    for warning in audit.warnings:
        console.print(f"  warning {warning.code}: {warning.path}: {warning.detail}")
    shown: dict[str, int] = {}
    for finding in audit.findings:
        shown[finding.code] = shown.get(finding.code, 0) + 1
        if shown[finding.code] <= SHOWN_FINDINGS:
            console.print(f"  {finding.code}: {finding.path}: {finding.detail}")
        elif shown[finding.code] == SHOWN_FINDINGS + 1:
            console.print(f"  ... more {finding.code} findings (--json lists every one)")
    counts = audit.counts
    summary = (f"{counts.get('files', 0)} engine-consumed file(s), {counts.get('entries', 0)} entries, "
               f"{counts.get('transcriptLines', 0)} transcript line(s) ({counts.get('requests', 0)} requests), "
               f"{len(audit.warnings)} warning(s)")
    if code == provenance.EXIT_PASSED:
        return Step(name, Status.PASSED, f"{files}: {summary}"), audit
    counted = ", ".join(f"{key} {count}" for key, count in shown.items())
    detail = f"{files}: {len(audit.findings)} finding(s): {counted}"
    return Step(name, Status.FAILED, detail, exit_code=code), audit


def self_test(fixtures: Path = PROVENANCE_FIXTURES) -> list[Step]:
    """Every fixture of <fixtures>/<Case>/ audited in disk mode against its Expected.json ({"Format":
    "ProvenanceAuditFixture", "Version": 1, "ExitCode": 0|1|3, "Findings": [{"Code", "Path"}], "Warnings": [{"Code",
    "Path"}]}): one step per case, "provenance self-test <Case>", failing when the exit code or the sets of (code, path)
    pairs differ (naming them) or when a case other than Clean has neither a finding nor a warning (the passing
    behaviours are folded into Clean, ADR 0026 decision 6); then one step "provenance self-test coverage", failing
    naming each finding code of provenance.FINDING_CODES that no case expects."""
    raise NotImplementedError("contract stub: provenance audit self-test (ADR 0026 decision 6, stream A)")


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit codes: 0 passed (warnings allowed), 1 findings (or a self-test mismatch), 2 usage error, 3 a "
               "required file is missing (the project directory or its .eproj, Automation/Provenance.json, "
               "Automation/BuildLog.jsonl) or git is missing or fails in index mode. With several projects, the first "
               "failing project's code.",
    )
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--project", action="append", metavar="P",
                           help="a game project to audit: its directory or its .eproj file (repeatable)")
    selection.add_argument("--demos", action="store_true",
                           help="audit every game project below Projects/ and Tests/Projects/ (not FeatureTest)")
    selection.add_argument("--self-test", action="store_true",
                           help="audit the seeded fixtures of Tests/Data/Provenance/ against their Expected.json")
    parser.add_argument("--files", choices=provenance.FILE_MODES,
                        help="the files audited: index, the staged blobs as a fresh clone holds them (default), or "
                             "disk, the working tree without *.bak files")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable result")
    options = parser.parse_args(sys.argv[1:] if argv is None else argv)
    if options.self_test and options.files is not None:
        parser.error("--self-test audits its fixtures in disk mode and takes no --files")
    console = Console(options.json)
    files = options.files or provenance.DEFAULT_FILE_MODE
    audits: list[provenance.ProjectAudit] = []
    steps: list[Step] = []
    if options.self_test:
        steps = self_test()
    else:
        projects: list[Path] = []
        unstaged: list[Path] = []
        if options.demos:
            try:
                projects = [project.directory for project in games.discover_projects(paths.REPOSITORY_ROOT, files)]
                if files == "index":
                    unstaged = [project.directory for project in games.discover_projects(paths.REPOSITORY_ROOT, "disk")
                                if project.directory not in projects]
            except ValueError as error:
                projects, unstaged = [], []
                steps.append(Step("provenance demos", Status.FAILED, str(error)))
            except RuntimeError as error:
                projects, unstaged = [], []
                steps.append(Step("provenance demos", Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED))
            else:
                if not projects and not unstaged:
                    steps.append(Step("provenance demos", Status.PASSED,
                                      "no game project below Projects/ or Tests/Projects/ (FeatureTest is never "
                                      "audited)"))
        else:
            projects = [resolve_project(argument) for argument in options.project]
        for project in projects:
            step, audit = audit_step(project, files, console)
            steps.append(step)
            if audit is not None:
                audits.append(audit)
        for project in unstaged:
            steps.append(Step(f"provenance {paths.display_path(project)}", Status.PASSED,
                              "index: not staged, so not part of the commit and not audited (git add it to audit it)"))
    for step in steps:
        console.result(step)
    code = overall_exit_code(steps)
    if options.json:
        emit_json({"success": code == 0, "exitCode": code,
                   "projects": [provenance.audit_to_json(audit) for audit in audits],
                   "steps": [step.to_json() for step in steps]})
    return code


if __name__ == "__main__":
    sys.exit(main())
