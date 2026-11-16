# 0047 — M32 fog and SSR's cost

- **Status:** accepted; the milestone record of M32 "Fog and SSR's cost" (Roadmap M32; `Docs/Decisions/0042-world-tree-engine-program.md`, phase 5). No contract task: one builder works M32's one wave, W32.1, in order (fog, then SSR's cost as a stream of its own), and no interface freezes between parallel streams, so AGENTS.md rule 3 asks for none (the lead's decision, 2026-11-15). Where ADR 0042 or its ledger says "C32.1 names it" or "C32.1's choice", this record decides it. Amended as the wave lands; the lead runs the wave's independent review pass and the milestone end (merge main, the milestone review, `CI.py --require-clang-cl`). Lead-owned, written by the builder.
- **Date:** 2026-11-15.
- **Base:** main `bb51e110` (phase 5's ledger, brief 08's landing and M35's join), branch `milestone/m32`, slot `.claude/worktrees/m32`.
- **Authority:** Roadmap "Rules for every milestone" and M32; ADR 0042 decisions 2, 3 ("M32 Fog"), 5 and 7 and its phase 5 reservation ledger; ADR 0028 decisions 11, 15 and 17; ADR 0023; Architecture §5.3, §6.1, §8.2 to §8.6, §8.14, §13.7, §15.4 to §15.6; ADR 0037 decisions 14 and 18 (the SceneRoughness target and the SSR pass this milestone changes); World Tree's briefs `02-Fog.md` and `09-SsrCost.md`.

## Context

M32 takes World Tree's brief 2 (fog: high, every realm's look) and brief 9 (SSR's cost: low, before the ice realm's mirror floors). It lands in phase 5's readiness order with M28, M29, M31, M34 and M35, and before M33, which applies fog to its own particle program at its merge. The facts this record relies on, each checked in the code at `bb51e110`:

1. **No fog exists.** No component, snapshot member, constant buffer or shader term fogs anything; `PostProcessComponent` (version 2) and `EnvironmentComponent` (version 1) have no fog member.
2. **The forward passes share one set-0 layout.** `Utils::GetForwardViewLayout` (`Renderer/Private/ForwardPipelines.h`) holds b0 `ViewConstants` to b4 `ProbeConstants`, t0 to t14 and s0 to s2, with the draw's push constants at `DrawConstantsSlot` 5 (NVRHI counts push constants as a constant-buffer slot); the Terrain program's forward pipeline shares it (`TerrainPass`), so one forward set of a view serves the meshes and the terrains. `DrawConstantsSlot` is also the push-constant slot of the prepass, overdraw, shadow-caster and terrain layouts.
3. **The forward shading's output.** `Passes/Scene.slang`'s `ShadeForward` returns the debug view's value for a view other than Lit and otherwise `float4(radiance, outputAlpha)` with the fragment's `WorldPosition`; `PSForward` serves the opaque (Opaque and Mask) and transparent (Blend, straight alpha) variants; there is no additive material blend mode (`MaterialAlphaMode` Opaque, Mask, Blend). `Passes/Terrain.slang`'s `PSForward` returns `float4(radiance, 1.0)` likewise.
4. **The skybox** (`Passes/Skybox.slang`, `SkyboxPass`) draws one fullscreen triangle at depth 0 over the pixels the scene left at the cleared depth, with set 0 b0 `ViewConstants`, b2 `EnvironmentConstants`, t4 and s0; it computes each pixel's view ray from `InverseViewProjection` (perspective) or the constant view direction (orthographic).
5. **The SSR pass** (`Renderer/SsrPass.h`, `Passes/Ssr.slang`, ADR 0037 decision 18) is two pipelines of the program Ssr in the Forward stage after the skybox and before the transparent pass: `CSTrace`, dispatched over every 8 × 8 group of the scene image, where each pixel returns 0 at once unless its surface reflects (SceneRoughness word 0's multiplier is not 0: the sky and surfaces without specular are 0) and its roughness (word 0's a) is at most `SsrMaxRoughness`, and otherwise marches the depth pyramid for at most 128 steps; and one fullscreen composite triangle that adds `(traced - prefiltered) * factor` into SceneColor (blend One + One), 0 where the trace's confidence is 0. `SsrColor` is a scene-size `RGBA16_FLOAT` storage image; the push constants (`Shared/SsrConstants.h`, 48 bytes) end with three padding words. With SSR off the pass records nothing. The pass's GPU time is the stats row `SSR`; the depth pyramid it traces over records in the Ambient stage under its own row.
6. **GTAO's half resolution** (`GtaoPass`, `Common/Gtao.slang`): the AO target is `ceil(width / 2)` × `ceil(height / 2)` with `Rendering.SsaoHalfResolution` or Low quality; `GtaoFullPixel` maps an AO pixel to the full-resolution pixel at its footprint's centre, and `SampleGtao` reconstructs a full-resolution pixel from four bilateral taps weighted by `GtaoEdgeWeight` (depth and normal), white when none represents the surface. Both helpers are `public` in the module.
7. **The quads.** The QuadBatcher's six pipelines (`QuadBatchTarget` Hdr, Ldr, HdrLit, Normals, LdrDepth, HdrAdditive) share two binding layouts (t0, s0 and the 80-byte `QuadConstants` push constants at slot 0; the lit one adds t1). Hdr, Ldr, LdrDepth and HdrAdditive use the vertex entry `VSMain`; HdrLit and Normals `VSLit`, which also passes the corner's batch-space position (`BatchPosition`). The Sprites stage records sprites and tilemaps with Hdr or HdrLit in world space through the camera's view-projection (`SceneRenderer::State::RecordSpriteQuads`); the Particles pass (`ParticlePass`, M33's) records with Hdr or HdrAdditive; the UI with Ldr and, for World canvases, LdrDepth.
8. **The snapshot** (`Renderer/RenderSnapshot.h`) ends with M26's `Terrains`; `RenderQualitySettings` holds `ShadowMapSize`, `SsaoHalfResolution` and the three reference paths (`Instancing`, `ClusteredLights`, `Probes`). Render extraction (`Scene/RenderExtraction.cpp`) takes the first effectively enabled `Environment` and `PostProcess` in canonical order, or their defaults.
9. **The probe bakes render through `ProbeCapture`,** whose `MakeCaptureSnapshot` copies the scene's snapshot and turns off what a capture must not see (TAA, SSR, the probes' own term, the overlays); `EnvironmentBaker` prefilters an environment map and renders no scene.
10. **Six hosts copy `ProjectSettings.Rendering` into `RenderQualitySettings`:** `PlaySession.cpp`, `Testing/FeatureTestRunner.cpp`, `Automation/Methods/ScreenshotMethods.cpp`, `Editor/Private/EditorHostViewports.cpp`, `EditorCore/Lighting/LightingBake.cpp` and `EditorCore/Thumbnails/ThumbnailCache.cpp` (whose thumbnail key hashes the quality), with the Runtime's `RuntimeApp.cpp` and the tests' `Support/GoldenScene.cpp`. `RenderingSettings` holds `ShadowMapSize` and `SsaoHalfResolution`; gate 8's `Tests/Data/Project/AllSettings.eproj` sets every field off its default and `Tests/Data/CoverageGaps/Gate8/` leaves `SsaoHalfResolution` at its default.
11. **The renderer's GPU timings** are read back without waiting: `SceneRenderer::Render` uses a timing slot only when that slot's previous submission completed (`TimingSubmissions`, `OnSubmitted`), and `RenderStatsHistory` publishes the CPU stats of this render and the GPU samples of the completed frame (`GpuFrameIndex`). `stats.get`'s per-view summary (`StatsViewSummary`) ends with `ssaoHalfResolution` and `historyFrames`.
12. **Counts.** 52 components in twelve categories ending with Terrain; 33 pass declarations and 73 startup pipelines (`RenderPassDeclaration.h`), the Ssr declaration with 2; 32 programs in `Shaders.json`; 67 validation codes; 120 automation methods.

## Decisions

### 1. Scope and the wave

M32 is one wave, W32.1, built in order as checkpoint commits on `milestone/m32`: this record; fog (decisions 2 to 9); SSR's cost (decisions 10 to 14); each with its tests, FeatureTest coverage, documents and regenerated references. Volumetric light (the brief's stretch item) is not in M32 (ADR 0042, the owner's answer 2a; `Docs/Backlog.md` §2). Particles in fog are M33's (ADR 0042 decision 3: M33 applies fog in its particle program at its merge, with `Common/Fog.slang`'s functions).

### 2. The `Fog` component

A new component, `Fog` (`Scene/Components/FogComponent.h`), version 1, `UniquePerScene` (the defaults' `Serializable | ScriptVisible | EditorVisible | Removable` kept), no requirement (a scene setting, like `Environment` and `PostProcess`), in a new category **Fog** (`Scene/Registration/FogRegistration.cpp`, `RegisterFogComponents`), appended after Terrain in `BuiltinComponents` and in `RegisterBuiltinComponents` (the ledger's categories row: M30's Navigation, M31's MaterialParameters and M32's Fog after Terrain in landing order). Fields, in file order:

| Field | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `Color` | `glm::vec3` (colour) | (0.5, 0.55, 0.6) | [0, 1] | The fog's linear colour |
| `AmbientTint` | `float` | 0 | [0, 1] | How much the environment's ambient light tints `Color`: 0 keeps it, 1 multiplies it by the ambient |
| `Density` | `float` | 0.02 | [0, 10] /m | Distance fog's extinction per metre of view depth |
| `StartDistance` | `float` | 0 | [0, 100000] m | The view depth where fog begins |
| `MaxOpacity` | `float` | 1 | [0, 1] | The most fog any point gets |
| `HeightDensity` | `float` | 0 | [0, 10] /m | Height fog's extinction per metre at `BaseHeight` |
| `HeightFalloff` | `float` | 0.5 | [0.001, 100] /m | How fast height fog thins above `BaseHeight` (and thickens below it) |
| `BaseHeight` | `float` | 0 | any finite value, m | The world height where height fog has `HeightDensity` |
| `SunGlow` | `float` | 0 | [0, 100] | The glow toward the first directional light, times its colour and intensity |
| `SunGlowExponent` | `float` | 8 | [1, 1000] | How tightly the glow gathers around the light's direction |
| `FogSkybox` | `bool` | true | | Whether the skybox is fogged |

The defaults fog a scene lightly as soon as the component is added. No enum, so gate 1b gains nothing. No new validation code: the ranges are `COMPONENT_FIELD_OUT_OF_RANGE`'s, a second Fog is `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s. Scripts read and write every field through the generic component proxy (`entity.Fog.Density`), which makes fog animatable (Roadmap M32); automation through `entity.update`; the inspector shows it with no code. No new script namespace, function or automation method (the ledger's script API and methods rows).

### 3. The fog model

A point P is fogged along its view ray from the eye point E: the camera's position for a perspective camera, P moved back along the camera's forward axis f to the camera's plane for an orthographic one (`E = P - f z`). **Distance is view depth** (ADR 0042 decision 3), `z = dot(P - C, f)`, so an orthographic, isometric camera fogs sensibly and a plane facing the camera fogs evenly. With `L = max(z - StartDistance, 0)`, the fog's path runs over view depths [StartDistance, z]; along it the height starts at `h_s = E.y + (P.y - E.y) StartDistance / z` and changes by `ΔH = (P.y - E.y) L / z`. The optical depth is

`τ = Density L + HeightDensity L e^(-k (h_s - BaseHeight)) G(k ΔH)`, with `k = HeightFalloff` and `G(x) = (1 - e^(-x)) / x`, `G(0) = 1`

(the exact integral of an exponential height density along the path), the fog amount `a = min(1 - e^(-τ), MaxOpacity)`, the fog colour

`F = Color · lerp(1, Ambient, AmbientTint) + SunGlow · SunColor · max(dot(v, s), 0)^SunGlowExponent`

with v the unit view ray (f for an orthographic camera), s the direction toward the first directional light of the snapshot's light list (canonical order) and `SunColor` its `Color · Intensity` (no glow without a directional light), and `Ambient` the environment's average ambient radiance (`IrradianceSH9[0] · Y00`, the SH9's mean irradiance / π, with an environment map; `FallbackColor · Intensity` without). A blended surface outputs `c (1 - a) + F a`; an additive one fades, `c (1 - a)` (ADR 0042 decision 3: additive quads fade rather than tint). The sky (a direction d, at infinite depth) takes the limit: distance fog gives `MaxOpacity` whenever `Density > 0`, and height fog `HeightDensity e^(-k (h_s - BaseHeight)) / (k r)` for a ray that climbs (`r = d.y / dot(d, f)` per unit of view depth, f.y for an orthographic camera), infinite otherwise. Exponents are clamped to ±80 so no term overflows. `CameraPosition`, f, the sun and the ambient are folded into the per-view constants on the CPU, so the shaders need the fog buffer and a world position only.

### 4. The snapshot and the extraction

`RenderFog` (`Renderer/RenderSnapshot.h`) holds `Enabled` and the component's fields; `RenderSnapshot::Fog` is appended after `Terrains` (the ledger's snapshot rule). Render extraction takes the first effectively enabled `Fog` in canonical order (as `Environment` and `PostProcess`), `Enabled` false without one. Every view of a scene that holds fog shows it: the game view, the scene view, captures and thumbnails. `ProbeCapture::MakeCaptureSnapshot` turns it off, so probe bakes exclude fog (ADR 0042 decision 3: fog applies to the baked light when it is shaded); environment bakes render no scene.

### 5. The constants and where fog binds

`Shared/FogConstants.h`: `FogConstants`, 112 bytes (Color, Density; CameraPosition, StartDistance; ViewForward, MaxOpacity; SunDirection, SunGlowExponent; SunColor, HeightDensity; HeightFalloff, BaseHeight, Enabled, FogSkybox; Orthographic and three padding words), made by `MakeFogConstants` (`Renderer/FogConstants.h`, pure) from the snapshot's fog, camera, light list and the view's `EnvironmentConstants`. `Enabled` is 1 only in a Lit view with a camera, outside the wireframe, with an extracted fog: the data views, the Overdraw view and the wireframe are never fogged. Each view's renderer writes it into its own `SceneRenderer.FogConstants` buffer with the other per-view constants (created with them by `RecreateGpuObjects`, so a device loss recreates it). `Common/Fog.slang` is an include file, as `Common/Probes.slang` is: it reads the `ConstantBuffer<FogConstants> Fog` its includer declares and gives `EvaluateFog(worldPosition)` (F in rgb, a in alpha), `EvaluateSkyFog(direction, eye)`, `ApplyFog` and `ApplyFogAdditive`. The bindings, each a binding layout gaining it without a new pipeline (the ledger's render-pass section):

| Program | Register | Read by |
|---|---|---|
| Scene, Terrain (the forward view layout) | b5; `DrawConstantsSlot` moves from 5 to 6 in every layout that uses it (the ledger: "a constant buffer takes b5 and moves `DrawConstantsSlot` up") | `PSForward`'s Lit output, opaque and transparent; Terrain's `PSForward` |
| Skybox | b5 | `PSMain`, with `FogSkybox` |
| Ssr (the composite) | b5 | `PSComposite` |
| Quad | b1 (slot 0 is the push constants') | `PSHdr` and `PSHdrLit` |

The QuadBatcher gets four layouts instead of two (Hdr and HdrAdditive with fog; Ldr and LdrDepth without; HdrLit with fog; Normals without), the Hdr and HdrAdditive pipelines draw with `VSLit` (its clip position is `VSMain`'s expression, so nothing they draw moves), and `QuadConstants`' first padding word becomes `FogMode` (0 none, 1 blend, 2 fade), set from `QuadBatchInputs::Fog` (the view's fog buffer, or null) and the target (HdrAdditive fades). A record without a fog buffer binds the batcher's own neutral buffer (`Enabled` 0), created and written at `Create` with its index buffer. The Sprites stage passes the view's buffer for its world-space sprites and tilemaps; the Particles pass passes none (M33); World-space UI and world text keep their no-fog limit (`Docs/Backlog.md` §6).

### 6. What fog changes and what it keeps

- **Applied alike** to the forward opaque and transparent shading, the terrain, the skybox (with `FogSkybox`), the SSR composite and the 3D sprites and tilemaps: an opaque surface is fogged by the forward pass, and the SSR composite's addition is multiplied by `1 - a` at its pixel, so `(forward + ssr)` fogs as one surface; a transparent surface is fogged before it blends over the fogged surface behind it.
- **A scene without fog renders byte-identically:** every fog term is behind a branch on `Enabled` (0 without fog), and the quad pipelines' new vertex entry computes the same clip position. The check: every existing golden unchanged, `--update-golden` writing no changed image on this tree (recorded under "Verification"), and the GPU case "a fog of zero density changes no pixel".
- **Cost:** a handful of transcendental operations per shaded pixel, no pass and no pipeline; ADR 0042's bound is 0.5 ms at 1920 × 1080 (Acceptance A5).

### 7. Determinism and run modes

Fog is presentation: it changes no simulation state, adds no state-hash block, and `Fog` is component data, serialized and hashed only where a scene holds one (no existing scene does), so no committed hash or replay moves. The component and its proxy exist in every run mode (`RunModes::All`): the editor, the exported Release and Dist Runtime. FeatureTest covers it per gate in each mode (decision 9).

### 8. Goldens

A golden scaffold of its own, `Scaffold/GoldenFog.jsonl` (IdSeed 2902, `GoldenDirectories` `Assets/Materials/GoldenFog` and `Assets/Scenes/GoldenFog`), with four scenes, each a golden image (`Tests/Source/Golden/FogGoldenTests.cpp`): **FogPillars** (a row of pillars receding into distance fog, perspective), **FogHollow** (height fog lying in a hollow below a rim, the upper air clear), **FogSunGlow** (the glow toward a low sun over a fogged skybox) and **FogTransparent** (a transparent pane and a sprite inside the fog). Each test also renders its scene with the fog off and checks that the fog changed it. Every existing golden stays byte-identical (decision 6).

### 9. FeatureTest coverage

A scaffold `Scaffold/Fog.jsonl` (IdSeed 2901): `Assets/Scenes/Fog.scene` holds a `Fog` with every field off its default (gate 1, a scene the suite plays), and `Assets/Tests/Fog/Fog.test.luau`, listed in `Testing.Suites` through `Suites.jsonl`'s one patch line, reads and writes every field through the proxy (gate 4) and checks that a write reaches the next render's snapshot (`Test.GetLastExtraction`), in every run mode. The setting of decision 12 is gate 8's (mode-independent).

### 10. SSR: the per-tile skip

The trace skips, early and per tile, every pixel that does not trace (above `SsrMaxRoughness`, a surface that does not reflect, the sky), so a mostly rough scene pays for its glossy pixels only, with the same image (ADR 0042 decision 3). The Ssr declaration's pipelines become three:

1. **`CSClassify`** (compute, 8 × 8 groups over the trace image): each thread tests its pixel as `Trace` does; a group with a tracing pixel appends its tile (x | y << 16) to the view's tile list (`SceneRenderer.SsrTiles`, one `uint` per tile of the trace image) and counts its tracing pixels, through atomics on the view's argument buffer (`SceneRenderer.SsrArguments`: the trace's dispatch arguments (tiles, 1, 1), the composite's draw arguments (6, tiles, 0, 0), the traced pixels and the tile count), which the stage resets with a write before the dispatch.
2. **`CSTrace`** (compute, dispatched indirectly, one group per listed tile): each pixel of the tile writes `Trace(pixel)`, the function as it is, into `SsrColor`.
3. **The composite** (`VSComposite` and `PSComposite`, drawn indirectly, one instance per listed tile, two triangles covering the tile's pixels): `PSComposite` as it is at each covered pixel.

At full resolution every pixel of a listed tile is traced and composited exactly as before, and a pixel of an unlisted tile added 0 before, so the image is the same: the Ssr golden, the SSR GPU cases and every golden of a scene with SSR stay as they are. The tile order in the list follows the atomics and changes nothing: each tile writes only its own pixels.

### 11. SSR: the half-resolution trace

`Rendering.SsrHalfResolution` (decision 12), copied into `RenderQualitySettings::SsrHalfResolution`, traces at half resolution: `SsrColor` is `ceil(width / 2)` × `ceil(height / 2)` (`SsrPass::GetTargetDesc`), cleared to 0 before the classification (the upsample reads neighbours across tile edges), and each trace pixel traces the full-resolution pixel `GtaoFullPixel` maps it to, over the full-resolution depth pyramid (so a ray costs as at full resolution, and there are a quarter of them). The tiles are 8 × 8 trace pixels (16 × 16 scene pixels), the composite covers each listed tile's scene pixels, and `PSComposite` reconstructs the traced reflection from the four taps and weights of `SampleGtao` (`GtaoFullPixel` and `GtaoEdgeWeight` over the full-resolution `ViewDepth` and `SceneNormals`, now bound to the composite at t3 and t1), premultiplied by each tap's confidence, and adds nothing at a pixel that does not trace itself. `Common/Gtao.slang` is not changed, so GTAO's image is not either. `SsrConstants`' first padding word becomes `HalfResolution`. Off by default, so nothing changes until a game turns it on.

### 12. The setting

`RenderingSettings::SsrHalfResolution` (`bool`, false, "Traces screen-space reflections at half resolution, upsampled with the depth-aware filter"), appended after `SsaoHalfResolution` (the ledger's settings row: M29's and M32's members at the end of `Rendering` in landing order). `ProjectSerializer::CurrentVersion` stays 1 (a reader keeps a missing key's default). Every host of fact 10 copies it with `SsaoHalfResolution`, and the thumbnail key hashes it. Gate 8: `AllSettings.eproj` sets it true, the observation case observes it in the play session's extraction, and the gate's seeded-gap fixture sets it true too, still leaving `SsaoHalfResolution` its one default field. The canonical default project of `MakeValidationFixtures.py` and FeatureTest's `Scaffold/Baseline/FeatureTest.eproj` carry the key; the audited demos (Tetris, Rolling Ball, Breakout, NativeTest) take it through their recorded `Editor --headless --project <P> --upgrade`, which the ledger gives the lander on its merged tree, where M29's member joins it.

### 13. The traced area in `stats.get`

The classification's counts are read back like the GPU timings, without waiting: a frame that records SSR in a timing slot copies its argument buffer's counts into that slot's 16-byte readback buffer, and the render that next uses the slot, its submission completed, reads them for that completed frame. `RenderStats` gains, appended after `HistoryFrames`: `SsrHalfResolution` (the setting the CPU frame rendered with) and the completed frame's area: `SsrAreaAvailable`, `SsrAreaFrameIndex`, `SsrTiles` (the trace image's 8 × 8 tiles), `SsrTracedTiles` and `SsrTracedPixels` (trace pixels), published by `RenderStatsHistory::PublishSsrArea` for each completed frame (unavailable for one that recorded no SSR) and kept by `PublishCpu` as the GPU timings are. `StatsViewSummary` gains `ssrHalfResolution`, `ssrAreaFrame` (exact decimal, empty while unavailable), `ssrTiles`, `ssrTracedTiles` and `ssrTracedPixels`, appended after `historyFrames` (the ledger's `stats.get` row; M28's readout takes the same rows from M28's struct at whichever of M28 and M32 lands second). The editor's Stats panel shows them under the SSR row.

### 14. The pass list, the programs and the GPU objects

The Ssr declaration's `PipelineCount` is 3, so 74 pipelines at startup (`RenderPassDeclaration.h`'s comment and the pipeline cases); no stage, declaration, stats row or program is added (`Shaders.json`'s Ssr program gains the entry `CSClassify`). `SsrTiles`, `SsrArguments` and the readback buffers are the view's renderer state, made on the first render with SSR (`EnsureSsrTargets`) and released with the targets by a resize or a device loss's release, as `SsrColor` is; a recovered view records the next SSR frame with new ones.

### 15. Documents

Architecture §5.3 (the Fog category), §6.1 (`Rendering`), §8.2 (the snapshot's fog), §8.3 (the passes fog reaches, Ssr's three pipelines, 74 at startup), §8.4 (b5 and the moved push-constant slot), §8.6 (the fogged skybox) and a fog paragraph, §13.7 (`stats.get`'s SSR area); the game-building skill's `look` and `post-processing` pages; the golden-images skill's scene list; the game-debugging skill (the stats rows); `Docs/Backlog.md` §6 (world UI and world text unfogged; particles unfogged until M33; the half-resolution composite's tile edges); the generated references.

## The wave

**W32.1** (fog, then SSR's cost, in checkpoint commits):
1. This record.
2. Fog: the component and its category, the snapshot and extraction, the constants and `Common/Fog.slang`, the forward, terrain, skybox, SSR-composite and quad paths, the bake exclusion; the unit and GPU cases; the goldens; FeatureTest's `Fog.jsonl` and `GoldenFog.jsonl`; the documents and references.
3. SSR's cost: the classification, the indirect trace and composite, the half-resolution trace and its setting, the traced area in `stats.get`; the unit, GPU and Python cases; gate 8; the documents and references.

## Acceptance

Each line is followed by the check that proves it.

1. **Golden images: pillars receding into distance fog, height fog in a hollow, the sun glow and a transparent surface inside the fog** (Roadmap M32; brief 2's "How the game will check it" but its particle cloud, which is M33's). "Golden: FogPillars", "Golden: FogHollow", "Golden: FogSunGlow" and "Golden: FogTransparent", each also checking that its fog changes the image.
2. **Fog applied alike** to opaque and transparent meshes, terrain, sprites and the SSR composite, the skybox optionally (brief 2 item 4). GPU cases "Fog: …" over synthetic snapshots against a double-precision reference of decision 3 (`Test::EvaluateFog`): a fogged mesh, terrain and sprite at known depths and heights; the additive fade; the skybox with and without `FogSkybox`; the SSR composite's fogged addition.
3. **Animatable from scripts and automation** (brief 2 item 5). FeatureTest's `Fog.test.luau` (gate 4 in every mode); the automation case `test_fog.py` (`entity.update` of a Fog's fields reaching the next capture).
4. **A scene without fog renders byte-identically** (Roadmap M32). The golden suite unchanged and `--update-golden` writing no changed image on this tree; the GPU case "a fog of zero density changes no pixel".
5. **Fog costs under 0.5 ms at 1920 × 1080** (Roadmap M32; brief 2 item 6). A measured run recorded under "Verification": the forward, terrain and skybox passes' GPU time of a fogged 1920 × 1080 view against the same view with its fog off.
6. **SSR's cost follows the glossy area, with the same image** (Roadmap M32; brief 9). "Golden: Ssr" and every golden with SSR unchanged; the GPU cases "SsrPass: the trace covers only the tiles that hold a tracing pixel" (the read-back counts of a floor with a known glossy area) and "SsrPass: a rough floor traces no tile"; a measured run recorded under "Verification": the SSR row's GPU time at 1920 × 1080 of a floor 8/35 glossy against the same floor all glossy.
7. **A half-resolution trace** beside `SsaoHalfResolution`, off by default (ADR 0042 decision 3). The GPU case "SsrPass: a half-resolution trace reflects what the full one does" (the mirror snapshot's classified pixels within a tolerance); the settings observation case and gate 8; the automation case of decision 13.
8. **`stats.get` reports the area traced** (brief 9). The unit case of `MakeStatsViewSummary`, the renderer case "SceneRenderer: the stats report the traced SSR area of a completed frame", and the automation case `test_stats_ssr_area.py`.
9. **FeatureTest coverage** in every run mode (Roadmap rule 5): the feature and export suites' gates 1 and 4 for `Fog`, gate 8 for the setting.
10. **The gate:** `CI.py --require-clang-cl` on the milestone's final tree (the lead's).

## Risks

- **Byte identity.** The fog branch, the quads' vertex entry and the tiled composite must not move a pixel of an existing image; the goldens (with their 2/255 tolerance) and a byte-for-byte `--update-golden` run are the check, and any moved image is a bug to fix before the commit.
- **Merging the renderer's shared files.** M29, M31 and M33 change `Scene.slang`, `Terrain.slang`, the forward layout's registers and `SceneRenderer.cpp`; the ledger's regions hold, and the later lander renumbers its own register after main's.
- **Indirect arguments.** The classification's atomics and the indirect dispatch and draw are new to the renderer; the validation and synchronization layers in the gpu suite, and the read-back counts, are the check.
- **The half-resolution image** is new and has no golden; tile edges where only a non-representative pixel is glossy get no reflection, an accepted limit (`Docs/Backlog.md` §6).

## Verification

Filled as the wave lands: the targeted runs of each checkpoint, the byte-identity run of the goldens, and the measured runs of Acceptance 5 and 6.

## Before milestone completion

- [ ] W32.1 committed as checkpoints, each reviewed with every finding fixed, strict (no stub, no skip).
- [ ] Acceptance 1 to 9 met, each by its named check, with the measured runs recorded.
- [ ] Architecture, the skills, `Docs/Backlog.md` and the generated references describe what landed.
- [ ] The lead's wave review with every finding fixed; the milestone end (merge main, the milestone review, `CI.py --require-clang-cl`), the demos' recorded upgrades for the setting at the landing, and the report to the World Tree session.
