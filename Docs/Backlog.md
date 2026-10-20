# Backlog

Status: index, not authoritative; current after M16 (2026-10-15), M17 (ADR 0027, 2026-10-19) and the backlog program's decisions (ADR 0028, 2026-10-19, revised the same day after the product owner's follow-up decisions and again after the owner's answers to the plan's open points), which schedule every in-scope item in a setup commit and M18 to M26, three phases of milestones developed in parallel, and record the rest as won't do, dropped by the owner, an accepted limit or out of scope; M20's first wave (W20.1, ADR 0031) closes the physics and session watch items, the faster Play copy, deleting prefab members and live nested prefabs. It gathers in one place the engine and editor work that v1 does not do, names the document that decides each item, and gives each item its milestone and wave (`W21.1` is milestone M21's wave 1, as in the plan ADR 0028 records; "the setup" is the setup commit before phase 1) or its outcome: § refers to `Docs/Architecture.md`, "ADR NNNN" to `Docs/Decisions/NNNN-*.md`. Where this file and its source disagree, the source wins and this file is fixed. A change that adds, closes or reschedules an item (an Appendix C row, an ADR's deferral, accepted limit, follow-up or watch item) updates this file in the same change (`Docs/ReviewChecklist.md` §9).

## 1. Planned milestones

Milestones run in phases: the milestones of a phase are developed in parallel, each on its own branch in its own worktree, and their milestone-end `CI.py` runs go one at a time on the one GPU. Phase end: once the phase's last milestone has landed, main is pushed to `origin` as an off-site backup, and the work pauses for the product owner's go-ahead (ADR 0028 decision 14).

| Phase | Milestone | Scope | Source |
|---|---|---|---|
| Setup | Program setup | The shared vendor-library cache, machine-wide build, GPU and gate locks, the fuzz budget from measured costs, the IdSeed guard, the gate scope in the scripts' texts, the prefab-member deletion refusal, `ci.yml` started only by hand | Roadmap "Program setup"; ADR 0028 decision 16 |
| 1 | M18 Renderer foundations, device-loss recovery and native dialogs | The renderer split into stages with frozen recreate and GPU-zone hooks, shader hot reload, GPU-lifetime instrumentation and the GPU-crash follow-up, profiler export and GPU merge, the per-frame arena measured, material sets kept across view exits, FeatureTest golden provenance, the golden device-class check and golden scaffolds, the QuadBatcher, the output stage and the SpriteSheet asset, recovery from device loss, driver-reset hangs and GPU out-of-memory as far as feasible, native file dialogs on Windows | Roadmap M18; ADR 0028 decisions 4, 5, 6 and 10 |
| 1 | M19 Automation, export, Luau and the native module | Automation, replay v3 and export basics, pak compression and pruning, the Luau compiler out of Dist, `stb_truetype.h`, a per-game native module with editor-only hot reload on Windows, the carried character | Roadmap M19; ADR 0028 decisions 4, 6 and 10 |
| 1 | M20 Scene model and physics semantics (lands last) | Physics and session semantics with the watch items, deleting prefab members and live nested prefabs, multiple open scenes, multiple scripts per entity | Roadmap M20; ADR 0028 decision 4 |
| 2 | M21 2D | Sprites, sorting, animation, the pixel-perfect camera, the 2D editor mode, 2D physics on Jolt, 2D lighting, tilemaps | Roadmap M21; ADR 0028 decisions 4 and 6 |
| 2 | M22 UI and text | The UI framework, static and interactive, text input, localization with RTL shaping, world-space UI | Roadmap M22; ADR 0028 decisions 4 and 6 |
| 2 | M23 3D rendering and assets (lands last) | Instancing, point-light shadows, clustered lights; mesh v2, EXR, Opus, BC1–BC7 compression; skeletal and morph animation | Roadmap M23; ADR 0028 decisions 4 and 9 |
| 3 | M24 Visual scripting and editor windows | Visual scripting, ImGui multi-viewport OS windows on Windows | Roadmap M24; ADR 0028 decisions 4, 6 and 10 |
| 3 | M25 Tetris rebuilt in place as pixel art (before M26) | `Projects/Tetris` rebuilt on the 2D path and the UI framework by builder sessions through the MCP tools only | Roadmap M25; ADR 0028 decision 8 |
| 3 | M26 Effects, post-processing and terrain (lands last) | Particles and baked probes; auto-exposure, LUTs, TAA, SSR; terrain | Roadmap M26; ADR 0028 decisions 4 and 9 |
| 4 | M27 Rolling Ball 3D | `Projects/RollingBall`, built by an agent through the MCP tools only, as Tetris was in M17; the engine and editor gaps it finds are fixed in their owning modules, never worked around in the game | Roadmap M27, §13.11 |

## 2. Deferred features

Decisions left open with a default in force until their milestone lands. Reopening one that has no milestone needs an ADR.

| Area | Item | Default in force | Milestone or outcome | Source |
|---|---|---|---|---|
| Rendering | Point-light shadows, TAA, auto-exposure, clustered lights, instancing | not built (also non-goals, §1.2) | point-light shadows, clustered lights and instancing in M23 (W23.1); auto-exposure in M26 (W26.1), TAA in M26 (W26.2) | Appendix C; ADR 0028 |
| Rendering | Further Vulkan 1.4 features: push descriptors, line rasterization | not used; they need NVRHI support | won't do (§8) | Appendix C, §8.1; ADR 0028 decision 2 |
| Editor | Shader hot reload | not built: build the `Shaders` project (or run `CompileShaders.py`) and restart the editor | M18 (W18.1) | Appendix C, §7.5, §8.12; ADR 0025 decision 9; ADR 0028 |
| Editor | Multiple open scenes | one open scene per editor | M20 (W20.2) | Appendix C, §12.1; ADR 0028 |
| Core | Profiler timeline export and GPU merge | CPU zones in per-thread rings and per-pass GPU times through `stats.get`; `Profiler::SubmitGpuZones`, `CollectZones` and `ExportChromeTrace` stay unit-tested API without a consumer | M18 (W18.1) | Appendix C, §4.13; ADR 0025 decision 1; ADR 0028 |
| Core | A per-frame arena for the render snapshot and debug draw lists | the default allocator | M18: measured in W18.1, adopted or closed with the numbers in W18.2 | Appendix C, §4.7; ADR 0025 decision 2; ADR 0028 |
| Physics | A character carried by moving ground | not built: a script adds the platform's velocity to the character's `Move` | M19 (W19.1) | Appendix C, §9.6; ADR 0025 decision 11; ADR 0028 |
| Assets | A second UV set and vertex colours in the mesh format | ignored with `ASSET_UNSUPPORTED_UV_SET` and `ASSET_VERTEX_COLORS_IGNORED` | M23 (W23.2) | Appendix C, §7.4; ADR 0028 |
| Assets | Vendoring `stb_truetype.h` separately | `imstb_truetype.h` from the vendored Dear ImGui, in `FontImporter` only | M19 (W19.2) | Appendix C, §7.4; ADR 0028 |
| Export | Pak compression and reachability pruning | uncompressed paks holding everything under `Assets/` minus the excludes | M19 (W19.2) | Appendix C, §7.6, §14.1; ADR 0028 |
| Export | Stripping Luau's compiler objects from Dist | linked but unreachable from game input; stripping them needs a vendored-source edit or a Release/Dist behaviour difference | M19 (W19.2), through a first-party require | Appendix C, §14.4; ADR 0025 decision 5; ADR 0028 decision 6 |
| Export | Build versioning | `EngineVersion` is the constant `0.1.0` in `Game.json`, `Engine.pak`, replays and session files | M19 (W19.1) | Appendix C, §14.1; ADR 0025 decision 6; ADR 0028 |
| Automation | Expectations in recordings made by `test.run {record}` (`test.run {record, expect}`) | `test.run {record}` writes none: the replay's state-hash checkpoints and the hashed HUD state prove the play, the recording suite's own `Test.Expect` asserts it at record time, and `input.record`'s `expect` stays available | M19 (W19.1) | ADR 0026 friction F10; ADR 0027 decision 12; ADR 0028 |
| Automation | The input check's optional warning when only the final hash does not depend on the input (it needs the run without input to continue past a failed checkpoint) | not done: the input check keeps its one rule | M19 (W19.1) | ADR 0026 friction F29; ADR 0027 decision 12; ADR 0028 |
| Automation | Entity paths in component data: an `EntityRef` value (a component field, a script's `Field.Entity()`) resolved from a path or unique prefix, as method params are | an `EntityRef` in component data keeps the handle pattern (ADR 0010 decision 32) and takes the 16-hex id; a path there is refused with a hint to use `{"$ref": "<op>.entity.id"}` in a batch or the id `entity.get` returns | M19 (W19.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Named `$ref` (an op named instead of its index) | positional op indices; the docs create the assets in a first batch and refer to them by path, name entities in entity params by path, and advise `dryRun: true` first | M19 (W19.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Reading a replay's input events back through automation | every demo replay comes from a recording suite (`test.run {record}`), whose input plan `script.read` shows; a replay made with `input.record` is re-recorded by planning its input again | M19 (W19.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | A builder-side Dist hash self-check (a testing export that runs `--feature-test` and reports its hashes) | the games suite's Dist half compares the testing Dist export's hashes with the editors' | M19 (W19.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | Screenshots kept outside `Library/Automation/Out/`, which the next editor session prunes | the lead copies the look screenshots to `bin/Reviews/M17-Look/` before the next editor launch on the project (ADR 0027 decision 9) | M19 (W19.1) | ADR 0027 decision 12; ADR 0028 |
| Automation | A replay header fingerprint of the starting state per entity and component, so a checkpoint failure names what changed | the divergence hint lists the possible causes (a serialized component, a script, an asset, the engine, nondeterminism) and the re-recording path | M19 (W19.1) | ADR 0027 friction F13; ADR 0028 |
| Testing | The editor soak renders each scene's game view (its camera and post-processing) | the Scene view renders the play scene (a headless editor displays no Game view); the game camera's own passes are covered by the golden images and the Runtime soak subject | M19 (W19.1) | ADR 0027 friction F08; ADR 0028 |
| Testing | Provenance of FeatureTest's 24 copied-in M9 golden files | no `Provenance.json` entries; a reviewed regeneration through one recorded editor run is needed before any audit targets FeatureTest | M18 (W18.2) | Appendix C, §15.4; ADR 0025 decision 7; ADR 0028 |
| Platform | Native Wayland | X11, including XWayland sessions | out of scope, stays here (the product owner: "We are not doing this right now") | Appendix C, §16; ADR 0028 decision 1 |

## 3. Tracked follow-ups

Follow-ups opened by earlier ADRs, each with its milestone or outcome.

| Item | State | Milestone or outcome | Source |
|---|---|---|---|
| An intermittent GPU-suite crash on a driver-created thread (a suspected NVIDIA driver fault; an engine lifetime bug is not ruled out) | open: it did not recur in the M15 milestone gate, and M16's passed its four gpu runs; it closes once reproduced on another driver version or GPU, or under the debug heap with the GPU objects' lifetimes logged; the driver is held during the program (ADR 0028 decision 9), which leaves the debug heap | M18: instrumentation in W18.1, 20 runs overnight under the GPU lock in W18.2; open with that evidence if it does not reproduce | ADR 0015 decision 27; ADR 0025 decision 18; ADR 0028 |
| A shared prebuilt vendor-library cache for new worktrees | closed by the setup: MSBuild builds link the vendored libraries from `<git common directory>/vendor-cache`, which every worktree shares (Architecture §2.2, `Scripts/Lib/vendor_cache.py`); its entries are never evicted, an accepted limit (§6) | the setup, before phase 1 | ADR 0023; ADR 0025 decision 18; ADR 0028 decision 16 |
| `Invariant` on the clip position, which Slang 2026.8 cannot decorate | the prepass and forward pipelines agree through `NoContraction` products | accepted limit (§6): the program keeps the toolchain | ADR 0013 decision 25; ADR 0025 decision 18; ADR 0028 decision 3 |

## 4. Linux and macOS catch-up

The product owner chose "Drop the catch-up" (ADR 0028 decision 10, superseding ADR 0026 decision 1's deferral of 2026-10-14 and the first version of ADR 0028, which scheduled it as M21). Windows is the only verified platform. First-party code stays portable by rule (the `Platform/` layering, no Windows-only shortcuts, the local portability stage's premake generation and clang-cl builds), but Linux and macOS are never verified: the Linux/macOS catch-up was dropped by the product owner, because there is no Linux or macOS machine without GitHub. GitHub is only an off-site backup of the repository: GitHub Actions is not part of the process, and nothing reads or waits for its results.

- The GitHub Actions failures of the M15 milestone commit, and whatever the runs of later pushes report (ADR 0026 decision 1): dropped by the owner (ADR 0028): there is no Linux or macOS machine without GitHub.
- A macOS run of an exported bundle: its signature and loader lookup (ADR 0026 decision 1): dropped by the owner (ADR 0028): there is no Linux or macOS machine without GitHub.
- **ADR 0025 finding M29:** port the Runtime's automation connection to POSIX, so that "RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1" runs on Linux and macOS (ADR 0025 decision 15): dropped by the owner (ADR 0028): there is no Linux or macOS machine without GitHub.
- **ADR 0025 finding M30:** verify the map check against a real GCC LTO Dist map, and read the executable's ELF symbols beside the map if GNU ld's map misses the symbols the linker plugin internalized (ADR 0025 decision 15; ADR 0024 decision 11): dropped by the owner (ADR 0028): there is no Linux or macOS machine without GitHub.
- GCC 14, Clang 19 with libstdc++ and Apple Clang build only in GitHub Actions, and their failures were to be fixed here (ADR 0013 decision 25's third limit): dropped by the owner (ADR 0028): there is no Linux or macOS machine without GitHub.

The designs for these (the bundle probe, the M29 port and the M30 ELF reader) stay in the gitignored `bin/Reviews/M16-Map.json` of the development machine (ADR 0026 decision 1).

The owner's answer to the plan's open points, "Windows backends only" (ADR 0028 decision 10), also drops the kept items' Linux and macOS backends. Windows backends only: a kept item's platform interface stays, so a later port can add backends, but no Linux or macOS backend is written, and on Linux and macOS the interface reports the capability as unavailable (`ErrorCode::Unsupported`) through the platform layer's existing error path. The rule covers the platform capabilities the program adds in first-party C++: existing POSIX backends (`Platform/Posix/`) keep compiling when their interface changes, and the Python tooling keeps its guarded POSIX branches. Vendored libraries' premake scripts stay portable (the portability stage generates gmake and xcode4 projects from them); their GCC, Clang and Apple Clang builds are never compiled.

- Native file dialogs on Linux: nativefiledialog-extended's xdg-desktop-portal backend, with `libdbus-1-dev` in `Setup.py`'s Linux package check: dropped by the owner (ADR 0028): "Windows backends only", since nothing here can compile or test a Linux or macOS backend. On Linux the FileDialog API falls back to the ImGui picker.
- Native file dialogs on macOS (the macOS backend of M18's dialogs): dropped by the owner (ADR 0028): "Windows backends only", since nothing here can compile or test a Linux or macOS backend. On macOS the FileDialog API falls back to the ImGui picker.
- ImGui multi-viewport OS windows on Linux (X11) and macOS (M24's platform windows outside Windows): dropped by the owner (ADR 0028): "Windows backends only", since nothing here can compile or test a Linux or macOS backend. There the editor keeps every panel docked in its main window.
- The per-game native module on Linux and macOS (M19's module outside Windows: a dlopen backend for the editor's load and hot reload, and the make, GCC, Clang or Xcode native build that building and exporting a game with `Native/` would need): dropped by the owner (ADR 0028): "Windows backends only", since nothing here can compile or test a Linux or macOS backend. There `native.*` build and load, and exporting a game that has `Native/`, return `ErrorCode::Unsupported`; a game without `Native/` is unaffected.

## 5. Watch items for the demo games

Defaults in force that the M16 dry run did not hit. ADR 0028 schedules each one; a demo that hits one before its milestone reopens it there.

| Item | Default in force | Milestone | Source |
|---|---|---|---|
| With a `SystemClock`, a frame that runs no step sees new device input only at the next tick | revisit with an ADR if a game needs it | M22 (W22.1), with UI input | ADR 0012 decision 4; ADR 0028 |

## 6. Accepted limits

Limits accepted for v1. A limit that names a milestone is scheduled by ADR 0028 and leaves this list when that milestone lands; the others have no work planned, and reopening one needs an ADR. Each milestone's ADR adds the kept limits it accepts.

- No render thread; two frames in flight (§4.11). Won't do (§8).
- Device loss, GPU out-of-memory and GPU hangs are fatal: autosave in the editor, a crash report and exit code 4, with no recovery (§8.14). M18 (W18.3, W18.4; ADR 0028 decision 5): device loss and driver-reset hangs are recovered, and GPU out-of-memory after startup is recovered by a trim and retry or fails only its request; a hang with no loss report, out-of-memory at startup and a failed recovery stay fatal, recorded here by M18's ADR.
- A material that leaves the view and returns gets a new binding set (ADR 0013 decision 25; ADR 0025 decision 18). M18 (W18.1).
- Golden images exist for the `nvidia-61x` device class only; every other device class runs them in smoke mode (§15.4; ADR 0013 decision 25). Kept: goldens for other classes won't do (§8). Goldens stay on the nvidia-61x device class only, and the NVIDIA driver's major version is held for the whole program; M18 (W18.2) reports a renamed class (ADR 0028 decision 9).
- `Invariant` on the clip position is not decorated, because Slang 2026.8 cannot; the prepass and forward pipelines agree through `NoContraction` products. The program stays on Vulkan SDK 1.4.350.0 and slangc 2026.8 (`Scripts/Lib/Toolchain.json` unchanged); it is revisited only if an in-scope item needs a newer toolchain, as an approval request at a phase boundary (ADR 0013 decision 25; ADR 0025 decision 18; ADR 0028 decision 3).
- `input.record`'s `stop` writes a recording without input events as recorded, with no verification, unlike `test.run {record}`; its expectations and any `script.eval` write during it are first checked by the `input.replay {verify: true, strictHash: true}` the docs require after every recording (§13.6; ADR 0027 decision 12). M19 (W19.1).
- `input.record`'s verification of a recording with input events counts the script errors raised up to its final tick, not those raised only when its session's scripts are torn down (`OnDestroy`), because a refused verification leaves its session live; `test.run`, which stops a replay's scripts before it reads their errors, counts them when it plays the replay (§13.6; ADR 0027 decision 12). M19 (W19.1).
- The machine-wide build, GPU and gate locks cover the scripts and the MCP bridge's launches, not processes started by hand: a Tests or Editor binary run directly takes no lock (the build-and-test skill routes GPU test cases through `Test.py --filter`), and an editor that the bridge leaves running with unsaved changes when its MCP client goes away renders without the GPU lock, whose hold ends with the bridge (Architecture §2.3; ADR 0028 decision 16). No work planned.
- The shared vendor-library cache never evicts an entry: every Visual Studio, MSVC or Windows SDK update, change of the per-user property sheet and vendored-library change adds a full set (about 1.5 GB for the Debug, Release, Dist, clang-cl and ASan entries of today's ten libraries), and the old sets stay. The space is reclaimed by hand, removing `<git common directory>/vendor-cache` (or old entries) at a phase end, which is safe only while no build runs in any worktree (`Scripts/Locks.py` lists no build holder; the next builds compile and publish what they need); doing it while builds run makes their links fail (Architecture §2.2; ADR 0028 decision 16). No work planned: an automatic prune would need each entry's last use recorded and the cache's lock against running links.
- A collider event reaches one piece where Jolt's manifold reduction merges a body pair's contacts: a body resting flat across two coplanar pieces gathered into one body, or a ball on the seam between two bricks, gives only the piece of the first sub-shape pair its own `OnCollisionEnter` and `OnCollisionExit`; pieces whose contacts face different ways each get theirs, and a piece with its own `Static` body gets one event per piece at a seam (§9.4; ADR 0031 decision 2). No work planned: a piece that needs its own event at a seam keeps its own `Static` body.
- Two staged game projects whose `.eproj` files share a file stem are not refused: their games steps and reports (`games <stem> ...`, `Games-<stem>-...`) collide, and two whose manifests share a `Name` share the Dist half's package directory (`bin/Exports/Dist/<Name>`); two `.eproj` files in one directory still fail the games suite (ADR 0027 decision 12; ADR 0026 R13). M19 (W19.1).

## 7. Non-goals for v1

Out of scope by design (§1.2), each open to an ADR. ADR 0028 schedules some, records others as won't do (§8), and leaves the rest out of scope:

- **Graphics:** backends other than NVRHI-Vulkan, ray tracing, bindless and GPU-driven rendering, render-graph aliasing (won't do, §8); clustered light culling and point-light shadows (M23, W23.1); TAA, histogram auto-exposure, colour-grading LUTs and SSR (M26: W26.1 and W26.2); GI beyond IBL plus AO (baked irradiance and reflection probes in M26, W26.1; dynamic GI won't do, §8).
- **Assets:** GPU texture compression, EXR import, Ogg Opus import (M23, W23.1 and W23.2) and mesh instancing (M23, W23.1).
- **Features:** skeletal and morph animation (M23, W23.3), particles and terrain (M26: W26.1, W26.2); visual scripting (M24: W24.1 and W24.2) and native gameplay modules (M19: W19.1 and W19.2, on Windows; on Linux and macOS dropped by the owner, §4); networking (out of scope, stays here: "Networking can stay in the backlog for now").
- **Editor and scenes:** native file dialogs (M18, W18.1, on Windows; their Linux and macOS backends dropped by the owner, §4), ImGui multi-viewport OS windows (M24, W24.1, on Windows; on Linux and macOS dropped by the owner, §4), multiple script components per entity (M20, W20.2), a UI framework beyond `TextComponent` (M22, W22.1 and W22.2).
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
| Dynamic GI (lightmaps, SSGI, DDGI, RT GI) | AAA cut. Device-dependent noise and loose golden thresholds; M26's baked probes cover the visible need | ADR 0028 decision 2 |
| Ray tracing (foundation and effects) | A declined extra. It would run on one machine only, and no game needs it | ADR 0028 decision 2 |
| Registry-based Play copy | A declined extra. It breaks §1.3 principle 3 ("one scene-copy path (the serializer)"); M20's faster serializer copy stays the only path | ADR 0028 decision 2 |
| NVRHI push descriptors and line rasterization | The vendored rule: NVRHI `main` equals the pin `6b96fb0`, and pull request #158 is unmerged. Reopen line rasterization (fixed widths only) if #158 or similar work merges; push descriptors have no upstream path | ADR 0028 decision 2 |
| Goldens on GPU classes other than nvidia-61x | A kept limit (§6): there is no other hardware, and the NVIDIA driver's major version is held | ADR 0028 decisions 2 and 9 |
| Consoles | The product owner: won't do; there is no SDK access | ADR 0028 decisions 1 and 2 |
| Identity code signing and notarization | The product owner: won't do; macOS bundles stay ad-hoc signed | ADR 0028 decisions 1 and 2 |
| Box2D (2D physics beside Jolt) | The product owner: "I dont see why we need to bother with box2d at all". 2D physics and tilemap collision run on Jolt: bodies with plane (degree-of-freedom) locks behind the `Physics2DWorld` interface, deterministic like the 3D world, with no new library (M21) | ADR 0028 decisions 1, 2 and 6 |
