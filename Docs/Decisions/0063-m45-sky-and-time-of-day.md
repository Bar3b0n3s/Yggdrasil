# 0063 — M45 Sky and time of day

- **Status:** accepted, the milestone's plan and record, in phase 8 of the Skerries program (`Docs/Decisions/0060-skerries-engine-program.md`). M45's builder amends this record on `milestone/m45` as each wave lands, in "The W45.1 record" to "The W45.4 record", making decision 3 exact where it leaves a choice. Lead-owned.
- **Date:** 2026-11-24.
- **Decided by:** the lead (the milestone, its design defaults, its ledger needs and the research's questions, decision 4), on the product owner's answers to ADR 0060's questions 6 and 8.
- **Base:** main `3aaf4b10`, engine 0.8.0. Every value this record calls today's was read from the code and data at that commit.
- **Authority:**
  - Roadmap "Rules for every milestone"; ADR 0023; ADR 0028 decisions 2, 10, 11 and 17; ADR 0051;
  - ADR 0060: decision 2's phase 8 rows, decision 4's M45 default, decision 6's M45 row, decision 7, decision 9's S03 corrections, the owner's answers 6 and 8;
  - ADR 0013 decision 9 (M8's environment contract); ADR 0029 (M18's stages, recovery, out of memory); ADR 0037 decisions 8, 9 and 16 (probes); ADR 0047 decisions 3 to 5 (fog); ADR 0059 (M42's mirror views);
  - Architecture §5.3, §6.8, §8.2 to §8.7, §8.13, §8.14, §13.7 and §15.
- **Brief:** Skerries' S03 (`Projects/Skerries/Design/EngineTasks/S03-SkyAndTimeOfDay.md`, high priority, the vertical slice). Design source: `bin/Reviews/P8/research/sky.md`, its S03 section and "Across the group".

## Context

- **The brief.** A day of about 24 minutes, a camp time-lapse and a night lit by beacons need:
  - a procedural, physically based sky driven by the sun light's direction, with a sun disc, a moon (direction, phase), stars that turn, haze and overcast, and a welcome cloud layer;
  - image-based light regenerated from that sky at run time, cheaply, presentation only;
  - probes that keep a cave mouth dark at night and lit at noon without a bake per frame;
  - optionally the sun's colour from the atmosphere, so a script only moves the sun.
- **What the engine has today** (each read at `3aaf4b10`):
  - **Environment.** `EnvironmentComponent {Environment, Intensity, Rotation, ShowSkybox, SkyboxBlur, FallbackColor}` (`Scene/Components/EnvironmentComponent.h:13-21`), version 1, `UniquePerScene`, default flags, so script-writable (`Scene/Registration/RenderingRegistration.cpp:275-288`; `Resources/Scripting/Engine.d.luau:884-897`). Extraction keeps the first enabled one (`Scene/RenderExtraction.cpp:850-861`) as `RenderEnvironment` (`Renderer/RenderSnapshot.h:398-406`).
  - **IBL is baked at import only** (Architecture §8.6). `EnvironmentBaker` makes the skybox cube, the GGX prefilter (256² × 7, `Asset/EnvironmentData.h:128-129`) and the SH9 by a GPU reduction, then reads them back on the main thread (`Renderer/EnvironmentBaker.h:12-35`, `:100-107`). Its push constants carry each dispatch's sample count (`Shared/EnvironmentBakeConstants.h:23-35`).
  - **How the IBL reaches the shaders.** `MakeEnvironmentConstants` writes `SH9 × Intensity` into `EnvironmentConstants` (b2, 192 bytes, M8-frozen; `Renderer/Private/LightingInputs.cpp:48-78`, `Shared/EnvironmentConstants.h:23-45`). Prepare picks the map's specular cube for t3 (`Renderer/SceneRenderer.cpp:1864-1873`), and `RecordViewSetup` writes b2 and b5 every frame (`:1559-1560`). `SkyboxPass` draws the depth-0 pixels after `TerrainForward` (`:2447-2460`; `Renderer/SkyboxPass.h:14-31`), in mirror views too (`Renderer/SceneRendererReflections.cpp:440-449`).
  - **Fog.** `FogConstants.Color` is `Fog.Color` tinted by the SH9's mean, and the glow follows the light list's first directional light (`Renderer/Fog.h:23-32`; `Shared/FogConstants.h:22-37`).
  - **Cascades.** Prepare gives them to the first visible shadow-casting directional light in snapshot (hierarchy) order that the planner accepts (`SceneRenderer.cpp:1691-1730`; Architecture §8.7).
  - **Probes.** `IrradianceVolumeData` is format 1, SH9 per probe (`Asset/IrradianceVolumeData.h:17-37`, `:88-93`), imported by importer version 1 (`AssetPipeline/Importers/IrradianceVolumeImporter.h:24`). `lighting.bake` captures six faces per probe through `ProbeCapture` with `Quality.Probes` false and projects them on the CPU in a fixed order (`Renderer/ProbeCapture.h:14-39`, `:65-70`; `Editor/Source/EditorCore/Lighting/LightingBake.cpp:171-191`). A view copies its volumes' coefficients to t13 only when they change, and writes b4 every frame (`Renderer/Private/ProbeShading.cpp:301-333`). Probes are baked, never dynamic (`Docs/Backlog.md:272`); dynamic GI is cut (ADR 0028 decision 2, `0028-backlog-program.md:57`; `Docs/Backlog.md:347`).
  - **Time.** `RenderExtractionRequest::SimulationTime` reaches extraction, 0 in edit scenes (`Scene/RenderExtraction.h:111-116`); `PlaySession::MaxTimeScale` is 100 (`Session/PlaySession.h:450`).
  - **Buffers.** Constant buffers are static (`SceneRenderer.cpp:1641-1646`). The engine records `copyBuffer` into structured buffers (`ProbeShading.cpp:312`, `EnvironmentBaker.cpp:591`, `SceneRenderer.cpp:2609`), never into a constant buffer.
  - **Shipping.** An export ships every compiled shader in `Engine.pak` (`Editor/Source/EditorCore/Export/Exporter.cpp:239-247`), so the editor's `EnvironmentBake` program is in an exported game.
- **Corrections to the brief** (to the Skerries session through the product owner at the phase's end):
  - **Environment fields** (ADR 0060 decision 9): every field is script-writable; what is missing is a tint or blend for a map (§2).
  - **Fog glow and cascades** (ADR 0060 decision 9): both follow the first directional light in hierarchy order, so a moon placed above the sun takes them. With a Sky, its `Sun` and `Moon` take these roles.
  - **The Sky is component data.** It is serialized and hashed like Fog; only its rendering is presentation. No engine hash moves, but a game scene that gains a Sky takes new hashes and its replays are recorded again.
  - **The moon's phase is not a field.** It follows the Moon's placement against the Sun (their elongation).
  - **The sun's colour from the atmosphere** tints the rendered colour only: the light's `Color` and `Intensity` stay as authored, and scripts read them.
  - **The time-lapse.** A raised time scale multiplies the ticks per frame (at most 100), so the tick cost grows, not the sky's. Turning the sun faster in the game's script keeps the tick rate.
  - **No spreading over frames.** The sky's lighting is regenerated in the frame its inputs change; its cost is measured (decision 3, "Perf").
  - **Probes** (the owner's answer 6): relit by the sky, they lack the light the sun and the sky bounce off surfaces.
  - **Clouds** (the owner's answer 6): one 2D layer, casting no shadows; volumetric clouds stay out (ADR 0060 decision 1).

## Decisions

### 1. Scope

S03, as one milestone M45, in phase 8. It delivers:
- a `Sky` component with a physically based atmosphere (Hillaire's LUTs), a sun disc, a procedural moon with its phase, turning stars, haze, overcast and a 2D cloud layer;
- run-time image-based light from the sky in the existing environment slots, with its fog colour;
- named Sun and Moon roles (fog glow, cascades, driven colours);
- sky-relit irradiance volumes (format 2) and reflection probes scaled with them;
- validation, stats, goldens, perf metrics and FeatureTest coverage in every mode.

**Out** (`Docs/Backlog.md` §2 rows at landing):
- volumetric clouds (ADR 0060 decision 1), cloud shadows on the ground;
- aerial perspective (Hillaire's froxel LUT): distance haze stays the Fog's;
- an HDRI or textured night sky, a moon texture;
- a tint or cross-fade for an Environment map;
- time-of-day keyframed probe bakes, which would carry the bounce;
- a solar-position script helper and dates or ephemerides;
- a player-facing sky quality setting.

**Accepted at landing** (§6 rows; `Docs/Backlog.md:272` amended for relit volumes):
- relit probes lack the light the sun and the sky bounce off surfaces;
- the sky's image-based light is taken at sea level, whatever the camera's altitude;
- a reflection probe is relit only by the ratio of the relit volume around its centre, and outside every relit volume it keeps its bake;
- the image-based light follows the clouds' drift in steps of `SkyCloudLightingStep` (1 s);
- the discs and stars are not in the image-based light (the lights carry the discs);
- with an Environment beside a Sky, the Environment is unused;
- the image-based light's cloud layer is taken over the world origin, not the camera's XZ, so a camera kilometres from the origin sees clouds overhead that its ambient light and reflections do not (the W45.2 review's nit 7);
- the atmosphere's ground below the horizon is lit by the Sun, the Moon light and the zenith's radiance alone and seen without the extinction between it and the camera, so it reads too bright from a high camera (the W45.2 review's nit 7).

### 2. Structure: one milestone, four waves

| Phase | # | Name | Brief | Lands | Gate | Size (builder-sessions) |
|---|---|---|---|---|---|---|
| 8 | M45 | Sky and time of day | S03 | third in phase 8's fixed train order (M43, M44, M45, M46, M47, M48), after M43 and M44 (ADR 0060 decision 2) | phase 8's batch gate, `CI.py --require-clang-cl` | ~11.5 (W45.1 ~2.5, W45.2 ~4 +1 at risk, W45.3 ~3, W45.4 ~2) |

One builder, on `milestone/m45` in `D:/Engine/.claude/worktrees/m45`, works the waves in order. No contract (ADR 0060 decision 7).
- **W45.1, the component and the CPU half:** `Sky`, its registration and extraction; the Sun and Moon roles; `Renderer/SkyAtmosphere` (transmittance, moon phase, star frame, light driving, the cascade order); `SKY_LIGHT_INVALID` and `SKY_ENVIRONMENT_IGNORED` with fixtures; `RENDER_NO_LIGHTING`'s Sky rule; the unit cases; and the copy's GPU proof (decision 3, "The copy"), the wave's one GPU run. Its first checkpoint fixes what a contract would freeze, M34's way: the component, `RenderSky`, `SkyConstants`, the two declarations with their rows and pipeline counts, `RenderStats`' fields and the format-2 layout.
- **W45.2, the sky on the GPU:** the LUTs, the Sky pass (disc, moon, stars, clouds), the run-time IBL and its copies, the t3 swap, the roles in Prepare, mirror views and captures, device loss and out of memory, the gpu cases, `GoldenSky.jsonl`'s cove scene and two goldens.
- **W45.3, probes under a moving sun:** the relightable bake, `IrradianceVolumeData` format 2, the cache's relight mirror, `ProbeRelight`, the reflection scaling, `PROBE_NOT_RELIGHTABLE`, `lighting.bake`'s `relightable`, the probes scene and golden.
- **W45.4, coverage, tools and documents:** `stats.get`'s fields, the Stats panel and the readout; FeatureTest's suite and replay; `test_sky.py`; Perf.py's metrics; Architecture, the Backlog, the skills and the generated references.

### 3. Design defaults: M45 Sky and time of day

**What M45 adds:** a component and a `RenderSnapshot` member; two pass declarations (11 pipelines, one program, four rows); three validation codes; a format version; a readout member, two `RenderStats` fields and a result field; three goldens and two perf metrics. **What it does not add:** a namespace, a script function or callback, an automation method, a settings section, an `AssetType`, a built-in handle, a `PlaySessionPhase`, a state-hash block, a fuzz target, a render stage, a device feature, a module or a vendored library.

- **The component.** `Sky`, version 1, category Rendering, after `Environment` in `RenderingRegistration.cpp` and in `BuiltinComponents.h`'s list; `UniquePerScene`, default flags, so its generated proxy reads and writes every field. Each field is checked on every write path:

  | Field | Type, default | Range | Meaning |
  |---|---|---|---|
  | `Sun` | EntityRef (`UUID`, as `HingeJointComponent.h:28`), null | | the sun's `DirectionalLight`; null: the first in canonical order |
  | `Moon` | EntityRef, null | | an entity turned as a `DirectionalLight` is, the moon along its +Z; normally a dim `DirectionalLight` (the moonlight); null: no moon |
  | `Intensity` | float 1 | [0, 65504] | scales the sky's radiance and its image-based light |
  | `DriveLights` | bool true | | the Sun's and Moon light's rendered colour × the atmosphere's transmittance (the moon's also × its illuminated fraction) |
  | `Haze` | float 1 | [0, 10] | aerosol (Mie) density scale |
  | `Overcast` | float 0 | [0, 1] | toward a grey overcast; dims the discs and, with `DriveLights`, the sun |
  | `GroundAlbedo` | Color 0.3 | [0, 1] | the atmosphere's ground |
  | `SunDisc` | float 1 | [0, 1000] | the disc's radiance, 1 physical (the light's illuminance over its solid angle); its size is the Sun's `LightAngle` |
  | `MoonSize` | float 0.52 deg | [0.1, 10] | the moon disc's angular diameter |
  | `Stars` | float 1 | [0, 100] | star brightness, faded by the sky's luminance |
  | `Latitude` | float 45 deg | [-90, 90] | the celestial pole's elevation |
  | `NorthAngle` | float 0 deg | [-360, 360] | north's direction about +Y (0: -Z) |
  | `Clouds` | float 0 | [0, 1] | the cloud layer's coverage |
  | `CloudSpeed` | vec2 (5, 0) m/s | each [-100, 100] | the layer's drift in world XZ |

- **Extraction** (`Scene/SkyExtraction.{h,cpp}`, called once after the canonical loop beside `snapshot.Fog`, `RenderExtraction.cpp:947-949`).
  - **The item.** The scene's one effectively enabled Sky (a component view; unique per scene) becomes `RenderSky` (`RenderSnapshot.h`, before `RenderSnapshot`): the fields, `Enabled`, `SimulationTime` (the request's), `Entity`, and the resolved roles: `SunLight` (index into `Lights`, by the light's `Entity`, or the first directional entry for a null `Sun`), `SunLightColor` (its authored `Color`; with its `Intensity`, the sun's illuminance the atmosphere scatters), `MoonLight`, `MoonLightColor`, `HasMoon` and `MoonDirection` (from the Moon's rendered matrix, `ComputeRenderedWorldMatrix`, when it holds no light).
  - **The member.** `RenderSnapshot::Sky`, appended last (after M42's `PlanarReflections`, `RenderSnapshot.h:874-877`, and after M43's `StaticSet`; M46's `Wind` and M47's `Waters` follow): a frozen-header change.
  - **Driving.** With `DriveLights`, `ApplySkyLightDriving(RenderSnapshot&)` sets the Sun's `LightData::Color` to `SunLightColor × T(sun)` and the Moon light's to `MoonLightColor × T(moon) × k` (k its illuminated fraction), T the transmittance from the camera's altitude. It is declared in `RenderSnapshot.h` beside `ComputeReverseZProjection` (`:912-921`) and implemented in `Renderer/SkyAtmosphere.cpp` with DetMath, so Scene may call it. It is idempotent (from the authored colours `RenderSky` keeps), so a golden can move the sun in a snapshot and drive again. Every consumer of the light list (shading, culling, fog, mirror views, captures) sees the driven colours with no renderer change, and the components keep theirs.
  - **Every view** extracts it, edit scenes included at `SimulationTime` 0. A scene without a Sky extracts exactly as before.
- **The atmosphere on the CPU** (`Renderer/SkyAtmosphere.{h,cpp}`, pure, double precision, DetMath, fixed steps):
  - Hillaire's (EGSR 2020) Earth constants: radii 6360 and 6460 km, Rayleigh, Mie and ozone coefficients, scaled by `Haze` and `Overcast`. The ground is world Y 0 under the camera; the camera's altitude is `max(Position.y, 0) + 1 m`.
  - `ComputeSkyTransmittance(altitude, direction, haze, overcast)`: analytic optical depth by fixed steps, 0 below the horizon.
  - `ComputeMoonIlluminatedFraction(sun, moon)` = (1 - cos ψ) / 2 of their elongation ψ.
  - `ComputeSkyStarFrame(sun, latitude, northAngle)`: the pole from `Latitude` and `NorthAngle`, turned about it by the sun's hour angle, so the stars turn with the sun and no time state exists.
  - `OrderShadowCandidates(snapshot)`: the Sun first while above the horizon, else the Moon light, then the rest in snapshot order; without a Sky, snapshot order (today's rule).
  - `GetSkyGlowLight(snapshot)`: the Sun's index with a Sky, else the first directional light. `MakeFogConstants` gains it as a parameter (`Fog.h:40-41`), so the glow follows the Sun.
- **The GPU, per view** (`Renderer/SkyLighting.{h,cpp}`, its stage functions in a new `Renderer/SceneRendererSky.cpp`, as M42's `SceneRendererReflections.cpp`):
  - **LUTs:** transmittance 256 × 64 and multiple scattering 32 × 32, rebuilt when `Haze`, `Overcast` or `GroundAlbedo` change; the sky-view LUT (192 × 108) every frame from the camera's altitude, and a sea-level one with each lighting update, scattering the Sun's light and the Moon light's (its colour × illuminated fraction). RGBA16F.
  - **The Sky pass** (`Renderer/SkyPass.{h,cpp}`, `Passes/Sky.slang`, `Shared/SkyConstants.h`): a fullscreen triangle at depth 0, as `SkyboxPass`, for a Lit view whose camera clears to the skybox. Each pixel adds the sky-view LUT, the sun disc (limb-darkened, `LightAngle` wide), the moon (phase-lit toward the sun, procedural maria from a seeded noise, no texture), stars from a seeded hash in the star frame (generated in code, as the blue noise), and the cloud layer: fbm coverage on a plane at 2,000 m (`SkyCloudHeight`), offset by `CloudSpeed × SimulationTime` in double (wrapped to the noise's period), lit by the sun's transmittance and the sky's ambient. It fogs with `FogSkybox` as the skybox does (ADR 0047 decision 5). `SkyboxPass` is unchanged and still draws an Environment's map without a Sky.
  - **Run-time IBL** (`SkyViewResources` in `SceneRenderer::State`, the placement rule's place 2): only when its key changes (the fields, the Sun's and Moon's directions and authored colours, and the clouds' offset at `SkyCloudLightingStep`), it renders the sea-level atmosphere and clouds (no discs, no stars) into a 256² RGBA16F cube with mips, prefilters it into a 256² × 7 specular cube (the asset's sizes, so `SpecularMaxLod` keeps its meaning) and projects its radiance SH9. It reuses the `EnvironmentBake` program's `CSDownsampleCube`, `CSPrefilterSpecular` (16 to 64 samples), `CSProjectIrradiance` and `CSReduceIrradiance` with `EnvironmentBaker::GetLayoutDescriptions()`, the same SPIR-V, so every import bake stays byte-identical. W45.2's first GPU run confirms each entry takes the sky cube as it takes a skybox, or the Sky program gains the entry it needs.
  - **Every frame a Sky lights the view,** `CSSkyFinalize` turns the persistent radiance SH9 into the cosine-convolved, windowed `SH9 × Intensity` (§8.6 step 4) and the fog's tinted colour (`MakeFogConstants`' rule on that SH9), then the copies below place them.
  - **Binding.** With a Sky, Prepare's environment block makes `EnvironmentConstants` with `HasEnvironment` 1, `Intensity` the Sky's, rotation 0 and the sky's mip counts (a new `MakeSkyEnvironmentConstants`, `LightingInputs`), and binds the sky's specular cube at t3. The forward, terrain, SSR, probe and fog shaders do not change, so M47's water reflects the sky for free.
  - **Mirror views** draw the Sky with their scissor and fog start, sampling the main view's sky-view LUT (world directions). **Probe captures** draw it from the probe's altitude. A capture of a frame is a pure function of that frame: nothing temporal is added (§8.3).
- **The copy into constant buffers** (the research's UNVERIFIED, decided: a GPU copy).
  - **What is copied,** by `nvrhi::ICommandList::copyBuffer` from the view's `SkyResults` buffer (UAV, written by `CSSkyFinalize` and `CSProbeRelight`), after `RecordViewSetup`'s CPU writes:
    - the SH9 to offset 0 of `EnvironmentConstants` (144 bytes, `EnvironmentConstants.h:28`);
    - the tinted colour to offset 0 of the view's and each mirror slot's `FogConstants` (12 bytes, `FogConstants.h:24`);
    - each relit reflection probe's `AxisScale.w` in `ProbeConstants` (4 bytes at 896 + 192 i + 156, `ProbeConstants.h:46-75`).

    No layout changes.
  - **Why it works.** In the vendored NVRHI (`6b96fb03`), every Vulkan buffer has `TRANSFER_SRC | TRANSFER_DST` (`Vendor/NVRHI/src/vulkan/vulkan-buffer.cpp:48-49`). `copyBuffer` moves the destination to `CopyDest` under automatic barriers (`:225-261`). `writeBuffer` on a static constant buffer already takes that state every frame (`vkCmdUpdateBuffer`, or a `copyBuffer` from the upload manager above 64 KiB, `:444-497`).
  - **The one trap.** Unlike `writeBuffer`, `copyBuffer` ends a render pass only when a barrier is pending (`vulkan-state-tracking.cpp:296-304`). So the copies are recorded in the Ambient stage, right after the sky's compute dispatches, outside every render pass.
  - **W45.1's proof,** before anything in W45.2 depends on it: the gpu case "SkyLighting: a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them".
    - Two frames running, it CPU-writes static constant buffers made as the renderer makes them (`SceneRenderer.cpp:1641-1646`), copies at the three offsets from a buffer a compute wrote, then reads them in a compute into a read-back buffer.
    - It checks the copied bytes, the untouched neighbours (`Density`, `AmbientColor`, a reflection's `Lookup`) and the second frame.
    - It runs under validation and synchronization validation, at API 1.4 and 1.3.
  - **The fallback,** only if that case cannot pass: no copy into a constant buffer. The results come back through a ring of `GetFramesInFlight()` read-back buffers, and the CPU applies the newest completed ones in `MakeEnvironmentConstants`, `MakeFogConstants` and the probe constants. A capture (`ViewportCapture`, `ProbeCapture`) and a render that resets the view's history wait for the current values, so captures and goldens stay exact, while a live view lags a changing sky by up to the frames in flight. The W45.1 record says which was taken.
- **Relit probes** (the owner's answer 6; inside ADR 0028 decision 2's cut, decision 4).
  - **The bake.** In a scene with an enabled Sky, `lighting.bake` captures each irradiance probe from a snapshot with the Sky's `Intensity` 0 and its Sun and Moon lights off (`MakeRelightCaptureSnapshot`, after `MakeCaptureSnapshot`, `ProbeCapture.cpp:282-319`). `ProbeCubeCapture` gains `Visibility`, read back from `SceneDepth` (D32, 1 where depth is 0), through a new `SceneRenderer::GetSceneDepthTexture` (beside `:427`). The D32 read-back is UNVERIFIED: W45.3's first GPU run confirms it, else a compute writes the visibility to an R8 target.
  - **What it projects,** on the CPU in double and DetMath, in a fixed order as `ProjectCubeToIrradianceSH9`:
    - the local term L (lamps, fires, emission) by that function;
    - the sky transfer T_ij = ∫ V Y_i Y_j dω, 45 floats of a symmetric 9 × 9 matrix (`ProjectCubeToSkyTransfer`);
    - once per bake, the sky's radiance SH9 S_bake, from a sky-only capture of the snapshot with no meshes, terrains or lights and with discs and stars at 0 (`ProjectCubeToRadianceSH9`).
  - **Format 2.** The probes' coefficients are `L + conv(T S_bake)` (`Renderer/ProbeRelight.h`'s `RelightIrradianceSH9`: the cosine lobe and the Hanning window of §8.6 step 4), what a renderer without relighting shows. An optional relight block follows: `uint32 HasRelight`, then per probe `{9 × f32x3 Local; 45 × f32 Transfer}`.
    - Version 1 files still read.
    - A volume without the block is written as version 1, byte for byte today's, so FeatureTest's committed bakes (`Docs/Backlog.md:285`) and `Scaffold.py --check` hold.
    - The cooked payload is always format 2. A cached version-1 artifact is rebuilt by the payload-version rule (Architecture §7.5, `Architecture.md:1056`; ADR 0034 decision 20), so `IrradianceVolumeImporter::Version` stays 1 and no `.meta` moves.
    - The reader is the fuzzed `IrradianceVolumeData` target, whose seed payload gains a block.
  - **At render.** `GpuIrradianceVolume` (`GpuResourceCache`, place 3) gains the block as a structured buffer. When the sky key or the view's probe uploads changed, `CSProbeRelight` writes each relit volume's `L + conv(T S_now)` into the view's t13 copy (now UAV-capable, `ProbeShading.cpp:245-257`). Every frame, it also writes each reflection probe's `Intensity ×` the ratio of the relit to the baked luminance of the DC term at the nearest probe of the relit volume around its centre, clamped to [0, 4], for the copy above. `Common/Probes.slang` is unchanged.
- **Where it runs: two declarations, no stage.**
  - **`SkyLighting`,** at the end of the Ambient stage after `LightClusters` (`RenderPassDeclaration.h:189`): rows `SkyLuts`, `SkyIbl` and `ProbeRelight`; 10 pipelines (the Sky program's six computes and `EnvironmentBake`'s four).
  - **`Sky`,** in the Forward stage directly after `Skybox` (`:204`): row `Sky`, 1 pipeline, target `SceneColor`. M47's `Water` follows it (decision 8).
  - **Recording.** `RecordAmbientStage` ends with `RecordSkyLighting(frame)` after `RecordLightClusters` (`SceneRenderer.cpp:2325`). `RecordForwardStage`'s sky block (`:2447-2460`) draws the Sky in the Skybox's place when the snapshot's Sky is enabled.
  - **Rows.** `SkyLuts` records whenever a Sky lights a Lit view; `SkyIbl` records the finalize and copies every frame and the update when the key changed; `ProbeRelight` records only when it runs.
  - **Comments.** `RenderStage.h`'s Ambient comment names the sky's lighting (comment only).
- **Device loss, trim and out of memory.** The LUTs, cubes and buffers live in `SkyViewResources` (place 2) and are rebuilt lazily after Release and Recreate. The relight block lives in the cache (place 3), the pipelines in the set; no registration of their own. A cube or LUT that cannot be created: the view lights with the Environment as without a Sky, Render returns its partial error, and the host trims and renders again (M18 decision 44).
- **Validation** (`Scene/SkyValidation.{h,cpp}`, its own code array, `SkyDiagnosticCodes`, holding all three codes, called by a new `CheckSky` after M43's `CheckStaticScenery`, with `PROBE_NOT_RELIGHTABLE` from `CheckProbes`; appended to both of `ProjectValidator.cpp`'s tables after M42's `RENDER_REFLECTION_INVALID`, `:224`, and after M43's and M44's codes; the count line adds `+ SkyDiagnosticCodes.size()`):
  - `SKY_LIGHT_INVALID` (Warning, not fixable): `Sun` names an entity without an enabled `DirectionalLight`; `Sun` is null and the scene has none; or `Moon` is the Sun's entity. Dangling references stay `ENTITY_DANGLING_REFERENCE`'s (`ProjectValidator.cpp:920-940`).
  - `SKY_ENVIRONMENT_IGNORED` (Warning, not fixable): an enabled Environment beside an enabled Sky.
  - `PROBE_NOT_RELIGHTABLE` (Warning, not fixable; in `SkyDiagnosticCodes`, checked by `Scene/ProbeValidation.cpp`): under a Sky, an IrradianceVolume whose data has no relight block, or a ReflectionProbe outside every relit volume. The hint is `lighting.bake`.
  - `RENDER_NO_LIGHTING` counts a Sky of positive `Intensity` as illumination (`Scene/RenderAnnotations.cpp`'s illumination block, `:160-173`; M43's mesh loop, `:175-193`, reads the static set).
  - The three fixtures come from `MakeValidationFixtures.py` (`COMPONENT_ORDER` gains `Sky` after `Environment`, `:75`), with the `validation-codes` rows and §13.7's.
- **Scripts, automation, editor and stats.**
  - **Scripts:** the generated `Sky` proxy; no function, method or callback. `PerformanceStats` appends `SkyUpdates`, after M43's five members.
  - **Automation:** no method (126 methods, 86 tools stay). `lighting.bake`'s `LightingBakeProbe` gains `relightable` (`EditorCore/Automation/LightingMethods.h:36-42`, M26's frozen wire format, a reviewed change).
  - **Stats.** `RenderStats` appends `SkyUpdates` (0 or 1, the render regenerated the sky's lighting) and `RelitIrradianceVolumes`, after M42's fields (`Renderer/RenderStats.h:98-101`) and M43's. `stats.get`'s view summary appends `skyUpdates` and `relitIrradianceVolumes` after M43's fields, described; the Stats panel gains "Sky: updates / relit volumes" after M43's row.
  - **Editor:** the generic inspector in Rendering, with the EntityRef pickers M37's joints use. `ClearMode::Skybox`'s description becomes "The scene's Sky, or its Environment skybox (its fallback colour when none is shown)."
- **FeatureTest** (IdSeeds 4201 to 4203, in M45's block 4200-4299 of phase 8's ledger; every gate in each mode the feature supports):
  - **`Sky.jsonl`** (4201) makes `Sky.scene` and the `Sky` suite. The scene holds a Sky with every field leaf non-default, `Sun` and `Moon` naming two DirectionalLights, and no Environment (no finding). The suite:
    - reads and writes every field through the proxy (gate 4);
    - turns the sun from a task;
    - where `Application.GetStats().RenderAvailable` holds, checks `SkyUpdates` 1 while the sun turns and 0 when it stops.
  - **`SkyReplay.jsonl`** (4202) records `Sky.replay` on `Sky.scene`: a held action quickens the sun's turn (the time-lapse), so the replay needs its input.
  - **`GoldenSky.jsonl`** (4203, `Renderer` vulkan for its bake; `GoldenDirectories` `Assets/Lighting/Scenes/GoldenSky`, `Assets/Materials/GoldenSky`, `Assets/Scenes/GoldenSky`) makes two edit scenes:
    - `SkyDay.scene` (named after its golden image; the ledger's Scenes row amended on main, `06551952`): ground, a glossy floor standing in for water, a rock, a hut, a Sky with Sun and Moon, fog and automatic exposure (the W45.2 record);
    - `SkyProbes.scene` (likewise): a room with a window and a cave mouth, a relightable volume, a reflection probe and a lamp.
  - **Gates:** 1, `Sky` with every field leaf non-default; 2, `api:Entity.Sky/read` (the generated `Entity.Sky` shortcut, which the suite reads); 4, its proxy; 6, the three fixtures; 7, every description (the component, its fields, the `stats.get` fields, the readout member). 1b, 3, 5, 8 and 9: nothing new.
- **Perf** (Perf.py's look part, the headless game view at 1920 × 1080; warning-only budgets in `Scripts/Lib/perf.py`'s `BUDGETS`, `:49-50`, with `test_perf.py` cases):
  - `sky/Static/gpuMs` ≤ 0.3 ms: the `SkyLuts`, `SkyIbl` and `Sky` rows of a generated `Assets/Perf/Sky.scene` whose sun stands still;
  - `sky/Turning/gpuMs` ≤ 0.6 ms: the same rows and `ProbeRelight`, the sun turned every tick by the scene's script, with one relightable 8 × 4 × 8 volume baked at the part's start (the brief's time-lapse cost).
- **Documents:**
  - Architecture: §5.3 (the row); §6.8 (irradiance volume 2); §8.2 (`RenderSky`); §8.3 (the declarations, a "Sky" paragraph); §8.4 (the Sky layout); §8.6 (run-time sky lighting, relit probes, the copies); §8.7 (the cascade owner with a Sky); the Fog paragraph (the glow light); §8.13 and §8.14; §13.7 (the codes, the fields);
  - the Roadmap's M45 section; `Docs/Backlog.md` §2 and §6;
  - the header comments of `RenderSnapshot.h`, `RenderPassDeclaration.h`, `SceneRenderer.h`, `ProbeCapture.h`, `IrradianceVolumeData.h` and `Fog.h`;
  - the skills: game-building's `look` (the day recipe: a Sky naming a sun and a moon light, auto-exposure, the sun turned by the game's script, a relit bake), game-debugging (fields, codes), golden-images (scenes);
  - the generated references.

### 4. The research's questions, decided by the lead

1. **Probes under a moving sun (research Q1): sky-relit**, as the owner answered (ADR 0060 question 6): one bake, continuous, no keyframe steps in a time-lapse. The missing bounce is §6's.
2. **Inside ADR 0028 decision 2's cut (Q2): yes** (ADR 0060 question 8). There are no lightmaps, SSGI, DDGI or ray tracing: it is a baked transfer re-evaluated against the sky. Recorded in Architecture §8.6 and beside `Docs/Backlog.md:347`.
3. **The cloud layer (Q5): in M45,** as one 2D procedural layer (~0.5 session in W45.2): the storm's overcast reads poorly without it.
4. **The moon (Q8): procedural.** Nothing is downloaded; a texture field may follow (§2).
5. **The Sun and Moon roles (Q11): yes.** With a Sky, its Sun owns the fog glow and the cascades by day and its Moon the cascades by night. Without a Sky, nothing changes.
6. **Irradiance volume 2 writing (Q12): version 1 when the volume has no relight block,** that is, when its scene has no Sky.
7. **The copy into constant buffers (UNVERIFIED in the research):** a GPU copy, proved by W45.1's case, with the read-back ring as the fallback (decision 3).
8. **Spreading the update over frames:** rejected as the default, since it adds state captures must clear. It stays the fallback if `sky/Turning/gpuMs` misses its budget, with lower sample counts and a 128² cube tried first.
9. **Departures from the research's design,** each for a reason:
   - **Driving at extraction, not in Prepare:** every consumer of the light list sees one colour, and M43's Prepare is untouched by it.
   - **One `SKY_LIGHT_INVALID`** for the Sun and the Moon, as M42 has one code per component; dangling references are already reported.
   - **No discs or stars in the image-based light:** the lights carry the discs (no double count), and stars alias at 256².
   - **Clouds in the image-based light at 1 s steps,** so a drifting layer does not regenerate it every frame.
   - **Two perf metrics:** the relight's cost is in `sky/Turning/gpuMs`.

### 5. Approvals

None. Hillaire's method needs physical constants only; the moon and stars are procedural. Nothing is downloaded or vendored.

### 6. Determinism, platforms and goldens

- **Determinism.** The Sky is component data, hashed like Fog. Nothing the renderer computes is hashed, and nothing in the simulation reads it: driving changes only the snapshot.
  - No hash block is added, no committed hash moves, no `project.upgrade` is needed (ADR 0060 decision 6's row).
  - `Sky.replay` verifies at one hash in the `--renderer none` Debug and Release editors and the rendering Release and Dist exports.
  - The bake's CPU steps stay pure (double, DetMath, fixed order), so two bakes of one scene write byte-identical files.
- **Platforms.** Only Windows is verified (ADR 0028 decision 10). M45 adds no platform capability and no device feature.
- **Goldens.** Three new on `nvidia-62x`, each also rendered twice to the same bytes:
  - `SkyDay`: six tiles of `SkyDay.scene` (the sun at dawn, morning, noon, afternoon and dusk, and night with the moon and stars), the sun moved in the snapshot and driven again;
  - `SkyOvercast`: two tiles (haze with clouds, the storm's full overcast);
  - `SkyProbes`: two tiles of `SkyProbes.scene`, noon and midnight with no bake between.

  The 56 existing goldens stay byte-identical: no existing shader changes and the `EnvironmentBake` program is reused unchanged. A drift is a bug, never a golden update. `SkyProbes` depends on its bake's device class (`Docs/Backlog.md:285`).
- **Fuzzing.** No new target. The `IrradianceVolumeData` target mutates format-2 payloads; its 10,000-mutation case runs at the gate, and the 1,000,000-iteration run waits for the program's hardened gate (ADR 0060 decision 7).

### 7. Process

As ADR 0060 decision 7:
- **One builder,** in its own worktree, works the waves in order. The lead runs one independent review per wave and has every confirmed finding fixed, nits included.
- **Frozen headers.** `RenderSnapshot.h`, `RenderPassDeclaration.h`, `ProbeCapture.h`, `LightingMethods.h` and the shared shader structs change only as decision 3 names, as reviewed changes to a frozen interface.
- **GPU discipline.** Filtered gpu, golden and automation runs only, through `Test.py` under the machine locks.
- **Checkpoint commits** run their targeted checks. Agents never run `PreCommit.py` or `CI.py`.
- **The landing.** Phase 8's one batch in the fixed train order M43, M44, M45, M46, M47, M48: M45 merges third. At its merge, M45 adapts its hunks in M43's files, appends after M43's and M44's entries, and regenerates FeatureTest and the references on the merged tree. `EngineVersionString` is M48's, the last lander's (0.9.0).
- **The gate:** phase 8's batch gate, `CI.py --require-clang-cl`. The hardening runs only at the program's last phase.

### 8. The milestone's reservation needs

Phase 8's reservation ledger, committed in `Docs/Decisions/0060-skerries-engine-program.md` as its section "Phase 8 reservation ledger", is the rule: the rows below are M45's rows there, with its values, orders and owners, in the fixed train order M43, M44, M45, M46, M47, M48. A deviation needs a ledger amendment on main. Today's values are read at `3aaf4b10`; "at its merge" counts M43's and M44's entries, which land first.

| Allocation | Today | M45 |
|---|---|---|
| ADR numbers | 0059 the last committed; 0060 the program | 0063, this record |
| Components, categories | 64 in 16 categories | `Sky` in Rendering after `Environment`, before `PostProcess` (mid-list): 69 at its merge (65 on its branch); no category, no registration function; `COMPONENT_ORDER` gains it after `Environment` |
| Validation codes | 75, the last `RENDER_REFLECTION_INVALID` | +3 after M44's: `SKY_LIGHT_INVALID`, `SKY_ENVIRONMENT_IGNORED`, `PROBE_NOT_RELIGHTABLE` (Warnings), appended in that order: 81 at its merge (78 on its branch); the array `SkyDiagnosticCodes` (3, `Scene/SkyValidation.h`) joins the count line; `CheckSky` after M43's `CheckStaticScenery`, and `PROBE_NOT_RELIGHTABLE` inside `CheckProbes` |
| Settings sections, script namespaces | as today; 22 namespaces | none; the `Sky` proxy (generated) |
| `IScriptHost`, readout | `IScriptHost` ends with `GetMusic`; `PerformanceStats` 24 members (`Scripting/ScriptPerformanceStats.h:15-50`) | no host member; `SkyUpdates` appended after M43's five: 30 at its merge (25 on its branch) |
| Automation methods, MCP tools | 126 and 86 | unchanged; `lighting.bake`'s `LightingBakeProbe` +`relightable` (M26's frozen wire format, a reviewed change); `stats.get`'s view summary +2 fields after M43's four (38 at its merge) |
| Stats rows, `RenderStats` | 41 rows; fields end with M42's `PlanarReflectionEntities` | 45 (`SkyLuts`, `SkyIbl` and `ProbeRelight` after `LightClusters`, `Sky` after `Skybox`); `RenderStats` +2 fields after M43's four |
| Declarations, pipelines, programs, stages | 34, 77 (`SceneRendererTests.cpp:475`), 34 programs, 13 stages | 36 (`SkyLighting` after `LightClusters`, `Sky` directly after `Skybox`), 88, 35 (`Sky`, sorted between `Skinning` and `Skybox`), 13; M47's declarations follow |
| Shared structs, draw flags | `ViewConstants` 512, `DrawConstants` 16, `InstanceRecord` 208; flags 1 to 16; `EnvironmentConstants` 192, `FogConstants` 128, `ProbeConstants` 2448 | all unchanged; new `Shared/SkyConstants.h`, in `ShadersTests.cpp`'s list after M43's `InstanceIndex` |
| Goldens | 56 on `nvidia-62x` | 59 (`SkyDay`, `SkyOvercast`, `SkyProbes`, `GoldenSky.jsonl`), the first of phase 8's |
| Perf metrics | `BUDGETS` holds 4 | +2 in the `look` part (`sky/Static/gpuMs` 0.3, `sky/Turning/gpuMs` 0.6): 7 at its merge, after M43's (6 on its branch) |
| Fuzz targets | 53 (`Tests/Data/Fuzz/Targets.json`) | 53; `IrradianceVolumeData`'s reader changes |
| FeatureTest | 101 scaffolds, the highest IdSeed 3902; 116 suites, 26 replays, 104 scenes | +3 scaffolds, `Sky.jsonl` 4201, `SkyReplay.jsonl` 4202 and `GoldenSky.jsonl` 4203 (M45's block 4200-4299), after M43's and M44's and before `Suites.jsonl` (1515) and `Replays.jsonl` (1516); +1 suite, +1 replay, +3 scenes |
| Formats | `IrradianceVolumeData` 1, importer 1; replay 5 | `IrradianceVolumeData` 2 (1 written without a block); importer 1; `Sky` 1 |
| `AssetType`, built-in handles, `PlaySessionPhase`, hash blocks, device features | as ADR 0059's ledger left them | none |
| `EngineVersionString` | 0.8.0 (`Core/BuildId.h:38`) | none: M48, the last lander, sets 0.9.0 |

**M45's own files.**
- **New:** `SkyComponent.h`; `Scene/SkyExtraction.{h,cpp}`, `Scene/SkyValidation.{h,cpp}`; `Renderer/SkyAtmosphere.{h,cpp}`, `SkyLighting.{h,cpp}`, `SkyPass.{h,cpp}`, `ProbeRelight.{h,cpp}`, `SceneRendererSky.cpp`; `Passes/Sky.slang`, `Common/Atmosphere.slang`, `Shared/SkyConstants.h`; the tests (`SkyAtmosphereTests.cpp`, `SkyLightingTests.cpp`, `ProbeRelightTests.cpp`, `SkyGoldenTests.cpp`, `test_sky.py`), the scaffolds, goldens and fixtures.
- **Changed, M45's alone in phase 8:** `SkyboxPass` (comment), `Fog.{h,cpp}`, `Private/LightingInputs.{h,cpp}`, `Private/ProbeShading.{h,cpp}`, `IrradianceVolumeData.{h,cpp}`, `LightingBake.{h,cpp}`, `LightingMethods.{h,cpp}`, `ProbeValidation.{h,cpp}`, `RenderingRegistration.cpp` (`Sky` after `Environment`; `ClearMode::Skybox`'s description), `RenderStage.h` (comment).

**Contended files and M45's regions in each.**
- **M43's, which lands first** (ADR 0060 decision 2):
  - `RenderSnapshot.h`: `RenderSky` before `RenderSnapshot`; the `Sky` member appended after M43's `StaticSet`; the two driving declarations beside `ComputeReverseZProjection` (`:912-921`).
  - `RenderExtraction.{h,cpp}`: one call after the canonical loop M43 rewrites (`:754-945`), beside `snapshot.Fog` (`:947-949`), and a header paragraph. Nothing inside the loop.
  - `SceneRenderer.cpp`'s Prepare: the cascade loop (`:1691-1730`, outside M43's regions) changes only the order it visits directional lights, through `OrderShadowCandidates`. The forward set (`:2378-2384`, where M43 appends t21 and t22) gains no binding from M45: t3 takes the sky's cube.
- **M45's regions in M43's renderer files** (the Forward stage's sky and the environment binding):
  - in Prepare, the environment block (`:1864-1873`) and the probe preparation (`:1874-1877`);
  - the end of `RecordAmbientStage` (`:2325`);
  - `RecordForwardStage`'s sky block (`:2447-2460`);
  - `SceneRendererReflections.cpp`'s slot fog (`:251`) and mirror sky (`:440-449`);
  - `Private/SceneRendererState.h`'s sky members beside `DrawsSkybox` (`:404`, `:428-429`);
  - amended on main (`70c4b7e8`, 2026-11-24, the W45.3 review's finding 4): the generation-change block (`:1336-1340`, the sky's keys and the probe copies, after M29's shadow-cache clear) and the `RenderStats` fill lines after M42's `PlanarReflectionEntities` (`:1475`; M47's follow).
- **M45's for the phase, with the others' regions in train order:**
  - `Scene/RenderAnnotations.cpp`: M45's region is the illumination block (`:160-173`); M43's mesh loop (`:175-193`) lands first, M44's vehicle call goes after `:135`, and M47's outline after M45's region;
  - `Renderer/RenderPassDeclaration.h` (frozen): `SkyLighting` after `LightClusters` (`:189`), `Sky` directly after `Skybox` (`:204`), and the comment; M47's `Water` follows `Sky`, and its `Underwater` goes first in HdrPost;
  - `Renderer/ProbeCapture.{h,cpp}` (frozen header): M43's static set in captures (`MakeCaptureSnapshot`, `:282-319`) lands first; M45 adds `MakeRelightCaptureSnapshot` after it and the visibility read-back; M47 clears `Waters` after the planar clear (`:317`);
  - `Renderer/SceneRenderer.h`: `GetSceneDepthTexture` beside `:427`, and `GetSkyRadianceTotals` after it (amended on main, `70c4b7e8`, the W45.3 review's finding 4).
- **M47's Water, which follows the sky:**
  - `RenderPassDeclaration.h`: M47's `Water` after `Sky` (above), since M47 lands after M45;
  - `SceneRenderer.cpp`: M47's water record follows M45's sky block;
  - set 0's t3, b2 and b5 keep their layouts, so the water reads them unchanged.
- **Append-only, in train order:**
  - the declarations' and pipelines' totals (`RenderPassDeclarationTests.cpp`, `SceneRendererTests.cpp`: M45's counts, then M47's) and `SceneRenderer.cpp`'s set creation (`:137-140`, `:608-632`);
  - `Shaders.json` (`Sky` between `Skinning` and `Skybox`) and `ShadersTests.cpp` (`SkyConstants` after M43's `InstanceIndex`);
  - `RenderStats.{h,cpp}`, `StatsMethods`, `StatsPanel`, `ScriptPerformanceStats.h`, `SessionStatistics.cpp`, `Bindings/Application.cpp`, after M43's entries;
  - `GpuResourceCache.{h,cpp}` (M43's file; the volume mirror, `:132-139`);
  - `Scripts/Perf.py`, `Scripts/Lib/perf.py` and `test_perf.py`, after M43's entries.
- **Conflicts by construction**, M45 adapting:
  - `BuiltinComponents.h` (M43's Core, M44's Physics and the later categories); `RenderingRegistration.cpp` is M45's alone;
  - `ProjectValidator.cpp`'s two tables and its tests' count;
  - `MakeValidationFixtures.py`;
  - FeatureTest's `Scaffolds.json` and `Suites.jsonl`;
  - Architecture, the Backlog, the skills;
  - the generated references, regenerated and never merged.

## Acceptance

The Roadmap's M45 acceptance, each a named test or check:
- "SkyAtmosphere: the transmittance toward the sun falls with the zenith angle, reddens at the horizon and is zero below it"
- "SkyAtmosphere: the moon's illuminated fraction follows its elongation from the sun"
- "SkyAtmosphere: the stars turn about the celestial pole by the sun's hour angle"
- "SkyAtmosphere: driving the lights is idempotent and leaves the components' colours as authored"
- "SkyAtmosphere: the cascades go to the Sun above the horizon and to the Moon below it"
- "Fog: with a Sky, the glow follows its Sun"
- "RenderExtraction: a Sky is extracted with its Sun and Moon in every view, and a scene without one extracts as before"
- "SkyValidation: a Sun or Moon that names no usable light, and an Environment beside a Sky, are reported"
- "SkyLighting: a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them" (gpu, W45.1)
- "SkyLighting: the transmittance and multiple-scattering LUTs match their CPU reference" (gpu)
- "SkyLighting: the run-time prefilter of a cube matches the environment bake's" (gpu)
- "SkyLighting: a constant sky gives a constant irradiance" (gpu)
- "SkyLighting: the view's ambient light, reflections and fog colour follow the sun" (gpu)
- "SkyLighting: the sky's lighting is regenerated only when its key changes" (gpu)
- "SkyPass: a mirror view and a probe capture draw the sky" (gpu)
- "SkyLighting: a sky scene renders the same bytes after a device loss" (gpu)
- "SkyLighting: a sky whose cubes cannot be created lights with the Environment and the render returns its error" (gpu)
- "IrradianceVolumeData: a relightable payload round-trips, a version 1 source still reads, and a volume without a relight block writes version 1"
- "ProbeCapture: an open probe's sky transfer is the identity and a closed one's is zero"
- "ProbeRelight: an open probe relights to the sky's irradiance and a closed one to its local light"
- "ProbeRelight: the GPU relight matches its CPU reference" (gpu)
- "LightingBake: a volume baked under a Sky is relightable, bakes byte-identically twice, and relights to its bake at the bake's sky" (gpu)
- "ProbeRelight: a cave darkens at night and lights at noon with no bake between" (gpu)
- "ProbeRelight: a reflection probe inside a relit volume dims with it" (gpu)
- "ProbeValidation: a volume without a relight block under a Sky is reported"
- `Golden: SkyDay`, `Golden: SkyOvercast` and `Golden: SkyProbes`, with every existing golden byte-identical.
- `test_sky.py`: entity updates of the Sky, Scene-view screenshots that differ between dawn and noon, `stats.get`'s `skyUpdates` (1 after a turn, 0 when still), `lighting.bake`'s `relightable`, `SKY_ENVIRONMENT_IGNORED` from `project.validate`.
- The `Sky` suite passes in the editor, Release and Dist; `Sky.replay` verifies at one hash in Debug, Release and Dist and fails without its input.
- The frozen-layout cases: `RenderPassDeclarationTests`' rows (45), the pipeline count (88), "Shaders: SharedStructsMatchReflection" with `SkyConstants`, and the existing `IrradianceVolumeData` fuzz case on its new seed.
- Perf.py's `sky/Static/gpuMs` within 0.3 ms and `sky/Turning/gpuMs` within 0.6 ms (warning-only), from the builder's `Perf.py --config Release --only look` at W45.4's end, and again at the program's hardened gate.

**S03's "How the game will check it":**
- **Six screenshots of Landfall's cove across a day agree** (sky, sun light, ambient, water's reflections): `Golden: SkyDay` (its glossy floor reflects the sky's cube, as M47's water will) and "SkyLighting: the view's ambient light, reflections and fog colour follow the sun". The cove itself is the game's check.
- **The ranger station and the cave mouth darken at night with no bake between:** `Golden: SkyProbes`, "ProbeRelight: a cave darkens at night and lights at noon with no bake between" and "a reflection probe inside a relit volume dims with it". The game bakes once with its Sky in the scene; `PROBE_NOT_RELIGHTABLE` names any older bake.
- **The time-lapse runs without hitches, its per-frame cost at 1080p measured:** `sky/Turning/gpuMs` and `stats.get`'s `SkyLuts`, `SkyIbl`, `ProbeRelight` and `Sky` rows. The game turns its sun in its script (the corrections).
- **Replays still verify:** `Sky.replay` at one hash in renderer-none editors and rendering exports; no committed hash moves. The Sky is hashed component data, the sky's rendering is not.

## Risks

- **The copy into constant buffers.** W45.1 proves it on the device before W45.2 builds on it. The fallback lags a live view by the frames in flight.
- **Cost.** A time-lapse regenerates the lighting every frame, and the editor's two views double it. The metrics measure it; the fallbacks are fewer samples, a 128² cube, then spreading over frames.
- **Look tuning.** Night needs auto-exposure; the disc's physical radiance, the stars and the moon need art direction against the goldens.
- **The M43 merge.** Prepare and extraction change under M43. M45 keeps its hunks there to single calls, with its code in its own files.
- **Byte identity.** The t3 swap, the copies and the reused bake program reach only views with a Sky. The filtered golden run of W45.2 and the gate check all 56.
- **Approximation.** Relit probes lack the bounce, so a noon interior reads darker and bluer than a static noon bake. This is §6's row and the look skill's advice (fill lights).
- **Hidden driving.** `DriveLights` is invisible to scripts' reads, as documented.
- **Relit-bake goldens** depend on the bake's device class (`Docs/Backlog.md:285`).
- **The changed fuzzed reader** waits for the program's hardened gate (ADR 0060 decision 7).
- **Phase 8's GPU lock.** Four renderer builders queue on one GPU, so every run stays filtered.

## The W45.1 record

W45.1 makes decision 3 exact where its CPU part leaves a choice, and fixes, M34's way, what a contract would freeze: the component, `RenderSky` and its two functions, `SkyConstants`, `SkyDispatchConstants` and the `SkyResults` layout, the two declarations with their rows, `RenderStats`' fields and the format-2 layout. Each is in the code at the wave's checkpoint, read by no pass until W45.2, so every image stays byte-identical: a scene without a Sky extracts, lights and renders exactly as before, and the cascade loop and the fog glow take today's lights in today's order without one.

- **The component** (`Scene/Components/SkyComponent.h`; `RenderingRegistration.cpp` after `Environment`; `BuiltinComponents.h` after `EnvironmentComponent`: 65 components on M45's branch, 69 at its merge): `Sky`, version 1, category Rendering, `UniquePerScene`, the default flags, so the generated proxy reads and writes every field. The fields, defaults and ranges are decision 3's table exactly: `Sun` and `Moon` `UUID` (EntityRef, null), `Intensity` 1 [0, 65504] (`MaxEnvironmentIntensity`), `DriveLights` true, `Haze` 1 [0, 10], `Overcast` 0 [0, 1], `GroundAlbedo` Color 0.3 [0, 1], `SunDisc` 1 [0, 1000], `MoonSize` 0.52 deg [0.1, 10], `Stars` 1 [0, 100], `Latitude` 45 deg [-90, 90], `NorthAngle` 0 deg [-360, 360], `Clouds` 0 [0, 1], `CloudSpeed` `glm::vec2` (5, 0) m/s, each [-100, 100]. The registry refuses an out-of-range write on every path; no cross-field rule exists (the Sun and the Moon are `SKY_LIGHT_INVALID`'s). `ClearMode::Skybox`'s description is decision 3's.
- **`RenderSky`** (`RenderSnapshot.h`, before `RenderSnapshot`): `{bool Enabled = false; UUID Sun, Moon; the twelve value fields with the component's defaults; double SimulationTime = 0; UUID Entity; std::optional<uint32_t> SunLight; glm::vec3 SunLightColor = 1; std::optional<uint32_t> MoonLight; glm::vec3 MoonLightColor = 1; bool HasMoon = false; glm::vec3 MoonDirection = (0, 1, 0)}`. **Choice:** it keeps the component's `Sun` and `Moon` references, so a snapshot built by hand (the goldens) resolves them as extraction does. `RenderSnapshot::Sky` is appended after M42's `PlanarReflections` on the branch (after M43's `StaticSet` at the merge); a plain value, so the snapshot's memory-resource rule is untouched.
- **The two functions** beside `ComputeReverseZProjection`, implemented in `Renderer/SkyAtmosphere.cpp` with DetMath:
  - **`ResolveSkyLights(RenderSnapshot&)`** (decision 3 names one function and the ledger two; this is the second): `SunLight` is the directional light whose `Entity` is `Sun` (the first directional light for a null `Sun`), `SunLightColor` its `Color`; `MoonLight` likewise for `Moon`, empty for a null `Moon`, a `Moon` that is the resolved Sun's entity, or one without a light. A Moon light sets `HasMoon` and `MoonDirection` (the opposite of its `Direction`); a Moon that is the Sun's, or null, clears `HasMoon`; a Moon without a light keeps what extraction set. Nothing without an enabled Sky.
  - **`ApplySkyLightDriving(RenderSnapshot&)`**: with `Enabled` and `DriveLights`, the Sun light's `Color` = `SunLightColor × T(toSun)` and the Moon light's `MoonLightColor × T(toMoon) × k`, T `ComputeSkyTransmittance` from `ComputeSkyCameraAltitude(Camera.Position)` (the origin without a camera) through `MakeSkyAtmosphere(Haze, Overcast, GroundAlbedo)`, k `ComputeMoonIlluminatedFraction`. **Choice:** without a Sun light the moon is lit as full (k 1). `MoonDirection` follows the Moon light's `Direction`, so a golden that turns both lights in the snapshot drives again. Idempotent ("SkyAtmosphere: driving the lights is idempotent and leaves the components' colours as authored").
- **Extraction** (`Scene/SkyExtraction.{h,cpp}`: `ExtractRenderSky(scene, request, snapshot)`, called once after the canonical loop beside `snapshot.Fog`; one header paragraph in `RenderExtraction.h`): a view of `SkyComponent` over valid entities without `HierarchyDisabledTag`. **Choice:** should a scene hold several (a file before its repair), the smallest entity UUID, so the choice never depends on the registry's order. A `Moon` naming an enabled entity other than the named Sun gives `HasMoon` and its rendered matrix's unit local +Z (`ComputeRenderedWorldMatrix` at the request's alpha; a degenerate axis gives no moon). Then `ResolveSkyLights` and `ApplySkyLightDriving`. Every view, an edit scene's at `SimulationTime` 0; without a Sky, `RenderSky{}`.
- **`Renderer/SkyAtmosphere.{h,cpp}`**, pure, double, DetMath (no `<cmath>` transcendental), fixed steps:
  - Hillaire's Earth: radii 6360 and 6460 km; Rayleigh (5.802, 13.558, 33.1) × 10⁻³ per km, scale height 8 km; Mie scattering 3.996 × 10⁻³ and absorption 0.444 × 10⁻³ per km (albedo 0.9), scale height 1.2 km, asymmetry 0.8; ozone (0.650, 1.881, 0.085) × 10⁻³ per km in a tent of half-width 15 km about 25 km.
  - **The overcast** (decision 3 leaves its model open): `MakeSkyAtmosphere` scales Mie scattering and extinction by `Haze + Overcast × SkyOvercastMieScale` (600: a vertical optical depth of about 3.2 at a full overcast, leaving about 4 % of the sun at the zenith) and lerps the asymmetry toward `SkyOvercastMieAsymmetry` (0.3), so the storm's sky turns grey and its sun dims within the one model the GPU evaluates, with no second blend to keep in step. Out-of-range or non-finite inputs are clamped or take the component's defaults.
  - `ComputeSkyTransmittance(atmosphere, radius, mu)`: zero when the ray meets the ground; otherwise exp(-depth) over `SkyTransmittanceSteps` (40) midpoint steps to the top, what each transmittance LUT texel holds at its (radius, mu). `ComputeSkyTransmittance(altitude km, direction, haze, overcast)`: the same at `SkyBottomRadius + altitude`, mu = `direction.y`. `ComputeSkyCameraAltitude(position)`: max(y, 0) m + `SkyCameraHeightOffset` (1 m), in km.
  - `ComputeMoonIlluminatedFraction(toSun, toMoon)` = (1 − cos ψ) / 2, 0 for a zero direction.
  - `ComputeSkyPole(latitude, northAngle)` = cos(latitude) north + sin(latitude) up, north being −Z turned counter-clockwise (seen from above) by `NorthAngle`. `ComputeSkyStarFrame(toSun, latitude, northAngle)`: the rows of the world-to-celestial rotation, the pole as +Z, X the meridian (east × pole, east = north × up), Y = Z × X, turned by −h about the pole, h the Sun's angle about the pole from X toward Y (0 for a zero sun).
  - `OrderShadowCandidates(snapshot)`: the indices of the directional lights, an enabled Sky's `SunLight` first while its direction toward it has y > 0, else its `MoonLight`, then every other directional light in snapshot order; without a Sky or a role, snapshot order. `GetSkyGlowLight(snapshot)`: an enabled Sky's `SunLight`; **choice:** otherwise (no Sky, or a Sky whose Sun resolves to no light) the first directional light, today's rule.
  - The sizes W45.2 builds to: transmittance 256 × 64, multiple scattering 32², sky view 192 × 108 over (world azimuth, view elevation) (**choice:** the world azimuth, not the azimuth from the sun, since the moon scatters at its own), the cube 256 (`SkyCubeSize`, the asset's `SpecularFaceSize`), `SkyCloudHeight` 2,000 m, `SkyCloudLightingStep` 1 s.
- **The fog's glow** (`Fog.h`): `MakeFogConstants(fog, camera, lights, std::optional<size_t> glowLight, environment, fogged)`; the glow follows `lights[glowLight]` when it is a directional light with a finite, non-zero direction, else +Y and zero. Both callers (`SceneRenderer.cpp`'s Prepare and `SceneRendererReflections.cpp`'s slot fog) pass `GetSkyGlowLight(snapshot)`, so a scene without a Sky glows as before.
- **The cascades** (`SceneRenderer.cpp`'s Prepare, M45's region): the loop visits `OrderShadowCandidates(snapshot)` instead of every light in snapshot order; its body is unchanged.
- **The declarations** (`RenderPassDeclaration.h`, frozen): `SkyLighting` (Ambient, after `LightClusters`, rows `SkyLuts`, `SkyIbl` and `ProbeRelight`, no target) and `Sky` (Forward, directly after `Skybox`, row `Sky`, target `SceneColor`); the header comment names them, and `RenderStage.h`'s Ambient comment the sky's lighting. **Departure (form only), as M42's W42.1:** both are declared without pipelines, so nothing records them, no `stats.get` output changes and no stub exists; W45.2 sets `PipelineCount` 10 and 1 and the programs (88 pipelines, 35 programs). 45 stats rows.
- **`RenderStats`** (after M42's fields on the branch, after M43's at the merge): `uint32_t SkyUpdates` and `RelitIrradianceVolumes`, 0 until W45.2 and W45.3 fill them; `stats.get`'s names and the readout's member land with W45.4.
- **The shared layouts** (`Shared/SkyConstants.h`; "Shaders: SharedStructsMatchReflection" checks `SkyConstants` against the Sky program's reflection):
  - `SkyConstants`, 224 bytes, a constant buffer: `RayleighScattering` (float3) and `BottomRadius` at 0; `OzoneAbsorption` and `TopRadius` at 16; `GroundAlbedo` and `MieScattering` at 32; `MieExtinction`, `MieAsymmetry`, `CameraAltitude` (km) and `Intensity` at 48; `SunDirection` and `SunCosRadius` at 64; `SunIlluminance` (the Sun light's authored Color × Intensity) and `SunDiscRadiance` (`SunDisc` over the disc's solid angle; 0 draws none, a `LightAngle` of 0 included) at 80; `MoonDirection` and `MoonCosRadius` at 96; `MoonIlluminance` (authored) and `MoonFraction` at 112; `StarFrame0` to `StarFrame2` (rows, float4) at 128; `Stars`, `Overcast`, `CloudCoverage` and `CloudHeight` at 176; `CloudOffset` (float2, m: `CloudSpeed × SimulationTime` in double, wrapped to the noise's period), `HasMoon` and `Padding0` at 192; `FogColor` and `FogAmbientTint` (made usable as `MakeFogConstants` makes them) at 208.
  - `SkyDispatchConstants`, 32 bytes, the computes' push constants: `{float2 CloudOffset; float Altitude; uint32_t Face; uint32_t Count; uint32_t Mode; uint32_t Padding0, Padding1}`, reflected with W45.2's computes. **Choice:** the image-based light's clouds take their stepped offset here, so the Sky pass keeps the live one in `SkyConstants`.
  - **`SkyResults`**, 18 float4 (`SkyResultsCount`): the irradiance SH9 × Intensity at 0 to 8 (`SkyResultsIrradiance`), the tinted fog colour at 9 (`SkyResultsFogColor`), and reflection probe i's relit intensity in the x of 10 + i (`SkyResultsReflectionScales`).
- **The copy** (`Renderer/SkyLighting.{h,cpp}`): `RecordSkyResultCopies(commandList, results, environment, fogs, probes, reflectionProbes)` records decision 3's copies: 144 bytes at offset 0 of `EnvironmentConstants`, 12 bytes at offset 0 of each `FogConstants` (the view's, then each drawn mirror slot's), and 4 bytes at 896 + 192 i + 156 of `ProbeConstants` for each i below the count (at most `MaxViewReflectionProbes`); null destinations are skipped. W45.2 records it at the end of the Ambient stage, outside every render pass.
  - **What the proof found:** the copy alone is not enough. `writeBuffer` (`vkCmdUpdateBuffer`) leaves a constant buffer in `CopyDest`, and `copyBuffer` writes it in the same state, between which NVRHI places no barrier, so synchronization validation reported a write-after-write hazard on every destination (the case's first run, at API 1.4 and 1.3; every copied value was right). `RecordSkyResultCopies` therefore first moves its destinations to `ConstantBuffer` (`setBufferState`, `commitBarriers`): that barrier completes the CPU writes, and the copy's own barrier back to `CopyDest` chains after it. A destination a pass already read is in `ConstantBuffer` already. The GPU copy stands; the read-back fallback is not needed.
  - **The proof:** "SkyLighting: a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them" (`SkyLightingTests.cpp`, gpu). Two command lists, submitted back to back before either is waited for, each: CPU writes (`writeBuffer`) of a `SkyConstants`, an `EnvironmentConstants`, a view's and a mirror slot's `FogConstants` and a `ProbeConstants`, all static constant buffers made as `SceneRenderer.cpp` makes them; the Sky program's test entry `CSTestCopyWrite` writing a `SkyResults` buffer from the frame's seed; `RecordSkyResultCopies` with three reflection probes; then `CSTestCopyRead`, bound at the forward view's registers (b2, b4, b5), reading the constants into a buffer read back, once with each fog. It checks the copied bytes, every untouched neighbour (`AmbientColor`, `Intensity`, the rotation and the lods, `HasEnvironment`, `Density`, `CameraPosition`, `StartDistance`, the fourth to eighth probes' `AxisScale.w`, every `AxisScale.xyz` and `Lookup`) and both frames. The two test entries live in `Passes/Sky.slang` (no pass creates their pipelines, as Gtao's `CSTestUpsample`), their layouts in `Tests/Source/Support/SkyCopyProgram.{h,cpp}`, checked by "Shaders: LayoutsMatchReflection"; the Sky program joins `Shaders.json` between `Skinning` and `Skybox` (35 programs) with them, and W45.2 adds its own entries.
- **The format-2 layout** (implemented by W45.3): the payload is version 1's followed by `uint32 HasRelight` (0 or 1) and, with 1, per probe in the probes' order `{9 × f32x3 Local; 45 × f32 Transfer}`, `Transfer` the upper triangle of the symmetric 9 × 9 T row by row (T₀₀, T₀₁ … T₀₈, T₁₁ … T₈₈). The cooked payload is always version 2; the source file is version 1, byte for byte today's, when `HasRelight` is 0.
- **Validation** (`Scene/SkyValidation.{h,cpp}`: `FindSkyIssues(scene)`, `SkySceneIssue`; `CheckSky` in `ProjectValidator.cpp` after `CheckPlanarReflections`, so after M43's `CheckStaticScenery` at the merge; both tables after M42's `RENDER_REFLECTION_INVALID` on the branch). For the first effectively enabled Sky in canonical order, at most one `SKY_LIGHT_INVALID` on `Sun` (a named entity without a `DirectionalLight` or a disabled one, or a null `Sun` in a scene without an enabled `DirectionalLight`) and one on `Moon` (the resolved Sun's entity); then `SKY_ENVIRONMENT_IGNORED` on each effectively enabled Environment. Warnings, not fixable; dangling references are `ENTITY_DANGLING_REFERENCE`'s. **Departure (timing):** `SkyDiagnosticCodes` holds the two codes until W45.3, which appends `PROBE_NOT_RELIGHTABLE` with its check in `CheckProbes`, since that check reads the relight block W45.3 adds. `SCENE_DUPLICATE_UNIQUE_COMPONENT`'s description names the Sky. The fixtures (`MakeValidationFixtures.py`, `COMPONENT_ORDER` with `Sky` after `Environment`): a Sky whose Sun names the camera, and a Sky beside an Environment. The game-debugging skill's two `validation-codes` rows and §13.7's sentence are written.
- **`RENDER_NO_LIGHTING`** (`RenderAnnotations.cpp`'s illumination block): an enabled Sky of positive `Intensity` illuminates.
- **FeatureTest.** Its scaffolds land with W45.4. Until then the feature suite's gates 1, 2 and 4 name the Sky as uncovered (gate 2: registering `Sky` adds the `Entity.Sky` shortcut, a readable property, so `api:Entity.Sky/read` is a counter), as M42's W42.1 left its own, and the milestone-end gate runs on W45.4's tree. **W45.4's `Sky` suite therefore reaches the proxy through the shortcut, `entity.Sky`** (as `Fog.test.luau` does with `Find("Haze").Fog`), never through `GetComponent("Sky")` alone, so gate 2 is covered.
- **Documents:** Architecture §5.3's `Sky` row and §13.7's codes; the header comments of `RenderSnapshot.h`, `RenderPassDeclaration.h`, `RenderStage.h`, `RenderExtraction.h`, `RenderAnnotations.h` and `Fog.h`; game-debugging's `validation-codes.md`.
- **Tests:**
  - `SkyAtmosphereTests.cpp`: the five `SkyAtmosphere:` acceptance cases.
  - `FogTests.cpp`: "Fog: with a Sky, the glow follows its Sun", and the existing cases with their glow light passed.
  - `RenderExtractionTests.cpp`: "RenderExtraction: a Sky is extracted with its Sun and Moon in every view, and a scene without one extracts as before".
  - `SkyValidationTests.cpp`: "SkyValidation: a Sun or Moon that names no usable light, and an Environment beside a Sky, are reported".
  - `SkyLightingTests.cpp`: the copy's gpu case.
  - `RenderPassDeclarationTests.cpp`: the 45-row order and "RenderPassDeclarations: the sky's lighting ends the Ambient stage and the Sky follows the Skybox"; the planar reflections' case now finds `SkyLighting` before them.
  - `RenderingRegistrationTests.cpp`: "RenderingRegistration: the Sky follows the Environment, unique per scene, with its fields' kinds and ranges"; `BuiltinComponentsTests.cpp`: 65; `ProjectValidatorTests.cpp`: the count and order lines; `ShadersTests.cpp`: `SkyConstants` and the two test layouts.
- **Generated references:** `Components.md`, the scene and prefab schemas, `Methods.json`, `ScriptAPI.md`, `Validation.md` and `Engine.d.luau` (the proxy type); `Tools/MCP/catalog.json` is unchanged (it holds no component schema).
- **Runs** (2026-11-24, the wave's checkpoint on `milestone/m45`):
  - Debug build: 0 warnings.
  - unit, Debug, filtered (`SkyAtmosphere*`, `Fog*`, `RenderExtraction*`, `SkyValidation*`, `RenderPassDeclarations*`, `RenderingRegistration*`, `BuiltinComponents*`, `ProjectValidator*`, `Validation*`, `Shaders*`, the snapshot, stage, annotation and terrain cases): 197 cases, 2 failing on the orders M45 changes ("BuiltinComponents: registry names are those of the component table" without `Sky`, and M26's places case reading `Skybox` before `Ssr`), both updated.
  - unit, Debug, unfiltered (`--parallel 4`): 5,390 passed, the 48 child-process targets skipped.
  - gpu, Debug, "SkyLighting: a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them", Vulkan 1.4 and 1.3: failed first on the write-after-write hazard (10 synchronization-validation errors each, every value right), passed after the fix.
  - `Lint.py` (0 findings), `Format.py --check`, `GenerateDocs.py --config Debug --check` and `MakeValidationFixtures.py --check`: passed.
  - No golden run: nothing W45.1 adds draws; a scene without a Sky renders as before.
- **After the lead's review** (`bin/Reviews/P8/M45-W1-review.md`: 0 blocker, 0 major, 5 minor, 5 nit; every finding fixed with W45.2's second checkpoint):
  - **Tests** (each run alone with its behaviour reverted, failing, then passing restored): "RenderSceneValidation: an enabled Sky of positive Intensity illuminates" (`RenderAnnotationsTests.cpp`: an unlit scene, a Sky, its `Intensity` 0, then 1 on a disabled entity); "Fog: with a Sky, the view's sky glows toward its Sun" (`FogTests.cpp`, gpu: a Moon light first and the Sun second, the cleared pixels fogged as the sky match the reference glowing toward the Sun, and over 200 differ from the one toward the Moon); "SceneRenderer: with a Sky, the cascades' shadow follows its Sun by day and its Moon by night" (`SkyLightingTests.cpp`, gpu: an occluder before a wall and the two lights, the Moon's first; by day the wall behind it along the Sun is darker than along the Moon, and with the Sun below the horizon, still lighting the wall, the Moon's shadow falls).
  - **Runs:** the five fog gpu cases and "SceneRenderer: a terrain is fogged as a mesh is" at Vulkan 1.4 and 1.3, and the existing goldens with fog or directional shadows, filtered in Release, byte-identical (the W45.2 record's second checkpoint lists them).
  - **Documents:** the glow and cascade rules where they still named the first directional light: Architecture §5.3's `Fog` row, the Fog paragraph and §8.7; `FogRegistration.cpp`'s `SunGlow` description (regenerated into `Components.md`, the schemas and `Engine.d.luau`); `FogComponent.h`, `RenderSnapshot.h`'s `RenderFog::SunGlow` and `Shared/FogConstants.h`'s comments; this record's gate 2 (the FeatureTest bullet and decision 3's gates).
  - **Nits:** the re-wrapped comments of `SkyAtmosphere.h` (`SkyCubeSize`), `RenderSnapshot.h` (`ApplySkyLightDriving`) and `SkyLighting.h`; `SkyValidation.h`'s Sun without a light (the glow and cascades go to the other directional lights); the registration case checks the Sky's place right after the Environment and `REQUIRE`s each field; `ResolveSkyLights`' precondition documented in `RenderSnapshot.h` (the lights' authored colours: before `ApplySkyLightDriving`, or restore them first); "twelve" value fields and `catalog.json` unchanged; `Sky.slang`'s unused `SkyTestReadbackCount` removed.

## The W45.2 record

W45.2 puts the sky on the GPU. Its first checkpoint (this section's "First checkpoint") holds the LUTs, the Sky pass, the run-time image-based light with its copies, the t3 swap and the Sun's roles in Prepare, the mirror views' sky, and the gpu cases of the LUTs, the prefilter, the furnace, the sun's following and the key; the second holds the mirror and capture case, device loss, out of memory, `GoldenSky.jsonl`'s cove scene and the two goldens.

### First checkpoint

- **Shaders** (`Passes/Sky.slang`, `Common/Atmosphere.slang`; the Sky program's entries in `Shaders.json`: `CSTransmittanceLut`, `CSMultipleScatteringLut`, `CSSkyViewLut`, `CSSkyCube`, `CSSkyFinalize`, `VSMain`, `PSMain`, then W45.1's two test entries):
  - **Set 0 of the computes:** b1 `SkyConstants`, t0 the transmittance LUT, t1 the multiple-scattering LUT, t2 the sky-view LUT, t3 `RadianceTotals` (the bake's reduced totals), s0 `LinearClamp`, s1 `LinearWrapU` (the sky-view LUT wraps in azimuth), u0 a LUT (RGBA16F), u1 the cube's level 0 (six layers, RGBA16F), u2 `SkyResults`; push constants `SkyDispatchConstants`. Each entry's layout holds only what it reads (`SkyLighting.cpp`'s `MakeSkyComputeDescription`).
  - **The Sky pass's set 0:** b0 `ViewConstants`, b1 `SkyConstants`, b5 `FogConstants`, t0 transmittance, t2 sky view, s0, s1 (`SkyPass.cpp`).
  - **The push constants:** slangc's reflection lists the program's global push constants (`Dispatch`) for every entry, the vertex shader's included, whether it reads them or not, so every Sky layout (the computes', the pass's, visible to the vertex and pixel stages, and W45.1's two test layouts) carries one `SkyDispatchConstants` range, and every dispatch and draw sets it (zeroed where unread).
  - **The atmosphere** (`Common/Atmosphere.slang`) is `SkyAtmosphere`'s in float: the same extinction, 40 transmittance steps, Bruneton's transmittance mapping (texel centres at 0 and 1), Rayleigh's and Cornette-Shanks' phase functions, the multiple-scattering LUT over (cos sun zenith, height), and the sky-view LUT over (world azimuth, Hillaire's non-linear zenith mapping with the horizon in the middle row).
  - **The multiple-scattering LUT:** 64 directions (8 × 8, uniform in cos θ and φ), 20 steps each, the ground's Lambertian bounce, Ψ = L₂ / (1 − f_ms).
  - **The sky-view LUT:** 30 steps along the view ray to the top or the ground, the Sun's illuminance (`SunIlluminance`) and the Moon light's (`MoonIlluminance × MoonFraction`) each scattered by single scattering (through the transmittance LUT) and Ψ, with Hillaire's energy-conserving step integration; radiance before `Intensity`.
  - **The cube** (`CSSkyCube`, level 0 of a 256² cube with its 9 mips): the sea-level LUT; below the horizon, **choice:** the atmosphere's ground, `GroundAlbedo / π` times the light reaching it (the Sun's and the Moon light's through the transmittance, and π times the zenith's radiance), so the image-based light's lower hemisphere is not black; the clouds at their stepped offset, seen from the origin's XZ (**choice:** the camera's XZ is not in the key, so the light does not regenerate as the camera moves); no discs, no stars.
  - **The clouds:** value noise on a 256-cell period of 4,000 m cells (a 1,024 km layer), five octaves; coverage `smoothstep(1 − Clouds, 1 − Clouds + 0.35, density)`, faded toward the horizon and beyond 60 km; lit by the Sun's transmittance at 2 km with a forward lobe (Cornette-Shanks g 0.6), the Moon light's, and the zenith's radiance as ambient.
  - **The stars:** a 512-cell cube grid in the star frame, 6 % of the cells holding a star at a hashed place, brightness (a sixth-power distribution) and tint, a Gaussian of a fifth of a cell, times `Stars × 0.05`, faded as the sky's own luminance rises (none above 0.05). Hashes are PCG's output permutation; nothing is read from a texture.
  - **The sun's disc:** `SunIlluminance × T(view) × SunDiscRadiance × (1 − 0.6 (1 − μ))`, μ across the disc, times `1 − Overcast`. **The moon:** a sphere of `MoonSize`, lit by `SunDirection` (its phase), its maria `0.6 + 0.4 ×` the cloud noise over the disc, `MoonIlluminance` over the disc's solid angle, through the transmittance, times `1 − Overcast`.
  - **`CSSkyFinalize`:** `EnvironmentBaker`'s `MakeIrradianceSH9` in float (4π over the solid angle, the cosine lobe 1, 2/3, 1/4, the Hanning window of `IrradianceShWindow` 4) times `Intensity`, and the fog's colour, `FogColor × lerp(1, max(SH₀ × Y₀₀, 0), FogAmbientTint)`.
- **`Renderer/SkyLighting.{h,cpp}`:** `SkyLutKey`, `SkyLightingKey` (the view's constants with the camera's altitude, the live cloud offset and the fog cleared, plus the stepped offset; compared byte-wise), `SkyViewResources`, `MakeSkyConstants`, `ComputeSkyCloudOffset`, `MakeSkyLightingKey`, `CreateSkyViewResources` and `SkyLightingPass` (`RecordLuts`, `RecordImageBasedLight`, `RecordCubeLighting`, `RecordFinalize`). The prefilter's samples run from `SkySpecularMinSamples` 16 at mip 1 to `SkySpecularMaxSamples` 64 at the last, the bake's rule; the SH9 projection reads the cube's level 2 (64², the bake's `IrradianceMaxFaceSize`).
- **`Renderer/SkyPass.{h,cpp}`:** the Sky pass's pipeline (as `SkyboxPass`: GreaterOrEqual at depth 0, writes off), its two samplers, `Record` with an optional scissor.
- **The scene renderer** (`SceneRendererSky.cpp`; M45's regions in `SceneRenderer.cpp` and `SceneRendererReflections.cpp`):
  - Prepare's environment block: `PrepareSky` (an enabled Sky in a Lit view with a camera outside the wireframe; the view's `SkyViewResources` created on its first such render, and on failure the view lit by its Environment, the error kept), then `MakeSkyEnvironmentConstants` (`Private/LightingInputs`) and the sky's specular cube at t3 in the Environment's place; `DrawsSky` (the Sky lights the view and its camera clears to the skybox) replaces `DrawsSkybox`, and `SkyIsClearColor` is 0 when either draws.
  - `RecordAmbientStage` ends with `RecordSkyLighting`: the `SkyConstants` written, row `SkyLuts` (the LUTs when `SkyLutKey` changed, then the camera's sky view), row `SkyIbl` (the update when `SkyLightingKey` changed, then the finalize and `RecordSkyResultCopies` into the view's environment and fog constants and each drawn mirror slot's fog). The keys are kept only once recorded.
  - `RecordForwardStage`'s sky block draws the Sky (row `Sky`) in the Skybox's place; mirror views draw it with their scissor and fog, sampling the main view's sky-view LUT.
  - `RenderStats::SkyUpdates` is 1 for a render that regenerated the light.
  - The bindings: `SkyLightingBindings` (cleared with the layouts) and `SkyBindings` (with the targets).
- **Pipelines:** `SkyLighting` 9 (the Sky program's five computes and EnvironmentBake's four), `Sky` 1: **87** at this checkpoint; **departure (timing):** W45.3 adds `CSProbeRelight` to `SkyLighting` (10, 88), since the relight lands with it.
- **Tests** (`SkyLightingTests.cpp`, gpu): "SkyLighting: the transmittance and multiple-scattering LUTs match their CPU reference" (every transmittance texel within 1 %, every fourth multiple-scattering texel within 5 %, against double references in the test), "SkyLighting: the run-time prefilter of a cube matches the environment bake's" (the bake at the sky cube's size and sample counts: the specular cube byte for byte, the SH9 within 0.1 %), "SkyLighting: a constant sky gives a constant irradiance", "SkyLighting: the view's ambient light, reflections and fog colour follow the sun" and "SkyLighting: the sky's lighting is regenerated only when its key changes"; `ShadersTests.cpp` reflects `SkyDispatchConstants`; the pipeline count cases read 87.
- **Runs** (2026-11-24, the first checkpoint):
  - Debug build: 0 warnings.
  - unit, Debug, filtered (`Shaders*`, `RenderPassDeclarations*`, `SkyAtmosphere*`, `Fog*`): 20 passed, after the push constants' layouts (the first run found every Sky entry reflecting `Dispatch`, and NVRHI's slot clash of push constants with b0 in the pass's layout, now slot 3).
  - gpu, Debug, filtered (`SkyLighting*`, `Pipelines*`, `Fog*`, `TerrainPass*`), Vulkan 1.4 and 1.3: 17 passed each. The run-time prefilter matched the bake byte for byte: the EnvironmentBake entries take the sky cube as they take a skybox (decision 3's UNVERIFIED item, confirmed).
  - `Lint.py`, `Format.py --check`, `GenerateDocs.py --config Debug --check` (log `gendocs-check-3.log`): passed; `Lint.py` found one missing designated initializer (`SkyDispatchConstants::CloudOffset`, which has no default member initializer), fixed, then passed (log `lint-4.log`).
  - No golden run: the second checkpoint runs the filtered existing goldens with its own two.

### Second checkpoint

- **The mirror views and the captures** (no code beyond the first checkpoint's): "SkyPass: a mirror view and a probe capture draw the sky" (gpu) shows a mirror's glass equal, within 2 %, to what the camera mirrored in its plane renders of the sky (the mirror view draws the Sky in the skybox's place with its scissor, sampling the main view's sky-view LUT by world direction), the mirror's row one draw more than without the Sky, and a probe capture's upward face blue where the Sky is drawn and black (the clear colour) without it; a capture draws the sky from the probe's position, since its camera is the probe's.
- **Device loss** (place 2): "SkyLighting: a sky scene renders the same bytes after a device loss" (gpu, `Test::RunDeviceLossTest` over the cache, the pipeline set and the renderer): the recovered renderer creates the view's `SkyViewResources` again and regenerates the LUTs and the light from empty keys, to the same bytes.
- **Out of memory:** "SkyLighting: a sky whose cubes cannot be created lights with the Environment and the render returns its error" (gpu): the cubes' creation fails (`CreateSkyViewResources` keeps nothing), the render returns `Gpu`, records no sky row, and its image is byte for byte a Sky-less render's (lit by the Environment's constant ambient); the next render creates them and draws the sky. **Choice (test support):** the fault is the engine's own, `GpuDiagnostics::SetTextureCreationFault` (`Support/TextureCreationFails.h`), before NVRHI, as M42's binding-set fault, since a failed `vkCreateImage` trips NVRHI's `ASSERT_VK_OK` in `createTexture` in a Debug build, so a Vulkan-level injection (`Support/BufferCreationFails.h`'s way) cannot reach a texture; "GraphicsDevice: a texture the test fault selects fails before NVRHI, and the others are created" (gpu) covers it. Never in Dist (`GpuTestHooksEnabled`).
- **The goldens** (`Projects/FeatureTest/Scaffold/GoldenSky.jsonl`, IdSeed 4203, `Renderer` vulkan, `GoldenDirectories` `Assets/Lighting/Scenes/GoldenSky`, `Assets/Materials/GoldenSky`, `Assets/Scenes/GoldenSky`, after M42's `GoldenPlanarReflections.jsonl` and before `Suites.jsonl` in `Scaffolds.json`; M43's and M44's entries precede it at the merge). **Departure (naming):** the cove scene is `SkyDay.scene`, not `Cove.scene`, and W45.3's probes scene will be `SkyProbes.scene`, not `Probes.scene`: a golden scene's image is named after its scene ("GoldenScene: every golden scene has its golden image in every committed device class"), and the scene list refuses a name twice, where M26's `GoldenProbes.jsonl` already has `Probes`. `SkyDay.scene`: a sand plane, a glossy water plane (roughness 0.04) standing in for M47's water, a rock, a hut with its roof, a `Sky` naming `Sun` (a shadow-casting `DirectionalLight` of intensity 4, `LightAngle` 0.53) and `Moon` (a dim, bluish shadow-casting `DirectionalLight`) with a 2° moon (`MoonSize`, so its phase reads in a tile) and a cloud layer (`Clouds` 0.35), a haze (`Fog` tinted by the ambient light), no Environment, and **automatic exposure** (**departure** from decision 3's "manual exposure": one exposure cannot show both the noon sky and the moonlit night, as the record's look note foresaw; a capture with automatic exposure renders a fixed warm-up from a reset history, so it is byte-identical).
  - `Tests/Source/Golden/SkyGoldenTests.cpp`: `Golden: SkyDay` (six 320 × 180 tiles in a 3 × 2 grid: the sun at dawn, 2° up and 40° left of the view; morning, 20°; noon, 65°, above the view; afternoon, 25° on the right; dusk, 1°; and night, the sun 25° below the horizon, with the gibbous moon 25° up in view and the stars) and `Golden: SkyOvercast` (two tiles: `Haze` 3 with `Clouds` 0.6, and `Overcast` 1 with `Clouds` 1, the sun 30° up). Each tile turns the Sun light (and the Moon light) in the extracted snapshot and drives the lights again with `ApplySkyLightDriving` (the roles stay resolved, so no `ResolveSkyLights`, whose precondition the review's finding 8 documents); each grid is rendered twice to the same bytes.
- **Documents:** Architecture §8.3 (the `SkyLighting` and `Sky` rows, the "Sky" paragraph), §8.4 (the Sky program's layouts), §8.6 ("Run-time sky lighting", the copies), §8.14 (place 2's `SkyViewResources`, and a test's textures among the injected faults); `SceneRenderer.h`'s pass list and pipeline count; the golden-images skill's scene list.
- **Look tuning** against the candidates: the Sky pass draws the atmosphere's ground below the horizon (`EvaluateSkyGround` in `Passes/Sky.slang`, the cube's own ground term, now shared by both), which the first checkpoint left black, so the cove showed a black band between its ground's far edge and the horizon. The radiances stay physical, the disc's included: the cove reads with automatic exposure and AgX, and a game scales its sky with `Intensity`.
- **Runs** (2026-11-24, the second checkpoint):
  - Debug and Release builds: 0 warnings.
  - unit, Debug, filtered: `RenderSceneValidation*`, `RenderingRegistration*` (14 passed); `DocExamples*`, `RenderSceneValidation*`, `RenderingRegistration*`, `SkyAtmosphere*`, `Fog*`, `BuiltinComponents*`, `Shaders*` (51 passed); `GoldenScene*` with the new images (6 passed).
  - gpu, Debug, filtered (`SkyLighting*`, `SkyPass*`, `SceneRenderer: with a Sky*`, `Fog*`, `SceneRenderer: a terrain is fogged*`, `GraphicsDevice: a texture the test fault*`), Vulkan 1.4 and 1.3: 20 of 21 passed each; the mirror case read its stats row through a pointer into a returned copy (a test bug: `GetRenderStats` returns by value), fixed, then `SkyPass*` passed at both caps. After the Sky pass's ground (look tuning): `SkyLighting*`, `SkyPass*`, 9 passed at both caps.
  - The review's three new cases, each run alone with its behaviour reverted (the Sky rule in `RenderAnnotations.cpp`, the cascade loop in snapshot order, the view's glow toward light 0): all three failed (unit; gpu at Vulkan 1.4), then passed restored.
  - `Scaffold.py` (Release) and `Scaffold.py --check`: passed, `GoldenSky.jsonl` in 7 to 10 s.
  - golden, Release, `--update-golden --filter "Golden: Sky*"`: the two candidates written and reviewed by eye, twice (the first showed the black band below the horizon, fixed as above, and a moon too small to read).
  - golden, Release, filtered over the 18 existing goldens a sky change could reach (`Golden: Fog*`, `LitScene`, `MaterialGrid`, `IblOnly`, `PlanarReflections`, `Probes`, `Shadows*`, `SpotShadows`, `Ssr`, `Terrain`, `ParticlesInFog`, `AutoExposure`, `MaterialParameters`): 18 matched, each comparison exactly as at main's phase-7 gate (`bin/Reviews/P7/scratch/GATE/ci-milestone.log`: largest differences 0/255 except `MaterialGrid` 6/255 at 2 pixels, `ShadowsOrtho` 5/255 at 1 pixel and 1/255 for `IblOnly`, `ShadowsNear`, `ShadowsFar` and `SpotShadows`, the references' standing differences on this machine), so no render moved.
  - `Lint.py` (0 findings; its banned and JSON steps again over `Sky.slang` and the scaffolds after the last edits), `Format.py --check`, `GenerateDocs.py --config Debug` then `--check` (log `bin/Reviews/P8/scratch/M45-builder/gendocs-check-w45.2b.log`): passed.

### After the lead's review

The lead's review of W45.2 (`bin/Reviews/P8/M45-W2-review.md`: 0 blocker, 0 major, 6 minor, 9 nit; the departures accepted) is fixed with W45.3's checkpoint, each fix with a case that failed without it (W45.3 record's runs):
- **1, a shader reload.** The renderer's generation-change block (`SceneRenderer.cpp`, beside the shadow cache's clear) resets the sky's two keys and forgets the view's probe copies (`ProbeViewResources::CopiedVolumes`, so they are copied again from their bakes and relit), so the LUTs, the image-based light and the relit volumes regenerate with the new shaders. Case: "SkyLighting: the sky's lighting is regenerated after the pipelines are created again" (`SkyUpdates` 1, the `SkyLuts` row's 3 dispatches, after `CreatePipelines`).
- **2, the key.** `MakeSkyLightingKey` keeps only what the update reads: it also clears `Intensity` (the finalize applies it every frame, and the shaders' `EnvironmentConstants::Intensity`), `SunCosRadius`, `SunDiscRadiance`, `MoonCosRadius`, the star frame, `Stars` and `Overcast` (the Mie terms carry it); the moon's direction, fraction and flag while `MoonIlluminance` is zero; and the stepped cloud offset while `CloudCoverage` is 0. So the key is: the atmosphere's coefficients and radii, the ground albedo, the Sun's direction and illuminance, the Moon's direction, illuminance and fraction (with a light), the cloud coverage and height, and the stepped offset (with clouds). The key case checks a new `Intensity` (0, the image still changing), new stars, disc and moon size (0), a turned Moon without a light (0), a cloudless drift across a step (0), and keeps a drift with clouds and a turned Sun (1); its comment is exact. W45.3's relight follows the Intensity itself (`ProbeViewResources::RelitIntensity`).
- **3, `RENDER_NO_LIGHTING`.** With an enabled Sky only its `Intensity` and the lights count; the Environment, its default fallback colour included, counts only without one. "RenderSceneValidation: an enabled Sky of positive Intensity illuminates" checks a scene without an Environment entity: lit without a Sky, unlit under a Sky of `Intensity` 0, lit at 1.
- **4, the reflections.** "SkyLighting: the view's ambient light, reflections and fog colour follow the sun" adds a chrome quad (`Metallic` 1, `Roughness` 0.1), its colour the sky's specular cube alone: positive at noon, above twice dusk's, redder relative to blue at dusk.
- **5, the ledger.** Amended on main by the lead (`06551952`); decision 3's FeatureTest bullet and decision 6's goldens name `SkyDay.scene` and `SkyProbes.scene`.
- **6, the unfiltered unit suite:** run on W45.3's tree before its commit (W45.3 record's runs).
- **7, two limits** joined decision 1's "Accepted at landing" (the image-based light's clouds over the world origin; the atmosphere's ground without extinction), for W45.4's Backlog §6 rows.
- **8:** game-building's `post-processing.md` names the Sky's Sun for the glow.
- **9:** the four long comment paragraphs re-wrapped (`SkyLighting.h`'s copy paragraph, `SkyValidation.h`'s two, `SkyPass.h`'s, `Sky.slang`'s `PSMain`).
- **10:** `SkyViewResources` is dropped on a device loss, not a trim (its comment; a reload empties its keys).
- **11, the moon without a Sun light.** `MakeSkyConstants` lights it full as `ApplySkyLightDriving` drives it (k 1): `MoonFraction` 1 and `SunDirection` opposite the Moon, the disc lit from behind the viewer while no sun scatters or shines (the star frame keeps its own +Y sun). Case: "SkyLighting: without a Sun light, the sky's moon is full, as its driven light is".
- **12 and 13, the Sky pass.** No star behind the moon's disc, and no disc, moon or star in an orthographic view (one direction for every pixel). Case: "SkyPass: the moon hides the stars behind it, and an orthographic view draws no disc or star" (a 10° moon's disc the same bytes with and without stars, which show around it; an orthographic view along the sun far below the disc's radiance).
- **14:** the device-loss case's comment says what it checks.
- **15:** Architecture §15.4 names M45's goldens, and §15.5's golden-scenes row `GoldenSky/SkyDay.scene` and `GoldenSky/SkyProbes.scene` (with M42's `GoldenPlanarReflections/PlanarReflections.scene`, the review's out-of-scope note).

## The W45.3 record

W45.3 relights the irradiance volumes by the sky: decision 3's "Relit probes" exact, with one departure (the bake's sky).

- **Format 2** (`Asset/IrradianceVolumeData.{h,cpp}`; W45.1's layout): `IrradianceVolumeData::FormatVersion` 2; `IrradianceProbeRelight {std::array<glm::vec3, 9> Local; std::array<float, 45> Transfer}` (`IrradianceTransferCount` 45, the upper triangle row by row) and `IrradianceVolumeData::Relight`, empty or one per probe. `ValidateIrradianceVolumeData` adds the block's count and its values' finiteness. The payload (format 2) is version 1's, then `uint32 HasRelight` (Parse unless 0 or 1) and the block, checked against the bytes left before anything is allocated. `WriteIrradianceVolumeSource` writes version 1 (`IrradianceVolumeSourceVersionWithoutRelight`, M26's bytes) without a block and version 2 with one; `ReadIrradianceVolumeSource` reads both (UnsupportedVersion otherwise). The cooked payload is format 2; `IrradianceVolumeImporter::Version` stays 1. The fuzz target's seed is a relightable volume's payload.
- **The relight capture** (`Renderer/ProbeCapture.{h,cpp}`, frozen header, decision 3's change): `ProbeCaptureMode {Radiance, Relight}` and `Capture(snapshot, position, faceSize, mode = Radiance)`; `ProbeCubeCapture` gains `Visibility` (per face, the faces' texel order, 1 where SceneDepth is 0) and `SkyRadianceTotals` (`std::optional<std::array<glm::vec4, 10>>`); `MakeRelightCaptureSnapshot` (the Sky's `Intensity` 0, its Sun's and Moon's lights' `Color` 0, their `Intensity` kept, since `MakeSkyConstants` takes the illuminance the sky scatters from `RenderSky::SunLightColor` and the light's `Intensity`); `ProjectCubeToSkyTransfer` (the same basis, solid angles and order as `ProjectCubeToIrradianceSH9`, in double). A relight capture of a snapshot without an enabled Sky is InvalidArgument.
- **The D32 read-back** (decision 3's UNVERIFIED, resolved without the R8 fallback): `Graphics/Readback` refuses depth formats by its contract (its case "Readback: depth formats and out-of-range subresources are InvalidArgument"), so the capture copies the face's SceneDepth into a D32 staging texture of its own (a depth-aspect `vkCmdCopyImageToBuffer`, four bytes a texel), on its command list, waited for as the colour's, its columns mirrored back as the colour's. `SceneRenderer::GetSceneDepthTexture` (beside `GetSceneColorTexture`, implemented in `SceneRendererSky.cpp`). Confirmed on the device: "ProbeCapture: a relight capture reads where each face sees the sky, and the sky's totals" (each face's marker 0, the texels a mirrored or flipped face would hold 1).
- **Departure: the bake's sky.** Decision 3 takes S_bake from a sky-only capture (`ProjectCubeToRadianceSH9`). The bake takes it instead from the image-based light the relight capture's own renderer made from the snapshot's sky: `SceneRenderer::GetSkyRadianceTotals` (the view's `SkyViewResources::Totals`, before Intensity, which the dark sky of the relight snapshot leaves unchanged since the cube is drawn before Intensity), read back after the first face, then `MakeSkyRadianceSH9(totals, Sky.Intensity)` (CSSkyFinalize's normalization, in double). Reasons: (1) it is the very projection the run-time relight reads, so a probe relit at the bake's sky reproduces its bake (the bake case's 0.5 %) and a reflection probe's ratio is 1 there; (2) a sky-only capture holds the moon's disc, which no field zeroes and the image-based light leaves out, so a bake under a risen moon would count the Moon light twice; (3) the Sky pass takes the camera's altitude and the live cloud offset, the light sea level and the stepped one; (4) one capture fewer. No `ProjectCubeToRadianceSH9` exists.
- **`Renderer/ProbeRelight.{h,cpp}`** (pure, double): `GetSkyTransferIndex`, `GetShBand`, `GetIrradianceBandFactors` (`ProjectCubeToIrradianceSH9` now uses it, the same values), `RelightIrradianceSH9(local, transfer, skyRadiance)` = Local + conv(T S), `MakeSkyRadianceSH9`, `ComputeReflectionRelightScale` (the Rec. 709 luminance ratio of the relit to the baked constant term, clamped to [0, `MaxReflectionRelightScale` 4], 1 below a baked luminance of 1e-6).
- **The cache** (`GpuResourceCache`, place 3): `GpuIrradianceVolume::Relight`, `SkyRelightProbeVectors` (21) float4 a probe (Local's nine, then Transfer's 45 values four by four, the last three 0), and `ConstantTerms`, the probes' baked SH9[0], for the reflection scale; null and empty without a block.
- **`CSProbeRelight`** (`Passes/Sky.slang`, the Sky program's sixth compute; `SkyLightingPass::PipelineCount` 10, 88 pipelines): set 0 b1 `SkyConstants`, t3 the totals, t4 a volume's relight block, t5 the reflection entries, u2 `SkyResults`, u3 the view's `IrradianceProbes`, and the `Dispatch` push constants; 64 threads a group. `SkyRelightModeVolume`: probe i of `Count` gets Local + conv(T S) (S the totals normalized, times `Intensity`) at element `Face` + 9 i (`Face`, the volume's first coefficient). `SkyRelightModeReflections`: reflection probe i of `Count` gets its Intensity times the ratio into `SkyResults` 10 + i. `SkyDispatchConstants`' `Face`, `Count` and `Mode` say so (comments); `Shared/SkyConstants.h` adds `SkyRelightProbeVectors`, the two modes and `SkyRelightGroupSize`. `SkyViewResources::ReflectionRelights`: eight float4, CPU-written, each the element of the relit constant term (an exact float, -1 outside every relit volume), the baked term's luminance and the probe's Intensity.
- **The view** (`Private/ProbeShading`, `SceneRendererSky.cpp`): `PrepareProbeShading(..., relight)` takes `frame.Sky`; with it each kept volume brings its relight block (`ProbeShadingInputs::Relights`) and each reflection probe its entry (the smallest relightable volume whose box holds its centre, its probe nearest the centre in grid coordinates, rounded and clamped). The view's `IrradianceProbes` copy is UAV-capable. Row `ProbeRelight` (`RecordProbeRelight`, after `SkyIbl`): each relightable volume relit when the sky's light was regenerated (`SkyUpdates` 1), its `Intensity` changed (`ProbeViewResources::RelitIntensity`) or the copy holds the bakes (`HoldsRelight` false: copied again, after a reload too, or never relit); then, every frame the view has a reflection probe, the scales and `RecordSkyResultCopies` into `ProbeConstants`; recorded only when it runs. A copy that holds a relight no sky asks for (the Sky gone) takes its bakes again. `RenderStats::RelitIrradianceVolumes`: the volumes the render relit.
- **The bake** (`LightingBake.{h,cpp}`): under an enabled Sky each volume's probe is a relight capture of `MakeRelightCaptureSnapshot`'s snapshot (made once per bake): `Local` its `ProjectCubeToIrradianceSH9`, `Transfer` its `ProjectCubeToSkyTransfer`, `SH9` = `RelightIrradianceSH9` at the bake's sky; reflection probes bake as before. `LightingBakeEntry::Relightable`; `lighting.bake`'s `LightingBakeProbe` appends `relightable` (M26's frozen wire format, decision 3's reviewed change; described; `test_lighting.py` checks it false without a Sky, W45.4's `test_sky.py` true under one).
- **`PROBE_NOT_RELIGHTABLE`** (`Scene/SkyValidation.h`, the third of `SkyDiagnosticCodes`; both tables after `SKY_ENVIRONMENT_IGNORED`): `FindProbeSceneIssues` (`Scene/ProbeValidation.cpp`) reports, in a scene with an effectively enabled Sky, a volume whose data loads without a block and a reflection probe whose data loads and whose centre (`TransformSystem::ComputeWorldMatrix`) lies in no relightable volume's box (inclusive), each on `Data` with the bake as the hint; a probe that is not baked is `PROBE_NOT_BAKED` alone. The fixture (`MakeValidationFixtures.py`: a Sky and a volume whose data is a version 1 `.irradiance`), `validation-codes.md`'s row and §13.7's sentence.
- **The golden** (`GoldenSky.jsonl`, after `SkyDay.scene`; two materials, `Plaster` and `Chrome`): `SkyProbes.scene`, a room of closed boxes (floor, ceiling, walls) with a window in its west wall and a cave mouth in its back wall, opening onto sand, a warm point lamp, a chrome ball, an `IrradianceVolume` (4 × 3 × 4 over the room) and a box-projected `ReflectionProbe`, under a `Sky` whose Sun stands 60° up in the west and whose Moon 40° up in the east, with automatic exposure; `lighting.bake` bakes it once into `Assets/Lighting/Scenes/GoldenSky/SkyProbes/`. `Golden: SkyProbes` (`SkyGoldenTests.cpp`): two 320 × 180 tiles, noon (the sun through the window) and midnight (the sun 40° down), the lights turned and driven in the snapshot as `SkyDay`'s, rendered twice to the same bytes. **Corrected by the W45.3 review's finding 1** (see "After the lead's review" below): the shell as first built (0.2 m single-sided slabs, the volume's faces in their middle) put 44 of the 48 probes inside the slabs, where a probe sees through the slab's culled back faces, closed box or plane (this record first said closed boxes avoided it: they do not).
- **Tests:** `IrradianceVolumeDataTests.cpp` ("IrradianceVolumeData: a relightable payload round-trips, a version 1 source still reads, and a volume without a relight block writes version 1"; the existing cases' layout lines, the version refused now 3 and 0, the block's refusals, the fuzz seed); `IrradianceVolumeImporterTests.cpp` (version 3 refused; the case's name says versions 1 and 2); `ProbeCaptureTests.cpp` ("ProbeCapture: an open probe's sky transfer is the identity and a closed one's is zero"; "ProbeCapture: a relight capture reads where each face sees the sky, and the sky's totals", gpu); `ProbeRelightTests.cpp` ("ProbeRelight: the transfer's upper triangle, the sky's totals and the reflection scale follow their rules", "ProbeRelight: an open probe relights to the sky's irradiance and a closed one to its local light", and gpu "ProbeRelight: the GPU relight matches its CPU reference", "ProbeRelight: a cave darkens at night and lights at noon with no bake between", "ProbeRelight: a reflection probe inside a relit volume dims with it"); `LightingBakeTests.cpp` ("LightingBake: a volume baked under a Sky is relightable, bakes byte-identically twice, and relights to its bake at the bake's sky", gpu); `ProbeValidationTests.cpp` ("ProbeValidation: a volume without a relight block under a Sky is reported"); the count and order lines (`ProjectValidatorTests.cpp`, 88 pipelines); `Golden: SkyProbes`.
- **Documents:** Architecture §6.8 (format 2) and the importer's row, §13.7, §15.4 and §15.5; the header comments of `IrradianceVolumeData.h`, `ProbeCapture.h`, `SkyLighting.h`, `SceneRenderer.h` (the pipeline count, the two accessors), `RenderPassDeclaration.h`, `Private/ProbeShading.h`, `GpuResourceCache.h`, `LightingBake.h` and `LightingMethods.h`; game-debugging's `validation-codes.md`; the generated `Validation.md`, `Automation.md`, `Methods.json` and `catalog.json`. W45.4 writes §8.6's relit probes and the rest of decision 3's list.
- **`Golden: SkyDay` changed** (W45.2's own golden, not one of main's 56): the review's nit 12 hides the stars behind the moon's disc, which the dawn and dusk tiles still showed through it: 6 pixels at the moon, at most 9/255; `SkyOvercast` is byte-identical.
- **Runs** (2026-11-24, logs in `bin/Reviews/P8/scratch/M45-builder/w45.3/`):
  - Debug and Release builds: 0 warnings.
  - unit, Debug, filtered (`IrradianceVolumeData*`, `IrradianceVolumeImporter*`, `ProbeCapture:` CPU cases, `ProbeRelight:` CPU cases, `ProbeValidation*`, `ProjectValidator*`, `Validation: *`, `RenderSceneValidation*`, `RenderPassDeclarations*`, `Shaders*`, `SkyLighting: without a Sun*`, the `LightingBake:` refusals, `LightingMethods*`, `SkyAtmosphere*`, `Fog*`): 114 of 115, the hemisphere's T01 expected at half its value (a test bug: the y over a hemisphere integrates to pi), fixed; then in the unfiltered run.
  - unit, Debug, unfiltered (`--parallel 4 --timeout 14400`, the W45.2 review's finding 6): 5,397 passed, the 48 child-process targets skipped (`unit-full.log`).
  - gpu, Debug, filtered (`SkyLighting*`, `SkyPass*`, `ProbeRelight*`, `ProbeCapture*`, `LightingBake*`, `Pipelines: count*`, the probe shading and cache cases, `SceneRenderer: with a Sky*`, `Fog: with a Sky*`), Vulkan 1.4 and 1.3: 39 of 40 each; the cave case expected no `ProbeRelight` row for a still sky, while its black reflection probe's scale records the row every frame (a test bug), fixed to the row's dispatches (2, then 1).
  - The review fixes' proof (`revert-proof.py` on a Release build with the six behaviours reverted): unit, the moon's and `RENDER_NO_LIGHTING`'s cases failed; gpu at Vulkan 1.4, the reload, key, follow-the-sun and Sky pass cases failed (`unit-proof.log`, `gpu-proof.log`); then restored.
  - `Scaffold.py` (Release, `SkyProbes.scene` baked relightable, version 2) and `Scaffold.py --check`: passed (the relight bake reproduces byte for byte).
  - golden, Release, `--update-golden --filter "Golden: Sky*"`: `SkyProbes` written and reviewed by eye (noon: the sun's patch under the window, the sky and the sand through the window and the mouth; midnight: both black, the room lit by its lamp), `SkyDay` as above.
  - gpu and golden, Release, after the fixes (`final-1.log`): the changed gpu cases (`SkyLighting*`, `SkyPass*`, `ProbeRelight*`, the relight capture, the Sky bake, the pipeline count, three probe shading cases) 20 of 20 at Vulkan 1.4 and 1.3; 21 goldens matched: the three sky goldens and the 18 existing ones a sky change can reach (W45.2's list), each comparison line exactly as at main's phase-7 gate.
  - automation, Release: `test_two_bakes_write_byte_identical_probe_files` (`relightable` false without a Sky) passed.
  - `Lint.py`: 5 Python line-length findings, fixed, then its Python step passed; `Format.py --check`, `GenerateDocs.py --config Debug` then `--check`, `MakeValidationFixtures.py --check`: passed.

### After the lead's review

The lead's review of W45.3 (`bin/Reviews/P8/M45-W3-review.md`: 0 blocker, 0 major, 4 minor, 2 nit; the three departures accepted) is fixed with W45.4's checkpoint, before FeatureTest's regeneration:
- **1, `SkyProbes` saw the sky through its shell** (the lead's option A; the review's inset, `Size` [5.6, 2.6, 5.6], would have left the room's walls, floor and ceiling outside the volume, lit by the open sky through the Environment's fallback, since `Common/Probes.slang` weighs a point outside a box 0):
  - **The scene:** `SkyProbes.scene`'s shell is 0.6 m thick and `DoubleSided` (`Plaster.material`), and the volume (`Size` [6.6, 3.6, 6.6] about (0, 1.5, 0), still 4 × 3 × 4) runs its faces through the shell's middle, so its 44 outer probes sit 0.3 m inside the slabs and see their inside. The window is widened to 0.8 to 2.6 m, so the noon sun reaches the floor through the thicker wall, and a plaster pedestal near the room's middle takes the interior probes' light.
  - **The bake** (parsed with `w45.4/parse_irradiance.py`, `logs/parse-skyprobes-2.log`): the 44 shell probes' sky transfer T00 is 0.00000, while their local terms reach 0.90 (red; corrected by the W45.4 review's finding 4: the lamp casts no shadow, so it lights the back side of each slab's outer face, whose flipped normal faces the room, and the shell is dark to the sky only), the 4 interior probes' 0.0066, 0.0087, 0.0136 and 0.0157 (the sky through the window and the mouth); before, the shell probes' T00 reached 0.71.
  - **The bands.** The first candidate showed faint horizontal bands (3 to 5 of 255) on the noon tile's dark back wall and the window's jamb. The lead's diagnostic (`w45.4/diag.py`, a temporary golden-suite case never committed, its variants under manual exposure) traced them:
    - They go when the sun's colour or its shadows go, and stay without the reflection probe, with its box shrunk to [5.9, 2.9, 5.9], without the moon, and with the Sky disabled, so they predate M45.
    - **The cause:** the flat normal texture's 128/255 decodes to 0.0039, which tilts every map-less surface's normal by about 0.3 degrees. The sun at azimuth -90 lay exactly in the plane of the back wall and the jamb, which so took a little of its light, banded by the cascade shadow's acne at that grazing angle.
    - **The same diagnostic** showed the noon moon, above the horizon and unshadowed (only the cascade owner casts, Architecture §8.7), lighting the west wall through the shell.
    - **The scene and the tiles now keep both lights off the walls' planes:** the noon sun at azimuth -75 with the moon below the horizon (-20 degrees), and the midnight moon at azimuth 75; the scene's own Sun and Moon, which the bake takes, are [-60, -75, 0] and [20, 75, 0]. Two Backlog §6 rows (the flat normal's bias, with its engine-side fix to decide; the unshadowed second directional light) and the look skill's "Light angles" record it.
  - **`Golden: SkyProbes`** updated (`golden-update-2.log`) and reviewed by eye: at noon the room is lit through the window (the sun's patch on the floor, the bright openings) and by its lamp, its walls dark, with no bluish sky through the shell; at midnight the lamp alone, both openings dark. The noon tile's back wall is smooth (its row-banding measure 45.9 against the midnight tile's 45.4, the image's own noise; 101 before).
  - **The cost:** the walls' darkening is the known cost of probes in walls (they interpolate mostly from the dark shell layer).
  - **Records:** the look skill's recipe and a Backlog §6 row (M26's grid under a Sky; the engine-side fix to decide); this record's sentence on closed boxes is corrected.
- **2, the relight rules' cases** (`ProbeRelightTests.cpp`, gpu):
  - "ProbeRelight: a relit volume follows the Sky's Intensity and takes its bake again without the Sky": half the `Intensity` gives `SkyUpdates` 0, `RelitIrradianceVolumes` 1 and the quad's light from the sky halved; with the Sky disabled, the copy takes the bake again, equal to a Sky-less render of the volume baked without a block.
  - "ProbeRelight: a reflection probe inside a relit volume dims with it" now bakes distinct probes (only probe 3, at (1, 1, 0), sees the sky). It checks a centre nearest that probe, one nearest a closed probe (no dimming), and the smaller of two nested relightable volumes.
  - **Tolerances:** both cases compare with relative tolerances (`scale(0)`), since doctest's default scale of 1 accepts any difference below the epsilon at these magnitudes (the first proof found it).
- **3, a reflection's relight no longer depends on the camera.** `PrepareProbeShading` collects the reflection probes first. In a view a Sky lights, it keeps a relightable volume the view culls when the volume's box holds a kept reflection probe's centre: after the view's own volumes, and only while a slot is free (not counted as dropped). No surface the camera sees lies in its box. `ProbeShading.h` and Architecture §8.6 say so. Case: "ProbeRelight: a reflection probe keeps its relit scale while the volume around its centre is out of view".
- **4, the ledger:** amended on main by the lead (`70c4b7e8`); decision 8 cites it.
- **5:** the golden-images skill names `SkyProbes`.
- **6:** `LightingBakeTests.cpp`'s comment is back above its own case.
- **The proofs.** Each check ran on a Release build of Tests with its rule reverted (`w45.4/proof.py`), at Vulkan 1.4, then the rules were restored:
  - `gpu-proof-1.log`, without the `RelitIntensity` clause and the off-view volume: the Intensity case failed on its stats, and the off-view case failed (night over noon 1 against 0.411, nothing relit).
  - `gpu-proof-2.log`, without `restoreBakes` and with probe 0 for every reflection: the Intensity case failed (0.0415 without the Sky against the bake's 0.0048), and the reflection case failed (night over noon 1 against 0.411).
  - `gpu-proof-3.log`, the first set again after the tolerance fix: the Intensity case also failed on its image (0.0782 at half `Intensity`, noon's value).

## The W45.4 record

W45.4 adds the stats, FeatureTest's coverage in every mode, `test_sky.py`, Perf.py's two metrics and the documents; its checkpoint also carries the W45.3 review's fixes (the W45.3 record's "After the lead's review").

- **Stats** (decision 3's "Stats"):
  - **`stats.get`:** the view summary appends `skyUpdates` and `relitIrradianceVolumes` after M42's fields (after M43's at the merge; `StatsViewSummary`, `MakeStatsViewSummary`), both described.
  - **The Stats panel:** a "Sky" row ("n updates / n relit volumes") after the planar reflections' (after M43's row at the merge).
  - **The readout:** `PerformanceStats::SkyUpdates` after M42's members (after M43's five at the merge): `ScriptPerformanceStats.h`, `AddRenderStatistics`, `Application.GetStats`' field, its alias and its description.
  - **Comments:** `RenderStats.h`'s and `SkyLighting.h`'s comments now give the key as the W45.2 review's finding 2 left it.
  - **Tests:** "StatsGet: renderer summaries preserve exact frame identifiers and hide unavailable samples" and "SessionStatistics: AddRenderStatistics sums the view's passes and copies its light and skin counters" carry the fields, and "PlaySession: Application.GetStats reads the host's frame statistics and the session's counters outside the fixed step only" reads `SkyUpdates` through Luau.
- **FeatureTest** (IdSeeds 4201 and 4202, in `Scaffolds.json` before `GoldenSky.jsonl`'s 4203; the suite in `Suites.jsonl`'s `Testing.Suites` after `PlanarReflections`'; the replay through the `Assets/Tests/Replays/*.replay` glob):
  - **`Sky.jsonl`** makes `Assets/Scenes/Sky.scene`:
    - a `Sky` with every field leaf off its default: `Sun` `/Sun`, `Moon` `/Moon`, `Intensity` 1.25, `DriveLights` false, `Haze` 1.5, `Overcast` 0.2, `GroundAlbedo` (0.25, 0.3, 0.2), `SunDisc` 2, `MoonSize` 1.5, `Stars` 3, `Latitude` 60, `NorthAngle` 30, `Clouds` 0.4, `CloudSpeed` (3, -2);
    - its Sun (a shadow-casting `DirectionalLight`, intensity 4, `LightAngle` 0.53) and its Moon (a dim bluish one);
    - no Environment (no finding), a camera that clears to the sky, automatic exposure, a ground and a hut;
    - `Assets/Scripts/Sky/SunTurner.luau` on the Sun.

    It also writes the `Sky` suite (`Assets/Tests/Sky/Sky.test.luau`), which reaches the Sky through the `entity.Sky` shortcut (gate 2's `api:Entity.Sky/read`). Its three cases:
    - read every field back;
    - write every field through the proxy (gate 4), with `Haze`, `Latitude` and `CloudSpeed` refused out of range;
    - turn the sun from a task, 1.5 degrees a tick for 24 ticks with the clouds stilled, checking the readout's `SkyUpdates`: 1 while it turns where `RenderAvailable` holds (the exported Release and Dist runs), and 0 once it stands still, in every mode.
  - **`SkyReplay.jsonl`** records `Assets/Tests/Replays/Sky.replay` on `Sky.scene`: 90 ticks, `F` (the `Fire` action) held from tick 10 to 70, with three expectations on the `SunTurner`'s `Turned` (0 at tick 5, 25 to 31 degrees at tick 40, 58 to 61 at tick 90), so it fails without its input.
    - **Choice:** the `SunTurner` turns the Sun a degree a tick only while `Fire` is held (decision 3's "a held action quickens the sun's turn"). Without input the sun stands still, which the suite's still check needs.
    - **No recording suite** is bound: none is needed, and the ledger's amended Suites row (`d49d221f`) stays unused.
  - **Gates:**
    - in each mode: 1 (`Sky` with every field leaf non-default, in a simulated scene), 2 (`api:Entity.Sky/read`) and 4 (every field read and written through the proxy);
    - 6: W45.1's and W45.3's three fixtures;
    - 7: the `stats.get` fields and the readout member, described;
    - 1b, 3, 5, 8 and 9: nothing new.
- **`Tests/Automation/test_sky.py`:**
  - "the Sky changes through entity updates and Scene-view screenshots follow the sun" (a rendering editor):
    - the Sky's values read back by `entity.get`;
    - `Haze`, `CloudSpeed` and a second Sky refused;
    - screenshots that differ between dawn and noon and with the Sky's own fields, and that come back byte for byte to the Sky-less image once the Sky is removed.
  - "a relightable bake and the sky updates while a script turns the sun" (a rendering editor with `--headless-game-view`):
    - `lighting.bake`'s `relightable` true under the Sky, and no `PROBE_NOT_RELIGHTABLE` after it;
    - then the game view's `skyUpdates` and `relitIrradianceVolumes`: 0 while the sky stands still; 1 in a playing frame while an `OnUpdate` script turns the sun every frame, with the `ProbeRelight` row; and 0 again after `play.stop`;
    - a bounded wait for each state, since the game view renders at the editor's frames.
  - "validate reports an Environment beside a Sky" (`--renderer none`): `SKY_ENVIRONMENT_IGNORED`, a Warning, gone without the Sky.
- **Perf** (`Scripts/Perf.py`'s `look` part, `sky_cost`; `Scripts/Lib/perf.py`'s `BUDGETS` +2; `test_perf.py`'s two cases):
  - **The scene:** a generated `Assets/Perf/Sky.scene`, a cove of 8 props on a 40 m ground under a `Sky` naming a shadowed Sun 40 degrees up and a Moon light, with `Clouds` 0.35, automatic exposure and the camera showing the horizon, and a 16 × 4 × 16 m `IrradianceVolume` of 8 × 4 × 8 probes baked by `lighting.bake` at the part's start.
  - **`sky/Static/gpuMs`** sums the `SkyLuts`, `SkyIbl` and `Sky` rows over the look part's 16 single rendered ticks, captured only when the last frame did not regenerate the light.
  - **`sky/Turning/gpuMs`** sums those and `ProbeRelight`, with `Assets/Perf/SkySunTurner.luau` turning the Sun a quarter degree a tick, captured only when the last frame regenerated the light and relit the volume.
  - **A bake that is not relightable** captures neither.
  - **The numbers:** `Perf.py --config Release --only look` on `nvidia-62x` (`logs/perf-2.log`, `logs/Perf.json`; 16 samples each, warning-only): `sky/Static/gpuMs` median 0.075 ms (p95 0.099) against 0.3, and `sky/Turning/gpuMs` median 0.362 ms (p95 0.549) against 0.6, both within budget, so decision 4.8's fallbacks (fewer samples, a 128² cube, spreading over frames) are not needed.
- **Documents:**
  - **Architecture:**
    - §8.2: `RenderSky` and the driving;
    - §8.3: the key, the `ProbeRelight` row, the stats and the reload;
    - §8.4: `CSProbeRelight`'s bindings;
    - §8.6: the third copy, and "Relit probes": the bake, the departure on the bake's sky, the relight, the reflection scale, the off-view volume, all inside ADR 0028 decision 2's cut;
    - §8.13;
    - §8.14: the relight block in place 3, and the view's copy;
    - §13.7: `skyUpdates` and `relitIrradianceVolumes`;
    - §15.5: `Sky.scene` and `Sky.replay`.
  - **The Roadmap:** the `test_sky.py` acceptance.
  - **`Docs/Backlog.md`:**
    - §2: seven rows (decision 1's "Out");
    - §6: M45's group (decision 1's "Accepted at landing", the W45.2 review's two included, and M26's grid under a Sky) and the two rows the `SkyProbes` diagnostic found (the flat normal's bias; the unshadowed second directional light);
    - the probe row, amended for relit volumes.
  - **The skills:**
    - game-building's `look`: the day recipe, where the probes sit, light angles;
    - its `SKILL.md` index;
    - game-debugging: the stats, the codes and the fields' symptoms;
    - golden-images: `SkyProbes`;
    - luau-gameplay's readout.
  - **The generated references:** `Methods.json`, `ScriptAPI.md` and `Engine.d.luau`; `Automation.md` and `catalog.json` hold no `stats.get` field and stay as they were.
- **Runs** (2026-11-25, logs in `bin/Reviews/P8/scratch/M45-builder/w45.4/logs/`):
  - **Builds:** Debug and Release, 0 warnings (`build-debug-2.log`, `build-debug-3.log`, `build-release-6.log`).
  - **unit, Debug, filtered** (`StatsGet:*`, `SessionStatistics:*`, the two readout cases, `DocExamples*`): 20 passed (`unit-1.log`).
  - **unit, Debug, unfiltered** (`--parallel 4`, after the shared renderer change): 5,397 passed, with the 48 child-process targets skipped (`unit-full.log`).
  - **The proofs** (Release builds of Tests with the rules reverted): `gpu-proof-1.log`, `gpu-proof-2.log` and `gpu-proof-3.log`, as the W45.3 record's "After the lead's review" gives them.
  - **The `SkyProbes` diagnostic:** `golden-diag.log`, `golden-diag-2.log` and `diag/`, from a temporary case, removed before the builds that followed.
  - **gpu and golden, Release** (`final-2.log`):
    - the changed and nearby gpu cases (`ProbeRelight*`, the probe shading cases, the Sky bake, `SkyLighting*`, `SkyPass*`): 20 of 20 at Vulkan 1.4 and 1.3;
    - 7 goldens matched: the three sky goldens (`SkyProbes`, the reviewed candidate of `golden-update-2.log`), and `Probes`, `PlanarReflections`, `Ssr` and `IblOnly`, each comparison as at main's phase-7 gate (largest differences 0/255, `IblOnly` 1/255).
  - **`Scaffold.py` and `--check`:** passed (`scaffold-2.log`, `scaffold-check-2.log`).
  - **automation, Release** (`test_sky`, `test_lighting`, `test_perf`, `--require-gpu`): 18 passed (`automation-2.log`).
  - **`Perf.py --config Release --only look`:** passed, with the numbers above (`perf-2.log`).
  - **feature, Debug:** passed, the six editor-mode gates evaluated and green, 468 of 469 rows passed and one quit as expected, the `Sky` suite's three cases and `Sky.replay` among them, and the project unchanged (`feature-2.log`, `Feature-Debug.json`). The exported Release and Dist runs, where the suite's `SkyUpdates` 1 is checked, are the gate's export suite.
  - **Static checks:** `Lint.py` (0 findings, `lint-1.log`), `Format.py --check` (`format-2.log`), `GenerateDocs.py --config Debug` then `--check` (`gendocs-1.log`, `gendocs-check-1.log`) and `MakeValidationFixtures.py --check` (`fixtures-check-1.log`) all passed.

### After the lead's review

The lead's review of W45.4 (`bin/Reviews/P8/M45-W4-review.md`: 0 blocker, 0 major, 1 minor, 3 nit; the departures accepted, the open item traced to pass in every run the gate makes) is fixed in one more checkpoint:
- **1:** the Stats panel's "Sky" row has its case, "StatsPanel: a displayed view shows its sky updates and relit volumes" (a host view with `SkyUpdates` 1 and `RelitIrradianceVolumes` 2 draws "1 updates / 2 relit volumes").
- **2:** "ProbeRelight: a cave darkens at night and lights at noon with no bake between" compares the relit night with the lamp's bake at a relative tolerance (`scale(0)`). Proof (`gpu-proof.log`, a Release build whose `CSProbeRelight` added 1.5 times each probe's local light): the check failed, 0.0048 against a night of 0.0072, a difference the default scale of 1 would accept; every other check of the case passed; then restored.
- **3:** Architecture §8.6 and Backlog §6's reflection-probe row state the condition: a culled relightable volume is kept for a reflection probe only while one of the view's 8 volume slots is free, an accepted limit at the cap.
- **4:** this record's figure for the shell probes' local light is corrected (up to 0.90: the lamp casts no shadow, so the shell is dark to the sky only), and the look skill says "dark to the sky".
- **Runs** (2026-11-25, logs in `bin/Reviews/P8/scratch/M45-builder/w45.5/logs/`): Debug and Release builds, 0 warnings; unit, Debug, `StatsPanel:*` and `DocExamples*`, 12 passed (`unit.log`); gpu, Release, the cave case at Vulkan 1.4 and 1.3, passed (`gpu.log`); `Format.py --check`, `GenerateDocs.py --config Debug --check` and `Lint.py` on the two changed test files passed (`format.log`, `gendocs-check.log`, `lint.log`).

## Before milestone completion

- [x] W45.1 committed, the copy's case passing (or the fallback recorded); [x] the lead's review pass taken and every finding fixed.
- [x] W45.2 committed; [x] the lead's review pass taken and every finding fixed.
- [x] W45.3 committed; [x] the lead's review pass taken and every finding fixed.
- [x] W45.4 committed; [x] the lead's review pass taken and every finding fixed.
- [ ] The landing: phase 8's batch in train order, third after M43 and M44, M45's hunks adapted in M43's files, M47's `Water` after `Sky`, FeatureTest regenerated, the gate `CI.py --require-clang-cl` green.
- [ ] Architecture, the Roadmap, the skills, `Docs/Backlog.md` §2 and §6, and the generated references describe what landed.
- [ ] S03's "How the game will check it" met in the goldens, the gpu cases, FeatureTest and Perf.py, and reported to the Skerries session through the product owner, with the corrections above.
