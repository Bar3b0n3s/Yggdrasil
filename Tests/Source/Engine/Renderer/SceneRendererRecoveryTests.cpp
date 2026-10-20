#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

// A scene renderer's recovery from a device loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 26 and 27):
// the placement rule's SceneRenderer::State, with its RenderTargetPool, through the shared loss test.

namespace Engine {

	namespace Test {

		// Renders `snapshot` with `setup`'s renderer into a command list of its own and reads the final image back, returning
		// every error instead of failing the test, so it also runs on a lost device.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderAndRead(GraphicsDevice& device, SceneRendererIntegrationFixture& setup,
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
		TEST_CASE("SceneRenderer: a device loss recreates its per-view objects and renders the same image"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, decisions 27 and 32): the renderer, its pipeline set and its cache recover, the image is
			// byte-identical, and the tracker's counts return to their pre-loss values.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 30;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]()
			{
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.FrameIndex = ++frame;
				return Test::RenderAndRead(device, setup, snapshot);
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(setup.Renderer().GetWidth() == 64);
			CHECK(setup.Pipelines().GetGeneration() == 2); // Create, then CreatePipelines in the recovery
		}

		TEST_CASE("SceneRenderer: Release forgets a pending render and its picks, and Recreate restores the final texture"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, decision 27): a loss in the middle of a frame leaves no render awaiting OnSubmitted and no
			// pick that could read a target of the lost device.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking;
			(*list)->open();
			REQUIRE(setup.Renderer().Render(**list, snapshot).has_value());
			(*list)->close();
			const uint64_t generation = setup.Renderer().GetViewGeneration();

			setup.Renderer().ReleaseGpuObjects();
			CHECK(setup.Renderer().GetFinalTexture() == nullptr);
			CHECK(setup.Renderer().GetEntityIdTexture() == nullptr);
			CHECK(setup.Renderer().GetViewGeneration() > generation);
			CHECK_FALSE(setup.Renderer().RequestPick({ .X = 1, .Y = 1 }).has_value());
			const Status recreated = setup.Renderer().RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			CHECK(setup.Renderer().GetFinalTexture() != nullptr);
			list->Reset();
		}
	}

}
