#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstddef>
#include <vector>

// A scene renderer's requests after startup that run out of GPU memory (M18, C18.4; Docs/Decisions/0029-m18-contract.md
// decisions 42, 43, 44 and 46): a view's new size and a debug-view switch are trimmed and retried once, and then fail alone
// with OutOfMemory, keeping what the renderer had; never fatal. ReleaseTargets lets an owner whose image nothing displays
// give its targets back.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: a Resize that runs out of memory is retried after a trim and renders the same image"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42 and 46): under oom-rendertarget the first target of the new size fails, the
			// Resize trims and creates the size again, and the view renders exactly as on a device without the fault.
			std::vector<std::vector<std::byte>> images;
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomRenderTarget })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				{
					Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
					gpu.GetDevice().RunGarbageCollection(); // the first frame's collection arms the fault
					const Status resized = fixture.Renderer().Resize(64, 48);
					REQUIRE_MESSAGE(resized.has_value(), resized.error().ToString());
					CHECK(fixture.Renderer().GetWidth() == 64);
					CHECK(fixture.Renderer().GetHeight() == 48);
					images.push_back(fixture.Render(fixture.Snapshot()).Pixels);
				}
				gpu.GetDevice().RunGarbageCollection();
			}
			REQUIRE(images.size() == 2);
			CHECK(images[1] == images[0]);
		}

		TEST_CASE("SceneRenderer: a Resize that keeps running out of memory returns OutOfMemory and keeps the previous size and targets"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 46): "a resize keeps the previous ... targets"; the renderer is as
			// before the request, its final texture and pick generation included, and a target is never made smaller than asked.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTargetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
				static_cast<void>(fixture.Render(fixture.Snapshot()));
				const nvrhi::ITexture* finalTexture = fixture.Renderer().GetFinalTexture();
				const uint64_t generation = fixture.Renderer().GetViewGeneration();
				gpu.GetDevice().RunGarbageCollection();

				const Status resized = fixture.Renderer().Resize(64, 48);
				REQUIRE_FALSE(resized.has_value());
				CHECK(resized.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Renderer().GetWidth() == 32);
				CHECK(fixture.Renderer().GetHeight() == 32);
				CHECK(fixture.Renderer().GetFinalTexture() == finalTexture);
				CHECK(fixture.Renderer().GetViewGeneration() == generation);
				CHECK_FALSE(gpu.GetDevice().IsLost());
				// The current size needs no creation.
				CHECK(fixture.Renderer().Resize(32, 32).has_value());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: ReleaseTargets drops the size-dependent targets, and the next Render creates them again with the same image"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decision 43): for an owner whose image nothing displays between renders (ViewportCapture's
			// trim hook and its release before a new size): the renderer keeps its size, its final texture is gone until the
			// next Render, a trim then frees what it held, and that Render creates the targets again and renders the same image.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
				RenderSnapshot snapshot = fixture.Snapshot();
				const Image before = fixture.Render(snapshot);
				const uint64_t generation = fixture.Renderer().GetViewGeneration();
				device.RunGarbageCollection();
				const uint64_t heldTextures = device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture);

				fixture.Renderer().ReleaseTargets();
				CHECK(fixture.Renderer().GetFinalTexture() == nullptr);
				CHECK(fixture.Renderer().GetEntityIdTexture() == nullptr);
				CHECK(fixture.Renderer().GetWidth() == 32);
				CHECK(fixture.Renderer().GetHeight() == 32);
				CHECK(fixture.Renderer().GetViewGeneration() > generation); // pending picks resolve as cancelled
				device.TrimUnused();
				if (GpuResourceTracker::IsEnabled())
					CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture) < heldTextures);

				// The current size needs no Resize; the next Render creates the targets again.
				CHECK(fixture.Renderer().Resize(32, 32).has_value());
				++snapshot.FrameIndex;
				CHECK(fixture.Render(snapshot).Pixels == before.Pixels);
				CHECK(fixture.Renderer().GetFinalTexture() != nullptr);
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: a debug view whose pipelines run out of memory once is retried after a trim and renders"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42 and 46): a debug-view switch is a request; under oom-pipeline its first
			// pipeline fails, the view's pipelines are created again after the trim, and the view renders exactly as on a
			// device without the fault.
			std::vector<std::vector<std::byte>> images;
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomPipeline })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				{
					Test::SceneRendererIntegrationFixture fixture(gpu);
					gpu.GetDevice().RunGarbageCollection();
					RenderSnapshot snapshot = fixture.Snapshot();
					snapshot.DebugView = RenderDebugView::Albedo;
					images.push_back(fixture.Render(snapshot).Pixels);
					CHECK(fixture.Pipelines().GetPipelineCount() == SceneRendererPipelines::StartupPipelineCount + SceneRendererPipelines::DebugViewPipelineCount);
				}
				gpu.GetDevice().RunGarbageCollection();
			}
			REQUIRE(images.size() == 2);
			CHECK(images[1] == images[0]);
		}

		TEST_CASE("SceneRenderer: a debug view whose pipelines keep running out of memory renders the previous mode and returns OutOfMemory"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 46; plan §3.8: "a debug view stays on its previous mode with a
			// diagnostic"): the view is recorded in the mode the renderer last rendered, and Render returns OutOfMemory as a
			// partial error; nothing ends the process.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomPipelinePersistent });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				RenderSnapshot snapshot = fixture.Snapshot();
				const Image lit = fixture.Render(snapshot);
				device.RunGarbageCollection();

				snapshot.DebugView = RenderDebugView::Albedo;
				++snapshot.FrameIndex;
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				const Status rendered = fixture.Renderer().Render(**commandList, snapshot);
				(*commandList)->close();
				fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**commandList));
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Read().Pixels == lit.Pixels);
				CHECK(fixture.Pipelines().GetPipelineCount() == SceneRendererPipelines::StartupPipelineCount);
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: under oom-specialization a debug view renders the previous mode and returns OutOfMemory"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 (C18.4, decision 49): the retarget of "SceneRenderer: an out-of-memory debug-view pipeline is fatal" (M9-F39),
			// which W18.4's stream B removes with its death child: a debug view's out-of-memory after startup fails only the
			// view's switch. M5's oom-specialization fault fails every specialization from the start, so it strikes the first
			// debug view a renderer asks for, after a Lit render.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomSpecialization });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 8, 8);
				RenderSnapshot snapshot = fixture.Snapshot();
				const Image lit = fixture.Render(snapshot);

				snapshot.DebugView = RenderDebugView::Albedo;
				++snapshot.FrameIndex;
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				const Status rendered = fixture.Renderer().Render(**commandList, snapshot);
				(*commandList)->close();
				fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**commandList));
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Read().Pixels == lit.Pixels);
			}
			device.RunGarbageCollection();
		}
	}

}
