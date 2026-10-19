# Backlog

Status: index, not authoritative; current after M16 (2026-10-15), M17 (ADR 0027, 2026-10-19) and the backlog program's decisions (ADR 0028, 2026-10-19), which schedule every in-scope item in M18 to M23 and record the rest as won't do, an accepted limit or out of scope. It gathers in one place the engine and editor work that v1 does not do, names the document that decides each item, and gives each item its milestone and wave (`W20.3` is milestone M20's wave 3, as in the plan ADR 0028 records) or its outcome: § refers to `Docs/Architecture.md`, "ADR NNNN" to `Docs/Decisions/NNNN-*.md`. Where this file and its source disagree, the source wins and this file is fixed. A change that adds, closes or reschedules an item (an Appendix C row, an ADR's deferral, accepted limit, follow-up or watch item) updates this file in the same change (`Docs/ReviewChecklist.md` §9).

## 1. Planned milestones

Milestones run one at a time in number order, each followed by a pause for the product owner's go-ahead (ADR 0028 decision 14).

| Milestone | Scope | Source |
|---|---|---|
| M18 Foundations | The renderer split into stages with frozen recreate and GPU-zone hooks, shader hot reload, the vendor-library cache, GPU-lifetime instrumentation and the GPU-crash follow-up, profiler export and GPU merge, the per-frame arena measured, FeatureTest golden provenance, the golden device-class check and rename procedure, the prefab-member deletion refusal, material sets kept across view exits, the gate scope in `CI.py` | Roadmap M18; ADR 0028 decisions 4 and 12 |
| M19 Device-loss recovery | Recovery from device loss and driver-reset hangs, and from GPU out-of-memory as far as feasible; what stays fatal is stated with its reasons | Roadmap M19; ADR 0028 decision 5 |
| M20 2D, UI and Tetris | Automation, replay v3 and export basics; the 2D path; 2D physics on Box2D and 2D lighting; the UI framework, static and interactive; tilemaps and text input; localization with RTL shaping and world-space UI; Tetris rebuilt in place as pixel art | Roadmap M20; ADR 0028 decisions 4, 6 and 8 |
| M21 Linux/macOS catch-up | The GitHub Actions failures since M15 and the items of §4, non-blocking; then the Linux and macOS jobs kept green | Roadmap M21; ADR 0028 decision 10 |
| M22 Scene, editor and scripting | Physics and session semantics with the watch items, prefabs II and editor windows with native file dialogs, multiple open scenes, multiple scripts per entity, a per-game native module with editor-only hot reload, visual scripting | Roadmap M22; ADR 0028 decisions 4 and 6 |
| M23 Rendering and content | Instancing, point-light shadows, clustered lights; mesh v2, EXR, Opus, BC1–BC7 compression, `stb_truetype.h`; pak compression and pruning, the Luau compiler out of Dist; animation; particles and baked probes; auto-exposure, LUTs, TAA, SSR; terrain | Roadmap M23; ADR 0028 decisions 4 and 6 |
| M24 Rolling Ball 3D | `Projects/RollingBall`, built by an agent through the MCP tools only, as Tetris was in M17; the engine and editor gaps it finds are fixed in their owning modules, never worked around in the game | Roadmap M24, §13.11 |

## 2. Deferred features

Decisions left open with a default in force until their milestone lands. Reopening one that has no milestone needs an ADR.

| Area | Item | Default in force | Milestone or outcome | Source |
|---|---|---|---|---|
| Rendering | Point-light shadows, TAA, auto-exposure, clustered lights, instancing | not built (also non-goals, §1.2) | M23: point-light shadows, clustered lights and instancing in W23.1, TAA and auto-exposure in W23.6 | Appendix C; ADR 0028 |
| Rendering | Further Vulkan 1.4 features: push descriptors, line rasterization | not used; they need NVRHI support | won't do (§8) | Appendix C, §8.1; ADR 0028 decision 2 |
| Editor | Shader hot reload | not built: build the `Shaders` project (or run `CompileShaders.py`) and restart the editor | M18 (W18.1) | Appendix C, §7.5, §8.12; ADR 0025 decision 9; ADR 0028 |
| Editor | Multiple open scenes | one open scene per editor | M22 (W22.3) | Appendix C, §12.1; ADR 0028 |
| Scene | A faster registry-based Play copy | the serializer copy, its 30 ms budget tracked warning-only by the perf capture | a faster serializer copy in M22 (W22.1); the registry-based copy won't do (§8) | Appendix C, §5.6; ADR 0024 decision 16; ADR 0028 |
| Core | Profiler timeline export and GPU merge | CPU zones in per-thread rings and per-pass GPU times through `stats.get`; `Profiler::SubmitGpuZones`, `CollectZones` and `ExportChromeTrace` stay unit-tested API without a consumer | M18 (W18.1) | Appendix C, §4.13; ADR 0025 decision 1; ADR 0028 |
| Core | A per-frame arena for the render snapshot and debug draw lists | the default allocator | M18: measured in W18.1, adopted or closed with the numbers in W18.2 | Appendix C, §4.7; ADR 0025 decision 2; ADR 0028 |
| Physics | A character carried by moving ground | not built: a script adds the platform's velocity to the character's `Move` | M22 (W22.1) | Appendix C, §9.6; ADR 0025 decision 11; ADR 0028 |
| Assets | A second UV set and vertex colours in the mesh format | ignored with `ASSET_UNSUPPORTED_UV_SET` and `ASSET_VERTEX_COLORS_IGNORED` | M23 (W23.2) | Appendix C, §7.4; ADR 0028 |
| Assets | Vendoring `stb_truetype.h` separately | `imstb_truetype.h` from the vendored Dear ImGui, in `FontImporter` only | M23 (W23.2) | Appendix C, §7.4; ADR 0028 |
| Export | Pak compression and reachability pruning | uncompressed paks holding everything under `Assets/` minus the excludes | M23 (W23.3) | Appendix C, §7.6, §14.1; ADR 0028 |
| Export | Stripping Luau's compiler objects from Dist | linked but unreachable from game input; stripping them needs a vendored-source edit or a Release/Dist behaviour difference | M23 (W23.3), through a first-party require | Appendix C, §14.4; ADR 0025 decision 5; ADR 0028 decision 6 |
| Export | Build versioning | `EngineVersion` is the constant `0.1.0` in `Game.json`, `Engine.pak`, replays and session files | M20 (W20.1) | Appendix C, §14.1; ADR 0025 decision 6; ADR 0028 |
| Automation | Expectations in recordings made by `test.run {record}` (`test.run {record, expect}`) | `test.run {record}` writes none: the replay's state-hash checkpoints and the hashed HUD state prove the play, the recording suite's own `Test.Expect` asserts it at record time, and `input.record`'s `expect` stays available | M20 (W20.1) | ADR 0026 friction F10; ADR 0027 decision 12; ADR 0028 |
| Automation | The input check's optional warning when only the final hash does not depend on the input (it needs the run without input to continue past a failed checkpoint) | not done: the input check keeps its one rule | M20 (W20.1) | ADR 0026 friction F29; ADR 0027 decision 12; ADR 0028 |
| Automation | Entity paths in component data: an `EntityRef` value (a component field, a script's `Field.Entity()`) resolved from a path or unique prefix, as method params are | an `EntityRef` in component data keeps the handle pattern (ADR 0010 decision 32) and takes the 16-hex id; a path there is refused with a hint to use `{"$ref": "<op>.entity.id"}` in a batch or the id `entity.get` returns | M20 (W20.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Named `$ref` (an op named instead of its index) | positional op indices; the docs create the assets in a first batch and refer to them by path, name entities in entity params by path, and advise `dryRun: true` first | M20 (W20.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Reading a replay's input events back through automation | every demo replay comes from a recording suite (`test.run {record}`), whose input plan `script.read` shows; a replay made with `input.record` is re-recorded by planning its input again | M20 (W20.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | A builder-side Dist hash self-check (a testing export that runs `--feature-test` and reports its hashes) | the games suite's Dist half compares the testing Dist export's hashes with the editors' | M20 (W20.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Screenshots kept outside `Library/Automation/Out/`, which the next editor session prunes | the lead copies the look screenshots to `bin/Reviews/M17-Look/` before the next editor launch on the project (ADR 0027 decision 9) | M20 (W20.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | A replay header fingerprint of the starting state per entity and component, so a checkpoint failure names what changed | the divergence hint lists the possible causes (a serialized component, a script, an asset, the engine, nondeterminism) and the re-recording path | M20 (W20.1) | ADR 0027 friction F13; ADR 0028 |
| Testing | The editor soak renders each scene's game view (its camera and post-processing) | the Scene view renders the play scene (a headless editor displays no Game view); the game camera's own passes are covered by the golden images and the Runtime soak subject | M20 (W20.1) | ADR 0027 friction F08; ADR 0028 |
| Testing | Provenance of FeatureTest's 24 copied-in M9 golden files | no `Provenance.json` entries; a reviewed regeneration through one recorded editor run is needed before any audit targets FeatureTest | M18 (W18.2) | Appendix C, §15.4; ADR 0025 decision 7; ADR 0028 |
| Platform | Native Wayland | X11, including XWayland sessions | out of scope, stays here (the product owner: "We are not doing this right now") | Appendix C, §16; ADR 0028 decision 1 |

## 3. Tracked follow-ups

Follow-ups opened by earlier ADRs, each with its milestone or outcome.

| Item | State | Milestone or outcome | Source |
|---|---|---|---|
| An intermittent GPU-suite crash on a driver-created thread (a suspected NVIDIA driver fault; an engine lifetime bug is not ruled out) | open: it did not recur in the M15 milestone gate, and M16's passed its four gpu runs; it closes once reproduced on another driver version or GPU, or under the debug heap with the GPU objects' lifetimes logged; the driver is held during the program (ADR 0028 decision 9), which leaves the debug heap | M18: instrumentation in W18.1, 20 runs in W18.2; open with that evidence if it does not reproduce | ADR 0015 decision 27; ADR 0025 decision 18; ADR 0028 |
| A shared prebuilt vendor-library cache for new worktrees | not decided: each fresh worktree rebuilds every vendored library | M18 (W18.1) | ADR 0023; ADR 0025 decision 18; ADR 0028 |
| `Invariant` on the clip position, which Slang 2026.8 cannot decorate | the prepass and forward pipelines agree through `NoContraction` products | accepted limit (§6): the program keeps the toolchain | ADR 0013 decision 25; ADR 0025 decision 18; ADR 0028 decision 3 |

## 4. Linux and macOS catch-up

Scheduled as M21, right after M20's Tetris rebuild, by the product owner's choice (ADR 0028 decision 10, superseding ADR 0026 decision 1's deferral of 2026-10-14). It is non-blocking: failures that remain when its session budget is spent become open rows here, and M22 starts anyway (ADR 0011). From M21 on, the Linux and macOS jobs are kept green: each later milestone's first wave fixes what the previous push's Actions runs reported, read through gh under the product owner's read-only login, and no milestone waits for a run. Linux and macOS stay unverified until their GitHub Actions jobs are green, and first-party code stays portable meanwhile.

- The GitHub Actions failures of the M15 milestone commit, and whatever the runs of later pushes report (ADR 0026 decision 1). M21 (W21.1).
- A macOS run of an exported bundle: its signature and loader lookup (ADR 0026 decision 1). M21 (W21.1).
- **ADR 0025 finding M29:** port the Runtime's automation connection to POSIX, so that "RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1" runs on Linux and macOS; only the synthesized keystroke stays Windows-only (ADR 0025 decision 15). M21 (W21.1).
- **ADR 0025 finding M30:** verify the map check against a real GCC LTO Dist map, and read the executable's ELF symbols beside the map if GNU ld's map misses the symbols the linker plugin internalized (ADR 0025 decision 15; ADR 0024 decision 11). M21 (W21.1).
- GCC 14, Clang 19 with libstdc++ and Apple Clang build only in GitHub Actions, and their failures are fixed here (ADR 0013 decision 25's third limit). M21 (W21.1), then kept green.

The designs for these (the bundle probe, the M29 port and the M30 ELF reader) are in the gitignored `bin/Reviews/M16-Map.json` of the development machine (ADR 0026 decision 1).

## 5. Watch items for the demo games

Defaults in force that the M16 dry run did not hit. ADR 0028 schedules each one; a demo that hits one before its milestone reopens it there.

| Item | Default in force | Milestone | Source |
|---|---|---|---|
| Neighbouring static bodies merged into one compound body by the `PHYSICS_ADJACENT_STATIC_BODIES` fix, so per-piece collision callbacks reach only the root | the fix stays as it is; the game-building and luau-gameplay skills give each piece its own `Static` body with a gap | M22 (W22.1): callbacks routed to scripted child colliders | ADR 0026 P01; ADR 0028 |
| A partner's collision callback meeting an entity destroyed earlier in the same dispatch | the luau-gameplay skill deactivates pieces that must outlive the contact | M22 (W22.1): an entity pending destruction stays readable until the flush | ADR 0026 P02; ADR 0028 |
| Speculative contacts reaching scripts, so a contact is reported without a bounce | revisit if a demo shows it | M22 (W22.1): reproduced first, fixed if it reproduces | ADR 0026 P03; ADR 0028 |
| `Scene.Reload` and restart in editor play (the saved scene file, two transitions in one frame) | restart in place, or `Scene.Load` of the saved scene | M22 (W22.1) | ADR 0026 P06; ADR 0028 |
| With a `SystemClock`, a frame that runs no step sees new device input only at the next tick | revisit with an ADR if a game needs it | M20 (W20.5), with UI input | ADR 0012 decision 4; ADR 0028 |

## 6. Accepted limits

Limits accepted for v1. A limit that names a milestone is scheduled by ADR 0028 and leaves this list when that milestone lands; the others have no work planned, and reopening one needs an ADR. Each milestone's ADR adds the kept limits it accepts.

- No render thread; two frames in flight (§4.11). Won't do (§8).
- Device loss, GPU out-of-memory and GPU hangs are fatal: autosave in the editor, a crash report and exit code 4, with no recovery (§8.14). M19 (ADR 0028 decision 5): device loss and driver-reset hangs are recovered, and GPU out-of-memory after startup is recovered by a trim and retry or fails only its request; a hang with no loss report, out-of-memory at startup and a failed recovery stay fatal, recorded here by M19's ADR.
- A material that leaves the view and returns gets a new binding set (ADR 0013 decision 25; ADR 0025 decision 18). M18 (W18.1).
- Golden images exist for the `nvidia-61x` device class only; every other device class runs them in smoke mode (§15.4; ADR 0013 decision 25). Kept: goldens for other classes won't do (§8). Goldens stay on the nvidia-61x device class only, and the NVIDIA driver's major version is held for the whole program; M18 (W18.2) reports a renamed class (ADR 0028 decision 9).
- `Invariant` on the clip position is not decorated, because Slang 2026.8 cannot; the prepass and forward pipelines agree through `NoContraction` products. The program stays on Vulkan SDK 1.4.350.0 and slangc 2026.8 (`Scripts/Lib/Toolchain.json` unchanged); it is revisited only if an in-scope item needs a newer toolchain, as an approval request at a milestone boundary (ADR 0013 decision 25; ADR 0025 decision 18; ADR 0028 decision 3).
- `input.record`'s `stop` writes a recording without input events as recorded, with no verification, unlike `test.run {record}`; its expectations and any `script.eval` write during it are first checked by the `input.replay {verify: true, strictHash: true}` the docs require after every recording (§13.6; ADR 0027 decision 12). M20 (W20.1).
- `input.record`'s verification of a recording with input events counts the script errors raised up to its final tick, not those raised only when its session's scripts are torn down (`OnDestroy`), because a refused verification leaves its session live; `test.run`, which stops a replay's scripts before it reads their errors, counts them when it plays the replay (§13.6; ADR 0027 decision 12). M20 (W20.1).
- Two staged game projects whose `.eproj` files share a file stem are not refused: their games steps and reports (`games <stem> ...`, `Games-<stem>-...`) collide, and two whose manifests share a `Name` share the Dist half's package directory (`bin/Exports/Dist/<Name>`); two `.eproj` files in one directory still fail the games suite (ADR 0027 decision 12; ADR 0026 R13). M20 (W20.1).

## 7. Non-goals for v1

Out of scope by design (§1.2), each open to an ADR. ADR 0028 schedules some, records others as won't do (§8), and leaves the rest out of scope:

- **Graphics:** backends other than NVRHI-Vulkan, ray tracing, bindless and GPU-driven rendering, render-graph aliasing (won't do, §8); TAA, histogram auto-exposure, clustered light culling, point-light shadows, colour-grading LUTs and SSR (M23: W23.1 and W23.6); GI beyond IBL plus AO (baked irradiance and reflection probes in M23, W23.5; dynamic GI won't do, §8).
- **Assets:** GPU texture compression, EXR import, Ogg Opus import (M23, W23.2) and mesh instancing (M23, W23.1).
- **Features:** skeletal and morph animation, particles and terrain (M23: W23.4, W23.5, W23.7); visual scripting and native gameplay modules (M22: W22.6 and W22.7, W22.5); networking (out of scope, stays here: "Networking can stay in the backlog for now").
- **Editor and scenes:** live nested prefabs, deleting prefab-member entities inside an instance, ImGui multi-viewport OS windows and native file dialogs (M22, W22.2; deleting a member is refused with a hint from M18), multiple script components per entity (M22, W22.4), a UI framework beyond `TextComponent` (M20, W20.4 and W20.5).
- **Platforms:** mobile and web, native Wayland, cross-OS export, bit-exact cross-OS or cross-compiler determinism (out of scope, stay here: "We are not doing this right now"); consoles, identity code signing and notarization (won't do, §8).

## 8. Won't do

Items the product owner decided not to build (ADR 0028, 2026-10-19). Reopening one needs a new decision by the product owner.

| Item | Reason | Source |
|---|---|---|
| Render thread | The product owner: "Cut the AAA items". No CPU-bound frame has been measured, and it would break §4.11's main-thread rules and the synchronous capture path automation uses | ADR 0028 decision 2 |
| Bindless rendering | AAA cut. It pays off only at draw-call scale or with GPU-driven rendering; M18's material-set fix removes the binding-set churn | ADR 0028 decision 2 |
| GPU-driven rendering | AAA cut. The games draw hundreds of objects, not millions | ADR 0028 decision 2 |
| Render-graph aliasing | AAA cut. It needs a render graph, which the engine deliberately does not have; M18's stage split is enough for adding passes | ADR 0028 decision 2 |
| Graphics backends other than NVRHI-Vulkan (D3D12) | AAA cut. With the platforms out of scope, another backend runs only where Vulkan already runs, and it doubles the GPU and golden matrix | ADR 0028 decision 2 |
| Dynamic GI (lightmaps, SSGI, DDGI, RT GI) | AAA cut. Device-dependent noise and loose golden thresholds; M23's baked probes cover the visible need | ADR 0028 decision 2 |
| Ray tracing (foundation and effects) | A declined extra. It would run on one machine only, and no game needs it | ADR 0028 decision 2 |
| Registry-based Play copy | A declined extra. It breaks §1.3 principle 3 ("one scene-copy path (the serializer)"); M22's faster serializer copy stays the only path | ADR 0028 decision 2 |
| NVRHI push descriptors and line rasterization | The vendored rule: NVRHI `main` equals the pin `6b96fb0`, and pull request #158 is unmerged. Reopen line rasterization (fixed widths only) if #158 or similar work merges; push descriptors have no upstream path | ADR 0028 decision 2 |
| Goldens on GPU classes other than nvidia-61x | A kept limit (§6): there is no other hardware, and the NVIDIA driver's major version is held | ADR 0028 decisions 2 and 9 |
| Consoles | The product owner: won't do; there is no SDK access | ADR 0028 decisions 1 and 2 |
| Identity code signing and notarization | The product owner: won't do; macOS bundles stay ad-hoc signed | ADR 0028 decisions 1 and 2 |
