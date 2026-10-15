# Backlog

Status: index, not authoritative; current after M16 (2026-10-15). It gathers in one place the engine and editor work that v1 does not do, and names the document that decides each item: § refers to `Docs/Architecture.md`, "ADR NNNN" to `Docs/Decisions/NNNN-*.md`. Where this file and its source disagree, the source wins and this file is fixed. A change that adds, closes or reschedules an item (an Appendix C row, an ADR's deferral, accepted limit, follow-up or watch item) updates this file in the same change (`Docs/ReviewChecklist.md` §9).

## 1. Planned milestones

| Milestone | Scope | Source |
|---|---|---|
| M17 Tetris | `Projects/Tetris`, built by an agent through the MCP tools only; the engine and editor gaps it finds are fixed in their owning modules, never worked around in the game | Roadmap M17, §13.11 |
| M18 Rolling Ball 3D | `Projects/RollingBall`, built the same way; it may run alongside M17 | Roadmap M18, §13.11 |

## 2. Deferred features

Decisions left open with a default in force. Reopening one needs an ADR.

| Area | Item | Default in force | Source |
|---|---|---|---|
| Rendering | Point-light shadows, TAA, auto-exposure, clustered lights, instancing | not built (also non-goals, §1.2) | Appendix C |
| Rendering | Further Vulkan 1.4 features: push descriptors, line rasterization | not used; they need NVRHI support | Appendix C, §8.1 |
| Editor | Shader hot reload | not built: build the `Shaders` project (or run `CompileShaders.py`) and restart the editor | Appendix C, §7.5, §8.12; ADR 0025 decision 9 |
| Editor | Multiple open scenes | one open scene per editor | Appendix C, §12.1 |
| Scene | A faster registry-based Play copy | the serializer copy, its 30 ms budget tracked warning-only by the perf capture | Appendix C, §5.6; ADR 0024 decision 16 |
| Core | Profiler timeline export and GPU merge | CPU zones in per-thread rings and per-pass GPU times through `stats.get`; `Profiler::SubmitGpuZones`, `CollectZones` and `ExportChromeTrace` stay unit-tested API without a consumer | Appendix C, §4.13; ADR 0025 decision 1 |
| Core | A per-frame arena for the render snapshot and debug draw lists | the default allocator | Appendix C, §4.7; ADR 0025 decision 2 |
| Physics | A character carried by moving ground | not built: a script adds the platform's velocity to the character's `Move` | Appendix C, §9.6; ADR 0025 decision 11 |
| Assets | A second UV set and vertex colours in the mesh format | ignored with `ASSET_UNSUPPORTED_UV_SET` and `ASSET_VERTEX_COLORS_IGNORED` | Appendix C, §7.4 |
| Assets | Vendoring `stb_truetype.h` separately | `imstb_truetype.h` from the vendored Dear ImGui, in `FontImporter` only | Appendix C, §7.4 |
| Export | Pak compression and reachability pruning | uncompressed paks holding everything under `Assets/` minus the excludes | Appendix C, §7.6, §14.1 |
| Export | Stripping Luau's compiler objects from Dist | linked but unreachable from game input; stripping them needs a vendored-source edit or a Release/Dist behaviour difference | Appendix C, §14.4; ADR 0025 decision 5 |
| Export | Build versioning | `EngineVersion` is the constant `0.1.0` in `Game.json`, `Engine.pak`, replays and session files | Appendix C, §14.1; ADR 0025 decision 6 |
| Automation | Expectations in autopilot recordings (`test.run {record, expect}`) | `test.run {record}` writes none: an autopilot replay proves its play through its state-hash checkpoints, and expectations stay with `input.record`'s `expect`; M17 at the earliest | ADR 0026 friction F10 |
| Testing | Provenance of FeatureTest's 24 copied-in M9 golden files | no `Provenance.json` entries; a reviewed regeneration through one recorded editor run is needed before any audit targets FeatureTest | Appendix C, §15.4; ADR 0025 decision 7 |
| Platform | Native Wayland | X11, including XWayland sessions | Appendix C, §16 |

## 3. Tracked follow-ups

Open items that no milestone schedules.

| Item | State | Source |
|---|---|---|
| An intermittent GPU-suite crash on a driver-created thread (a suspected NVIDIA driver fault; an engine lifetime bug is not ruled out) | open: it did not recur in the M15 milestone gate, and M16's passed its four gpu runs; it closes once reproduced on another driver version or GPU, or under the debug heap with the GPU objects' lifetimes logged | ADR 0015 decision 27; ADR 0025 decision 18 |
| A shared prebuilt vendor-library cache for new worktrees | not decided: each fresh worktree rebuilds every vendored library | ADR 0023; ADR 0025 decision 18 |
| `Invariant` on the clip position, which Slang 2026.8 cannot decorate | the prepass and forward pipelines agree through `NoContraction` products; revisited with the next slangc pin | ADR 0013 decision 25; ADR 0025 decision 18 |

## 4. Linux and macOS catch-up

Deferred until the product owner asks for it (ADR 0026 decision 1, 2026-10-14). Remote CI stays informational, Linux and macOS stay unverified until their GitHub Actions jobs are green, and first-party code stays portable meanwhile.

- The GitHub Actions failures of the M15 milestone commit, and whatever the runs of later pushes report (ADR 0026 decision 1).
- A macOS run of an exported bundle: its signature and loader lookup (ADR 0026 decision 1).
- **M29:** port the Runtime's automation connection to POSIX, so that "RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1" runs on Linux and macOS; only the synthesized keystroke stays Windows-only (ADR 0025 decision 15).
- **M30:** verify the map check against a real GCC LTO Dist map, and read the executable's ELF symbols beside the map if GNU ld's map misses the symbols the linker plugin internalized (ADR 0025 decision 15; ADR 0024 decision 11).
- GCC 14, Clang 19 with libstdc++ and Apple Clang build only in GitHub Actions, and their failures are fixed here (ADR 0013 decision 25's third limit).

The designs for these (the bundle probe, the M29 port and the M30 ELF reader) are in the gitignored `bin/Reviews/M16-Map.json` of the development machine (ADR 0026 decision 1).

## 5. Watch items for the demo games

Defaults in force that the M16 dry run did not hit. M17 or M18 reopens one if it does.

| Item | Default in force | Source |
|---|---|---|
| Neighbouring static bodies merged into one compound body by the `PHYSICS_ADJACENT_STATIC_BODIES` fix, so per-piece collision callbacks reach only the root | the fix stays as it is; the game-building and luau-gameplay skills give each piece its own `Static` body with a gap; revisit if M17 or M18 hits it | ADR 0026 P01 |
| A partner's collision callback meeting an entity destroyed earlier in the same dispatch | the luau-gameplay skill deactivates pieces that must outlive the contact | ADR 0026 P02 |
| Speculative contacts reaching scripts, so a contact is reported without a bounce | revisit if M17 or M18 shows it | ADR 0026 P03 |
| `Scene.Reload` and restart in editor play (the saved scene file, two transitions in one frame) | restart in place, or `Scene.Load` of the saved scene | ADR 0026 P06 |
| With a `SystemClock`, a frame that runs no step sees new device input only at the next tick | revisit with an ADR if a game needs it | ADR 0012 decision 4 |

## 6. Accepted limits

Limits accepted for v1, with no work planned. Reopening one needs an ADR.

- No render thread; two frames in flight (§4.11).
- Device loss, GPU out-of-memory and GPU hangs are fatal: autosave in the editor, a crash report and exit code 4, with no recovery (§8.14).
- A material that leaves the view and returns gets a new binding set (ADR 0013 decision 25; ADR 0025 decision 18).
- Golden images exist for the `nvidia-61x` device class only; every other device class runs them in smoke mode (§15.4; ADR 0013 decision 25).

## 7. Non-goals for v1

Out of scope by design (§1.2), each open to an ADR:

- **Graphics:** backends other than NVRHI-Vulkan, ray tracing, bindless and GPU-driven rendering, render-graph aliasing; TAA, histogram auto-exposure, clustered light culling, point-light shadows, colour-grading LUTs, SSR and GI beyond IBL plus AO.
- **Assets:** GPU texture compression, EXR import, Ogg Opus import and mesh instancing.
- **Features:** skeletal and morph animation, particles, terrain, networking, visual scripting and native gameplay modules.
- **Editor and scenes:** live nested prefabs, deleting prefab-member entities inside an instance, multiple script components per entity, a UI framework beyond `TextComponent`, ImGui multi-viewport OS windows and native file dialogs.
- **Platforms:** mobile, consoles and web; native Wayland; cross-OS export; bit-exact cross-OS or cross-compiler determinism; identity code signing and notarization.
