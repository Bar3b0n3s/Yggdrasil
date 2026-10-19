# 0028 — The backlog program (M18 to M26) before Rolling Ball

- **Status:** accepted. It records the product owner's decisions of 2026-10-19 on the backlog program and amends the standing documents to match (see "Amendments"). It is documentation only: no code changes with it, and the first code change of the program is the setup commit (decision 16). Lead-owned.
- **Revised 2026-10-19 after the owner's follow-up decisions (no Box2D, no Linux/macOS catch-up, GitHub as backup only, parallel milestones).** The first version (`eb8933f`) scheduled six milestones run one at a time, M18 to M23, with Rolling Ball as M24. This revision replaces that schedule in place, keeps this record's number and the owner's words of every round, and holds phase 1's reservation ledger. M18's own ADR stays 0029.
- **Revised again the same day after the owner's answers to the plan's three open points (the third round):** Windows backends only (decision 10), hardening only at each phase's last lander (decision 12), and the fuzz count unchanged (decision 16).
- **Date:** 2026-10-19.
- **Decided by:** the product owner, 2026-10-19, in three rounds; recorded by the lead.
- **Base:** `cdf35ef` (M17 complete, ADR 0027); revised on `eb8933f`.
- **Authority:** Roadmap "Rules for every milestone"; Architecture §1.1, §1.2, §15.8, §15.9, §16, §17, §18 and Appendix C; ADR 0004, ADR 0011, ADR 0013 decision 25, ADR 0023, ADR 0024 decisions 14 and 19, ADR 0025 decisions 5 and 18, ADR 0026 decision 1 and ADR 0027. The lead's plan (`bin/Reviews/Backlog-Plan.md`, revision 5: its phases, milestones, waves, streams, acceptance, engineering defaults, ledgers, risks, facts and item appendix) and the first draft's completeness critic (`bin/Reviews/Backlog-Plan-Critic.md`) are kept under the ignored `bin/Reviews/`; the schedule critic's 24 issues are recorded only in the plan's §11 revision log.

## Context

- **Tetris is rendered as 3D cubes.** M17 built `Projects/Tetris` through automation, but the engine has no 2D path, so every cell is a MeshRenderer cube under an orthographic camera, and the HUD is Screen-space `TextComponent` text, because there is no UI framework beyond `TextComponent` (§1.2).
- **The UI gap.** A game's menus, buttons, scroll views, text input and localized text all need a UI framework that v1 lists as a non-goal; Rolling Ball and any later game would hit the same gap.
- **The owner's choice.** On 2026-10-19 the product owner chose to complete the engine before Rolling Ball: the whole backlog (`Docs/Backlog.md` §2 to §6, and §7 except the Platforms row and networking) plus 2D rendering and a UI framework, all before Rolling Ball. The owner also chose to simplify: milestones are a made-up unit, so the plan's numbered groups became milestones (the same work, fewer milestones), with a lighter process than M16's and M17's.
- **The plan.** Revision 2 of the lead's plan sequenced 124 items over twenty milestones with eight questions to the owner. The owner answered every question, restructured the program into six milestones and declined the toolchain bump; revision 3 applied that, and this record's first version (`eb8933f`) recorded it.
- **The follow-up.** After that commit, the same day, the owner decided four more things:
  - "I dont see why we need to bother with box2d at all".
  - "github means nothing to use, all that matters is that we have a git". The owner chose to drop the Linux/macOS catch-up ("Drop the catch-up") and to keep pushing main to GitHub after each phase, purely as an off-site backup of the repository.
  - "we should run as many milestones as possible in parallel".

  Revision 4 of the plan applies them, with the scheduler's parallel schedule and its critic's findings: 131 items (revision 3's 127 plus four setup items the critic's review added), of which 105 are built.
- **The open points.** Revision 4 left three points to the owner, who answered them the same day (the third round):
  - the kept items' non-Windows halves: "Windows backends only";
  - hardening once per phase: "Last lander per phase";
  - the fuzz iteration count for heavy readers: unchanged.

  Revision 5 of the plan applies them: 134 rows (three non-Windows halves split off as rows of their own when the owner dropped them), of which 104 are built.

## Decisions

### 1. Scope

In scope, all before Rolling Ball:
- `Docs/Backlog.md` §2, §3, §5 and §6 (§4, the Linux/macOS catch-up, is dropped by the owner, decision 10);
- §7 except the Platforms row and networking;
- 2D rendering and a UI framework, the two new capabilities;
- the extras the owner kept or chose: 2D lighting, world-space UI, a separately vendored `stb_truetype.h`, RTL and complex-script shaping, a per-game native C++ gameplay module with editor-only hot reload (on Windows, decision 10), and visual scripting (a node graph compiling to the deterministic Luau runtime). Native file dialogs on Linux, which the first round kept, are dropped by the third round ("Windows backends only", decision 10).

Out of scope, in the owner's words:
- **The Platforms row:** mobile, consoles, web, native Wayland, cross-OS export, bit-exact cross-OS or cross-compiler determinism, identity code signing and notarization ("We are not doing this right now"). Consoles and signing are also won't-do (decision 2); the other five rows stay in the Backlog.
- **Networking:** "Networking can stay in the backlog for now".
- **Box2D**, which the first round kept beside Jolt: "I dont see why we need to bother with box2d at all". It is won't-do (decision 2), and 2D physics runs on Jolt (decision 6).

An in-scope item is dropped only on the owner's yes, at a phase boundary.

### 2. The cuts

Won't do, by the owner's decisions:
- **The AAA items** ("Cut the AAA items"): the render thread, bindless rendering, GPU-driven rendering, render-graph aliasing, graphics backends other than NVRHI-Vulkan (D3D12 among them) and dynamic GI.
- **Declined extras:** ray tracing and the registry-based Play copy.
- **The vendored rule:** NVRHI push descriptors and line rasterization. Upstream `main` equals the pin `6b96fb0`, and the pull request that adds line rasterization (#158) is unmerged; vendoring it would edit vendored source.
- **A kept limit:** goldens on GPU classes other than nvidia-61x. There is no other hardware, and the driver is held (decision 9).
- **Consoles, and identity code signing and notarization.**
- **Box2D** (the follow-up): "I dont see why we need to bother with box2d at all".

Dropped by the owner (the follow-up): the Linux/macOS catch-up, the old M21's five items (L-TRIAGE, L-M29, L-M30, L-MACBUNDLE, L-CIGAPS), which cover Backlog §4's five rows (decision 10). Dropped by the owner (the third round, "Windows backends only"): the kept items' Linux and macOS backends, E-DIALOGS-LINUX, E-DIALOGS-MACOS, E-VIEWPORTS-POSIX and C-NATIVE-POSIX (decision 10).

The "Won't do" and "Dropped by the owner" lists below give each item's reason, and `Docs/Backlog.md` carries the same entries.

### 3. The toolchain stays

The owner (2026-10-19): "I'm not sure updating vulkan will make a difference, especially since it's a minor version". The program stays on Vulkan SDK 1.4.350.0 and slangc 2026.8 (`Scripts/Lib/Toolchain.json` unchanged).

- The bump's only reason was the Backlog §3 row "`Invariant` on the clip position, which Slang 2026.8 cannot decorate" (ADR 0013 decision 25, ADR 0025 decision 18). Its `NoContraction` workaround keeps the prepass and forward pipelines in agreement, so the row becomes an accepted limit (Backlog §6).
- It is revisited only if an in-scope item ever needs a newer toolchain. That need then goes to the owner as an approval request at a phase boundary, never mid-milestone.
- No golden is regenerated for a toolchain change, and M18's renderer split is proven against main at the setup commit, which changes no rendering.

### 4. Structure: a setup commit, three phases of parallel milestones, then Rolling Ball

The owner: "we should run as many milestones as possible in parallel". Milestones are a made-up unit, so the plan regroups revision 3's waves into nine milestones that run three at a time, the same work minus the dropped items. A setup commit on main comes first (decision 16); then three phases, M18 to M26; then Rolling Ball as M27 (phase 4). The plan lists each milestone's waves, contracts, streams, dependencies, acceptance and size.

| Phase | # | Name | Goal | Lands | Hardening | Size |
|---|---|---|---|---|---|---|
| Setup | — | Program setup | The shared vendor-library cache, machine-wide build, GPU and gate locks, the fuzz budget from measured costs, the IdSeed guard, the gate-scope texts, the prefab-member deletion refusal, `ci.yml` started only by hand | before phase 1 forks | no: `CI.py --require-clang-cl` plus `Hardening.py --steps asan-generate,asan-build` | ~4.5 sessions |
| 1 | M18 | Renderer foundations, device-loss recovery and native dialogs | The renderer split into stages with frozen per-pass recreate and GPU-zone hooks; shader hot reload; GPU-lifetime instrumentation and the GPU-crash follow-up; profiler export and GPU merge; the per-frame arena measured; the material-set fix; FeatureTest golden provenance, the golden device-class check, the rename procedure and the golden scaffolds; the QuadBatcher with 9-slice, the output stage and the SpriteSheet asset; recovery from device loss and driver-reset hangs, and from GPU out-of-memory as far as feasible (decision 5); native file dialogs on Windows through nativefiledialog-extended's Windows backend (decision 10) | readiness order with M19 | no: `CI.py --require-clang-cl` | ~17 |
| 1 | M19 | Automation, export, Luau and the native module | Automation, replay v3 and export basics; pak compression and reachability pruning; the Luau compiler out of Dist; `stb_truetype.h`; the per-game native module with editor-only hot reload, on Windows (decision 10); the carried character | readiness order with M18 | no: `CI.py --require-clang-cl` | ~20, plus one bridge session |
| 1 | M20 | Scene model and physics semantics | Physics and session semantics with the watch items; deleting prefab members and live nested prefabs; multiple open scenes; multiple scripts per entity | last in phase 1 | yes: phase 1's `CI.py --milestone` | ~20, plus one bridge re-record |
| 2 | M21 | 2D | Sprites, sorting, animation, the pixel-perfect camera, the 2D editor mode, 2D physics on Jolt, 2D lighting, tilemaps | readiness order with M22 | no: `CI.py --require-clang-cl` | ~14 |
| 2 | M22 | UI and text | The UI framework, static and interactive; text input; localization with HarfBuzz and SheenBidi; world-space UI | readiness order with M21 | no: `CI.py --require-clang-cl` | ~19.5 |
| 2 | M23 | 3D rendering and assets | Instancing, point-light shadows and clustered lights; mesh v2, EXR, Opus and BC1–BC7 compression including BC6H (CMP_Core); skeletal and morph animation | last in phase 2 | yes: phase 2's `CI.py --milestone` | ~21 |
| 3 | M24 | Visual scripting and editor windows | Visual scripting; ImGui multi-viewport OS windows on Windows (decision 10) | any time | no: `CI.py --require-clang-cl` | ~14.5 |
| 3 | M25 | Tetris rebuilt in place as pixel art | Tetris rebuilt by builder sessions on the 2D path and the UI framework (decision 8) | before M26 | no: `CI.py --require-clang-cl` | ~4, plus 2–3 builder sessions |
| 3 | M26 | Effects, post-processing and terrain | Particles and baked light probes; auto-exposure, colour-grading LUTs, TAA and SSR; terrain | last in phase 3 | yes: phase 3's `CI.py --milestone` | ~19, plus one bridge re-record |
| 4 | M27 | Rolling Ball 3D | The Roadmap's Rolling Ball section, content unchanged, renumbered | after phase 3 | decided in its plan | as M17 |

About 154 agent sessions plus 5–6 bridge sessions, then M27; nine milestone-end gates, three of them hardened (decision 12): about 15.4 h of gate time at M17's measured hardening time instead of about 28 h, perhaps about 16.5 h once the fuzz step's growth and M18's debug-heap step are counted, plus the setup gate.

### 5. Device-loss recovery: what is recovered and what stays fatal (M18)

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

M18's ADR moves the remaining limits to Backlog §6 and rewrites §8.14 items 6 to 8.

### 6. Design defaults of the kept items

- **2D physics on Jolt (M21).** 2D physics and tilemap collision run on Jolt: bodies with plane (degree-of-freedom) locks behind the `Physics2DWorld` interface, deterministic like the 3D world, with no new library. The default is a second Jolt world whose dynamic bodies allow only X and Y translation and Z rotation (`PhysicsDofs`, which maps bit for bit to Jolt's `EAllowedDOFs`), whose shapes are the existing shape variant laid in the XY plane (so the frozen M11 `PhysicsShape.h` does not change), and which has its own step phases and its own state-hash block after the 3D one, so a change to the 3D world never moves a 2D hash. An entity belongs to one world; holding both 2D and 3D physics components is a validation error. Its contact events use M20's canonical sort key. Like any simulation feature, the Physics2D replay is identical in all four runs and fails without its input. This replaces the first version's Box2D default, its four-run adoption proof and its Jolt fallback.
- **The output stage and the shared 2D primitives (M18).** Before any 2D or UI milestone forks, M18 lands the QuadBatcher with 9-slice expansion, the samplers, the SpriteSheet asset (frames, pivots, 9-slice borders and clips as data) and the output stage: a scene resolution separate from the view, an integer upscale with point sampling and a letterbox, the view-to-scene pixel mapping for picking, rays and capture, game Text and the UI slot drawn at scene resolution before the upscale and the editor overlays after it, UI laid out and hit-tested in scene pixels, and a 2D colour path that leaves sprite texels unchanged. Sibling milestones cannot share a contract, so these interfaces land, implemented, one phase before their users.
- **The native module (M19): static linking, editor-only hot reload.** One module per game, in `<Game>/Native/`, built with the first-party settings (precise FP, warnings as errors) and the determinism lint, and statically linked into that game's exported Runtime in Release and Dist; a game without one exports the prebuilt Runtime unchanged. Only the editor hot-reloads it, as a shared library swapped at a frame boundary while no play session runs; exports never load code at run time. Native systems read the latched step input as scripts do. Native build and load need the user-only "Allow native code" preference, which automation cannot set. M19's ADR amends §13.2's process allowlist (the fixed-path native build) and §14.2's rule that the editor never spawns builds, for games with a native module. Windows backends only (decision 10): the native build, the editor's shared-library load and hot reload (a new `Platform/` shared-library interface, which the platform layer lacks today) and the export of a game with `Native/` have a Windows backend only; on Linux and macOS, `native.*` build and load, and exporting a game that has `Native/`, return `ErrorCode::Unsupported` naming the platform through the platform layer's error path, and a game without `Native/` is unaffected. No dlopen backend and no make, GCC, Clang or Xcode native-build path is written.
- **The Luau compiler out of Dist through a first-party require (M19).** A first-party require replaces Luau's Require library in every configuration, with the same cache, cycle and yield-continuation behaviour, and removes both `luarequire_*` calls, so Config and the Compiler no longer link into Dist (Parser and Ast stay for `parseType`). No vendored source is edited, signature validation stays in Dist, and M19's ADR replaces ADR 0025 decision 5's premise and adds a LuauCompiler family to the map check.
- **Visual scripting (M24).** A graph is one behaviour, transpiled to Luau text at import and compiled by the normal script pipeline, so it is deterministic and Dist needs no compiler for it. Every graph action has an automation method.
- **2D lighting (M21) and world-space UI (M22).** Sprites stay unlit by default and opt in to 2D lights; world-space canvases share the screen canvases' layout and hit-test code and take pointer rays through the camera from latched input.

### 7. Approvals

Every library and asset the program needs is approved up front (Roadmap rule 6), recorded in `Docs/Decisions/0001-approvals.md` with its upstream source, licence, destination and purpose: tinyexr (with miniz only if its build needs it), libogg, libopus, opusfile and miniaudio's unmodified libopus decoder with one Ogg Opus test fixture, Compressonator CMP_Core (bc7enc_rdo and bcdec as fallbacks), HarfBuzz and SheenBidi, nativefiledialog-extended (for its Windows backend only, decision 10), `stb_truetype.h`, and anything else an in-scope item needs from official upstream sources. The follow-up removed the Box2D row (decision 2) and the GitHub CLI row (decision 10). There is no Vulkan SDK or slangc row (decision 3). Versions are pinned at the contract of the wave that vendors each item, with checksums and licences committed under the standing rules.

### 8. Tetris rebuilt in place as pixel art (M25)

Tetris is rebuilt in place in `Projects/Tetris`, as pixel art under a pixel-perfect camera, by builder sessions through the MCP tools only, in M25, at the end of the 2D/UI work. The 3D build stays in the index and git history until the finished pixel-art build replaces it in one checkpoint; the builder starts from an emptied working-tree folder, with a backup after each session (ADR 0027 decision 11's rule). M25 merges main only after the builder's last session, keeps its own side of `Projects/Tetris` at that merge, and redoes main's canonical upgrades on the rebuilt project with `Editor --headless --project Projects/Tetris --upgrade`. The acceptance is M17's feature list on sprites and UI, with no MeshRenderer cells and no Screen Text HUD, its replays strict-hash identical in both editors and the testing Dist export, its suites green, `StartScene` and `Export.BuildScenes` set, `project.validate` clean and the Dist export smoke exiting 0 (the games suite's three halves), and the provenance audit passing from a fresh clone.

### 9. Goldens: one device class, the driver held

Goldens stay on the nvidia-61x device class only, and the NVIDIA driver's major version is held for the whole program. M18 adds the device-class check and the rename procedure, so an accidental driver update is reported instead of silently dropping to smoke mode. Only M23's texture compression regenerates existing goldens, on phase 2's merged tree and including every golden of M21 and M22 it changes, plus M19's reviewed Text regeneration if the `stb_truetype.h` switch changes the atlases and M18's recorded regeneration of FeatureTest's golden files with provenance. The editor-UI goldens (`EditorDefaultLayout` and the five `EditorDesign` images) are also regenerated: by a lander that changes the editor shell, and on each phase's merged tree by M20, by the later of M21 and M22 and by M24 (plan §4.3), each regeneration named in its milestone's record. Every other milestone only adds goldens, through M18's golden scaffolds, and any other change is a reviewed regeneration named in its milestone's record.

### 10. Windows only; GitHub as an off-site backup

The owner: "github means nothing to use, all that matters is that we have a git"; the owner chose "Drop the catch-up" and to keep pushing main after each phase as a backup; in the third round the owner answered "Windows backends only".

- **The catch-up is dropped.** The first version's M21 (the Actions failures per compiler family, the POSIX automation connection, the ELF map check, the macOS bundle, the hosted feature and games stages) and its "kept green" rule are dropped by the owner: there is no Linux or macOS machine without GitHub. Windows is the only verified platform. First-party code stays portable by rule (the `Platform/` layering, no Windows-only shortcuts, the local portability stage's premake generation and clang-cl builds), but Linux and macOS are never verified: the Linux/macOS catch-up was dropped by the product owner, because there is no Linux or macOS machine without GitHub. This supersedes ADR 0026 decision 1's deferral and replaces ADR 0011's remote-CI role.
- **GitHub as an off-site backup.** GitHub is only an off-site backup of the repository: GitHub Actions is not part of the process, and nothing reads or waits for its results. Main is pushed to `origin` once per phase, after the phase's last milestone has landed (decision 14). There is no gh CLI, no log access, no `ci/**` branch and no push before review: only main is pushed. From the program's setup commit, `.github/workflows/ci.yml` runs only when started by hand (`workflow_dispatch`), and no step of the process starts it (T-CIMANUAL, the lead's addition, so a backup push starts no failing run).
- **The kept items' platform backends** (the third round; the owner: "Windows backends only"). Windows backends only: a kept item's platform interface stays, so a later port can add backends, but no Linux or macOS backend is written, and on Linux and macOS the interface reports the capability as unavailable (`ErrorCode::Unsupported`) through the platform layer's existing error path. Vendored libraries' premake scripts stay portable (the portability stage generates gmake and xcode4 projects from them); their GCC, Clang and Apple Clang builds are never compiled.
  - **Scope.** The rule covers the platform capabilities the program adds in first-party C++. Existing POSIX backends (`Platform/Posix/`) keep compiling when their interface changes, and the Python tooling keeps its guarded POSIX branches (the `fcntl` lock beside each `msvcrt` one, T-LOCKS's included; `Docs/ReviewChecklist.md` §11).
  - **Dropped by the owner:** E-DIALOGS's macOS backend (E-DIALOGS-MACOS); E-DIALOGS-LINUX, with nativefiledialog-extended's xdg-desktop-portal backend, `libdbus-1-dev` in `Setup.py`'s Linux package check and the Linux-only text of its approval row; E-VIEWPORTS on Linux (X11) and macOS (E-VIEWPORTS-POSIX); and the native module's build, shared-library load and export on Linux and macOS (C-NATIVE-POSIX). E-DIALOGS, E-VIEWPORTS and the native module are built for Windows only.
  - **The behaviour on Linux and macOS.** The FileDialog API's native backend returns `ErrorCode::Unsupported` naming the platform, as `ResourcePatcher::Patch` does there, and the API falls back to the ImGui picker, exactly as it does headless (M18). `native.*` build and load, and exporting a game that has `Native/`, return `ErrorCode::Unsupported` naming the platform, the shared-library interface's POSIX file returning it as `ResourcePatcher::Patch` does, and the editor treats the module as not loaded, as it does while the "Allow native code" preference is off; a game without `Native/` is unaffected (M19, decision 6). `ImGuiLayer` keeps `ImGuiConfigFlags_ViewportsEnable` cleared, as today, so every panel stays docked in the main window, and the editor's platform-window support reports `ErrorCode::Unsupported` through the same error path (M24).
  - **nativefiledialog-extended stays, for its Windows backend only** (its wrapper of the COM `IFileDialog`), rather than a first-party `IFileDialog` implementation: it was approved for Windows, macOS and Linux, not only for the Linux backend; M18's FileDialog API (C18.1) is built over it; and a later port can add its macOS and portal backends under the kept interface without new first-party platform code. `Vendor/nativefiledialog-extended/premake5.lua` defines its project on every OS, so the portability stage's per-configuration project sets still match the vs2026 reference; only its Windows branch lists a backend source (`nfd_win.cpp`), its Linux and macOS branches list no backend source and link no system library, the editor links the library only under `filter "system:windows"`, and its `VENDOR.md` records the backends left out of the build.

### 11. The lighter process

These rules replace the M16/M17 rigor for every milestone from M18 on:

1. **Branch, worktree and waves.** Milestones run in phases: the milestones of a phase are developed in parallel, each on its own branch in its own worktree, and their milestone-end `CI.py` runs go one at a time on the one GPU. Inside a milestone, waves of streams in worktrees integrate into the milestone branch, in the plan's order; a wave starts when the waves it depends on are integrated. Shared integration files have one owner per wave inside a milestone; milestones of one phase each change them on their own branch, and a later lander appends its entries to main's at its merge, in the order the phase's reservation ledger fixes, never reordering them. This amends Roadmap rule 4 and AGENTS.md's milestone-workflow rule 4.
2. **Contracts.** A contract only where a milestone or wave freezes shared interfaces; no contract task otherwise. A contract freezes headers with documented signatures, stubs that start with `ENGINE_CONTRACT_STUB();` and return `Unsupported`, and tests marked `doctest::skip`, and records its decisions in the milestone's ADR. Changing a frozen header needs the contract owner's review. A contract exists only inside its milestone: stubs never reach main, so an interface is frozen for another milestone only by landing, implemented, in an earlier phase; siblings share only their phase's reservation ledger (decision 17). This amends Roadmap rule 3 and AGENTS.md's milestone-workflow rule 3.
3. **Checkpoint commits.** Wave and contract commits are checkpoint commits on the milestone branch: each is reviewed and runs only the tests and suites its change touches, never `PreCommit.py` or `CI.py`. A contract checkpoint runs its targeted checks in contract mode (`Lint.py --allow-contract-stubs`, `Test.py --allow-skips`); the wave's integration replaces every stub and skip of its contract, and the milestone-end `CI.py` is strict, so none reaches main. `PreCommit.py --contract` and `CI.py --contract` stay available, but no gate run of the program uses them.
4. **Review.** One review pass per wave: blocker and major findings are verified before they are fixed, every confirmed finding is fixed at once, minors and nits included (never batched or deferred), and there are no rechecks of rechecks. A contract checkpoint gets one review of the frozen interfaces; the wave's one review pass covers the integrated wave diff; neither is rechecked. An unfixed confirmed finding of any severity, a nit included, blocks the commit.
5. **Milestone end and phase end.** Milestone end: the milestone branch merges current main, then one review pass of the milestone diff with every finding fixed, then one `CI.py` on the final tree; main fast-forwards to the milestone branch only after that run is green. Phase end: once the phase's last milestone has landed, main is pushed to `origin` as an off-site backup, and the work pauses for the product owner's go-ahead. The milestone ends of a phase follow its merge train (decision 17).
6. **Outside a milestone.** Commits straight to main outside a milestone keep ADR 0023's rule: `PreCommit.py`, or for documentation-only edits the checks that read the edited files. The setup lands like a milestone without a phase (decision 16).
7. **Trailer.** A checkpoint commit's `Reviewed:` trailer names its targeted runs instead of a gate: `Reviewed: <outcome>; <n> findings (<n> fixed); checkpoint (<runs>)`, with `contract mode` for a contract checkpoint.
8. **Unchanged:** ADR 0023 decisions 2 to 5 (review before the gate, documentation edits after a green gate, agents iterate with targeted runs, a red gate reruns only its failed stages); parity; determinism across Debug, Release and Dist; testability without a window, GPU or audio device.
9. **What this replaces:** the M16/M17 contract commits through `PreCommit.py --contract`, integration commits through plain `CI.py`, ordinary-commit fix batches inside a milestone, multi-reviewer passes with rechecks, and this record's first version's one-milestone-at-a-time pace with a push and a pause after each milestone.

### 12. The gate scope

The owner (the third round): "Last lander per phase". Within a phase, only the last lander's milestone-end `CI.py` adds `--milestone` (the hardening stage: perf, soak, ASan, fuzz), on the merged tree of all the phase's milestones, when any milestone of the phase touches core systems, file readers (fuzzed formats) or the renderer, as its plan or contract decides; every other milestone end runs `CI.py --require-clang-cl` without `--milestone`. The program's setup gate keeps its own rule (decision 16), and M27, alone in phase 4, decides in its plan.

- **This revision amends the rule of the first two versions,** under which every milestone end in that scope ran `CI.py --milestone`; hardening now runs once per phase, on the merged tree its last lander gates.
- **Supersedes ADR 0024 decision 19 in part:** its rule that the commit completing a milestone runs `CI.py --milestone` now holds only under this scope. `CI.py`'s own behaviour is unchanged: `--milestone` still selects every stage plus `hardening` and requires the clang-cl builds, and its combination rules stand.
- **`--require-clang-cl` is the lead's addition.** The owner's words are "plain CI.py"; the flag keeps ADR 0024's clang-cl requirement (the portability stage's clang-cl Release and Dist builds) at every milestone end, hardening or not.
- **The program:** the setup gate is `CI.py --require-clang-cl` plus `Hardening.py --steps asan-generate,asan-build` (tooling only; the ASan steps prove the vendor cache in the ASan workspace). Every milestone of phases 1 to 3 is in the scope: M18 (renderer, Graphics, hosts, the SpriteSheet reader), M19 (new readers, the Scripting require, a native step in Session), M20 (Scene, Physics, Session, Scripting, prefab readers), M21 (renderer, Physics, the Tilemap reader), M22 (renderer, the Session hash, StringTable and font readers), M23 (renderer, mesh, material, texture, environment and audio readers), M24 (the graph reader, the transpiler, a GPU owner), M25 (friction fixes in engine modules) and M26 (renderer, new readers, Physics). So the hardened gates are M20's (phase 1, on the merged tree of M18, M19 and M20), M23's (phase 2) and M26's (phase 3), and M18, M19, M21, M22, M24 and M25 end with `CI.py --require-clang-cl`. M25's acceptance includes the soak, so it runs `Hardening.py --steps soak` as a targeted step under the GPU lock right after its green plain gate, on the gated tree, before main fast-forwards: the soak's Runtime subject needs a current exported Release FeatureTest, which the gate's feature and export stages leave, and a soak narrowed with `--soak-only` ends as a warning. A soak failure is fixed on M25's branch, and the fixed tree is gated again. M27 decides in its plan.
- **A defect the hardened gate finds** (an ASan fault, a fuzz finding, a soak leak), in whichever milestone's code, is fixed on the last lander's branch with a regression test that fails before the fix, before main fast-forwards; the gate then reruns as ADR 0023 decision 5 says. A wave that adds a reader of external input still runs that reader's fuzz case in its targeted unit run.
- **Gate time:** three hardened gates (`CI.py --milestone`, about 9.3 h) and six plain gates (`CI.py --require-clang-cl`, about 1 h each, about 6.1 h), about 15.4 h instead of about 28 h for nine hardened gates, plus the setup gate (about 1.5 h) and M25's soak (about 20–30 min). These figures use M17's measured hardening time (97 min) and the fuzz model's 52-minute reference; the fuzz step's growth toward about 4,350 s as the program's new targets land and M18's debug-heap GPU step lengthen the later hardened gates, perhaps to about 10.5 h for the three (about 16.5 h for the nine gates), and T-FUZZMODEL's measurement at each phase end updates them.
- **The documents are amended now; the setup commit lands the code texts** (T-GATESCOPE). This record's change states the checkpoint and milestone-end gates in Architecture §2.3's `CI.py` and `PreCommit.py` rows and §15.1's tier gates and in the build-and-test skill's commands block. `CI.py`'s module docstring, `--milestone` help text and summary not-run note (`MILESTONE_ONLY_NOTE`) name this rule. The contract-mode texts (`CONTRACT_MODE_NOTE` and `CONTRACT_FLAG_HELP` in `Scripts/Lib/scripts.py`, `PreCommit.py`'s docstring, the help of `Lint.py --allow-contract-stubs` and of `Test.py --allow-skips`) stop saying "only for the commit of a milestone's contract task": they describe contract mode as a contract checkpoint's targeted runs (decision 11), and `PreCommit.py --contract` and `CI.py --contract` as available but unused by the program's gates.

### 13. Tests

Tests per behaviour, not per message string: no exact-message pin unless the message is itself a frozen contract; every real bug fix keeps a regression test that failed before the fix; every existing test is kept. The suite grows more slowly. Parity still requires a Python test for every new method and FeatureTest coverage for every new component, field, enum value, setting, format, script API or method in every run mode it supports.

### 14. Numbering and pace

- **Numbering.** The setup, then M18 to M26 in phase order (within a phase, a milestone that must land last takes the highest number), Rolling Ball M27. The living documents (Roadmap, Architecture, Backlog, AGENTS.md, the skills) use these numbers; the historical ADRs stay as written, so "M18" in the ADRs up to 0027 means Rolling Ball, now M27, and this record's first version (`eb8933f`) used M18 to M24 for the replaced six-milestone schedule. The failure messages in `games.py`, `Test.py`, `AuditProvenance.py` and `test_games_suite.py` that cite "Roadmap M16 to M18" or "M17 and M18" change in the setup commit (T-GATESCOPE), with their tests, to cite "Roadmap M16, M17, M25 and M27" and, for the build-settings rule, "M17, M25 and M27"; the documents' citations (Architecture §2.1's layout and §2.3's `Test.py` row, the build-and-test skill's games-suite paragraph) use them from this revision.
- **Pace.** Milestones run in phases: the milestones of a phase are developed in parallel, each on its own branch in its own worktree, and their milestone-end `CI.py` runs go one at a time on the one GPU. The pause comes after each phase, not after each milestone: once the phase's last milestone has landed, main is pushed to `origin` as an off-site backup, and the work pauses for the product owner's go-ahead. If a member cannot finish, the owner decides at the boundary whether to close the phase without it and carry its branch into the next phase; that is not a drop of scope.

### 15. Records

The plan's engineering defaults (revision 5, §6) stay in the plan, and each goes into its milestone's own record: one ADR per milestone, amended per wave: M18 0029, M19 0030, M20 0031, M21 0032, M22 0033, M23 0034, M24 0035, M25 0036, M26 0037, M27 0038. A wave's ADR that this record or the plan names (M18's dialogs parity section, M19's native-module and first-party-require sections, M24's undocking parity section, M26's temporal-state section) is that wave's section of its milestone's ADR, not a separately numbered one. A contract may change a default only with a stated reason. Kept limits a milestone accepts go to Backlog §6 through its ADR. The setup's decisions are decision 16 of this record.

### 16. Program setup

Before phase 1 forks, one setup branch lands on main like a milestone without a phase: streams in worktrees, one review pass, one gate, one fast-forward, no pause. Its gate is `CI.py --require-clang-cl` instead of PreCommit (which builds Debug only), plus `Hardening.py --steps asan-generate,asan-build`, because the vendor cache changes the link inputs of every configuration and both extra toolsets that phase 1 forks from. It holds:
- **T-VCACHE:** the shared prebuilt vendor-library cache, keyed by the ABI-relevant defines, each library's source pin and premake build settings (floating-point flags, Jolt's instruction set, the sanitizer), the toolset and the configuration; ASan and clang-cl get separate entries; entries are published atomically; `CheckBuildConfig.py` verifies each cached entry's recorded flags.
- **T-LOCKS** (the lead's addition): machine-wide locks in the git common directory, shared by every worktree: a counted build lock in `Build.py` (one uncapped build, or two capped through MSBuild's compiler count); a GPU lock for the GPU suites, `Scaffold.py`, `--update-golden`, `Perf.py`, `Hardening.py` and the MCP bridge (`Tools/MCP` holds it while it owns a rendering editor, from its launch until that editor exits, unless it runs under a process that already holds it, as in the automation suite); an exclusive gate lock in `CI.py` that `Build.py`, `Test.py` and the bridge's editor launch wait on.
- **T-FUZZMODEL:** the fuzz step sized from per-target costs measured in its JUnit report instead of one fixed reference, re-measured at each phase end. The owner (the third round) kept the count: every reader keeps ADR 0024 decision 14's 1,000,000 mutations; only the shards and the timeout follow the measured costs.
- **T-SEEDGUARD:** `Scaffold.py` refuses a duplicate IdSeed.
- **T-GATESCOPE:** the scripts' gate-scope, contract-mode and renumbered texts (decisions 12 and 14).
- **E-PFB-REFUSE:** `entity.destroy` and the editor's Delete refuse a prefab member that is not an instance root, with the hint to deactivate it or unpack the instance, and a regression test; M20's RemoveEntity later replaces the refusal with a recorded deletion.
- **T-CIMANUAL:** `ci.yml`'s `push` (`main`, `ci/**`) and `pull_request` triggers removed together with their `ci/**` comment, `workflow_dispatch` kept, and its header comments rewritten to say that it runs only when started by hand, that its results are never read and that Windows is the only verified platform (no "verified once its job here is green", no ADR 0011); its jobs and steps are otherwise unchanged (decision 10).

After the setup gate the lead provisions the worktree slots: each is created once, gets `Setup.py` (its own `Tools/MCP/.venv`) and a first build, and is then reused with `git switch -C`. Workflow scripts run each agent in its assigned slot, one agent per slot. Slot agents never use the engine MCP tools: the `.mcp.json` that `Setup.py` writes in each slot stays (`CI.py`'s setup stage rewrites it), but the workflow never enables a slot's project MCP server, so the only bridge is `D:\Engine`'s (decision 17).

### 17. Reservation ledgers and the merge train

- **The ledger.** Siblings in a phase cannot share a contract, so they share text: each phase's reservation ledger (format versions, ADR numbers, IdSeed ranges, AssetType names, built-in handles, PlaySessionPhase order, state-hash blocks, render-pass slots, settings sections and validation codes, and the one owner of each contended file). It is a section of this record, committed on main in a documentation-only change before the phase's branches fork; phase 1's is below, and phases 2 and 3 get theirs at their phase starts from the plan's drafts. A contract follows its ledger; a deviation needs a ledger amendment on main.
- **The merge train.** Phase 1: M18 and M19 in readiness order, then M20. Phase 2: M21 and M22 in readiness order, then M23. Phase 3: M24 at any time, M25 before M26, M26 last. Each milestone in turn merges current main (`git merge`, never rebase; milestone branches never merge into one another), redoes what the plan's protocol lists (registries appended by the ledger, its own AssetType values, generated files, FeatureTest, fixtures, the demos' recorded operations, the last lander's adaptation of its siblings' code, goldens), runs the targeted suites the merge touched, takes its one review pass and its gate, and fast-forwards main.
- **The calendar.** Gates run one at a time. The hardened gate, the last lander's, runs at night on a quiet machine (the fuzz needs at least 14 GiB of available memory); an earlier lander's plain gate (about 1 h, no hardening) needs the gate lock but no quiet machine, so it runs as soon as its lander is ready, and the next lander merges main at once. The lander merges, redoes and reviews by day. A bridge window holds the GPU lock while its editor renders, so it runs by day and the siblings' GPU runs wait for it. A phase end is about 13–16 h of serialized machine time for the merges, redo and re-record, plus two plain gates of about 1 h and one night for the hardened gate; phase 3 adds M25's soak (about 20–30 min) after its plain gate.
- **Limits.** At most three milestones develop at once, with about twelve stream agents and at most sixteen workflow agents; one build uncapped or two capped, one GPU run and one gate at a time (T-LOCKS); one bridge user, in `D:\Engine`.

## Phase 1 reservation ledger

Committed with this revision, before phase 1 forks.

**Format versions** (today's values at `eb8933f`):

| Format or version | Today | Owner | Reserved |
|---|---|---|---|
| `ReplayFormatVersion`, `ReplayImporter::Version` | 2, 2 | M19 | 3, 3; fixture `Formats/Replay/v2` and its `.upgraded` |
| `PakFormat` header and TOC `CurrentVersion` | 1 | M19 (B-PAKZ) | 2 |
| `GameManifest::CurrentVersion` | 1 | M19 (B-BVER) | 2 |
| `FontImporter::Version` | 2 | M19 (A-STBTT) | 3 |
| CharacterController | v1 | M19 (P-CARRY) | v2 only if InheritGroundVelocity needs a migration |
| Script component | v1 | M20 | v2, with a `Formats/Scene` fixture |
| PrefabInstance component | v1 | M20 | v2 |
| PrefabLink | v1 | M20 | an additive key written with its default; no version change; the canonical change goes through M20's recorded upgrade |
| New formats | none | M18 | SpriteSheet v1 |
| `EngineVersionString` | 0.1.0 | B-BVER in M19; the last lander of each phase bumps the minor version | 0.2.0 at the end of phase 1 |
| `CurrentProtocolVersion` | {1,0} | none | unchanged |

**Allocations:**

| Allocation | Phase 1 |
|---|---|
| ADR numbers | M18 0029, M19 0030, M20 0031 |
| FeatureTest IdSeed ranges | 1501–1516 are taken; M18 1600–1699, M19 1700–1799, M20 1800–1899 |
| Golden scenes | `Golden.jsonl` changes only through M18's recorded regeneration with provenance; later golden scenes go in golden scaffolds of their own (M18, W18.2) |
| `Suites.jsonl` | one `Testing.Suites` patch line, unioned by hand at a merge |
| AssetType (append only) | M18 SpriteSheet; values assigned in landing order (they live only in cooked headers) |
| PlaySessionPhase | M19 NativeStep, after Tasks and before PreStepTransformUpdate; M20's pending enter, if P03 reproduces, at the position C20.1 records by amending this ledger |
| State-hash blocks | M19's native block after the physics block, empty without a native module |
| Render passes and stages | M18 fixes the output stage (the scene-resolution overlay stage for game Text and the reserved UI slot, then the integer upscale, then the view-resolution editor overlays) and appends the QuadBatcher's pass declarations; slots for phase 2 (Sprites and Lights2D before Tonemap, Ui in the reserved slot) are reserved, not built |
| Settings sections, validation codes, BuiltinComponents categories | appended in landing order: a later lander appends its entries after main's at its merge, never reordering them; both validation-code tables kept in step |
| Importer dependencies | every new importer of phase 1, M18's SpriteSheet included, fills `Import.Dependencies` with every asset it references (M19's pruning rule, C19.1) |
| Hardening `STEPS` | M18's debug-heap GPU step after `asan-feature`, before `fuzz`; M19 changes the soak step in place |
| Entity paths and scenes | One text decision shared by M19 and M20: an entity path is the `/`-separated names from a scene root and continues through a nested instance's members as authored; a path or `$ref` resolves in the batch's target scene, the active scene when the batch names none. M19 resolves against a scene parameter (the active scene until M20 lands); M20 passes the target |

**Single-owner files** (the owner resolves conflicts; the later lander adapts after merging main):

| Files | Owner |
|---|---|
| `Exporter.cpp`, the pak format, `games.py`, `Lint.py`, `provenance.py`, `CheckRuntimeMap.py`, `runtime_map.py`, Architecture §13.2, the font and text pipeline, `Vendor/stb`, `ScreenshotMethods`, `CharacterController` | M19 |
| The post chain, tonemap, the output stage, `ViewportCapture`, `ImGuiLayer`, `ImGuiRenderer`, the FileDialog API | M18 |
| `EditorApp`, `Autosave`, `EditorHostViewports` (autosave, recovery, per-view renderers, picking) | M18; M19 adds the headless game-view option for the soak (C19.1): if M19 lands after M18 it adapts after merging main, and if it lands first, M18 adapts at its merge; M20 adapts them to multiple open scenes after merging main |
| `EditorMethodContext`, `MethodSupport`, `BatchRunner`, `EntityMethods`, `PrefabMethods` (entity resolution, `$ref`, scene targets) | M19 (the resolver); M20 adds the scene target after merging main |
| The Jolt contact path (`PhysicsSystem.cpp`, the contact buffer, the listeners), `InspectorPanel`, `SceneSerializer`, the prefab update path | M20 |
| Demo changes that alter state hashes | M20 (one bridge re-record of Tetris Opening and LineClear and Breakout Opening after merging main). M19's native test game holds no Script, Prefab, PrefabLink, PostProcess or physics component (body, collider or CharacterController), so neither M20's re-record nor M26's PostProcess v2 re-record changes its hash |

## Won't do

Each entry is also in `Docs/Backlog.md`'s "Won't do" section, with this record as its source. Reopening one needs a new decision by the owner.

| Item | Reason |
|---|---|
| Render thread | The owner: "Cut the AAA items". No CPU-bound frame has been measured, and it would break §4.11's main-thread rules and the synchronous capture path automation uses |
| Bindless rendering | AAA cut. It pays off only at draw-call scale or with GPU-driven rendering; M18's material-set fix removes the binding-set churn |
| GPU-driven rendering | AAA cut. The games draw hundreds of objects, not millions |
| Render-graph aliasing | AAA cut. It needs a render graph, which the engine deliberately does not have; M18's stage split is enough for adding passes |
| Graphics backends other than NVRHI-Vulkan (D3D12) | AAA cut. With the platforms out of scope, another backend runs only where Vulkan already runs, and it doubles the GPU and golden matrix |
| Dynamic GI (lightmaps, SSGI, DDGI, RT GI) | AAA cut. Device-dependent noise and loose golden thresholds; M26's baked probes cover the visible need |
| Ray tracing (foundation and effects) | A declined extra. It would run on one machine only, and no game needs it |
| Registry-based Play copy | A declined extra. It breaks §1.3 principle 3 ("one scene-copy path (the serializer)"); M20's faster serializer copy stays the only path |
| NVRHI push descriptors and line rasterization | The vendored rule: NVRHI `main` equals the pin `6b96fb0`, and pull request #158 is unmerged. Reopen line rasterization (fixed widths only) if #158 or similar work merges; push descriptors have no upstream path |
| Goldens on GPU classes other than nvidia-61x | A kept limit: there is no other hardware, and the driver's major version is held (decision 9) |
| Consoles | The owner: won't do; there is no SDK access |
| Identity code signing and notarization | The owner: won't do; macOS bundles stay ad-hoc signed |
| Box2D (2D physics beside Jolt) | The owner: "I dont see why we need to bother with box2d at all". 2D physics and tilemap collision run on Jolt behind `Physics2DWorld` (decision 6) |

## Dropped by the owner

The owner chose "Drop the catch-up" (decision 10). Each item: dropped by the owner: there is no Linux or macOS machine without GitHub.

| Item |
|---|
| The GitHub Actions failures since M15 and the GCC 14, Clang 19 and Apple Clang build fixes |
| The Runtime's automation connection on POSIX (ADR 0025 finding M29) |
| A GCC LTO map and ELF symbol check (ADR 0025 finding M30) |
| The macOS bundle's loader lookup and ad-hoc signature check |
| The feature and games stages on the hosted runners |

The owner answered "Windows backends only" in the third round (decision 10). Each item: dropped by the owner: "Windows backends only", since nothing here can compile or test a Linux or macOS backend. Decision 10 states the rule and the behaviour on Linux and macOS.

| Item |
|---|
| Native file dialogs on Linux (E-DIALOGS-LINUX): nativefiledialog-extended's xdg-desktop-portal backend, the system libdbus-1 it links, `libdbus-1-dev` in `Setup.py`'s Linux package check, and the Linux-only text of its approval row |
| Native file dialogs on macOS (E-DIALOGS-MACOS, the macOS backend of E-DIALOGS) |
| ImGui multi-viewport OS windows on Linux (X11) and macOS (E-VIEWPORTS-POSIX, the non-Windows half of E-VIEWPORTS) |
| The per-game native module on Linux and macOS (C-NATIVE-POSIX): a dlopen backend for the editor's load and hot reload, and the make, GCC, Clang or Xcode native build that building and exporting a game with `Native/` would need there |

## Risks

1. **One full gate per milestone.** A defect an early wave introduces into a stage no targeted run covered surfaces only at the milestone end. A wave therefore runs the suites its change touches, the export, determinism and games suites included when it changes what they test.
2. **Parallel milestones.** Each later lander redoes registries, generated files, FeatureTest, fixtures, demos and goldens on the merged tree, and the gates of a phase run one at a time, the hardened one at night. The ledger keeps the redo mechanical, and the last lander of each phase owns the adaptation of its siblings' code.
3. **The renderer split (M18)** restructures the function every pass goes through; zero diffs on all 25 goldens prove it.
4. **The recreation protocol (M18)** must reach every later GPU owner on branches forked after it lands; a missed owner is a crash only an injected loss finds.
5. **2D physics on Jolt (M21).** Plane locks keep bodies in the plane, but the solver is 3D; a plane-drift test and the Physics2D replay in all four runs prove that shapes laid in the plane produce no out-of-plane motion.
6. **The native module (M19)** runs unsandboxed code in the editor and needs a toolchain at export; the user-only preference, the fixed-path build and its ADR bound it. It is built for Windows only (decision 10).
7. **Linux and macOS are never verified.** No Linux or macOS backend is written for the kept items (decision 10), but the portable first-party code, the `ErrorCode::Unsupported` halves and the new libraries' GCC, Clang and Apple Clang builds may fail at their first build there; nothing in the program checks them beyond the portability stage's generation.
8. **The accepted `Invariant` limit:** the prepass and forward pipelines agree only through the `NoContraction` products.
9. **A driver major update** would rename nvidia-61x; the owner holds the driver, and M18's check reports a rename.
10. **Machine limits:** one GPU, the gate's memory and a growing fuzz step; T-LOCKS enforces the tokens, T-FUZZMODEL sizes the fuzz from measurements, and the hardened gates run at night.
11. **Calendar:** Rolling Ball, the next proof of AI control, moves back by the whole program: three phase ends, each about 13–16 h of merges and redo, two plain gates and one night for its hardened gate (phase 3 also M25's soak), and three pauses.
12. **Hardening once per phase** (decision 12). A defect hardening would catch in an earlier lander (an ASan fault, a fuzz finding, a soak leak) shows only on its phase's merged tree, at the hardened gate, where the last lander fixes it; a new reader's fuzz case still runs in its wave's targeted unit run, and M25 runs its acceptance's soak right after its green plain gate, on the gated tree, before main fast-forwards.

## Before program completion

- [ ] The setup commit landed after one review pass and `CI.py --require-clang-cl` plus `Hardening.py --steps asan-generate,asan-build`.
- [ ] M18 to M26 each closed by its milestone end (main merged in, one review pass with every finding fixed, one `CI.py` on the final tree, `CI.py --milestone` at each phase's last lander and `CI.py --require-clang-cl` otherwise (decision 12), main fast-forwarded), in its phase's merge train; main pushed to `origin` as a backup after each phase, and the owner's go-ahead given before the next.
- [ ] Every built item meets its acceptance; R-GPUCRASH and R-ARENA are closed by their evidence; every Backlog row shows its outcome, and Backlog §2 to §7 hold only out-of-scope rows, accepted limits (the kept limits each milestone recorded included), dropped rows and pointers to the "Won't do" section.
- [ ] Device-loss recovery covers what decision 5 lists, and what stays fatal is in Backlog §6 with its reasons.
- [ ] Tetris, rebuilt in place as pixel art, passes the games suite's three halves and the provenance audit from a fresh clone.
- [ ] Windows is the only verified platform, and every milestone-end `CI.py` passed the portability stage (premake generation and the clang-cl builds); no Linux or macOS backend was written for a kept item, and each kept platform interface reports `ErrorCode::Unsupported` there (decision 10).
- [ ] Goldens are still nvidia-61x only with the driver's major version held, and every golden change was reviewed and named.
- [ ] Every approval used has its pin and licence committed (`VENDOR.md`, `FetchAssets.py`, the licence lists), and its `0001-approvals.md` row points to them.
- [ ] Architecture (§1.1, §1.2, §2.1, §2.3, §15.1, §15.8, §15.9, §16, §17, §18 and Appendix C), the Roadmap, `AGENTS.md`, the skills and the Backlog describe what landed.
- [ ] Rolling Ball (M27) starts only on the owner's go-ahead.

## Amendments

This record's first version amended, in its change (`eb8933f`): the Roadmap (the introduction, rules 1, 3, 4 and 6 and a new rule 7, the dependency graph and the milestone sections), `Docs/Backlog.md` (§1, every item's milestone or outcome, a new "Won't do" section), `Docs/Decisions/0001-approvals.md` (one row per approved item), `AGENTS.md` (milestone-workflow rules 1, 3 and 4, the commit gate, the "Commit gate" and "Full CI" command rows, the Backlog row of the repository map), Architecture (§1.2, §2.1, §2.3's `Test.py`, `CI.py` and `PreCommit.py` rows, §15.1, §15.8, §15.9, §17, §18 and Appendix C), `Docs/ReviewChecklist.md` and the `commit-review` and `build-and-test` skills.

This revision amends, in its change:
- **Roadmap:** the introduction, rules 1, 3 and 4, the dependency graph as phases, a program-setup section, M18 to M26 replacing the six-milestone sections, and Rolling Ball as M27 with its content unchanged.
- **`Docs/Backlog.md`:** the status line and §1 as phases, every item's new milestone or outcome, the §4 rows dropped by the owner, and Box2D in the "Won't do" section.
- **`Docs/Decisions/0001-approvals.md`:** the Box2D and GitHub CLI rows and the `ci/**` push exception removed, the push-as-backup policy, Windows as the only verified platform, and each row's new milestone.
- **`AGENTS.md`:** milestone-workflow rules 1, 3 and 4, the milestone-end and push paragraph, "Remote CI is non-blocking" replaced by "Windows is the only verified platform", and the Linux and macOS compile rule.
- **Architecture:** §1.1 G6, §1.2's milestone pointers, §2.1 (the `ci.yml` line and Rolling Ball as M27), §2.3's `Test.py` row, §15.8's GitHub Actions paragraph and header-self-containment sentence, §15.9, §16, §17, §18 and Appendix C.
- **`Docs/CodeStyle.md` §13:** the portability rule's last sentence: Windows is the only verified platform, so nothing but the rule and the portability stage's clang-cl builds checks the other toolchains.
- **`Docs/ReviewChecklist.md`** and the `commit-review` and `build-and-test` skills: the sentences that state the push, the pause, GitHub Actions, the `ci/**` exception and Linux and macOS verification.
- **`Vendor/JoltPhysics/VENDOR.md`:** the verification line that said GitHub Actions compiles the Linux and macOS projects.
- **Superseded in part:** ADR 0024 decision 19 (decision 12), ADR 0026 decision 1's deferral and ADR 0011's remote-CI role (decision 10), and the M16 and M17 process rules for milestones from M18 on (decision 11). Those ADRs stay as written.

The third round's answers amend, in the same change (the plan's revision 5 closes its open points):
- **This record:** the status, the context, decisions 1, 2, 4, 6 (the native module on Windows only), 7, 10 (Windows backends only), 12 (hardening at each phase's last lander), 15, 16 (the fuzz count unchanged) and 17 (the calendar), the "Dropped by the owner" section, risks 2, 6, 7, 10, 11 and 12, and the completion checklist.
- **Roadmap:** the introduction, rule 1, the gate line of each milestone section from M18 to M26, the program setup's fuzz deliverable, M18's, M19's and M24's dialogs, native module and platform windows on Windows only, M21's demo-hash acceptance and M25's soak after its gate.
- **`Docs/Backlog.md`:** the status line, §1's M18, M19 and M24 rows, §4's rows dropped by the owner and §7's features and editor lines.
- **`Docs/Decisions/0001-approvals.md`:** the nativefiledialog-extended row, for its Windows backend only, without the libdbus-1 text, with its portable premake script.
- **`AGENTS.md`:** milestone-workflow rule 1, the "Commit gate" and "Full CI" command rows, the commit gate's milestone-end rule, the milestone-end paragraph and "Windows is the only verified platform".
- **Architecture:** §1.2's dialogs, windows and native-module pointers, §2.3's `CI.py` row, §15.1's T6 row, §15.8's stage line and hardening row, §15.9, §16 and §18 with its M18, M19 and M24 rows.
- **`Docs/CodeStyle.md` §13:** the portability rule's last sentence (no Linux or macOS toolchain or standard library builds the code; the clang-cl builds against MSVC's library are the only non-MSVC compile).
- **`Docs/ReviewChecklist.md`** and the `commit-review` and `build-and-test` skills: the sentences that state the gate scope, and the cross-platform rules (the scoped Windows-backends-only item).
