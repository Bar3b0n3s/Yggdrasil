# 0059 — M42 Planar reflections

- **Status:** accepted. M42's plan and milestone record, in phase 7 beside M41 (`Docs/Decisions/0058-m41-bone-attachments.md`, amended the same day for M42).
  - **Approval.** World Tree's brief 23 arrived after phase 7's plan. The product owner pointed the lead to it on 2026-11-19, and the lead took it into phase 7: phase 7 had just started, and the owner can still hold it for a later phase before its builder starts.
  - **The record.** M42's builder amends this record on `milestone/m42` as each wave lands, in "The W42.1 record" to "The W42.3 record", making decision 3 exact where it leaves a choice to the milestone. The lead's reviews and the landing follow.
  - Lead-owned.
- **Date:** 2026-11-19.
- **Decided by:** the lead (the milestone, its design defaults, the ledger, and the two questions the research left for the owner, decision 4), recorded by the lead.
- **Base:** main `bb4b257f` (phase 7's plan; engine 0.7.0). Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone";
  - ADR 0023; ADR 0028 decisions 10, 11, 12 and 17;
  - ADR 0052 decision 6 (the process);
  - ADR 0058 (phase 7 and its amendment for M42);
  - ADR 0029 (M18's renderer stages, recovery and out-of-memory rules); ADR 0044 (M29's light counts, the model for the stats); ADR 0047 (M32's Fog and SSR);
  - Architecture §8, §13.7, §15.5 and §16.

## Context

- **The brief.** World Tree's brief 23 (`Projects/WorldTree/Design/EngineTasks/23-PlanarReflections.md`, medium priority) came from a player's screenshot of the distributable build.
  - The ice realm's wall mirrors (metallic 1, roughness 0.03) show a smeared gold fragment of the Warden and a hole where the Warden hides the floor. Nothing off screen appears in them.
  - Its Expected asks for a flat surface that is a true mirror:
    - rendered from the camera mirrored in its plane, clipped at the plane, at a lower resolution, through a lighter path;
    - capped per view;
    - followed when it moves;
    - presentation only;
    - costing about 2 ms or less for three mirrors at 1080p on the development machine.
- **The research.** One read-only report of the lead's (kept outside the repository) read the brief and the renderer at `bb4b257f`. This record draws its design defaults and ledger from it.
- **What the engine has today** (each fact checked at `bb4b257f`):
  - **Glossy reflections come in three layers:**
    - the environment's prefiltered cube (or the constant ambient);
    - the reflection probes, inside their boxes, over it;
    - SSR on top. Its composite adds (traced − prefiltered) in proportion to its confidence, and it traces only where the prepass's `SceneRoughness` multiplier is non-zero and the roughness is at most `PostProcess.SsrMaxRoughness` (engine default 0.6, not the brief's 0.4).

    SSR fades with roughness. The probes blur through their cubes' mips.
  - **One `SceneRenderer` is one view.** It renders a fixed list of 13 stages and 33 pass declarations (37 stats rows, 76 startup pipelines, 33 shader programs).
    - Every per-view object lives in `SceneRenderer::State`; pipelines live in the shared `SceneRendererPipelines`.
    - The renderer never includes Scene: a new render item is a member of the frozen `RenderSnapshot`.
  - **The projection is reverse-Z with an infinite far plane,** and several shaders linearize depth as `near / depth`. An oblique near plane would ruin far depth precision and those linearizations. A fragment discard needs a feature the engine does not enable. `shaderClipDistance` is neither required nor enabled today.
  - **Per view, and reusable or not:**
    - Light clusters, the culled light list and the shadow plan are per view and per camera.
    - Spot and point shadow maps depend on the light alone and can be sampled by another view.
    - Directional cascades are chosen by the shading view's depth.
    - Skinning deforms once per renderer per frame, so a second view inside the same renderer deforms nothing twice.
    - Fog, the sky, particles (play scenes only) and the terrain have per-view state.
    - Exposure is applied only in Tonemap, so `SceneColor` holds pre-exposure radiance.
  - **Where views are rendered:**
    - the editor's Scene and Game views (the Game view also headless, as Perf.py uses it);
    - `ViewportCapture` (`viewport.screenshot`, FeatureTest screenshots, thumbnails);
    - `ProbeCapture` (the bake);
    - the Runtime: a headless Runtime with Vulkan still renders its game view.

    Only `--renderer none` renders nothing.
  - **`stats.get`** reports per-view rows and M29's light counts. The script readout `Application.GetStats` (`PerformanceStats`) carries them to scripts in every configuration. Perf.py's look part measures the headless game view at 1920 × 1080, warning-only, in `CI.py --milestone`'s hardening.
- **Corrections to the brief** (each goes to the World Tree session through the product owner at the phase's end):
  - **The component is component data.** It is in the canonical serialization and the state hash of any scene that holds it, as M32's Fog is. No engine hash moves, but World Tree's ice scenes take new hashes once the game adds the component, so any of their replays is recorded again.
  - **A headless Runtime with a GPU renders the reflections.** Only `--renderer none` skips them.
  - **`SsrMaxRoughness`' engine default is 0.6.**
  - **Shadows are reused exactly only within the main view's plan:**
    - a light that only a reflection sees is unshadowed;
    - directional shadows reach a reflection only inside the main view's cascades.
  - **Particles appear only in play scenes,** in a mirror as outside one.

## Decisions

### 1. Scope

Brief 23's mirrors, as one milestone M42, in phase 7 beside M41 (ADR 0058, as amended). It delivers:
- a component that makes a flat surface a true mirror, rendered by a lighter path from the camera mirrored in its plane, clipped at the plane;
- the reflection sampled by the mirror's own material in place of its probe and SSR terms;
- a cap per view with a fallback past it;
- stats, a perf metric, a golden image and FeatureTest coverage in every mode.

What M42 leaves out is listed in decision 3, "Out", and recorded in `Docs/Backlog.md` §2. A limit it accepts on landing goes to §6.

### 2. Structure: one milestone, three waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 7 | M42 | Planar reflections | 23 | after M41, in phase 7's batch (ADR 0058, as amended) | the batch's `CI.py --milestone` | ~8 (W42.1 ~2, W42.2 ~4, +1 at risk, W42.3 ~2) |

One builder, on `milestone/m42` in `D:/Engine/.claude/worktrees/m42`, works the waves in order:
- **W42.1, the component and the plan (CPU):**
  - the component, its registration, `PlanarReflectionItem` and its extraction;
  - the bake's cleared list;
  - `Renderer/PlanarReflections.{h,cpp}`: the plane, the mirrored and cull cameras, the facing and visibility tests, the ranking and the cap;
  - `RENDER_REFLECTION_INVALID` with its fixture;
  - the CPU and extraction cases.

  Its first checkpoint fixes what a contract would freeze, M34's way: the component's fields, `PlanarReflectionItem`, the `ViewConstants`, `InstanceRecord` and draw-flag layouts, the pass declaration and its rows, and the stats fields.
- **W42.2, the GPU path:**
  - the device feature;
  - the clip in the scene, terrain and particle vertex shaders;
  - the per-slot state and targets;
  - the mirror view's passes, the mip chain, the main view's sampling and the SSR skip;
  - the shadow reuse and light lists, and the fog start;
  - the rows and counts;
  - out-of-memory and device loss;
  - the gpu cases;
  - a filtered golden run proving every existing golden byte-identical.

  It merges main first once brief 21's first fix (the bake's allocations, in `SceneRenderer.cpp` and `ProbeCapture.cpp`) has landed.
- **W42.3, coverage, tools and documents:**
  - `stats.get`'s fields, the Stats panel and the readout;
  - Perf.py's metric;
  - FeatureTest's scaffold, suite, golden scaffold and image;
  - the Python case;
  - Architecture, the Backlog, the skills and the generated references.

Numbering: M42's record is this ADR, and the next record takes 0060.

### 3. Design defaults: M42 Planar reflections

**What M42 adds:**
- a component and a `RenderSnapshot` member;
- a pass declaration with one pipeline, one program and four stats rows;
- stats fields and two readout members;
- one validation code, one golden and one perf metric;
- one required device feature.

**What it does not add:** a namespace, a settings section, an `AssetType`, a built-in handle, a `PlaySessionPhase`, a fuzz target, an automation method, a script function, method or callback, a state-hash block, a format version, a render stage, a module or a vendored library.

- **A component, not a render-to-texture camera.** `PlanarReflection`, version 1, category Probes:
  - appended after `ReflectionProbe` in `Scene/Registration/ProbesRegistration.cpp`, and in `BuiltinComponents.h`'s include and type list after `ReflectionProbeComponent`;
  - requires Transform; script-visible through its generated proxy.

  It sits beside the other reflection sources in Add Component's Probes group. Why a component:
  - A mirror needs its plane, the clip, the facing and visibility tests, the cap and the fallback. A camera into a texture would leave all of them to scripts, a frame late.
  - A render-to-texture camera needs a texture handle that materials bind (an `AssetType` or built-in kind and a Material format change), and shadow, history and resolution rules of its own. Its uses (security cameras, portals, minimaps) stay a §2 row.
- **Fields.** Each is checked on every write path, and the plane is explicit: a glass may be a quad facing ±Y or ±Z or a thin box, and its reflecting side cannot be guessed. The validator checks the mesh against the plane and suggests the axis.
  - `Normal`: vec3, default (0, 0, 1). The reflecting side's normal in the entity's local axes, normalized and never zero, as M37's `Axis`. The default is the built-in Quad's face.
  - `Offset`: float, default 0, finite. The plane's distance from the entity's origin along `Normal`, in local units.
  - `ResolutionScale`: float, default 0.5, in [0.25, 1]. The reflection's size as a share of the view's scene resolution.
- **No roughness field.** The planar sample fades into the probe term over the upper half of the roughnesses up to the view's `PostProcess.SsrMaxRoughness`, the cutoff SSR uses. That field's description becomes "the roughness above which only the probes reflect (screen-space and planar reflections fade out below it)".
- **Extraction.**
  - **The snapshot.** `RenderSnapshot` gains `PlanarReflections` after `Fog`, a frozen-header change: a vector of `PlanarReflectionItem {World; Normal; Offset; ResolutionScale; Entity}`.
  - **When an item is extracted:** in the canonical loop, for an effectively enabled entity that holds the component and a `MeshRenderer` that is `Visible` with a mesh.
  - **At which pose:** `ComputeRenderedWorldMatrix`, the rendered pose its mesh draws at.
  - **In which views:** every view, edit scenes included, as the probes are extracted.
  - **The bake.** `ProbeCapture`'s `MakeCaptureSnapshot` clears the list: one bounce, as it turns the probes off.
  - **The world plane.** The renderer derives it from `World`: the normal through the inverse-transpose, the point `World × (Normal × Offset)`. So a non-uniform scale is handled, and a moving or turning glass (the aurora mirror) is followed from the pose it draws at, every frame.
- **Where it runs: one declaration, `PlanarReflections`, first in the Forward stage before `ForwardOpaque`.** No stage is added, since a stage would change the frozen `RenderStage.h` and its zone tests for a profiler zone the rows already give.
  - **Prepare.** After the main view's draws are prepared, Prepare selects the reflections and prepares each mirror view's draws.
    - The mirror views' instance records are appended after the main view's, the casters' and the selection's.
    - The main view's records of a reflected entity carry `DrawFlagPlanarReflection` (16) and the slot (`InstanceRecord::Padding0`, the size unchanged), so instanced batches are kept.
  - **The Forward stage** renders each selected mirror view in slot order, then the main view's forward passes sample them.
- **The mirrored camera is rigid.** With the plane's reflection R and the main view V, the reflection renders with `View = S·V·R`, where S flips view-space X.
  - **Why S.** `V·R` alone has a negative determinant, which would flip every draw's winding and face test. `S·V·R` is rigid, so every pipeline and face rule works unchanged, and the stored image is mirrored left for right.
  - **The camera's values:**
    - `Position` = R × the camera's position;
    - the main view's unjittered projection, for the reflection's target size;
    - no history and no jitter.
  - **Sampling.** The main forward shader samples the reflection at the shaded point's projection by the reflection's view-projection. On the glass this is the main view's pixel mirrored, exact under TAA's jitter and the scissor.
- **The clip at the plane: `SV_ClipDistance`.**
  - **`ViewConstants` goes from 480 to 512 bytes:**
    - `ReflectionView` takes `Padding4`;
    - `ClipPlane` (float4) is appended: the world plane, offset by the engine constant `PlanarReflectionClipBias` (about 1 cm) toward the reflecting side, so neither the glass nor its backing shows;
    - `CascadeDepthPlane` (float4) is appended.
  - **Which shaders clip.** The vertex shaders a mirror view runs write `SV_ClipDistance0 = dot(ClipPlane, (world, 1))`: `Scene.slang`'s `TransformVertex`, `Terrain.slang` and `Particles.slang`.
  - **Every other view clips nothing.** It sets `ClipPlane = (0, 0, 0, 1)`, a distance of 1 at every vertex, so every existing image and golden stays byte-identical.
  - **The device feature.** `shaderClipDistance` becomes required at device selection and is enabled at device creation (decision 4).
  - **Rejected alternatives:**
    - an oblique projection, for depth precision and the `near / depth` linearizations;
    - a coverage clip in the fragment shader, which puts every draw on the Mask variants, leaves the transparent pass with no coverage variant, and forces late depth.
- **The cull.** A mirror view's draws are the snapshot's mesh items, minus the reflection's own entity, culled two ways:
  - against the mirror camera with its projection cropped to the glass's screen rectangle;
  - against the clip plane, so an item wholly behind the glass is culled.
- **The mirror view's path**, in the slot's targets:
  1. clear and the prepass;
  2. `TerrainDepthNormal` with the slot's own terrain view resources;
  3. light clusters for the mirror camera;
  4. `ForwardOpaque`, `TerrainForward` and the Skybox;
  5. `ForwardTransparent`, sorted for the mirror camera;
  6. particles, with the slot's plan and depth (play scenes only);
  7. the HDR colour's mip chain.

  Rasterization is scissored to the glass's screen rectangle, widened by the blur's footprint, so the cost follows the glass's size on screen.

  **Left out of a mirror view:**
  - SSR, GTAO (the neutral white AO, as the transparent pass binds it), TAA, auto-exposure, bloom, Tonemap and FXAA. The reflection is pre-exposure radiance in the main view's units, so the main view's post chain exposes, blooms and resolves it.
  - Sprites, tilemaps, 2D lights, world and screen text, the UI, debug draw and every editor overlay.
- **Shadows and lights are the main view's.**
  - **Bound from the main view:** its `ShadowConstants` and maps, environment constants, probe set and instance buffer. Nothing is drawn into a shadow map for a reflection.
  - **The slot's own light list:**
    - first the main view's culled lights, at their indices, so every shadow slot and the directional index stay valid;
    - then the lights only the mirror camera sees, unshadowed, up to `MaxShadedLights`.
  - **The directional cascade.** A reflection view chooses it by the main camera's depth of the point (`dot(CascadeDepthPlane, p)`, only when `ReflectionView` is set, so main views compute exactly as today). A point outside the main cascades is lit.
- **Fog.**
  - **The rule.** The reflection fogs only the path from the glass to the reflected point; the main view's forward pass fogs the path from the camera to the glass as it fogs any surface.
  - **How.** `Fog.slang` starts a reflection view's fog at the view ray's crossing of `ClipPlane`, the sky included.
- **How the mirror's material samples the reflection.**
  - **Replacement.** On a flagged draw, the main view's forward pass replaces `prefiltered` (the environment's or probe's radiance) with the slot's reflection. The reflection is sampled at the projected position, blurred by roughness through the chain's mips as the probe cube is blurred (the builder fixes the level curve against the golden), and faded back into `prefiltered` over the upper half of the roughnesses up to `SsrMaxRoughness`.
  - **Energy.** It is multiplied by the same `multiScatter × SpecularOcclusion × HorizonOcclusion` as today, so Fresnel and energy agree with the other paths.
  - **SSR skips the glass.** The prepass writes a zero `SceneRoughness` multiplier for a flagged draw, so SSR neither traces nor composites over the glass, with no SSR change. SSR on the floors still sees the mirror and its reflection.
  - **Glass of any kind.** Transparent glass works through the same shader. Any material opts in without a format change, since the flag comes from the component: `Mirror.material` stays as it is.
  - **Bindings.** Set 0 gains `PlanarReflections0` to `PlanarReflections3` (t17 to t20) and `PlanarReflectionConstants` (b7). Views and slots without a reflection bind a neutral 1 × 1 black texture, a pipeline-set startup resource. `Terrain.slang`, which shares the forward set, declares them unread.
- **Resolution and memory.**
  - **Size.** The reflection's target is `ceil(scene size × ResolutionScale)`: 960 × 540 at 1080p by default. It uses `SceneColorFormat` with a full mip chain, from the renderer's pool.
  - **Scratch.** The prepass scratch targets are shared by the slots of one size, since slots render in sequence. Only each slot's colour lives until the main forward pass.
  - **Memory.** About 32 MB with four slots at 1080p, half resolution.
- **The cap per view.** `MaxPlanarReflectionsPerView = 4`, an engine constant, applied per renderer: the editor's Scene and Game views and each capture apply it on their own.
  - **A candidate** is a reflection that is all three:
    - **in view:** a draw of its entity survived the main view's culling;
    - **facing:** the camera lies on the reflecting side (by the camera's position for a perspective view, by its forward axis for an orthographic one);
    - **drawable:** in a Lit view outside the wireframe, which is SSR's condition.
  - **The ranking:** by screen area, largest first, ties by entity UUID, so captures are reproducible.
  - **Past the cap,** a mirror falls back to today's probe and SSR, drawing exactly as without the component, and the view logs once that it dropped reflections.
  - **Out of view or facing away,** a mirror costs nothing and counts as culled.
  - **When a reflection's target cannot be created,** it falls back for this render. Render returns its partial error, and the host trims and renders again (M18 decision 44).
- **Recursion: none.** A mirror view's records carry no flag and bind the neutral textures, so a mirror seen in a mirror shows its fallback. A reflection never draws its own glass.
- **Determinism and presentation.**
  - **What is hashed.** The component is component data, in the serialization and the hash of a scene that holds it, like Fog's. Nothing the renderer does is hashed.
  - **What does not change.** No hash block is added, no committed hash moves, and no replay is recorded.
  - **Renderer none.** A `--renderer none` editor or Runtime extracts the items, one lookup per entity, and renders nothing.
  - **Run modes.** Headless runs with a GPU and Dist exports render the reflections (`RunModes::All`).
- **The editor's views, captures, screenshots and pick.**
  - **Each `SceneRenderer` renders its own camera's reflections:**
    - the Scene view in edit and play, so a mirror shows while authoring;
    - the Game view, also headless;
    - `ViewportCapture`, whose warm-up is unchanged, since a reflection keeps no history.
  - **Picking is unchanged:** `viewport.pick`'s CPU ray and the GPU click pick hit the glass.
  - **No editor overlay** draws inside a reflection.
- **Stats and perf.**
  - **`RenderStats`** appends four fields after `SkinnedMeshes`:
    - `PlanarReflections`: the number drawn;
    - `DroppedPlanarReflections`: candidates past the cap;
    - `CulledPlanarReflections`: out of view, facing away, or in a view that draws none;
    - `PlanarReflectionEntities`: the drawn reflections' entities, in slot order.
  - **Each reflection's cost.** The declaration gives four rows, `PlanarReflection0` to `PlanarReflection3`, one per slot by rank, each timing that slot's whole view. A slot's light clusters record under the shared `LightClusters` row, since rows never nest.
  - **`stats.get`'s view summary** appends `planarReflections`, `droppedPlanarReflections`, `culledPlanarReflections` and `planarReflectionEntities`, each described, as M29's rows name lights.
  - **The Stats panel** gains the row "Planar reflections: drawn / dropped".
  - **The script readout** `PerformanceStats` appends `PlanarReflections` and `DroppedPlanarReflections`, so a suite sees in Release and Dist that a reflection rendered.
  - **Perf.py's look part** gains `reflections/Mirrors3/gpuMs`:
    - a generated `Assets/Perf/Mirrors.scene` at 1920 × 1080, with three built-in Quad mirrors facing a lit room of props and a shadowed point light;
    - the metric is the sum of the `PlanarReflection<k>` rows' GPU time in the game view;
    - a warning-only budget of 2.0 ms (the brief's target), with `test_perf.py` cases.
- **Device loss, trim and recreation.**
  - **Per view, in `SceneRenderer::State`** (the placement rule's second place): the per-slot targets, buffers, terrain resources, particle plans and binding caches. The renderer's existing Release and Recreate drop and rebuild them, and the next Render creates them lazily.
  - **In the pipeline set,** through the declaration: the mip pipeline, the neutral texture and the bindings.
  - **No new registration.** The pool's `TrimUnused` releases free reflection targets.
  - **Cases:** a `Test::RunDeviceLossTest` case on a mirror scene (identical bytes), and an out-of-memory case (the reflection falls back and the partial error is returned).
- **Validation: `RENDER_REFLECTION_INVALID`** (Warning, not auto-fixable).
  - **Causes:**
    - the entity holds no `MeshRenderer` with a mesh;
    - the mesh's bounds lie farther than `PlanarReflectionMaxMeshDistance` (an engine constant of about 5 cm, measured after the entity's scale) from the plane. The finding suggests the local axis along which the bounds are thinnest and the matching `Offset`, which catches a glass whose `Normal` names the wrong axis.
  - **Where it is reported.** `project.validate` reports it through `FindPlanarReflectionIssues(scene, assets)` in a new `Scene/PlanarReflectionValidation.{h,cpp}`, with its own code array (M36's and M37's pattern).
  - **Where it is appended.** At the end of both of `ProjectValidator.cpp`'s tables, after M41's `ANIMATION_ATTACHMENT_INVALID`; `ProjectValidatorTests.cpp`'s count line grows by one.
  - **Its fixture and rows.** The fixture is `Tests/Data/Validation/RENDER_REFLECTION_INVALID/`, from `MakeValidationFixtures.py`. It gets the game-debugging skill's `validation-codes` row and §13.7's row.
- **Scripts, automation and the editor.**
  - **Scripts.** No function, method or callback. The proxy reads and writes the three fields (gate 4), and `PerformanceStats` gains its two members.
  - **Automation.** No method; 126 methods and 86 MCP tools stay. The component goes through `entity.create`, `entity.update` (in play too), `edit.batch`, `component.schema` and `prefab.*`.
  - **Rule 5's Python case** goes in a new `Tests/Automation/test_planar_reflections.py`, under `self.require_gpu()`. It makes a mirror and a box, then reads from `stats.get`'s Scene view:
    - `planarReflections` 1 and a `PlanarReflection0` row;
    - after the glass is turned away, `culledPlanarReflections` 1.
  - **The editor** uses the generic inspector in the Probes group. There is no plane gizmo (§2).
- **FeatureTest** (IdSeeds 3900 to 3999; every gate in each mode the feature supports).
  - **`PlanarReflections.jsonl`** (IdSeed 3901) makes `PlanarReflections.scene` and its suite, bound in `Suites.jsonl`.
    - **The scene:** five mirrors of built-in Quads and Planes. Across them every field leaf is non-default: `Normal` along X, Y and Z in turn, a non-zero `Offset`, and a `ResolutionScale` other than 0.5. Each glass is flat on its plane, so there is no validation finding.
    - **The suite:**
      - reads and writes the three fields through the proxy;
      - turns a mirror from a task;
      - where `Application.GetStats().RenderAvailable` holds (the exported Release and Dist runs render headless), checks that four are drawn and one dropped with all five in view, and that a turned-away mirror is culled.
  - **`GoldenPlanarReflections.jsonl`** (IdSeed 3902, `GoldenDirectories` `Assets/Scenes/GoldenPlanarReflections/`) makes one edit scene for the golden `Golden: PlanarReflections`. The scene holds:
    - a sharp mirror (roughness 0.045);
    - an asymmetric object with a marker on its back that only the mirror shows, mirrored;
    - a box behind the glass, which is clipped;
    - the sky in the glass's upper part;
    - a directional light with cascades and a shadowed point light;
    - fog;
    - a rough mirror (roughness 0.35, blurred);
    - a mirror turned away, which shows its fallback.

    Every existing golden stays byte-identical.
  - **No replay.** The determinism suite is unchanged, and the soak gains two subjects.
  - **Gates:**
    - 1: `PlanarReflection` with every field leaf non-default;
    - 4: the three fields through the proxy;
    - 6: the code's fixture;
    - 7: every description (the component, its fields, the `stats.get` fields, the readout members);
    - 1b, 2, 3, 5, 8 and 9: nothing new.
- **Tests.**
  - **CPU** (`Tests/Source/Engine/Renderer/PlanarReflectionsTests.cpp`):
    - the mirrored camera is rigid and maps the plane's points to the main view's pixels mirrored;
    - the facing test for both projections;
    - the ranking with ties by UUID;
    - the cropped cull camera;
    - the plane from a scaled `World`;
    - the extraction cases in `RenderExtractionTests.cpp`: every view, edit included; no item without a visible mesh; the rendered pose at alpha 0.5;
    - `ProbeCapture`'s cleared list.
  - **GPU** (`PlanarReflectionTests.cpp`, `Test::GpuSuite`):
    - a mirror shows the view a camera mirrored in its plane sees;
    - nothing behind the plane appears in a mirror;
    - a view draws at most four reflections, the largest first, and a mirror past the cap draws exactly as without the component;
    - a mirror out of view or facing away renders no reflection;
    - a mirror seen in a mirror shows its fallback;
    - SSR does not trace a planar mirror's pixels;
    - a skinned mesh deforms once for a view and its reflections;
    - the main view's shadows shade a reflection;
    - a reflection of a moving plane follows its rendered pose;
    - the device-loss and out-of-memory cases.
  - **The frozen-layout cases:** `RenderPassDeclarationTests`' rows (41), the pipeline count (77), and the shared-struct reflection (`ViewConstants` 512).
- **Documents.**
  - **Architecture:**
    - §5.3: the component row;
    - §8.1: the required feature;
    - §8.2: the snapshot member;
    - §8.3: the pass table, the Forward stage's row and a "Planar reflections" paragraph;
    - §8.4: `ViewConstants`, set 0's bindings, the record's flag and slot;
    - §8.6: the order of the reflection terms, planar over SSR over probe over environment;
    - §8.13 and §8.14: captures and the per-slot objects;
    - §13.7, §15.5 and §16: the device requirement.
  - **The Roadmap's M42 section.**
  - **`Docs/Backlog.md`:** §2 and §6.
  - **The header comments** of `RenderSnapshot.h`, `RenderPassDeclaration.h`, `SceneRenderer.h` and `ViewConstants.h`.
  - **`PostProcessComponent`'s `SsrMaxRoughness` description.**
  - **The skills:**
    - game-building's `look`: the mirror recipe (the component on the glass's own entity, `Normal` matching the mesh, half resolution, the cap of 4, the cost);
    - game-debugging: the `stats.get` fields, the readout members and the `validation-codes` row;
    - golden-images: the scene list.
  - **The generated references.**
- **Out** (`Docs/Backlog.md` §2):
  - a general render-to-texture camera (security cameras, portals, minimaps);
  - recursive reflections;
  - curved mirrors, a normal map's distortion of the reflection, refraction and water waves;
  - a terrain as the reflecting surface;
  - opt-in per submesh or per material;
  - 2D, text, UI and debug draw inside a reflection;
  - a reflection's own shadow planning, probes and temporal filtering;
  - a project setting for the cap, and hysteresis at it;
  - a plane gizmo.
- **Accepted at landing** (§6):
  - A light that only a reflection sees is unshadowed, and directional shadows reach a reflection only inside the main view's cascades.
  - A reflection uses the main view's probes.
  - A reflection is not temporally filtered, so TAA can trail a reflection that moves against its glass.
  - A mirror at the cap switches between planar and fallback as the ranks change.
  - Geometry within the clip bias of the plane is clipped.

### 4. The research's two questions, decided by the lead

1. **`shaderClipDistance` becomes a required device feature.**
   - It is a core Vulkan 1.0 feature that desktop drivers report, and it is the only clip that fits the infinite reverse-Z projection and keeps every existing image byte-identical.
   - A device without it is refused at startup, with the missing features named, as for today's required features.
   - W42.2's first GPU run confirms that the development machine's device reports it, before anything depends on it.
   - **Rejected:** optional support, which needs shader variants, a mirrors-off path, and a device class nobody can test.
   - Only Windows is verified (ADR 0028 decision 10).
2. **Phase 7 lands in one batch by default** (ADR 0058, as amended):
   - `landing/phase7` merges M41 and then M42, and one `CI.py --milestone` gates both.
   - M41 lands alone ahead of it, gated by `CI.py --require-clang-cl`, only if M42 runs far behind: W42.3 not begun when M41's milestone review is done.

### 5. Approvals

None. Nothing is downloaded or vendored: the fixtures are built-in meshes and materials made by the scaffolds.

### 6. Determinism, platforms and goldens

- **Determinism.** M42 is presentation. The component is component data, and no committed hash moves (`Scaffold.py --check`, the determinism and games suites).
- **Platforms.** Windows only is verified. The one new device requirement is decision 4's.
- **Goldens.** One new golden scene, `GoldenPlanarReflections`, gives 56 goldens on `nvidia-62x`. Every existing golden stays byte-identical, and a drift is a bug, never a golden update. No editor-UI capture opens Add Component.

### 7. Process

As ADR 0058 decision 6:
- **No contract.** The lead runs one independent review of each wave and has every confirmed finding fixed, nits included.
- **Frozen headers.** `RenderSnapshot.h`, `RenderPassDeclaration.h` and the shared shader structs change only as decision 3 names, as reviewed changes to a frozen interface.
- **GPU discipline.** Filtered gpu, golden and automation runs only.
- **Checkpoint commits** run their targeted checks.
- **The landing** is the lead's, in phase 7's batch.

### 8. Reservation ledger

Today's values are read at `bb4b257f`; M41's are ADR 0058 decision 9's.

| Allocation | Today | M42 |
|---|---|---|
| ADR numbers | 0058 is the last, M41's | 0059, this record; the next takes 0060 |
| FeatureTest IdSeeds | 97 scaffolds, the highest 3702; M41 reserves 3801 and 3802 | `PlanarReflections.jsonl` 3901 and `GoldenPlanarReflections.jsonl` 3902 (`GoldenDirectories`), listed after main's and M41's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516) |
| Suites, replays and scenes | 114, 25 and 101 (M41: +1, +1, +1) | +1 suite, no replay, +2 scenes (one golden) |
| BuiltinComponents | 16 categories, 62 components (M41: 63) | `PlanarReflection` in Probes after `ReflectionProbe`: 64. No category or registration function. `MakeValidationFixtures.py`'s `COMPONENT_ORDER` gains it after `ReflectionProbe` |
| Validation codes | 73 (M41: 74) | 75: `RENDER_REFLECTION_INVALID` (Warning) after M41's code, with its own array |
| Script API | 22 namespaces; `PerformanceStats` has 22 members | no namespace, function, method or callback; `PerformanceStats` +2 members (`RegisterApplication`); `Engine.d.luau` gains the proxy type and the members (generated) |
| Methods and MCP tools | 126 and 86 | unchanged |
| Stats | `RenderStats` and the view summary end with M28's `SkinnedMeshes` | four fields appended to each, the Stats panel's row and the readout's two |
| Render stages, passes and pipelines | 13 stages, 33 declarations, 37 rows, 76 startup pipelines, 33 programs | 13, 34 (`PlanarReflections`, Forward stage, before `ForwardOpaque`), 41 rows (`PlanarReflection0`–`3`), 77 pipelines (the mip chain's compute), 34 programs (`Passes/PlanarReflection.slang`) |
| Shared shader structs | `ViewConstants` 480 bytes; `InstanceRecord` 208 bytes; draw flags 1 to 8 | `ViewConstants` 512 (`ReflectionView` in `Padding4`; `ClipPlane` and `CascadeDepthPlane` appended); `InstanceRecord` 208 (`Padding0` becomes the slot); `DrawFlagPlanarReflection` 16; the new `Shared/PlanarReflectionConstants.h` and `Common/PlanarReflections.slang` |
| Formats | entity document 1, replay 5, Material unchanged, pak 2, `NativeApiVersion` 1 | unchanged; `PlanarReflection` version 1; no Material field |
| Goldens | 55 on `nvidia-62x` | 56 |
| Perf metrics | `BUDGETS` holds 3 | `reflections/Mirrors3/gpuMs`, budget 2.0 |
| Device features | `shaderClipDistance` neither required nor enabled | required at selection, enabled at creation |
| Fuzz targets, `PlaySessionPhase`, settings sections, `AssetType`, built-in handles, state-hash blocks | as ADR 0052's ledger left them | none |
| `EngineVersionString` | 0.7.0 | one bump for phase 7, 0.8.0, at its batch landing, with FeatureTest regenerated once |

**M42's own files.**
- **New:**
  - `Scene/Components/PlanarReflectionComponent.h`, `Scene/PlanarReflectionValidation.{h,cpp}`;
  - `Renderer/PlanarReflections.{h,cpp}` (the CPU part), `Renderer/PlanarReflectionPass.{h,cpp}` (the mip pipeline), `Renderer/SceneRendererReflections.cpp` (the stage functions);
  - the shaders;
  - the tests: `PlanarReflectionsTests.cpp`, `PlanarReflectionTests.cpp`, `PlanarReflectionGoldenTests.cpp` and `test_planar_reflections.py`;
  - the two scaffolds, the golden image and the validation fixture.
- **Changed:**
  - **Renderer:** `RenderSnapshot.h` and `RenderPassDeclaration.h` (frozen, reviewed), `SceneRenderer.{h,cpp}`, `Private/SceneRendererState.h`, `Private/ForwardPipelines.{h,cpp}`, `RenderPrepare.{h,cpp}`, `RenderStats.{h,cpp}`, `ProbeCapture.cpp`;
  - **Graphics:** `DeviceSelection.{h,cpp}`, `GraphicsDevice.cpp`;
  - **Scene and registration:** `RenderExtraction.{h,cpp}` (the item in the canonical loop), `BuiltinComponents.h`, `ProbesRegistration.cpp`;
  - **Stats and scripting:** `StatsMethods.{h,cpp}`, `StatsPanel.cpp`, `ScriptPerformanceStats.h`, `SessionStatistics.cpp`, `Bindings/Application.cpp`;
  - **Validation:** `ProjectValidator.cpp`;
  - **Scripts:** `Scripts/Perf.py`, `Scripts/Lib/perf.py`;
  - **Their tests.**

**Shared with M41, whose owner M41 is in every contended file** (it lands first in the batch, and M42 appends after it at the landing):
- **`RenderExtraction.cpp`.** M41's region is `ComputeRenderedWorldMatrix`, M42's the item in the canonical loop: disjoint.
- **`RenderExtraction.h`.** Both add a header paragraph; keep both.
- **`RenderExtractionTests.cpp`.** Both append; M41's goes first.
- **Conflicts by construction**, M42 adapting:
  - `BuiltinComponents.h`, within git's context;
  - `ProjectValidator.cpp`'s two tables and `ProjectValidatorTests.cpp`'s count line;
  - `MakeValidationFixtures.py`'s `COMPONENT_ORDER` (one line holds both neighbours) and its appended fixtures;
  - `Tests/Data/Validation/README.md` and the `validation-codes` page;
  - FeatureTest's `Scaffolds.json` and `Suites.jsonl`;
  - `BuiltinComponentsTests.cpp`;
  - the generated references, regenerated and never merged.

**Shared with brief 21,** which lands on main first:
- **b21a** (the bake's allocations) touches `SceneRenderer.cpp` (the allocation warning), `ProbeCapture.cpp`, `StatsMethods.cpp` (a description) and `GraphicsDevice.{h,cpp}`. These are adjacent to M42's hunks, not overlapping, so M42 merges main before W42.2.
- **b21b and b21c** share no code file with M42.
- **All three** share Architecture, the Backlog and the generated references, which merge by hand.

## Acceptance

The Roadmap's M42 acceptance, each a named test or check:
- "PlanarReflections: the mirrored camera is rigid and maps the plane to the view's pixels mirrored"
- "PlanarReflections: a reflection faces the camera only from its reflecting side"
- "PlanarReflections: reflections rank by screen area with ties by UUID"
- "RenderExtraction: a planar reflection is extracted at its rendered pose in every view"
- "PlanarReflections: a mirror shows the view a camera mirrored in its plane sees" (gpu)
- "PlanarReflections: nothing behind the plane appears in a mirror" (gpu)
- "PlanarReflections: a view draws at most four reflections, and a mirror past the cap draws as without one" (gpu)
- "PlanarReflections: a mirror seen in a mirror shows its fallback" (gpu)
- "PlanarReflections: SSR does not trace a planar mirror's pixels" (gpu)
- "PlanarReflections: the main view's shadows shade a reflection" (gpu)
- "PlanarReflections: a reflection of a moving plane follows its rendered pose" (gpu)
- "PlanarReflections: a mirror scene renders the same bytes after a device loss" (gpu)
- `Golden: PlanarReflections`, with every existing golden byte-identical.
- `test_planar_reflections.py`; the `PlanarReflections` suite passes in the editor, Release and Dist.
- Perf.py's `reflections/Mirrors3/gpuMs` within 2.0 ms at the gate's perf capture (warning-only).

**Brief 23's "How the game will check it":**
- **The Mirror Hall's north mirror** shows the Warden's back, mirrored, at its distance, with the floor whole and the sky above, and a husk behind the Warden in it. This is the golden's mirror, its marker and its clipped box, and the gpu cases. The Mirror Hall itself is the game's check.
- **`Mirror.material` stays as it is**, with the component on the glass entities, and the aurora mirror's reflection turns with it: the material needs no field, and "a reflection of a moving plane follows its rendered pose".
- **Three mirrors at 1080p cost about 2 ms or less,** measured with `stats.get` in a headless game view: Perf.py's metric on the same view and `stats.get`'s `PlanarReflection<k>` rows. The hall's own figure is the game's check.
- **Every suite and replay passes unchanged.** No engine hash moves. The game re-records only replays of the scenes it adds the component to (the corrections above).

## Risks

- **Byte identity of every existing image.** The clip with the plane (0, 0, 0, 1), set 0's new bindings, the 512-byte `ViewConstants` and the prepass's multiplier rule reach every main view. W42.2 runs the filtered golden suite before its checkpoint, and the gate runs all of it.
- **The `SceneRenderer` refactor.** The stage functions read the renderer's single targets, constant buffers and draw lists, and a mirror view needs them parameterized. It is the largest edit of the core renderer file since M26, so the wave review checks the main path line by line.
- **Flat stats scopes.** Pass helpers that open their own rows (`LightClusterPass::Record`) need the shared-row treatment inside a slot's row, or the recording context's asserts fire.
- **The 2 ms target.** Each mirror view draws the room again. The Perf.py metric measures it. The fallbacks are a lower default resolution, dropping the reflection's transparent pass or particles, or a lower cap.
- **The device requirement** (decision 4), confirmed on the development machine first.
- **TAA trailing, and the light cap** inside a full view (accepted, §6).
- **The phase's length.** M42 is the phase's long pole (~8 sessions against M41's ~5), and decision 4's landing rule keeps M41 from waiting long.
- **World Tree's part.** The component goes on the glass's own entity with the mesh's facing axis, which the validator checks, and the ice scenes' replays are recorded again. Both go to the World Tree session through the product owner.

## The W42.1 record

W42.1 makes decision 3 exact where its CPU part leaves a choice, and fixes, M34's way, what a contract would freeze: the component's fields, `PlanarReflectionItem`, the `ViewConstants`, `InstanceRecord` and draw-flag layouts, the pass declaration and its rows, and the stats fields. Each is in the code at the wave's checkpoint, unread by any pass until W42.2, so every image stays byte-identical.

- **The component** (`Scene/Components/PlanarReflectionComponent.h`; `ProbesRegistration.cpp` after `ReflectionProbe`; `BuiltinComponents.h` after `ReflectionProbeComponent`: 63 components on M42's branch, 64 with M41's): `PlanarReflection`, version 1, category Probes, requires Transform, the default flags (serialized, script-visible, editor-visible, removable), so the generated proxy reads and writes the three fields.
  - `Normal`: `glm::vec3`, default (0, 0, 1), normalized when used, with no per-component bound. A zero `Normal` is refused on every write path by the component's `Validate` rule ("must not be zero"), its length taken in double so that no finite vector overflows; its `Generate` hook turns a random zero into (0, 0, 1).
  - `Offset`: `float`, default 0, in [-10000, 10000] (unit "m", local units along `Normal`). Decision 3 asks only for a finite value; the bound is the reflection probe's largest box side, so the plane's arithmetic stays far from overflow.
  - `ResolutionScale`: `float`, default 0.5, in [`MinPlanarReflectionResolutionScale`, `MaxPlanarReflectionResolutionScale`] = [0.25, 1]. The bounds and the default (`DefaultPlanarReflectionResolutionScale`) live in `Renderer/RenderSnapshot.h`, the one renderer header Scene may include, so the registration and the renderer share them.
- **`PlanarReflectionItem`** (`RenderSnapshot.h`, before `RenderSnapshot`): `{glm::mat4 World = I; glm::vec3 Normal = (0, 0, 1); float Offset = 0; float ResolutionScale = 0.5; UUID Entity}`. `Normal` is the component's as written; the renderer normalizes it, and an item with a zero or non-finite one renders no reflection. `RenderSnapshot::PlanarReflections`, a `std::pmr::vector` after `Fog`, allocates from the snapshot's memory resource ("RenderSnapshot: a snapshot over a memory resource allocates its containers from it").
- **Extraction** (`RenderExtraction.cpp`, the canonical loop after the probes, M42's region): an effectively enabled entity that holds a `PlanarReflection` and a `MeshRenderer` that is `Visible` with a valid `Mesh` gives one item at `getWorld()`, the cached `ComputeRenderedWorldMatrix` its mesh item draws at, in every view. A hidden mirror gives none, so a hidden glass costs nothing.
- **The bake** (`ProbeCapture.cpp`'s `MakeCaptureSnapshot`): the list is cleared. Its GPU case ("ProbeCapture: a capture leaves the scene's planar reflections out, so a PlanarReflection changes no texel") lands with W42.2, when a reflection first renders, and fails there without the clear.
- **`Renderer/PlanarReflections.{h,cpp}`**, pure:
  - `MaxPlanarReflectionsPerView` = 4; `PlanarReflectionClipBias` = 0.01 m.
  - `PlanarReflectionPlane {glm::vec3 Normal; float Distance}`: the unit normal out of the reflecting side, `dot(Normal, p) + Distance` the signed distance; `ToVector()` gives `ViewConstants`' form.
  - `ComputePlanarReflectionPlane(item)`: in double precision, the local point `normalize(Normal) * Offset` through `World`, and the normal through `World`'s inverse transpose, normalized. Empty for a zero or non-finite `Normal`, a non-finite `Offset` or `World`, or a singular upper 3 x 3. A mirroring `World` keeps the reflecting side on the image of the local positive side.
  - `ComputePlanarReflectionMatrix(plane)`: `R = I - 2 n n^T` with the translation `-2 d n`.
  - `ComputePlanarReflectionClipPlane(plane)`: `(n, d - PlanarReflectionClipBias)`; a point is kept where `dot(clip, (p, 1)) >= 0`, so the glass on the plane is behind it.
  - `IsPlanarReflectionFacing(camera, plane)`: perspective, the camera's `Position` strictly on the positive side (a camera on the plane does not face it); orthographic, the camera's local -Z (from `View`) against the normal.
  - `MakePlanarReflectionCamera(camera, plane, width, height)`: `View = S * camera.View * R`, `Position = R * camera.Position`; the camera's own `Projection` (`CameraData` holds it unjittered), kind, field of view, size, clip range, clear colour and entity; the viewport `width` x `height`; no history, jitter or delta time. **Choice:** the projection is the camera's own, not rebuilt for the reflection's size, so the plane maps exactly to the main view's pixels mirrored; the reflection's aspect differs from the view's only by `ceil`'s rounding, which projective sampling absorbs.
  - `ComputePlanarReflectionSize(sceneWidth, sceneHeight, scale)`: `ceil(size * scale)` per axis in double, at least 1, the scale clamped to [0.25, 1] and a non-finite one taking 0.5 (960 x 540 at 1080p).
  - `PlanarReflectionScreenRect {glm::vec2 Min = (-1, -1); glm::vec2 Max = (1, 1)}` in normalized device coordinates, with `GetPixelArea(width, height)`. `ComputePlanarReflectionScreenRect(bounds, world, camera)`: the projected corners' bounds clipped to [-1, 1]. **Choice:** a box with a corner on or behind a perspective camera's plane (clip w <= 1e-6) takes the whole view, a conservative rectangle; one wholly behind, wholly outside on one side, empty or of zero area gives none.
  - `MirrorPlanarReflectionScreenRect(rect)`: X negated and the bounds swapped, the glass's rectangle in the mirror camera's coordinates. `MakePlanarReflectionCullCamera(mirror, mainRect)`: the mirror camera with `Projection` premultiplied by the crop that maps the mirrored rectangle onto the clip volume (`x' = sx * (x - cx * w)`, likewise y), so `IsOutsideView` keeps exactly what is seen through the glass's rectangle.
  - `IsBehindPlanarReflectionClipPlane(bounds, world, clip)`: all eight corners behind; empty bounds never are.
  - `ComputeCascadeDepthPlane(camera)`: the negated third row of `View`, so `dot(plane, (p, 1))` is the camera's view depth `-(View * p).z`, the depth `Common/Shadows.slang`'s cascade weights take.
  - `PlanarReflectionCandidate {size_t Item; UUID Entity; float ScreenArea}` and `RankPlanarReflections(candidates, cap = 4)` give `PlanarReflectionSelection {std::vector<size_t> Slots; uint32_t Dropped}`: by `ScreenArea` (the drawn glass's rectangle in scene pixels), largest first, ties by ascending `Entity`, then `Item`; a non-finite area last; the first `cap` are the slots in rank order, the rest dropped.
- **The shared layouts** (`Resources/Shaders/Shared/`):
  - `ViewConstants`, 512 bytes: `Jitter` at 464, `ReflectionView` (uint, `Padding4`'s place) at 472, `Padding5` at 476, `ClipPlane` (float4, default (0, 0, 0, 1)) at 480 and `CascadeDepthPlane` (float4, default 0) at 496. Every view's default `ClipPlane` clips nothing, and `ReflectionView` 0 keeps every shader on today's path.
  - `InstanceRecord`, 208 bytes: `PlanarReflectionSlot` (uint) at 188, `Padding0`'s place.
  - `DrawFlagPlanarReflection` = 16 in `DrawConstants.h`.
  - "Shaders: SharedStructsMatchReflection" and "ShaderReflection: parses bindings, structs and the entry point of a vertex shader" check the sizes and offsets against the compiled shaders.
- **The pass declaration** (`RenderPassDeclaration.h`): `PlanarReflections`, Forward stage, first in it (before `BrdfLut`, the DFG LUT's startup-only declaration, and `ForwardOpaque`); stats rows `PlanarReflection0` to `PlanarReflection3` after `LightClusters` (41 rows); targets `PlanarReflectionColor` (each slot's reflection, `SceneColorFormat` with its mip chain) and the mirror views' shared prepass scratch `PlanarReflectionDepth`, `PlanarReflectionNormals`, `PlanarReflectionVelocity` and `PlanarReflectionRoughness` (the prepass pipelines' four attachments). **Departure (form only):** W42.1 declares it without its pipeline, as M26's contract declared its passes, so nothing records it, no `stats.get` output changes and no stub exists; W42.2 sets `PipelineCount` 1 and the program `PlanarReflection` (77 pipelines, 34 programs).
- **The stats fields** (`RenderStats.h`, after M28's `SkinnedMeshes`): `uint32_t PlanarReflections`, `DroppedPlanarReflections` and `CulledPlanarReflections`, and `std::vector<UUID> PlanarReflectionEntities`, which W42.2's Prepare fills. `stats.get`'s view-summary names (`planarReflections`, `droppedPlanarReflections`, `culledPlanarReflections`, `planarReflectionEntities`) and the readout's members (`PerformanceStats.PlanarReflections` and `DroppedPlanarReflections`) land with W42.3.
- **Validation** (`Scene/PlanarReflectionValidation.{h,cpp}`: `FindPlanarReflectionIssues(scene, assets)`; `RenderReflectionInvalidCode` in its own array `PlanarReflectionDiagnosticCodes`; `PlanarReflectionMaxMeshDistance` = 0.05 m, the one Scene-side constant). At most one finding an entity, over effectively enabled entities in canonical order:
  - no `MeshRenderer` (component `PlanarReflection`, no field), or one without a `Mesh` (`MeshRenderer.Mesh`);
  - **an addition:** a zero `Normal` (`PlanarReflection.Normal`), which no write path accepts but a file read in Repair mode can hold;
  - a mesh that loads as a Mesh whose bounds' corners, at the entity's world matrix, lie farther than 5 cm from the plane, the world distance being the local one over `|inverse transpose(world) * n|`. The hint names the local axis along which the bounds are thinnest in the world (the extent times the axis's world length), `Normal`'s sign kept where it has one, and the `Offset` through the bounds' centre on it. A mesh that does not load is the asset checks' (`ASSET_MISSING`, `ASSET_TYPE_MISMATCH`); a hidden `MeshRenderer` is checked as a shown one.
  - `ProjectValidator.cpp`: appended to both tables after M37's code on this branch (after M41's at the landing) and checked by `CheckPlanarReflections` after `CheckNavigation`; Warning, not fixable. The fixture (`MakeValidationFixtures.py`, `COMPONENT_ORDER` with `PlanarReflection` after `ReflectionProbe`) is a default `PlanarReflection` on the built-in Cube. The game-debugging skill's `validation-codes` row and §13.7's sentence are written.
- **FeatureTest.** Its scaffolds land with W42.3. Until then the feature suite's gates 1 and 4 name the new component's fields as uncovered, as M41's W41.1 left its own, and the milestone-end gate runs on W42.3's tree.
- **Tests:**
  - `PlanarReflectionsTests.cpp`: the three acceptance cases ("the mirrored camera is rigid and maps the plane to the view's pixels mirrored", "a reflection faces the camera only from its reflecting side", "reflections rank by screen area with ties by UUID"), and the cull camera, the plane from a scaled, turned and moved `World`, the reflection matrix, the size, the screen rectangle, the clip plane and the cascade depth plane.
  - `RenderExtractionTests.cpp`: "RenderExtraction: a planar reflection is extracted at its rendered pose in every view" (the game and explicit views of an edit scene; the hidden, meshless, disabled and bare mirrors left out; the pose at alpha 0.5 equal to its mesh item's).
  - `ProjectValidatorTests.cpp`: "ProjectValidator: RENDER_REFLECTION_INVALID reports a mirror without a mesh or off its glass, with the glass's axis", and the order and count lines.
  - `RenderPassDeclarationTests.cpp`: "RenderPassDeclarations: the planar reflections come first in the Forward stage with a row per slot", and the 41-row order.
  - `BuiltinComponentsTests.cpp`: 63 components, their order and the defaults; `RenderAllocatorTests.cpp`: the new container's resource.
- **Generated references:** `Components.md`, the scene and prefab schemas, `Methods.json` (the schemas it embeds), `ScriptAPI.md`, `Validation.md` and `Engine.d.luau` (the proxy type).
- **Runs** (2026-11-19, the wave's checkpoint on `milestone/m42`, before the merge of main):
  - Debug build: 0 warnings.
  - unit, Debug, unfiltered: 5,518 of 5,519 passed. The failure, "TerrainRegistration: the Terrain and its collider are registered after the probes, the collider requiring the Terrain", read the ReflectionProbe as the Probes category's last component; it now reads the PlanarReflection.
  - unit, Debug, targeted after the fixes (`PlanarReflections`, `RenderExtraction`, `RenderPassDeclarations`, `BuiltinComponents`, `ProjectValidator`'s code cases, `Validation`, `RenderSnapshot`, `ShaderReflection`, `Shaders`, `ComponentAccess`, `ProbesRegistration`, `TerrainRegistration`): 119 passed.
  - `Lint.py`, `Format.py --check`, `GenerateDocs.py --config Debug --check` and `MakeValidationFixtures.py --check`: passed.
  - No GPU run: nothing W42.1 adds renders.

## Before milestone completion

- [x] W42.1 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W42.2 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] W42.3 committed; [ ] the lead's review pass taken and every finding fixed.
- [ ] The landing: phase 7's batch (ADR 0058, as amended), with M41's contended files kept and M42's entries appended, FeatureTest regenerated, `EngineVersionString` 0.8.0, the gate green.
- [ ] Brief 23's "How the game will check it" met in FeatureTest, the goldens and the gpu cases, and reported to the World Tree session through the product owner with the corrections above.
- [ ] Architecture, the Roadmap, the skills, the Backlog and the generated references describe what landed.
