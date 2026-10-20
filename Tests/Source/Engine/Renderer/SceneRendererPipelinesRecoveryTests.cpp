#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

// The pipeline set's recovery from a device loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 26 and 27):
// every declared pass, the QuadBatcher and the output stage's UpscalePass included, is released with the set and created
// again through CreatePipelines.

namespace Engine {

	namespace Test {

		// Renders `snapshot` with `setup`'s renderer into a command list of its own and reads the final image back, returning
		// every error instead of failing the test, so it also runs on a lost device.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderSetAndRead(GraphicsDevice& device, SceneRendererIntegrationFixture& setup,
			const RenderSnapshot& snapshot)
		{
			ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
			list->open();
			const Status rendered = setup.Renderer().Render(*list, snapshot);
			list->close();
			setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
			ENGINE_TRY(rendered);
			nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
			if (image == nullptr)
				return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
			Readback readback(device);
			ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
			return pixels.Pixels;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRendererPipelines: a device loss creates every declared pass again and draws the same images"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, decisions 26, 27 and 32): "Release and Recreate through CreatePipelines". A lit frame, a
			// debug view (whose lazily created pipelines are created again on first use) and an upscaled frame at scale 3 (the
			// output stage) render byte-identical images after the recovery, with the declared pipeline count.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 66, 48);
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 40;
			Test::DeviceLossTest test;
			test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> images;
				RenderSnapshot lit = setup.Snapshot();
				lit.FrameIndex = ++frame;
				RenderSnapshot albedo = lit;
				albedo.FrameIndex = ++frame;
				albedo.DebugView = RenderDebugView::Albedo;
				RenderSnapshot upscaled = lit;
				upscaled.FrameIndex = ++frame;
				upscaled.Output.SceneWidth = 22;
				upscaled.Output.SceneHeight = 16;
				upscaled.Camera.ViewportWidth = 22;
				upscaled.Camera.ViewportHeight = 16;
				for (const RenderSnapshot* snapshot : { &lit, &albedo, &upscaled })
				{
					ENGINE_TRY_ASSIGN(const std::vector<std::byte> pixels, Test::RenderSetAndRead(device, setup, *snapshot));
					images.insert(images.end(), pixels.begin(), pixels.end());
				}
				return images;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount() + SceneRendererPipelines::DebugViewPipelineCount);
		}

		TEST_CASE("SceneRendererPipelines: Release drops every pass and keeps the generation, which CreatePipelines increases"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, decision 27): the set's Release hook, and its Recreate hook, CreatePipelines.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			SceneRendererPipelines& pipelines = setup.Pipelines();
			const uint64_t generation = pipelines.GetGeneration();
			pipelines.ReleaseGpuObjects();
			CHECK(pipelines.GetPipelineCount() == 0);
			CHECK(pipelines.GetGeneration() == generation);
			const Status created = pipelines.CreatePipelines();
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK(pipelines.GetPipelineCount() == GetDeclaredPipelineCount());
			CHECK(pipelines.GetGeneration() == generation + 1);
		}
	}

}
