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
// stage; Ui after Text in SceneOverlay. Phase 3's (Particles, Exposure, Lut, Taa, Ssr and terrain) take the stages their
// contracts name. A declaration with no stats row creates pipelines its users record under their own rows (the
// QuadBatcher), or is a startup-only computation (the DFG LUT); one with no pipeline records with another declaration's
// pipelines (Wireframe and Overlays share the debug lines' pipelines, Labels the Text pipelines).

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
		inline constexpr std::array<std::string_view, 1> ShadowPrograms = { "Shadow" };
		inline constexpr std::array<std::string_view, 2> ShadowRows = { "DirectionalShadows", "SpotShadows" };
		inline constexpr std::array<std::string_view, 2> ShadowTargets = { "ShadowCascades", "ShadowAtlas" };
		inline constexpr std::array<std::string_view, 1> ScenePrograms = { "Scene" };
		inline constexpr std::array<std::string_view, 1> DepthNormalRows = { "DepthNormal" };
		inline constexpr std::array<std::string_view, 3> DepthNormalTargets = { "SceneDepth", "SceneNormals", "EntityId" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidPrograms = { "DepthPyramid" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidRows = { "DepthPyramid" };
		inline constexpr std::array<std::string_view, 1> DepthPyramidTargets = { "ViewDepth" };
		inline constexpr std::array<std::string_view, 1> GtaoPrograms = { "Gtao" };
		inline constexpr std::array<std::string_view, 3> GtaoRows = { "GTAO", "GTAODenoiseHorizontal", "GTAODenoiseVertical" };
		inline constexpr std::array<std::string_view, 2> GtaoTargets = { "AmbientOcclusion", "AmbientOcclusionScratch" };
		inline constexpr std::array<std::string_view, 1> BrdfLutPrograms = { "BrdfLut" };
		inline constexpr std::array<std::string_view, 1> ForwardOpaqueRows = { "ForwardOpaque" };
		inline constexpr std::array<std::string_view, 1> SceneColorTargets = { "SceneColor" };
		inline constexpr std::array<std::string_view, 1> SkyboxPrograms = { "Skybox" };
		inline constexpr std::array<std::string_view, 1> SkyboxRows = { "Skybox" };
		inline constexpr std::array<std::string_view, 1> ForwardTransparentRows = { "ForwardTransparent" };
		inline constexpr std::array<std::string_view, 1> OverdrawRows = { "Overdraw" };
		inline constexpr std::array<std::string_view, 1> OverdrawTargets = { "Overdraw" };
		inline constexpr std::array<std::string_view, 1> QuadPrograms = { "Quad" };
		inline constexpr std::array<std::string_view, 1> BloomPrograms = { "Bloom" };
		inline constexpr std::array<std::string_view, 1> BloomRows = { "Bloom" };
		inline constexpr std::array<std::string_view, 1> BloomTargets = { "Bloom" };
		inline constexpr std::array<std::string_view, 2> TonemapPrograms = { "Tonemap", "SceneDataView" };
		inline constexpr std::array<std::string_view, 1> TonemapRows = { "Tonemap" };
		inline constexpr std::array<std::string_view, 1> TonemapTargets = { "LdrColor" };
		inline constexpr std::array<std::string_view, 1> FxaaPrograms = { "Fxaa" };
		inline constexpr std::array<std::string_view, 1> FxaaRows = { "FXAA" };
		inline constexpr std::array<std::string_view, 1> FxaaTargets = { "LdrColorFxaa" };
		inline constexpr std::array<std::string_view, 1> TextPrograms = { "Text" };
		inline constexpr std::array<std::string_view, 1> TextRows = { "Text" };
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

	}

	// The pass list, in stats order (see the file comment). 58 pipelines at startup: M17's 54 plus the QuadBatcher's 2 and the
	// output stage's 2.
	inline constexpr std::array ScenePassDeclarations = std::to_array<RenderPassDeclaration>({
		{ .Name = "Prepare", .Stage = RenderStage::Prepare, .StatsRows = Detail::PrepareRows },
		{ .Name = "Shadows", .Stage = RenderStage::Shadows, .PipelineCount = 6, .Programs = Detail::ShadowPrograms, .StatsRows = Detail::ShadowRows, .Targets = Detail::ShadowTargets },
		// The prepass and the picking prepass, {Opaque, Mask} x {CullBack, CullFront, CullNone} each.
		{ .Name = "DepthNormal", .Stage = RenderStage::DepthNormal, .PipelineCount = 12, .Programs = Detail::ScenePrograms, .StatsRows = Detail::DepthNormalRows, .Targets = Detail::DepthNormalTargets },
		{ .Name = "DepthPyramid", .Stage = RenderStage::Ambient, .PipelineCount = 2, .Programs = Detail::DepthPyramidPrograms, .StatsRows = Detail::DepthPyramidRows, .Targets = Detail::DepthPyramidTargets },
		{ .Name = "Gtao", .Stage = RenderStage::Ambient, .PipelineCount = 2, .Programs = Detail::GtaoPrograms, .StatsRows = Detail::GtaoRows, .Targets = Detail::GtaoTargets },
		// Generates the DFG LUT once at creation (BrdfLut.h); the forward passes read it.
		{ .Name = "BrdfLut", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::BrdfLutPrograms },
		// The Lit forward variants, {Opaque, Mask} x the three cull modes; a material debug view adds its own lazily.
		{ .Name = "ForwardOpaque", .Stage = RenderStage::Forward, .PipelineCount = 6, .Programs = Detail::ScenePrograms, .StatsRows = Detail::ForwardOpaqueRows, .Targets = Detail::SceneColorTargets },
		{ .Name = "Skybox", .Stage = RenderStage::Forward, .PipelineCount = 1, .Programs = Detail::SkyboxPrograms, .StatsRows = Detail::SkyboxRows, .Targets = Detail::SceneColorTargets },
		// Blend, for the three cull modes.
		{ .Name = "ForwardTransparent", .Stage = RenderStage::Forward, .PipelineCount = 3, .Programs = Detail::ScenePrograms, .StatsRows = Detail::ForwardTransparentRows, .Targets = Detail::SceneColorTargets },
		// The Overdraw data view's additive pass, for the three cull modes.
		{ .Name = "Overdraw", .Stage = RenderStage::Forward, .PipelineCount = 3, .Programs = Detail::ScenePrograms, .StatsRows = Detail::OverdrawRows, .Targets = Detail::OverdrawTargets },
		// QuadBatcher.h: its users record under their own rows (M21's Sprites in this stage, M22's Ui in SceneOverlay).
		{ .Name = "QuadBatcher", .Stage = RenderStage::Sprites, .PipelineCount = 2, .Programs = Detail::QuadPrograms },
		{ .Name = "Bloom", .Stage = RenderStage::HdrPost, .PipelineCount = 3, .Programs = Detail::BloomPrograms, .StatsRows = Detail::BloomRows, .Targets = Detail::BloomTargets },
		// The tonemap pass and the data views' composite, which replaces it under the same row.
		{ .Name = "Tonemap", .Stage = RenderStage::Tonemap, .PipelineCount = 2, .Programs = Detail::TonemapPrograms, .StatsRows = Detail::TonemapRows, .Targets = Detail::TonemapTargets },
		{ .Name = "Fxaa", .Stage = RenderStage::LdrPost, .PipelineCount = 1, .Programs = Detail::FxaaPrograms, .StatsRows = Detail::FxaaRows, .Targets = Detail::FxaaTargets },
		// Game Text at the scene resolution (TextRenderer's two pipelines, which Labels shares).
		{ .Name = "Text", .Stage = RenderStage::SceneOverlay, .PipelineCount = 2, .Programs = Detail::TextPrograms, .StatsRows = Detail::TextRows },
		{ .Name = "Upscale", .Stage = RenderStage::Output, .PipelineCount = 2, .Programs = Detail::UpscalePrograms, .StatsRows = Detail::UpscaleRows, .Targets = Detail::UpscaleTargets },
		{ .Name = "Selection", .Stage = RenderStage::EditorOverlay, .PipelineCount = 8, .Programs = Detail::SelectionPrograms, .StatsRows = Detail::SelectionRows, .Targets = Detail::SelectionTargets },
		{ .Name = "Wireframe", .Stage = RenderStage::EditorOverlay, .StatsRows = Detail::WireframeRows },
		// The debug lines' two pipelines (DebugRenderer.h): debug draw, annotations, grid and icons.
		{ .Name = "Overlays", .Stage = RenderStage::EditorOverlay, .PipelineCount = 2, .Programs = Detail::DebugLinesPrograms, .StatsRows = Detail::OverlaysRows },
		// The debug and annotation labels, at the view's resolution with the Text pipelines.
		{ .Name = "Labels", .Stage = RenderStage::EditorOverlay, .StatsRows = Detail::LabelsRows },
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
