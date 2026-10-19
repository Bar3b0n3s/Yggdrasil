# 0028 — The backlog program (M18 to M23) before Rolling Ball

- **Status:** accepted. It records the product owner's decisions of 2026-10-19 on the backlog program and amends the standing documents to match (see "Amendments"). It is documentation only: no code changes with it, and the first code change of the program is M18's contract. Lead-owned.
- **Date:** 2026-10-19.
- **Decided by:** the product owner, 2026-10-19; recorded by the lead.
- **Base:** `cdf35ef` (M17 complete, ADR 0027).
- **Authority:** Roadmap "Rules for every milestone"; Architecture §1.2, §15.8, §15.9, §17, §18 and Appendix C; ADR 0004, ADR 0011, ADR 0013 decision 25, ADR 0023, ADR 0024 decision 19, ADR 0025 decisions 5 and 18, ADR 0026 decision 1 and ADR 0027. The lead's plan (`bin/Reviews/Backlog-Plan.md`, revision 3: its waves, streams, acceptance, engineering defaults, cross-cutting foundations, risks, facts and item appendix) and its completeness critic (`bin/Reviews/Backlog-Plan-Critic.md`) are kept under the ignored `bin/Reviews/`.

## Context

- **Tetris is rendered as 3D cubes.** M17 built `Projects/Tetris` through automation, but the engine has no 2D path, so every cell is a MeshRenderer cube under an orthographic camera, and the HUD is Screen-space `TextComponent` text, because there is no UI framework beyond `TextComponent` (§1.2).
- **The UI gap.** A game's menus, buttons, scroll views, text input and localized text all need a UI framework that v1 lists as a non-goal; Rolling Ball and any later game would hit the same gap.
- **The owner's choice.** On 2026-10-19 the product owner chose to complete the engine before Rolling Ball: the whole backlog (`Docs/Backlog.md` §2 to §6, and §7 except the Platforms row and networking) plus 2D rendering and a UI framework, all before Rolling Ball. The owner also chose to simplify: milestones are a made-up unit, so each of the plan's numbered groups becomes one milestone (the same work, fewer milestones), with a lighter process than M16's and M17's.
- **The plan.** Revision 2 of the lead's plan sequenced 124 items over twenty milestones, M18 to M37, with eight questions to the owner. The owner answered every question, restructured the program into six milestones, and declined the toolchain bump. Revision 3 applies that: 127 items (revision 2's, plus the gate scope in `CI.py` and two items split out of device-loss recovery so that each lifted limit has one outcome), of which 107 are built.

## Decisions

### 1. Scope

In scope, all before Rolling Ball:
- `Docs/Backlog.md` §2 to §6;
- §7 except the Platforms row and networking;
- 2D rendering and a UI framework, the two new capabilities;
- the extras the owner kept or chose: 2D lighting, world-space UI, Box2D vendored beside Jolt, a separately vendored `stb_truetype.h`, RTL and complex-script shaping, native file dialogs on Linux, a per-game native C++ gameplay module with editor-only hot reload, and visual scripting (a node graph compiling to the deterministic Luau runtime).

Out of scope, in the owner's words:
- **The Platforms row:** mobile, consoles, web, native Wayland, cross-OS export, bit-exact cross-OS or cross-compiler determinism, identity code signing and notarization ("We are not doing this right now"). Consoles and signing are also won't-do (decision 2); the other five rows stay in the Backlog.
- **Networking:** "Networking can stay in the backlog for now".

An in-scope item is dropped only on the owner's yes, at a milestone boundary.

### 2. The cuts

Won't do, by the owner's decisions:
- **The AAA items** ("Cut the AAA items"): the render thread, bindless rendering, GPU-driven rendering, render-graph aliasing, graphics backends other than NVRHI-Vulkan (D3D12 among them) and dynamic GI.
- **Declined extras:** ray tracing and the registry-based Play copy.
- **The vendored rule:** NVRHI push descriptors and line rasterization. Upstream `main` equals the pin `6b96fb0`, and the pull request that adds line rasterization (#158) is unmerged; vendoring it would edit vendored source.
- **A kept limit:** goldens on GPU classes other than nvidia-61x. There is no other hardware, and the driver is held (decision 9).
- **Consoles, and identity code signing and notarization.**

The "Won't do" list below gives each item's reason, and `Docs/Backlog.md` gains a "Won't do" section with the same entries.

### 3. The toolchain stays

The owner (2026-10-19): "I'm not sure updating vulkan will make a difference, especially since it's a minor version". The program stays on Vulkan SDK 1.4.350.0 and slangc 2026.8 (`Scripts/Lib/Toolchain.json` unchanged).

- The bump's only reason was the Backlog §3 row "`Invariant` on the clip position, which Slang 2026.8 cannot decorate" (ADR 0013 decision 25, ADR 0025 decision 18). Its `NoContraction` workaround keeps the prepass and forward pipelines in agreement, so the row becomes an accepted limit (Backlog §6).
- It is revisited only if an in-scope item ever needs a newer toolchain. That need then goes to the owner as an approval request at a milestone boundary, never mid-milestone.
- No golden is regenerated for a toolchain change, and M18's renderer split is proven against main at `cdf35ef`.

### 4. Structure: six milestones, then Rolling Ball

The program is six milestones, M18 to M23, then Rolling Ball as M24. Revision 2's milestones become waves inside them; the plan lists each wave's items, contract, streams, dependencies, acceptance and size.

| # | Name | Goal | Waves | Depends on | Hardening | Size |
|---|---|---|---|---|---|---|
| M18 | Foundations | The renderer split into stages with frozen per-pass recreate and GPU-zone hooks; shader hot reload; the vendor-library cache; GPU-lifetime instrumentation and the GPU-crash follow-up; profiler export and GPU merge; the per-frame arena measured; FeatureTest golden provenance; the golden device-class check and rename procedure; the prefab-member deletion refusal; the material-set fix; the gate scope in `CI.py` | W18.1 renderer stages, hot reload, diagnostics and tooling; W18.2 golden tooling and measured outcomes | M17 | yes | L (~7 sessions) |
| M19 | Device-loss recovery | The editor and the Runtime survive device loss and driver-reset hangs, and GPU out-of-memory as far as feasible (decision 5) | W19.1 recreation protocol and device-loss recovery; W19.2 hangs and GPU out-of-memory | M18 | yes | L (~7) |
| M20 | 2D, UI and Tetris | The 2D path, the UI framework and the automation the builder needs, proven by Tetris rebuilt in place as pixel art | W20.1 automation, replay v3 and export basics; W20.2 2D rendering path; W20.3 2D physics on Box2D and 2D lighting; W20.4 UI I, static; W20.5 UI II, interaction; W20.6 tilemaps and text input; W20.7 localization (with HarfBuzz and SheenBidi) and world-space UI; W20.8 Tetris rebuilt by builder sessions | M19 | yes | XL (~49, plus 2–3 builder) |
| M21 | Linux/macOS catch-up | The Linux and macOS jobs green, then kept green (decision 10) | W21.1 triage per compiler family, the POSIX automation connection, the ELF map reader, the macOS bundle, the hosted feature and games stages | M20 | no: its shared-code edits are compile-portability fixes that leave Windows behaviour unchanged; `--milestone` if its triage changes the behaviour of core systems, a fuzzed reader or the renderer (decided in its contract) | L (~5, plus Actions rounds) |
| M22 | Scene, editor and scripting | Physics and session semantics with the watch items; prefabs II and editor windows with native file dialogs (nativefiledialog-extended, Linux included); multiple open scenes; multiple scripts per entity; the per-game native module; visual scripting | W22.1 physics and session semantics; W22.2 prefabs II and editor windows; W22.3 multiple open scenes; W22.4 multiple scripts per entity; W22.5 native gameplay module; W22.6 visual scripting I, graphs and transpiler; W22.7 visual scripting II, the graph editor | M20 (M21 runs before it by the owner's order) | yes | XL (~42, plus a builder re-record) |
| M23 | Rendering and content | Instancing, point-light shadows and clustered lights; EXR, Opus, the second UV set and vertex colours, and BC1–BC7 compression including BC6H (CMP_Core); pak compression, reachability pruning and the Luau compiler out of Dist; `stb_truetype.h` vendored separately; skeletal and morph animation; particles and baked light probes; auto-exposure, colour-grading LUTs, TAA and SSR; terrain | W23.1 instancing and lights; W23.2 asset pipeline; W23.3 export II; W23.4 animation; W23.5 particles and probes; W23.6 post-processing and temporal; W23.7 terrain | M22 | yes | XL (~45) |
| M24 | Rolling Ball 3D | The current Roadmap M18, content unchanged, renumbered | per its own plan | M23 | decided in its plan | as M17 |

About 155 agent sessions plus 4–6 builder sessions; six gate runs instead of twenty (about 13 h of gate time instead of about 50 h).

### 5. Device-loss recovery: what is recovered and what stays fatal (M19)

The owner chose to lift the hang and out-of-memory limits as far as feasible.

- **Recovered in process:**
  - device loss in each of the three ways it surfaces (§8.1): a `VkResult`, NVRHI's "Device Removed!" callback and a `vk::SystemError` at the frame boundary;
  - a hang that the driver or the operating system resets: after the 10 s bounded wait times out, the engine waits a second bounded interval, long enough for the reset, for a loss report, and recovers if one arrives;
  - out-of-memory on render targets, pipelines (the debug views' included) and the swapchain after startup: the engine trims what it can rebuild and retries once; if the retry fails, only the request fails (a capture returns `OutOfMemory`, a debug view stays on its previous mode with a diagnostic, a resize keeps the previous swapchain and retries at the next resize), and a target is never shrunk silently, because headless must equal windowed;
  - out-of-memory during asset uploads, unchanged (a placeholder and a diagnostic).
- **Still fatal** (autosave in the editor, a crash report, exit 4):
  - a hang with no loss report after the second wait: Vulkan cannot cancel submitted work, a device with pending work cannot be destroyed, and a second device on the same GPU would queue behind the hung work; `project.open {recover}` restores the editor's autosave;
  - out-of-memory at startup (the device, the swapchain, the startup pipelines): nothing has rendered and there is nothing to fall back to;
  - a second loss during recovery, or device creation failing again after one retry.

M19's ADR moves the remaining limits to Backlog §6 and rewrites §8.14 items 6 to 8.

### 6. Design defaults of the kept items

- **Box2D beside Jolt (W20.3).** Box2D runs the 2D bodies and Jolt stays the 3D engine; an entity belongs to one world, and holding both 2D and 3D physics components is a validation error. Box2D is vendored unmodified under `Vendor/Box2D`, built with the precise FP flags, stepped serially in the fixed step, and kept behind a pimpl in Physics, so no public header exposes its types; its contact events are sorted canonically like Jolt's, and its determinism across Debug, Release and Dist is proven by a replay in all four runs before adoption. 2D physics and tilemap collision use it. If Box2D fails that proof, the lead's fallback keeps M20 moving: W20.3's 2D physics and W20.6's tilemap collision run on Jolt bodies with plane (degree-of-freedom) locks behind the same `Physics2DWorld` interface, and R-2D-BOX2D goes to the owner at M20's boundary (decision 1), to retry Box2D in a later milestone or keep the Jolt backend.
- **The native module (W22.5): static linking, editor-only hot reload.** One module per game, in `<Game>/Native/`, built with the first-party settings (precise FP, warnings as errors) and the determinism lint, and statically linked into that game's exported Runtime in Release and Dist; a game without one exports the prebuilt Runtime unchanged. Only the editor hot-reloads it, as a shared library swapped at a frame boundary while no play session runs; exports never load code at run time. Native build and load need the user-only "Allow native code" preference, which automation cannot set. W22.5's ADR amends §13.2's process allowlist (the fixed-path native build) and §14.2's rule that the editor never spawns builds, for games with a native module.
- **The Luau compiler out of Dist through a first-party require (W23.3).** A first-party require replaces Luau's Require library in every configuration, with the same cache, cycle and yield-continuation behaviour, and removes both `luarequire_*` calls, so Config and the Compiler no longer link into Dist (Parser and Ast stay for `parseType`). No vendored source is edited, signature validation stays in Dist, and W23.3's ADR replaces ADR 0025 decision 5's premise and adds a LuauCompiler family to the map check.
- **Visual scripting (W22.6–W22.7).** A graph is one behaviour, transpiled to Luau text at import and compiled by the normal script pipeline, so it is deterministic and Dist needs no compiler for it. Every graph action has an automation method.
- **2D lighting and world-space UI.** Sprites stay unlit by default and opt in to 2D lights; world-space canvases share the screen canvases' layout and hit-test code and take pointer rays through the camera from latched input.

### 7. Approvals

Every library and asset the program needs is approved up front (Roadmap rule 6), recorded in `Docs/Decisions/0001-approvals.md` with its upstream source, licence, destination and purpose: tinyexr (with miniz only if its build needs it), libogg, libopus, opusfile and miniaudio's unmodified libopus decoder with one Ogg Opus test fixture, Compressonator CMP_Core (bc7enc_rdo and bcdec as fallbacks), HarfBuzz and SheenBidi, nativefiledialog-extended, Box2D, `stb_truetype.h`, the GitHub CLI for reading Actions logs, and anything else an in-scope item needs from official upstream sources. There is no Vulkan SDK or slangc row (decision 3). Versions are pinned at the contract of the wave that vendors each item, with checksums and licences committed under the standing rules.

### 8. Tetris rebuilt in place as pixel art (W20.8)

Tetris is rebuilt in place in `Projects/Tetris`, as pixel art under a pixel-perfect camera, by builder sessions through the MCP tools only, as the last wave of M20. The 3D build stays in the index and git history until the finished pixel-art build replaces it in one checkpoint; the builder starts from an emptied working-tree folder, with a backup after each session (ADR 0027 decision 11's rule). The acceptance is M17's feature list on sprites and UI, with no MeshRenderer cells and no Screen Text HUD, its replays strict-hash identical in both editors and the testing Dist export, and the provenance audit passing from a fresh clone.

### 9. Goldens: one device class, the driver held

Goldens stay on the nvidia-61x device class only, and the NVIDIA driver's major version is held for the whole program. M18 adds the device-class check and the rename procedure, so an accidental driver update is reported instead of silently dropping to smoke mode. Only W23.2's texture compression regenerates existing goldens (plus a reviewed Text regeneration if the `stb_truetype.h` switch changes the atlases); every other wave only adds goldens, and any other change is a reviewed regeneration named in its wave's record.

### 10. The Linux and macOS catch-up and log access

- **Timing.** The catch-up is M21, run right after M20's Tetris rebuild (the owner's choice), non-blocking: if failures remain when its session budget is spent, they become open catch-up rows and M22 starts anyway (ADR 0011). This supersedes ADR 0026 decision 1's deferral and the 2026-10-14 instruction. Its Actions rounds run from pushes to `ci/**` branches (ci.yml runs on `main` and `ci/**`): W21.1's stream branches and the milestone branch may be pushed there before their review, as often as the triage needs, and a `ci/**` branch never reaches main except through the milestone-end review and `CI.py`. This is the one exception to the push policy (push only after review and gate; `Docs/Decisions/0001-approvals.md`).
- **Kept green.** From M21 on, the Linux and macOS jobs are kept green: each later milestone's first wave fixes what the previous push's Actions runs reported, read through gh under the product owner's read-only login, and no milestone waits for a run. A failure that points to a design problem is raised at once. Linux and macOS count as verified only once their jobs are green.
- **gh access.** The GitHub CLI is installed, and the owner logs it in with read-only access to the repository. The lead runs read commands only (`gh run list`, `gh run view --log-failed`) and never handles a token's value.

### 11. The lighter process

These rules replace the M16/M17 rigor for every milestone from M18 on:

1. **Branch and waves.** A milestone runs on its own branch in waves; each wave is streams in worktrees, integrated into the milestone branch. Waves run in the plan's order; a wave starts when the waves it depends on are integrated. Each shared integration file has one owner per wave instead of one per milestone; this amends Roadmap rule 4 and AGENTS.md's milestone-workflow rule 4.
2. **Contracts.** A contract only where a milestone or wave freezes shared interfaces; no contract task otherwise. A contract freezes headers with documented signatures, stubs that start with `ENGINE_CONTRACT_STUB();` and return `Unsupported`, and tests marked `doctest::skip`, and records its decisions in the milestone's ADR. Changing a frozen header needs the contract owner's review. This amends Roadmap rule 3 and AGENTS.md's milestone-workflow rule 3.
3. **Checkpoint commits.** Wave and contract commits are checkpoint commits on the milestone branch: each is reviewed and runs only the tests and suites its change touches, never `PreCommit.py` or `CI.py`. A contract checkpoint runs its targeted checks in contract mode (`Lint.py --allow-contract-stubs`, `Test.py --allow-skips`); the wave's integration replaces every stub and skip of its contract, and the milestone-end `CI.py` is strict, so none reaches main. `PreCommit.py --contract` and `CI.py --contract` stay available, but no gate run of the program uses them.
4. **Review.** One review pass per wave: blocker and major findings are verified before they are fixed, every confirmed finding is fixed at once, minors and nits included (never batched or deferred), and there are no rechecks of rechecks. A contract checkpoint gets one review of the frozen interfaces; the wave's one review pass covers the integrated wave diff; neither is rechecked. An unfixed confirmed finding of any severity, a nit included, blocks the commit.
5. **Milestone end.** One review pass of the milestone diff with every finding fixed, then one `CI.py` on the final tree; main fast-forwards to the milestone branch and is pushed only after that run is green, then the work pauses for the product owner's go-ahead.
6. **Outside a milestone.** Commits straight to main outside a milestone keep ADR 0023's rule: `PreCommit.py`, or for documentation-only edits the checks that read the edited files.
7. **Trailer.** A checkpoint commit's `Reviewed:` trailer names its targeted runs instead of a gate: `Reviewed: <outcome>; <n> findings (<n> fixed); checkpoint (<runs>)`, with `contract mode` for a contract checkpoint.
8. **Unchanged:** ADR 0023 decisions 2 to 5 (review before the gate, documentation edits after a green gate, agents iterate with targeted runs, a red gate reruns only its failed stages); parity; determinism across Debug, Release and Dist; testability without a window, GPU or audio device.
9. **What this replaces:** the M16/M17 contract commits through `PreCommit.py --contract`, integration commits through plain `CI.py`, ordinary-commit fix batches inside a milestone, and multi-reviewer passes with rechecks.

### 12. The gate scope

The milestone-end `CI.py` adds `--milestone` (the hardening stage: perf, soak, ASan, fuzz) only for a milestone that touches core systems, file readers (fuzzed formats) or the renderer, as its plan or contract decides; any other milestone ends with `CI.py --require-clang-cl` without `--milestone`.

- **Supersedes ADR 0024 decision 19 in part:** its rule that the commit completing a milestone runs `CI.py --milestone` now holds only under this scope. `CI.py`'s own behaviour is unchanged: `--milestone` still selects every stage plus `hardening` and requires the clang-cl builds, and its combination rules stand.
- **`--require-clang-cl` is the lead's addition.** The owner's words are "plain CI.py"; the flag keeps ADR 0024's clang-cl requirement (the portability stage's clang-cl Release and Dist builds) at every milestone end, hardening or not.
- **The program's milestones:** M18 yes (the renderer, Graphics); M19 yes (Graphics, the renderer, the hosts); M20 yes (the renderer, Scene, Physics, new readers); M21 no, because its shared-code edits are compile-portability fixes that leave Windows behaviour unchanged and its other changes are POSIX-only code and tooling, and its contract switches it to `--milestone` if the triage changes the behaviour of core systems, a fuzzed reader or the renderer; M22 yes (Scene, Scripting, Session, new readers); M23 yes (the renderer, new readers); M24 decided in its plan.
- **The documents are amended now; M18 lands the code texts** (T-GATESCOPE, W18.1). This ADR's change states the checkpoint and milestone-end gates in Architecture §2.3's `CI.py` and `PreCommit.py` rows and §15.1's tier gates and in the build-and-test skill's commands block. W18.1 changes only what the scripts print. `CI.py`'s module docstring, `--milestone` help text and summary not-run note (`MILESTONE_ONLY_NOTE`) name this rule. The contract-mode texts (`CONTRACT_MODE_NOTE` and `CONTRACT_FLAG_HELP` in `Scripts/Lib/scripts.py`, `PreCommit.py`'s docstring, the help of `Lint.py --allow-contract-stubs` and of `Test.py --allow-skips`) stop saying "only for the commit of a milestone's contract task": they describe contract mode as a contract checkpoint's targeted runs (decision 11), and `PreCommit.py --contract` and `CI.py --contract` as available but unused by the program's gates.

### 13. Tests

Tests per behaviour, not per message string: no exact-message pin unless the message is itself a frozen contract; every real bug fix keeps a regression test that failed before the fix; every existing test is kept. The suite grows more slowly. Parity still requires a Python test for every new method and FeatureTest coverage for every new component, field, enum value, setting, format, script API or method in every run mode it supports.

### 14. Numbering and pace

- **Numbering.** M18 to M23 for the program, Rolling Ball M24. The living documents (Roadmap, Architecture, Backlog, AGENTS.md, the skills) use the new numbers; the historical ADRs stay as written, so "M18" there means Rolling Ball, now M24. The failure messages in `games.py`, `Test.py`, `AuditProvenance.py` and `test_games_suite.py` that cite "Roadmap M16 to M18" or "M17 and M18" change in W18.1, with their tests; the documents' citations (Architecture §2.1's layout and §2.3's `Test.py` row, the build-and-test skill's games-suite paragraph) use M24 from this ADR's change.
- **Pace.** Milestones run one at a time, in number order, with a pause after each one for the owner's go-ahead.

### 15. Records

The plan's engineering defaults (revision 3, §6) stay in the plan, and each goes into its milestone's own record: one ADR per milestone, from 0029, amended per wave. A wave's ADR that this record or the plan names (W22.2's parity ADR, W22.5's and W23.3's ADRs, W23.6's temporal-state ADR) is that wave's section of its milestone's ADR, not a separately numbered one. A contract may change a default only with a stated reason. Kept limits a milestone accepts go to Backlog §6 through its ADR.

## Won't do

Each entry is also in `Docs/Backlog.md`'s "Won't do" section, with this ADR as its source. Reopening one needs a new decision by the owner.

| Item | Reason |
|---|---|
| Render thread | The owner: "Cut the AAA items". No CPU-bound frame has been measured, and it would break §4.11's main-thread rules and the synchronous capture path automation uses |
| Bindless rendering | AAA cut. It pays off only at draw-call scale or with GPU-driven rendering; M18's material-set fix removes the binding-set churn |
| GPU-driven rendering | AAA cut. The games draw hundreds of objects, not millions |
| Render-graph aliasing | AAA cut. It needs a render graph, which the engine deliberately does not have; M18's stage split is enough for adding passes |
| Graphics backends other than NVRHI-Vulkan (D3D12) | AAA cut. With the platforms out of scope, another backend runs only where Vulkan already runs, and it doubles the GPU and golden matrix |
| Dynamic GI (lightmaps, SSGI, DDGI, RT GI) | AAA cut. Device-dependent noise and loose golden thresholds; M23's baked probes cover the visible need |
| Ray tracing (foundation and effects) | A declined extra. It would run on one machine only, and no game needs it |
| Registry-based Play copy | A declined extra. It breaks §1.3 principle 3 ("one scene-copy path (the serializer)"); M22's faster serializer copy stays the only path |
| NVRHI push descriptors and line rasterization | The vendored rule: NVRHI `main` equals the pin `6b96fb0`, and pull request #158 is unmerged. Reopen line rasterization (fixed widths only) if #158 or similar work merges; push descriptors have no upstream path |
| Goldens on GPU classes other than nvidia-61x | A kept limit: there is no other hardware, and the driver's major version is held (decision 9) |
| Consoles | The owner: won't do; there is no SDK access |
| Identity code signing and notarization | The owner: won't do; macOS bundles stay ad-hoc signed |

## Risks

1. **One full gate per milestone.** A defect an early wave introduces into a stage no targeted run covered surfaces only at the milestone end. A wave therefore runs the suites its change touches, the export, determinism and games suites included when it changes what they test.
2. **Large milestones.** M20, M22 and M23 hold about 40 to 50 sessions each, so a pause comes rarely and a design problem found late costs more. Waves keep the integration points small.
3. **The renderer split (M18)** restructures the function every pass goes through; zero diffs on all 25 goldens prove it.
4. **The recreation protocol (M19)** must reach every later GPU owner; a missed owner is a crash only an injected loss finds.
5. **Box2D (W20.3)** is a second physics engine with its own determinism story; if it fails the four-run proof, W20.3's 2D physics and W20.6's tilemap collision run on Jolt plane locks behind the same interface (decision 6), so M20 continues, and Box2D goes to the owner at M20's boundary.
6. **The native module (W22.5)** runs unsandboxed code in the editor and needs a toolchain at export; the user-only preference, the fixed-path build and its ADR bound it.
7. **M21's triage** covers everything since M15, including M20's new code and its three new libraries (Box2D, HarfBuzz and SheenBidi).
8. **The accepted `Invariant` limit:** the prepass and forward pipelines agree only through the `NoContraction` products.
9. **A driver major update** would rename nvidia-61x; the owner holds the driver, and M18's check reports a rename.
10. **Calendar:** Rolling Ball, the next proof of AI control, moves back by the whole program.

## Before program completion

- [ ] M18 to M23 each closed by one review pass of its diff with every finding fixed and one `CI.py` on its final tree (with `--milestone` where decision 12 applies), main fast-forwarded and pushed, and the owner's go-ahead given before the next.
- [ ] Every built item meets its acceptance; R-GPUCRASH and R-ARENA are closed by their evidence; every Backlog row shows its outcome, and Backlog §2 to §7 hold only out-of-scope rows, accepted limits (the kept limits each milestone recorded included) and pointers to the "Won't do" section.
- [ ] Device-loss recovery covers what decision 5 lists, and what stays fatal is in Backlog §6 with its reasons.
- [ ] Tetris, rebuilt in place as pixel art, passes the games suite's three halves and the provenance audit from a fresh clone.
- [ ] The Linux and macOS jobs were green after M21 and kept green, or their remaining failures are open catch-up rows.
- [ ] Goldens are still nvidia-61x only with the driver's major version held, and every golden change was reviewed and named.
- [ ] Every approval used has its pin and licence committed (`VENDOR.md`, `FetchAssets.py`, the licence lists), and its `0001-approvals.md` row points to them.
- [ ] Architecture (§1.2, §2.3, §15.1, §15.8, §15.9, §17, §18 and Appendix C), the Roadmap, `AGENTS.md`, the skills and the Backlog describe what landed.
- [ ] Rolling Ball (M24) starts only on the owner's go-ahead.

## Amendments

This ADR amends, in the same change:
- **Roadmap:** the introduction, rules 1, 3, 4 and 6 and a new rule 7, the dependency graph, M18 to M23 replacing the Rolling Ball section, and Rolling Ball as M24 with its content unchanged.
- **`Docs/Backlog.md`:** §1 lists M18 to M24, every §2 to §7 item gains its milestone or outcome, and a new "Won't do" section.
- **`Docs/Decisions/0001-approvals.md`:** one row per approved item, and the push policy's `ci/**` exception (decision 10).
- **`AGENTS.md`:** milestone-workflow rules 1, 3 and 4, the commit gate (rules 1 to 5, the milestone-commit paragraph and "Remote CI is non-blocking"), the "Commit gate" and "Full CI" command rows, and the Backlog row of the repository map.
- **Architecture:** §1.2 (scheduled items marked, cut items left as non-goals), §2.1's layout (Rolling Ball as M24), §2.3's `Test.py`, `CI.py` and `PreCommit.py` rows, §15.1's tier gates, §15.8 and §15.9 (the gate, review and hardening wording), §17 and §18, and the Appendix C rows the program schedules or decides.
- **`Docs/ReviewChecklist.md`** and the `commit-review` and `build-and-test` skills: the sentences that state the gate, review, contract, push and hardening rules (the build-and-test skill's commands block and games-suite paragraph included).
- **Superseded in part:** ADR 0024 decision 19 (decision 12), ADR 0026 decision 1's deferral (decision 10), and the M16 and M17 process rules for milestones from M18 on (decision 11). Those ADRs stay as written.
