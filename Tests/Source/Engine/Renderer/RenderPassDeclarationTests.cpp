#include "TestsPCH.h"

#include "Engine/Renderer/RenderPassDeclaration.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GtaoPass.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/Lights2DPass.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderStage.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TerrainPass.h"
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
// decisions 1 and 2): the table's invariants and the frozen stats order, the one creation entry, CreatePipelines, the
// declared startup count and the stage boundaries every Render passes.

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
			// M17's order (§8.3) with Text moved to the SceneOverlay stage, the output stage's Upscale after it, the labels
			// after the other editor overlays and the letterbox repaint last; M23's Skinning after Prepare (C23.3), PointShadows
			// after SpotShadows and LightClusters after the GTAO denoise; M21's Sprites, then Lights2D (C21.2), in the Sprites
			// stage, between the forward passes and Bloom, and M22's Ui after Text in M18's reserved slot
			// (Docs/Decisions/0033-m22-contract.md decision 7): ADR 0028's phase-2 ledger's 31 rows. M26's six (C26.1,
			// Docs/Decisions/0037-m26-contract.md decision 13) at the places phase 3's ledger fixes: TerrainDepthNormal after
			// DepthNormal, TerrainForward after ForwardOpaque, SSR after Skybox, Particles after Lights2D, TAA before Bloom and
			// Exposure after it: the ledger's 37 rows, a frozen contract of stats.get.
			constexpr std::array<std::string_view, 37> Expected = { "Prepare", "Skinning", "DirectionalShadows", "SpotShadows", "PointShadows",
				"DepthNormal", "TerrainDepthNormal", "DepthPyramid", "GTAO", "GTAODenoiseHorizontal", "GTAODenoiseVertical", "LightClusters",
				"ForwardOpaque", "TerrainForward", "Skybox", "SSR", "ForwardTransparent", "Overdraw", "Sprites", "Lights2D", "Particles", "TAA",
				"Bloom", "Exposure", "Tonemap", "FXAA", "Text", "Ui", "Upscale", "SelectionMask", "SelectionDilateHorizontal",
				"SelectionDilateVertical", "SelectionComposite", "Wireframe", "Overlays", "Labels", "Letterbox" };
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
			// M23: the cluster masks' compute pipeline.
			CHECK(GetDeclaredPipelineCountOf("LightClusters") == LightClusterPass::PipelineCount);
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
			CHECK(GetDeclaredPipelineCountOf("Letterbox") == 0);
			// M22: the Ui pass records with the QuadBatcher's Ldr and (C22.3) LdrDepth pipelines and the Text pipelines.
			CHECK(GetDeclaredPipelineCountOf("Ui") == 0);
			// M23: the point shadows draw with the Shadows declaration's caster pipelines.
			CHECK(GetDeclaredPipelineCountOf("PointShadows") == 0);
			// M26 (C26.1, Docs/Decisions/0037-m26-contract.md decision 7): the particles record with the QuadBatcher's Hdr and
			// HdrAdditive pipelines.
			CHECK(GetDeclaredPipelineCountOf("Particles") == 0);
			// M26 (decision 22, stream T-C): the terrain pass's prepass and picking prepass, and its forward pass.
			CHECK(GetDeclaredPipelineCountOf("TerrainDepthNormal") == TerrainPass::DepthNormalPipelineCount);
			CHECK(GetDeclaredPipelineCountOf("TerrainForward") == TerrainPass::ForwardPipelineCount);
			CHECK(TerrainPass::GetLayoutDescriptions().size() == TerrainPass::PipelineCount);
		}

		// M26 (C26.1, Docs/Decisions/0037-m26-contract.md decisions 13 and 14): the six passes at phase 3's ledger's places, in
		// existing stages, with their targets; the prepass gains its velocity and roughness targets after EntityId. Their
		// pipelines join with their streams, so the contract adds none to the startup count.
		TEST_CASE("RenderPassDeclarations: M26's passes take the ledger's places and targets")
		{
			const auto find = [](std::string_view name)
			{
				const auto found = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [name](const RenderPassDeclaration& declaration)
				{
					return declaration.Name == name;
				});
				REQUIRE_MESSAGE(found != ScenePassDeclarations.end(), name);
				return found;
			};
			CHECK(find("TerrainDepthNormal")->Stage == RenderStage::DepthNormal);
			CHECK((find("TerrainDepthNormal") - 1)->Name == "DepthNormal");
			CHECK(find("TerrainForward")->Stage == RenderStage::Forward);
			CHECK((find("TerrainForward") - 1)->Name == "ForwardOpaque");
			CHECK((find("TerrainForward") + 1)->Name == "Skybox");
			CHECK(find("Ssr")->Stage == RenderStage::Forward);
			CHECK((find("Ssr") - 1)->Name == "Skybox");
			CHECK((find("Ssr") + 1)->Name == "ForwardTransparent");
			CHECK(find("Particles")->Stage == RenderStage::Sprites);
			CHECK((find("Particles") - 1)->Name == "Lights2D");
			CHECK(find("Taa")->Stage == RenderStage::HdrPost);
			CHECK((find("Taa") + 1)->Name == "Bloom");
			CHECK(find("Exposure")->Stage == RenderStage::HdrPost);
			CHECK((find("Exposure") - 1)->Name == "Bloom");
			CHECK((find("Exposure") + 1)->Stage == RenderStage::Tonemap);
			const auto depthNormal = find("DepthNormal");
			REQUIRE(depthNormal->Targets.size() == 5);
			CHECK(depthNormal->Targets[2] == "EntityId");
			CHECK(depthNormal->Targets[3] == "SceneVelocity");
			CHECK(depthNormal->Targets[4] == "SceneRoughness");
			REQUIRE(find("Ssr")->Targets.size() == 1);
			CHECK(find("Ssr")->Targets[0] == "SsrColor");
			REQUIRE(find("Taa")->Targets.size() == 1);
			CHECK(find("Taa")->Targets[0] == "TaaHistory");
		}

		// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27; stream C): the Skinning pass's one compute pipeline joins
		// its declaration with the pass, and the deformed draws keep the 24 mesh pipelines (Roadmap M23: "there are still 24
		// mesh pipelines").
		TEST_CASE("RenderPassDeclarations: the Skinning pass declares its compute pipeline, and the mesh pipelines stay 24")
		{
			CHECK(GetDeclaredPipelineCountOf("Skinning") == SkinningPass::PipelineCount);
			CHECK(SkinningPass::PipelineCount == 1);
			CHECK(SceneRendererPipelines::MeshPipelineCount == 24);
			// 64 on phase 2's merged tree with M21's Lights2D and QuadBatcher pipelines and M22's LdrDepth (60 on M23's branch);
			// 65 with M26's QuadBatcher HdrAdditive (Docs/Decisions/0037-m26-contract.md decision 7), 67 with its Exposure pass
			// and 73 with its Taa, Ssr and terrain passes (decisions 13 and 22).
			CHECK(GetDeclaredPipelineCount() == 73);
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the Lights2D declaration has its pipeline of the Lights2D
		// program and the QuadBatcher its HdrLit and Normals pipelines: 61 startup pipelines on M21's tree, 62 since M22 landed
		// with the QuadBatcher's LdrDepth (Docs/Decisions/0033-m22-contract.md decision 57, "Landing"), 64 with M23's
		// LightClusters and Skinning on phase 2's merged tree, 65 with M26's HdrAdditive for the additive particles
		// (Docs/Decisions/0037-m26-contract.md decision 7), the QuadBatcher's sixth, 67 with M26's Exposure pass, 68 with its Taa
		// pass, 70 with its Ssr pass (decision 13) and 73 with its terrain pass (decision 22).
		TEST_CASE("RenderPassDeclarations: the Lights2D pass declares one pipeline of its program and the QuadBatcher six")
		{
			const auto lights = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [](const RenderPassDeclaration& declaration)
			{
				return declaration.Name == "Lights2D";
			});
			REQUIRE(lights != ScenePassDeclarations.end());
			CHECK(lights->Stage == RenderStage::Sprites);
			CHECK(lights->PipelineCount == 1);
			REQUIRE(lights->Programs.size() == 1);
			CHECK(lights->Programs[0] == "Lights2D");
			REQUIRE(lights->Targets.size() == 2);
			CHECK(lights->Targets[0] == "Lights2DNormals");
			CHECK(lights->Targets[1] == "Lights2DLight");
			// It follows Sprites, which follows the QuadBatcher.
			REQUIRE(lights != ScenePassDeclarations.begin());
			CHECK((lights - 1)->Name == "Sprites");
			CHECK(GetDeclaredPipelineCountOf("QuadBatcher") == 6);
			CHECK(QuadBatcher::PipelineCount == 6);
			CHECK(GetDeclaredPipelineCount() == 73);
			CHECK(GetDeclaredPipelineCountOf("Lights2D") == Lights2DPass::PipelineCount);
			CHECK(Lights2DPass::GetLayoutDescriptions().size() == Lights2DPass::PipelineCount);
			CHECK(QuadBatcher::GetLayoutDescriptions().size() == QuadBatcher::PipelineCount);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("SceneRenderer: every Render passes each stage boundary in order")
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

		TEST_CASE("Pipelines: Create makes exactly the declared startup pipelines")
		{
			// M18 acceptance (C18.1): the startup count is the declarations' sum, and Create goes through CreatePipelines.
			// M17's 54, M18's 4, M21's 3 (Lights2D, the QuadBatcher's HdrLit and Normals), M22's 1 (the QuadBatcher's LdrDepth),
			// M23's 2 (the LightClusters and Skinning passes, Docs/Decisions/0034-m23-contract.md decisions 5 and 27) and M26's 9
			// (the QuadBatcher's HdrAdditive, Docs/Decisions/0037-m26-contract.md decision 7, the Exposure pass's 2, the Taa pass's
			// 1 and the Ssr pass's 2, decision 13, and the terrain pass's 3, decision 22).
			static_assert(GetDeclaredPipelineCount() == 73);
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
			CHECK(SceneRendererPipelines::StartupPipelineCount == GetDeclaredPipelineCount());
			CHECK((*pipelines)->GetPipelineCount() == GetDeclaredPipelineCount());
			CHECK(SceneRendererPipelines::GetLayoutDescriptions().size() == GetDeclaredPipelineCount());
			CHECK((*pipelines)->GetGeneration() == 1);
		}

		TEST_CASE("Pipelines: CreatePipelines replaces every pipeline and the image stays the same")
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
			CHECK(generation == 1);

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

		TEST_CASE("SceneRenderer: the first Render after CreatePipelines makes every binding set for the new layouts")
		{
			// M18 (C18.1, decision 2): a renderer whose binding sets were made for an earlier generation of its pipeline set
			// clears them, its material sets included, before its next Render. Once that Render retired, no set holds a
			// replaced binding layout, so as many layouts and sets are alive as before the replacement. A material set kept
			// across the generation would keep the replaced material layout alive (one layout more).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			const auto retire = [&device]()
			{
				device.WaitForIdle();
				device.RunGarbageCollection();
			};
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				RenderSnapshot snapshot = setup.Snapshot();
				setup.Submit(snapshot);
				retire();
				const uint64_t layouts = tracker.GetCounts(GpuResourceType::BindingLayout).GetLive();
				const uint64_t sets = tracker.GetCounts(GpuResourceType::BindingSet).GetLive();

				const Status created = setup.Pipelines().CreatePipelines();
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				++snapshot.FrameIndex;
				setup.Submit(snapshot);
				retire();
				if (GpuResourceTracker::IsEnabled())
				{
					CHECK(tracker.GetCounts(GpuResourceType::BindingLayout).GetLive() == layouts);
					CHECK(tracker.GetCounts(GpuResourceType::BindingSet).GetLive() == sets);
				}
			}
			retire();
		}
	}

}
