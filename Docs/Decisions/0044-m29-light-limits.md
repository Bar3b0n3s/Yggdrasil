# 0044 — M29 light limits

- **Status:** accepted; M29's milestone record. M29 has no contract (ADR 0028 decision 11; the lead's decision of 2026-11-15 for phase 5): one builder works its one wave, W29.1, in checkpoint commits on `milestone/m29`, and freezes no interface between parallel streams. Where ADR 0042 or its ledger says "C29.1 names it" or "C29.1's choice", this record decides it. It is amended as each checkpoint lands; the lead's review of the wave, the landing (merge of main, the milestone review, `CI.py --require-clang-cl`) and the fast-forward are recorded by the lead. Lead-owned.
- **Date:** 2026-11-15.
- **Base:** main `bb51e110` (phase 5's reservation ledger at `bc7de792` and brief 08's waiver record), branch `milestone/m29`, milestone slot `D:/Engine/.claude/worktrees/m29`.
- **Authority:** Roadmap "Rules for every milestone" and M29; ADR 0042 decisions 1 to 3 (M29's design defaults), 5, 6 and the phase 5 reservation ledger (every row that names M29); ADR 0028 decisions 10, 11, 15 and 17; ADR 0023; ADR 0034 decisions 3 to 5 (M23's point shadows, light loop and clusters, which this milestone extends); ADR 0037 decision 10 (the temporal-state section, whose history reset the light history and the shadow cache follow); Architecture §6.1, §8.2 to §8.4, §8.14, §13.5, §13.7 and §15.4 to §15.6. World Tree's brief `Projects/WorldTree/Design/EngineTasks/07-LightLimits.md` (high priority).

## Context

M29 raises the light limits World Tree's fire realm and horde mode need (brief 07): the shaded lights per view become a project setting up to 2,048, 16 point lights and 16 spot lights per view cast shadows, the shadowed lights are chosen with hysteresis, and a shadow map whose light and casters did not change is reused. Below the old caps nothing changes. ADR 0042 decision 3 sets the defaults; this record makes them exact.

The facts this record relies on, checked in the code at `bb51e110`:

1. **The shaded-light cap is a constant.** `MaxVisibleLights` (256, `Renderer/RenderPrepare.h`) caps `CullLights`' list: every directional light in snapshot order, then the visible point and spot lights by descending importance (`Intensity * max(Color) / max(distance to the light's sphere, 1)²`), ties by entity UUID then snapshot index; the rest are `Dropped`. The light buffer (t0) is created for `MaxVisibleLights` entries (`SceneRendererRecovery.cpp`), and the renderer logs `RENDER_LIGHT_LIMIT_EXCEEDED` once per renderer when a view drops lights.
2. **The cluster masks' word count is compiled in on the CPU only.** `LightClusterWordCount` (8, `Shared/LightClusterConstants.h`) sizes the mask buffer (`LightClusterPass::GetMaskBufferDesc`), the dispatch (`TilesX * TilesY * LightClusterWordCount` threads per slice) and `LightClusterConstants::Grid.w` (`MakeLightClusterConstants`). Every shader reads the stride from `Clusters.Grid.w`: `Passes/LightClusters.slang`'s `CSMain` (tile and word of a thread, `end = min(View.LightCount, first + 32)`, so a word past the list writes 0), `Common/LightLoop.slang`'s `ShadeDirectLights` (mask offset `cluster * Grid.w`, words visited while `word * 32 < View.LightCount`) and `Passes/SceneDataView.slang`'s LightCount view (every word up to `Grid.w`). So a larger `Grid.w` with at most 256 lights writes and reads the same bits.
3. **Spot shadows.** `MaxSpotShadowLights` (8, `Renderer/SpotShadowAtlas.h`): `AllocateSpotShadowAtlas` ranks the visible shadow-casting spots afresh each render (`CullLights`' importance, UUID, index) and puts tile `i` at `(i % 4, i / 4) * 1024` of the 4096 atlas, so the eight tiles fill its first two rows and rows 2 and 3 are free. `ShadowConstants::Spots[8]`.
4. **Point shadows.** `MaxPointShadows` = `MaxPointShadowLights` = 4 (`Shared/ShadowConstants.h`, `Renderer/PointShadowMaps.h`, a `static_assert`): the first four visible shadow-casting point lights in `CullLights`' order, `Layer` = plan index, faces of `GetPointShadowFaceSize(ShadowMapSize)` (a quarter, at least 128) in the `PointShadowMaps` cube array of 24 layers (t12). `ShadowPass::Record` clears the planned layers, then draws six faces per light with the reflected caster variants; the caster-constant slots are 4 cascades, 8 spot tiles and 24 point faces (`ShadowCasterSlotCount` 36, 256 bytes each).
5. **The shader searches the shadowed lights.** `Common/Shadows.slang`'s `SampleSpotShadow` and `SamplePointShadow` loop over `Counts.y` spots and `Counts.w` points comparing `Indices.x` with the uploaded light index; `ShaderLight` (64 bytes) ends with two padding floats (`Padding0`, `Padding1`).
6. **No hysteresis.** `CullLights`, `AllocateSpotShadowAtlas` and `AllocatePointShadows` are pure and rank each render afresh, so lights at a cap pop as their importance changes.
7. **The view's history and the renderer's state.** A render continues its view's temporal history when its snapshot has a camera with `CameraData::PreviousViewProjection` (`SceneRenderer.cpp`'s `HistoryFrames`); a `RenderViewHistory` reset, a camera cut, a scene change and every capture's first warm-up render leave it empty (`TemporalHistory.h`, `ViewportCapture::RenderWarmup`), and hosts that apply no history (tests, thumbnails, `lighting.bake`'s captures) never set it. A device loss releases and rebuilds the renderer's state (`SceneRendererRecovery.cpp`). Shader hot reload bumps `SceneRendererPipelines::GetGeneration()`; an asset's GPU mirror changes `GpuMesh::Version` and `GpuMaterial::Generation`.
8. **The shadow targets.** `EnsureFrameTargets` acquires the cascades (on a size change), the spot atlas (once) and the point maps (on a face-size change) from the renderer's own `RenderTargetPool`, lazily. NVRHI clears whole subresources only (`clearDepthStencilTexture`), never a rectangle of one.
9. **Validation.** `CheckLightLimit` (`ProjectValidator.cpp`) counts a scene's enabled lights against `MaxVisibleLights`; `CheckRenderScene` compares `EvaluateRenderSceneValidation`'s shadow-casting spot and point counts (`Scene/RenderAnnotations.cpp`) with `MaxSpotShadowLights` and `MaxPointShadowLights`. Their fixtures (257 lights, 9 spots, 5 points) come from `Tests/Data/Generate/MakeValidationFixtures.py`.
10. **Settings and extraction.** `RenderingSettings` holds `ShadowMapSize` and `SsaoHalfResolution` (`Project/ProjectSettings.h`). Seven requests copy them into `RenderExtractionRequest::Quality` (`RenderQualitySettings`): `PlaySession.cpp`, `Testing/FeatureTestRunner.cpp`, `Automation/Methods/ScreenshotMethods.cpp`, `Editor/Private/EditorHostViewports.cpp`, `EditorCore/Lighting/LightingBake.cpp`, `EditorCore/Thumbnails/ThumbnailCache.cpp` (which also hashes the quality into its cache key) and `Runtime/RuntimeApp.cpp`'s screenshot request; `ExtractRenderSnapshot` refuses a shadow size outside its rule. `ProjectSerializer` keeps a missing key's default, so a member appended with its default needs no project version.
11. **Stats.** `RenderStats` and `stats.get`'s view summary carry `ShadowDraws`, the spot and point shadow counts and drops, the quality settings and `HistoryFrames`; `SceneRenderStats` counts `Lights`, `CulledLights` and `DroppedLights` (M28 brings the last two to `stats.get`, the ledger's stats row).

## Decisions

### 1. The shaded-light cap: `Rendering.MaxShadedLights`

- **The setting.** `RenderingSettings::MaxShadedLights` (`uint32_t`, default 256), appended after `SsaoHalfResolution` (the ledger's settings row), registered as "The most lights one view shades: 256, 512, 1024 or 2048. More visible lights drop the least important ones (RENDER_LIGHT_LIMIT_EXCEEDED)." with `Min` 256, `Max` 2048 and the unit `lights`. `ValidateRendering` refuses any other value ("must be 256, 512, 1024 or 2048"); the settings generator picks one of the four. `Renderer/RenderPrepare.h` names the rule: `DefaultMaxShadedLights` (256), `MaxShadedLightsCeiling` (2,048) and `IsValidMaxShadedLights(value)`.
- **The snapshot.** `RenderQualitySettings::MaxShadedLights` (default 256), after `SsaoHalfResolution` (the ledger's snapshot rule). Every extraction request that copies `Rendering` copies it beside `ShadowMapSize`, one designated initializer each, and `ThumbnailCache` hashes it into its key with the other quality members. `ExtractRenderSnapshot` refuses an invalid value beside the shadow-size check (`InvalidArgument`). Hosts that build a snapshot by hand (tests, `ProbeCapture`) keep the default.
- **`CullLights(lights, camera, maxShadedLights, previous)`** shades at most `maxShadedLights` lights (asserted valid); `MaxVisibleLights` is removed, and every user reads the cap in force. The renderer's warning names it ("{} lights exceed the view's budget of {}").
- **The light buffer** is created once per renderer for `MaxShadedLightsCeiling` entries (2,048 × 64 bytes = 128 KiB), so changing the setting never recreates it.
- **The cluster masks follow the cap at run time.** `LightClusterGrid` gains `WordCount` (`MaxShadedLights / 32`: 8, 16, 32 or 64), which `ComputeLightClusterGrid` takes as its last argument; `MakeLightClusterConstants` writes it into `Grid.w`, `LightClusterPass::GetMaskBufferDesc(clusterCount, wordCount)` sizes the buffer (at 1920 × 1080: 30 × 17 × 24 clusters, 1.57 MB at 1,024 lights, 3.13 MB at 2,048) and the dispatch runs `TilesX * TilesY * WordCount` threads per slice. `LightClusterWordCount` becomes `MaxLightClusterWordCount` (64), the ceiling's. The shaders do not change (fact 2), so a cap above 256 with at most 256 lights renders byte for byte as the default.
- **The 32-pixel tile** stays out (ADR 0042 decision 3; `Docs/Backlog.md` §2's row).

### 2. Shadowed lights: 16 point lights in two tiers, 16 spot lights

- **Caps.** `MaxSpotShadowLights` 16 and `MaxPointShadowLights` 16 (`MaxSpotShadows` and `MaxPointShadows` in `Shared/ShadowConstants.h`, the shader's names, equal to them by `static_assert`).
- **Spot tiles.** The 16 tiles of 1024 fill the existing 4096 `ShadowAtlas`, tile `t` at `(t % 4, t / 4) * 1024`; tiles 0 to 7 are where they are today.
- **Point tiers.** The four most important shadowed point lights (decision 3's tier rank) are the full tier: today's `PointShadowMaps` (t12, 4 × 6 layers of `GetPointShadowFaceSize`). The other twelve are the half tier: a second cube array, **`PointShadowMapsHalf`** (t15, 12 × 6 layers of `GetPointShadowHalfFaceSize(ShadowMapSize)`, half the full face, at least 64 texels), the second target of the PointShadows declaration (the ledger's render-pass row), created lazily like the first when a half-tier light is planned and bound to the `EmptyPointShadows` placeholder otherwise. Its face texel size feeds the light's `Bias.z`, so the normal offset scales with the coarser map. Memory at the default `ShadowMapSize` 2048: 25.2 MB full, 18.9 MB half (D32). The startup pipeline count stays 73: the half tier draws with the Shadows declaration's six caster pipelines.
- **`PointShadowLight`** gains `Tier` (`PointShadowTier::Full` or `Half`) and `Entity`; `Layer` is its layer group within its tier's array. **`SpotShadowTile`** gains `Entity`; its `X`, `Y` and `UvScaleBias` follow its tile slot (decision 3).
- **`ShadowConstants`** grows in place: `Spots[MaxSpotShadows]` (16 × 128 bytes) and `Points[MaxPointShadows]` (16 × 64 bytes), 3,728 bytes, every member after `Spots` moving with it (the shader reads members by name; the reflection case "Shaders: SharedStructsMatchReflection" pins the new offsets). `PointShadowConstants::Indices` is (uploaded light index, layer group, tier, 0).
- **The shadow slot.** `ShaderLight::Padding0` becomes `ShadowSlot` (`uint32_t`, `ShaderLightNoShadowSlot` = 0xFFFFFFFF): a spot light's index in `ShadowConstants::Spots`, a point light's in `Points`, and no slot otherwise (directional lights keep `Counts.z`'s cascades). `GetLightVisibility` reads `Spots[ShadowSlot]` or `Points[ShadowSlot]` directly (a slot at or beyond `Counts.y` or `Counts.w` is unshadowed, as a failed search was), and samples the point light's tier's array; `SampleSpotShadow` and `SamplePointShadow` take the entry instead of searching. Below the old caps every light finds the entry its search found, so the image is the same.
- **Caster slots.** `ShadowCasterSlotCount` becomes 4 + 16 + 16 × 6 = 116 (29.7 KB); ShadowPass records the full tier's faces, then the half tier's, both in the PointShadows row, and clears only the layer groups it draws (decision 4).

### 3. Hysteresis

A light that held its place at a cap in the view's previous render keeps it unless a newcomer is clearly more important, so lights at a cap stop popping.

- **The rule.** Wherever more candidates than places compete (the shaded lights at `MaxShadedLights`; the shadowed spots at 16; the shadowed point lights at 16; the full point tier at 4), a candidate that held such a place in the previous render ranks with its importance times **`LightHysteresis` = 1.25**; the others rank with their importance (ties as today: UUID, then snapshot index). The chosen lights are then ordered by their plain importance, as today, so the light list's order, the spot tiles' and point planes' order and every sum stay the plain-importance ones. Where the candidates fit (the old caps' case), all are chosen and the rule changes nothing.
- **Identity.** A light is `LightKey {Entity, Type}`; a light without an entity (a snapshot built by hand) never holds a place.
- **Stable slots.** A spot that keeps its shadow keeps its atlas tile, and a point light that keeps its tier keeps its layer group; a newcomer takes the lowest free slot, in importance order. After a reset the slots are the importance order (tile and layer `i` for the `i`-th), as today.
- **The pure functions.** `CullLights(lights, camera, maxShadedLights, previousShaded)` (the sorted keys of the previous render's shaded point and spot lights; `LightCullResult::Shaded` returns this render's), `AllocateSpotShadowAtlas(lights, visible, camera, previous)` and `AllocatePointShadows(lights, visible, previous)` take the previous render's plan (null: none) and stay pure, so each rule is a CPU unit case.
- **The history.** `SceneRenderer::State::LightHistory` keeps the previous render's shaded keys and its spot and point plans. It is used only when the render continues its view's history (fact 7), and is cleared otherwise; a capture therefore starts from the plain ranking, and the device loss's release drops it with the state.

### 4. The static shadow cache

A shadowed point light, or the spot atlas, whose light and casters did not change since the render that drew its map reuses that map instead of clearing and drawing it again (ADR 0042 decision 3; owner's answer 2b).

- **The signature.** `ShadowPass::PrepareCasters` gives each caster draw a 64-bit `Signature` (XXH64 of its world matrix, mesh handle and `GpuMesh::Version`, submesh range, caster variant and reflected variant, its Mask material's handle and `GpuMaterial::Generation`, and for a deformed caster its entity, joint palette and morph weights). A point light's signature is XXH64 of its tier, layer group, face size, near and far planes, each face's view-projection and the signatures of the draws each face records after its frustum culling; the spot atlas's is that of every tile's slot, view-projection and draws. So a moved, re-meshed, re-materialized, deformed, hidden, shown or added caster inside the map's frusta, and a moved or re-ranged light, changes it; a caster outside every frustum does not.
- **The cache** (`ShadowMapCache`, renderer state beside the light history, passed to `ShadowPass::Record` as `ShadowRenderInputs::Cache`) keeps each point layer group's signature per tier and the atlas's signature, with the pipelines' generation they were drawn under. Record compares before recording: a match skips the light's clear and its six faces (the spot atlas all its tiles), counted as cached; otherwise it clears that light's layer group (the whole atlas) and draws it, and stores the signature.
- **What drops it:** a render that does not continue its view's history (fact 7: a history reset, a camera cut, a scene change, every capture's first warm-up render, every host without history, `ShadowRenderInputs::Cache` null), a new shadow target (`EnsureFrameTargets` creating or re-acquiring the atlas or either cube array), a shader reload (the pipelines' generation) and a device loss (the state's release). A capture's later warm-up renders draw the same snapshot, so a map they reuse is the one their first render drew: every capture and golden renders as before.
- **The spot atlas is cached whole** (fact 8: no rectangle clear): its tiles are reused together when every tile's light and casters are unchanged, and drawn together otherwise. Caching single tiles would need a tile-clear pipeline; a kept limit (`Docs/Backlog.md` §6).
- **The cost when nothing moves:** the PointShadows and SpotShadows rows record no draw for a cached light, so `shadowDraws` and their rows' draw counts fall to the uncached lights'.

### 5. Stats

- **`RenderStats`** appends `MaxShadedLights` (the snapshot's cap; 0 while unavailable), `CachedPointShadows` and `CachedSpotShadows` (the shadowed point and spot lights whose maps the render reused), after `HistoryFrames`' group and the M23 counters, in the ledger's landing order after M28's rows.
- **`stats.get`'s view summary** appends `maxShadedLights`, `cachedPointShadows` and `cachedSpotShadows`, with descriptions; the editor's Stats panel shows "Shaded light cap" and the two cached counts beside the shadow rows. The existing `shadowedPointLights` and `shadowedSpotLights` report up to 16 each. M28's readout carries the three rows at the later lander's merge (the ledger's stats row).

### 6. Validation

The three budget codes keep their spellings, severities and fixability, reworded in place (the ledger's validation-codes row):
- `RENDER_LIGHT_LIMIT_EXCEEDED`: a scene with more enabled lights than the project's `Rendering.MaxShadedLights`; the message names the cap and the hint the setting ("raise Rendering.MaxShadedLights, disable or remove lights, or keep fewer in view at once").
- `RENDER_SPOT_SHADOW_BUDGET`: more than 16 shadow-casting spot lights; `RENDER_POINT_SHADOW_BUDGET`: more than 16 shadow-casting point lights (each view shadows the 16 most important, the four most important points at full resolution).
- Fixtures: `MakeValidationFixtures.py` writes 17 spots and 17 points; the light-limit fixture keeps 257 lights at the default cap. The game-debugging skill's `validation-codes` rows and the codes' `ValidatorCodeInfos` descriptions follow.

### 7. Determinism and run modes

Everything here is presentation: the cap, the tiers, the hysteresis and the cache change what a view draws, never the simulation, the state hash or a replay. The light history and the shadow cache are renderer state keyed by the view's history, never by the wall clock or an unordered container (keys are sorted vectors). The setting is a project setting, read in every host and run mode that renders (editor play, the Runtime in Release and Dist, FeatureTest's runs, captures, thumbnails, `lighting.bake`); headless and `--renderer none` runs ignore it.

### 8. FeatureTest coverage, goldens and the projects' upgrade

- **Settings (gate 8):** `Tests/Data/Project/AllSettings.eproj` and `Tests/Data/CoverageGaps/Gate8/AllSettings.eproj` set `MaxShadedLights` 1024, and `ProjectSettingsObservationTests.cpp` observes it in the session's extraction (`/Rendering/MaxShadedLights`).
- **FeatureTest:** a scaffold `LightLimits.jsonl` (IdSeed 2601, `--renderer none`) sets `Rendering.MaxShadedLights` to 1024 in `FeatureTest.eproj` (a value observable at run time, §15.5), and a golden scaffold `GoldenManyLights.jsonl` (IdSeed 2602, `GoldenDirectories` `Assets/Materials/GoldenManyLights` and `Assets/Scenes/GoldenManyLights`) writes `Assets/Scenes/GoldenManyLights/ManyLights.scene`: a floor with blocks, 1,000 small point lights in a grid, and twelve shadow-casting point lights and twelve shadow-casting spot lights beyond the old caps. Both are listed after main's scaffolds and before `Suites.jsonl` and `Replays.jsonl`. No new component, field, enum value, script API or method, so gates 1 to 5, 1b, 7 and 9 gain no entry; gate 6's three codes keep their fixtures.
- **Goldens:** `ManyLights` (the lit image) is the one new golden on `nvidia-62x`; every existing golden stays byte-identical. The golden-images skill's scene list takes its line.
- **The audited projects** (Tetris, Rolling Ball, Breakout, NativeTest): the recorded `Editor --headless --renderer none --audio-device none --user-data-dir <scratch> --project <P> --upgrade` writes `"MaxShadedLights": 256` into each `.eproj` canonically, keeping every hash, then the provenance audit; FeatureTest through `Scaffold.py`.

### 9. Documents

Architecture §6.1 (the setting), §8.2 and §8.3 pass 1 (the cap, the hysteresis), §8.4 (the shadow tiers, the slot lookup, the cache), §13.5 (`stats.get`'s rows) and §13.7 (the codes); the game-building skill's `look` page (light budgets and shadows), the game-debugging skill's `SKILL.md` shadow entry and `validation-codes` rows, the golden-images skill's scene list; `Docs/Backlog.md` §1 (M29's row when it lands) and §6 (the whole-atlas cache); the generated references (`Project.schema.json`, `Methods.json`'s `stats.get`).

## The wave

One wave, W29.1, in checkpoint commits, each with its tests and documents:

| Checkpoint | Holds |
|---|---|
| 1 | This record |
| 2 | Decision 1 (the setting, the cap, the run-time masks, the light buffer), decision 5's `maxShadedLights`, decision 6's light-limit rewording, gate 8, the projects' upgrade |
| 3 | Decisions 2 and 3 (the shadow caps, the tiers, the slot lookup, the hysteresis), decision 6's two shadow codes, the shadow GPU cases |
| 4 | Decision 4 (the cache) with decision 5's cached counts and the invalidation case |
| 5 | Decision 8's FeatureTest scaffolds and the `ManyLights` golden, decision 9's documents, the references |

## Acceptance

Roadmap M29's acceptance and brief 07's "How the game will check it", each with the case or check that states it:

- **A scene of 1,000 small point lights renders every one with no drop warning, and its golden matches the brute-force loop:** "Golden: ManyLights" (the snapshot of `ManyLights.scene` at `MaxShadedLights` 1024: every light shaded, `DroppedLights` 0 and no `RENDER_LIGHT_LIMIT_EXCEEDED` warning; the brute-force loop and the per-draw render equal the clustered, instanced render byte for byte; the golden); "RenderPrepare: CullLights shades up to the cap it is given and drops the least important beyond it"; "LightClusterPass: the grid's word count follows the shaded-light cap".
- **Below the old caps every golden, the brute-force equality and FeatureTest's probe bakes stay byte-identical:** the golden suite (every existing golden unchanged, "Golden: ClusteredLights"' brute-force equality among them), "SceneRenderer: a cap above 256 renders a scene of at most 256 lights byte for byte as the default", the feature suite's byte-compared probe bakes, and the PointShadows and SpotShadows goldens through the slot lookup.
- **16 shadowed point lights (four at full size, the rest at half) and 16 shadowed spot lights per view, with hysteresis:** "PointShadowMaps: sixteen point lights are planned, the four most important at full face size and the rest at half", "PointShadowMaps: a light that held its shadow or its tier keeps it until a newcomer is a quarter more important", "SpotShadowAtlas: sixteen spot lights take the atlas's sixteen tiles, and a kept light keeps its tile", "RenderPrepare: a light shaded at the cap stays shaded until a newcomer is a quarter more important"; GPU: "SceneRenderer: sixteen shadowed point and spot lights each darken their own receivers".
- **A cached shadow is drawn again when any kind of caster in its range changes, and the shadow pass costs less when nothing moves:** GPU: "ShadowCache: an unchanged view reuses its shadow maps and draws no caster" and "ShadowCache: a moved, skinned, morphed, hidden, re-materialized or added caster, or a moved light, draws its map again", each comparing the cached image with an uncached render byte for byte.
- **Stats and validation report the caps:** "StatsMethods: stats.get reports the shaded-light cap and the cached shadow counts" (C++) and `Tests/Automation/test_renderer_ii.py`'s `stats.get` test; `project.validate` on the three codes' fixtures (gate 6) and "ProjectValidator: the light limit follows the project's MaxShadedLights".
- **Brief 07's frame-time check** (the fire realm at 60 fps at 1920 × 1080 on the development machine) and **horde mode's panel** are the World Tree session's to check on its scenes; the engine's side is the cached counts and `stats.get`'s cap above, and M28's readout carries them.

## Risks

1. **Byte identity through the slot lookup.** The shader's direct index replaces a search; a compiler that schedules the same arithmetic differently could move a pixel. The PointShadows, SpotShadows and ClusteredLights goldens are the check, run at checkpoint 3.
2. **A missed invalidation shows a stale shadow** (ADR 0042's risk). The signature covers every input of a map's depth (decision 4); the invalidation case moves each kind of caster and the light, and compares with an uncached render.
3. **Memory at the largest settings.** 16 point lights at `ShadowMapSize` 8192 take 402 MB (full) and 302 MB (half) of D32; the setting is the project's, and the default costs 44 MB.
4. **A whole-atlas cache** redraws all 16 spot tiles when one changes (decision 4): no worse than today, accepted.

## Open

- **For the lead:** `Runtime/RuntimeApp.cpp` (M28's file) copies `Rendering` into its screenshot request's `Quality` (fact 10) but is not in the ledger's extraction-requests row; decision 1 adds its one designated initializer there, as in the six listed files, which needs the ledger's amendment.

## Before milestone completion

- [ ] Checkpoints 1 to 5 committed, each with its targeted runs and review.
- [ ] The wave's suites run once, unfiltered: unit, gpu, golden, feature and automation (the renderer's, the validator's and the settings').
- [ ] The lead's review of the wave, every finding fixed.
- [ ] The landing: main merged, the ledger's redo, the milestone review, `CI.py --require-clang-cl` green on the final tree, main fast-forwarded.
