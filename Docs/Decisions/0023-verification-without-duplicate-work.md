# 0023 — Verification without duplicate work

- **Status:** accepted
- **Date:** 2026-10-13
- **Decided by:** the product owner ("we need to stop all of this duplicate work"), recorded by the lead.
- **Context:** A single gate pass had grown to about 80 minutes, and most of that time was repeated work. The M14 CI run's stage times: unit Debug 15.5 min, the clang-cl portability builds 6.4 min, gpu Debug 5.7 min (both API caps), lint 3.6 min, automation and gpu Release 2.9 min each, unit Release 1.9 min, and the rest under a minute each. The repetitions:
  - A milestone commit ran strict `PreCommit.py` and then the full `CI.py` on the same tree. CI repeats every PreCommit stage, so the Debug unit and GPU suites alone ran twice (about 21 minutes).
  - Implementation and fix agents ran full suites, and sometimes the whole gate, several times per task: before and after their changes, after reverting a fix to show its test fails, and again after the lead integrated their work.
  - A gate was rerun after edits limited to documentation that no check reads.
  - A gate sometimes ran while a review that could still change the tree was pending, so it had to run again.

## Decisions

### 1. One gate run per tree state

Every commit gets exactly one strict gate run on its final tree:

- **An ordinary commit** runs `python Scripts/PreCommit.py`.
- **A milestone commit** (the commit that completes a milestone, an integration commit, or any commit the Roadmap requires full CI for) runs `python Scripts/CI.py` **instead of** PreCommit, never in addition to it. CI runs every PreCommit stage: generate, the same static checks, the Debug build, the references check, and the unit, gpu and feature suites in Debug. The two PreCommit stages it does not repeat are covered by their Release runs:
  - **Golden:** CI compares the goldens in Release. Their CPU inputs are configuration-independent (§9.1) and the SPIR-V flags are identical in Debug and Release (§15.8).
  - **Automation:** CI drives the Release editor. Engine asserts stay on in Release, and the Debug-only behaviour the automation suite could reach is covered by the Debug unit suite.
- **A contract commit** runs `PreCommit.py --contract` once, as before (ADR 0004).

### 2. Review before the gate

The recorded review, and the fixes it causes, come before the gate. A gate does not run on a tree that a pending review may still change. A review may read the tree and the results of earlier targeted runs, but the commit's gate is the one run after the last code change.

### 3. Documentation edits after a green gate

An edit that touches only documentation no check reads keeps the gate result: ADRs, `Docs/Architecture.md`, `Docs/Roadmap.md`, `AGENTS.md`, `Docs/CodeStyle.md`, `Docs/ReviewChecklist.md` and project `AGENTS.md` files, as long as the edit adds or changes no Luau code fence. Edits to files a check does read run only that check, not the whole gate:

- a Luau fence in `Docs/**` or `.claude/skills/**`, or a skill file: the `DocExamples` unit cases;
- anything under `Docs/Reference/`, `Tools/MCP/catalog.json` or `Resources/Scripting/Engine.d.luau`: regenerate them, then run the `references` check;
- a Python file: `Lint.py --paths` on it;
- anything else is a code change and needs the gate.

The commit trailer names the gate run and lists any targeted checks run after it.

### 4. Agents iterate with targeted runs

Implementation, fix and stream agents never run `PreCommit.py` or `CI.py`; the lead runs the one gate on the integrated tree.

- While working, an agent builds what it changed (Release where the behaviour does not need Debug) and runs the affected test cases (`Tests.exe -tc=...`, `Test.py --suite <suite> --filter ...`).
- It shows that a regression test fails without its fix by running that test case alone against the reverted code, never by rerunning a suite.
- At the end it runs the suites its change can affect once (for example the unit suite of the touched modules, or the GPU suite when it changed GPU code) and reports the results; it does not repeat suites the lead's gate will run.

### 5. A red gate

When the gate fails, the cause is fixed and only the failed stages are rerun (`CI.py --stages <stage>`, or the `Test.py` suite). If the fix changed production code, build files or shared test support, the full gate runs once more on the final tree; that is the only repeat. A fix confined to the failing test's own files needs only the failed stage.

## Consequences

- A milestone commit's gate costs one `CI.py` run (about 45 minutes on this machine) instead of PreCommit plus CI (about 80 minutes), and agents no longer spend most of their time rerunning suites.
- The `Reviewed:` trailer of a milestone commit names its CI run: `Reviewed: <outcome>; <n> findings (<n> fixed); CI green (<stages>)`.
- Amended to match: `AGENTS.md` (commit gate, commands table, remote CI paragraph), Architecture §15.9, `Docs/ReviewChecklist.md`, and the `commit-review` and `build-and-test` skills.
- Not decided here, and tracked as follow-ups: a shared prebuilt vendor-library cache for new worktrees (each fresh worktree currently rebuilds every vendored library), and moving the long Debug fuzz iterations to the hardening stage (the 10,000-call binding fuzz takes 117 s in Debug and 7 s in Release). Both change the build or test layout and need their own decision.
- Remote CI stays non-blocking (ADR 0011).
