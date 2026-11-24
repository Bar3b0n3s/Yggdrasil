# 0064 — M46 Mesh LODs, draw distance and wind

- **Status:** accepted. M46's plan and milestone record, in phase 8 of the Skerries program (`Docs/Decisions/0060-skerries-engine-program.md`).
  - **The record.** M46's builder amends this record on `milestone/m46` as each wave lands, in "The W46.1 record" to "The W46.4 record", making decision 3 exact where it leaves a choice to the milestone. The lead's reviews and the landing follow.
  - Lead-owned.
- **Date:** 2026-11-24.
- **Decided by:** the lead (the milestone, its design defaults, its reservation needs and the research's questions as the product owner answered them on 2026-11-24, ADR 0060 questions 2, 3, 5 and 8), recorded by the lead.
- **Base:** main `3aaf4b10`, engine 0.8.0. Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone"; ADR 0023; ADR 0028 decisions 2, 10, 11, 12 and 17; ADR 0051;
  - ADR 0060 decisions 2, 4, 5, 6, 7 and 9, and its owner's answers; ADR 0061 (M43, which owns the renderer files M46 shares in phase 8);
  - ADR 0034 (M23: mesh version 2, instancing, Material version 2, the shared default stream); ADR 0046 (M31: the dithered fade and its Mask routing); ADR 0013 decision 27 (the `NoContraction` products); ADR 0010 (M6: the importers, MikkTSpace's vendoring); ADR 0059 (M42: the views a mesh draws in);
  - Architecture §2.2, §3, §5.3, §6.5, §6.8, §7.4, §7.5, §8.2 to §8.5, §8.7, §8.14 and §15.
- **Brief:** Skerries' S04 (`Projects/Skerries/Design/EngineTasks/S04-VegetationAndDrawDistance.md`, high priority), items 1 (mesh levels of detail, a cull distance) and 3 (wind). Item 2, the scatter, is M49 in phase 9 (ADR 0060 decision 2).

## Context

- **The brief.** Islands 0.5 to 2 km apart must show each other's trees at a sane cost, and plants must bend more as the storm comes in. It asks for levels authored in the glTF (`MSFT_lod` or a naming convention) or generated at import, switched by screen size with a dithered cross-fade, shadows from a coarser level and a cull distance per renderer; and a scene wind (direction, strength, gusts) that bends vegetation by vertex animation, read by scripts and later by particles (S11).
- **The research.** `bin/Reviews/P8/research/scale.md`, "S04" (its first milestone, "S04a": designs A to C, sizes, risks) and "Across the group" (the "parallel, same phase" option against S16, which is M43).
- **What the engine has today** (each fact checked at `3aaf4b10`):
  - **A mesh draws one level.**
    - The cooked mesh is version 2 (`Engine/Source/Engine/Asset/MeshData.h:139-141`): one vertex and one index buffer, submeshes of one index range each (`:114-123`), optional per-vertex streams whose higher bits stay reserved (`:55-64`).
    - `GltfImporter` is version 3 and reads no level (`AssetPipeline/Importers/GltfImporter.h:180`). cgltf keeps a node's unknown extensions and extras as raw JSON (`Vendor/cgltf/cgltf.h:268-271`, `:688-711`) and parses an attribute named `_…` as a custom one (`:2864-2869`), so the importer can read `MSFT_lod` and an application attribute itself.
    - Only terrains have levels (Architecture §8.3, line 1212). Physics, the navigation bake and CPU raycasts read a mesh's whole index buffer (`Scene/PhysicsComposition.cpp:412`, `Scene/NavigationBakeInput.cpp:177`, `Scene/SceneRaycast.cpp:313-315`).
  - **Draw distance is the camera's.** Culling is the frustum plus `FarClip` (`Renderer/RenderPrepare.h:32-36`), 1,000 m by default (`Scene/Components/CameraComponent.h:34`); directional shadows stop at `ShadowDistance`, 100 m by default (`DirectionalLightComponent.h:18`). `MeshRenderer` holds five fields (`MeshRendererComponent.h:13-20`), each written in every scene (`Projects/RollingBall/Assets/Scenes/Level1.scene:219-225`), and the state hash hashes the canonical scene (`Session/PlaySession.cpp:2214`): a new MeshRenderer field would move every committed hash.
  - **The dithered fade.** M31's `Opacity` clips by a 4 × 4 ordered dither moved with TAA's 8-frame cycle (`Resources/Shaders/Passes/Scene.slang:320-335`, `:497-506`), and an Opaque material takes the Mask variants while it fades (`RenderPrepare.h:160-166`). One opacity per draw cannot cross-fade two levels, whose pixels must be complementary.
  - **Instancing.** Every mesh draw is instanced; a record is 208 bytes with two free words, `Padding1` and `Padding2` (`Resources/Shaders/Shared/InstanceRecord.h:37-60`, `:55-56`). Draw flags 1 to 16 are taken (`Shared/DrawConstants.h:28-40`).
  - **Prepass and forward agreement.** The forward pass tests against the prepass's depth without writing it, so every pipeline computes bit-identical positions through `NoContraction` products (`Scene.slang:244-255`, `Selection.slang:44-52`; Architecture §8.3 line 1216).
  - **Where the vertex stage sees what.** The Scene program's sets are visible to every stage (`Renderer/Private/ForwardPipelines.cpp:36`); its set 0 takes b0 to b5, the push constants at b6 and M42's b7 (`Private/ForwardPipelines.h:55-61`, `:105`). The Shadow and SelectionMask programs bind no material set in their Opaque variants and bind it to the pixel stage only in their Mask variants (`Renderer/ShadowPass.cpp:30-66`, `Renderer/SelectionPass.cpp:50-80`), and Opaque casters batch across materials (`ShadowPass.cpp:421-425`, `:478`). Every program reads the instance record in its vertex shader (`Scene.slang:410-441`, `Shadow.slang:62`, `Selection.slang:58`).
  - **Nothing moves in the wind.**
    - No wind component or constant exists, and `COLOR_0` always tints the base colour (`MeshData.h:40-45`).
    - MaterialParameters animate at the request's `SimulationTime`: the world clock's rendered time in play, 0 at edit time and in bakes (`Scene/RenderExtraction.h:111-116`; Architecture §8.3 line 1206).
    - Scene singletons are `UniquePerScene` and extracted as the first effectively enabled one (`Scene/Registration/FogRegistration.cpp:40-45`, `Scene/RenderExtraction.cpp:863-866`).
    - A script's world time is `Tick − WorldHeldTicks` fixed steps (`Scripting/ScriptHost.h:48-64`); component proxy methods exist (`Scripting/Bindings/Terrain.cpp:19-23`); `RegisterBindings.cpp:57` ends with `RegisterJoints`.
  - **Material is version 2** (`Asset/MaterialData.h:60-66`).
    - `project.upgrade` re-saves every `.material` through its reader and canonical writer (`Editor/Source/EditorCore/Automation/ProjectMethods.cpp:203-209`) and every `.meta` as parsed (`:187-191`), so a new import setting rewrites no `.meta`.
    - The 140 tracked game and test-project materials are all version 2: FeatureTest 118, RollingBall 11, Breakout 8, NativeTest 3. The games suite fails a project the upgrade dry run would rewrite (`Scripts/Lib/games.py:785-794`), and `Editor --upgrade` transcribes its writes (`EditorCore/Automation/ProvenanceRecorder.h:31-32`).
  - **The cache.** A `.meta`'s `ImporterVersion` is informational (`Asset/AssetMetadata.h:76-78`), and an artifact of another payload format version is discarded and rebuilt (Architecture §7.5, line 1056): cooked meshes and materials are cache artifacts, and none is committed.
  - **The vendoring model** is MikkTSpace, the glTF importer's other library: a static library with the precise floating-point model, linked by AssetPipeline alone and included by one first-party file (`Vendor/MikkTSpace/VENDOR.md`, `Dependencies.lua:409-418`, `Scripts/CheckBuildConfig.py:211-213`, `Scripts/ModuleRules.json:447-467`). A library the Engine project references ships attributed (`Scripts/Lib/notices.py:16-23`).
- **Corrections to the brief** (to the Skerries session through the product owner at phase 8's end; ADR 0060 decision 9):
  - **Entity cap.** 65,536 is the default of `Simulation.MaxEntities` (`Project/ProjectSettings.h:45`), not a hard limit.
  - **Far clip.** A camera's default `FarClip` of 1,000 m culls the islands beyond it: raise it to 3,000 m. Reverse-Z with an infinite far plane keeps depth precise (Architecture §8.3 line 1147). Directional shadows still stop at `ShadowDistance`, so far trees cast none.
  - **Vertex colour.** `COLOR_0` always tints, so wind weights have an attribute of their own, the glTF application attribute `_WIND`. A mesh without it still bends by height.
  - **The dither.** A cross-fade needs complementary coverage, which `Opacity`'s dither alone cannot give: M46 adds a level fade whose two halves split exactly the pixels a level keeps, and which composes with `Opacity`.
  - **Skinning.** Morph targets deform vertices too (Architecture §8.3 line 1208). Minor.
  - **Cull distance per renderer.** It is a `RenderDistance` component beside the MeshRenderer, since a MeshRenderer field would move every committed hash.
  - **Readers of the wind.** Scripts read it with `Wind:Sample(position)` at the world's clock; particles from M61 in phase 11. In phase 8, M45's clouds and M47's water keep their own drift fields; following the scene wind is a later row.
  - **Shadows** come from one level, by default one coarser than the view's (`ShadowLodOffset` 1), and never cross-fade.
  - **Leaf cards.** meshoptimizer's simplifier cannot merge disconnected leaf cards: author a tree's far levels (a cross-quad last level), for example as `_LOD<n>` objects from Blender.

## Decisions

### 1. Scope

S04's items 1 and 3, as one milestone M46 in phase 8, fourth in its fixed train order, after M43, M44 and M45 (ADR 0060 decision 2). It delivers:
- mesh levels in the cooked mesh, authored in the glTF or generated at import by meshoptimizer, chosen per view by screen size, cross-faded with complementary dither, with shadows from a coarser level;
- a `RenderDistance` component: a cull distance, a level bias, a shadow offset and a fade width per renderer;
- a scene `Wind` component, a `_WIND` vertex attribute, Material version 3's wind block, displacement identical in every pipeline, and `Wind:Sample`;
- stats, a perf metric, two goldens and FeatureTest coverage in every mode.

What M46 leaves out is decision 3's "Out", recorded in `Docs/Backlog.md` §2 at landing; the limits it accepts go to §6.

### 2. Structure: one milestone, four waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 8 | M46 | Mesh LODs, draw distance and wind | S04 items 1 and 3 | fourth in phase 8's fixed train order (M43, M44, M45, M46, M47, M48), after M43, M44 and M45 (ADR 0060 decision 2) | the batch's `CI.py --require-clang-cl` | ~8 (W46.1 ~3, W46.2 ~3, +1 at risk, W46.3 ~1.5, W46.4 ~0.5) |

One builder, on `milestone/m46` in `D:/Engine/.claude/worktrees/m46` from main `3aaf4b10`, works the waves in order:
- **W46.1, the data, disjoint from M43:**
  - meshoptimizer vendored (decision 5); cooked mesh version 3 and its fuzz seeds;
  - `GltfImporter` version 4: authored and generated levels, `_WIND`, the `Wind` setting, `ASSET_LOD_INVALID` and its fixtures;
  - Material version 3 with its migration fixtures, then `project.upgrade` of RollingBall, Breakout and NativeTest and FeatureTest regenerated, so the tree stays green;
  - the `RenderDistance` and `Wind` components, `Scene/Wind.{h,cpp}` and `Wind:Sample`; the pure selection rules (`Renderer/MeshLod.{h,cpp}`); their unit cases.

  It touches none of M43's files (`RenderSnapshot.h`, `RenderExtraction`, `SceneRenderer.cpp`, `ShadowPass`, `GpuResourceCache`, `Scene.slang`, `Shadow.slang`). Its first checkpoint fixes, M34's way, what a contract would freeze: both components' fields, the mesh version 3 layout, Material version 3's block and the import settings.
- **W46.2, the renderer, before M43 lands:** the snapshot members and their extraction; `GpuMesh`'s levels and wind stream; `WindConstants` and `Common/Wind.slang`; the displacement in the Scene, Shadow and SelectionMask programs; the fade's clip; the per-view choice in Prepare, the casters and the selection mask; the counters; device loss; the gpu cases; a filtered golden run proving every existing golden byte-identical. Its hunks in M43's files stay small (call sites of `MeshLod`, `Common/Wind.slang` and `Common/LodFade.slang`), so W46.4 re-applies them.
- **W46.3, coverage, tools and documents:** `stats.get`'s fields, the Stats panel and the readout; Perf.py's metric; FeatureTest's five scaffolds; the Python cases; Architecture, the Backlog, the skills and the generated references.
- **W46.4, the adaptation at landing** (the research's "parallel, same phase"): once `landing/phase8` holds M43, M44 and M45, the builder merges `landing/phase8` into `milestone/m46` and adapts M46 to M43's static path (decision 3E, last item), with its own review and filtered runs; then the lead merges `milestone/m46` into `landing/phase8`.

The size is the program's ~7 for W46.1 to W46.3 plus W46.4, which ADR 0060 folds into "adapts at landing".

### 3. Design defaults: M46 Mesh LODs, draw distance and wind

**What M46 adds:** two components in two new categories; a cooked mesh, a Material and an importer version; a `RenderSnapshot` and a `MeshDrawItem` member; one draw flag and the record's two free words; one shared constant buffer and one vertex binding; one validation code, one proxy method, stats fields and two readout members; two goldens, one perf metric and one vendored library.

**What it does not add:** a namespace, a settings section, an `AssetType`, a built-in handle, a `PlaySessionPhase`, a render stage or pass, a pipeline, a program, a fuzz target, an automation method, an `IScriptHost` member, a callback, a state-hash block or a module.

**A. Levels in the cooked mesh (MeshData version 3).**
- **Level 0 stays exactly today's** `Vertices`, streams, `Indices`, `Submeshes` and `Bounds`, so physics, the navigation bake, raycasts, CPU picking and the tangent generator read what they read today.
- **The new members:**
  - `Lods`: up to `MaxMeshLods − 1` = 4 levels after level 0, each with its screen size and one `{IndexOffset, IndexCount}` per submesh into `LodIndices`;
  - `LodVertices`: the authored levels' own vertices, with their attribute and wind streams when the mesh has them; a generated level indexes level 0's vertices only;
  - `Wind`: one `MeshVertexWind` (4 × unorm8) per vertex, the stream `MeshStreams::Wind` (bit 3, append-only).
- **The payload:** version 2's layout, its `Reserved` word becoming `LodCount`, the wind section after the morph section, then the levels' section. The builder writes it in `MeshData.h`'s comment at W46.1's first checkpoint.
- **Validation** (`ValidateMeshData`, also on untrusted data): level indices below the vertex count plus `LodVertices`; a skinned or morphed mesh's levels index-only; whole-triangle ranges inside `LodIndices`; screen sizes strictly decreasing in (0, 1]; the wind stream empty or one per vertex.
- **No reader keeps version 2:** the format check and `GltfImporter` 4 rebuild every cooked mesh (M23's rule, `MeshData.h:139-140`).

**B. Import (`GltfImporter` version 4).**
- **Five settings** after `AnimationEvents`; a `.meta` without them reads their defaults, so no `.meta` is rewritten:
  - `Lods`: `GltfLodSource` `None` (default), `FromGltf` or `Generate`;
  - `LodScreenSizes`: [0.5, 0.25, 0.125, 0.0625], where level k+1 takes over from level k;
  - `LodTriangleRatios`: [0.5, 0.25, 0.125, 0.0625], each generated level's share of level 0's triangles;
  - `LodMaxError`: 0.05, meshoptimizer's error bound relative to the mesh's extent;
  - `Wind`: a `MaterialWind` (H), disabled, given to every material the file imports.
- **`FromGltf`:** a node's `MSFT_lod` extension names its levels by `ids`, and its `MSFT_screencoverage` extras, when present, replace the screen sizes; a node named by `ids` is a level and is never placed. Otherwise sibling nodes `<name>_LOD<n>` (n from 0, contiguous) form one group: the `_LOD0` node is placed as `<name>`, the others are its levels.
- **`Generate`:** meshoptimizer's `meshopt_simplify`, per submesh, over positions, with `meshopt_SimplifyLockBorder` so submesh seams stay closed; each level from level 0 at its ratio within `LodMaxError`, index-only.
- **A level is dropped with `ASSET_LOD_INVALID`,** the import still succeeding, when: its submesh count or material slots differ from level 0's; it is a fifth level or later; it leaves a gap in the `_LOD<n>` names; it is an authored level of a skinned or morphed mesh; it is generated and keeps more than 90 % of its predecessor's triangles; or `FromGltf` meets `MergeMeshes` (`Generate` works on the merged mesh).
- **`_WIND`:** a VEC4 of float or normalized unsigned byte or short, clamped to [0, 1] and stored as unorm8: x the branch sway, y the branch's phase, z the flutter, w the bend mask (1 holds the vertex still). Where only some primitives carry it, the others' vertices take (0, 0, 0, 0).
- **Without levels and `_WIND`,** every mesh's level 0 equals version 3's import byte for byte (a unit case against checksums committed at the base). Sub-asset keys are unchanged.

**C. `RenderDistance`, version 1, in a category `RenderDistance` of its own** (MaterialParameters' precedent), appended after M44's Vehicles (then `Wind`'s, then M47's Water) so no category gains a mid-list entry. It requires `MeshRenderer`, is script-visible through its generated proxy, and governs the MeshRenderer on its entity:
- `CullDistance`: float, m, default 0 (none: only `FarClip` culls), [0, 100000]. Beyond it, from the view's camera, the item draws and casts nothing.
- `LodBias`: float, default 1, [0.01, 100]. It multiplies the screen size before the switches compare it: 2 keeps each level until half its switch size.
- `ShadowLodOffset`: integer, default 1, [0, 4]. A caster draws its view's level plus this, clamped to the last level.
- `FadeWidth`: float, default 0.1, [0, 0.5]. The band around each switch and before `CullDistance`, as a share of the value; 0 switches sharply.

A MeshRenderer without the component takes these defaults, so a mesh with levels switches without it.

**D. Selection per view (`Renderer/MeshLod.{h,cpp}`, pure, unit-tested).**
- **The screen size** is the projected diameter of the item's bounding sphere (level 0's bounds through `World`, its largest axis scale) over the view's height: `r × P[1][1] / d` in perspective, d the camera's distance to the sphere's centre, and `r × P[1][1]` in orthographic.
- **The level** is the finest whose switch size the biased screen size still reaches. Inside a `FadeWidth` band around a switch both levels draw, the coarser with a fade t in (0, 1) that rises as the item shrinks, the finer with −t. Before `CullDistance`, the drawn level fades out across the last `FadeWidth` with −t; beyond it the item is culled.
- **Stateless:** no hysteresis and no per-view history, so captures and goldens are reproducible. Each view chooses by its own camera: the editor's views, captures, M42's mirror views and probe captures.

**E. Drawing.**
- **The fade** rides in the record's first free word, `InstanceRecord::LodFade` (was `Padding1`), signed, 0 for none. The Mask variants' fragment shaders (`Common/LodFade.slang`, called from `GetCoverage`) keep a pixel where `threshold < t` for t > 0 and where `threshold ≥ −t` for t < 0, with `Opacity`'s threshold (`GetFadeThreshold`, `Scene.slang:327`). So the two levels split exactly the pixels a fading item's `Opacity` keeps, and TAA's cycle resolves the cross-fade. The prepass, the forward passes and the picking prepass evaluate the same clip; a Blend material clips by the same rule. The fade takes no draw flag: a static record (W46.4) is shared by every view.
- **Pipelines and batches.** `SelectMeshAlphaMode` gains the draw's fade: an Opaque material takes the Mask variants in the camera's passes while its draw fades, so no pipeline is added. `OpaqueSortKey` and `MeshInstanceKey` (frozen by M8 and M23; a reviewed change) gain `Lod`, so two levels never share an instanced draw; draws without levels sort and batch as today.
- **Shadows** (`ShadowPass::PrepareCasters`, `ShadowPass.cpp:369-501`, which gains the view's camera): each caster draws the view's level plus `ShadowLodOffset`, never faded, and casts nothing beyond its `CullDistance`. M29's caster signature gains the level and, for a wind caster, the wind's phases, so a cached spot or point map holding a wind caster re-renders each frame.
- **The selection mask** outlines the level that holds more of the pixels; picking goes through the picking prepass's own clip.
- **The GPU mesh** (`GpuResourceCache`): `GpuSubmesh` gains its levels' ranges into the one index buffer (level 0's indices first) and bounds covering every level; `GpuMesh` gains the screen sizes. Vertex binding 3 binds the wind stream, or the cache's shared default wind stream of zeros, grown as M23's default attribute stream is (`GpuResourceCache.h:46-49`). All 36 mesh pipelines take binding 3 (the Scene program's 24, the six Shadow and six SelectionMask pipelines), and every vertex entry reads it.
- **M43's static path (W46.4, on M43's landed code).** M46's per-view choice lives in M43's per-view index entries. M43's record (ADR 0061, its decisions 3 and 11) gives each entry an 8-byte `InstanceIndex {Record; Reserved}` whose `Reserved` word phase 8's ledger keeps for M46, read through its one load function in `Common/Instances.slang`. W46.4:
  - writes each static entry's signed fade into `Reserved` (the dynamic records keep `LodFade`), and the load function returns it beside the record;
  - adds `Lod` to M43's per-view static grouping key (pipeline, material, mesh, submesh), so one indexed draw per key and level; the static casters' keys per face and their M29 signatures take the level too, and a face whose visible static casters hold a wind caster adds the frame's wind phases to its hash, as the dynamic casters do (above);
  - adds each cell version's largest `CullDistance` to M43's per-cell data, fed from `MeshDrawItem::Distance`, so a view culls a cell beyond it before its items;
  - writes `WindResponse` and `DrawFlagWind` into the static records once per cell version, and the view's level into the record M43 copies for a selected static item;
  - adds `RenderDistance` to the signals M43's `RenderStaticCache` rebuilds a cell on, and counts static items in M46's counters.

  Should M43's landed entry leave no room for the fade, a static item inside a band draws that frame as two dynamic records, which needs no entry bits.

**F. The `Wind` component, version 1, in a category `Wind` of its own** (Fog's precedent). It is `UniquePerScene`, so a second Wind is refused on every write path and a file with two is `SCENE_DUPLICATE_UNIQUE_COMPONENT`. Script-visible. M46 updates `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s description (`ProjectValidator.cpp:238`) to name Environment, PostProcess, Fog, Sky and Wind. Fields:
- `Direction`: vec3, default (1, 0, 0), where the wind blows, in world axes; only its horizontal part counts, normalized when used. A zero horizontal part is refused by the component's `Validate` rule with M42's `Normal` treatment (the length in double; the `Generate` hook turns a random zero into the default).
- `Strength`: float, m/s, default 5, [0, 100]; a gale is about 20.
- `GustStrength`: float, m/s, default 3, [0, 100]: the gusts' added speed at their peak.
- `GustFrequency`: float, Hz, default 0.2, [0, 10]. Gusts travel with the wind, so their wavelength is `Strength / GustFrequency`.
- `Turbulence`: float, default 0.25, [0, 1]: how much gusts vary across positions and turn the wind sideways.

**Sampling (`Scene/Wind.{h,cpp}`).** `SampleWind(const WindComponent&, glm::dvec3 position, double time)` gives the wind's velocity in m/s: the gusted speed along the direction plus the turbulence's sideways part, each gust a smoothed triangle wave (`frac`, `abs` and a cubic), in double precision with IEEE-exact operations and DetMath (the simulation path's rule), so every configuration gives the same bits. `FindSceneWind(const Scene&)` returns the effectively enabled Wind. These are the interface later readers take: M45's clouds and M47's water once a later row couples them (in phase 8 they keep their own drift fields), and M61's particles in phase 11.

**G. Wind on the GPU.**
- **The snapshot.** `RenderSnapshot::Wind`, a `RenderWind` (`Enabled`, the five fields, `Time` = the request's `SimulationTime`), after M43's `StaticSet` and M45's `Sky` (`RenderWind` before `RenderSnapshot`, after M45's `RenderSky`), extracted by a component view after the canonical loop, beside `snapshot.Fog` (`RenderExtraction.cpp:947-949`), after M45's Sky. A world pause holds it; edit-time images and bakes show time 0, the lean without the sway.
- **`Shared/WindConstants.h`:** the direction, strength, gust strength, turbulence and gust wavelength; the gust, branch and flutter waves' phases in [0, 1), computed on the CPU in double precision so no float time loses precision in a long session; and the previous render's values, kept by `SceneRenderer::State` and equal to the current ones after a history reset. Bound at b8 of set 0 in every mesh layout: the Scene program's prepass, overdraw and forward layouts (after M42's b7 and M43's t21 and t22), both Shadow variants and SelectionMask (b8 is free in each), so `Common/Wind.slang` declares one register. `Terrain.slang` and M47's `Water.slang` declare it unread, since they share the forward layout. The builder records the layout.
- **The response** rides in the record's second free word, `InstanceRecord::WindResponse` (was `Padding2`): the material's `Bend`, `Branch`, `Flutter` and `Stiffness` as unorm8 over their ranges, marked by `DrawFlagWind`, both written by Prepare from the material (`GpuMaterial` keeps the block). So Opaque casters keep their pipelines and batches, and no material set changes visibility.
- **The displacement** (`Common/Wind.slang`, one function every vertex entry calls) is added in world space after the `World` product and before the view-projection product:
  - **bend:** every vertex leans along the wind by `Bend` times a response to the speed times its squared height above the entity's origin, less its bend mask, and drops so its distance from the origin is nearly kept;
  - **branch sway:** `_WIND`.x times `Branch` times a wave at the branch phase plus `_WIND`.y;
  - **flutter:** `_WIND`.z times `Flutter` along the vertex normal, at the flutter phase.

  The response is `speed / (speed + Stiffness × 10 m/s)`, so a calm wind gives an offset of exactly zero. The builder fixes the curves against the golden.
- **Bit-identical positions.** The displacement uses only +, −, ×, `frac`, `abs`, `min` and `max`, every product and sum `NoContraction` (written once, as `MultiplyExact` is), and no transcendental function. VSMain, VSPrepass, VSPicking, VSOverdraw, the casters and the selection mask therefore compute the same bits, and the forward pass's `GreaterOrEqual` test keeps exactly the prepass's surface.
- **Motion vectors.** The prepass's previous position takes `PreviousWorld` and the previous wind, so TAA follows the sway.
- **What is displaced:** positions only, normals and tangents keeping their rest direction; a skinned mesh after its deformation. Terrains, sprites, text and particles are not displaced.

**H. Material version 3.**
- **The block:** a registry struct `MaterialWind` as the field `Wind`, after `UVOffset`: `Enabled` (false); `Bend` ([0, 1], 0.1: about `Bend` m of lean at 1 m of height in a 10 m/s wind); `Branch` ([0, 1] m, 0.1); `Flutter` ([0, 0.25] m, 0.02); `Stiffness` ([0, 1], 0.5: how much of the wind the plant resists).
- **Versions:** 1 and 2 read with wind disabled and save as 3; the cooked material goes to 3. Materials are not in the state hash.
- **Fixtures:** `Tests/Data/Formats/Material/MaterialV1.upgraded.material` becomes version 3, its case keeping its behaviour as "MaterialData: MaterialV1 fixture loads with UV0 sets and saves as the current version"; `MaterialV2.material` and `MaterialV2.upgraded.material` are added.
- **The upgrade** (W46.1): `Editor --headless --project <P> --upgrade` rewrites the 22 materials of RollingBall, Breakout and NativeTest as transcribed `project.upgrade` writes, M20's way. `Scaffold.py` rewrites FeatureTest's 118, and the lander regenerates FeatureTest once more on the merged tree.

**I. Validation: `ASSET_LOD_INVALID`** (Warning, not auto-fixable; an import diagnostic). It reports B's dropped levels, each naming the node or mesh and the reason. It is appended at the end of both of `ProjectValidator.cpp`'s tables (`:115-225`, `:230`) after M45's codes and to `ImportDiagnosticCodes` (`:522-528`); its constant `AssetLodInvalidCode` lives in `Asset/AssetDiagnostic.h` beside the other `ASSET_*` codes, and `GetAssetDiagnosticCodes()` (`Asset/AssetDiagnostic.cpp:12`, 13 codes) lists it, so a reimport clears it; `ProjectValidatorTests.cpp`'s count line takes it in its constant (59 → 60). Its fixture `Tests/Data/Validation/ASSET_LOD_INVALID/` comes from `MakeValidationFixtures.py` with a glTF from `MakeGltfFixtures.py`, and it gets §13.7's and the game-debugging skill's `validation-codes` rows. No wind code: `UniquePerScene` already refuses a second Wind.

**J. Scripts, automation and the editor.**
- **Scripts.** The generated `RenderDistance` and `Wind` proxies read and write every field (gate 4). One proxy method, `Wind:Sample(position: vector, time: number?): vector`, in `Scripting/Bindings/Wind.cpp`, registered by `RegisterWind` after M44's `RegisterVehicles`: its time defaults to `Time.GetTime()`, the world's clock, and it reads in read-only evaluations. No namespace. `PerformanceStats` gains two members (K).
- **Automation.** No method: 126 methods and 86 MCP tools stay. The components go through `entity.create`, `entity.update` (in play too), `edit.batch`, `component.schema` and `prefab.*`; the import settings through `asset.import {settings}` and the existing setting methods; `stats.get`'s view summary gains three fields (K).
- **The editor.** The generic inspector, in Add Component's `RenderDistance` and `Wind` groups; the material inspector shows the block; the asset inspector shows the import settings. No wind gizmo and no level readout (§2).

**K. Stats and perf.**
- **`RenderStats`** appends, after M43's and M45's fields: `MeshLodDraws` (draws per level, five counts, both draws of a band counted), `LodCrossFades` (items drawn in a band) and `DistanceCulledMeshes` (items beyond their `CullDistance`). They are deterministic for a fixed camera, so the gpu cases gate on them.
- **`stats.get`'s view summary** (`Engine/Source/Engine/Automation/Methods/StatsMethods.cpp`) appends `meshLodDraws`, `lodCrossFades` and `distanceCulledMeshes` after M43's and M45's fields, each described. The Stats panel gains "Mesh LODs: per-level draws / cross-fades / distance-culled" after M42's row (`StatsPanel.cpp:172`) and M43's and M45's rows. `PerformanceStats` appends `LodCrossFades` and `DistanceCulledMeshes` after M45's `SkyUpdates`.
- **Perf.py** gains `lod/Forest/gpuMs` in the `look` part, warning-only, budget 3.0 ms, with `test_perf.py` cases: a generated `Assets/Perf/Forest.scene` at 1920 × 1080 with 2,000 instances of a tree with authored levels and `_WIND` under a gale and 4,000 rocks with generated levels over a 1 km plane, lit by a directional light with cascades; the metric is the sum of the game view's `DepthNormal`, `Shadows` and `ForwardOpaque` rows.

**L. Device loss and memory.** The levels' ranges and the wind stream live in `GpuMesh` and the shared default wind stream in `GpuResourceCache` (the placement rule's third place); the current and previous `WindConstants` buffers in `SceneRenderer::State` (the second). The existing Release, Recreate and `TrimUnused` cover them, and creations after startup go through `RetryAfterTrim`. One `Test::RunDeviceLossTest` case on a scene with levels and wind (identical bytes). A generated level adds indices only; an authored one its vertices.

**M. FeatureTest** (IdSeeds 4301 to 4305, in M46's block 4300-4399 of phase 8's ledger; every gate in each mode the feature supports).
- **`MeshLods.jsonl`** (4301) imports from `Scaffold/Sources/Gltf/` (generated by `MakeGltfFixtures.py`) `LodTree.gltf` (`MSFT_lod`, screen coverage, `_WIND`), `LodRock.gltf` (`_LOD<n>` nodes) and `LodSphere.gltf` (imported with `Generate`). It makes `MeshLods.scene`: instances along the Primary camera's view, each with a `RenderDistance`, every field non-default across them, one inside a band and one beyond its `CullDistance`. Its suite `MeshLods` reads and writes both proxies' fields and, where `Application.GetStats().RenderAvailable` holds, checks `LodCrossFades` and `DistanceCulledMeshes`.
- **`Wind.jsonl`** (4302) makes `Assets/Materials/Wind/Leaves.material` (its block non-default) and `Wind.scene`, a `Wind` with every field non-default over the trees. Its suite `Wind` checks `Wind:Sample` at three points and times against committed values (the same in every mode), every field through the proxy, and that a world pause holds `Time.GetTime` and so the sample.
- **`WindReplay.jsonl`** (4303) records `Assets/Tests/Replays/Wind.replay`: input raises the wind's `Strength`, and a `Kite` behaviour pushes a body by `Wind:Sample` each fixed step, so the wind reaches the hash. It verifies in Debug, Release and Dist, and fails without its input.
- **`GoldenMeshLods.jsonl`** (4304) and **`GoldenWind.jsonl`** (4305), golden scaffolds (`GoldenDirectories` below `Assets/Scenes/` and `Assets/Materials/`): `Golden: MeshLods` (an authored and a generated mesh at three distances, one mid-band, one beyond its cull distance, a shadow from the coarser level) and `Golden: Wind` (a gale at time 0 over wind-weighted plants and trunks, beside a mesh without wind).
- **Gates:** 1 (both components, every field leaf non-default); 2 (`Wind:Sample`); 4 (both proxies' fields); 6 (the code's fixture); 7 (every description); 9 (mesh and Material version 3 cook, load and render in the exported runs through the new scenes; no new extension); 1b, 3, 5 and 8: nothing new. The soak gains the new scenes.

**N. Documents.**
- **Architecture:** §2.2 (the `Use` table, the precise floating-point list); §3 (AssetPipeline's third party); §5.3 (two rows); §6.5 and §6.8 (Material and cooked mesh version 3); §7.4 (`GltfImporter` 4); §8.2 (the members); §8.3 ("Mesh levels" and "Wind" paragraphs; the record and the keys under "Instancing"); §8.4 (`WindConstants`, binding 3, the record's words, `DrawFlagWind`); §8.7 and §8.10 (casters, selection and picking); §8.14 (the default stream); §13.7 (the code, the stats); §15.5 (the scenes).
- **The Roadmap's M46 section; `Docs/Backlog.md` §2 and §6;** the header comments of `MeshData.h`, `MaterialData.h`, `GltfImporter.h`, `RenderSnapshot.h`, `RenderPrepare.h`, `InstanceRecord.h`, `DrawConstants.h` and `RenderStats.h`.
- **The skills:** game-building's `look` page (`FarClip` 3,000; levels at import; `RenderDistance`; the wind recipe: the import's `Wind` setting or a `.material` block, `_WIND` from Blender, authored far levels for leaf cards); game-debugging (the stats fields, the readout members, the code's row); golden-images (the scene list). The generated references.

**Out** (`Docs/Backlog.md` §2):
- impostors for far trees (question 5);
- per-level materials; authored levels of skinned or morphed meshes; sloppy or attribute-aware simplification;
- hysteresis or a temporal fade at a switch; a project or display-quality level bias;
- a cull distance for terrains, sprites, text, particles and lights;
- wind on terrains and water (M47 later), particles (M61) and cloth; bent normals;
- the sway in edit scenes; local wind zones; wind from physics (rotor wash, blasts); a wind gizmo;
- a readout of a mesh's levels (`asset.info`, the asset inspector) and a level debug view;
- CPU picking and raycasts against displaced or coarser geometry.

**Accepted at landing** (§6):
- Shadows come from one level, by default one coarser than the view's, and never cross-fade.
- Colliders, the navigation bake, raycasts and CPU picking use level 0 without wind.
- Wind displaces positions only.
- A wind caster's spot and point maps re-render every frame.
- Edit-time images show the wind at time 0.
- Inside a band, the selection outline follows one level.

### 4. The research's questions, decided

1. **LODs generated at import** (question 4): **yes**, through meshoptimizer beside authored levels, approved by the owner on 2026-11-24 (ADR 0060 question 2). Generated levels are index-only, sharing level 0's vertices, so they cost indices alone. Fallback: authored levels only.
2. **Impostors** (question 5): **not now.** A 10 m tree at 1 km is about 9 px tall at 1080p, and a coarse level with fog suffices. A §2 row.
3. **The render thread and GPU-driven rendering** (question 3): **kept cut.** M46 chooses levels per view on the CPU, O(visible items), and O(visible cells) after W46.4.
4. **Sequential or parallel with S16:** **parallel, same phase** (ADR 0060 decision 2). W46.1 is disjoint from M43, and W46.4 adapts at landing.
5. **Where the research left a choice:**
   - a second Wind is **refused** (`UniquePerScene`, as the Fog) rather than reported by a `WIND_MULTIPLE` warning, since every scene singleton follows that rule;
   - the fade is **signed and flagless**, since a static record is shared by every view;
   - the wind's **response rides in the record**, so Opaque casters keep their pipelines and batches;
   - the **bend comes from height** for every vertex, and `_WIND` weights only the branch sway and the flutter and can mask the bend;
   - the import code is **`ASSET_LOD_INVALID`**, joining the importer's `ASSET_*` family, rather than `MESH_LOD_INVALID`;
   - the script read is a **proxy method**, not a `Wind` namespace, so no namespace is added.

### 5. Approvals

meshoptimizer is approved (ADR 0060 decision 5 and question 2): MIT, from the official upstream `github.com/zeux/meshoptimizer`; the fallback is authored levels only. The lead records its `Docs/Decisions/0001-approvals.md` row at the setup. The builder's `Vendor/meshoptimizer/` duties, MikkTSpace's model:
- **The pin:** the latest release tag at W46.1, its commit and the SHA-256 of every kept file (UNVERIFIED until then: the tag and the file list). `LICENSE.md` committed verbatim.
- **The files:** `src/` kept unmodified; `gltf/` (gltfpack), `js/`, `demo/`, `tools/` and the CMake files removed; no local modification.
- **The build** (`Vendor/meshoptimizer/premake5.lua`): a static library, precise floating point without contraction (MSVC `floatingpoint "Default"`; GCC and Clang `-fno-fast-math -ffp-contract=off`, clang-cl through `/clang:`), no defines, the compiler's default warning level, warning-free in Debug, Release and Dist.
- **Determinism:** cooked meshes must be identical from a Debug and a Release editor, since cache keys carry no configuration (§7.5). `Scripts/CheckBuildConfig.py`'s `PRECISE_VENDOR_PROJECTS` and its define families gain the project, with a seeded fixture `Tests/Data/BuildConfig/FastMathMeshOptimizer` and its pattern in `Scripts/Lib/scripts.py`; a unit case pins a generated level's checksum in both unit runs.
- **The consumers:** `UseMeshOptimizer()` in `Dependencies.lua` (the include directory and the link), copied verbatim from `VENDOR.md` and called by `Engine/premake5.lua`; `premake5.lua` includes the project; `Scripts/ModuleRules.json` lists it in `ThirdParty`, private to AssetPipeline and restricted to `AssetPipeline/Private/MeshSimplifier.cpp`. AssetPipeline is editor-only, so no exported game links code that calls it.
- **The notices:** a "## Notices" section, Shipped "yes" (the over-approximation for what the Engine project references, `notices.py:16-23`). The vendor cache keys the project from its pin like any vendored library.

### 6. Determinism, platforms and goldens

- **Determinism.** No committed hash moves: `RenderDistance` and `Wind` are component data that no committed scene holds; materials and cooked meshes are not hashed; level 0 imports byte for byte as before, so mesh colliders are unchanged; the renderer is presentation. `Wind:Sample` is DetMath in double precision: `Wind.replay` verifies in Debug, Release and Dist, and a unit case pins `SampleWind` to committed values in the Debug and Release unit runs. `Scaffold.py --check`, the determinism suite and the games suite confirm it.
- **`project.upgrade`.** W46.1 rewrites the 22 materials of RollingBall, Breakout and NativeTest and regenerates FeatureTest. No scene or prefab changes, and no demo's `.meta` or replay (none of the three has a glTF). FeatureTest's regenerated glTF `.meta` files write `ImporterVersion` 4 and the five new settings, and its replays' start fingerprints name the new cooked hashes of every glTF sub-asset they reference (meshes, prefabs, skeletons and clips: the importer version, 4, is in each artifact's header) and of its materials (format version 3); no `FinalStateHash` or checkpoint hash moves (the lead's correction after W46.1's review, which confirmed the changes unavoidable). World Tree and Skerries upgrade their materials in their own sessions, through the product owner (ADR 0060 decision 6).
- **Platforms.** Only Windows is verified (ADR 0028 decision 10); M46 adds no platform capability.
- **Goldens.** `GoldenMeshLods` and `GoldenWind` give 58 on `nvidia-62x` on its branch, and 61 at its merge, after M45's three. Every existing golden stays byte-identical: a draw without levels or wind runs today's arithmetic, its record words 0 and its flag clear, and binding 3's zero stream is read under no flag. A drift is a bug, never a golden update. No editor-UI capture opens Add Component.
- **Fuzz.** No new target (53 stay). The `MeshData`, `MaterialData` and `GltfImporter` targets' seeds gain version 3 payloads, version 3 materials and the level and `_WIND` fixtures. Each phase gate runs their 10,000 seeded mutations; the 1,000,000-iteration fuzz waits for the program's hardened gate (ADR 0060 decision 7).

### 7. Process

- **One builder** in its own worktree works the waves in order, with no contract. The lead runs one independent review of each wave, W46.4's included, and has every confirmed finding fixed, nits included.
- **Frozen headers.** `RenderSnapshot.h`, `RenderPrepare.h`, `InstanceRecord.h` and `DrawConstants.h` change only as decision 3 names, as reviewed changes to a frozen interface.
- **GPU discipline.** Filtered gpu, golden and automation runs only, through `Test.py` under the machine locks; four renderer builders share the one GPU in phase 8.
- **Checkpoint commits** run their targeted checks, never `PreCommit.py` or `CI.py`.
- **The landing** is phase 8's one batch in train order (ADR 0060 decision 7): M43, M44 and M45 merge into `landing/phase8`; W46.4 runs on `milestone/m46` after it merges `landing/phase8`; the lead merges `milestone/m46`, then the rest of the train; then the merge review; M48, the last lander, sets `EngineVersionString` 0.9.0 and regenerates FeatureTest.
- **The gate** is the batch's `CI.py --require-clang-cl`. The hardening waits for the program's last phase (ADR 0060 decision 7, the owner's answer 4).

### 8. Reservation needs

Phase 8's reservation ledger, committed in `Docs/Decisions/0060-skerries-engine-program.md` as its section "Phase 8 reservation ledger", is the rule: the rows below are M46's rows there, with its values, orders and owners, in the fixed train order M43, M44, M45, M46, M47, M48. A deviation needs a ledger amendment on main. Today's values are read at `3aaf4b10`; "at its merge" counts M43's, M44's and M45's entries, which land first.

| Allocation | Today | M46 adds |
|---|---|---|
| ADR numbers | 0059 the last committed; 0060 the program's | 0064, this record |
| Components and categories | 64 in 16 categories (`BuiltinComponents.h:78-129`) | categories `RenderDistance` and then `Wind`, appended after M44's Vehicles (M47's Water follows), each with one component and one registration function (`RegisterRenderDistanceComponents`, `RegisterWindComponents`) in a registration file of its own: 71 components in 19 categories at its merge (66 in 18 on its branch); `MakeValidationFixtures.py`'s `COMPONENT_ORDER` gains both after M44's `VehicleWheel` and before `ValidatorTarget` (`:73-84`) |
| Validation codes | 75 (`ProjectValidator.cpp:115-225`) | `ASSET_LOD_INVALID` (Warning), appended to both tables after M45's codes (82 at its merge, 76 on its branch) and to `ImportDiagnosticCodes` (`:522-528`); `AssetLodInvalidCode` inline in `Asset/AssetDiagnostic.h`, and `GetAssetDiagnosticCodes()` 13 → 14; the count line's constant 59 → 60; no check in `CheckScene` |
| Settings sections | as ADR 0059's ledger left them | none |
| Script namespaces and proxies | 22 namespaces (`Resources/Scripting/Engine.d.luau`); `PerformanceStats` 24 members; `RegisterBindings.cpp:57` ends with `RegisterJoints` | no namespace; two generated proxies; the method `Wind:Sample` (`RegisterWind`, after M44's `RegisterVehicles`); `PerformanceStats` +2 after M45's `SkyUpdates`: 32 at its merge (26 on its branch) |
| `IScriptHost` | ends with `GetMusic` (`ScriptHost.h:250`) | none |
| Automation methods and MCP tools | 126 (`Docs/Reference/Methods.json`) and 86 (`Tools/MCP/catalog.json`) | none; `stats.get`'s view summary +3 fields after M45's (41 at its merge) |
| Stats rows, pipelines, programs and passes | 41 rows, 77 startup pipelines (24 mesh), 34 declarations, 34 programs (`RenderPassDeclarationTests.cpp:245`, `:252`) | none; `RenderStats` +3 fields after M43's and M45's; one Stats panel row after M45's |
| `ViewConstants`, `DrawConstants` | 512 and 16 bytes (`ViewConstants.h:82`, `DrawConstants.h:77`) | unchanged |
| `InstanceRecord` | 208 bytes, `Padding1` and `Padding2` free | 208: `LodFade` in `Padding1`, `WindResponse` in `Padding2`; at W46.4, M43's `InstanceIndex::Reserved` holds a static entry's signed fade |
| Draw flags | 1 to 16 | `DrawFlagWind` 32, the phase's only new flag (the next is 64) |
| Shared shader structs and bindings | set 0 up to b7 and t20 | `Shared/WindConstants.h` at b8 of every mesh layout's set 0 (prepass, overdraw, forward, Shadow, SelectionMask), after M43's t21 and t22 (`Terrain.slang` and M47's `Water.slang` declare it unread), and in `ShadersTests.cpp`'s list after M45's `SkyConstants`; vertex binding 3 in all 36 mesh pipelines; `Common/Wind.slang`, `Common/LodFade.slang` |
| Snapshot | ends with M42's `PlanarReflections` | `RenderSnapshot::Wind` after M43's `StaticSet` and M45's `Sky` (M47's `Waters` follows); `RenderWind` after M45's `RenderSky`; `MeshDrawItem::Distance` appended, which W46.4 fills in M43's static cells too |
| Formats | cooked mesh 2, Material 2, `GltfImporter` 3; entity document 1, replay 5, pak 2 | cooked mesh 3 (`MeshStreams::Wind` bit 3, the levels' section), Material 3 (`MaterialWind`), `GltfImporter` 4 (five settings, `GltfLodSource`); the rest unchanged |
| Goldens | 56 on `nvidia-62x` | +2, `Golden: MeshLods` and `Golden: Wind` (`GoldenMeshLods.jsonl`, `GoldenWind.jsonl`): 61 at its merge, after M45's three (58 on its branch) |
| Perf metrics | 4 in `BUDGETS` (`Scripts/Lib/perf.py:49-50`) | `lod/Forest/gpuMs`, budget 3.0, in the `look` part: 8 budgets at its merge (5 on its branch) |
| Fuzz targets | 53 (`Tests/Data/Fuzz/Targets.json`) | none; three targets' seeds extended |
| FeatureTest | 101 scaffolds, highest IdSeed 3902; 116 suites, 26 replays, 104 scenes | IdSeeds 4301 to 4305 (M46's block 4300-4399): `MeshLods.jsonl` 4301, `Wind.jsonl` 4302, `WindReplay.jsonl` 4303, `GoldenMeshLods.jsonl` 4304, `GoldenWind.jsonl` 4305, after M43's to M45's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516); +2 suites, +1 replay, +4 scenes (two golden) |
| Vendored libraries | as `Docs/Decisions/0001-approvals.md` lists | meshoptimizer (decision 5): 26 directories in `Vendor/` |
| `PlaySessionPhase`, `AssetType`, built-in handles, state-hash blocks | as ADR 0059's ledger left them | none |
| `EngineVersionString` | 0.8.0 (`Core/BuildId.h:38`) | none: M48, the last lander, sets 0.9.0 |

**M43's ledger rows M46 builds on:** the ledger keeps `InstanceIndex::Reserved` for M46, returned by M43's load function in `Common/Instances.slang`. M46 itself adds, at W46.4, `Lod` to M43's static grouping key, making it (pipeline, material, mesh, submesh, Lod), the same to the static casters' per-face keys, and the per-cell largest `CullDistance`. Should M43's landed entry leave no room, decision 3E's fallback applies.

**The contended files M46 touches, and its region in each:**
- **Owned by M43 in phase 8** (M46 lands after M43, M44 and M45 and adapts at W46.4):
  - `Renderer/SceneRenderer.cpp`: `PrepareViewDraws` (`:899-1093`: the distance cull, the level, one or two draws per submesh, the record's words), the casters' list (`:1823-1842`), `WindConstants`' b8 in the forward, prepass and overdraw sets (`:2217`, `:2234`, `:2389`) after M43's t21 and t22, and the counters;
  - `Renderer/SceneRendererReflections.cpp`: `WindConstants`' b8 in the mirror view's set (`:370`), after M43's t21 and t22;
  - `Renderer/Private/ForwardPipelines.{h,cpp}`: `WindConstantsRegister` 8 in the prepass, overdraw and forward layouts after M43's t21 and t22, and the wind stream's slot 3 beside `:133` and `:136`;
  - `Renderer/Private/SceneRendererState.h`: the current and previous `WindConstants`, after M45's sky members;
  - `Renderer/ShadowPass.{h,cpp}`: `ShadowLayout` (`:32-66`: set 0's b8, binding 3) and `PrepareCasters` (`:369-501`: the level, the signature, the view's camera); at W46.4, the static casters' level and keys, and the wind phases in a face's M29 signature;
  - `Resources/Shaders/Passes/Scene.slang`: `TransformVertex`, `GetPreviousClip` and `VSOverdraw` (`:423-491`) and `GetCoverage` (`:497`), never `GetInstanceIndex` (`:410-413`, M43's); `Passes/Shadow.slang`: `VSMain` (`:59-79`), after M43's record load at `:62`;
  - `Renderer/RenderSnapshot.h`: the two members; `Scene/RenderExtraction.{h,cpp}`: `RenderDistance` on each mesh item (`:769`) and the Wind beside `snapshot.Fog` (`:947-949`), after M45's call; `Renderer/GpuResourceCache.{h,cpp}`: the mesh upload, the shared default wind stream (beside `:46-49`) and `GpuMaterial` only;
  - `Renderer/SelectionPass.{h,cpp}` and `Passes/Selection.slang`: the layout (b8, binding 3), the displacement and the level;
  - `Shared/DrawConstants.h` and `Shared/InstanceRecord.h` (M43's, then M46's): `DrawFlagWind`, `LodFade`, `WindResponse`, and the fade in `InstanceIndex::Reserved`;
  - M43's static grouping key and `RenderStaticCache`: `Lod` appended at W46.4, and `RenderDistance` among the signals a cell rebuilds on.
- **Shared with other phase-8 milestones, M46 after the earlier landers:** `Passes/Terrain.slang` (b8 declared unread, after M43's t21 and t22); `RenderStats.{h,cpp}`, `StatsMethods.cpp`, `StatsPanel.cpp`, `ScriptPerformanceStats.h`, `SessionStatistics.cpp` and `Bindings/Application.cpp` (fields appended after M43's and M45's); `ShadersTests.cpp` (`WindConstants` after M45's `SkyConstants`).
- **Append-only registries:** `BuiltinComponents.h` (the two categories after M44's Vehicles), `RegisterBindings.cpp` (`RegisterWind` after M44's `RegisterVehicles`), `ProjectValidator.cpp` and its tests (the code after M45's, its `ImportDiagnosticCodes` entry and `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s description, `:238`), `MakeValidationFixtures.py`, FeatureTest's `Scaffolds.json` and `Suites.jsonl`, `Scripts/Lib/perf.py`, `Scripts/Perf.py` and `test_perf.py`; the generated references, regenerated and never merged.
- **M46's alone in phase 8:** `premake5.lua`, `Dependencies.lua` and `Engine/premake5.lua` (meshoptimizer); `Scripts/ModuleRules.json`'s `ThirdParty` and AssetPipeline rows (M43 owns `SnapshotHeaders` and its Lint allowlist; distinct keys); `Scripts/CheckBuildConfig.py`; `MeshData`, `MaterialData`, `GltfImporter`, `MakeGltfFixtures.py` and `Tests/Data/Formats/Material/`; `Renderer/RenderPrepare.{h,cpp}` (the keys' `Lod`, `SelectMeshAlphaMode`); `Asset/AssetDiagnostic.{h,cpp}`.

## Acceptance

The Roadmap's M46 acceptance, each a named test or check:
- "MeshData: a version 3 payload with levels and a wind stream reads back equal"
- "MeshData: a payload whose level addresses a vertex beyond the mesh is refused"
- "GltfImporter: MSFT_lod nodes become their mesh's levels with their screen coverage"
- "GltfImporter: sibling nodes named _LOD<n> become one mesh's levels"
- "GltfImporter: a level whose submeshes differ from level 0 is dropped with ASSET_LOD_INVALID"
- "GltfImporter: generated levels share level 0's vertices and match their committed checksum", in Debug and Release
- "GltfImporter: an import without levels gives each fixture's mesh as version 3 did"
- "GltfImporter: a _WIND attribute becomes the mesh's wind stream, and the Wind setting reaches every material"
- "PhysicsComposition: a mesh collider of a mesh with levels holds level 0's triangles only"
- "MaterialData: a version 2 material loads without wind and saves as version 3"
- "MeshLod: a mesh's level follows its projected screen size times LodBias"
- "MeshLod: inside a fade band the two levels take complementary fades"
- "MeshLod: an item fades out across FadeWidth before its CullDistance and is culled beyond it"
- "MeshLod: a caster draws its view's level plus ShadowLodOffset"
- "Wind: Sample gives the committed velocity at fixed points and times", in Debug and Release
- "RenderExtraction: a mesh's RenderDistance and the scene's Wind reach the snapshot at the request's time"
- "MeshLods: across a fade band the two levels cover every pixel exactly once" (gpu)
- "MeshLods: a mesh draws its coarser level below its switch's screen size" (gpu)
- "MeshLods: a caster's shadow comes from its coarser level" (gpu)
- "MeshLods: a mesh beyond its CullDistance draws nothing and casts no shadow" (gpu)
- "Wind: the forward pass leaves no hole where the prepass covered a wind-swept mesh" (gpu)
- "Wind: a calm Wind renders the same bytes as no Wind" (gpu)
- "Wind: a wind-swept caster's shadow sways with it" (gpu)
- "Wind: the prepass's motion vectors follow the sway" (gpu)
- "MeshLods: a scene with levels and wind renders the same bytes after a device loss" (gpu)
- W46.4: the `MeshLods` and `Wind` gpu cases pass with their meshes `Static` too, and M43's "StaticScenery: a scene renders the same bytes with its scenery static and without" passes on a scene with levels and wind.
- `Golden: MeshLods` and `Golden: Wind`, with every existing golden byte-identical.
- `test_mesh_lods.py` and `test_wind.py` pass.
- The `MeshLods` and `Wind` suites pass in the editor, Release and Dist; `Wind.replay` verifies at one hash in Debug, Release and Dist and fails without its input.
- Perf.py's `lod/Forest/gpuMs` is within 3.0 ms at the gate's perf capture (warning-only).

**S04's "How the game will check it"** (items 1 and 3; item 2's parts are M49's):
- **Landfall's 20,000 grass, 2,000 trees and 4,000 rocks within the frame budget, with `stats.get` showing the draws.** M46's part is the trees and rocks: Perf.py's `lod/Forest/gpuMs` on the same counts, and `stats.get`'s `meshLodDraws`, `lodCrossFades` and `distanceCulledMeshes`. The grass and the scatter's draws are M49's; the Release export's frame time is the game's check.
- **Ridgeback's trees visible at 1 km from Landfall's beacon.** `FarClip` raised to 3,000 m (the corrections); "a mesh draws its coarser level below its switch's screen size"; `Golden: MeshLods`'s farthest level. The screenshot is the game's.
- **The cartographer collides with tree trunks, and a replay through the trees verifies.** For trees as entities: "PhysicsComposition: a mesh collider of a mesh with levels holds level 0's triangles only", and `Wind.replay` in all three configurations. The scatter's trunks are M49's.
- **Screenshots at wind 0 and in the storm's gale.** `Golden: Wind`, "Wind: a calm Wind renders the same bytes as no Wind" and the `Wind` suite; the storm's screenshots are the game's.

## Risks

- **Prepass and forward agreement under wind.** Every product and sum of the displacement is `NoContraction` and transcendental-free, in one function every vertex entry calls. The hole case runs a gale over a dense field, so a pipeline that skips the helper shows as depth holes.
- **Byte identity of every existing image.** Binding 3 in 36 pipelines, the record's words, the new set-0 slots and the keys' `Lod` reach every main view. W46.2 runs the filtered golden suite before its checkpoint, W46.4 again on the merged tree, and the gate runs all of it.
- **The adaptation at landing.** M43's static path is code M46 has not seen. W46.4 is sized and reviewed, and if M43's entry has no room for the fade, the fallback needs no entry bits.
- **meshoptimizer's determinism across editors:** precise floating point, checked by `CheckBuildConfig.py` with its fixture, and a checksum case in both unit runs.
- **Generated levels of leaf cards** stop early and may be dropped. The skills point trees at authored levels.
- **Shadows from a coarser level** can self-shadow where the coarse caster lies above the fine surface. `ShadowLodOffset` 0 removes it; the default follows the brief.
- **Wind casters and M29's cache.** A wind caster re-renders its spot or point map every frame, a cost where many swaying plants sit under cached lights (§6).
- **The Material upgrade and the re-cook.** The demos' materials are rewritten through a transcribed `Editor --upgrade`, as M20's were, and every glTF re-cooks once at first open (cache only).
- **Phase 8's GPU lock.** Four renderer builders queue on one GPU, so the waves' GPU runs stay filtered.
- **Same-phase readers.** M45's clouds and M47's water cannot depend on `Wind` inside phase 8 (Roadmap rule 3); they keep their own drift, and coupling them is a later row.

## The W46.1 record

W46.1, the data (decision 2), on `milestone/m46` from main `f244e1e7` (the program's setup landed). It touches none of M43's files. Its checkpoint fixes what a contract would freeze: both components' fields, the mesh version 3 layout, Material version 3's block and the import settings, as below.

- **meshoptimizer** (decision 5): `Vendor/meshoptimizer/`, release **v1.3**, commit `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (the latest tag on 2026-11-24, checked with `git ls-remote --tags`); upstream's `src/` (22 files) and `LICENSE.md` kept byte for byte, each with its SHA-256 in `VENDOR.md`; `gltf/`, `js/`, `demo/`, `tools/`, `extern/`, the CMake and make files, `README.md`, `CONTRIBUTING.md` and the dotfiles removed; no local modification.
  - **The build:** `Vendor/meshoptimizer/premake5.lua`, a C++17 static library `meshoptimizer`, `floatingpoint "Default"`, GCC and Clang `-fno-fast-math -ffp-contract=off` (clang-cl through `/clang:`), no defines, the compiler's default warnings; warning-free in the Debug and Release builds.
  - **The consumers:** `UseMeshOptimizer()` after `UseDetour()` in `Dependencies.lua` (`IncludeDir.meshoptimizer` = `Vendor/meshoptimizer/src`, `links { "meshoptimizer" }` for linking kinds). **Departure (necessary):** decision 5 names `Engine/premake5.lua` as its caller; tinyexr's model needs the linking projects too, so `Engine/premake5.lua`, `Editor/premake5.lua` (EditorCore and Editor) and `Tests/premake5.lua` call it, and `UseRuntimeDependencies()` does not (AssetPipeline is editor-only).
  - **The checks:** `Scripts/CheckBuildConfig.py`'s `PRECISE_VENDOR_PROJECTS` gains `"meshoptimizer"` and `VENDOR_ABI_FAMILIES` the family `MESHOPTIMIZER_` (empty everywhere); the fixture `Tests/Data/BuildConfig/FastMathMeshOptimizer` (`floatingpoint "Fast"`) and its pattern in `Scripts/Lib/scripts.py`. `Tests/Automation/test_vendor_cache.py`'s two lists of the vendor-cache libraries gain it (21).
  - **`Scripts/ModuleRules.json`:** `ThirdParty.meshoptimizer` (`Vendor/meshoptimizer/src`), AssetPipeline's private third party, restricted to `Engine/Source/Engine/AssetPipeline/Private/MeshSimplifier.cpp`.
  - **The notices:** `VENDOR.md`'s "## Notices", Shipped `yes`, Projects `-`, License files `LICENSE.md`. `Docs/Decisions/0001-approvals.md`'s row names the pin.
- **Cooked mesh version 3** (`Asset/MeshData.h`, decision 3A), exactly as `MeshData.h`'s comment states it:
  - `MeshStreams::Wind` = bit 3 (8); `MeshVertexWind` {`uint8_t Branch`, `Phase`, `Flutter`, `BendMask`}, 4 bytes, all 0 by default; `MaxMeshLods` = 5; `MeshLodRange` {`uint32_t IndexOffset`, `IndexCount`}; `MeshLod` {`float ScreenSize` (0.5), `std::vector<MeshLodRange> Submeshes`}.
  - `MeshData` appends `Wind`, `Lods`, `LodVertices`, `LodAttributes`, `LodWind` and `LodIndices`. A level index addresses level 0's vertices then the levels' own (`Vertices.size() + i` is `LodVertices[i]`); `LodAttributes` and `LodWind` hold one entry per level vertex when the mesh has the attribute or wind stream, else none.
  - **The payload:** version 2's layout with its `Reserved` word as `LodCount` (0 to 4); after the morph section `[Streams & Wind] VertexCount x MeshVertexWind`; then, with levels, `uint32 LodVertexCount; uint32 LodIndexCount; LodCount x { f32 ScreenSize; SubmeshCount x { uint32 IndexOffset; uint32 IndexCount; } }; LodVertexCount x MeshVertex; [Attributes] LodVertexCount x MeshVertexAttributes; [Wind] LodVertexCount x MeshVertexWind; LodIndexCount x uint32`. A mesh without wind and levels writes version 2's bytes.
  - **Validation** (`ValidateMeshData`): decision 3A's rules, plus `LodVertices` and `LodIndices` empty without levels and each level vertex and level attribute vertex checked as level 0's are; the reader refuses a `LodCount` above 4 (Parse) and counts that cannot fit before sizing anything.
- **`GltfImporter` version 4** (decision 3B), `AssetPipeline/Importers/GltfImporter.h`'s version 4 paragraph:
  - **The settings**, registered after `AnimationEvents`: `Lods` (registry enum `GltfLodSource` {`None`, `FromGltf`, `Generate`}, default `None`), `LodScreenSizes` (`std::vector<float>`, [0.5, 0.25, 0.125, 0.0625]), `LodTriangleRatios` (`std::vector<float>`, the same), `LodMaxError` (`float`, 0.05, [0, 1]), `Wind` (`MaterialWind`, disabled). A Validate rule refuses more than four sizes or ratios (`MaxGltfLodSettings`), sizes not strictly decreasing in (0, 1] and ratios outside (0, 1); its Generate hook makes random settings valid. `MaterialData.h`'s `RegisterMaterialWindType` registers `MaterialWind` unless the registry has it, and both `RegisterMaterialTypes` and `GltfImporter::RegisterTypes` call it, so either may register first (a registry of the importers' types alone, as `RegisterMethods`' tests build, has it too).
  - **`FromGltf`:** `CollectLevels` runs before the scene's nodes are collected. MSFT_lod groups first, in node order: the extension's `ids` (a JSON object with an array of unsigned integers, else the group is reported and ignored) name the level nodes in level order; an id out of range, naming its own node or a node of another group is reported. The extras' `MSFT_screencoverage` replaces the sizes when it holds at least one value per level, each in (0, 1] and below the one before: level L takes over at value L - 1 (the last value, where the last level would end, is not used: `RenderDistance` culls); another one is reported (subject `nodes[<i>].extras.MSFT_screencoverage`) and `LodScreenSizes` used. Then the `<name>_LOD<n>` groups among the other nodes: one per parent (or the scene's roots) and `<name>` (a non-empty prefix, n one to nine decimal digits), in order of their `_LOD0` node; without `_LOD0` the names form no group. Each group's levels go to its level 0 node's glTF mesh, so every node drawing that mesh draws them; every level node, kept or dropped, and its subtree are never placed. A level is its level node's mesh in its mesh's own space, Scale applied (**choice:** the level node's transform is ignored, so levels authored at one origin line up; Blender's `_LOD<n>` objects are exported that way).
  - **`Generate`:** `GenerateLevels` on every cooked mesh (each skin variant and `mesh:merged`): per level k (up to `LodTriangleRatios.size()`, at most four) and per submesh, `meshopt_simplify` over level 0's positions (the whole mesh's, so the error is relative to the mesh's extent) towards `clamp(floor(triangles x ratio), 1, triangles)` triangles within `LodMaxError`, with `meshopt_SimplifyLockBorder` (`AssetPipeline/Private/MeshSimplifier.{h,cpp}`, the one file that includes `meshoptimizer.h`).
  - **The streams:** the attribute and wind streams cover level 0 and every authored level once one of them has the stream, the others' vertices at the stream's defaults; generated levels add indices only.
  - **`ASSET_LOD_INVALID`**, one per dropped level (subjects `nodes[<level node>]`, `meshes[<i>].lods[<k>]`, `mesh:merged.lods[<k>]`): decision 3B's reasons, and four more the code meets (**departure**, decision 3B's list is not exhaustive): a level with no switch size (`LodScreenSizes` shorter than its number), a level of a node without a mesh or a level node without one, a group whose level 0 mesh an earlier group already gave levels, and a generated level that keeps no triangle. Every level is reported with MergeMeshes (FromGltf), and MSFT_lod's malformed extension and screen coverage as above.
  - **`_WIND`:** `ReadWind` accepts VEC4 of float or of normalized unsigned byte or short (anything else fails the import naming `_WIND`), clamps to [0, 1] and rounds to unorm8; it follows a vertex that MikkTSpace's tangents split (`ApplyCornerTangents`).
  - **The Wind setting** is every imported material's block.
- **Material version 3** (decision 3H): registry struct `MaterialWind` {`bool Enabled` (false); `float Bend` (0.1, [0, 1]); `float Branch` (0.1 m, [0, 1]); `float Flutter` (0.02 m, [0, 0.25]); `float Stiffness` (0.5, [0, 1])} as `MaterialData::Wind` after `UVOffset`; `CurrentVersion` and `FormatVersion` 3, `MaterialImporter::Version` 2. Fixtures: `MaterialV1.upgraded.material` at version 3; `MaterialV2.material` (with UV1 sets) and `MaterialV2.upgraded.material`. The `MaterialData` fuzz case's seeds gain `Tests/Data/Formats/Material/`; the reference's example material is version 3.
- **The components** (decisions 3C and 3F), exactly as `Scene/Components/RenderDistanceComponent.h` and `WindComponent.h` state:
  - `RenderDistance` (category `RenderDistance`, `Registration/RenderDistanceRegistration.cpp`, requires `MeshRenderer`): `float CullDistance` (0 m, [0, 100000]), `float LodBias` (1, [0.01, 100]), `uint32_t ShadowLodOffset` (1, [0, 4]), `float FadeWidth` (0.1, [0, 0.5]).
  - `Wind` (category `Wind`, `Registration/WindRegistration.cpp`, `UniquePerScene`): `glm::vec3 Direction` ((1, 0, 0); a Validate rule refuses a zero X and Z, length in double, and its Generate hook makes it (1, 0, 0)), `float Strength` (5 m/s, [0, 100]), `GustStrength` (3 m/s, [0, 100]), `GustFrequency` (0.2 Hz, [0, 10]), `Turbulence` (0.25, [0, 1]).
  - `BuiltinComponents` ends with them (66 components in 18 categories on the branch); `MakeValidationFixtures.py`'s `COMPONENT_ORDER` gains both after `FixedJoint`.
- **`Scene/Wind.{h,cpp}`** (decision 3F): `GetWindDirection` (X and Z normalized in double, (1, 0, 0) without them), `GetWindGustWavelength` (`Strength / GustFrequency`, 0 when either is 0), `WindGustWave(x)` (the triangle u = 1 - |2 frac(x) - 1| smoothed by u²(3 - 2u)), `SampleWind` (along d: `Strength + GustStrength G(phase + Turbulence G(c / 2))`; across: `Turbulence GustStrength (2 G(phase / 2 + c + 1/4) - 1)`; phase = `GustFrequency t - a`, a and c the point's coordinates along and across d over the wavelength) and `FindSceneWind` (the effectively enabled Wind, the lowest entity UUID's among several). Double precision, IEEE-exact operations, `floor` and `sqrt`.
- **`Wind:Sample(position: vector, time: number?): vector`** (`Scripting/Bindings/Wind.cpp`, `RegisterWind` after `RegisterJoints` on the branch, after M44's `RegisterVehicles` at the landing): `SampleWind` at the world's clock (the world's tick times the fixed delta) when time is nil; a read, allowed in read-only evaluations.
- **`Renderer/MeshLod.{h,cpp}`** (decision 3D): `MeshLodParameters` (the component's four fields), `ComputeMeshLodSphere`, `ComputeMeshScreenSize` (infinite inside the sphere), `SelectMeshLod` and `SelectShadowLod`, as `MeshLod.h` states. **Choices:** a band runs from s (1 - FadeWidth / 2) to s (1 + FadeWidth / 2) around a switch size s; the band of the switch below the level is checked before the one above it; inside the fade-out before `CullDistance` the level alone fades (no cross-fade there); a caster's level is the selection's `Level`, the finer level's in the upper half of a band and the coarser's in the lower half.
- **`ASSET_LOD_INVALID`** (decision 3I): `AssetLodInvalidCode` in `Asset/AssetDiagnostic.h`, `GetAssetDiagnosticCodes()` 14 (appended), both validator tables and `ImportDiagnosticCodes`; the count line's constant 60; the fixture `Tests/Data/Validation/ASSET_LOD_INVALID/` imports `LodMismatch.gltf` with `Lods` `FromGltf` in its `.meta`. `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s description names Environment, PostProcess, Fog, Navigation and Wind (**departure:** decision 3F's list omits Navigation, which is unique per scene too; M45 adds Sky at its merge).
- **Material version 3 elsewhere:** `Tests/Data/Assets/Materials/Red.material` and `Textured.material` and `MakeProvenanceFixtures.py`'s material (its fixtures regenerated) move to version 3, since the reference's format schema names the current version; the game-debugging skill's `validation-codes.md` gains `ASSET_LOD_INVALID`'s row now (its unit case names every code), ahead of W46.3's other skill pages.
- **The glTF fixtures** (`MakeGltfFixtures.py`): `LodTree.gltf` (MSFT_lod with screen coverage, `_WIND` as normalized bytes and floats), `LodRock.gltf` (`Rock_LOD0` to `Rock_LOD2`), `LodSphere.gltf` (512 triangles for `Generate`) and `LodMismatch.gltf` (a kept level and three dropped ones). The spheres subdivide an octahedron with square roots only, so the bytes are the same everywhere. The `GltfImporter` fuzz case draws `Lods` among its three values.

- **The upgrade** (decisions 3H and 6): the Release editor's `Editor --headless --renderer none --project <P> --upgrade` rewrote RollingBall's 11, Breakout's 8 and NativeTest's 3 materials, transcribed in each project's `Automation/BuildLog.jsonl` (`project.refreshAssets` id 1, `project.upgrade` id 2) and recorded in its `Provenance.json`; `AuditProvenance.py --files disk` passes on all three. `Scaffold.py` (Release) regenerated FeatureTest: its 118 materials at version 3 and, **departing from decision 6's "no .meta or replay changes"** (both inevitable): the eight glTF `.meta` files its scaffolds import write `ImporterVersion` 4 and the five new settings at their defaults, and eight replays' start fingerprints name the new cooked hashes of every glTF sub-asset they reference (the importer version, 4, is in each artifact's header: 7 meshes, 5 prefabs, 7 skeletons and 5 clips) and of the 14 materials (format version 3). Decision 6 is corrected to say so. No state hash moves: no `FinalStateHash`, checkpoint or recorded hash changed, and the fingerprint only explains a difference (§12.4).
- **What W46.3 adds to FeatureTest:** the two components' coverage lands with W46.3's scaffolds, in the wave order of decision 2: until then the feature suite's gates 1 (both components and their fields), 2 (`Wind:Sample` and the `Entity.RenderDistance` and `Entity.Wind` reads, the shortcuts each registered component adds) and 4 (both proxies' fields) name them as uncovered. Gate 7 (every description) is met already. W46.3's `MeshLods` and `Wind` suites read the components through `entity.RenderDistance` and `entity.Wind`, so gate 2 counts both shortcuts.
- **Runs** (W46.1, as its logs show): Debug and Release builds without warnings; the Debug unit suite unfiltered (`--parallel 4`), 7 failures, fixed and rerun with "ReferenceGenerator: every authored file validates against its generated format schema" still failing on FeatureTest's kept golden materials at version 2, which FeatureTest's recorded `--upgrade` and `Scaffold.py` then fixed (no Debug run after them); the Release unit cases `GltfImporter:*` and `Wind:*` (the committed `SampleWind` bits and the generated level's checksum hold in both configurations); the base checksums came from a Release build of `f244e1e7` with a probe case never committed; `CheckBuildConfig.py` on the workspace; `Scaffold.py` and `Scaffold.py --check`; `GenerateDocs.py --config Debug` and `--check`; `Lint.py`; a rewriting `Format.py`. The checkpoint's other claimed runs have no log; the review's fixes run and log each of them (below).

- **The lead's review** of W46.1 (5 minor findings and 4 nits, every one fixed in one commit after the W46.2 snapshot step):
  - **Runs:** the "Runs" item above now states what W46.1's logs show; the fixes' commit reran and logged every claimed check (its trailer names them).
  - **Tests:** "GltfImporter: every level the importer cannot keep is dropped with ASSET_LOD_INVALID and the import succeeds" covers every drop reason the earlier cases left out: a fifth level, a level `LodScreenSizes` gives no switch size, a level node or level 0 node without a mesh, a mesh an earlier group already gave levels, an authored level of a skinned and morphed mesh, a malformed `MSFT_lod` extension (not an `ids` array, a fraction, an id out of range, its own node, another group's node) and a generated level that keeps no triangle (`LodSphere` at ratio 0.001 within `LodMaxError` 1: reachable, so it is tested rather than recorded as unreachable); "ScriptWind: Sample without a time reads the world's clock, which a world pause holds" (600 session ticks of which 135 held, then 30 more held, then 30 resumed); "Wind: FindSceneWind returns the scene's effectively enabled Wind" gains several Winds, created out of UUID order and re-created, the lowest UUID's always chosen.
  - **Descriptions:** `ASSET_LOD_INVALID`'s three descriptions (`ProjectValidator.cpp`, `AssetDiagnostic.h`, the game-debugging skill's `validation-codes.md`) and `GltfImporter.h`'s version 4 paragraph name departure 2's reasons, the malformed `MSFT_lod` extension and ids and their subjects (`nodes[<i>].extensions.MSFT_lod`, `nodes[<i>].extensions.MSFT_lod.ids[<k>]`, `nodes[<i>].extras.MSFT_screencoverage`).
  - **`Vendor/meshoptimizer/VENDOR.md`** records the library's one piece of process-level state, the allocation hooks the engine never sets.
  - **`GetWorldTick(const ScriptFrameState&)`** (`Scripting/ScriptHost.h`) is the world clock's one rule: `Time.GetTick` and `Time.GetTime` (`Bindings/Time.cpp`), `Wind:Sample`'s default time (`Bindings/Wind.cpp`) and the tasks on the world's clock (`TaskScheduler.cpp`, `Private/ScriptInstances.cpp`) read it.
  - **Decision 6 and departure 4** state the replay fingerprints' moved hashes exactly; gate 2's uncovered entries include the two `Entity` shortcuts.

## Before milestone completion

- [x] W46.1 committed; [x] the lead's review pass taken and every finding fixed.
- [ ] W46.2 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W46.3 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W46.4 committed on `milestone/m46` after its merge of `landing/phase8` with M43, M44 and M45; [ ] the lead's review pass taken and every finding fixed.
- [x] meshoptimizer's `VENDOR.md`, notices and approvals row in place, and `CheckBuildConfig.py` with its fixture green.
- [ ] The landing: phase 8's batch in train order, fourth after M43, M44 and M45, M43's contended files kept and M46's entries appended, FeatureTest regenerated, the gate `CI.py --require-clang-cl` green.
- [ ] S04's "How the game will check it" (items 1 and 3) met in FeatureTest, the goldens, the gpu cases and Perf.py, and reported to the Skerries session through the product owner with the corrections above.
- [ ] Architecture, the Roadmap, the skills, the Backlog and the generated references describe what landed.
