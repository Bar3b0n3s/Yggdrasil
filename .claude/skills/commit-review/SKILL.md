---
name: commit-review
description: Use this skill before every commit in this repository (the commit gate, Architecture §15.9), and whenever asked to review a change, diff or task result. It runs the gate that fits the commit (a checkpoint commit's targeted runs on a milestone branch, PreCommit.py for a commit straight to main, or one CI.py at the milestone end, with --milestone at a phase's last lander), reviews the staged diff against Docs/ReviewChecklist.md in one pass, reports findings with file, line and fix, blocks the commit until every confirmed finding is fixed, and writes the commit message's "Reviewed" trailer.
---

# Commit review (commit gate)

This is the repository's recorded review of Architecture §15.9 (which calls it the `code-review` skill; it is named `commit-review` so it never collides with the generic `code-review` skill that ships with Claude Code, `Docs/Decisions/0002-m0-deviations.md`). A commit happens only after the gate that fits it is green on the final tree and this review records no unresolved finding (`Docs/Decisions/0023-verification-without-duplicate-work.md`; `Docs/Decisions/0028-backlog-program.md` decisions 11 and 12). Wave and contract commits are checkpoint commits on the milestone branch: each is reviewed and runs only the tests and suites its change touches, never `PreCommit.py` or `CI.py`. Commits straight to main outside a milestone keep ADR 0023's rule: `PreCommit.py`, or for documentation-only edits the checks that read the edited files. The milestone end runs one `python Scripts/CI.py` instead of PreCommit, never both. Within a phase, only the last lander's milestone-end `CI.py` adds `--milestone` (the hardening stage: perf, soak, ASan, debug heap, fuzz), on the merged tree of all the phase's milestones, when any milestone of the phase touches core systems, file readers (fuzzed formats) or the renderer, as its plan or contract decides; every other milestone end runs `CI.py --require-clang-cl` without `--milestone`. The program's setup gate keeps its own rule (ADR 0028 decision 16), and M27, alone in phase 4, decides in its plan. Review first and fix, then run the gate once. The checklist is `Docs/ReviewChecklist.md`; read it in full at the start of every review, because it changes over time. Review what is actually in the tree, and run the checks yourself rather than trusting a summary of what was run.

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

- PreCommit runs generate, the static checks (`CheckBuildConfig.py` on the workspace and on its fixtures, `Lint.py`, `Lint.py --self-test`, `CheckRuntimeMap.py --self-test`, the provenance audit self-test `AuditProvenance.py --self-test`, the format check, each fixture generator under `Tests/Data/Generate/` with `--check`, the DetMath reference check `GenerateDetMathReference.py --check`, `FetchAssets.py defaults --check` and `FetchAssets.py fixture --check`, the same list as `CI.py`'s lint stage), the Debug build, and the unit, gpu, golden, feature, automation and games suites in Debug (with `--require-gpu` unless `--gpu-optional` is given on a machine without a Vulkan device; the automation suite needs the MCP virtual environment from `python Scripts/Setup.py`; the games suite audits every staged demo game's provenance and runs its Debug editor half, and, with no game project, passes with one `games` step; a project on disk but not staged gets one passed `games <Name>` step naming it). Record the result of each step; `--json` adds a machine-readable summary.
- **Checkpoint commits** run the tests and suites their change touches instead of the block above (record each run), and **the milestone end** runs `CI.py` instead (below).
- **Mode.** PreCommit and the milestone-end `CI.py` are always strict (Roadmap rule 3, `Docs/Decisions/0004-contract-stub-gate.md`). A contract checkpoint runs its targeted checks in contract mode (`Lint.py --allow-contract-stubs`, `Test.py --allow-skips`); the wave's integration replaces every stub and skip of its contract, and the milestone-end `CI.py` is strict, so none reaches main. A strict run fails on any `ENGINE_CONTRACT_STUB` (Lint `contract-stub`) and on any test case skipped outside the child-process targets (Lint `test-skip`, and Test.py's `UNEXPECTED SKIP` lines). Contract mode anywhere else is a blocking finding.
- A red PreCommit is a blocking finding on its own. Report the first failing step and its error output, and do not continue to sign-off.
- For a build or test failure, use the `build-and-test` skill to diagnose it.
- The milestone end runs `python Scripts/CI.py` instead of PreCommit (it covers every PreCommit stage), whose determinism stage replays every FeatureTest replay in the Debug and Release editors and compares the hashes with the exported Release and Dist runs. `python Scripts/CI.py --milestone` adds the hardening stage (ASan unit and feature suites, the Debug gpu suite on the debug heap, long fuzz, soak, perf capture) and requires the clang-cl builds; `CI.py --require-clang-cl` without `--milestone` requires the clang-cl builds and reports hardening as not run (ADR 0024 decision 19, ADR 0028 decision 12).
- Steps that guard generated and shared content: `references` (`GenerateDocs.py --check`: the whole `Docs/Reference/`, `Resources/Scripting/Engine.d.luau` and `Tools/MCP/catalog.json` match the registries, with no extra file), the unit test "DocExamples: every Luau example in the docs type-checks" (every `lua`/`luau` fence in the documentation and skills), and the feature suite (`Test.py --suite feature`: all six coverage gates of `Projects/FeatureTest` evaluated and passing, no skipped row, and the project left byte-identical).
- Steps that guard the demo games (Architecture §13.12, ADR 0026): the games suite (`Test.py --suite games`) audits each staged game's provenance in index mode, as a fresh clone holds it (`AuditProvenance.py --project <dir>` reproduces one project's audit), and runs its suites and replays at their committed hashes with the canonical re-save dry run and the input check (at least one committed replay with input events, and each failing its verification without them); `CI.py`'s games stage adds the Release editor half and the Dist export with its smoke test. A change that alters canonical project output or simulation hashes must carry the demos' `project.upgrade`, the FeatureTest regeneration and the re-recorded replays in the same change (`Docs/ReviewChecklist.md` §9).

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
- **Cross-platform:** Linux and macOS code is written to compile on GCC 14, Clang 19+ and Apple Clang and is never verified (ADR 0028 decision 10); read those branches as carefully as the Windows path. A new platform capability in first-party C++ gets a Windows backend only and reports `ErrorCode::Unsupported` on Linux and macOS; existing POSIX backends keep compiling when their interface changes, and Python tooling keeps its guarded POSIX branches (`Docs/ReviewChecklist.md` §11).
- **Coverage parity** (Roadmap rule 5): every new component, field, enum value, setting, format, script API or method has its FeatureTest coverage in every run mode it supports (gates 1 to 9, §15.6), a validation code has its `Tests/Data/Validation/<CODE>/` fixture and `ValidatorCodeInfos` entry, and every new registered name has a description (gate 7).
- **Recorded changes:** a changed golden image names the golden and the reason (`golden-images`), and a changed replay hash states why the simulation changed (`game-building`, re-recording); an unexplained change to either is a blocking finding.

For each finding, confirm the defect is real: trace the code path, or write a quick test in a scratch location. Remove anything you cannot substantiate. Do not report speculation as a finding. One review pass per wave: blocker and major findings are verified before they are fixed, every confirmed finding is fixed at once, minors and nits included (never batched or deferred), and there are no rechecks of rechecks (ADR 0028 decision 11). A contract checkpoint gets one review of the frozen interfaces; the wave's one review pass covers the integrated wave diff; neither is rechecked.

## 4. Report

Rank findings most severe first. Use the host's findings-reporting tool if the host provides one; otherwise use a table:

| # | Severity | File:line | Checklist item | Problem and concrete failure scenario | Fix |
|---|---|---|---|---|---|

- **Blocking:** any gate failure, any unfixed confirmed finding of any severity (a finding against checklist §0–§13, missing tests for new behaviour, a style violation, a minor finding or a nit).
- **Minor findings and nits** (naming taste beyond the style guide, optional simplifications inside the change) are findings too: fixed at once, in the same pass, never batched or deferred, and the commit waits for them like any other finding.
- **Out of scope:** follow-ups beyond the change. List them separately, and suggest a separate task where one is warranted.
- **Verdict:** end the report with one line: **BLOCKED** (n unfixed findings, or the failing gate) or **APPROVED**.

## 5. Resolve, then sign off

- **Fixing:** fix every confirmed finding, minors and nits included, or hand them back to the author. After any fix:
  - rerun only the checks the fix affects (the failed stage, the touched test cases); run the gate once more only if the fix changed production code, build files or shared test support, and doc-only fixes rerun only a check that reads the file (ADR 0023);
  - re-review the changed hunks once (there are no rechecks of rechecks);
  - update the report.

  A finding is never resolved with "will fix later" or a `TODO`, or by weakening, skipping or deleting a test. When the rule itself is wrong, it is resolved by an ADR in `Docs/Decisions/`.
- **No commit while blocked.** Never commit while anything is BLOCKED, and never bypass the gate: no `--no-verify`, no skipped checks, no partial runs presented as a full run.
- **Trailer.** When APPROVED, the commit message ends with the review trailer in the format `Docs/ReviewChecklist.md` defines, `Reviewed: <outcome>; <n> findings (<n> fixed); PreCommit <green|red> (<stages>)`. Write the real numbers and step results, not a template:

  ```
  Reviewed: approved; 3 findings (3 fixed); PreCommit green (generate, checkbuildconfig, lint, lint self-test, format, Debug build, unit 42/42)
  ```

  A checkpoint commit names its targeted runs instead: `checkpoint (<runs>)`, with `contract mode` for a contract checkpoint. The milestone end names its CI run: `CI green (<stages>)`, with `--milestone` at a phase's last lander.

- **Commit** only when the task includes committing. Checkpoint commits stay on the milestone branch, and main fast-forwards to a milestone branch only after its milestone-end review and green `CI.py` (`AGENTS.md`, Commit gate). Only main is pushed: once the phase's last milestone has landed, main is pushed to `origin` as an off-site backup (ADR 0028 decisions 10 and 14). GitHub is only an off-site backup of the repository: GitHub Actions is not part of the process, and nothing reads or waits for its results.
