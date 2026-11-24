# 0061 — M43 Static scenery

- **Status:** accepted. M43's plan and milestone record, in phase 8 of the Skerries program (`Docs/Decisions/0060-skerries-engine-program.md`).
  - **The record.** M43's builder amends this record on `milestone/m43` as each wave lands, in "The W43.1 record" to "The W43.3 record", making decision 3 exact where it leaves a choice to the milestone. The lead's reviews and the landing follow.
  - **Amends Architecture.** §5.2's "O(n), no dirty flags, no stale-cache bugs" (`Docs/Architecture.md:510`) is reversed: world matrices become change-tracked. §5.7's steps 0, 5, 9 and the frame phase's step 2 are rewritten to match. ADR 0028 decision 2's render-thread and GPU-driven cuts stay (ADR 0060, question 5).
  - Lead-owned.
- **Date:** 2026-11-24.
- **Decided by:**
  - the product owner: the program, M43's design default and its gate (ADR 0060 decisions 4 and 7), the kept cuts (question 5) and the research's recommendations (question 8);
  - the lead: the waves, the design defaults below, the research's questions (decision 4) and the ledger needs;
  - recorded by the lead.
- **Base:** main `3aaf4b10`, engine 0.8.0. Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone";
  - ADR 0023; ADR 0028 decisions 2, 10, 11, 12 and 17;
  - ADR 0060 decisions 2, 4, 6, 7 and 9, and its questions 5 and 8;
  - ADR 0059 (the form); ADR 0012 (render interpolation); ADR 0034 decision 2 (instancing); ADR 0044 decision 4 (M29's static shadow cache);
  - Architecture §5.1, §5.2, §5.3, §5.7, §8.2 to §8.4, §8.14, §13.7 and §15.
- **Brief:** Skerries' S16 (`Projects/Skerries/Design/EngineTasks/S16-StaticSceneryCost.md`, high priority), with its measured-world section of 2026-11-24.
- **Research:** `bin/Reviews/P8/research/scale.md`, its S16 section and "Across the group". Where it and this record differ, this record is the rule (decision 4).

## Context

- **The brief.** Every static entity costs CPU every frame and every tick, whatever changed.
  - **Pipeline test** (8,591 glTF pieces, 17,193 entities, no script, one body): about 2.7 µs a static entity a frame; `Stage:Prepare` 15.1 ms a view, `ExtractRenderSnapshot` 7.8 ms, `FixedSteps` 9.1 ms, `Update` 13.9 ms; 3,574 `DirectionalShadows` draws against 11 `ForwardOpaque`; 11 frames a second while the GPU takes 3 to 5.5 ms.
  - **The measured world** (6,365 entities: 2,844 mesh renderers, 245 scripts, 145 bodies): 47.8 ms a frame in the editor's two views (21 fps), about 37 ms in a game's one view; lockstep test runs take about 19 ms a tick, so the game's 23 suites and five replays take 91 minutes.
  - **Expected:** transforms, bounds and render data cached for what did not change; culling through a spatial structure; shadow draws instanced as the forward pass's; a perf metric.
  - **Its check:** dense Landfall at 60 fps in a Release export (about 2 ms of CPU for 20,000 static entities in one view), `DirectionalShadows` draws of the order of `ForwardOpaque`'s, and replays that still verify when a script moves a static entity.
- **What the engine has today** (each fact checked at `3aaf4b10`):
  - **Transforms.** `TransformSystem::Update` recomputes every world matrix in canonical order, two hash lookups and an `emplace_or_replace` each (`Scene/TransformSystem.cpp:210-223`), by design (`TransformSystem.h:24-26`).
    - A tick runs it at least twice, pre-step and post-step (`Session/PlaySession.cpp:1923-1930`), and the frame phase once more (`:1350-1351`).
    - `TakeInterpolationSnapshot` copies every world matrix into `PreviousWorldTransformComponent` (`:1224-1235`); `RecordReferenceTransforms` copies it again (`:1237-1242`); `TagWritesOutsideSteps` compares and re-records every one (`:1244-1257`).
    - The editor runs a full update every frame in edit mode (`Editor/Source/Editor/Private/EditorHostViewports.cpp:306-307`).
  - **Writes.** Reflected writes go through `Entity::Patch` (`Scene/ComponentRegistration.h:48`), which fires EnTT's `on_update` (`Scene/Entity.h:142-145`), as do `SetParent`'s `keepWorld` patch (`Scene/Scene.cpp:517`), root motion (`AnimationSystem.cpp:286`), bone attachments (`BoneAttachments.cpp:395`) and native systems (`Session/NativeWorld.cpp:415`). Six writes bypass the signals: `PhysicsSystem.cpp`'s `RestoreLocalPose` and `WriteWorldPose` (`:269-294`) and its `Teleport` (`:2365`), and `Physics2DSystem.cpp:271, 280, 2263`.
  - **Per-tick whole-scene scans.**
    - Physics: `PreStep` and `PostStep` copy the canonical order and probe each entity (`PhysicsSystem.cpp:1497`, `:1703-1720`); `BodiesByOwner` is a `std::map` (`Scene/Private/PhysicsSystemState.h:274`); `HasHierarchyChanged` re-records the whole hierarchy whenever the data revision moved (`PhysicsSystem.cpp:567-588`; 2D: `Physics2DSystem.cpp:561-580`), and the revision moves on every write (`Scene/Scene.h:169-170`); `FindShapeChanges` visits every body's inputs every tick (`PhysicsSystem.cpp:949-997`).
    - Scripts: `RefreshLifecycleOrder` rescans the canonical order whenever the data revision moved (`Scripting/Private/ScriptEngineState.cpp:49-66`); `Scene.FindByName`, `FindByTag` and `FindAllWithComponent` walk it per call (`Scripting/Bindings/Scene.cpp:168, 213, 228`).
    - Audio walks every entity (`Scene/AudioSystem.cpp:605-615`), and its listener selection walks it again (`:405-431`).
    - UI layout scans every entity once any canvas exists (`Scene/UiLayout.cpp:127-131`, guarded at `:454-457`), at step 10, in the frame phase and in each extraction (`RenderExtraction.cpp:957-958`).
    - Animation (`AnimationSystem.cpp:324-339, 376`), particles (`ParticleSystem.cpp:94`) and sprite animation (`SpriteAnimationSystem.cpp:109`) scan it once one such component exists.
  - **Canonical order** is one cached `std::vector<UUID>` (`Scene.h:250-251`), dirtied by every structural change (`Scene.cpp:312, 348, 528, 585, 638`) and rebuilt whole (`:709`).
  - **Extraction** visits every entity with about twenty component probes and a fresh `MeshDrawItem` per renderer (`Scene/RenderExtraction.cpp:754-786`), per view per frame. Pick ids are dense in snapshot order (`Renderer/RenderSnapshot.h:342, 834`), and the renderer copies the table for the picked image (`Renderer/SceneRenderer.cpp:1426`).
  - **Prepare** culls every submesh of every item (`SceneRenderer.cpp:958`), copies each mesh's submesh and default-material vectors (`:944-945`), keeps a per-render `std::map` of materials (`:905`) and writes a 208-byte `InstanceRecord` per draw (`:1078`; `Resources/Shaders/Shared/InstanceRecord.h:37-60`). `RenderViewHistory::Apply` stable-sorts every mesh item per view (`Renderer/TemporalHistory.cpp:75-104`).
  - **Shadows.** Every finite item is copied into the casters (`SceneRenderer.cpp:1823-1836`); `ShadowPass::PrepareCasters` copies each `GpuMesh` and hashes an XXH64 signature per caster draw every frame (`Renderer/ShadowPass.cpp:391, 432-466`) and batches once for every face (`:494`); each cascade culls the whole list (`:688`) and draws runs of adjacent visible draws (`:566-584`), so a batch breaks at every culled neighbour. Cascades are cleared and redrawn every frame (`:677-692`).
  - **Bindings.** The forward set 0 holds t0 to t20 but t4 and t7 (t11 is the cluster masks, `Passes/Scene.slang:101`; t17 to t20 M42's reflections, `:118-121`); Shadow and Selection bind `Instances` at t10 (`Passes/Shadow.slang:26`, `Passes/Selection.slang:21`). `DrawConstants` is 16 bytes with a free `Padding0` (`Shared/DrawConstants.h:69-72`).
  - **Registration.** A reflected component cannot be empty: `static_assert(!std::is_empty_v<T>, ...)` (`Scene/ComponentRegistration.h:76`).
  - **Already cheap:** "no component at all" guards in animation, particles and UI; instancing of every mesh draw (ADR 0034 decision 2); M29's spot and point shadow cache by signature (ADR 0044 decision 4).
- **Corrections to the brief** (each goes to the Skerries session through the product owner at the phase's end):
  - **The entity cap** is the default of `Simulation.MaxEntities` (`Project/ProjectSettings.h:45`, `ProjectSettings.cpp:463`), not a limit (ADR 0060 decision 9).
  - **The 2 ms is measured, never gated.** No outcome may depend on how long anything takes (`Docs/Architecture.md:655`). The gate is deterministic counters; the 2 ms is a warning-only Perf.py budget and the game's Release-export check.
  - **"Shadow draws instanced as the forward pass's"**: both are instanced already. Shadows draw more because the casters are batched before each cascade culls them (above); M43 batches static casters after the cull.
  - **"By a Static flag, by change tracking, or both"**: both. Change tracking for every entity's transform; an authored `Static` component on a scenery root, covering its subtree.
  - **Scripts.** The pipeline test had none. A scripted island adds three whole-scene paths on every data write (the lifecycle order, the physics hierarchy record, `Scene.Find*`), which is why the Ridgeback pass measured 4.5 µs an entity. M43 covers them.
  - **Test runs.** A `--renderer none` session still extracts the game view every frame (`ExtractionEnabled` defaults on, `PlaySession.cpp:427, 1370-1371`), so M43 shortens test runs too. The state hash stays O(n) at each replay checkpoint (every 60 ticks, `Asset/ReplayData.h:59`) and each `play.step` (`Automation/Methods/PlayMethods.cpp:168`); M43 measures it (decision 3, "Perf").
  - **A static entity that a script moves** is handled correctly, but it rebuilds its cell and renders at its new pose without interpolation. Moving parts belong outside a static subtree, or under `Scope: NotStatic`.

## Decisions

### 1. Scope

S16, as one milestone M43, phase 8's first lander (ADR 0060 decision 2). It delivers:
- change-tracked transforms and incremental per-tick systems, so a tick costs what changed;
- an authored `Static` component and a render static set: CPU cell culling and GPU-resident instance records;
- per-face batching of static shadow casters after the cull;
- deterministic counters, two Perf.py metrics, a validation code, a Lint rule and FeatureTest coverage in every mode.

**Out** (`Docs/Backlog.md` §2 at landing; the first stays in §8):
- the render thread and GPU-driven rendering: kept cut (ADR 0028 decision 2; ADR 0060 question 5), the render-thread row's reason updated;
- automatic static classification (an entity unchanged for N ticks joins the set);
- jobs for extraction or Prepare (§4.11: jobs never touch the ECS);
- a bounding-volume hierarchy for static or dynamic items;
- a project setting for the static cell size (an engine constant, decision 4);
- dynamic shadow casters batched after each face's cull (their recording stays byte for byte);
- a cheaper state hash (a streamed sink or per-entity fragments), until `stateHash/20000Entities/ms` is measured (decision 4);
- static sprites, tilemaps, texts, lights, probes and terrains (they are extracted through component views, not cached);
- an editor overlay of the static cells, and a static badge in the hierarchy.

**Accepted at landing** (§6):
- A static entity that moves rebuilds its cell each tick it moves and renders at its current pose, without interpolation or motion vectors.
- Skinned, morphed, mirrored-glass (`PlanarReflection`) and scrolling (`MaterialParameters.UVScroll` non-zero) items stay out of the set and are extracted every frame.
- A structural change under one root rebuilds that root's canonical segment: a spawn under a flat root of 8,000 children costs that root.
- Debug builds recompute every world matrix and rebuild the static set after each incremental pass, to check them.
- The static cell is 32 m, an engine constant.

### 2. Structure: one milestone, three waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 8 | M43 | Static scenery | S16 | first in phase 8's fixed train order (M43, M44, M45, M46, M47, M48; ADR 0060 decision 2) | the batch's `CI.py --require-clang-cl` | ~9.5 (W43.1 ~3.5, W43.2 ~4, W43.3 ~2), +1 at risk |

One builder, on `milestone/m43` in `D:/Engine/.claude/worktrees/m43`, works the waves in order:
- **W43.1, change tracking and incremental systems (CPU; Scene, Session, Physics, Scripting):**
  - the structure revision; canonical segments, the canonical key and `CollectCanonical`; every per-tick and per-event whole-scene scan moved to them;
  - change-tracked transforms, `Scene::MarkTransformWritten`, the Debug full-recompute check and the Lint rule;
  - interpolation and frame-write tagging over the changed list;
  - `Static`, `HierarchyStaticTag` and `SCENE_STATIC_MOVES` with its fixture;
  - the tick counters; the unit cases.

  Its first checkpoint fixes what W43.2 builds on, M34's way: the structure revision, the canonical key, the changed-list API, `Static` and its tag, `Renderer/RenderStaticSet.h`, `RenderSnapshot::StaticSet` (unfilled), and the `DrawConstants`, `InstanceIndex` and register layouts (declared, unread), so every image stays byte-identical.
- **W43.2, the static set and the renderer's static path:**
  - `Scene/RenderStaticCache.{h,cpp}` and extraction through component views;
  - cell culling, GPU-resident records and indexed draws in every mesh view;
  - static shadow casters per cell version, batched per face after the cull;
  - `RenderStats`' fields, device loss and out-of-memory, the gpu cases;
  - a filtered golden run proving every existing golden byte-identical.
- **W43.3, coverage, tools and documents:** `stats.get`, the Stats panel and the readout; Perf.py's metrics; FeatureTest's scaffolds, suite and replay; the Python case; Architecture, the Backlog, the skills and the generated references.

Numbering: M43's record is this ADR, 0061; M44 to M48 take 0062 to 0066 (ADR 0060 decision 3).

### 3. Design defaults: M43 Static scenery

**What M43 adds:** a component and a registry enum; a `RenderSnapshot` member and a snapshot header; a shared struct and a `DrawConstants` flag; two set-0 registers; stats fields and readout members; one validation code; one Lint rule; two perf metrics; two FeatureTest scaffolds.

**What it does not add:** a namespace, a function, method or callback, an `IScriptHost` member, an automation method, a settings section or field, an `AssetType`, a `PlaySessionPhase`, a render stage, pass, pipeline or program, a state-hash block, a format version, a fuzz target, a golden, a module or a vendored library.

- **The structure revision.** `Scene::GetStructureRevision()` moves only with creation, destruction, reparenting, sibling order and enable or disable. `PhysicsSystem` and `Physics2DSystem`'s `HasHierarchyChanged` and `RefreshLifecycleOrder` key on it instead of `GetRevision()`. `GetRevision()` keeps its meaning (automation `ifRevision`).
- **Canonical order in segments.**
  - One segment per root entity, rebuilt alone when a structural change touches its subtree; a new root appends one. The flattened order stays what `GetCanonicalOrder()` returns today.
  - `Scene::GetCanonicalKey(entity)`: (root ordinal, index in its segment), kept in a runtime-only component.
  - `Scene::CollectCanonical<T...>(std::vector<entt::entity>&)`: the view of `T...`, sorted by key, O(k log k).
  - **Moved to it** (each list keeps today's order, so callbacks, events and hashes do not move): physics, through lists kept with the bodies in canonical-key order: the pre-step over the kinematic bodies' owners and the owners whose transforms changed (teleports), the post-step over the dynamic bodies' and characters' owners, `FindShapeChanges` over the bodies whose shape inputs were written, and the state-hash block over every body; audio sources; the listener and `FindPrimaryCamera`, cached until a structural change or a Camera or AudioListener write; UI layout over each canvas's subtree; Animators, SkinnedMeshes, emitters, sprite animators and bone attachments; `Scene.FindByName` and `FindByTag` through an index rebuilt on a structural change or a Name or Tags write, and `FindAllWithComponent` over the view; the script engine's instance synchronization and creation (`ScriptEngine.cpp:324`, `ScriptInstances.cpp:42`). The W43.1 record lists every scan moved and every one kept (load, save, validation, captures).
- **Change-tracked transforms.**
  - A per-scene changed set, fed by `on_construct` and `on_update` of `TransformComponent`, by `SetParent` (the moved subtree's root) and by `Scene::MarkTransformWritten(entity)`, which the six signal-bypassing writers call.
  - `TransformSystem::Update` reduces the set to its topmost entries and recomputes each one's subtree, parents first, with the same products in the same order (`World = ParentWorld * Local`), so every matrix is bit-identical to today's. It returns the entities whose matrix bits changed.
  - **The Debug check.** In `ENGINE_DEBUG` builds (`Dependencies.lua:201`) each update also recomputes every matrix and `ENGINE_CORE_VERIFY`s equality, naming the first stale entity, so a missed write path fails a Debug test instead of drawing stale in every configuration alike.
  - **The Lint rule `banned-transform-write`**: a mutable `TransformComponent` access (`GetComponent`, `TryGetComponent`, `get`, `try_get`) outside `ModuleRules.json`'s new `Banned.TransformWriteFiles` (`TransformSystem.cpp`, `PhysicsSystem.cpp`, `Physics2DSystem.cpp`), with the seeded fixture `Tests/Data/Lint/TransformDirectWrite/`. Today's reads through a non-const `GetComponent<TransformComponent>()` (`BoneAttachments.cpp:262, 392, 434`, `PhysicsComposition.cpp:327`, `Physics2DComposition.cpp:282`, `PrefabInstantiator.cpp:1789`, `GizmoController.cpp:201, 255`) stay legal: the rule flags a write, or W43.1 makes them const accesses; the fixture holds a read that passes and a write that fails. Every later transform writer goes through `Entity::Patch`, or through one of the three files with `Scene::MarkTransformWritten` (M44's wheel poses among them), and a new file on the list needs a ledger amendment.
- **Interpolation over the changed list** (§5.2's semantics unchanged; ADR 0012's cases and FeatureTest's `Timing` suite pin them):
  - the snapshot copies `PreviousWorld` only for entities whose world changed since the last snapshot;
  - `SessionDisabledAtSnapshotTag` is re-recorded only when the structure revision moved;
  - the frame phase tags the entities its update changed, and re-records only their references; an entity written to the same bits is still not tagged.
- **The tick counters** (`PlaySession::GetLastTickWork()`): `TransformsUpdated` (matrices recomputed in the tick and its frame phase) and `EntitiesVisited` (the entities the per-entity passes of the tick iterated, summed). Both deterministic. The per-tick passes later landers add (M44's vehicle input, M47's buoyancy) iterate through `CollectCanonical` from their merges and add their visits to `EntitiesVisited` (the ledger's rule).
- **The `Static` component.** `Static`, version 1, category Core, after `PrefabLink` in `CoreRegistration.cpp` and in `BuiltinComponents.h`'s Core row; serialized, script-visible, editor-visible, removable; no requirement.
  - **`Scope`:** registry enum `StaticScope`, default `Subtree`:
    - `Subtree`: the entity and its descendants are static;
    - `EntityOnly`: the entity alone;
    - `NotStatic`: the entity and its descendants are not static, even below a static ancestor.
  - **The rule.** Walking up from an entity, the first `Static` met decides: its own with any scope but `NotStatic` makes it static; an ancestor's `Subtree` makes it static, `NotStatic` makes it not, and `EntityOnly` is passed by.
  - **`HierarchyStaticTag`** (runtime-only) marks effectively static entities, refreshed as `RefreshHierarchyDisabled` refreshes its tag (`Scene.cpp:728`): on a structural change and on a `Static` write.
  - **Writing to a static entity** is allowed and correct (decision 1's §6 limit).
- **The render static set.**
  - `Renderer/RenderStaticSet.h` (new; added to `ModuleRules.json`'s `SnapshotHeaders`): `RenderStaticSet {std::vector<Ref<const RenderStaticCell>> Cells; uint64_t Version}`; `RenderStaticCell {glm::ivec2 Key; uint64_t Version; std::vector<MeshDrawItem> Items; std::vector<UUID> PickTable}`.
  - **Cells:** a loose XZ grid of `StaticCellSize` (32 m): each item in the cell of its world origin, cells in key order, items in canonical order. The renderer derives each cell's bounds from its items' meshes, so a large item enlarges its cell.
  - **Items:** a static entity's `MeshRenderer` that is `Visible` with a mesh, at its current world matrix, its `MaterialParameters` resolved. Skinned or morphed items, items whose entity holds a `PlanarReflection`, and items with a non-zero `UVScroll` stay dynamic.
  - **`Scene/RenderStaticCache.{h,cpp}`**, owned by the `Scene` (a `Scope` member, created at the first `Static`), fed by the changed list and by `MeshRenderer`, `MaterialParameters`, `Static` and hierarchy-tag signals. Its refresh at extraction copies only the changed cells (copy on write); `StaticCellRebuilds` counts them.
  - **The Debug check.** `ENGINE_DEBUG` builds rebuild the whole set at each refresh and verify it equals the cached one.
  - **Extraction.** `RenderSnapshot` gains `Ref<const RenderStaticSet> StaticSet` after M42's `PlanarReflections` (`RenderSnapshot.h:877`), a frozen-header change. `Meshes` keeps the dynamic items, and the canonical loop becomes `CollectCanonical` over the drawn and lit components (a list later landers append their drawn types to: M47's `Water` after the terrains), so a static entity costs extraction nothing. Without `Static` the snapshot extracts as before, byte for byte.
- **The renderer's static path.**
  - **Per cell version** (keyed also by its meshes' and materials' mirror versions): its submesh draws, keys, bounds, submesh count, caster draws with their M29 signatures, and its records. Static records carry `PreviousWorld` equal to `World`; `RenderViewHistory` never sees them.
  - **GPU-resident records.** `GpuResourceCache` (§8.14's third place) mirrors the set's records in one device-local buffer, each cell a range rewritten only when its version changes; device loss drops it and the next render rebuilds it; creations go through `RetryAfterTrim`.
  - **Pick ids.** A static record's id is `StaticPickIdBase` (2^31) plus its cell's slot and index, resolved against the cells' tables, so records never change with the frame's dynamic items. The W43.2 record fixes the encoding.
  - **The cull.** Each view culls cells (frustum, `FarClip`, M42's mirror cull and clip plane), then the items of the cells it cuts; a culled cell adds its submesh count to `CulledSubmeshes`, so the counts equal the dynamic path's. Cost: O(visible cells + visible items).
  - **Indirection.** A per-view `InstanceIndices` buffer (SceneRenderer state, place 2) of `InstanceIndex {uint32_t Record; uint32_t Reserved}` (`Shared/InstanceRecord.h`, 8 bytes): `Reserved` is 0 and unread, reserved for M46's per-view fade (the ledger; M46's level goes in the grouping key).
    - `DrawConstants::Padding0` becomes `Flags`, with `DrawConstantsStaticInstances` = 1: instance i is `StaticInstances[InstanceIndices[FirstInstance + i].Record]`.
    - Set 0 gains `StaticInstances` at t21 and `InstanceIndices` at t22 in the prepass, overdraw and forward layouts (`Private/ForwardPipelines.cpp:48, 60, 85`) and in Shadow's; `Terrain.slang`, and every later program that shares the forward layout (M47's `Water.slang`), declares them unread (`PipelineFactory.cpp:647-652`).
    - `Common/Instances.slang` (new) holds the one load function that `Scene.slang`'s `GetInstanceIndex` and record reads (`:302, 361, 410-419, 503, 642`) and `Shadow.slang:62, 93` call.
    - Dynamic draws keep today's path, byte for byte.
  - **Draw order.** Static draws sort by (pipeline, material, mesh, submesh) as today and come before the dynamic draws of their key; within a key, instances follow cell order, then canonical order. One indexed draw per key per view. Static transparent draws join the back-to-front sort, and each run of adjacent static draws of one key is one indexed draw.
  - **Every mesh view** takes the set: the camera, its prepass, picking, the overdraw and data views, M42's mirror views, `ProbeCapture` and `ViewportCapture`; the CPU readers of the snapshot's meshes read the set too: the wireframe overlay (`EditorOverlay.cpp:338`), `RENDER_NO_LIGHTING`'s emission check (`RenderAnnotations.cpp:175-193`) and `GetExtractedPosition` (`FeatureTestRunner.cpp:521`). **Selection** copies a selected static item's record into the frame's `Instances` (a handful), so `Selection.slang` is unchanged.
- **Shadows.**
  - Static caster draws and their signatures are prepared once per cell version, never per frame (M46's W46.4 adds the wind phases for a face holding a static wind caster).
  - Each cascade, spot tile and point face culls cells, then its cells' casters, and writes its visible static casters' indices grouped by key: one draw per key and face. M29's cache hashes the visible static casters' signatures as it hashes the dynamic draws'.
  - Depth maps do not depend on draw order (the nearest depth wins; ties write the same value), so a static scene's shadow texels equal its dynamic twin's, byte for byte.
  - Dynamic casters keep `PrepareCasters` and `RecordView` as they are.
- **Determinism and presentation.** `Static` is component data, in the serialization and the hash of a scene that holds it; the set, the cache, the tags and the counters are not hashed. No committed hash moves (decision 6).
- **Validation: `SCENE_STATIC_MOVES`** (Warning, not fixable): an effectively static entity with a Dynamic or Kinematic `RigidBody`, a `CharacterController`, a `BoneAttachment` or an `Animator` whose `RootMotion` is `Apply`. One finding an entity, naming the component (M44 appends `VehicleWheel` to the list at its merge); `FindStaticSceneryIssues(scene)` in `Scene/StaticScenery.{h,cpp}` with its own code array, `StaticSceneryDiagnosticCodes`, called by a new `CheckStaticScenery` in `CheckScene`, after `CheckPlanarReflections` (`ProjectValidator.cpp:1670`); appended to both of `ProjectValidator.cpp`'s tables after M42's `RENDER_REFLECTION_INVALID` (`:224`), the first of phase 8's ten codes; fixture `Tests/Data/Validation/SCENE_STATIC_MOVES/`.
- **Scripts.** No function, method or callback; the generated `Static` proxy reads and writes `Scope` (gate 4). `PerformanceStats` appends `TransformsUpdated`, `EntitiesVisited`, `StaticCellRebuilds`, `StaticItems` (the last extraction's set, in every mode) and `StaticCellsVisible` (the game view's).
- **Automation and the editor.** No method: 126 methods and 86 MCP tools stay. `Static` goes through `entity.create`, `entity.update`, `edit.batch`, `component.schema` and `prefab.*`. The generic inspector shows it in Core.
- **Stats.**
  - `RenderStats` appends, after `PlanarReflectionEntities` (`RenderStats.h:101`): `StaticItems`, `StaticCells` (the rendered set), `StaticCellsVisible` (the camera's cull) and `DynamicItems` (the snapshot's `Meshes`).
  - `stats.get`'s view summary appends `staticItems`, `staticCells`, `staticCellsVisible` and `dynamicItems`; `StatsGetResult` appends `transformsUpdated`, `entitiesVisited` and `staticCellRebuilds` after `particles` (0 without a session). Each is described.
  - The Stats panel appends the row "Static scenery: <items> items, <visible> / <cells> cells" after "Planar reflections", the first of phase 8's four rows (M45's, M46's and M47's follow), as M42 appended its row with every editor-UI golden byte-identical (ADR 0058, line 45).
- **Perf** (`Scripts/Perf.py`, `Scripts/Lib/perf.py`, warning-only), in a new capture part `staticScenery`, appended to `CAPTURE_PARTS` (`Perf.py:88`):
  - `staticScenery/20000Entities/cpuMs`, budget 2.0 ms: a generated `Assets/Perf/StaticScenery.scene` of 10,000 built-in-mesh pairs (a root and its mesh child) under one `Static` root, a camera, a shadowed sun; lockstep ticks with the headless game view at 1920 × 1080; the per-tick sum of `profiler.capture`'s CPU zones of the fixed step, the frame phase and the game view's render (the W43.3 record names them).
  - `stateHash/20000Entities/ms`, no budget: `PlaySession::ComputeStateHash` on the same scene, which decides the deferred cheaper hash.
- **Device loss and out-of-memory.** The record buffer is `GpuResourceCache`'s, the index buffer the renderer state's: the existing release and rebuild cover both, with one `Test::RunDeviceLossTest` case and one out-of-memory case (the static draws skipped for the render, its error returned, the next render drawing them).
- **FeatureTest** (IdSeeds 4001 and 4002, in M43's block 4000-4099 of phase 8's ledger; every gate in each mode the feature supports):
  - **`StaticScenery.jsonl`** (4001) makes `StaticScenery.scene` and its suite, bound in `Suites.jsonl`: a `Static` root over built-in-mesh pairs with static colliders, an `EntityOnly` and a `NotStatic` entity (gate 1), a script that moves one static entity in `OnFixedUpdate`, and a raycast against it. The suite reads `Scope` through the proxy; checks the moved entity's pose and the raycast's hit at it; reads the readout's counters (`TransformsUpdated` and `StaticCellRebuilds` 0 on a tick where nothing moves, `StaticCellRebuilds` 1 on a tick that moves the static entity); and, where `RenderAvailable`, checks `StaticCellsVisible`.
  - **`StaticSceneryReplay.jsonl`** (4002) records `StaticScenery.replay` from a recording suite whose planned input drives a body into the static colliders while the script moves a static entity. It verifies at one hash in Debug, Release and Dist, and fails without its input.
  - Gates: 1 (`Static`), 1b (`StaticScope`'s three values), 4 (`Scope` through the proxy), 6 (the code's fixture), 7 (every description). Nothing new for 2, 3, 5, 8 or 9.
- **Python.** `Tests/Automation/test_static_scenery.py`: marks a root `Static` through `entity.update`, steps a lockstep session, and reads `stats.get`'s `transformsUpdated`, `entitiesVisited` and `staticItems`; positions and counts are compared with a non-static copy (their hashes differ by the component).
- **Documents.**
  - **Architecture:** §2.3 (the Lint rule; Perf.py's metrics); §5.1 (the structure revision, segments, the canonical key, `CollectCanonical`); §5.2 (change-tracked transforms replacing "no dirty flags", interpolation over the changed list, `Static`'s rule); §5.3 (the row and the runtime-only list); §5.7 (steps 0, 5 and 9 and the frame phase's step 2: incremental updates); §8.2 (the snapshot member); §8.3 (the static path, culling, the draw order and the shadow batching); §8.4 (`DrawConstants`' flag, t21 and t22, `InstanceIndex`); §8.14 (the record buffer's place); §13.7 (the stats fields, the readout, the code); §15.5 (the scene's row).
  - **The Roadmap's M43 section.**
  - **`Docs/Backlog.md`:** §2 and §6 (decision 1); §8's render-thread row keeps its cut with its reason updated ("S16 measured the first CPU-bound frame; the cost was per-entity bookkeeping, which M43 removed").
  - **Header comments** of `TransformSystem.h`, `Scene.h`, `RenderSnapshot.h`, `RenderExtraction.h`, `DrawConstants.h`, `InstanceRecord.h` and `ShadowPass.h`.
  - **The skills:** game-building's `prefabs-and-levels` (marking an island's scenery root, `Scope`, what stays dynamic) and `look` (shadow draws); game-debugging (the counters, why a static entity renders without interpolation) and its `validation-codes` row; add-component (never write a `TransformComponent` directly; the Lint rule).
  - **The generated references.**

### 4. The research's questions, decided by the lead

The owner answered the research's three S16 questions with their recommendations (ADR 0060 questions 5 and 8):
1. **How static scenery is marked:** an authored `Static` on a scenery root, plus change tracking of every transform. Automatic classification makes cost depend on history and hides what is cached.
2. **The acceptance:** deterministic counters gate; the 2 ms is a warning-only budget and the game's Release-export check (`Docs/Architecture.md:655`).
3. **The cuts stay:** the cost is per-entity bookkeeping; CPU cell culling with GPU-resident records is not GPU-driven rendering (no compute culling, no indirect counts).

The design choices the research left open:
4. **`Static` has a field, `Scope`.** The research had none, but a reflected component cannot be empty (`ComponentRegistration.h:76`). The field also gives the opt-out the research lacked: a turning windmill under an island root takes `NotStatic`.
5. **The cell size is an engine constant (32 m), not `Rendering.StaticCellSize`.** A settings field rewrites every tracked project's `.eproj` through `project.upgrade`, as M32's `SsrHalfResolution` did (ADR 0047, line 115), and ADR 0060 decision 6 gives M43 none.
6. **The cheaper state hash waits.** The research's streamed hash (its G) still serializes every entity, because the document's `ComponentVersions` precedes its entities (`Scene/SceneSerializer.cpp:1135-1180`). M43 measures `stateHash/20000Entities/ms` and leaves the change to a §2 row. ADR 0060 decision 4 does not list it for M43.
7. **Registers t21 and t22, not t11.** t11 is the forward set's cluster masks (`Passes/Scene.slang:101`).
8. **Selection keeps its program.** A selection is a handful of items, so their records are copied per frame.
9. **Dynamic casters keep their recording.** Existing cases count their shadow draws (`RenderStatsTests.cpp:203`), and the brief's check needs only the static casters batched after the cull.
10. **A loose grid, no oversize list.** Extraction never reads assets, so it cannot know an item's size; the renderer derives each cell's bounds instead.
11. **`InstanceIndex` reserves a word for M46,** whose per-view level and fade the research puts in this entry, so M46 changes no layout of M43's.

### 5. Approvals

None. Nothing is downloaded or vendored; the fixtures are built-in meshes made by the scaffolds.

### 6. Determinism, platforms and goldens

- **Determinism.** No committed hash moves: incremental matrices are bit-identical, no existing scene holds `Static`, every iteration order is unchanged. `Scaffold.py --check`, the determinism suite and the games suite confirm it. `StaticScenery.replay` verifies in Debug, Release and Dist.
- **No `project.upgrade`,** no format change: `Static` is a new component, version 1; the entity document stays 1 (ADR 0060 decision 6).
- **Platforms.** Only Windows is verified (ADR 0028 decision 10). M43 adds no platform capability.
- **Goldens.** None added; the 56 on `nvidia-62x` stay byte-identical, since none holds `Static` and the dynamic path is unchanged. A drift is a bug, never a golden update.
- **Fuzz.** No reader, no target: 53 stay.

### 7. Process

As ADR 0060 decision 7:
- **No contract.** One builder works the waves in order; the lead runs one independent review of each wave and has every confirmed finding fixed, nits included.
- **Frozen headers.** `RenderSnapshot.h`, `TransformSystem.h` and the shared shader structs change only as decision 3 names, as reviewed changes to a frozen interface.
- **GPU discipline.** Filtered gpu, golden and automation runs only, through `Test.py` under the machine locks.
- **Checkpoint commits** run their targeted checks.
- **The landing.** The lead's, first in phase 8's batch, in train order; one merge review; the gate is `CI.py --require-clang-cl`. The hardening runs once, at the program's last phase (ADR 0060 decision 7).

### 8. Reservation needs

Phase 8's reservation ledger, committed in `Docs/Decisions/0060-skerries-engine-program.md` as its section "Phase 8 reservation ledger", is the rule: the rows below are M43's rows there, with its values, orders and owners, in the fixed train order M43, M44, M45, M46, M47, M48. A deviation needs a ledger amendment on main. Today's values are read at `3aaf4b10` (ADR 0058's landing record, lines 43-45, for the counts phase 7 raised).

| Allocation | Today | M43 |
|---|---|---|
| ADR numbers | 0059 the last record; 0060 the program | 0061, this record |
| FeatureTest IdSeeds | 101 scaffolds, the highest 3902; phase 8 from 4000 | `StaticScenery.jsonl` 4001, `StaticSceneryReplay.jsonl` 4002 (M43's block 4000-4099), listed first of phase 8's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516) |
| Suites, replays and scenes | 116, 26, 104 | +1 each |
| Components and categories | 64 in 16 categories; Core ends with `PrefabLink` | `Static` in Core after `PrefabLink`: 65, no category; `MakeValidationFixtures.py`'s `COMPONENT_ORDER` gains it after `PrefabLink`; registry enum `StaticScope`; runtime-only `HierarchyStaticTag` and the canonical key's component, neither serialized nor hashed |
| Validation codes | 75, the last `RENDER_REFLECTION_INVALID` (`ProjectValidator.cpp:224`) | 76: `SCENE_STATIC_MOVES` (Warning), first of phase 8's ten; its array `StaticSceneryDiagnosticCodes` (1, `Scene/StaticScenery.h`) joins the count line; `CheckStaticScenery` in `CheckScene` after `CheckPlanarReflections` (`:1670`), then M45's `CheckSky` and M47's `CheckWater` |
| Settings sections | as today | none, and no field |
| Script namespaces and proxies | 22 namespaces; `PerformanceStats` 24 members (`ScriptPerformanceStats.h:15-50`) | no namespace; the `Static` proxy (generated); `PerformanceStats` 29: `TransformsUpdated`, `EntitiesVisited`, `StaticCellRebuilds`, `StaticItems`, `StaticCellsVisible`, first of phase 8's |
| `IScriptHost` | ends with `GetMusic` (`ScriptHost.h:250`) | none |
| Automation methods, MCP tools | 126, 86 | unchanged |
| Stats | `RenderStats` and the view summary end with `PlanarReflectionEntities`; `StatsGetResult` with `Particles` | +4 view fields (36) and +4 `RenderStats` fields, +3 top-level fields (21), one Stats panel row, each first of phase 8's (M45, M46 and M47 append after them) |
| Stages, passes, pipelines, programs | 13, 34 (41 rows), 77, 34 | unchanged |
| Binding layouts | forward set 0 t0–t20 (t4, t7 free); Shadow t10, t16 | t21 `StaticInstances`, t22 `InstanceIndices` in the prepass, overdraw, forward and Shadow layouts, M46's b8 after them; SelectionMask unchanged; `Terrain.slang` and M47's `Water.slang` declare them unread |
| Shared shader structs | `ViewConstants` 512; `InstanceRecord` 208; `DrawConstants` 16; draw flags 1–16 | unchanged sizes; `DrawConstants::Flags` (`Padding0`) with `DrawConstantsStaticInstances` 1; new `InstanceIndex` (8 bytes, `Reserved` for M46); no draw flag |
| Snapshot | last member `PlanarReflections`; `SnapshotHeaders` holds 3 | `StaticSet` after it, then M45's `Sky`, M46's `Wind` and M47's `Waters`; `Renderer/RenderStaticSet.h` added (4) |
| Lint | the rules of `Lint.py`, `Banned` with 7 lists | `banned-transform-write`, `Banned.TransformWriteFiles` (the eighth list), fixture `TransformDirectWrite` (25 fixtures) |
| Goldens | 56 on `nvidia-62x` | unchanged |
| Perf metrics | `BUDGETS` holds 4 (`Scripts/Lib/perf.py:49-50`) | `staticScenery/20000Entities/cpuMs` (2.0), `stateHash/20000Entities/ms` (no budget), in a new part `staticScenery` (then M47's `water`): 5 budgets |
| Fuzz targets, `AssetType`, `PlaySessionPhase`, state-hash blocks, formats | 53; as ADR 0058's ledger left them | none |

**Contended files and M43's regions** (M43 lands first; later landers adapt to these regions, as the ledger's "Contended files" rows give them):
- **Scene's transform files (M43's for the phase):** `TransformSystem.{h,cpp}` (`Update`); `Scene.{h,cpp}` (revisions, canonical order, the changed set, `HierarchyStaticTag`, the cache member; M50 owns `Scene` in phase 9).
- **`Session/PlaySession.cpp`:** the `State` helpers `UpdateTransforms` to `TagWritesOutsideSteps` (`:1218-1257`) and the tick counters; `FixedStep` and `RunFramePhase` unchanged but for counter reads.
- **`PhysicsSystem.cpp`** (M44 owns it for the phase): `HasHierarchyChanged` and `RecordHierarchy` (`:567-588`), `FindShapeChanges` (`:949-997`), `PreStep`'s owner loop (`:1497`), `PostStep`'s write-back loop (`:1702-1720`, the characters' part included), `AppendStateHash`'s order (`:2529-2571`), and `MarkTransformWritten` in `:269-294` and `:2365`. **`Scene/Private/PhysicsSystemState.h`** (M44's): the owner lists, first. **`Physics2DSystem.cpp`** (M43's alone): `:561-580`, its pre- and post-step loops (`:1447`, `:1641`) and `:271, 280, 2263`. The ledger records them, and M44, M47 and M48 merge over them.
- **`Scene/RenderExtraction.{h,cpp}`:** `ExtractRenderSnapshot`'s loop (`:754-945`) and `FindPrimaryCamera` (`:609-623`). M45's Sky and M46's Wind are component views after the loop, beside `snapshot.Fog` (`:947-949`); M46 adds `MeshDrawItem::Distance` in the MeshRenderer branch; M47 appends `WaterComponent` to `CollectCanonical`'s list after the terrains.
- **`Renderer/RenderSnapshot.h`:** `StaticSet` after `PlanarReflections`; M45, M46 and M47 append after it.
- **`Renderer/SceneRenderer.cpp`:** `PrepareViewDraws` (`:899-1093`), the casters' and selection's preparation (`:1816-1862`), the pick table (`:1426`), the instance upload (`:1539-1565`) and the forward and prepass binding sets (`:2217, 2234, 2389`), where M43 appends t21 and t22 last and M46 appends b8 after them (M45 and M47 add no item). `SceneRendererReflections.cpp:370` gains the same two items, M46's b8 after them. `Private/SceneRendererState.h`: the per-view `InstanceIndices` buffer and the static state, first.
- **`Renderer/ShadowPass.{h,cpp}`:** `PrepareCasters` (`:369-501`) and the three faces' cull and record (`:677-830`); M46 adapts.
- **Shaders:** `Scene.slang` (`:100-121`'s declarations, `:410-419`, the record reads), `Shadow.slang` (`:26`, `:62`, `:93`), `Terrain.slang` (declarations), `Shared/DrawConstants.h`, `Shared/InstanceRecord.h`; M46 adapts in `Scene.slang` and `Shadow.slang`.
- **Also:** `GpuResourceCache.{h,cpp}` (the static record buffer, first), `Private/ForwardPipelines.{h,cpp}` (t21 and t22 in the three layouts, M46's b8 after them), `RenderStats.{h,cpp}`, `SelectionPass.{h,cpp}`, `AudioSystem.cpp`, `UiLayout.cpp`, `AnimationSystem.cpp`, `ParticleSystem.cpp`, `SpriteAnimationSystem.cpp`, `BoneAttachments.cpp`, `ScriptEngineState.cpp`, `ScriptEngine.cpp`, `ScriptInstances.cpp`, `Bindings/Scene.cpp`, `Renderer/EditorOverlay.cpp` (the wireframe, `:338`), `Testing/FeatureTestRunner.cpp` (`GetExtractedPosition`, `:521`), `Scene/RenderAnnotations.cpp` (M45's file; the mesh loop only, `:175-193`), `Renderer/ProbeCapture.{h,cpp}` (M45's file; captures keep the static set, `MakeCaptureSnapshot`, `:282-319`) and `ShadersTests.cpp` (`InstanceIndex`, first of phase 8's four structs).
- **M43's alone in phase 8** (the ledger): the transform files, `PlaySession.{h,cpp}`, `Physics2DSystem.cpp`, the per-tick systems and script files above, `EditorOverlay.cpp`, `FeatureTestRunner.cpp`, `CoreRegistration.cpp`, and `Scripts/Lint.py` with `Tests/Data/Lint/`.
- **Shared, M43's entries first:** `BuiltinComponents.h`, `ProjectValidator.cpp` and its tests' count, `MakeValidationFixtures.py`, `Scripts/ModuleRules.json` (M43's `SnapshotHeaders` and `Banned` keys; M46's `ThirdParty` key after), `StatsMethods.{h,cpp}`, `StatsPanel.cpp`, `ScriptPerformanceStats.h`, `SessionStatistics.cpp`, `Bindings/Application.cpp`, `Scripts/Perf.py`, `Scripts/Lib/perf.py` and `test_perf.py`, FeatureTest's `Scaffolds.json` and `Suites.jsonl`; the generated references are regenerated, never merged.

## Acceptance

The Roadmap's M43 acceptance, each a named test or check:
- "TransformSystem: an incremental update equals the full recompute bit for bit after any sequence of edits" (a seeded op fuzz of writes, reparents, spawns, destroys and disables, about 2,000 entities)
- "TransformSystem: an update of a scene where nothing changed recomputes no matrix"
- "TransformSystem: a Transform written past change tracking fails the Debug full-recompute check" (Debug death test)
- "Scene: the structure revision moves with structural changes only"
- "Scene: canonical segments equal the full canonical order after any structural edit"
- "Scene: Static marks its subtree, its entity alone, or no part of a subtree"
- "PlaySession: an unchanged static scene visits no entity in a tick"
- "PlaySession: an entity written to the same bits in the frame phase is not tagged"
- "PhysicsSystem: a tick visits only the moving bodies and the bodies whose transforms changed"
- "RenderStaticCache: a write to a static entity rebuilds only its cell"
- "RenderExtraction: a static subtree's meshes are in the snapshot's static set, not its mesh list"
- "RenderExtraction: a scene without Static extracts as before"
- "ProjectValidator: SCENE_STATIC_MOVES reports a static entity that moves every tick"
- "StaticScenery: a scene renders the same bytes with its scenery static and without" (gpu)
- "StaticScenery: a view culls whole cells, so its static work follows what it sees" (gpu)
- "StaticScenery: each shadow face draws one draw per key of its visible static casters" (gpu)
- "StaticScenery: a moved static entity's cell rebuilds and its draw follows" (gpu)
- "StaticScenery: mirror views, captures, picking and selection draw static items" (gpu)
- "StaticScenery: the wireframe overlay and RENDER_NO_LIGHTING see static meshes" (gpu)
- "StaticScenery: a static scene renders the same bytes after a device loss" (gpu)
- `Lint.py --self-test` with the `TransformDirectWrite` fixture.
- Every existing golden byte-identical (56); every existing interpolation case and FeatureTest's `Timing` suite unchanged.
- `test_static_scenery.py`; the `StaticScenery` suite passes in the editor, Release and Dist; `StaticScenery.replay` verifies at one hash in Debug, Release and Dist.
- Perf.py's `staticScenery/20000Entities/cpuMs` within 2.0 ms at the gate's perf capture, and `stateHash/20000Entities/ms` recorded (warning-only).

**S16's "How the game will check it":**
- **Dense Landfall at 60 fps in a Release export, about 2 ms for 20,000 static entities in one view.** The gate: "an unchanged static scene visits no entity in a tick" and "a view culls whole cells". The measure: Perf.py's `staticScenery/20000Entities/cpuMs` (warning-only). The Release export's frame rate is the game's own check.
- **`DirectionalShadows` draws of the order of `ForwardOpaque`'s.** "Each shadow face draws one draw per key of its visible static casters", and `stats.get`'s pass rows, which the Python case reads.
- **Replays still verify; a static entity a script moves is handled.** "A moved static entity's cell rebuilds and its draw follows", the `StaticScenery` suite and replay in every mode, and every committed replay at its hash.

## Risks

- **A missed transform write path** draws stale and moves physics wrongly in every configuration alike, so the determinism suite cannot see it. The Debug full-recompute check, the Lint rule and the op fuzz cover it.
- **A forgotten view** draws nothing static. The byte-identity case covers the camera, prepass, picking and shadows; the views case covers mirrors, captures and selection; the filtered golden run covers every existing image.
- **Depth ties.** Within a key, static instances follow cell order, so exact depth ties between instances of one key could draw differently from the dynamic path. No existing golden holds `Static`; the byte-identity case's scene has no coplanar instances.
- **`PhysicsSystem.cpp` is M44's for the phase.** M43 lands first, with the small, named regions above, so M44 merges over them.
- **The Debug cost.** The checks recompute every matrix and rebuild the set after each pass, so Debug runs pay about today's cost again; Release and Dist pay nothing.
- **The renderer edit.** W43.2 is the largest edit of Prepare and `ShadowPass` since M42; its review checks the dynamic path line by line, and one session is at risk.
- **GPU memory.** 208 bytes a static record: 3.5 MB for 17,000 items. M49's scatter adds its own.
- **The ASan bound.** Unit cases stay small (about 2,000 entities); the setup shards the sanitized unit run first (ADR 0060 decision 3).
- **The Stats panel row** could reach `EditorDesignSupporting1600x900`'s capture; M42's row did not, and W43.3's filtered golden run checks it. The four rows phase 8 adds are checked together on the merged tree at the landing (the ledger).
- **The game's part.** The game marks each island's scenery root `Static`, keeps moving parts outside or under `NotStatic`, and measures its own Release export. These go to the Skerries session through the product owner.

## Before milestone completion

- [ ] W43.1 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W43.2 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W43.3 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] The landing: first in phase 8's batch, the merge review's findings fixed, FeatureTest regenerated, every golden byte-identical against main's build, `CI.py --require-clang-cl` green, main fast-forwarded and pushed.
- [ ] Architecture (§5.2 and §5.7 amended), the Roadmap, the skills, the Backlog (§2, §6, §8's row) and the generated references describe what landed.
- [ ] S16's "How the game will check it" met in the cases, suites and metrics above, and reported to the Skerries session through the product owner, with the corrections above.
