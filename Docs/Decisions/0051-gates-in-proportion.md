# 0051 — Gates in proportion to the change

- **Status:** accepted. The product owner, 2026-11-15, after the setup gate of the World Tree program ran for hours on one editor method ("if we're taking hours and hours for a few changes, we're doing something wrong"), on the lead's proposal: "yes, adopt 1 and 2". Lead-owned.
- **Date:** 2026-11-15.
- **Amended 2026-11-16 (the first batch gate):** decision 4's measurement, in "The first batch gate" below.
- **Amended 2026-11-17 (the second batch gate):** "The second batch gate" below.
- **Decided by:** the product owner; recorded by the lead.
- **Base:** main `bb51e110` (phase 5 forked: M28 to M35, `Docs/Decisions/0042-world-tree-engine-program.md`).
- **Authority:** amends `Docs/Decisions/0023-verification-without-duplicate-work.md` for commits straight to main outside a milestone, and `Docs/Decisions/0042-world-tree-engine-program.md` decisions 6 and 8 and its merge train; Architecture §15.9; `AGENTS.md`'s Commit gate.

## Context

- **What a full gate costs.** `PreCommit.py` on `bc7de792` (brief 08's `asset.import {replace: true}`: one editor method, its tests and fixtures): generate, the static checks, the Debug build and the references check about 31 min; the unit suite in Debug 42 min; the gpu suite under its two API caps 40 min; the golden suite 12 min; the feature suite 2 min; the automation suite more than 45 min when the owner stopped it; the games suite, which did not run, about 30 to 45 min. About 4 h, almost all of it in suites the change could not reach.
- **What phase 5 planned.** Eight milestone ends, each its own `CI.py --require-clang-cl` of about 6 h: about 48 h of machine time, one gate at a time on the one GPU.
- **What already scales.** Checkpoint commits on a milestone branch run only the tests and suites their change touches (ADR 0028 decision 11), and documentation-only edits run only the checks that read them (ADR 0023). ADR 0042 decision 6 already lets milestones ready together land on one gate.

## Decisions

1. **Commits straight to main outside a milestone run the targeted checks of their change,** as a checkpoint commit does, not `PreCommit.py`: `Generate.py` when files are added or removed; `Build.py` for each configuration the change compiles in; the static checks the change can fail (`Lint.py`, `Format.py --check`, `CheckBuildConfig.py` for build files, a fixture generator's `--check` when it or its fixtures change); `GenerateDocs.py --check` when a registry, a description or a generated reference can change; and the test cases and suites that read the changed files, filtered (`Test.py --suite unit --filter`, the gpu and golden suites filtered for rendering, the automation suite filtered for its Python tests, `Scaffold.py --check` when FeatureTest's inputs change, the games suite's halves for a changed demo, the `DocExamples` cases for a Luau fence or a skill). The review is unchanged: one `commit-review` pass, every finding fixed. The trailer names the runs, `checkpoint (<runs>)`. Documentation-only edits keep ADR 0023's rule. `PreCommit.py` stays for a change too broad to scope (shared test support, the build system, a cross-cutting rule), which the lead judges and the trailer names.
2. **Only a fully gated tree is pushed.** Main is pushed to `origin` only at a tree that a full gate passed (a milestone-end or batch `CI.py`, or `PreCommit.py` when no milestone follows), with nothing after it but documentation-only commits. A commit landed on targeted checks is covered by the next full gate; a full gate that fails on it is fixed like any red gate (ADR 0023 decision 5).
3. **Phase 5 lands in two batch gates** (ADR 0042 decision 6's "Landing together", made the rule): the first batch takes whichever of M28, M29, M31, M32, M34 and M35 are ready when the lead calls it; the second takes the rest, with M30 and M33 (M32 before M33 holds). Each batch is a landing branch from current main that merges its milestones in the merge train's order, each with its adaptations and its one review pass (its own diff and its merge), runs the targeted suites the merges touched, then one `CI.py --require-clang-cl` on the merged tree, and main fast-forwards to it. The second batch does the last lander's duties (`EngineVersionString` 0.6.0, FeatureTest and the editor-UI goldens on the merged tree), then main is pushed and the work pauses (ADR 0042, answer 8). A batch gate that fails on one milestone may land without it; that milestone joins the next batch. The first batch gate also runs the suites the setup gate skipped (ADR 0042 decision 2).
4. **Next: the gate itself.** The lead measures the first batch gate's stages and proposes what the full gate runs twice or slowly for no extra assurance (for example the gpu suite's second API cap, or the unit suite in Debug as well as Release), to the product owner (`Docs/Backlog.md` §3).

## The first batch gate

Batch 1 (M29, M35, M31, M32 and M34 on `landing/batch1`, `9ac024e2`) ran one `CI.py --require-clang-cl` from 14:51 to 17:19 UTC on 2026-11-16, green in every stage at its first run: 8,892 s, 2 h 28 min, against the 6 h a milestone end was planned at. Its stages:

| Stage | Time | Note |
|---|---|---|
| Setup, generate and the static checks | about 20 min | `CheckBuildConfig.py` 13.2 min (the workspace and 19 fixtures at about 40 s each), `Lint.py` 6.1 min, the rest under a minute |
| Builds (Debug, Release, Dist) | 2.4 min | linked from the vendor cache |
| unit Debug, Release | 30.4 min, 4.7 min | 5,165 and 5,164 cases |
| gpu Debug (Vulkan 1.4, 1.3) | 17.7 min, 17.5 min | 596 cases each |
| gpu Release (Vulkan 1.4, 1.3) | 6.2 min, 6.1 min | 596 cases each |
| golden Release | 6.1 min | 52 goldens |
| feature Debug, Release | 1.2 min, 0.1 min | |
| automation Release | 13.7 min | 873 tests, method coverage 121/121 |
| export, determinism, games | 0.4 min, 5.3 min, 9.1 min | 17 replays in four runs; four demos |
| portability (generate, clang-cl Release and Dist) | 6.6 min | |

The candidates for decision 4's proposal, where a stage repeats work for little assurance: `CheckBuildConfig.py`'s fixtures (13 min each gate, proving the checker still fails on fixtures that change only when the checker or the build files do) and the gpu suite's Vulkan 1.3 cap in Release as well as Debug (6 min). Together about 19 min of 148; the rest checks something no other stage does (Debug's iterator and heap checks, Release's optimizer, the 1.3 fallback, the four run modes). The lead puts them to the product owner at phase 5's end.

## The second batch gate

Batch 2 (M33, M28 and M30, with 0.6.0, on `landing/batch2`) passed in four runs, each after the last's failed stage, as ADR 0023 decision 5 has it:

1. `83ae4655`, 6,397 s: every stage to the gpu suite green but its Release run under Vulkan 1.3, where the windowed child of "ImGuiPlatformWindows: a platform window's swapchain out of memory at its creation or resize is retried after a trim, or the window waits for its next size" hung past the case's 120 s limit. The case takes about 3 s, passed in the gate's three other gpu runs and in batch 1's gate, and passed 10 runs of 10 alone in the failing configuration; no change of the batch reaches it. An intermittent hang, recorded with R-GPUCRASH's evidence (`Docs/Backlog.md` §3).
2. The gpu stage in Release, then golden, feature and automation, 2,390 s: one automation test of 890 failed, `test_replay`'s recording read at replay format 3 after M28's version 4 (M28's builders ran its other Python tests filtered); fixed in `054950eb`, a test file.
3. Automation, export, determinism, games and portability, 3,193 s: the clang-cl Dist build refused M30's assert-only `IsNavigationQueryPosition` (`-Wunneeded-internal-declaration`), which Dist's compiled-out asserts leave unused; fixed in `7f7a83aa` with `[[maybe_unused]]`, as M19 did for `IsValidBuildId`.
4. The builds and portability, 215 s: green.

Both defects were the kind only a full gate finds: a Python test outside the milestone's filtered runs and a configuration (clang-cl Dist) no targeted run builds. That is the cost decision 2 accepted, and the batch gate found them before main moved.

## Consequences

- A regression that a change's targeted checks miss can sit on main until the next full gate, hours or days later. Decision 2 keeps it off `origin`, the review names the suites a change reads, and `PreCommit.py` stays for broad changes.
- A red batch gate is harder to trace than one milestone's. Each milestone's targeted runs and wave reviews come first, and decision 3 lets a batch land without the milestone that broke it.
- Unchanged: checkpoint commits, the milestone review, strict mode, `CI.py`'s stages, the program's hardening rule (ADR 0042, answer 9) and the light path for game-only changes.
