#pragma once

#include "Engine/Renderer/RenderStage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The scene renderer's pass declarations (Architecture §8.3, §8.12; M18, C18.1, R-PASS; Docs/Decisions/0029-m18-contract.md
// decision 2): one entry per pass of the pass list, in stats order. They are the single source of
//   - the startup pipeline count: the sum of every PipelineCount (SceneRendererPipelines::StartupPipelineCount, which
//     SceneRendererPipelines::CreatePipelines creates and "Pipelines: Create makes exactly the declared startup pipelines"
//     checks); the forward variants of a non-Lit material debug view stay lazily created (EnsureDebugView) and outside it;
//   - the stats order (§8.3, stats.get): RenderStatsOrder, every declaration's StatsRows in table order, so a pass's stats
//     position is its declaration's position, whatever order it is recorded in;
//   - the programs of Resources/Shaders/Shaders.json each pass's pipelines use, and the named targets created for it (the
//     per-view targets of SceneRendererTargets and SceneRendererOutputTargets that the pass produces, from the renderer's
//     RenderTargetPool; C18.3's placement rule). Targets is not the pass's whole write set: a pass that draws over another
//     pass's target (Text, the QuadBatcher's users, the selection composite, the wireframe, the overlays and the labels into
//     the LDR or output colour target) lists none.
// The table is in RenderStage order. A later milestone appends a pass by appending its declaration where its stats
// position falls, a frozen-header change reviewed by the declaration owner. Phase 2's reserved passes and their stages:
// Skinning, the first GPU work of a frame, alone in the Deform stage (before the shadow casters and the prepass read the
// deformed vertices); PointShadows in Shadows, after SpotShadows; LightClusters at the end of Ambient, after the depth
// pyramid it can bound clusters with and before the forward passes read the clusters; Sprites and Lights2D in the Sprites
// stage (Sprites, then Lights2D: M21's C21.2 records the accumulated light before the lit sprites that sample it); Ui after
// Text in SceneOverlay (declared by M22's contract, C22.1). Phase 3's, all M26's, take the places ADR 0028's phase-3 ledger
// fixes, adding no stage: TerrainDepthNormal after DepthNormal; TerrainForward after ForwardOpaque, then Ssr after Skybox,
// before ForwardTransparent; Particles after Lights2D in Sprites; Taa first in HdrPost, before Bloom, and Exposure after
// Bloom; the grading LUT is applied inside Tonemap, with no declaration of its own; the prepass's velocity and roughness
// targets join DepthNormal's. M26's contract (C26.1, Docs/Decisions/0037-m26-contract.md decision 13) declares the six at
// those places with their stats rows and targets and without pipelines, so nothing records them and no stats.get output
// changes; each stream sets its declaration's PipelineCount and Programs with its pass (the startup count follows). A
// declaration with no stats row creates pipelines its users record under their own
// rows (the QuadBatcher), or is a startup-only computation (the DFG LUT); one with no pipeline records with another
// declaration's pipelines (Wireframe and Overlays share the debug lines' pipelines, Labels the Text pipelines, Letterbox the
// Upscale pipelines, Ui the QuadBatcher's and the Text pipelines). Phase 8's (ADR 0060's ledger) add no stage: M45's
// SkyLighting at the end of Ambient after LightClusters (the sky's LUTs, its run-time image-based light and the probes' relight,
// rows SkyLuts, SkyIbl and ProbeRelight) and its Sky directly after Skybox in Forward (the procedural sky in the skybox's place,
// row Sky; Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3), declared by its W45.1 with their rows and target and
// without pipelines, as M26's contract declared its passes; W45.2 set their PipelineCount (9 and 1) and Programs with the
// passes (87 pipelines), and W45.3 the relight's (10, 88 pipelines). M47's contract (C47, Docs/Decisions/0065-m47-water.md
// decision 2) declares Water (after Sky, before Ssr) and Underwater (first in HdrPost, before Taa) M26's way, with their rows
// and without pipelines until W47.2's render stream (Water's two) and W47.3 (Underwater's one) set them, both of the program
// Water (91 pipelines).

namespace Engine {

	struct RenderPassDeclaration
	{
		std::string_view Name{};
		RenderStage Stage = RenderStage::Prepare;
		// The pipelines SceneRendererPipelines::CreatePipelines makes for the pass at startup.
		uint32_t PipelineCount = 0;
		// The Shaders.json programs of those pipelines.
		std::span<const std::string_view> Programs{};
		// The RenderRecordingContext rows the pass records (BeginPass names), in stats order.
		std::span<const std::string_view> StatsRows{};
		// The named targets created for the pass, which it produces (see the file comment); not every target it draws into.
		std::span<const std::string_view> Targets{};
	};

	namespace Detail {

		inline constexpr std::array<std::string_view, 1> PrepareRows = { "Prepare" };
		inline constexpr std::array<std::string_view, 1> SkinningPrograms = { "Skinning" };
		inline constexpr std::array<std::string_view, 1> SkinningRows = { "Skinning" };
		inline constexpr std::array<std::string_view, 1> ShadowPrograms = { "Shadow" };
		inline constexpr std::array<std::string_view, 2> ShadowRows = { "DirectionalShadows", "SpotShadows" };
		inline constexpr std::array<std::string_view, 2> ShadowTargets = { "ShadowCascades", "ShadowAtlas" };
		inline constexpr std::array<std::string_view, 1> PointShadowsRows = { "PointShadows" };
		inline constexpr std::array<std::string_view, 2> PointShadowsTargets = { "PointShadowMaps", "PointShadowMapsHalf" };
		inline constexpr std::array<std::string_view, 1> ScenePrograms = { "Scene" };
		inline constexpr std::array<std::string_view, 1> DepthNormalRows = { "DepthNormal" };
		// M26 (C26.1, R-TAA-VEL, R-SSR-IN; decision 14): the prepass's motion vectors and specular inputs after EntityId
		// (SceneTargetFormats.h), created and written by stream X-C (the mesh prepass) and T-C (TerrainDepthNormal).
		inline constexpr std::array<std::string_view, 5> DepthNormalTargets = { "SceneDepth", "SceneNormals", "EntityId", "SceneVelocity",
			"SceneRoughness" };
		// M26 (C26.1, C-TERR; decision 22, stream T-C): the Terrain program (Passes/Terrain.slang, Renderer/TerrainPass.h) of
		// TerrainDepthNormal's and TerrainForward's pipelines.
		inline constexpr std::array<std::string_view, 1> TerrainPrograms = { "Terrain" };
		inline constexpr std::array<std::string_view, 1> TerrainDepthNormalRows = { "TerrainDepthNormal" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidPrograms = { "DepthPyramid" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidRows = { "DepthPyramid" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidTargets = { "ViewDepth" };
		inline constexpr std::array<std::string_view, 1> GtaoPrograms = { "Gtao" };
		inline constexpr std::array<std::string_view, 3> GtaoRows = { "GTAO", "GTAODenoiseHorizontal", "GTAODenoiseVertical" };
		inline constexpr std::array<std::string_view, 2> GtaoTargets = { "AmbientOcclusion", "AmbientOcclusionScratch" };
		inline constexpr std::array<std::string_view, 1> LightClustersPrograms = { "LightClusters" };
		inline constexpr std::array<std::string_view, 1> LightClustersRows = { "LightClusters" };
		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; ADR 0060's phase-8 ledger): the sky's lighting at the end
		// of Ambient: its LUTs, its image-based light (the update when its key changed, the finalize and copies every frame) and
		// the irradiance volumes' relight (Renderer/SkyLighting.h).
		inline constexpr std::array<std::string_view, 2> SkyLightingPrograms = { "Sky", "EnvironmentBake" };
		inline constexpr std::array<std::string_view, 3> SkyLightingRows = { "SkyLuts", "SkyIbl", "ProbeRelight" };
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger): one row per reflection slot, in rank order,
		// each timing that slot's whole mirror view; the slots' reflections and the mirror views' prepass scratch; the program of
		// the reflections' mip chain (PlanarReflectionPass.h).
		inline constexpr std::array<std::string_view, 1> PlanarReflectionsPrograms = { "PlanarReflection" };
		inline constexpr std::array<std::string_view, 4> PlanarReflectionsRows = { "PlanarReflection0", "PlanarReflection1", "PlanarReflection2",
			"PlanarReflection3" };
		inline constexpr std::array<std::string_view, 5> PlanarReflectionsTargets = { "PlanarReflectionColor", "PlanarReflectionDepth",
			"PlanarReflectionNormals", "PlanarReflectionVelocity", "PlanarReflectionRoughness" };
		inline constexpr std::array<std::string_view, 1> BrdfLutPrograms = { "BrdfLut" };
		inline constexpr std::array<std::string_view, 1> ForwardOpaqueRows = { "ForwardOpaque" };
		inline constexpr std::array<std::string_view, 1> TerrainForwardRows = { "TerrainForward" };
		inline constexpr std::array<std::string_view, 1> SceneColorTargets = { "SceneColor" };
		inline constexpr std::array<std::string_view, 1> SkyboxPrograms = { "Skybox" };
		inline constexpr std::array<std::string_view, 1> SkyboxRows = { "Skybox" };
		// M45 (decision 3): the procedural sky, drawn into SceneColor in the Skybox's place (Renderer/SkyPass.h).
		inline constexpr std::array<std::string_view, 1> SkyPrograms = { "Sky" };
		inline constexpr std::array<std::string_view, 1> SkyRows = { "Sky" };
		// M47 (Docs/Decisions/0065-m47-water.md decision 3): the Water pass's rows (the copy of SceneColor, then the surfaces)
		// and its target, and the Underwater pass's row.
		inline constexpr std::array<std::string_view, 1> WaterPrograms = { "Water" };
		inline constexpr std::array<std::string_view, 2> WaterRows = { "WaterRefraction", "Water" };
		inline constexpr std::array<std::string_view, 1> WaterTargets = { "WaterRefraction" };
		inline constexpr std::array<std::string_view, 1> UnderwaterRows = { "Underwater" };
		// M26 (C26.1, R-SSR; decision 18, stream X-D): the Ssr program (Passes/Ssr.slang, Renderer/SsrPass.h).
		inline constexpr std::array<std::string_view, 1> SsrPrograms = { "Ssr" };
		inline constexpr std::array<std::string_view, 1> SsrRows = { "SSR" };
		inline constexpr std::array<std::string_view, 1> SsrTargets = { "SsrColor" };
		inline constexpr std::array<std::string_view, 1> ForwardTransparentRows = { "ForwardTransparent" };
		inline constexpr std::array<std::string_view, 1> OverdrawRows = { "Overdraw" };
		inline constexpr std::array<std::string_view, 1> OverdrawTargets = { "Overdraw" };
		inline constexpr std::array<std::string_view, 1> QuadPrograms = { "Quad" };
		inline constexpr std::array<std::string_view, 1> SpritesRows = { "Sprites" };
		// M21 (C21.2): the Lights2D program (Passes/Lights2D.slang, Renderer/Lights2DPass.h).
		inline constexpr std::array<std::string_view, 1> Lights2DPrograms = { "Lights2D" };
		inline constexpr std::array<std::string_view, 1> Lights2DRows = { "Lights2D" };
		inline constexpr std::array<std::string_view, 2> Lights2DTargets = { "Lights2DNormals", "Lights2DLight" };
		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the Particles program (Passes/Particles.slang,
		// Renderer/ParticlePass.h).
		inline constexpr std::array<std::string_view, 1> ParticlesPrograms = { "Particles" };
		inline constexpr std::array<std::string_view, 1> ParticlesRows = { "Particles" };
		// M26 (C26.1, R-TAA; decision 17, stream X-C): the Taa program (Passes/Taa.slang, Renderer/TaaPass.h).
		inline constexpr std::array<std::string_view, 1> TaaPrograms = { "Taa" };
		inline constexpr std::array<std::string_view, 1> TaaRows = { "TAA" };
		inline constexpr std::array<std::string_view, 1> TaaTargets = { "TaaHistory" };
		inline constexpr std::array<std::string_view, 1> BloomPrograms = { "Bloom" };
		inline constexpr std::array<std::string_view, 1> BloomRows = { "Bloom" };
		inline constexpr std::array<std::string_view, 1> BloomTargets = { "Bloom" };
		// M26 (C26.1, R-AEX; decision 15, stream X-A): the Exposure program (Passes/Exposure.slang, Renderer/ExposurePass.h).
		inline constexpr std::array<std::string_view, 1> ExposurePrograms = { "Exposure" };
		inline constexpr std::array<std::string_view, 1> ExposureRows = { "Exposure" };
		inline constexpr std::array<std::string_view, 2> TonemapPrograms = { "Tonemap", "SceneDataView" };
		inline constexpr std::array<std::string_view, 1> TonemapRows = { "Tonemap" };
		inline constexpr std::array<std::string_view, 1> TonemapTargets = { "LdrColor" };
		inline constexpr std::array<std::string_view, 1> FxaaPrograms = { "Fxaa" };
		inline constexpr std::array<std::string_view, 1> FxaaRows = { "FXAA" };
		inline constexpr std::array<std::string_view, 1> FxaaTargets = { "LdrColorFxaa" };
		inline constexpr std::array<std::string_view, 1> TextPrograms = { "Text" };
		inline constexpr std::array<std::string_view, 1> TextRows = { "Text" };
		inline constexpr std::array<std::string_view, 1> UiRows = { "Ui" };
		inline constexpr std::array<std::string_view, 1> UpscalePrograms = { "Upscale" };
		inline constexpr std::array<std::string_view, 1> UpscaleRows = { "Upscale" };
		inline constexpr std::array<std::string_view, 2> UpscaleTargets = { "OutputColor", "OutputDepth" };
		inline constexpr std::array<std::string_view, 3> SelectionPrograms = { "SelectionMask", "SelectionDilate", "SelectionComposite" };
		inline constexpr std::array<std::string_view, 4> SelectionRows = { "SelectionMask", "SelectionDilateHorizontal", "SelectionDilateVertical",
			"SelectionComposite" };
		inline constexpr std::array<std::string_view, 2> SelectionTargets = { "SelectionMask", "SelectionScratch" };
		inline constexpr std::array<std::string_view, 1> WireframeRows = { "Wireframe" };
		inline constexpr std::array<std::string_view, 1> DebugLinesPrograms = { "DebugLines" };
		inline constexpr std::array<std::string_view, 1> OverlaysRows = { "Overlays" };
		inline constexpr std::array<std::string_view, 1> LabelsRows = { "Labels" };
		inline constexpr std::array<std::string_view, 1> LetterboxRows = { "Letterbox" };
		inline constexpr std::array<std::string_view, 1> LetterboxTargets = { "LetterboxTexel" };

	}

	// The pass list, in stats order (see the file comment). 76 pipelines at startup (74 with M32's, below, M33's Particles 3,
	// Docs/Decisions/0048-m33-particle-look.md decision 4, less M26's QuadBatcher HdrAdditive 1, which M33's merge removed,
	// decision 10): M17's 54 plus the QuadBatcher's 2 and
	// the output stage's 2 (M18), M21's Lights2D 1 and the QuadBatcher's HdrLit and Normals 2 (C21.2; M21's Sprites
	// records with the QuadBatcher's), the QuadBatcher's LdrDepth 1 (M22, C22.3, Docs/Decisions/0033-m22-contract.md
	// decision 57; M22's Ui adds none of its own), M23's LightClusters 1 and Skinning 1, M26's QuadBatcher HdrAdditive 1
	// (stream P-B, Docs/Decisions/0037-m26-contract.md decision 7; M26's Particles adds none of its own), M26's Exposure 2
	// (stream X-A, decision 15), M26's Taa 1 (stream X-C, decision 17), M26's Ssr 2 (stream X-D, decision 18) and M26's
	// TerrainDepthNormal 2 and TerrainForward 1 (stream T-C, decision 22). M23's C23.1 adds PointShadows and LightClusters
	// at the places phase 2's ledger fixes (ADR 0028), and C23.3 Skinning, alone in Deform. M26's C26.1 adds
	// TerrainDepthNormal, TerrainForward, Ssr, Particles, Taa and Exposure at the places phase 3's ledger fixes, without
	// pipelines until their streams (37 stats rows). Their planned pipelines (Docs/Decisions/0037-m26-contract.md decision
	// 13): TerrainDepthNormal 2 (the terrain prepass and picking prepass, program Terrain), TerrainForward 1 (Terrain),
	// Ssr 2 (the trace and the composite, program Ssr), Taa 1 (Taa), Exposure 2 (the histogram and the adaptation, program
	// Exposure) and the QuadBatcher's HdrAdditive 1 for additive particles (Particles declares none): 73 at M26's landing.
	// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 10 and 14) adds Ssr's classification 1: 74.
	inline constexpr std::array ScenePassDeclarations = std::to_array<RenderPassDeclaration>({
		{ .Name = "Prepare", .Stage = RenderStage::Prepare, .StatsRows = Detail::PrepareRows },
		// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27): the skinned and morphed meshes' vertices, deformed by
		// SkinningPass's compute pipeline of the program Skinning into each deformed item's own vertex buffer before the
		// shadow casters, the prepass and the forward passes draw them; no target (the vertices are buffers). One pipeline.
		{ .Name = "Skinning", .Stage = RenderStage::Deform, .PipelineCount = 1, .Programs = Detail::SkinningPrograms, .StatsRows = Detail::SkinningRows },
		{ .Name = "Shadows", .Stage = RenderStage::Shadows, .PipelineCount = 6, .Programs = Detail::ShadowPrograms, .StatsRows = Detail::ShadowRows, .Targets = Detail::ShadowTargets },
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 3): the point lights' cube faces, drawn with the Shadows
		// declaration's caster pipelines (ShadowPass) into its own cube-array target. M29 (Docs/Decisions/0044-m29-light-limits.md
		// decision 2): a second target, PointShadowMapsHalf, the half-size faces of the lights beyond the four most important,
		// drawn with the same pipelines, so the startup count is unchanged.
		{ .Name = "PointShadows", .Stage = RenderStage::Shadows, .StatsRows = Detail::PointShadowsRows, .Targets = Detail::PointShadowsTargets },
		// The prepass and the picking prepass, {Opaque, Mask} x {CullBack, CullFront, CullNone} each.
		{ .Name = "DepthNormal", .Stage = RenderStage::DepthNormal, .PipelineCount = 12, .Programs = Detail::ScenePrograms, .StatsRows = Detail::DepthNormalRows, .Targets = Detail::DepthNormalTargets },
		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 13 and 22): the terrains' prepass and picking prepass
		// into DepthNormal's final targets (velocity and roughness included), so GTAO, SSR and picking see the terrain:
		// TerrainPass's two pipelines of the program Terrain (stream T-C).
		{ .Name = "TerrainDepthNormal", .Stage = RenderStage::DepthNormal, .PipelineCount = 2, .Programs = Detail::TerrainPrograms, .StatsRows = Detail::TerrainDepthNormalRows },
		{ .Name = "DepthPyramid", .Stage = RenderStage::Ambient, .PipelineCount = 2, .Programs = Detail::DepthPyramidPrograms, .StatsRows = Detail::DepthPyramidRows, .Targets = Detail::DepthPyramidTargets },
		{ .Name = "Gtao", .Stage = RenderStage::Ambient, .PipelineCount = 2, .Programs = Detail::GtaoPrograms, .StatsRows = Detail::GtaoRows, .Targets = Detail::GtaoTargets },
		// M23 (C23.1, decision 5): the cluster masks of the clustered light loop, a per-view buffer (no target), written by
		// LightClusterPass's one compute pipeline of the program LightClusters.
		{ .Name = "LightClusters", .Stage = RenderStage::Ambient, .PipelineCount = 1, .Programs = Detail::LightClustersPrograms, .StatsRows = Detail::LightClustersRows },
		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; ADR 0060's phase-8 ledger: after LightClusters): a Sky's
		// lighting for a Lit view, recorded after the light clusters and before the forward passes read it: the transmittance,
		// multiple-scattering and sky-view LUTs (SkyLuts), the run-time image-based light and the copies of its results into the
		// view's constant buffers (SkyIbl), and the relit irradiance volumes (ProbeRelight); no target (the LUTs, cubes and
		// buffers are the view's SkyViewResources). SkyLightingPass's pipelines: the Sky program's six computes (CSProbeRelight the
		// relight's) and EnvironmentBake's four (CSDownsampleCube, CSPrefilterSpecular, CSProjectIrradiance, CSReduceIrradiance;
		// the same SPIR-V as the import bake's).
		{ .Name = "SkyLighting", .Stage = RenderStage::Ambient, .PipelineCount = 10, .Programs = Detail::SkyLightingPrograms, .StatsRows = Detail::SkyLightingRows },
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger: first in the Forward stage, before
		// ForwardOpaque): the planar reflections a Lit view draws (Renderer/PlanarReflections.h), each a mirror view of the
		// scene rendered from the camera mirrored in its plane into its slot's PlanarReflectionColor, whose mips the main view's
		// forward passes sample on the mirror's glass; one row per slot (PlanarReflection0 to PlanarReflection3, by rank), each
		// timing that slot's whole mirror view, its light clusters included (LightClusterPass::RecordInRow); PlanarReflectionPass's
		// one compute pipeline of the program PlanarReflection (the colour's mip chain).
		{ .Name = "PlanarReflections", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::PlanarReflectionsPrograms, .StatsRows = Detail::PlanarReflectionsRows, .Targets = Detail::PlanarReflectionsTargets },
		// Generates the DFG LUT once at creation (BrdfLut.h); the forward passes read it.
		{ .Name = "BrdfLut", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::BrdfLutPrograms },
		// The Lit forward variants, {Opaque, Mask} x the three cull modes; a material debug view adds its own lazily.
		{ .Name = "ForwardOpaque", .Stage = RenderStage::Forward, .PipelineCount = 6, .Programs = Detail::ScenePrograms, .StatsRows = Detail::ForwardOpaqueRows, .Targets = Detail::SceneColorTargets },
		// M26 (C26.1, C-TERR; decisions 13 and 22): the terrains shaded with M23's light loop and the probes' indirect term, into
		// SceneColor, before Skybox: TerrainPass's forward pipeline of the program Terrain (stream T-C).
		{ .Name = "TerrainForward", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::TerrainPrograms, .StatsRows = Detail::TerrainForwardRows },
		{ .Name = "Skybox", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::SkyboxPrograms, .StatsRows = Detail::SkyboxRows, .Targets = Detail::SceneColorTargets },
		// M45 (decision 3; its ledger: directly after Skybox): the procedural sky (Renderer/SkyPass.h), one fullscreen triangle at
		// depth 0 into SceneColor as the Skybox's, drawn in its place when the snapshot's Sky lights the view and its camera clears
		// to the skybox, in mirror views too. SkyPass's one pipeline of the program Sky. M47's Water follows it.
		{ .Name = "Sky", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::SkyPrograms, .StatsRows = Detail::SkyRows, .Targets = Detail::SceneColorTargets },
		// M47 (Docs/Decisions/0065-m47-water.md decisions 2 and 3; phase 8's ledger: after M45's Sky, before Ssr): the snapshot's
		// waters in a Lit view with a camera outside the wireframe, after a copy of SceneColor into WaterRefraction (row
		// WaterRefraction), each surface drawn into SceneColor, SceneNormals, SceneVelocity and SceneRoughness (and EntityId with
		// picking), SceneDepth tested and written, so SSR traces over it (row Water; Renderer/WaterPass.h). Its contract (C47)
		// declared it with its rows and target and without pipelines; W47.2's render stream set its two pipelines of the program
		// Water (the surface, and the surface with EntityId).
		{ .Name = "Water", .Stage = RenderStage::Forward, .PipelineCount = 2, .Programs = Detail::WaterPrograms, .StatsRows = Detail::WaterRows, .Targets = Detail::WaterTargets },
		// M26 (C26.1, R-SSR; decisions 13 and 18): glossy reflections traced over the opaque image with the depth pyramid into
		// SsrColor, falling back to the probes, composited into SceneColor before the transparent pass draws over them;
		// recorded only with PostProcess's SsrEnabled. SsrPass's three pipelines of the program Ssr (stream X-D's trace and
		// composite, and M32's classification, which lists the tiles the trace and the composite cover).
		{ .Name = "Ssr", .Stage = RenderStage::Forward, .PipelineCount = 3, .Programs = Detail::SsrPrograms, .StatsRows = Detail::SsrRows, .Targets = Detail::SsrTargets },
		// Blend, for the three cull modes.
		{ .Name = "ForwardTransparent", .Stage = RenderStage::Forward, .PipelineCount = 3, .Programs = Detail::ScenePrograms, .StatsRows = Detail::ForwardTransparentRows, .Targets = Detail::SceneColorTargets },
		// The Overdraw data view's additive pass, for the three cull modes.
		{ .Name = "Overdraw", .Stage = RenderStage::Forward, .PipelineCount = 3, .Programs = Detail::ScenePrograms, .StatsRows = Detail::OverdrawRows, .Targets = Detail::OverdrawTargets },
		// QuadBatcher.h: its users record under their own rows (M21's Sprites and Lights2D in this stage, M22's Ui in
		// SceneOverlay); Hdr and Ldr, M21's HdrLit and Normals (C21.2, Docs/Decisions/0032-m21-contract.md decision 22) and M22's
		// LdrDepth (C22.3, Docs/Decisions/0033-m22-contract.md decision 57). M26's HdrAdditive (C26.1,
		// Docs/Decisions/0037-m26-contract.md decision 7, stream P-B), the additive particles' pipeline, was removed at M33's
		// merge (Docs/Decisions/0048-m33-particle-look.md decision 10).
		{ .Name = "QuadBatcher", .Stage = RenderStage::Sprites, .PipelineCount = 5, .Programs = Detail::QuadPrograms },
		// M21 (C21.1, Docs/Decisions/0032-m21-contract.md decision 4): the snapshot's sprites, in its draw order, with the
		// QuadBatcher's Hdr pipeline into SceneColor, depth-tested against SceneDepth (lit ones with its HdrLit pipeline, C21.2).
		{ .Name = "Sprites", .Stage = RenderStage::Sprites, .StatsRows = Detail::SpritesRows },
		// M21 (C21.2, decision 22): with a lit sprite or tilemap in a Lit view, the lit items' normals (the QuadBatcher's
		// Normals pipeline) into Lights2DNormals, then every 2D light added into Lights2DLight at the scene resolution, recorded
		// before the lit items that sample it; its row counts the normals' and the lights' draws. Its one pipeline is the
		// Lights2D program's (Renderer/Lights2DPass.h); with the QuadBatcher's HdrLit and Normals, 61 startup pipelines on M21's
		// tree (62 with M22's LdrDepth).
		{ .Name = "Lights2D", .Stage = RenderStage::Sprites, .PipelineCount = 1, .Programs = Detail::Lights2DPrograms, .StatsRows = Detail::Lights2DRows, .Targets = Detail::Lights2DTargets },
		// M26 (C26.1, C-PART; decisions 7 and 13): the snapshot's particles into SceneColor, tested against SceneDepth without
		// writing it, after the 3D forward passes and the sprites and before the HDR post chain, so TAA, bloom and exposure see
		// them (Renderer/ParticlePass.h). M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the Particles program's
		// three pipelines of ParticlePass, ParticlesAlpha and ParticlesAdditive into SceneColor and ParticlesVelocity into
		// SceneVelocity in a view with TAA, SceneDepth read as a texture; before M33 it drew with the QuadBatcher's Hdr and
		// HdrAdditive pipelines and declared none.
		{ .Name = "Particles", .Stage = RenderStage::Sprites, .PipelineCount = 3, .Programs = Detail::ParticlesPrograms, .StatsRows = Detail::ParticlesRows },
		// M47 (Docs/Decisions/0065-m47-water.md decisions 2 and 3; phase 8's ledger: first in HdrPost, before Taa): the view under
		// a water's surface fogged toward its UnderwaterColor over its UnderwaterVisibility, with the waterline where the camera
		// straddles the surface, recorded when the camera's near plane may lie under a water, before TAA so the transparents and
		// particles under the surface are fogged too. Declared by C47 with its row and without pipelines; W47.3 sets its one
		// pipeline of the program Water (WaterPass::RecordUnderwater).
		{ .Name = "Underwater", .Stage = RenderStage::HdrPost, .PipelineCount = 1, .Programs = Detail::WaterPrograms, .StatsRows = Detail::UnderwaterRows },
		// M26 (C26.1, R-TAA; decisions 10, 13 and 17): the jittered HDR image resolved against its history (TaaHistory), in
		// HdrPost before Bloom and Exposure (after M47's Underwater), so they read the resolved image; recorded only with
		// AntiAliasing Taa in a Lit view outside the wireframe and pixel-art mode. TaaPass's one compute pipeline of the program
		// Taa (stream X-C).
		{ .Name = "Taa", .Stage = RenderStage::HdrPost, .PipelineCount = 1, .Programs = Detail::TaaPrograms, .StatsRows = Detail::TaaRows, .Targets = Detail::TaaTargets },
		{ .Name = "Bloom", .Stage = RenderStage::HdrPost, .PipelineCount = 3, .Programs = Detail::BloomPrograms, .StatsRows = Detail::BloomRows, .Targets = Detail::BloomTargets },
		// M26 (C26.1, R-AEX; decisions 13 and 15): the luminance histogram of the image Tonemap exposes and the adapted exposure,
		// ExposurePass's two compute pipelines of the program Exposure (the histogram and the adaptation, stream X-A) writing the
		// renderer's exposure buffers (no target); recorded only with ExposureMode Auto in a Lit view outside the wireframe and
		// pixel-art mode.
		{ .Name = "Exposure", .Stage = RenderStage::HdrPost, .PipelineCount = 2, .Programs = Detail::ExposurePrograms, .StatsRows = Detail::ExposureRows },
		// The tonemap pass and the data views' composite, which replaces it under the same row.
		{ .Name = "Tonemap", .Stage = RenderStage::Tonemap, .PipelineCount = 2, .Programs = Detail::TonemapPrograms, .StatsRows = Detail::TonemapRows, .Targets = Detail::TonemapTargets },
		{ .Name = "Fxaa", .Stage = RenderStage::LdrPost, .PipelineCount = 1, .Programs = Detail::FxaaPrograms, .StatsRows = Detail::FxaaRows, .Targets = Detail::FxaaTargets },
		// Game Text at the scene resolution (TextRenderer's two pipelines, which Labels shares).
		{ .Name = "Text", .Stage = RenderStage::SceneOverlay, .PipelineCount = 2, .Programs = Detail::TextPrograms, .StatsRows = Detail::TextRows },
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): the UI draw list at the scene resolution, after game Text,
		// with the QuadBatcher's Ldr pipeline (C22.3: and its LdrDepth for World canvases) and the Text pipelines (UiPass.h), so
		// it declares none of its own.
		{ .Name = "Ui", .Stage = RenderStage::SceneOverlay, .StatsRows = Detail::UiRows },
		{ .Name = "Upscale", .Stage = RenderStage::Output, .PipelineCount = 2, .Programs = Detail::UpscalePrograms, .StatsRows = Detail::UpscaleRows, .Targets = Detail::UpscaleTargets },
		{ .Name = "Selection", .Stage = RenderStage::EditorOverlay, .PipelineCount = 8, .Programs = Detail::SelectionPrograms, .StatsRows = Detail::SelectionRows, .Targets = Detail::SelectionTargets },
		{ .Name = "Wireframe", .Stage = RenderStage::EditorOverlay, .StatsRows = Detail::WireframeRows },
		// The debug lines' two pipelines (DebugRenderer.h): debug draw, annotations, grid and icons.
		{ .Name = "Overlays", .Stage = RenderStage::EditorOverlay, .PipelineCount = 2, .Programs = Detail::DebugLinesPrograms, .StatsRows = Detail::OverlaysRows },
		// The debug and annotation labels, at the view's resolution with the Text pipelines.
		{ .Name = "Labels", .Stage = RenderStage::EditorOverlay, .StatsRows = Detail::LabelsRows },
		// At a scaled output with a letterbox, the letterbox bars painted again after the editor overlays, with the upscale's
		// Color pipeline (UpscalePass::RecordLetterbox), so the overlays end clipped to the scaled scene rectangle.
		{ .Name = "Letterbox", .Stage = RenderStage::EditorOverlay, .StatsRows = Detail::LetterboxRows, .Targets = Detail::LetterboxTargets },
	});

	// The sum of every declaration's PipelineCount: the pipelines SceneRendererPipelines creates at startup.
	[[nodiscard]] consteval uint32_t GetDeclaredPipelineCount()
	{
		uint32_t count = 0;
		for (const RenderPassDeclaration& declaration : ScenePassDeclarations)
			count += declaration.PipelineCount;
		return count;
	}

	// The number of stats rows the declarations name.
	[[nodiscard]] consteval size_t GetDeclaredStatsRowCount()
	{
		size_t count = 0;
		for (const RenderPassDeclaration& declaration : ScenePassDeclarations)
			count += declaration.StatsRows.size();
		return count;
	}

	namespace Detail {

		[[nodiscard]] consteval std::array<std::string_view, GetDeclaredStatsRowCount()> MakeRenderStatsOrder()
		{
			std::array<std::string_view, GetDeclaredStatsRowCount()> order{};
			size_t next = 0;
			for (const RenderPassDeclaration& declaration : ScenePassDeclarations)
			{
				for (const std::string_view row : declaration.StatsRows)
					order[next++] = row;
			}
			return order;
		}

	}

	// The stats order (§8.3): RenderStats::Passes lists the recorded rows in this order, whatever their recording order.
	inline constexpr std::array<std::string_view, GetDeclaredStatsRowCount()> RenderStatsOrder = Detail::MakeRenderStatsOrder();

}
