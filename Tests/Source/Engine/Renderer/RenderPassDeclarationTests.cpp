#include "TestsPCH.h"

#include "Engine/Renderer/RenderPassDeclaration.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GtaoPass.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderStage.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The pass declarations and the stages of the scene renderer (M18, C18.1, R-PASS; Docs/Decisions/0029-m18-contract.md
// decisions 1 and 2): the table's invariants and the frozen stats order (live), and the stage functions and the one creation
// entry, CreatePipelines (skipped until stream A implements them).

namespace Engine {

	namespace {

		// The PipelineCount of the declaration named `name`; fails the test case when there is none.
		uint32_t GetDeclaredPipelineCountOf(std::string_view name)
		{
			const auto found = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [name](const RenderPassDeclaration& declaration)
			{
				return declaration.Name == name;
			});
			REQUIRE_MESSAGE(found != ScenePassDeclarations.end(), name);
			return found->PipelineCount;
		}

		// The CPU profiler zones of `zones` named "Stage:...", in timeline order.
		std::vector<std::string> GetCpuStageZoneNames(const std::vector<ProfileZone>& zones)
		{
			std::vector<std::string> names;
			for (const ProfileZone& zone : zones)
			{
				if (zone.ThreadIndex != Profiler::GpuThreadIndex && zone.Name.starts_with("Stage:"))
					names.emplace_back(zone.Name);
			}
			return names;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStage: every stage has its name, in order")
		{
			constexpr std::array<std::string_view, RenderStageCount> Names = { "Prepare", "Deform", "Shadows", "DepthNormal", "Ambient", "Forward",
				"Sprites", "HdrPost", "Tonemap", "LdrPost", "SceneOverlay", "Output", "EditorOverlay" };
			for (uint32_t index = 0; index < RenderStageCount; ++index)
				CHECK(RenderStageToString(static_cast<RenderStage>(index)) == Names[index]);
		}

		TEST_CASE("RenderPassDeclarations: the declarations are in stage order and name every stats row once")
		{
			std::set<std::string_view> names;
			std::set<std::string_view> rows;
			for (size_t index = 0; index < ScenePassDeclarations.size(); ++index)
			{
				const RenderPassDeclaration& declaration = ScenePassDeclarations[index];
				CAPTURE(declaration.Name);
				CHECK_FALSE(declaration.Name.empty());
				CHECK(names.insert(declaration.Name).second);
				if (index > 0)
					CHECK(static_cast<uint32_t>(ScenePassDeclarations[index - 1].Stage) <= static_cast<uint32_t>(declaration.Stage));
				// A pass with pipelines names their programs; one without records with another pass's pipelines.
				CHECK((declaration.PipelineCount == 0) == declaration.Programs.empty());
				for (const std::string_view row : declaration.StatsRows)
				{
					CHECK_FALSE(row.empty());
					// A stage's profiler zone ("Stage:<name>") is never a stats row.
					CHECK_FALSE(row.starts_with("Stage:"));
					CHECK(rows.insert(row).second);
				}
			}
			CHECK(RenderStatsOrder.size() == rows.size());
		}

		TEST_CASE("RenderPassDeclarations: the stats order puts game Text before the upscale and the editor overlays after it")
		{
			// M17's order (§8.3) with Text moved to the SceneOverlay stage, the output stage's Upscale after it and the labels
			// last; a frozen contract of stats.get.
			constexpr std::array<std::string_view, 24> Expected = { "Prepare", "DirectionalShadows", "SpotShadows", "DepthNormal", "DepthPyramid", "GTAO",
				"GTAODenoiseHorizontal", "GTAODenoiseVertical", "ForwardOpaque", "Skybox", "ForwardTransparent", "Overdraw", "Bloom", "Tonemap", "FXAA",
				"Text", "Upscale", "SelectionMask", "SelectionDilateHorizontal", "SelectionDilateVertical", "SelectionComposite", "Wireframe",
				"Overlays", "Labels" };
			REQUIRE(RenderStatsOrder.size() == Expected.size());
			for (size_t index = 0; index < Expected.size(); ++index)
				CHECK(RenderStatsOrder[index] == Expected[index]);
		}

		TEST_CASE("RenderPassDeclarations: each pass declares the pipelines its class creates")
		{
			CHECK(GetDeclaredPipelineCountOf("Shadows") == ShadowPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("DepthNormal") + GetDeclaredPipelineCountOf("ForwardOpaque")
					+ GetDeclaredPipelineCountOf("ForwardTransparent") + GetDeclaredPipelineCountOf("Overdraw")
				== SceneRendererPipelines::MeshPipelineCount);
			CHECK(GetDeclaredPipelineCountOf("ForwardOpaque") + GetDeclaredPipelineCountOf("ForwardTransparent")
				== SceneRendererPipelines::DebugViewPipelineCount);
			CHECK(GetDeclaredPipelineCountOf("DepthPyramid") == DepthPyramidPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Gtao") == GtaoPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("BrdfLut") == BrdfLut::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Skybox") == SkyboxPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("QuadBatcher") == QuadBatcher::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Bloom") == BloomPass::PipelineCount);
			// The tonemap pass and the data views' composite.
			CHECK(GetDeclaredPipelineCountOf("Tonemap") == TonemapPass::PipelineCount + 1);
			CHECK(GetDeclaredPipelineCountOf("Fxaa") == FxaaPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Text") == TextRenderer::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Upscale") == UpscalePass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Selection") == SelectionPass::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Overlays") == DebugRenderer::PipelineCount);
			CHECK(GetDeclaredPipelineCountOf("Wireframe") == 0);
			CHECK(GetDeclaredPipelineCountOf("Labels") == 0);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("SceneRenderer: every Render passes each stage boundary in order" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			Test::SceneRendererIntegrationFixture setup(gpu);
			const uint64_t since = Profiler::GetTimeNs();
			setup.Submit(setup.Snapshot());
			std::vector<std::string> expected;
			for (uint32_t stage = 0; stage < RenderStageCount; ++stage)
				expected.push_back(std::format("Stage:{}", RenderStageToString(static_cast<RenderStage>(stage))));
			CHECK(GetCpuStageZoneNames(Profiler::CollectZones(since)) == expected);
		}

		TEST_CASE("Pipelines: Create makes exactly the declared startup pipelines" * doctest::skip(true))
		{
			// M18 acceptance (C18.1): the startup count is the declarations' sum, and Create goes through CreatePipelines.
			static_assert(GetDeclaredPipelineCount() == 58);
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
			CHECK(SceneRendererPipelines::StartupPipelineCount == GetDeclaredPipelineCount());
			CHECK((*pipelines)->GetPipelineCount() == GetDeclaredPipelineCount());
			CHECK(SceneRendererPipelines::GetLayoutDescriptions().size() == GetDeclaredPipelineCount());
			CHECK((*pipelines)->GetGeneration() == 1);
		}

		TEST_CASE("Pipelines: CreatePipelines replaces every pipeline and the image stays the same" * doctest::skip(true))
		{
			// M18 acceptance (C18.1): startup, shader reload and recovery share one creation entry; a replaced set draws the
			// same image, and a debug view's lazily created pipelines are created again after it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			const Image before = setup.Render(setup.Snapshot());
			REQUIRE(setup.Pipelines().EnsureDebugView(RenderDebugView::Albedo).has_value());
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount() + SceneRendererPipelines::DebugViewPipelineCount);
			const uint64_t generation = setup.Pipelines().GetGeneration();

			const Status created = setup.Pipelines().CreatePipelines();
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK(setup.Pipelines().GetGeneration() == generation + 1);
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount());

			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.FrameIndex = 18;
			const Image after = setup.Render(snapshot);
			CHECK(after.Pixels == before.Pixels);
			snapshot.FrameIndex = 19;
			snapshot.DebugView = RenderDebugView::Albedo;
			static_cast<void>(setup.Render(snapshot));
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount() + SceneRendererPipelines::DebugViewPipelineCount);
		}
	}

}
