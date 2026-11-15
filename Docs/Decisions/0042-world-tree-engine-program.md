# 0042 — The World Tree engine program (M28 to M33)

- **Status:** accepted. The product owner answered every point of the lead's draft on 2026-11-15 with "go with your recommendations" ("The owner's answers"). It is committed on main as a documentation-only change, with the Roadmap's M28 to M33 sections, `Docs/Backlog.md`'s §1 and §2 rows, the `0001-approvals.md` row and Architecture §18's rows. It schedules the seven engine tasks the World Tree showcase game asked for (`Projects/WorldTree/Design/EngineTasks/`, briefs 01 to 07, read at engine 0.5.0, `8d2e1469`). Lead-owned.
- **Date:** 2026-11-15.
- **Decided by:** the product owner, 2026-11-15; recorded by the lead.
- **Base:** main `26bb9565` (the backlog program closed by ADR 0038; Rolling Ball's level clock, session S6).
- **Authority:** Roadmap "Rules for every milestone"; ADR 0023; ADR 0028 decisions 10 to 12, 14, 16 and 17, whose process this program keeps (decision 6 below); Architecture §1.2, §3, §5, §8, §9, §11, §13 to §16.

## Context

- **The game.** World Tree is a showcase game another agent session is building in `Projects/WorldTree` (untracked, in `D:\Engine`'s working tree). Its engine tasks say what is missing, why the game needs it, the behaviour it needs and how the game will check it; the engine side owns the design, the API names and the process.
- **The briefs** (each verified by the lead against the code at `26bb9565`):

  | # | Brief | The game's priority | What the engine has today |
  |---|---|---|---|
  | 1 | Navigation | High: every enemy | No navigation mesh, path query, component, script API or method. `PhysicsComposition` already composes static bodies and colliders in canonical order, and `lighting.bake` is the bake pipeline to copy |
  | 2 | Fog | High: every realm's look | No fog anywhere in the renderer or shaders. World-space UI is drawn without fog by an accepted limit (Backlog §6) |
  | 3 | Save data | High, before the slice is finished | No script API for persistent data; `user://` holds the engine's own files; `FileSystem::WriteFileAtomic` and `Platform/PublishedFile` publish atomically but never flush (ADR 0003 decision 5) |
  | 4 | Display settings and performance readout | High: the options menu; horde mode in Dist | The `Application` namespace reads the window size only; fullscreen exists only at window creation; `stats.get` is automation-only and absent from Dist, and lacks visible and dropped lights, live particles, skinned meshes and active bodies |
  | 5 | Per-entity material parameters | Medium: the art pass | Only `MeshRenderer:SetMaterial`; the 144-byte instance record has 8 bytes of padding; no simulation time reaches render extraction |
  | 6 | Particle look | Medium: the fire realm | Square, unrotated, single-texture camera-facing quads, hard depth cut, colours capped at 1, no velocity for TAA (Backlog §6) |
  | 7 | Raise the light limits | High: the fire realm and horde mode | 256 lights per view (`MaxVisibleLights`, an 8-word cluster mask), 4 shadowed point lights (`MaxPointShadows`), 8 shadowed spot lights in a 4096 atlas that holds 16; importance recomputed each frame with no hysteresis |

- **The order the game needs:** briefs 1 and 4 first (enemies and the options menu), then 2, 5, 6 and 7 for the art pass, then 3 before the slice is finished. None blocks the game's start: it greyboxes meanwhile. Later realms will add briefs (physics joints, decals, audio effects); they are not in this program.

## Decisions

### 1. Scope

The seven briefs, each with the behaviour its brief lists, and the stretch items the owner keeps (owner's answer 2). Each brief's "Out of scope" stays out: crowds, local avoidance, dynamic obstacles, run-time rebakes and off-mesh links (navigation); ray-traced and virtual shadow maps (light limits; won't do under ADR 0028).

### 2. Structure: two phases of three parallel milestones

| Phase | # | Name | Briefs | Lands | Hardening | Size (agent sessions) |
|---|---|---|---|---|---|---|
| 5 | M28 | Game services | 4 (display settings, performance readout), 3 (save data) | readiness order with M29 | no: `CI.py --require-clang-cl` | ~11 (display ~4, readout ~2–3, save data ~5) |
| 5 | M29 | Light limits | 7 | readiness order with M28 | no: `CI.py --require-clang-cl` | ~6–8 with the shadow cache and shadow tiers |
| 5 | M30 | Navigation | 1 | last in phase 5 | yes: phase 5's `CI.py --milestone` (Scene, Session, a new fuzzed reader) | ~12 |
| 6 | M31 | Material parameters | 5 | any time | no: `CI.py --require-clang-cl` | ~3, ~5 with dissolve |
| 6 | M32 | Fog | 2 | before M33 | no: `CI.py --require-clang-cl` | ~4 (volumetric light +3–5 if kept) |
| 6 | M33 | Particle look | 6 | last in phase 6 | yes: phase 6's `CI.py --milestone` (renderer) | ~5–7 |

- **Why this grouping.** The explorers' maps put the contention where it decides the phases:
  - Display settings, the readout and save data all change the same host files (`Scripting/ScriptHost.h`, `Session/PlaySession.*`, `Runtime/RuntimeApp.*`, `EditorCore/Play/EditorPlayController.cpp`, the `Application` bindings), so they are one milestone in two waves, not two siblings.
  - Light limits touch mostly their own files (the cluster pass, the shadow maps and atlas, `ShadowConstants.h`, the validator's budget checks), so they run beside services and navigation. Their only overlap is `RenderPrepare` and `SceneRenderer.cpp` in other functions, and `StatsMethods`' light counters, which the phase 5 ledger gives one owner.
  - Navigation is the largest and touches Scene, Session and a new cooked format, so it lands last in phase 5 and takes the hardened gate.
  - Fog and particle look both change `Quad.slang`, `QuadBatcher` and the particle pass, the strongest conflict found. Fog lands first and owns the quad pipelines; particle look lands last, draws particles through a program of its own and applies fog in it at its merge (the last lander's adaptation, ADR 0028 decision 17).
  - Material parameters and fog meet only in `Scene.slang`'s shading (a small merge) and in the extraction, so material parameters land at any time in phase 6.
- **The game's order.** Phase 5 delivers briefs 1, 3, 4 and 7, so enemies, the options menu and horde mode's caps come first; phase 6 is the art pass (2, 5, 6). If the owner wants the art pass sooner, M29 (light limits) can swap with M31 (material parameters) without changing anything else.
- **Numbering.** M28 to M33, as above. One ADR per milestone, amended per wave: M28 0043, M29 0044, M30 0045, M31 0046, M32 0047, M33 0048.

### 3. Design defaults

Each default goes into its milestone's contract, which may change it only with a stated reason (ADR 0028 decision 15's rule).

**M28 Game services**
- **W28.1, display settings.** `Application` gains the window mode (Windowed, Borderless, Exclusive), the resolution from the monitor's mode list, which scripts can read, and VSync, each applied at a frame boundary.
  - **Platform:** a GLFW monitor and video-mode query and a set-mode call in `Platform/`, Windows backend only, reporting `Unsupported` on Linux and macOS (ADR 0028 decision 10; `PlatformWindowSupport` is the precedent).
  - **Swapchain:** `Swapchain::SetVSync` triggers a recreate, and `Application` keeps the chosen VSync, so device-loss recovery does not revert it.
  - **Persistence:** the engine persists the chosen display settings itself in `user://Display.json` and applies them when the Runtime starts, so a restored fullscreen mode never flashes a window first.
  - **Where the calls do nothing:** the editor's game view, headless runs and every fixed-view session (suites, replays, recordings, lockstep). There the calls return false with a reason and never touch the project's `Window` settings, so they never reach the state hash.
  - **Exclusive mode:** GLFW's monitor mode, without `VK_EXT_full_screen_exclusive`.
- **W28.1, performance readout.** A script call (for example `Application.GetStats()`) that returns frame time (CPU and GPU), visible, dropped and shadowed lights, draw calls, triangles, live particles, skinned meshes drawn (a new counter), bodies (all and active), entities and the script heap.
  - **Dist:** it is available in every configuration, Dist included. The readout's struct moves out of `Automation/` (whose symbols Dist forbids), and `CountSessionBodies` moves to Session.
  - **Phase rule:** `ParticleEmitter:GetParticleCount`'s rule applies: an error in fixed-step gameplay, readable in `OnUpdate`, `OnLateUpdate` and test cases.
  - **Parity:** `stats.get` gains the counters it lacks.
- **W28.2, save data.** A `SaveData` script namespace: get (nil when absent), set, delete, list keys, and an explicit commit.
  - **Values:** what Luau already converts to JSON, held by a session-owned store that survives `Scene.Load`. It is written by a double-preserving serializer (not `JsonWriter`, which writes 32-bit floats) and capped at 1 MiB with a clear error.
  - **Runtime:** the store file is `user://SaveData/Store.json`, published atomically and, on Windows, flushed durably through a new Platform hook (owner's answer 5).
  - **Corruption:** an unreadable file is logged, moved aside as `Store.corrupt-<n>.json`, reported to scripts, and the game starts empty.
  - **Determinism:** fixed-view sessions use an in-memory store, empty by default. A suite seeds it through its settings or `Test`; a replay header (format version 4) records the store's contents at the recording's start, so a replay verifies whatever the machine holds. The store's contents form a state-hash block that is empty when unused, so every committed hash stays.
  - **Editor play:** a store of its own in the project's `Library/`, a private temporary store in a `--read-only` editor, and methods to read, seed and clear it, with an editor panel (parity).
  - **Headless:** a headless Runtime, the Dist smoke test among them, reads the real store but never writes it.

**M29 Light limits**
- **Light cap.** A `Rendering` project setting for the shaded lights per view: 256, 512, 1,024 or 2,048, default 256.
  - The cluster mask's word count follows the setting at run time. The shaders already read the stride from `Clusters.Grid.w`, so only the CPU sizing and the light buffer change.
  - Validation, `stats.get`, the readout and every description that names a cap read the setting.
- **Shadowed lights.** 16 point lights and 16 spot lights per view, ranked by importance with hysteresis, so they stop popping.
  - **Point lights:** the four most important keep today's face size; the rest get half-size faces in a second cube array.
  - **Spot lights:** the existing 4096 atlas holds 16 tiles of 1024.
  - **Lookup:** each shaded light carries its shadow slot in `ShaderLight`'s padding, so the shader no longer searches the shadowed lights linearly.
- **Static shadow caching** (owner's answer 2b). A shadowed light that has not moved, with no caster in its range that moved or deformed, reuses last frame's map. The cache is renderer state that a history reset, a capture's warm-up and a device loss drop, so every capture and golden renders as before.
- **Below the old caps nothing changes.** The ClusteredLights, PointShadows and SpotShadows goldens, the brute-force equality case and the out-of-memory fallback stay byte-identical. The 32-pixel cluster tile is not included (it is compiled into the shaders; a later item if a game needs it).

**M30 Navigation**
- **Recast and Detour** (recastnavigation, zlib licence), vendored as two projects at a pinned release under the standing vendoring rules (owner's answer 3): Recast for the editor's bake only, Detour for the runtime.
  - Both join `CheckBuildConfig.py`'s precise-floating-point list with their FastMath fixtures and their ABI define families.
  - Their process-level allocator and assert hooks are set by `ProcessContext` and listed under §3 rule 5.
  - The vendored code's calls are audited for CRT transcendental functions: walkable slopes are marked first-party with `DetMath`, never by Recast's `cosf`, and a random point is picked first-party from a seeded `RandomGenerator`, never through Detour's global `frand` callback.
- **The bake input** is a pure Scene function over `ComposePhysicsBodies`: static, non-sensor, creatable bodies on the layers the bake names, each collider triangulated first-party in canonical order (primitives tessellated, mesh colliders' triangles, height fields through `TerrainGeometry`), in world space.
  - Its XXH64, together with the settings and modifiers, is stored in the asset and recomputed by `project.validate`, which reports `NAV_MESH_STALE` (and `NAV_MESH_NOT_BAKED`).
  - Triangulating first-party leaves M11's frozen `PhysicsShape.h` unchanged.
- **Asset.** A `NavMesh` asset type with a first-party, validated payload (polygons, detail meshes, areas, agent settings, input hash). Detour's data is built from it at load with `dtCreateNavMeshData`, never trusted as bytes. A `NavMeshData` fuzz target joins `Targets.json`, which is why phase 5 hardens.
- **Components.** A unique-per-scene `Navigation` component holds the `NavMesh` reference, up to four agent types (radius, height, maximum slope, maximum step) and the bake settings. A `NavModifier` component is a box (yaw rotation only) that is either not walkable or carries an area cost.
- **`navigation.bake`.** Like `lighting.bake`: CPU-only, so it works with `--renderer none`; one undoable step; byte-identical rebakes that write nothing; an editor menu action (parity).
- **The `Navigation` script namespace** is usable in the fixed step: `FindPath` (corners, or nil, plus the partial path to the closest reachable point), `ClosestPoint`, `Raycast` along the surface, and `RandomPoint` from a seeded generator. Queries read only the baked data and their arguments, so they are deterministic; a corner cap goes in `ScriptLimits.h`.
- **Debug view.** A navigation view flag for the scene view and an `annotate.navMesh` option for `viewport.screenshot`, drawn as polygon edges.
- **Cost.** A perf case with 150 agents each finding a path two or three times a second, measured within the 60 Hz fixed step.

**M31 Material parameters**
- **A new component,** not new `MeshRenderer` fields: they would re-hash nearly every 3D scene and force every replay's re-recording. Its fields: a tint (multiplied with the base colour), an emissive colour and strength (added), a texture-coordinate offset, and a scroll velocity that advances with simulation time. Simulation time is plumbed into render extraction.
- **GPU path.** The values ride in the instance record (grown from 144 bytes), so instancing keeps its batches and skinned meshes work unchanged. An entity without the component renders byte-identically.
- **Dissolve** (owner's answer 2d): an amount from 0 to 1 with an edge colour and width, over a new built-in noise texture, through the Mask variant in the forward and shadow paths.

**M32 Fog**
- **A new unique-per-scene `Fog` component,** not new `PostProcess` fields (they would change the canonical form of every scene with a PostProcess). Its fields:
  - exponential distance fog: density, start distance, maximum opacity;
  - height fog: density falling off above a base height;
  - colour, optionally tinted by the environment's ambient light;
  - a sun glow toward the first directional light;
  - whether the skybox is fogged.
- **Where it applies.** One `Common/Fog.slang` function, applied in the forward opaque and transparent shading, terrain, the skybox (optional), the SSR composite (multiplied by the transmittance) and the 3D sprites' quad pipelines through a fog constant buffer. Particles get fog in M33.
  - Distance is view depth, so an orthographic, isometric camera fogs sensibly.
  - Additive quads fade rather than tint.
  - World-space UI keeps its accepted no-fog limit.
- **What stays the same.** A scene without fog renders byte-identically. Probe and environment bakes exclude fog. The cost stays under 0.5 ms at 1920 × 1080.
- **Volumetric light** (the brief's stretch item) is not in M32 unless the owner keeps it (owner's answer 2a).

**M33 Particle look**
- **Flipbooks:** a `SpriteSheet` and a clip or frame range on the emitter, played over the particle's life or picked per particle, with optional blending between frames (two UV sets).
- **Rotation:** a random start angle and spin range, drawn from a second random stream of the emitter's, so the committed particle digests of FeatureTest's `ParticleEmitter` suite keep their values.
- **Soft particles:** a fade distance against the scene depth, in a dedicated particle program that reads depth as a shader input (§8.3 avoids read-only depth attachments).
- **Alignment:** camera-facing, stretched along velocity, or flat on a plane.
- **HDR:** an intensity multiplier, so colours above 1 bloom.
- **Fog:** applied in the particle program at M33's merge.
- **TAA:** particle motion vectors, which remove Backlog §6's "particles write no velocity" limit (owner's answer 2e).
- **Lit smoke** stays out (owner's answer 2f).
- **What stays the same:** particles stay presentation only, and their fields' defaults render the Particles golden byte-identically.

### 4. Approvals

Recast and Detour (recastnavigation, zlib) need a `0001-approvals.md` row before phase 5 starts: the catch-all row covers M18 to M26 only, and Roadmap rule 6 collects approvals up front. The pin, checksum and licence follow at M30's contract under the standing rules. Nothing else is downloaded: the noise texture for dissolve is generated, and the flipbook, fog and light fixtures are generated by committed generators.

### 5. Determinism, platforms and goldens

- **Determinism.**
  - Navigation queries, save-data reads and the readout's frame rule follow §4.12. Every replay with agents or saved data verifies in Debug, Release and Dist.
  - Fog, particles, material parameters and the light caps are presentation, except the new components' fields, which are serialized and hashed like any component's. No existing committed hash moves, because every new component, setting and hash block is absent or empty by default.
- **Platforms.** New platform capabilities (display modes, the durable flush) get Windows backends only and report `Unsupported` on Linux and macOS (ADR 0028 decision 10).
- **Goldens.** Each new look gets new golden scenes in golden scaffolds of its own: fog rows and hollows, the light-heavy scene against the brute-force loop, the tinted and dissolving meshes, the flipbook, stretched and flat particles. Existing goldens stay byte-identical. The device class stays nvidia-62x with the driver's major version held.

### 6. Process

ADR 0028's process holds unchanged:
- decision 11: branches and worktrees per milestone, waves and contracts only where interfaces freeze, checkpoint commits with targeted runs, one review pass per wave with every finding fixed, and the milestone end (merge main, one review of the milestone diff, one `CI.py`, fast-forward);
- decision 12: `--milestone` only at each phase's last lander;
- decision 14: the push and the pause after each phase;
- decision 17: a reservation ledger per phase, committed on main before the phase forks, and the merge train.

Two additions:
- **Game-only changes.** A change that touches only a demo game outside a milestone takes the light path the owner set on 2026-11-15: builder session, review, provenance audit, the owner's waiver of `PreCommit.py` recorded.
- **World Tree's needs.** The World Tree session reports new gaps as briefs in `Projects/WorldTree/Design/EngineTasks/`. The lead triages them at each phase boundary, never in the middle of a milestone.

### 7. Reservation ledgers

Each phase's ledger is written at its start, as ADR 0028's were. The contended files the explorers found, and their proposed owners:

- **Phase 5:**
  - **M28:** `ScriptHost.h`, `PlaySession.*`, `RuntimeApp.*`, `EditorPlayController.cpp`, `Application.cpp`'s bindings, `ProjectSettings.*`, the replay header (format version 4) and `StatsMethods.*`.
  - **M29:** the shadow, cluster and `RenderPrepare` files, the `Rendering` settings section's new member (appended after M28's at the merge), and the light rows of `stats.get` (appended to M28's).
  - **M30:** `BuiltinComponents.h`'s new Navigation category, `AssetType` value 19, the validation codes, `RenderSnapshot.h`'s view flag, the screenshot annotation files and the `IScriptHost` navigation member (appended after M28's).
  - **Shared by append in landing order:** `RegisterBindings.cpp`, `RegisterMethods.cpp`, the premake files, `ModuleRules.json` (M30's new module), FeatureTest's scaffolds, IdSeed ranges and the generated references.
- **Phase 6:**
  - **M32:** `Quad.slang`, `QuadBatcher`, `QuadVertex.h`, the view and fog constants, and the SSR composite.
  - **M33:** `ParticlePass`, its new particle program and `ParticleSimulation`.
  - **M31:** the instance record and `PrepareDraws`.
  - **Merged by M33 last:** `Scene.slang` (M31's and M32's hunks), `RenderExtraction.cpp`, `Shaders.json` and `RenderPassDeclaration.h`.

### 8. Gate time

- **Phase 5:** two plain gates (about 1 h each) and one hardened gate (about 9–10 h, at night). Navigation's fuzz target adds a little to the fuzz step, which is sized from the measured costs (75,649 s at M27's gate).
- **Phase 6:** the same.

A rendering editor of the World Tree session holds the GPU lock while it runs, so a hardened gate needs that session idle for the night (owner's answer 7).

## The owner's answers

The product owner, 2026-11-15: "go with your recommendations". Each point of the draft is decided as the lead recommended:

1. **Scope:** the seven briefs, and nothing from the later realms' briefs (physics joints, decals, audio effects) until they are written.
2. **Stretch items:**
   - (a) volumetric light: later, its own item once the art pass shows the need (`Docs/Backlog.md` §2);
   - (b) static shadow caching: kept, in M29;
   - (c) shadow resolution by importance: kept, in M29;
   - (d) dissolve: kept, in M31;
   - (e) particle motion vectors: kept, in M33;
   - (f) lit smoke: later (`Docs/Backlog.md` §2).
3. **Recast and Detour (zlib):** approved for vendoring (`Docs/Decisions/0001-approvals.md`), pinned at M30's contract.
4. **Phases and order:**
   - phase 5: M28 game services, M29 light limits, M30 navigation, landing last;
   - phase 6: M31 material parameters, M32 fog, M33 particle look, landing last.
5. **Save data's durability:** a durable flush on Windows through a Platform hook, `Unsupported` on Linux and macOS.
6. **Process:** ADR 0028's, with the push and the pause after each phase (decision 6).
7. **The World Tree session:** the lead announces each hardened gate beforehand, and the World Tree session closes its rendering editor for that night.

## Risks

- **Recast and Detour's determinism and trust.** Byte-identical bakes and identical paths in Debug, Release and Dist depend on the vendored float code under the precise model; the audit at vendoring and a committed path digest in FeatureTest catch a difference early. Detour data is rebuilt from a validated first-party payload, never trusted.
- **Display modes on the one test machine.** Exclusive fullscreen and resolution switches can disturb the desktop the GPU suites run on. Windowed child tests cover them in a guarded way, and the destructive cases stay out of the default suites.
- **Byte-identity below the old caps.** Growing the light buffers, the instance record and the quad constants must not reorder floating-point work; the existing goldens, the brute-force equality and FeatureTest's byte-compared probe bakes are the guard.
- **The shadow cache's invalidation.** A missed caster change shows a stale shadow. Captures and goldens never use the cache, so a dedicated test moves each kind of caster (transform, skinning, morph, visibility) and checks the map is redrawn.
- **Two sessions in one checkout.** The World Tree session works in `D:\Engine`; the program works in worktrees and never touches `Projects/WorldTree`. The bridge stays the World Tree session's, and milestone FeatureTest work uses `Scaffold.py`'s own editors.

## Before program completion

- [ ] M28 to M33 each closed by its milestone end in its phase's merge train; main pushed after each phase, and the owner's go-ahead given before the next.
- [ ] Each brief's "How the game will check it" met in FeatureTest or a dedicated test, and reported back to the World Tree session.
- [ ] Every approval used has its pin, checksum and licence committed.
- [ ] Architecture, the Roadmap, the skills, the Backlog and the generated references describe what landed; the Backlog's §6 holds each limit the program accepted.
