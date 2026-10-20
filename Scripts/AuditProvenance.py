#!/usr/bin/env python3
"""Audit the provenance of the demo game projects (Docs/Architecture.md §13.12, §2.3; Roadmap M16, M17, M25 and M27;
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
import collections
import json
import sys
import time
from pathlib import Path

from Lib import games, paths, provenance
from Lib.report import (EXIT_FAILED, EXIT_INIT_FAILED, Console, Status, Step, configure_stdio, emit_json,
                        overall_exit_code)

PROVENANCE_FIXTURES = paths.REPOSITORY_ROOT / "Tests" / "Data" / "Provenance"
# The generator that owns PROVENANCE_FIXTURES (ADR 0026 decision 6).
FIXTURE_GENERATOR = "MakeProvenanceFixtures.py"
# The case that holds every passing behaviour and must pass; every other case must have a finding or a warning.
CLEAN_CASE = "Clean"
# <Case>/Expected.json: {"Format": EXPECTED_FORMAT, "Version": 1, "ExitCode", "Findings", "Warnings"}.
EXPECTED_FORMAT = "ProvenanceAuditFixture"
EXPECTED_MEMBERS = ("Format", "Version", "ExitCode", "Findings", "Warnings")
EXPECTED_EXIT_CODES = (provenance.EXIT_PASSED, provenance.EXIT_FINDINGS, provenance.EXIT_MISSING_FILE)
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
    # "." stands for the project itself (a finding whose path is "").
    for warning in audit.warnings:
        console.print(f"  warning {warning.code}: {warning.path or '.'}: {warning.detail}")
    shown: dict[str, int] = {}
    for finding in audit.findings:
        shown[finding.code] = shown.get(finding.code, 0) + 1
        if shown[finding.code] <= SHOWN_FINDINGS:
            console.print(f"  {finding.code}: {finding.path or '.'}: {finding.detail}")
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
    naming each finding code of provenance.FINDING_CODES that no case expects, except provenance.INDEX_ONLY_CODES,
    which a disk-mode fixture cannot hold (Tests/Automation/test_audit_provenance.py builds them in a repository)."""
    if not fixtures.is_dir():
        return [Step("provenance self-test", Status.FAILED, f"{paths.display_path(fixtures)} does not exist: python "
                                                             f"Tests/Data/Generate/{FIXTURE_GENERATOR} writes it",
                     exit_code=EXIT_INIT_FAILED)]
    cases = sorted(entry.name for entry in fixtures.iterdir() if entry.is_dir())
    steps: list[Step] = []
    if CLEAN_CASE not in cases:
        steps.append(Step(f"provenance self-test {CLEAN_CASE}", Status.FAILED,
                          f"{paths.display_path(fixtures / CLEAN_CASE)} does not exist: every passing behaviour is "
                          "folded into it (ADR 0026 decision 6)", exit_code=EXIT_INIT_FAILED))
    expected_codes: set[str] = set()
    for case in cases:
        started = time.perf_counter()
        step, expected = self_test_case(fixtures / case)
        step.duration = time.perf_counter() - started
        steps.append(step)
        expected_codes.update(code for code, _ in expected)
    seeded = [code for code in provenance.FINDING_CODES if code not in provenance.INDEX_ONLY_CODES]
    uncovered = [code for code in seeded if code not in expected_codes]
    if uncovered:
        steps.append(Step("provenance self-test coverage", Status.FAILED,
                          f"no case expects the finding code(s) {', '.join(uncovered)}: seed a case for each "
                          f"(Tests/Data/Generate/{FIXTURE_GENERATOR})", exit_code=EXIT_FAILED))
    else:
        steps.append(Step("provenance self-test coverage", Status.PASSED,
                          f"every one of the {len(seeded)} disk-mode finding codes has a case (index mode only: "
                          f"{', '.join(provenance.INDEX_ONLY_CODES)})"))
    return steps


def self_test_case(case: Path) -> tuple[Step, list[tuple[str, str]]]:
    """One fixture case audited in disk mode against its Expected.json: its step and its expected (code, path) findings
    (none when Expected.json cannot be read)."""
    name = f"provenance self-test {case.name}"
    expected_path = case / "Expected.json"
    try:
        expected = read_expected(expected_path)
    except (OSError, ValueError) as error:
        return Step(name, Status.FAILED, f"{paths.display_path(expected_path)}: {error}",
                    exit_code=EXIT_INIT_FAILED), []
    exit_code, findings, warnings = expected
    try:
        audit = provenance.audit_project(case / "Project", "disk")
    except (OSError, RuntimeError, ValueError) as error:
        return Step(name, Status.FAILED, f"the audit failed: {error}", exit_code=EXIT_FAILED), findings
    problems: list[str] = []
    actual_code = provenance.audit_exit_code(audit)
    if actual_code != exit_code:
        problems.append(f"exit code {actual_code}, expected {exit_code}")
    for label, wanted, got in (("findings", findings, audit.findings), ("warnings", warnings, audit.warnings)):
        difference = describe_difference(wanted, [(finding.code, finding.path) for finding in got])
        if difference:
            problems.append(f"{label}: {difference}")
    if case.name != CLEAN_CASE and not audit.findings and not audit.warnings:
        problems.append("neither a finding nor a warning: a seeded case must fail (the passing behaviours belong in "
                        f"{CLEAN_CASE})")
    if problems:
        return Step(name, Status.FAILED, "; ".join(problems), exit_code=EXIT_FAILED), findings
    return Step(name, Status.PASSED, f"exit code {actual_code}, {len(audit.findings)} finding(s) and "
                                     f"{len(audit.warnings)} warning(s), as expected"), findings


def read_expected(path: Path) -> tuple[int, list[tuple[str, str]], list[tuple[str, str]]]:
    """A fixture's Expected.json: its exit code and its expected (code, path) findings and warnings. Raises OSError, or
    ValueError for a document that is not a ProvenanceAuditFixture of version 1."""
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or sorted(document) != sorted(EXPECTED_MEMBERS):
        raise ValueError(f"must be an object with exactly the members {', '.join(EXPECTED_MEMBERS)}")
    if document["Format"] != EXPECTED_FORMAT or type(document["Version"]) is not int or document["Version"] != 1:
        raise ValueError(f"must be {{\"Format\": \"{EXPECTED_FORMAT}\", \"Version\": 1}}")
    exit_code = document["ExitCode"]
    if type(exit_code) is not int or exit_code not in EXPECTED_EXIT_CODES:
        raise ValueError(f"\"ExitCode\" must be one of {', '.join(map(str, EXPECTED_EXIT_CODES))}")
    lists: list[list[tuple[str, str]]] = []
    for member, allowed in (("Findings", provenance.FINDING_CODES), ("Warnings", provenance.WARNING_CODES)):
        items = document[member]
        if not isinstance(items, list):
            raise ValueError(f"\"{member}\" must be an array")
        pairs: list[tuple[str, str]] = []
        for item in items:
            if (not isinstance(item, dict) or sorted(item) != ["Code", "Path"] or item["Code"] not in allowed
                    or not isinstance(item["Path"], str)):
                raise ValueError(f"each item of \"{member}\" must hold exactly a \"Code\" (one of "
                                 f"{', '.join(allowed)}) and a \"Path\" (project-relative, \"\" for the project)")
            pairs.append((item["Code"], item["Path"]))
        lists.append(pairs)
    return exit_code, lists[0], lists[1]


def describe_difference(expected: list[tuple[str, str]], actual: list[tuple[str, str]]) -> str:
    """"unexpected [...], missing [...]" for the (code, path) pairs that differ between the two lists (counted, so a
    pair expected once and found twice differs), or "" when they hold the same pairs."""
    unexpected = collections.Counter(actual) - collections.Counter(expected)
    missing = collections.Counter(expected) - collections.Counter(actual)
    shown = [f"{label} [{', '.join(show_pair(pair) for pair in sorted(pairs.elements()))}]"
             for label, pairs in (("unexpected", unexpected), ("missing", missing)) if pairs]
    return ", ".join(shown)


def show_pair(pair: tuple[str, str]) -> str:
    code, path = pair
    return f"{code} {path}" if path else f"{code} (the project)"


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

    def finished(step: Step) -> None:
        # Printed as soon as it is known, so each project's result follows the findings audit_step printed for it.
        console.result(step)
        steps.append(step)

    if options.self_test:
        for step in self_test():
            finished(step)
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
                finished(Step("provenance demos", Status.FAILED, str(error)))
            except RuntimeError as error:
                projects, unstaged = [], []
                finished(Step("provenance demos", Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED))
            else:
                if not projects and not unstaged:
                    finished(Step("provenance demos", Status.PASSED,
                                  "no game project below Projects/ or Tests/Projects/ (FeatureTest is never audited)"))
        else:
            projects = [resolve_project(argument) for argument in options.project]
        for project in projects:
            step, audit = audit_step(project, files, console)
            finished(step)
            if audit is not None:
                audits.append(audit)
        for project in unstaged:
            finished(Step(f"provenance {paths.display_path(project)}", Status.PASSED,
                          "index: not staged, so not part of the commit and not audited (git add it to audit it)"))
    code = overall_exit_code(steps)
    if options.json:
        emit_json({"success": code == 0, "exitCode": code,
                   "projects": [provenance.audit_to_json(audit) for audit in audits],
                   "steps": [step.to_json() for step in steps]})
    return code


if __name__ == "__main__":
    sys.exit(main())
