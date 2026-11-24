# 0060 — The Skerries engine program (M43 to M65)

- **Status:** accepted. The product owner answered every point of the lead's draft on 2026-11-24 ("The owner's answers"): the three libraries approved, the hardening once at the program's end, phase 12 confirmed, the briefs re-read first, then "go with your recommendations". It is committed on main as a documentation-only change in the program's setup (decision 3), with phase 8's milestone ADRs (0061 to 0066), the Roadmap's M43 to M48 sections, Architecture §18's rows, `Docs/Backlog.md`'s rows, the `0001-approvals.md` rows and AGENTS.md's gate line.
- **Date:** 2026-11-23 (the draft) and 2026-11-24 (accepted).
- **Scope:** it schedules the twenty engine tasks that Skerries, the second showcase game, asked for (`Projects/Skerries/Design/EngineTasks/`, S01 to S20, read at engine 0.8.0, `3aaf4b10`; re-read on 2026-11-24).
- **Owner:** the lead.
- **Inputs:** the six research notes (`bin/Reviews/P8/research/{water,vehicles,sky,scale,ui,life}.md`, read only, every claim cited `path:line`). Each milestone's own ADR (0061 onwards) takes its design from these notes, and the lead writes it before the milestone's phase forks.

## Context

- **The game.** Skerries is an archipelago expedition game, built by its own session through the editor's automation, as World Tree is. Its README orders the briefs in three steps:
  1. **The vertical slice:** S01, S02, S03, S04 and S16, then S06.
  2. **The slice's polish and the Hollows:** S17, S07, S05, S13, S10, S09 and S12.
  3. **The later islands and the finale:** S08, S11, S14, S15 and S18.
- **Nothing blocks the game's start.** It works meanwhile with a flat placeholder sea, the existing character and an HDRI sky.
- **What the research found.**
  - **S16 (measured, high priority):** a static entity costs about 2.7 µs of CPU a frame, and 4.5 µs with scripts. The cause is the engine's own bookkeeping:
    - the transform update walks every entity about four times a tick (Architecture §5.2's "no dirty flags");
    - physics pre-step and post-step, audio, UI, animation and the script order scan every entity;
    - extraction and Prepare redo every item each frame;
    - directional shadows batch before each cascade culls.
    S16 is therefore the foundation that S04's scatter, S06's islands and S07's texture views build on.
  - **S06 reopens an accepted limit.** `Docs/Backlog.md` §6 says a play session, the Runtime and an exported game hold one scene at a time. The owner chose to reopen it (2026-11-23). After S16 and S04's scatter, the whole archipelago probably fits one scene's frame time: about 6,120 entities, with the flora as scatter. S06 is then about memory, load time and authoring, so its streaming half waits for the slice's measurements.
  - **The measured world (S16, 2026-11-24).** The applied archipelago, 6,365 entities, runs at 21 frames a second in the editor's two views (47.8 ms of CPU a frame, the GPU at 2.7 ms), about 27 in a game's one view. Lockstep test runs take about 19 ms a tick, so a full run of the game's 23 suites and five replays takes 91 minutes. M43 shortens those too.
  - **The two briefs of 2026-11-24.**
    - **S19:** a character on a Dynamic body lighter than it crosses once, then sinks about 0.2 m into the deck and stalls. M48's plan traced it to M34's documented rule itself, that such a body cannot push the character, so it rises through one it is driven up under: the crossing leaves the deck swinging, and on the way back the planks rise into the capsule (`Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md`, "Context"; the cause is confirmed by its W48.2's first run). M48 changes the rule.
    - **S20:** the bridge ends `test.run`, `input.record` and `input.replay` at a fixed 900 s, so the game's long recordings go round the bridge, and its provenance with them.
  - **The ASan unit run is near its time bound.** At phase 7's gate it took 3,245 s of its 3,600 s: 0.59 s a case, so about 6,100 cases fit. The program adds several hundred cases. It must be sharded before the program's first hardened gate (decision 3).
  - **Same-phase milestones are independent.** Roadmap rule 3 and ADR 0028 decision 17 freeze an interface for another milestone only once it lands in an earlier phase. So no milestone depends on another in its own phase except through the reservation ledger. Each phase below respects that: what a milestone needs from another (S16's static set, S07's raw image, S13's seating, S05's terrain layers) lands in an earlier phase.

## Decisions

### 1. Scope

- **All twenty briefs are in scope.** S20 is a setup item straight to main (decision 3). The other nineteen form twenty-three milestones in five phases, phases 8 to 12, with S19 joining S17's milestone. Phase 12 is confirmed by the owner (2026-11-24).
- **Out of scope** (`Docs/Backlog.md` §2 or §6 rows at each milestone's landing):
  - **Rendering:**
    - the render thread and GPU-driven rendering, which stay cut (question 5);
    - impostors;
    - volumetric clouds;
    - render targets bound by materials;
    - light layers.
  - **Vehicles and input:**
    - tracked vehicles and motorcycles;
    - rumble on pads other than Xbox-compatible ones;
    - rebinding the UI navigation actions.
  - **Navigation:**
    - carving the navmesh around obstacles (DetourTileCache);
    - DetourCrowd.
  - **Scenery:** run-time foliage edits.
  - **Audio:** a playback position that gameplay reads.

### 2. Structure: five phases

| Phase | Milestone | Title | Brief | Landing | Sessions |
|---|---|---|---|---|---|
| 8 | M43 | Static scenery | S16 | **first** (owns the renderer's extraction, Prepare, `ShadowPass`, `RenderSnapshot.h` and Scene's transform files) | ~10 |
| 8 | M44 | Wheeled vehicles | S02 | readiness order; owns `PhysicsSystem*.cpp` and `PhysicsWorld.h` for the phase (M47 and M48 append) | ~7–9 |
| 8 | M45 | Sky and time of day | S03 | readiness order; owns the Forward stage's sky and environment binding | ~10–13 |
| 8 | M46 | Mesh LODs, draw distance and wind | S04 items 1 and 3 | after M43 (shares Prepare, `ShadowPass`, `Scene.slang`, `Shadow.slang`) | ~7 |
| 8 | M47 | Water | S01 | readiness order, after M45 in the Forward stage's list | ~12–14 |
| 8 | M48 | Characters off the ground and on light bodies | S17, S19 | last (appends to M44's `PhysicsSystem` and the frozen `CharacterController.h`) | ~3.5–4 |
| 9 | M49 | Scatter | S04 item 2 | readiness order | ~7.5 |
| 9 | M50 | Island scenes in one world | S06 items 1, 3 and 5 | readiness order; owns `Scene`, `PlaySession` and `PlaySessionPhase` | ~7 |
| 9 | M51 | Tiled navigation | S14 part 1: tiles, links, exclusion | readiness order | ~6–7 |
| 9 | M52 | Camera targets and render layers | S07 part 1: targets, layers, raw image | readiness order; the later lander of M49 and M52 adds the `LayerMask` member to the other's item | ~9–10 |
| 9 | M53 | Fonts from text | S18 | any | ~5.5–6.5 |
| 10 | M54 | Terrain for islands | S05, with its 2026-11-24 steep-face projection | readiness order; owns `ShadowPass` and `Terrain.slang` for the phase | ~10–13 |
| 10 | M55 | Riding and seating | S13 | readiness order | ~3.5–4.5 |
| 10 | M56 | Audio effects and ambience | S09 | any | ~6–7 |
| 10 | M57 | IK and additive layers | S12 | any | ~6.5–7.5 |
| 10 | M58 | UI rotation and masks | S07 part 2 | any | ~6–7 |
| 11 | M59 | Decals and trails | S10 | readiness order; owns the mesh pipelines' surface function | ~10–12 |
| 11 | M60 | Photo mode | S08 | readiness order (Tonemap and HdrPost with M61) | ~7–8 |
| 11 | M61 | Weather | S11 part 1: rain, snow, wetness, lit particles, particle collision | after M59 (the forward layout) | ~9–10 |
| 11 | M62 | Wildlife crowds | S14 part 2: agents, avoidance, obstacles | any | ~5 |
| 11 | M63 | Rumble and rebinding | S15 | any | ~5–6 |
| 12 | M64 | Volumetric light | S11 part 2 | readiness order | ~5–6 |
| 12 | M65 | Island streaming | S06 items 2 and 4 | readiness order; its design takes the slice's measured memory and activation times | ~6–7 |

- **About 175 builder sessions in all**, with the setup's 3 to 4. By phase: 8 about 53, 9 about 37, 10 about 35, 11 about 39 and 12 about 12. Each phase lasts about as long as its longest milestone plus the landing: M47 in phase 8, M52 in 9, M54 in 10 and M59 in 11.
- **Departures from the README's order,** each for a reason the research found:
  - **S04 splits.** Its renderer half (LODs, cull distance, wind) goes in phase 8, and its scatter, which builds on M43's static set, in phase 9.
  - **S14's tiles move up to phase 9, beside S06.** Skerries' next island fits no single tile at a usable cell size, and the tile grid is the interface S06's streaming uses. S14's crowds stay in phase 11.
  - **S18 moves up to phase 9.** It is independent and cheap, the game is building its languages now, and its Japanese font has grown to 68.7 MB, the largest file the exported game ships.
  - **S19 joins S17 in phase 8 (M48).** Both change the frozen `CharacterController.h` and the character's update, and the game's rope bridge works round it with 90 kg planks meanwhile.
  - **S20 goes straight to main in the setup.** It is bridge and automation tooling, and every long recording of the game needs it now.
  - **S13 joins S12 and S05 in phase 10.** S12's hands on the wheel use S13's seating. Each of S13's `RideGround`, S12's `Additive` and S05's Terrain v2 re-hashes the games' scenes, and landing them in one phase makes World Tree and Skerries upgrade and re-record once (decision 6).
  - **S07 splits into two milestones.** Camera targets with render layers and the raw image go in phase 9; UI rotation and masks in phase 10. At about 16 sessions in all, S07 is twice M42's size, and its second half is UI only.
  - **S11 splits into two milestones.** Weather goes in phase 11 and volumetric light in phase 12. Both touch `Fog.slang`, the forward layout and the Particles program, so they cannot run in parallel.
  - **S06 splits into two milestones.** Sub-scenes go in phase 9 and streaming in phase 12, which the owner confirmed (2026-11-24). Its design takes the slice's measured memory and activation times (question 7).

### 3. Setup before phase 8 forks

All of it goes straight to main, with ADR 0051's targeted checks.

1. **Shard the ASan unit run,** as the fuzz step is sharded: `Hardening.py`'s `asan-unit` step and `Test.py`'s sanitized unit run, with their tests. This closes `Docs/Backlog.md` §3's row. It needs one builder session.
2. **S20, long runs through the bridge** (about 1.5 to 2 sessions; ADR 0051's targeted checks: the automation and MCP suites, the references):
   - `test.run`, `input.record` and `input.replay` report progress from the editor: the suite or replay, its tick, and which of a recording's three runs it is in (a recording without input events has two, with no input check; corrected at S20's review);
   - the bridge ends such a call when the editor finishes, however long that takes, and times it out only when its progress stops advancing;
   - the files such a run writes (the replay and a new `.meta`) are transcribed like any bridge write, so `AuditProvenance.py` passes with no re-import;
   - a second client refused while a run holds the main thread is told to retry once the current step of the run ends (corrected at S20's review: the main thread answers again after that step, a suite's collection or a scene load, while the run itself may go on for hours, so waiting for the whole run would be wrong; Architecture §13.2 "Watchdog").
3. **This ADR,** with the Roadmap's phase 8 sections (M43 to M48), Architecture §18's rows, the approvals rows and AGENTS.md's gate line for the program.
4. **Phase 8's reservation ledger,** the section "Phase 8 reservation ledger" below, read from the code at main `3aaf4b10` (ADR 0028 decision 17's form). It covers:
   - components, categories, validation codes, settings sections and script namespaces;
   - `IScriptHost`, automation methods, stats rows, pipelines and passes;
   - the `ViewConstants` and `DrawConstants` layouts and the draw flags;
   - goldens, perf metrics, fuzz targets and FeatureTest IdSeeds (4000 on);
   - the owner of each contended file.
5. **Phase 8's milestone ADRs (0061 to 0066),** written by the lead from the research notes, cross-checked against each other and the ledger, each with its "corrections to the brief" (decision 9).

### 4. Design defaults

The research notes hold each design with its rejected alternatives. These are the shapes each milestone's ADR starts from.

- **M43 (S16):** an authored `Static` component on a scenery root, covering its subtree, plus change tracking of every entity's transform. This replaces §5.2's "no dirty flags", and Architecture §5.2 and §5.7 are amended. The milestone also brings:
  - a static set with CPU cell culling and GPU-resident instances;
  - per-cascade batching after the cull;
  - incremental physics and audio records;
  - a Debug-only full-recompute check and a Lint rule for transform writes.

  The gate is deterministic counters: an unchanged static scene visits no entity a tick, and the shadow draws equal its keys times its cascades. The 2 ms for 20,000 static entities becomes a warning-only perf budget.
- **M44 (S02):**
  - Jolt's `VehicleConstraint`, which is already compiled (`premake5.lua:424-447`), through `WheeledVehicle` and `VehicleWheel` components on the wheel children.
  - A `MassProperties` component (a centre-of-mass offset and inertia), not a `RigidBody` field, so no hash moves. Its offset wraps the body's shape, so every shape reader, the contact listener's collider lookup included, must see through the wrapper.
  - Engine revs and wheel spin are hashed.
  - The milestone leaves a per-layer friction hook for M54.
- **M45 (S03):**
  - a `Sky` component with Hillaire's sky look-up tables (physical constants only, no dependency);
  - run-time image-based light in the existing environment slots;
  - a sun and a moon named on the Sky, which take the fog glow and the cascades;
  - a 2D procedural cloud layer;
  - "sky-relit" probes (irradiance-volume format 2, written as version 1 when a scene has no Sky), with the sun's bounce off the ground left as an accepted limit (question 6).
- **M46 (S04 items 1 and 3):**
  - mesh LODs, authored (`MSFT_lod` or `_LOD<n>`) or generated at import by meshoptimizer (question 2), cross-faded with complementary dither;
  - a `RenderDistance` component;
  - a scene `Wind` component with a wind-weight vertex attribute of its own (`COLOR_0` always tints);
  - cooked mesh version 3 and Material version 3, which upgrade the demos' and FeatureTest's materials without moving a hash.

  Wind must give the prepass and the forward pass bit-identical positions.
- **M47 (S01):**
  - a `Water` component (Ocean or Rectangle) with waves in deterministic double precision;
  - `Water:SampleHeight` and `IsUnderwater`;
  - Jolt's submerged-volume buoyancy against the water's `Density`;
  - a Water pass in the Forward stage after the sky, reflecting through the environment, the probes and SSR;
  - M42's planar reflection as an opt-in, used by the game only after M43 lands.

  An internal contract after W1 lets a physics stream and a render stream run in parallel. Waves are still at time 0 in edit scenes. The sea stays out of mirror views and probe bakes, as fog does.
- **M48 (S17 and S19):**
  - **S17:** `CharacterController:MoveFree(velocity)`, a per-tick call. In the air the character takes the requested velocity with no gravity and no floor-sticking; on the ground it behaves as `Move`. It adds no field and moves no hash.
  - **S19:** a Dynamic body lighter than the character supports the character standing on it (the brief's outcome (a)): Jolt's contact plane and penetration recovery hold the character on the walkable surface of its ground, so a light deck rising under it lifts it and never passes into it. The body's velocity never becomes the character's, a light body is still never a stair, still takes at most its own weight and never squeezes. M34's rising-platform case is rewritten to the new rule, a decided change recorded in ADR 0066. The regression case is the brief's bridge (thirteen hinged 25 kg planks under a 70 kg character, crossed four times, each onto the far rim), which fails on the base first. Outcome (b), a validation warning, is not taken: it would flag every bridge and raft the game wants.
- **M49 (S04 item 2):**
  - a `.foliage` asset (`AssetType` 20, a fuzzed reader) painted per terrain layer through one function that M54 extends;
  - a `Foliage` component drawn through M43's static set;
  - one static compound body per cell;
  - a `foliage.edit` brush method.
- **M50 (S06 items 1, 3 and 5):**
  - islands load into the session's one scene under an anchor entity, with ids derived from the anchor as a prefab's members are;
  - each island is placed at a fixed tick, and its state is ordinary component data, so the hash and the replay format do not change;
  - singletons such as Sky, Water and Environment come from the world scene;
  - one session-wide `AudioSystem`;
  - the editor's world context.

  No origin rebasing is needed at 2.5 km.
- **M51 (S14 part 1):**
  - navmesh format 2 (tiles; version 1 still read), on one world tile grid anchored at the origin;
  - `NavLink` off-mesh links, read by the bake;
  - a run-time area exclusion for the tide;
  - `Navigation.GetAgentTypes()`.

  FeatureTest's `Navigation.replay` is re-recorded, because tiled polygons move its chasers.
- **M52 (S07 part 1):**
  - a `CameraTarget` component, rendered by a small renderer of its own inside the main view's, so the main view's TAA and exposure stay untouched;
  - one `RenderLayer` per entity, inherited by its subtree, and a `CameraLayers` mask on cameras;
  - `UiRawImage`, with pan and zoom;
  - a `RenderLayers` settings section (a `project.upgrade` of every tracked project).

  Texture views reach UI only, on the Screen and World canvases.
- **M53 (S18):**
  - the font import gathers its characters from the string tables and a `Characters` setting, and rebakes when a table changes (FontImporter 6);
  - UAX #14 line breaking through libunibreak (question 2), with today's space handling kept;
  - `Loc` functions that list a table's locales and keys, in byte order;
  - text lines in `ui.tree`, and the cooked size in `asset.info`.
- **M54 (S05):**
  - holes in the heightfield (format 2, written as version 1 when the terrain has no holes);
  - terrains cast into the cascades, the spot tiles and the point faces, with `Terrain.CastShadows` false for migrated terrains;
  - Terrain 2: a second splat map (eight layers) and a named surface per layer through Jolt's per-cell height-field materials;
  - steep faces projected from the side (a triplanar blend past a slope, per layer), so the Spire's sea cliffs and the craters read as rock rather than vertical streaks.

  Holes are one cell wide, so caves keep a mesh collar.
- **M55 (S13):**
  - a `RideGround` field: the character rides its ground rigidly by the body's actual pose change, turns with it, and keeps the deck's velocity when it jumps;
  - a tested seating recipe (remove the controller, parent to the seat, re-add it at a checked exit spot) and `Physics.OverlapCapsule`, rather than an engine seated state.
- **M56 (S09):**
  - filter and reverb nodes (verblib's Freeverb behind a first-party node, question 2), with M40's one-setting-per-pull hand-off to the device thread;
  - one listener-driven reverb blended across zones, and a per-source send;
  - a fixed `Ambience` group with ambience layers;
  - start offsets (`Play(offset)`, `StartOffset`, `RandomStartOffset`);
  - a position that only test runs read.

  Pause muffling is `PauseWorld("Finish")` plus a group low-pass.
- **M57 (S12):**
  - two-bone and look-at IK in the animation phase, before M41's attaching pass, through an `AnimatorIk` component;
  - an `Additive` field on Animator layers, with the clip's first frame as the reference pose;
  - `Animator:GetJointNames()`, which closes M41's §2 row.

  IK runs only in Play and Simulate. Its one tick of lag on a moving character is an accepted limit.
- **M58 (S07 part 2):** `UiRotation` and `UiMask` components, with rotation through DetMath because layout runs in the fixed step, and two masked pipelines. A mask nests only to the nearest one.
- **M59 (S10):**
  - decals evaluated per light cluster inside the one surface function that the prepass, the forward pass and picking share;
  - a `NoDecals` engine tag for characters and vehicles;
  - trails through the particle pass, fogged and unlit.

  There are no decals in mirrors, probe captures or transparent surfaces. All 56 existing goldens must stay byte-identical.
- **M60 (S08):**
  - `Photos` capture by a blocking re-render into `user://Photos`, also in test sessions;
  - loaded photos are display only, never in the hash;
  - a per-camera `CameraLens` for depth of field and vignette.
- **M61 (S11 part 1):**
  - a `Weather` component: GPU rain and snow over an occlusion map, so rain stops at roofs;
  - scene-wide wetness for meshes and terrains;
  - lit particles (ParticleLook 2);
  - `ParticleWorld` collision with terrains, water and planes on the CPU.

  This reopens World Tree's deferral 2f.
- **M62 (S14 part 2):** `NavAgent` and `NavObstacle`, with first-party avoidance (ORCA through DetMath) whose only state is component data. DetourCrowd is rejected: its avoidance calls CRT `cosf` and `sinf`, keeps hidden state and moves its agents itself.
- **M63 (S15):**
  - rumble through XInput on Windows (only Xbox-compatible pads rumble);
  - rebinding through the `Input` namespace, with the player's bindings in `user://Input.json`, applied only in live Runtime runs, as `Display.json` is, so replays and suites always use the project's bindings.
- **M64 (S11 part 2):** froxel volumetric fog, a `VolumetricFog` component. This reopens World Tree's deferral 2a.
- **M65 (S06 items 2 and 4):**
  - asynchronous island loading at a fixed activation tick: on a slow disk the session stalls briefly rather than diverge (question 7);
  - per-scene navigation tiles;
  - asset eviction.

### 5. Approvals

Three new vendored libraries, all from official upstream with permissive licences. Each is pinned and checksummed with its `VENDOR.md` at its milestone. A first-party fallback exists for each.

| Library | For | Licence | Fallback |
|---|---|---|---|
| meshoptimizer | M46's generated LODs | MIT | authored LODs only |
| libunibreak 8.0 | M53's UAX #14 line breaking | zlib | first-party UAX #14, about 1.5 more sessions |
| verblib (from miniaudio's repository) | M56's reverb | MIT-0 or public domain | a first-party Freeverb |

### 6. Determinism, formats and hashes

**No milestone moves a committed demo replay except M54.** Every format change keeps its old version readable.

| Milestone | Format | Hash and upgrade impact |
|---|---|---|
| M43, M44, M47, M48, M50, M56, M58, M60, M62, M64, M65 | new components only | none |
| M45 | irradiance volume 2 (version 1 written without a Sky) | none |
| M46 | cooked mesh 3, Material 3 | a `project.upgrade` of the demos' and FeatureTest's materials; no hash moves |
| M49 | `.foliage` 1 (fuzzed), `AssetType` 20 | none |
| M51 | navmesh 2 (fuzzed; version 1 read) | FeatureTest's `Navigation.replay` re-recorded |
| M52 | the `RenderLayers` settings section | a `project.upgrade` of every tracked project; the builder confirms that project settings reach no replay hash |
| M53 | FontImporter 6 | every font's `.meta` rewritten; a wrapped Japanese golden may move, reviewed with the golden-images skill |
| M54 | heightfield 2 (fuzzed; version 1 written without holes), Terrain 2 | RollingBall's three level replays and FeatureTest's `Terrain.replay` re-recorded |
| M55 | `RideGround` on CharacterController | every scene with a character re-hashes (no tracked demo has one) |
| M57 | `Additive` on Animator layers | every scene with an Animator re-hashes (no tracked demo has one) |
| M59 | new components and a reserved tag | none; the 56 goldens byte-identical |
| M61 | ParticleLook 2 | a `project.upgrade` of FeatureTest; no committed hash moves |
| M63 | `user://Input.json` (fuzzed) | none |

**World Tree and Skerries** upgrade and re-record in their own sessions, through the product owner:
- after phase 9, the navmesh rebake if they want tiles;
- after phase 10, all three re-hashes at once;
- after phase 11, the particle looks.

### 7. Process

- **As in World Tree's program:**
  - one builder per milestone, in waves, each in its own worktree;
  - a contract only inside a milestone, as M47 has;
  - one independent review per wave, with every finding fixed;
  - batch landing per phase in train order, with a merge review and one gate;
  - every phase ends with the push and the owner's go-ahead.
- **Gates (the owner's answer 4, 2026-11-24).** The hardening runs once, at the end of the program's last phase: its last lander's `CI.py --milestone` (about 8.5 h on a quiet machine, announced first). Phase 12's, which the owner confirmed (2026-11-24). Every other phase gate is `CI.py --require-clang-cl`, as the World Tree program's phase 5 was (ADR 0042, the owner's answer 9). This departs from ADR 0028 decision 12, which would harden every phase that touches the renderer, core systems or fuzzed readers.
  - **What each phase gate still runs:** the unit suite's fuzz cases (10,000 seeded mutations per reader in Release).
  - **What waits for the program's end:** the 1,000,000-iteration fuzz of the new readers, the ASan runs, the soak and the debug heap. `Docs/Backlog.md` §3 gains a row for that until then.
- **Two sessions in one checkout.** The game sessions work in `D:\Engine`. The program works in worktrees and reads only the games' `Design/EngineTasks/` folders.
- **Briefs keep arriving.** Each phase start re-lists both briefs folders. A new brief joins a later phase by an amendment to this ADR.

### 8. Reservation ledgers

- **One ledger per phase,** written before the phase forks, from the code at its setup tree.
- **The program's hottest shared files,** in order of contention:
  1. `ShadowPass.cpp`: M43, M46, M49, M54 and M61;
  2. `SceneRenderer.cpp`'s Prepare and `RenderExtraction.cpp`: M43, M46, M47, M49, M52 and M59;
  3. the forward layout and `Scene.slang`: M46, M47, M59, M61 and M64;
  4. `PhysicsSystem*.cpp`: M44, M47, M48, M49, M50, M54 and M55;
  5. `PlaySession.cpp`: M50, M56, M57 and M62;
  6. `BuiltinComponents.h` and `ProjectValidator.cpp`: almost every milestone.
- **What the phase plan already does with them:** the first lander of each phase owns these files, and no phase holds two milestones that rewrite the same function.

### 9. Corrections to the briefs

Each correction goes to the Skerries session through the product owner. "Today's engine" items are facts about 0.8.0, which the game can use now. The rest follow this plan's answers.

- **Today's engine:**
  - **Entity cap:** the 65,536-entity cap is the default of `Simulation.MaxEntities`, not a hard limit (S04, S06, S16).
  - **Far clip:** a camera's default `FarClip` is 1,000 m, and it culls the islands beyond it. Raise it, to 3,000 m for the archipelago (S04).
  - **Vertex colour:** `COLOR_0` always tints the base colour, so wind weights cannot live there (S04).
  - **Sea floor:** a `NotWalkable` NavModifier box from the waterline down already leaves the sea floor out of the navmesh. Only the tide needs a run-time exclusion (S14).
  - **Navigation components:** they are hidden from scripts on purpose (ADR 0045 decision 5) (S14).
  - **Bake cap:** the 4,096-cell cap is per agent type and per bake (S14).
  - **Cameras:** validation refuses only several *Primary* cameras. Any number of other cameras is valid, and `viewport.screenshot {camera}` already renders one (S07).
  - **Environment fields:** every Environment field is script-writable. What is missing is a tint for a map (S03).
  - **Fog glow:** `Fog.SunGlow` and the cascades follow the first directional light in hierarchy order, so a moon placed above the sun takes both (S03).
  - **Ground carry:** a character already rides a turning or heaving deck: Jolt's ground velocity carries rotation and vertical motion. The real gaps are its facing, a per-tick lag on a force-driven boat (about 0.4 to 0.8 m of drift over 10 s in a 5 m/s turn) and the deck's velocity lost when it jumps (S13).
  - **Light boats:** a boat lighter than the character (under the 70 kg default `Mass`) cannot be ridden today; M48 changes that rule so a light body supports a character standing on it (S13, S19).
  - **Air control:** in the air, `Move`'s horizontal part already applies in full every tick; only the vertical is missing (S17).
  - **Replays and bindings:** replays hold device events as well as actions, so a player's rebinding must never reach a replay, a suite or a lockstep session (S15).
  - **Particle cap:** the 4,096-particle cap is a code constant, not a §6 row (S11).
  - **Wet terrain:** terrains take no `MaterialParameters`, so a roughness parameter cannot wet the ground; wetness must be scene-wide (S11).
  - **Particle collision:** collision against the depth buffer does not fit CPU particles. Rain stops at an occlusion map on the GPU, and emitter particles collide with terrains, water and planes on the CPU (S11).
  - **Terrain friction:** a `TerrainCollider` already has a collider-wide `Friction`, `Restitution` and `Layer` (S05).
  - **Simulate:** Simulate has no audio at all (S09).
  - **Audio positions:** `audio.stats` already reports each voice's position and length (S09).
  - **Audio hashing:** new effect components are serialized and hashed like any component, so "off the state hash" holds for voices only (S09).
  - **Pause muffling:** muffling the world behind the pause menu is `PauseWorld("Finish")` plus a group low-pass; `"Pause"` holds the world's voices (S09).
  - **Animation:** animation is fixed-step only; there is no rendered-pose evaluation (S12).
  - **Water from below:** a single-sided Blend plane is culled from below, so a surface seen from underneath needs a double-sided material (S01).
- **After this plan's answers:**
  - **Water hashing:** water is a pure function of its component and the tick, so it is "deterministic", not "hashed" (S01).
  - **Mirrored water:** M42's planar reflection on water is an opt-in, affordable only after M43 (S01).
  - **Centre of mass:** the offset is a `MassProperties` component beside `RigidBody` (S02).
  - **Terrain surfaces:** per-terrain-layer surfaces arrive with M54. Until then, use the `TerrainCollider`'s friction and `ColliderSurface`s (S02).
  - **Mirrors and decals:** the sea and decals do not show in mirrors (S01, S10).
  - **Trails:** trails are unlit (S10).
  - **Decal opt-out:** characters and vehicles opt out of decals with the `NoDecals` tag (S10).
  - **Rumble:** only Xbox-compatible pads rumble (S15).
  - **Holes:** holes are one heightfield cell wide, about 1.2 m on Ridgeback, so caves keep a mesh collar (S05).
  - **Layer contact:** the "dominant layer at a contact" is the layer of the contact manifold's cell (S05).
  - **Raw splat maps:** per-layer surfaces need raw splat maps, the kind `terrain.edit` makes (S05).
  - **Foot check:** the 2 cm foot check measures the sole, not the ankle joint (S12).
  - **Hands on the wheel:** they stay exact only when the driver is parented to the vehicle, by M55's seating (S12).
  - **Additive reference pose:** an additive layer's reference pose is the clip's first frame (S12).
  - **Look-at axis:** the look-at carries the head's forward axis as a field (S12).
  - **Playback position:** a start offset lands, but gameplay never reads a playback position; only test runs do (S09).
  - **Loading completion:** island loading completes at a tick the simulation fixes, never when a worker finishes (S06).
  - **Load on a worker:** `AssetManager::LoadAsync` exists. What is missing is scene-level preparation, a GPU upload budget and eviction (S06).
  - **Island addressing:** island entities are addressed by path, such as `/Islands/Landfall/...` (S06).
  - **Atlas size:** with 384 Japanese characters, the brief's 2026-11-23 count, a baked set should fit about a 2,048-pixel atlas against today's 8,192, about a sixteenth. ASCII and Latin-1 are always baked. This is an estimate; M53 measures it (S18).
  - **Latin breaking:** UAX #14 also breaks after hyphens and slashes, so Latin breaking changes there (S18).
  - **Key order:** `Loc.GetKeys` returns byte order (S18).
  - **Literal UI texts:** fonts take an explicit `Characters` string rather than scanning scenes (S18).
  - **Photos in tests:** test sessions with a GPU really capture photos (S08).
  - **The S19 sink:** it is M34's documented rule meeting a swinging deck, not a separate defect; M48 changes the rule, so a light body supports a character standing on it, and the game's planks can go back to 25 kg after phase 8 (S19).
  - **Lens:** depth of field lives on the camera (`CameraLens`), not on `PostProcess` (S08).

## Phase 8 reservation ledger

Phase 8's ledger (decision 8; ADR 0028 decision 17's form, as ADR 0042's phase 5 ledger and ADR 0052's phase 6 ledger), read at main `3aaf4b10` (engine 0.8.0), every value cited `path:line`. It was compiled from the six milestone drafts and a cross-check of them against each other and the code (31 findings, each applied to the milestone ADRs before this commit).
- **Train order:** M43, M44, M45, M46, M47, M48. In every shared list a later lander appends after the earlier ones and never reorders them. M46 merges `landing/phase8` once it holds M43, M44 and M45, then adapts in W46.4.
- **The rule:** where a milestone's ADR and this ledger differ, the ledger is the rule, and a deviation needs an amendment here on main.

### Format versions

| Format or version | Today (`3aaf4b10`) | Owner | Reserved, and after phase 8 |
|---|---|---|---|
| Cooked mesh `MeshData::FormatVersion` | 2 (`Engine/Source/Engine/Asset/MeshData.h:141`) | M46 | **3.** Version 2's `Reserved` word becomes `LodCount`. The wind section follows the morph section, then the levels' section; `MeshStreams::Wind` is bit 3. No reader keeps 2: every cooked mesh is rebuilt (M23's rule). Seeds of the `MeshData` fuzz target gain version 3 payloads. |
| Material: the file `MaterialData::CurrentVersion`, the cooked `FormatVersion` and `MaterialImporter::Version` | 2, 2 (`Asset/MaterialData.h:64`, `:66`) and 2 (`AssetPipeline/Importers/MaterialImporter.h:22`) | M46 | **3 and 3,** with the importer unchanged at 2. `MaterialWind` is the field `Wind`, after `UVOffset`. Files at 1 and 2 read with wind disabled and save as 3; a cached artifact of another payload version is rebuilt. Fixtures: `Tests/Data/Formats/Material/MaterialV1.upgraded.material` moves to 3; `MaterialV2.material` and `MaterialV2.upgraded.material` are new. The 140 tracked materials move to 3: FeatureTest's 118 through `Scaffold.py`, and the 22 of Rolling Ball (11), Breakout (8) and NativeTest (3) through a recorded `Editor --headless --project <P> --upgrade`. |
| `GltfImporter::Version` | 3 (`AssetPipeline/Importers/GltfImporter.h:180`) | M46 | **4:** five settings after `AnimationEvents` (`Lods` as `GltfLodSource`, `LodScreenSizes`, `LodTriangleRatios`, `LodMaxError`, `Wind`). A `.meta` without them reads the defaults, so no `.meta` is rewritten. |
| Irradiance volume: `IrradianceVolumeData::FormatVersion` and `IrradianceVolumeImporter::Version` | 1 (`Asset/IrradianceVolumeData.h:64`) and 1 (`AssetPipeline/Importers/IrradianceVolumeImporter.h:24`) | M45 | **2**, with the importer unchanged at 1. A relight block follows the coefficients. A volume without the block is written as version 1, byte for byte today's, so FeatureTest's committed bakes hold. The cooked payload is always 2, and no `.meta` moves. The `IrradianceVolumeData` fuzz seed gains a block. |
| Entity document, replay, graph, project file, pak, `GameManifestBuildVersion`, `NativeApiVersion`, protocol | 1, 5, 2, 1, 2, 2, 1 and {1,0} (ADR 0058's ledger) | none | Unchanged. |
| Existing components | today's versions | none | Unchanged. M44's centre of mass is the `MassProperties` component, not a `RigidBody` field. M46's cull distance is `RenderDistance`, not a `MeshRenderer` field. M48 adds no field: `CharacterController` stays version 1, and only the descriptions of `Mass` and `GetVelocity` change. |
| New components | — | M43, M44, M45, M46, M47 | Each at version 1: `Static` (M43); `MassProperties`, `WheeledVehicle` and `VehicleWheel` (M44); `Sky` (M45); `RenderDistance` and `Wind` (M46); `Water` and `Buoyancy` (M47). |
| `EngineVersionString` | 0.8.0 (`Engine/Source/Engine/Core/BuildId.h:38`) | **M48, the last lander** | **0.9.0** at M48's merge into `landing/phase8`, with FeatureTest regenerated by `Scaffold.py` on the merged tree. The demos' committed replays keep the versions they were recorded with. No other lander bumps it. |

### Allocations

#### Registries

| Allocation | Today (`3aaf4b10`) | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| ADR numbers | 0059 is the last record and 0060 the program | M43 0061, M44 0062, M45 0063, M46 0064, M47 0065 and M48 0066, each amended per wave | A record outside the milestones takes 0067 on. |
| Component categories (`Scene/Components/BuiltinComponents.h:78-129`; registration functions `:134-158`; `BuiltinComponentsTests.cpp:750`) | 16: Core, Rendering, Physics, Audio, Scripting, Rendering2D, Physics2D, Ui, Animation, Particles, Probes, Terrain, MaterialParameters, Fog, Navigation, Joints. 16 functions, `RegisterJointsComponents` last. | M44: Vehicles (`Registration/VehiclesRegistration.cpp`, `RegisterVehiclesComponents`).<br>M46: RenderDistance (`RegisterRenderDistanceComponents`), then Wind (`RegisterWindComponents`), each in a registration file of its own.<br>M47: Water (`Registration/WaterRegistration.cpp`, `RegisterWaterComponents`). | **20:** the 16, then Vehicles, RenderDistance, Wind and Water. `RegisterBuiltinComponents` (`BuiltinComponents.cpp`) calls the four new functions in that order, and `BuiltinComponentsTests.cpp:750`'s list ends with them. |
| Components (`BuiltinComponents::Size`, `BuiltinComponentsTests.cpp:44`) | 64. Core ends with `PrefabLink` (`BuiltinComponents.h:81`), Rendering has `Environment` sixth (`:84`), Physics ends with `ColliderSurface` (`:88`) and Joints ends with `FixedJoint` (`:129`). | M43: `Static` after `PrefabLink` (mid-list).<br>M44: `MassProperties` after `ColliderSurface` (mid-list), and `WheeledVehicle` and `VehicleWheel` in Vehicles.<br>M45: `Sky` after `Environment`, before `PostProcess` (mid-list).<br>M46: `RenderDistance` and `Wind`, each alone in its category.<br>M47: `Water` and `Buoyancy` in Water. | **73.** |
| Registry enums and structs | — | M43: `StaticScope` {Subtree, EntityOnly, NotStatic}.<br>M44: `VehicleTransmissionMode`, `VehicleWheelCast`, `VehicleCurveKey`, `VehicleDifferential`, `VehicleAntiRollBar`.<br>M46: `MaterialWind` (Material) and `GltfLodSource` (an import setting).<br>M47: `WaterShape`, `WaterWave`. | Every value of `StaticScope`, `VehicleTransmissionMode`, `VehicleWheelCast` and `WaterShape` is used in FeatureTest (gate 1b). |
| Runtime-only components | as §5.3 lists | M43: `HierarchyStaticTag` (amended 2026-11-24: the canonical keys are a scene-side table, not a runtime-only component; `Docs/Decisions/0061-m43-static-scenery.md`, the W43.1 record). | Neither is serialized or hashed. |
| Validation codes (`EditorCore/Project/ProjectValidator.cpp:115-225` `ValidatorCodes`, `:230-500` `ValidatorCodeInfos`, `:514` static_assert) | 75, the last M42's `RENDER_REFLECTION_INVALID` (`:224`) | Appended to both tables in this order, none auto-fixable:<br>M43: `SCENE_STATIC_MOVES` (Warning).<br>M44: `PHYSICS_MASS_UNUSED` (Warning), then `PHYSICS_VEHICLE_INVALID` (Error).<br>M45: `SKY_LIGHT_INVALID`, `SKY_ENVIRONMENT_IGNORED` and `PROBE_NOT_RELIGHTABLE` (Warnings).<br>M46: `ASSET_LOD_INVALID` (Warning; an import diagnostic).<br>M47: `WATER_INVALID`, `WATER_WAVES_TOO_STEEP` and `PHYSICS_BUOYANCY_INVALID` (Warnings). | **85,** the last `PHYSICS_BUOYANCY_INVALID`. |
| Validation code arrays (`ProjectValidatorTests.cpp:1386-1388`'s count line and `:1450-1454`'s order chain) | `59 + PhysicsDiagnosticCodes + Physics2DDiagnosticCodes + PhysicsSurfaceDiagnosticCodes + PhysicsJointDiagnosticCodes + BoneAttachmentDiagnosticCodes + PlanarReflectionDiagnosticCodes`; `codes.back()` is `RenderReflectionInvalidCode` | Each code sits in an array of its own milestone's header:<br>M43: `StaticSceneryDiagnosticCodes` (1), `Scene/StaticScenery.h`.<br>M44: `PhysicsMassDiagnosticCodes` (1), `Scene/PhysicsMassValidation.h`; `PhysicsVehicleDiagnosticCodes` (1), `Scene/PhysicsVehicles.h`.<br>M45: `SkyDiagnosticCodes` (3, `PROBE_NOT_RELIGHTABLE` included), `Scene/SkyValidation.h`.<br>M46: `AssetLodInvalidCode`, inline in `Asset/AssetDiagnostic.h` beside the other `ASSET_*` codes, and in `GetAssetDiagnosticCodes()` (`Asset/AssetDiagnostic.cpp:12`, 13 codes, which becomes 14).<br>M47: `WaterDiagnosticCodes` (3), `Scene/WaterValidation.h`. | The count line becomes `60 + ... + StaticSceneryDiagnosticCodes + PhysicsMassDiagnosticCodes + PhysicsVehicleDiagnosticCodes + SkyDiagnosticCodes + WaterDiagnosticCodes`. The order chain runs from `RENDER_REFLECTION_INVALID` through the ten codes, and `codes.back()` is `PhysicsBuoyancyInvalidCode`. |
| Validation checks (`ProjectValidator.cpp:1656-1672` `CheckScene`; `:1247-1275` `CheckPhysics`) | `CheckScene` ends with `CheckPlanarReflections` (`:1670`) and then `CheckRenderScene`. `CheckPhysics` ends with M37's joints (`:1258-1260`). | M44: inside `CheckPhysics`, after the joints, the mass diagnostics and then `ComposePhysicsVehicles`' diagnostics.<br>M45: `PROBE_NOT_RELIGHTABLE` inside `CheckProbes`.<br>M46: none in `CheckScene`. Its code arrives as an import diagnostic (`ImportDiagnosticCodes`, `:522-528`, gains it).<br>After `CheckPlanarReflections`, in this order: M43's `CheckStaticScenery`, M45's `CheckSky`, M47's `CheckWater`. | Each check is a call of its own, and none reorders another. |
| Settings sections (`Project/ProjectSettings.h:284-298`) | 12: Window, Simulation, Physics, Input, Rendering, Scripting, Export, Testing, SortingLayers, Physics2D, Localization, Ui | none (M43's 32 m cell is an engine constant) | 12 |
| `AssetType` | None 0 to NavMesh 19 | none | unchanged |
| Built-in handles (`Asset/BuiltinAssets.h:77`) | Textures 0x0181 to 0x018c, `DissolveNoiseTexture` last | M47: `WaterNormal` 0x018d and `WaterFoam` 0x018e, both procedural and never compressed. `Resources/EngineAssets.json` stays sorted by handle. | Textures 0x0181 to 0x018e, 14 of the block's 64. |
| `PlaySessionPhase` | the order below | none | unchanged |
| State-hash blocks | Scene, tick, Random, id draws, physics (bodies, characters, the joint part last), native, physics2d, ui, save data, world pause, locale | M44 adds a vehicle part at the end of the physics block, after the joint part (`Scene/PhysicsSystem.cpp:2569`), and `AppendStateHash`'s early return (`:2533`) also requires that no vehicle frames are kept. The others add none: M43's set and tags, M45's and M46's rendering, M47's planes and M48's mode are not hashed. | No new block. The physics block ends with the joint part, then the vehicle part. |

#### Script API

| Allocation | Today (`3aaf4b10`) | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| Namespaces (`Resources/Scripting/Engine.d.luau:610-2367`) | 22 | None. M47's `Physics.SampleWater` and the alias `WaterSample` reopen `Physics` from `Bindings/Water.cpp`, so `Bindings/Physics.cpp` is untouched. | 22 |
| `RegisterBindings.cpp` and `Scripting/Private/BindingRegistration.h` | ends with `RegisterJoints` (`RegisterBindings.cpp:57`) | `RegisterVehicles` (M44, `Bindings/Vehicles.cpp`), then `RegisterWind` (M46, `Bindings/Wind.cpp`), then `RegisterWater` (M47, `Bindings/Water.cpp`) | Three lines after `RegisterJoints`. |
| Proxy types (generated) | One per script-visible component. `Navigation` and `NavModifier` are hidden (ADR 0045 decision 5). | M43: `Static`. M44: `MassProperties`, `WheeledVehicle`, `VehicleWheel`. M45: `Sky`. M46: `RenderDistance`, `Wind`. M47: `Water`, `Buoyancy`. | +9 types, all script-visible. |
| Proxy methods | `CharacterController` has 7 (`Bindings/Components.cpp:532-550`) | M44: 12, on `WheeledVehicle` (`GetSpeed`, `GetEngineRpm`, `GetGear`, `IsShifting`) and `VehicleWheel` (`IsTouching`, `GetContact`, `GetSuspensionLength`, `GetSuspensionForce`, `GetRotation`, `GetSteerAngle`, `GetAngularVelocity`, `GetSlip`).<br>M46: `Wind:Sample`.<br>M47: `Water:SampleHeight`, `SampleNormal`, `SampleVelocity`, `IsUnderwater`.<br>M48: `CharacterController:MoveFree`, after `Move` (`Components.cpp:533`). | 18 new methods; `CharacterController` has 8. |
| Namespace functions and aliases | — | M47: `Physics.SampleWater` and the alias `WaterSample` | +1 function, +1 alias |
| Behaviour callbacks and reserved field names | 28, ending with `OnJointBreak` and `OnLocaleChanged` | none | 28 |
| `IScriptHost` (`Scripting/ScriptHost.h`) | ends with `GetMusic` (`:250`) | none. The reads go through `GetPhysics()` and the host's frame state. | unchanged |
| `PerformanceStats` (`Scripting/ScriptPerformanceStats.h:15-50`) | 24 members, ending with `DroppedPlanarReflections` | M43: `TransformsUpdated`, `EntitiesVisited`, `StaticCellRebuilds`, `StaticItems`, `StaticCellsVisible`.<br>M45: `SkyUpdates`.<br>M46: `LodCrossFades`, `DistanceCulledMeshes`.<br>M47: `WaterSurfaces`. | **33,** appended in that order. They are filled by `Session/SessionStatistics.cpp` and returned by `Bindings/Application.cpp`. |
| `ScriptLimits.h` | as today | none (M44's caps are engine constants) | unchanged |

#### Methods and stats

| Allocation | Today (`3aaf4b10`) | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| Automation methods (`Docs/Reference/Methods.json`, Architecture §13.5 "(126)" at `Docs/Architecture.md:1857`) | 126 | M44: `physics.vehicleInfo`, registered by `RegisterPhysicsMethods` and `RegisterPhysicsMethodTypes`. It is in the observe domain, read-only, batch-allowed and in the Runtime subset, with `SceneRelation::None`.<br>M47: `water.sample`, a shared method: `RegisterWaterMethodTypes` after `RegisterUiMethodTypes` (`RegisterSharedMethods.cpp:47`) and `RegisterWaterMethods` after `RegisterUiMethods` (`:71`), with `SceneRelation::Target`. | **128.** `RegisterMethods.cpp` is unchanged. `SceneTargetTests.cpp` (`:147-151`) gains both rows, in train order. |
| MCP tools (`Tools/MCP/catalog.json`, `PROXIED_TOOLS`) | 86 | none (neither method is a tool) | 86 |
| Result and param fields | `physics.bodyInfo` ends `..., groundCollider, squeezed, squeezedBy, tick` (`Automation/Methods/PhysicsMethods.h:55-82`); `LightingBakeProbe` holds `{entity, path, asset, changed}` (`EditorCore/Automation/LightingMethods.h:36-42`) | `physics.bodyInfo`, before `tick`: M44's `centerOfMass`, then M47's `buoyancy {water, submergedVolume, totalVolume}` (null without a `Buoyancy`), then M48's `lastMove` ("Walk" or "Free", empty for a body that is not a character). In the engine, `PhysicsBodyReport` gains `CenterOfMass` and then `Buoyancy` after `Character` (`Scene/PhysicsSystem.h:425`), and `PhysicsCharacterState` gains `LastMove` (`:368-382`).<br>`lighting.bake`'s `LightingBakeProbe` gains M45's `relightable` (M26's frozen wire format, a reviewed change). | — |
| `stats.get` top level (`StatsGetResult`) | 18 fields, ending with `particles` | M43: `transformsUpdated`, `entitiesVisited`, `staticCellRebuilds` (0 without a session) | 21 |
| `stats.get` view summary (`StatsViewSummary`) | 32 fields, ending with `planarReflectionEntities` | M43: `staticItems`, `staticCells`, `staticCellsVisible`, `dynamicItems`.<br>M45: `skyUpdates`, `relitIrradianceVolumes`.<br>M46: `meshLodDraws` (five counts), `lodCrossFades`, `distanceCulledMeshes`.<br>M47: `waterSurfaces`, `underwater`. | 43 |
| `RenderStats` (`Renderer/RenderStats.h:98-101`) | ends with `PlanarReflectionEntities` | M43: `StaticItems`, `StaticCells`, `StaticCellsVisible`, `DynamicItems`.<br>M45: `SkyUpdates`, `RelitIrradianceVolumes`.<br>M46: `MeshLodDraws` (five), `LodCrossFades`, `DistanceCulledMeshes`.<br>M47: `WaterSurfaces`, `Underwater`. | +11 fields, appended in that order. |
| Stats panel rows (`Editor/Panels/StatsPanel.cpp:172`) | ends with "Planar reflections" | "Static scenery" (M43), "Sky" (M45), "Mesh LODs" (M46), "Water" (M47) | +4 rows. The landing renders the six editor-UI goldens on the merged tree (the rules at the end). |
| Pass stats rows (`RenderPassDeclarationTests.cpp:122-127`) | 41 | M45: `SkyLuts`, `SkyIbl` and `ProbeRelight` after `LightClusters`, and `Sky` after `Skybox`.<br>M47: `WaterRefraction` and `Water` after `Sky`, and `Underwater` after `Particles` (first in HdrPost). | **48** (the full list is under "Render passes"). |

#### Data and formats

| Allocation | Today (`3aaf4b10`) | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| Goldens (`Tests/Golden/nvidia-62x/`) | 56: 50 scene goldens and the six editor-UI images | M45: `SkyDay`, `SkyOvercast`, `SkyProbes` (`GoldenSky.jsonl`; `SkyProbes` depends on its bake's device class, `Docs/Backlog.md:285`).<br>M46: `MeshLods` (`GoldenMeshLods.jsonl`) and `Wind` (`GoldenWind.jsonl`).<br>M47: `Water`, `WaterUnderwater`, `WaterWaterline` (`GoldenWater.jsonl`).<br>M43, M44 and M48: none. Every existing golden stays byte-identical, and a drift is a bug, never a golden update. | **64.** The golden-images skill's scene list takes them in train order. |
| Perf metrics (`Scripts/Lib/perf.py:49-50` `BUDGETS`; parts in `Scripts/Perf.py:88` `CAPTURE_PARTS`) | 4 budgets: `playCopy/1000Entities` 30, `fog/Fog/gpuCostMs` 0.5, `navigation/150Agents/tickMs` 8.0, `reflections/Mirrors3/gpuMs` 2.0. Parts: playCopy, scenes, navigation, replay, look. | M43: `staticScenery/20000Entities/cpuMs` 2.0, and `stateHash/20000Entities/ms` with no budget, in a new part `staticScenery`.<br>M45: `sky/Static/gpuMs` 0.3 and `sky/Turning/gpuMs` 0.6, in `look`.<br>M46: `lod/Forest/gpuMs` 3.0, in `look`.<br>M47: `water/Ocean/gpuMs` 1.0 and `water/Underwater/gpuMs` 0.3 in `look`, and `water/Buoyancy64/tickMs` 1.0 in a new part `water` (a `--renderer none` editor's tick). M47's budgets stay UNVERIFIED until they are measured on `nvidia-62x`. | **11 budgets and 12 metrics,** all warning-only. `CAPTURE_PARTS` gains `staticScenery` and then `water`. `test_perf.py` gains each milestone's cases. |
| Fuzz targets (`Tests/Data/Fuzz/Targets.json`) | 53 | None new. Seeds are extended: M45 `IrradianceVolumeData` (format 2); M46 `MeshData`, `MaterialData` and `GltfImporter` (version 3 payloads and materials, the level and `_WIND` glTFs); M47 `Bindings` (the water calls) and `SceneSerializer` (`Tests/Data/Scenes/AllComponents.scene` gains a `Water` and a buoyant body); M48 `Bindings` (a `MoveFree` family, `ScriptBindingFuzzTests.cpp:91`). | 53. The 1,000,000-iteration run waits for the program's hardened gate (ADR 0060 decision 7). |
| Vendored libraries (`Vendor/`) | 25 directories | M46: meshoptimizer (approved, ADR 0060 decision 5), as `Vendor/meshoptimizer/` with its `VENDOR.md` | 26 |

#### FeatureTest

| Allocation | Today (`3aaf4b10`) | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| IdSeed blocks | `Projects/FeatureTest/Scaffold/Scaffolds.json`: 101 scaffolds, the highest seed 3902 (`GoldenPlanarReflections.jsonl`), and the last two `Suites.jsonl` (1515) and `Replays.jsonl` (1516). | M43 4000-4099, M44 4100-4199, M45 4200-4299, M46 4300-4399, M47 4400-4499, M48 4500-4599, all free. | Taken: 4001-4002, 4101-4102, 4201-4203, 4301-4305, 4401-4403, 4501-4502. T-SEEDGUARD refuses a duplicate. |
| Scaffolds | 101 | Listed after main's in train order, all before `Suites.jsonl` and `Replays.jsonl`:<br>M43: `StaticScenery.jsonl` 4001, `StaticSceneryReplay.jsonl` 4002.<br>M44: `Vehicles.jsonl` 4101, `VehiclesReplay.jsonl` 4102.<br>M45: `Sky.jsonl` 4201, `SkyReplay.jsonl` 4202, `GoldenSky.jsonl` 4203 (golden; `Renderer` vulkan).<br>M46: `MeshLods.jsonl` 4301, `Wind.jsonl` 4302, `WindReplay.jsonl` 4303, `GoldenMeshLods.jsonl` 4304 (golden), `GoldenWind.jsonl` 4305 (golden).<br>M47: `Water.jsonl` 4401, `WaterReplay.jsonl` 4402, `GoldenWater.jsonl` 4403 (golden).<br>M48: `CharacterMoves.jsonl` 4501, `CharacterMovesReplay.jsonl` 4502. | **118;** the highest seed is 4502. A golden scaffold writes only below its `GoldenDirectories`. |
| Suites (`Testing.Suites`, one `Suites.jsonl` patch line unioned by hand) | 116 | `StaticScenery` (M43), `Vehicles` (M44), `Sky` (M45), `MeshLods` and `Wind` (M46), `Water` (M47), `Physics.CharacterMoves` (M48). Each runs in the Editor, Release and Dist modes. Amended 2026-11-24 (M48's W48.3 review): a replay's recording suite, `Record.<Name>` (Editor only, as main's `Record.Physics2D` and `Record.TextInput`), is bound too where the milestone's replay scaffold binds one, after its milestone's suite: M48's `Record.CharacterMoves`. | 123 plus the recording suites bound (124 with M48's; at most 129, one per new replay). The landing counts the merged tree. |
| Replays (`Assets/Tests/Replays/*.replay`) | 26 | `StaticScenery`, `Vehicles`, `Sky`, `Wind`, `Water`, `CharacterMoves`, each recorded by its own milestone's scaffold, never in `Replays.jsonl`. Each verifies at one hash in Debug, Release and Dist and fails without its input. M48's last lander re-records every replay at 0.9.0. | 32 |
| Scenes (tracked `.scene` files in FeatureTest) | 104 | M43: `StaticScenery`.<br>M44: `Vehicles`.<br>M45: `Sky`, `GoldenSky/SkyDay`, `GoldenSky/SkyProbes` (amended 2026-11-24: a golden scene's image takes its scene's name, so the scenes are named after the goldens `SkyDay` and `SkyProbes`, and `Probes` is GoldenProbes' scene already; ADR 0063's W45.2 record).<br>M46: `MeshLods`, `Wind`, and its two golden scenes.<br>M47: `Water` and its golden scene.<br>M48: `CharacterMoves`. | 116, all soak subjects at the program's hardened gate. |
| Coverage gates | — | Gate 1: every new component. Gate 1b: `StaticScope`, `VehicleTransmissionMode`, `VehicleWheelCast`, `WaterShape`. Gate 2: M44's 12 methods, `Wind:Sample`, M47's four methods and `Physics.SampleWater`, `MoveFree`. Gate 4: every new proxy's fields. Gate 5: `physics.vehicleInfo` (`test_physics.py`), `water.sample` (`test_water.py`). Gate 6: the ten fixtures. Gate 7: every description. Gate 9: mesh and Material version 3 (M46). Gate 8: none. | — |

#### Validation fixtures (`Tests/Data/Generate/MakeValidationFixtures.py`)

- **`COMPONENT_ORDER`** (`:73-84`):
  - **Today:** it is `BuiltinComponents`' order followed by the validator's test components. It reads `"PrefabLink", "MeshRenderer", ..., "Environment", "PostProcess", ..., "ColliderSurface", "AudioSource", ..., "FixedJoint", "ValidatorTarget", "ValidatorFollower"`.
  - **After phase 8**, with every new name before `ValidatorTarget`:
    - `"Static"` after `"PrefabLink"` (M43);
    - `"MassProperties"` after `"ColliderSurface"` (M44);
    - `"Sky"` after `"Environment"` (M45);
    - after `"FixedJoint"`: `"WheeledVehicle"`, `"VehicleWheel"` (M44), `"RenderDistance"`, `"Wind"` (M46), `"Water"`, `"Buoyancy"` (M47), then `"ValidatorTarget"`, `"ValidatorFollower"`.
  - **`COMPONENT_VERSIONS`** is unchanged, since every new component is version 1.
- **The fixture blocks of `validation_fixtures()`** (`:532-1065`; the last today is `item = fixture("RENDER_REFLECTION_INVALID")` at `:1061`, before `return fixtures` at `:1065`). Ten blocks are appended, in this order:
  - `SCENE_STATIC_MOVES` (M43);
  - `PHYSICS_MASS_UNUSED`, `PHYSICS_VEHICLE_INVALID` (M44);
  - `SKY_LIGHT_INVALID`, `SKY_ENVIRONMENT_IGNORED`, `PROBE_NOT_RELIGHTABLE` (M45);
  - `ASSET_LOD_INVALID` (M46, with a glTF from `MakeGltfFixtures.py`);
  - `WATER_INVALID`, `WATER_WAVES_TOO_STEEP`, `PHYSICS_BUOYANCY_INVALID` (M47).
- **Each block also adds:**
  - its directory `Tests/Data/Validation/<CODE>/`;
  - its line in the generated `Tests/Data/Validation/README.md`;
  - its row in the game-debugging skill's `validation-codes` page;
  - its §13.7 row.
- **The generator's module comment** (`:33`) names the codes whose fixtures reference meshes or glTF files. M46's code joins that list.

### Render passes, pipelines and shaders

#### Pass declarations (`Renderer/RenderPassDeclaration.h:167-265`)

- **Today:** 13 stages and 34 declarations.
  - 77 startup pipelines: `RenderPassDeclarationTests.cpp:252`, and `SceneRendererTests.cpp:475`, `:483`, `:484` and `:495`.
  - 24 mesh pipelines (`RenderPassDeclarationTests.cpp:245`).
  - 41 stats rows.
  - 34 programs, sorted, in `Resources/Shaders/Shaders.json`.
- **Phase 8 adds no stage.** Its declarations go in these places:

| Stage | Today | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| Ambient | DepthPyramid, Gtao, LightClusters (`:189`) | M45: `SkyLighting` after `LightClusters`, with rows `SkyLuts`, `SkyIbl` and `ProbeRelight` and 10 pipelines (the Sky program's six computes and `EnvironmentBake`'s four, the same SPIR-V) | DepthPyramid, Gtao, LightClusters, SkyLighting |
| Forward | PlanarReflections, BrdfLut, ForwardOpaque, TerrainForward, Skybox (`:204`), Ssr, ForwardTransparent, Overdraw | M45: `Sky` directly after `Skybox` (row `Sky`, 1 pipeline, target `SceneColor`).<br>M47: `Water` after `Sky` and before `Ssr` (rows `WaterRefraction` and `Water`, target `WaterRefraction`, 2 pipelines: the surface, and the surface with `EntityId`). | PlanarReflections, BrdfLut, ForwardOpaque, TerrainForward, Skybox, Sky, Water, Ssr, ForwardTransparent, Overdraw |
| HdrPost | Taa (`:239`, "first in HdrPost"), Bloom, Exposure | M47: `Underwater` first, before `Taa` (row `Underwater`, 1 pipeline, program `Water`) | Underwater, Taa, Bloom, Exposure |
| All other stages | unchanged | none | unchanged |
| **Totals** | 34 declarations, 41 rows, 77 pipelines, 34 programs | M45 +2, +4, +11, +1 (`Sky`). M47 +2, +3, +3, +1 (`Water`). | **38 declarations, 48 rows, 91 pipelines, 36 programs,** with `MeshPipelineCount` still 24. The declaration header's pipeline comment (`:151-166`) names each addition. |

- **The stats rows after phase 8, in order (48):**
  - Prepare, Skinning, DirectionalShadows, SpotShadows, PointShadows;
  - DepthNormal, TerrainDepthNormal, DepthPyramid, GTAO, GTAODenoiseHorizontal, GTAODenoiseVertical, LightClusters, **SkyLuts, SkyIbl, ProbeRelight**;
  - PlanarReflection0 to PlanarReflection3, ForwardOpaque, TerrainForward, Skybox, **Sky, WaterRefraction, Water**, SSR, ForwardTransparent, Overdraw;
  - Sprites, Lights2D, Particles, **Underwater**, TAA, Bloom, Exposure;
  - Tonemap, FXAA, Text, Ui, Upscale;
  - SelectionMask, SelectionDilateHorizontal, SelectionDilateVertical, SelectionComposite, Wireframe, Overlays, Labels, Letterbox.
- **Programs** (`Shaders.json`, sorted): `Sky` goes between `Skinning` and `Skybox`, and `Water` after `Upscale`. 34 become 36.
- **Recording:**
  - M45: `RecordAmbientStage` ends with `RecordSkyLighting` after `RecordLightClusters` (`SceneRenderer.cpp:2325`). `RecordForwardStage`'s sky block (`:2447-2460`) draws the Sky in the Skybox's place.
  - M47: its Forward call follows M45's sky block. The depth pyramid's gate (`:2306`) becomes `frame.Ao || frame.Ssr || frame.Water`. Its HdrPost call precedes `RecordTaa` (`:3199`).

#### Binding layouts (set 0 per layout; the registers in `Renderer/Private/ForwardPipelines.h:61-112`)

`PipelineFactory`'s reflection check refuses a layout item that no entry point of the program declares (`Graphics/PipelineFactory.cpp:647-652`). So every program that shares a layout declares each item of that layout, those it does not read included (ADR 0042's `Terrain.slang` row).

| Layout | Today | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| Forward view (`ForwardPipelines.cpp:66-102`): the Scene program's forward variants, Terrain, and M47's Water set 0 | b0 to b5; b6 push constants (`DrawConstantsSlot`, `:61`); b7 `PlanarReflectionConstants`; t0 to t3, t5, t6 and t8 to t20; s0 to s2. t4 and t7 are free. | M43: t21 `StaticInstances` and t22 `InstanceIndices`, after t20's item.<br>M46: b8 `WindConstants`, after M43's items.<br>M45: none (t3 takes the sky's cube, and b2 and b5 keep their layouts).<br>M47: none (its set 0 is a variant of this layout's set, with `SceneNormals` and the AO swapped for neutral textures). | b0 to b8, t0 to t3, t5, t6, t8 to t22, s0 to s2. t4 and t7 are still free. |
| Prepass view (`:43-54`) | b0, b6 push constants, t5, t10, t16, s0, s2 | M43: t21 and t22. M46: b8. | b0, b6, b8, t5, t10, t16, t21, t22, s0, s2 |
| Overdraw view (`:56-64`) | b0, b6 push constants, t10, s2 | M43: t21 and t22. M46: b8. | b0, b6, b8, t10, t21, t22, s2 |
| Shadow (`Renderer/ShadowPass.cpp:32-66`) | Opaque: b0 `ShadowCasterConstants`, t10, b6 push constants. Mask adds t16 and s2; set 1 is the material (pixel stage). | M43: t21 and t22 in both variants. M46: b8 in both variants. | b0, b6, b8, t10, t21, t22, plus t16 and s2 for Mask |
| SelectionMask (`Renderer/SelectionPass.cpp:50-80`; 6 of the Selection declaration's 8 pipelines, `SelectionPass.h:69`) | push constants at slot 0, t0, t10 | M43: none (a selected static item's record is copied into the frame's `Instances`). M46: b8. | slot 0 push constants, b8, t0, t10 |
| Programs that declare items unread | `Terrain.slang` declares t10, t16, b7 and t17 to t20 (`:65-80`) | `Terrain.slang`: M43's t21 and t22, then M46's b8.<br>M47's `Water.slang`: every forward set-0 item it does not read, M43's and M46's included. Its variant set binds M43's static buffers and M46's `WindConstants` as the transparent set binds the forward set's (`SceneRenderer.cpp:2468-2478`). | — |
| Own layouts | — | M45: the Sky program's (`Shared/SkyConstants.h`, the LUTs; `SkyResults` as a UAV). M47: Water's set 1, its registers fixed by the render stream (`Shared/WaterConstants.h` as a structured buffer, `WaterRefraction`, `ViewDepth`, the normal and foam maps, the planar slot). | — |
| Mesh vertex bindings (`ForwardPipelines.h:133`, `:136`) | 0 `MeshVertex`, 1 `MeshAttributeStreamSlot`, 2 `PreviousPositionSlot` | M46: 3, the wind stream (or the cache's shared default stream of zeros), in all 36 mesh pipelines: Scene 24, Shadow 6 and SelectionMask 6. | 0 to 3 |

- **Binding sets.** The sets built on these layouts follow the same order, in train order:
  - the forward, prepass and overdraw sets (`SceneRenderer.cpp:2217`, `:2234`, `:2389`);
  - the mirror view's set (`SceneRendererReflections.cpp:370`);
  - the Shadow sets and the SelectionMask sets.

#### Shared shader structs and flags

| Struct or list | Today | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| `ViewConstants` (`Resources/Shaders/Shared/ViewConstants.h:82`) | 512 bytes | none | 512 |
| `DrawConstants` (`Shared/DrawConstants.h:69-77`) | 16 bytes: `FirstInstance`, `Padding0`, `Padding1` (UInt2) | M43: `Padding0` becomes `Flags`, with the push-constant bit `DrawConstantsStaticInstances` = 1 | 16 bytes. `Padding1` is free, and the next `Flags` bit is 2. |
| Draw flags (`InstanceRecord::Flags`; `DrawConstants.h:29-39`) | 1 Mirrored, 2 ReceiveShadows, 4 PreviousPositions, 8 MaterialParameters, 16 PlanarReflection | M46: `DrawFlagWind` = 32. No other milestone takes one. | 1 to 32, and the next is 64. |
| `InstanceRecord` (`Shared/InstanceRecord.h:37-60`) | 208 bytes; `Padding1` and `Padding2` free (`:55-56`) | M46: `Padding1` becomes `LodFade` (signed, 0 for none), and `Padding2` becomes `WindResponse` (4 × unorm8) | 208 bytes, with no free word left. |
| `InstanceIndex` (new, in `Shared/InstanceRecord.h`) | — | M43: `{uint32_t Record; uint32_t Reserved}`, 8 bytes. `Reserved` is 0 and unread at M43.<br>M46 (W46.4): `Reserved` holds a static entry's signed fade, returned by M43's load function in `Common/Instances.slang`. | 8 bytes, with no free word left. |
| New shared headers | — | M45 `Shared/SkyConstants.h`; M46 `Shared/WindConstants.h`; M47 `Shared/WaterConstants.h` | `ShadersTests.cpp:811` ("Shaders: SharedStructsMatchReflection") gains `InstanceIndex`, `SkyConstants`, `WindConstants` and `WaterConstants`, in that order. |
| Existing constant buffers | `EnvironmentConstants` 192 (`EnvironmentConstants.h:42`), `FogConstants` 128 (`FogConstants.h:56`), `ProbeConstants` 2448 (`ProbeConstants.h:74`) | M45 copies into them on the GPU, with no layout change | unchanged |
| New common shader files | — | M43 `Common/Instances.slang`; M45 `Common/Atmosphere.slang`; M46 `Common/Wind.slang` and `Common/LodFade.slang`; M47 `Common/Waves.slang` | — |

#### Snapshot (`Renderer/RenderSnapshot.h`, frozen; M43 owns it for the phase)

| Item | Today | Phase 8, in train order | After phase 8 |
|---|---|---|---|
| `RenderSnapshot` members | the last is `PlanarReflections` (`:877`) | `StaticSet` (M43, `Ref<const RenderStaticSet>`), then `Sky` (M45), then `Wind` (M46), then `Waters` (M47) | ..., `PlanarReflections`, `StaticSet`, `Sky`, `Wind`, `Waters` |
| Types before `RenderSnapshot` | — | `RenderSky` (M45), then `RenderWind` (M46), then `RenderWaterShape`, `MaxWaterWaves` and `WaterItem` (M47). `RenderStaticSet` and `RenderStaticCell` live in M43's own header. | — |
| `MeshDrawItem` | as today | M46: `Distance` appended. M43's static cells hold `MeshDrawItem`s, and M46's W46.4 fills it there. | +1 member |
| Free functions | `ComputeReverseZProjection` (`:920-921`) | M45: its two declarations (`ApplySkyLightDriving` among them) beside it | — |
| `SnapshotHeaders` (`Scripts/ModuleRules.json`) | 3 files | M43: `Engine/Source/Engine/Renderer/RenderStaticSet.h` | 4 |
| Pick ids | dense in snapshot order (`RenderSnapshot.h:342`, `:834`) | M43: a static record's id is `StaticPickIdBase` (2^31) plus its cell's slot and index. Dynamic items, terrains and M47's waters keep the dense range, in that order: the waters' ids follow the terrains'. | — |

### Execution order

**PlaySessionPhase is unchanged.**
- The fixed step: InterpolationSnapshot, ApplyInput, TextLatch, UiInput, StartFlush, UiEvents, FixedUpdate, Tasks, NativeStep, SpriteAnimation, AnimationSystem, PreStepTransformUpdate, PhysicsPreStep, PhysicsStep, PhysicsPostStep, Physics2DPreStep, Physics2DStep, Physics2DPostStep, DestroyFlush, PostStepTransformUpdate and UiLayout.
- The frame phase: LatchFrame, Update, LateUpdate, FrameDestroyFlush, FrameTransformUpdate, FrameUiLayout, AudioUpdate, Particles and RenderExtraction.
- Every phase 8 system is the world's, so the world pause holds it.

**PhysicsPreStep's items** (`Scene/PhysicsSystem.h:31-77`; `Scene/PhysicsSystem.cpp:1473-1615`):
1. Rebuilds (items 1 and 2), M36's surfaces and M37's joints (`:1482-1493`). M44's `SynchronizeVehicles` follows `SynchronizeJoints`, here and in `Rebuild` and `SynchronizeBodies`.
2. Items 3 and 4: the kinematic moves and the teleports (`:1495-1578`). M43 drives this owner loop from its lists.
3. `WakeOwners` (`:1579`).
4. **4b (M44):** the vehicles' input, in canonical order.
5. **4c (M47):** buoyancy, over the `Buoyancy` view in canonical order.
6. **Item 5:** the characters (`:1581-1615`). M48's change is inside it (`:1608-1614`: the update call with the mode, and the consumed state), after 4b and 4c.

**PhysicsStep:** vehicles run in Jolt's step-listener jobs.

**PhysicsPostStep:**
1. The joint breaks (`:1700`).
2. The write-back (`:1702-1720`, which M43's lists drive).
3. **M44's wheel poses.**
4. The §9.4 dispatch (from `:1722`).

**Rules for every pass a later lander adds:**
- It iterates in canonical order through M43's `CollectCanonical` once M43 lands.
- Until then, on the milestone branch, it iterates `Scene::GetCanonicalOrder()`.
- It adds its visits to M43's `EntitiesVisited` counter.

### Tooling and documents

| Item | Today (`3aaf4b10`) | Phase 8, in train order |
|---|---|---|
| Lint rules (`Scripts/Lint.py`; `Tests/Data/Lint/Fixtures.json`, 24 fixtures) | as today | M43: `banned-transform-write`, with `Banned.TransformWriteFiles` (`TransformSystem.cpp`, `PhysicsSystem.cpp`, `Physics2DSystem.cpp`) and the fixture `TransformDirectWrite`, making 25 fixtures. Every later transform writer goes through `Entity::Patch`, or through one of the three files with `Scene::MarkTransformWritten`. M44's wheel poses follow that rule. A new file on the list needs a ledger amendment. |
| `Scripts/ModuleRules.json` | `SnapshotHeaders`: 3 files. `Banned`: 7 lists. `ThirdParty`: 24 libraries. | M43: `SnapshotHeaders` +`RenderStaticSet.h`; `Banned.TransformWriteFiles`, the eighth list.<br>M46: `ThirdParty.meshoptimizer`, private to AssetPipeline and restricted to `AssetPipeline/Private/MeshSimplifier.cpp`. |
| Premake, `Dependencies.lua`, `CheckBuildConfig.py` | `PRECISE_VENDOR_PROJECTS` holds 9 (`Scripts/CheckBuildConfig.py:211-213`) | M46 alone: `Vendor/meshoptimizer/premake5.lua`, the `premake5.lua` include, `UseMeshOptimizer()` (after `UseDetour`) called by `Engine/premake5.lua`, `PRECISE_VENDOR_PROJECTS` +`"meshoptimizer"`, the fixture `Tests/Data/BuildConfig/FastMathMeshOptimizer` with its pattern in `Scripts/Lib/scripts.py`, and the notices (Shipped "yes"). The lead adds the `0001-approvals.md` row at the setup. |
| Architecture new sections | §9 ends with §9.10 Joints (`Docs/Architecture.md:1411`) | **M44: §9.11 "Vehicles". M47: §9.12 "Water and buoyancy".** No other draft claims a new section. M45's "Sky", M46's "Mesh levels" and "Wind", and M47's "Water" are paragraphs of §8.3. |
| Architecture shared lists | — | Each milestone edits its own rows, in train order: §5.3 (the rows in `BuiltinComponents` order), §5.7, §8.2 (the members, in the snapshot's order), §8.3 (the rows), §8.4 (the layouts above), §8.14, §13.5 (the heading "Method list (126)" becomes 128), §13.7 (the codes in table order, the stats fields), §15.5 (the scenes). M43 alone amends §5.1, §5.2 and §2.3; M45 §8.6; M46 §2.2, §3, §6.5, §7.4 and §8.10; M48 §9.6. §6.8 and §8.7 are amended by M45 (the irradiance volume's format 2 and the sky-relit probes) and then M46 (cooked mesh 3 and the shadow casters' level), each its own sentences, in train order. |
| Skills | `luau-gameplay/SKILL.md` is 46,171 bytes of its 49,152-byte `DocExamples` limit | luau-gameplay: M44 adds a `vehicles` page and M47 a `water` page, each with one index line in `SKILL.md`; M48 edits `characters-and-input.md` (its line 8 and the idioms).<br>game-building: `look.md` (M43, M45, M46, M47, in that order) and `prefabs-and-levels.md` (M43, M44).<br>game-debugging: `SKILL.md` (all six) and `validation-codes.md` (M43 to M47).<br>golden-images: `SKILL.md`'s scene list (M45, M46, M47).<br>add-component: M43 (never write a `TransformComponent` directly). |
| Generated references (`Docs/Reference/*`, `Engine.d.luau`, `Tools/MCP/catalog.json`) | — | Regenerated by each lander on its merged tree, never merged as files. |

### Contended files

The shared integration files (`premake5.lua`, `Dependencies.lua`, `ModuleRules.json`, `BuiltinComponents.h`, `RegisterBindings.cpp`, `RegisterMethods.cpp`, `Docs/Reference/*`) follow ADR 0028 decision 11's append rule in train order. In a row that names several milestones, each has its region. The later lander keeps the earlier one's hunks, and runs the suites that read the file on its merged tree.

| File | Owner in phase 8 | Regions, in train order (line numbers at `3aaf4b10`) |
|---|---|---|
| `Scene/TransformSystem.{h,cpp}`, `Scene/Scene.{h,cpp}`, `Session/PlaySession.{h,cpp}`, `Scene/Physics2DSystem.cpp`, `AudioSystem.cpp`, `UiLayout.cpp`, `AnimationSystem.cpp`, `ParticleSystem.cpp`, `SpriteAnimationSystem.cpp`, `BoneAttachments.cpp`, `ScriptEngineState.cpp`, `ScriptEngine.cpp`, `ScriptInstances.cpp`, `Scripting/Bindings/Scene.cpp`, `Renderer/EditorOverlay.cpp` (the wireframe, `:338`), `Testing/FeatureTestRunner.cpp` (`GetExtractedPosition`, `:521`), `Scene/Registration/CoreRegistration.cpp` | M43 alone | M43's own regions (ADR 0061 decision 8). M50 owns `Scene` in phase 9. |
| `Scene/RenderExtraction.{h,cpp}` | M43 | **M43:** the canonical loop (`:754-945`) becomes `CollectCanonical` over the drawn and lit components; `FindPrimaryCamera` (`:609-623`).<br>**M45:** `SkyExtraction`'s call after the loop, beside `snapshot.Fog` (`:947-949`).<br>**M46:** `MeshDrawItem::Distance` on each dynamic mesh item (the MeshRenderer branch, `:769`), and the Wind by a component view after the loop, after M45's call.<br>**M47:** `WaterComponent` appended to M43's drawn components after the terrains, so its PickIds follow the terrains'; M42's PlanarReflection branch (`:923-937`) for a Water.<br>Each adds a header paragraph. |
| `Renderer/RenderSnapshot.h` (frozen) | M43 | As "Snapshot" above. |
| `Renderer/SceneRenderer.cpp` | M43 (Prepare) | **M43:** `PrepareViewDraws` (`:899-1093`), the casters and the selection (`:1816-1862`), the pick table (`:1426`), the instance upload (`:1539-1565`), and t21 and t22 in the three sets (`:2217`, `:2234`, `:2389`).<br>**M45:** the cascade order (`:1691-1730`), the environment block (`:1864-1873`), the probes (`:1874-1877`), `RecordAmbientStage`'s end (`:2325`), the Forward sky block (`:2447-2460`), and the set creation (`:137-140`, `:608-632`).<br>**M46:** inside `PrepareViewDraws` (the distance cull, the level, the record's words), the casters' list (`:1823-1842`), b8 in the three sets after M43's items, and the counters; and (amended 2026-11-24, M46's W46.2 review) the mesh passes' vertex bindings (slots 2 and 3), the per-render `WindConstants` write and the wind frames.<br>**M47:** `PrepareWater` beside `PreparePlanarReflections` (`:1912`), the pyramid gate (`:2306`), the Forward call after the sky, and the HdrPost call before `RecordTaa` (`:3199`). |
| `Renderer/SceneRendererReflections.cpp` | M43 | **M43:** t21 and t22 in the mirror set (`:370`).<br>**M45:** the slot's fog (`:251`) and the mirror sky (`:440-449`).<br>**M46:** b8 in the mirror set, after M43's items.<br>**M47:** the candidate loop (`:113-146`) and the water's slot. |
| `Renderer/Private/SceneRendererState.h` | M43 | Appended in train order: M43's per-view `InstanceIndices` buffer and static state; M45's sky members beside `DrawsSkybox` (`:404`, `:428-429`) and `SkyViewResources`; M46's current and previous `WindConstants`, and (amended 2026-11-24) its draw, material and frame members for the levels and the wind stream; M47's per-view water state. |
| `Renderer/SceneRenderer.h` | M45 | `GetSceneDepthTexture` beside `:427`. Amended 2026-11-24: M46 appends its three counters to `SceneRenderStats` (M46's W46.2 review). |
| `Renderer/ShadowPass.{h,cpp}` | M43 | **M43:** the layout (`:32-66`, t21 and t22); `PrepareCasters` (`:369-501`); the faces' cull and record (`:677-830`).<br>**M46:** the layout (b8 and binding 3); the level and the signature in `PrepareCasters`, which gains the view's camera; at W46.4, the static casters' level and keys, and the wind phases in a face's M29 signature. |
| `Renderer/SelectionPass.{h,cpp}`, `Passes/Selection.slang` | M43 | **M43:** a selected static item's record copied into `Instances` (`Selection.slang` unchanged).<br>**M46:** the layout (b8, binding 3), the displacement and the level. |
| `Renderer/Private/ForwardPipelines.{h,cpp}` | M43 | **M43:** the registers t21 and t22, in the prepass (`:43-54`), overdraw (`:56-64`) and forward (`:66-102`) layouts.<br>**M46:** `WindConstantsRegister` 8 in each layout after M43's items, and the wind stream's slot 3 beside `:133` and `:136`. |
| `Renderer/GpuResourceCache.{h,cpp}` | M43 | **M43:** the static record buffer.<br>**M45:** the volume's relight block (`:132-139`).<br>**M46:** `GpuMesh`'s levels and wind stream, the shared default wind stream (beside `:46-49`), and `GpuMaterial`'s wind block. |
| `Renderer/RenderPrepare.{h,cpp}` (keys frozen by M8 and M23) | M46 alone | `Lod` in `OpaqueSortKey` and `MeshInstanceKey`; `SelectMeshAlphaMode`. |
| M43's static grouping key (`Renderer/`, in M43's W43.2 record) | M43 | **M43:** (pipeline, material, mesh, submesh), as a struct.<br>**M46 (W46.4):** appends `Lod`, making (pipeline, material, mesh, submesh, Lod), and the same for the static casters' per-face keys. |
| `Renderer/TemporalHistory.{h,cpp}` | M47 alone | The water's `PreviousWorld` and `PreviousPhases`, by entity. |
| `Renderer/ProbeCapture.{h,cpp}` (frozen header) | M45 | **M43:** captures keep the static set (`MakeCaptureSnapshot`, `:282-319`).<br>**M45:** `MakeRelightCaptureSnapshot` after it, and the visibility read-back.<br>**M47:** clears `Waters` in `MakeCaptureSnapshot`, after the planar clear (`:317`). |
| `Renderer/RenderPassDeclaration.h` (frozen) | M45 | **M45:** `SkyLighting` (after `:189`), `Sky` (after `:204`), and the comment.<br>**M47:** `Water` after `Sky`, `Underwater` before `Taa` (`:239`), and the comment. |
| `Renderer/RenderStage.h`, `Renderer/Fog.{h,cpp}`, `Renderer/Private/LightingInputs.{h,cpp}`, `Renderer/Private/ProbeShading.{h,cpp}`, `SkyboxPass` (its comment) | M45 alone | — |
| `Resources/Shaders/Passes/Scene.slang` | M43 | **M43:** the declarations (`:100-121`), `GetInstanceIndex` (`:410-419`), and the record reads (`:302`, `:361`, `:503`, `:642`) through `Common/Instances.slang`.<br>**M46:** `TransformVertex`, `GetPreviousClip` and `VSOverdraw` (`:423-491`), `GetCoverage` (`:497`), the b8 declaration and the binding-3 input. M46 never edits `GetInstanceIndex`. Amended 2026-11-24 (M46's W46.2 review): M46 also adds the Blend fade in `EvaluateSurface` and moves `GetCoverage`'s record read; at W46.4 both read the instance record through M43's load function, so a static Blend item in a fade band reads its fade from `InstanceIndex::Reserved`. |
| `Passes/Shadow.slang` | M43 | **M43:** `:26`, `:62` and `:93` through `Common/Instances.slang`.<br>**M46:** the displacement in `VSMain` (`:59-79`), after M43's record load at `:62`. |
| `Passes/Terrain.slang` | append | M43's t21 and t22, then M46's b8, each declared unread. |
| `Passes/Ssr.slang` | M47 alone | The composite's water flag in word 1's y byte. |
| `Shared/DrawConstants.h`, `Shared/InstanceRecord.h` | M43, then M46 | As "Shared shader structs" above. |
| `Resources/Shaders/Shaders.json` | append, sorted | M45 `Sky`, M47 `Water`. |
| `Scene/RenderAnnotations.cpp` | **M45** | **M43:** `EvaluateRenderSceneValidation`'s mesh loop (`:175-193`) also visits the static set's items, so `RENDER_NO_LIGHTING` sees static meshes.<br>**M44:** one call, `AppendVehicleDebugDraw`, after the joints' (`:135`), in the Colliders block (`:129-136`).<br>**M45:** the illumination block (`:160-173`): a Sky of positive `Intensity` illuminates.<br>**M47:** the Rectangle-volume outline, a hunk of its own in `AppendRenderAnnotations` that lands after M45's region in train order. It is in M47's scope; the W47.3 record names its place (in the bounds block `:99-105`, or after the navigation block `:137-140`). |
| `Scene/PhysicsSystem.cpp` | **M44** | **M43 (lands first):** `HasHierarchyChanged` and `RecordHierarchy`, on the structure revision (`:567-588`); `FindShapeChanges` (`:949-997`); PreStep's owner loop (`:1497`); PostStep's write-back loop (`:1702-1720`, the characters' part `:1715-1719` included); `AppendStateHash`'s order (`:2529-2571`); and `MarkTransformWritten` in `:269-294` and `:2365`; and (amended 2026-11-24, M43's W43.1 review) the owner lists' and world-change list's lines in `AssignPlan`, `RemoveBody` (`OwnerListsStale`), `CreateCharacter`, `RemoveCharacter`, `Create`, PreStep's head and the destructor (`CloseWorldChangeList`), which M44 keeps where it changes `RemoveBody` and the destructor.<br>**M44:** `MakeBodySettings` and `SameBodySettings` (`:297-341`); the signals (`:505-545`); `RemoveBody` (`:1166`), `Rebuild`'s end (`:1382`) and the destructor (`:1414-1419`); `SynchronizeVehicles` after `:1493`; item 4b after `:1579`; `SynchronizeBodies` (`:1637`, `:1646`); the wheel poses between `:1720` and `:1722`; `FlushDestroyed` (`:2145`); `GetBodyInfo` (`:2433-2507`); the vehicle part after `:2569`, and the early return (`:2533`). M44 merges over M43's regions, and its vehicle composition keys on M43's structure revision.<br>**M47:** item 4c's call after M44's 4b, the logic in `Scene/PhysicsSystemBuoyancy.cpp`; `GetBodyInfo`'s buoyancy.<br>**M48:** inside item 5, `:1608-1614`, and after its loop the sweep that drops the pending moves of characters that no longer exist (amended 2026-11-24 for M48's fix of its W48.1 review: a tick's move survives a field write that creates the character again). |
| `Scene/PhysicsSystem.h` (frozen) | M44 | The item list (M44's 4b, M47's 4c, and M48's free move in item 5 at `:72`); `PhysicsBodyReport`, with M44's `CenterOfMass` and then M47's `Buoyancy` after `:425`; M44's vehicles section after the joints' (`:582-588`), and M47's buoyancy section after it; M48's `PhysicsCharacterState::LastMove` (`:368-382`) and `MoveCharacter`'s mode (`:513-518`); the frozen paragraph (`:162-166`). |
| `Scene/Private/PhysicsSystemState.h` | M44 | M43's owner lists; M44's vehicles section after the joints' (`:409-446`); M47's per-body planes after M44's; M48's `PhysicsCharacterMove` and `State::PendingMoves`, beside `Characters` (amended 2026-11-24: they replace the entry's `DesiredVelocity` and the planned `DesiredMode`, so a move survives the character's re-creation in the same tick; `Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md`, "The W48.1 review's fixes"). |
| `Scene/PhysicsSystemVehicles.cpp`, `Scene/PhysicsVehicles.{h,cpp}`, `Scene/PhysicsMassValidation.{h,cpp}`, `Scene/VehicleDebugDraw.{h,cpp}`, `Physics/PhysicsVehicle.h`, `Physics/PhysicsWorldVehicles.cpp`, `Scripting/Bindings/Vehicles.cpp` | M44, new files | — |
| `Physics/PhysicsWorld.{h,cpp}` (M11's frozen header) | M44 | **M44:** `BodyDescription::MassOverride` (after `:132`); the vehicles section after the joints' (`:316-348`); `GetCenterOfMassPosition`; the destructor (`.cpp:321-322`); `CreateBody`; `DestroyBody`; `SetShape`.<br>**M47:** `ApplyBuoyancy` after M44's section, its work in `Physics/PhysicsWorldBuoyancy.cpp`. |
| `Physics/Private/PhysicsWorldState.h` | M44 | M44's vehicles section (after `:84-92` and `:106-112`), then M47's. |
| `Physics/Private/PhysicsContactListener.{h,cpp}`, `Physics/PhysicsWorldQueries.cpp` (`GetSubShapeBounds`, `:457-492`), `Physics/Private/PhysicsMassProperties.{h,cpp}`, `Physics/PhysicsShape.{h,cpp}`, `Physics/PhysicsTypes.h`, `Scene/PhysicsValidation.cpp` (`:104-112`), `Scene/ColliderDebugDraw.cpp` (`:153`) | M44 alone | M54 extends `ResolveGroundFriction` in phase 10. |
| `Physics/CharacterController.{h,cpp}` (M11's frozen header), `Physics/Private/CharacterContactRecorder.{h,cpp}`, `Scene/PhysicsSystemCharacters.cpp` | M48 alone | M55 owns `PhysicsSystemCharacters.cpp` in phase 10. |
| `Automation/Methods/PhysicsMethods.{h,cpp}` (frozen wire format), `PhysicsMethodsTests.cpp`, `Tests/Automation/test_physics.py` | M44 | **M44:** `physics.vehicleInfo` and its types; `centerOfMass`.<br>**M47:** `buoyancy`.<br>**M48:** `lastMove`.<br>All go before `tick`. |
| `Scene/Registration/PhysicsRegistration.cpp` | M44 | **M44:** `MassProperties` after `ColliderSurface`.<br>**M48:** the description of `CharacterController.Mass` (`:133-136`). |
| `Scene/Registration/RenderingRegistration.cpp` | M45 alone | `Sky` after `Environment`; the description of `ClearMode::Skybox`. M46 adds nothing to Rendering. |
| `Scene/Components/BuiltinComponents.h`, `BuiltinComponents.cpp`, `BuiltinComponentsTests.cpp` | append | As "Registries" above. |
| `Scripting/Bindings/Components.cpp` | M48 alone | `Utils::CharacterMove` (`:242-253`), and the `CharacterController` type (`:532-550`). |
| `Scripting/RegisterBindings.cpp`, `Scripting/Private/BindingRegistration.h` | append | M44, then M46, then M47. |
| `Scripting/ScriptPerformanceStats.h`, `Session/SessionStatistics.cpp`, `Scripting/Bindings/Application.cpp`, `Renderer/RenderStats.{h,cpp}`, `Automation/Methods/StatsMethods.{h,cpp}`, `Editor/Panels/StatsPanel.cpp`, `StatsMethodsTests.cpp`, `StatsPanelTests.cpp` | append | M43, then M45, then M46, then M47. |
| `Automation/Methods/RegisterSharedMethods.cpp`, `Automation/Methods/WaterMethods.{h,cpp}` | M47 alone | — |
| `Tests/Source/EditorCore/Automation/SceneTargetTests.cpp` | append | M44's row, then M47's. |
| `EditorCore/Project/ProjectValidator.cpp`, `ProjectValidatorTests.cpp` (`:1386-1388`, `:1450-1454`), `MakeValidationFixtures.py`, `Tests/Data/Validation/`, the `validation-codes` page | append | As "Registries" and "Validation fixtures" above. |
| `ProjectValidator.cpp:238`, `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s description (the scene singletons it names, today Environment and PostProcess) | append | M45 adds `Sky`, then M46 `Wind`, in train order (M47's second Ocean is `WATER_INVALID`'s, not this code's). |
| `Scene/StaticScenery.cpp` (new, M43's: the components that keep an entity out of the static set) | M43 | M44 appends `VehicleWheel` to its list at its merge. |
| `Asset/AssetDiagnostic.{h,cpp}` | M46 alone | `AssetLodInvalidCode`, and `GetAssetDiagnosticCodes()`, which grows from 13 to 14. |
| `Asset/BuiltinAssets.h`, `Asset/BuiltinTextures.{h,cpp}`, `Resources/EngineAssets.json` | M47 alone | — |
| `Asset/IrradianceVolumeData.{h,cpp}`, `EditorCore/Lighting/LightingBake.{h,cpp}`, `EditorCore/Automation/LightingMethods.{h,cpp}`, `Scene/ProbeValidation.{h,cpp}` | M45 alone | — |
| `Asset/MeshData.{h,cpp}`, `Asset/MaterialData.{h,cpp}`, `AssetPipeline/Importers/GltfImporter.{h,cpp}`, `Tests/Data/Generate/MakeGltfFixtures.py`, `Tests/Data/Formats/Material/`, the premake and build files | M46 alone | — |
| `Tests/Data/Scenes/AllComponents.scene` | M47 alone | — |
| `ScriptBindingFuzzTests.cpp`, `ScriptProxyTests.cpp` | append | M47's water calls (`:297`), then M48's `MoveFree` (`:91`, `ScriptProxyTests.cpp:323`). Amended 2026-11-24 (M44's W44.3 review): `ScriptProxyTests.cpp`'s pending-entity read list takes each milestone's new reads, appended in train order: M44's 12 vehicle reads, M46's `Wind:Sample`, M47's four `Water` methods. |
| `RenderPassDeclarationTests.cpp`, `SceneRendererTests.cpp` | append | M45's counts, then M47's: 48 rows and 91 pipelines on the merged tree. |
| `Tests/Source/Engine/Graphics/ShadersTests.cpp` | append | M43, then M45, then M46, then M47. |
| `Scripts/Perf.py`, `Scripts/Lib/perf.py`, `Tests/Automation/test_perf.py` | append | M43, then M45, then M46, then M47. |
| `Scripts/Lint.py`, `Tests/Data/Lint/` | M43 alone | — |
| `Scripts/ModuleRules.json` | M43, then M46 | Distinct keys. |
| `Engine/Source/Engine/Core/BuildId.h` | M48 | 0.9.0. |
| FeatureTest `Scaffold/Scaffolds.json` and `Suites.jsonl` | append in train order | `Replays.jsonl` is never touched. |
| Architecture, the Roadmap, `Docs/Backlog.md`, the skills | merged by hand | As "Tooling and documents" above. The Backlog: §2 and §6 for every milestone; §8's render-thread row (M43); the row at line 96 narrowed (M47); the line-260 amendment (M45); §6's M34 row (M48). |
| `Scene/SceneSerializer.cpp`, `Scene/ComponentHostOps.h` | M43 alone | Amended 2026-11-24 (M43's W43.1 review): `AddOrOverwriteComponent` adds a component the entity lacks with its loaded value through the new host op `ComponentHostOps::AddCopy`, so value-derived state (`Static`'s tags) follows the loaded value on load, copy, undo and prefab sync; `ComponentHostOps::CollectCanonical` serves `Scene.FindAllWithComponent`. |
| `PhysicsComposition.h`, `PhysicsDiagnostics.h`, `PhysicsValidation.h`, `Physics2DWorld.h`, the pak format, `Exporter.cpp`, `Migrations`, `Scripting/Bindings/Physics.cpp`, `RegisterMethods.cpp` | none | No phase 8 milestone changes them. One that finds it must takes a ledger amendment on main. |

### The merge train, the landing and the gate

1. **The landing branch.** `landing/phase8` is cut from main, and M43 merges into it first.
2. **M44 and M45** follow, in that order. Each keeps the earlier hunks and appends after them.
3. **M46** merges `landing/phase8`, which by then holds M43, M44 and M45, into `milestone/m46`. W46.4 adapts on that tree, with its own review and filtered runs. The lead then merges `milestone/m46`.
4. **M47, then M48.** M47 adds, at its merge, the floating-body case with M44's offset centre of mass ("PhysicsWorld: a body with an offset centre of mass floats at its analytic draft", or its record's name). M47's `Water.slang` declares M43's and M46's forward-set items.
5. **M48's merge** sets `EngineVersionString` to 0.9.0 and regenerates FeatureTest with `Scaffold.py`, which re-records every replay.
6. **On the merged tree:**
   - the lead renders the six editor-UI goldens. The four new Stats panel rows may reach `EditorDesignSupporting1600x900`, as M28's two rows did in phase 5. A changed image is regenerated and reviewed with the golden-images skill, never merged as a file.
   - The demos' recorded upgrades (M46's materials) run again, followed by the provenance audit.
7. **Each lander:**
   - regenerates the references and FeatureTest on its merged tree;
   - runs the targeted suites its merge touched, with M44, M47 and M48 running the physics unit cases and the determinism suite.
8. **The gate.** The merge review comes first, then one `CI.py --require-clang-cl` on the merged tree (ADR 0060 decision 7). Main fast-forwards only after the gate is green. Main is then pushed, and the work pauses for the owner's go-ahead.
9. **Hardening** waits for the program's last phase. Phase 8 adds no fuzz target. It adds 12 FeatureTest scenes as soak subjects, and unit cases to the sharded ASan run (the setup's item 1).
10. **Machine limits:**
    - six milestones, one builder each, plus M47's two W47.2 stream sessions;
    - the machine-wide locks: one uncapped build or two capped ones, one GPU run and one gate at a time;
    - the game sessions in `D:\Engine` share the GPU lock while their rendering editors run.

    FeatureTest work uses `Scaffold.py`'s own editors, and the demos' upgrades use `Editor --upgrade`. No milestone touches `Projects/WorldTree` or `Projects/Skerries`.

## The owner's answers

The lead's draft put eight questions, each with a recommendation. The owner answered on 2026-11-24: questions 2 and 4 directly, phase 12 confirmed with the hardening after it, the briefs re-read first (S19 and S20 joined the plan, S05, S14, S16 and S18 gained sections), then "go with your recommendations" for the rest.

1. **The phase plan** (decision 2), including its departures from the README's order. **Accepted.** Phase 12 is confirmed ("I do want phase 12").
2. **The three new libraries** (decision 5): meshoptimizer, libunibreak and verblib. **Approved.**
3. **The hash moves** (decision 6):
   - RollingBall's three level replays re-recorded for Terrain 2;
   - the games' scenes re-hashed by `RideGround`, `Additive` and Terrain 2, all in phase 10;
   - FeatureTest's `Navigation.replay` re-recorded for tiles.

   **Accepted.**
4. **The hardening** (decision 7). **Once, after phase 12** ("the gate should run after phase 12 is done").
5. **Keep the render thread and GPU-driven rendering cut** (ADR 0028 decision 2), although S16 is the first measured CPU-bound frame. **Kept.** The cost is per-entity bookkeeping, which M43 removes, and CPU cell culling with GPU-resident instances is not GPU-driven rendering.
6. **The scope choices:**
   - sky-relit probes with no sun bounce;
   - a 2D cloud layer in M45;
   - migrated terrains cast no shadows until the game opts in;
   - World Tree's deferrals reopened: volumetric light (M64) and lit particles (M61);
   - wheeled vehicles only;
   - XInput rumble only;
   - no navmesh carving;
   - no impostors;
   - no run-time foliage edits.

   **Accepted, each.**
7. **S06's depth:** build the sub-scenes (M50) in phase 9 and streaming (M65) in phase 12 (the owner confirmed phase 12 on 2026-11-24), its design taking the slice's measured memory and activation times. On a slow disk, a fixed activation delay that the game chooses, with a short stall if the island is not ready, rather than a replay format that records the ready tick. **Accepted.**
8. **The smaller design questions** in the six research notes (`bin/Reviews/P8/research/*.md`, "Open questions"): take the recommendations as written. **Accepted.** Phase 8's milestone ADRs record each one they decide.

## Risks

- **M43 changes the transform rule.**
  - **A missed write path:** a transform write the change tracking misses gives stale matrices in every configuration alike, so the determinism suite cannot catch it. The Debug full-recompute check and the Lint rule cover it.
  - **A forgotten view:** a view that forgets the static set (a mirror, a probe capture, picking or selection) draws wrong. All 56 goldens must stay byte-identical.
- **Phase 8 is the program's heaviest.** It holds about 51 sessions, four renderer milestones, and two physics milestones that share `PhysicsSystem.cpp`. The ledger names each region's owner, as phase 5's did with four renderer milestones.
- **The GPU lock.** Four renderer builders in parallel queue on the one GPU, so each wave's GPU runs stay filtered.
- **GPU cost in the storm frame.** The volumetric fog, the rain, the water and S16's game view of 2.4 to 5.5 ms together must fit the frame. Perf budgets are warning only, so the game measures its Release export.
- **The new fuzzed readers.** `.foliage`, navmesh 2, heightfield 2, irradiance volume 2, cooked mesh 3 and `Input.json` each add fuzz time. The fuzz step re-sizes itself from its measured costs (ADR 0028 decision 16).
- **Vehicles in Jolt's worker jobs.** The 0, 1 and 8 thread-count test must prove they are deterministic. A vehicle that outlives its body leaves Jolt a dangling pointer, and the vehicle's lifetime rules prevent it.
- **The device-thread hand-off in M56.** A torn filter setting can become unstable, so every new audio node uses M40's one-setting-per-pull hand-off.

## Before program completion

- [ ] The setup (decision 3) on main: the ASan sharding, S20, this ADR, the Roadmap's phase 8 sections, the ledger and phase 8's milestone ADRs.
- [ ] Phases 8 to 11 each landed in one batch, gated, pushed, and their briefs' outcomes reported to the Skerries session through the product owner, with decision 9's corrections.
- [ ] Phase 12 landed (confirmed by the owner, 2026-11-24), and the program's one hardened gate run at its end (decision 7).
- [ ] `Docs/Backlog.md` §2 and §6 hold what each milestone left out or accepted, and the §6 row on one scene a session is closed by M50.
