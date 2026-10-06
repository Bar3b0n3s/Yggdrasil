---
name: commit-review
description: Use this skill before every commit in this repository (the commit gate, Architecture §15.9), and whenever asked to review a change, diff or task result. It runs PreCommit.py, reviews the staged diff against Docs/ReviewChecklist.md, reports findings with file, line and fix, blocks the commit on any failure, and writes the commit message's "Reviewed" trailer.
---

# Commit review (commit gate)

This is the repository's recorded review of Architecture §15.9 (which calls it the `code-review` skill; it is named `commit-review` so it never collides with the generic `code-review` skill that ships with Claude Code, `Docs/Decisions/0002-m0-deviations.md`). A commit happens only after `python Scripts/PreCommit.py` is green and this review records no unresolved blocking finding. The checklist is `Docs/ReviewChecklist.md`; read it in full at the start of every review, because it changes over time. Review what is actually in the tree, and run the checks yourself rather than trusting a summary of what was run.

## 1. Collect the change

```
git status --short
git diff --staged --stat
git diff --staged
```

- **Nothing staged:** to review work in progress, use `git diff HEAD` and list untracked files with `git status --short`.
- **New files:** read every new file in full.
- **Changed files:** read enough of each to understand every hunk in context.
- **Expected but untouched files:** check the files the change should have touched but did not:
  - a test for each new behaviour;
  - a premake file for new projects;
  - a `VENDOR.md` for vendor build changes;
  - docs and skills for changed commands or behaviour;
  - generated references.
- **Task context:** read the task statement and the owning milestone section of `Docs/Roadmap.md`, so the change is judged against what was asked: scope, file ownership, frozen contract headers.

## 2. Run the automated gate

```
python Scripts/PreCommit.py
```

- PreCommit runs generate, the static checks (`CheckBuildConfig.py` on the workspace and on its fixtures, `Lint.py`, `Lint.py --self-test` and the format check, the same list as `CI.py`'s lint stage), the Debug build, and the unit and feature suites. Record the result of each step; `--json` adds a machine-readable summary.
- **Mode.** PreCommit is strict by default. Run `python Scripts/PreCommit.py --contract` only when the task statement says the change is a milestone's contract commit (Roadmap rule 3, `Docs/Decisions/0004-contract-stub-gate.md`); the summary prints the mode it ran in. A strict run fails on any `ENGINE_CONTRACT_STUB` (Lint `contract-stub`) and on any test case skipped outside the child-process targets (Lint `test-skip`, and Test.py's `UNEXPECTED SKIP` lines). Contract mode on any other commit is a blocking finding.
- A red PreCommit is a blocking finding on its own. Report the first failing step and its error output, and do not continue to sign-off.
- For a build or test failure, use the `build-and-test` skill to diagnose it.
- A milestone commit additionally needs `python Scripts/CI.py` green.

## 3. Walk the checklist

Go through `Docs/ReviewChecklist.md` section by section (§0–§13) and give every item **pass**, **n/a** or **finding**. Items that need judgement are where reviews usually miss defects, so give them the most attention:

- **Correctness:**
  - boundary and empty inputs;
  - failure paths that leave state half-applied;
  - narrowing conversions;
  - non-finite values from external input.
- **Error handling:**
  - discarded `Result`s;
  - asserts used as the only guard on external input;
  - a new `try`/`catch` outside the allowlisted files.
- **Determinism:**
  - iteration over unordered containers that reaches output;
  - CRT transcendental functions on the simulation path;
  - wall-clock reads;
  - unseeded randomness.
- **Ownership:**
  - stored raw pointers or `string_view`s;
  - component references held across structural changes;
  - deferred lambdas with default captures.
- **Tests:**
  - every new behaviour is asserted, including its failure paths;
  - a bug fix's test would fail without the fix;
  - names follow `"<Unit>: <behaviour>"`, and Roadmap acceptance names are used verbatim.
- **Cross-platform:** Linux and macOS code must compile on GCC 14, Clang 19+ and Apple Clang. Read those branches as carefully as the Windows path.

For each finding, confirm the defect is real: trace the code path, or write a quick test in a scratch location. Remove anything you cannot substantiate. Do not report speculation as a finding.

## 4. Report

Rank findings most severe first. Use the host's findings-reporting tool if the host provides one; otherwise use a table:

| # | Severity | File:line | Checklist item | Problem and concrete failure scenario | Fix |
|---|---|---|---|---|---|

- **Blocking:** any PreCommit failure, any finding against checklist §0–§13, missing tests for new behaviour, or a style violation.
- **Non-blocking:** optional improvements and out-of-scope follow-ups. List them separately, and suggest a separate task where one is warranted.
- **Verdict:** end the report with one line: **BLOCKED** (n blocking findings) or **APPROVED**.

## 5. Resolve, then sign off

- **Fixing:** fix blocking findings, or hand them back to the author. After any fix:
  - re-run PreCommit on the final tree;
  - re-review the changed hunks;
  - update the report.

  A finding is never resolved with "will fix later" or a `TODO`, or by weakening, skipping or deleting a test. When the rule itself is wrong, it is resolved by an ADR in `Docs/Decisions/`.
- **No commit while blocked.** Never commit while anything is BLOCKED, and never bypass the gate: no `--no-verify`, no skipped checks, no partial runs presented as a full run.
- **Trailer.** When APPROVED, the commit message ends with the review trailer in the format `Docs/ReviewChecklist.md` defines, `Reviewed: <outcome>; <n> findings (<n> fixed); PreCommit <green|red> (<stages>)`. Write the real numbers and step results, not a template:

  ```
  Reviewed: approved; 3 findings (3 fixed); PreCommit green (generate, checkbuildconfig, lint, lint self-test, format, Debug build, unit 42/42)
  ```

  A contract commit names its mode: `PreCommit green, contract mode (...)`.

- **Commit and push** only when the task includes committing. Push only after the gate has passed (`AGENTS.md`, Commit gate).
