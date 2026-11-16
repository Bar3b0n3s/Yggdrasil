#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/InstanceRecord.h"
#include "Support/BufferCreationFails.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// A scene renderer's requests after startup that run out of GPU memory (M18, C18.4; Docs/Decisions/0029-m18-contract.md
// decisions 42, 43, 44 and 46): a view's new size and a debug-view switch are trimmed and retried once, and then fail alone
// with OutOfMemory, keeping what the renderer had; never fatal. ReleaseTargets lets an owner whose image nothing displays
// give its targets back. M23 (Docs/Decisions/0034-m23-contract.md decision 20): the per-view buffers a Render creates
// lazily, the instance buffer and the light clusters' buffers, whose out-of-memory creation fails only that Render.

namespace Engine {

	namespace {

		// Renders `snapshot` into a command list of its own and submits it, returning Render's status: an error before
		// recording leaves nothing pending, which OnSubmitted then ignores.
		Status RenderOnce(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			(*list)->open();
			const Status rendered = renderer.Render(**list, snapshot);
			(*list)->close();
			renderer.OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			return rendered;
		}

		// `fixture`'s snapshot with four quads and a point light: the view has instances, and its forward passes read the
		// light clusters.
		RenderSnapshot MakeClusteredSnapshot(const Test::SceneRendererIntegrationFixture& fixture)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Meshes.clear();
			for (int index = 0; index < 4; ++index)
			{
				const glm::vec3 position(static_cast<float>(index) - 1.5f, 0.0f, -4.0f);
				snapshot.Meshes.push_back({ .World = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f)),
					.Mesh = UUID(8910),
					.Materials = { UUID(8911) } });
			}
			snapshot.Lights = { { .Type = RenderLightType::Point, .Intensity = 20.0f, .Position = glm::vec3(0.0f, 0.0f, -2.0f), .Range = 6.0f, .Entity = UUID(77) } };
			return snapshot;
		}

		// Whether the last render of `renderer` recorded the pass whose stats row is `name`.
		bool Recorded(SceneRenderer& renderer, std::string_view name)
		{
			return Test::SceneRendererIntegrationFixture::FindPass(renderer.GetRenderStats(), name) != nullptr;
		}

		// Selects the creation of a storage buffer of `size` bytes: a view's instance buffer or its light cluster masks.
		Test::BufferCreationFails::Selector StorageBufferOfSize(uint64_t size)
		{
			return [size](const VkBufferCreateInfo& info)
			{
				return (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0 && info.size == size;
			};
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: a Resize that runs out of memory is retried after a trim and renders the same image"
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("SceneRenderer: a debug view that keeps running out of memory renders the view last rendered, or Lit once its pipelines are dropped"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 44): Render falls back to the debug view this renderer last rendered, not to a fixed Lit: a
			// data view, whose pipelines exist from startup, stays. A material view's pipelines are created on first use and
			// dropped by CreatePipelines (a shader reload or a recovery), so a renderer that last rendered one falls back to
			// Lit. GetRenderedDebugView names the view the image shows, which a host sets its setting back to.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomPipelinePersistent });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				// Records `snapshot` and returns Render's status (an OutOfMemory partial error records the view).
				const auto record = [&device, &fixture](const RenderSnapshot& snapshot)
				{
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
					(*commandList)->open();
					const Status rendered = fixture.Renderer().Render(**commandList, snapshot);
					(*commandList)->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**commandList));
					return rendered;
				};
				RenderSnapshot snapshot = fixture.Snapshot();
				const Image lit = fixture.Render(snapshot);
				CHECK(fixture.Renderer().GetRenderedDebugView() == RenderDebugView::Lit);
				// Before the arming: Albedo's pipelines are created by its first render, then a reload's CreatePipelines drops them.
				snapshot.DebugView = RenderDebugView::Albedo;
				++snapshot.FrameIndex;
				static_cast<void>(fixture.Render(snapshot));
				CHECK(fixture.Renderer().GetRenderedDebugView() == RenderDebugView::Albedo);
				device.WaitForIdle();
				const Status reloaded = fixture.Pipelines().CreatePipelines();
				REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
				device.RunGarbageCollection();

				// The view last rendered has no pipelines any more: Lit.
				++snapshot.FrameIndex;
				const Status dropped = record(snapshot);
				REQUIRE_FALSE(dropped.has_value());
				CHECK(dropped.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Renderer().GetRenderedDebugView() == RenderDebugView::Lit);
				CHECK(fixture.Read().Pixels == lit.Pixels);

				// A data view rendered last: the failed switch stays on it.
				snapshot.DebugView = RenderDebugView::Overdraw;
				++snapshot.FrameIndex;
				const Image overdraw = fixture.Render(snapshot);
				CHECK(overdraw.Pixels != lit.Pixels);
				snapshot.DebugView = RenderDebugView::Emissive;
				++snapshot.FrameIndex;
				const Status stayed = record(snapshot);
				REQUIRE_FALSE(stayed.has_value());
				CHECK(stayed.error().GetCode() == ErrorCode::OutOfMemory);
				CHECK(fixture.Renderer().GetRenderedDebugView() == RenderDebugView::Overdraw);
				CHECK(fixture.Read().Pixels == overdraw.Pixels);
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("SceneRenderer: under oom-specialization a debug view renders the previous mode and returns OutOfMemory"
			* doctest::test_suite(Test::GpuSuite))
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

		// M23 (decisions 2 and 20): the Prepare stage creates the view's instance buffer; an out-of-memory creation fails the
		// Render before recording (nothing recorded), as a lazily created target's does, never fatal, and the host's next
		// Render creates it and renders the image a renderer without the failure renders.
		TEST_CASE("SceneRenderer: an instance buffer that runs out of memory fails the render before recording, and the next render creates it"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			std::vector<std::byte> reference;
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				reference = fixture.Render(MakeClusteredSnapshot(fixture)).Pixels;
			}
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				RenderSnapshot snapshot = MakeClusteredSnapshot(fixture);
				{
					// The first instance buffer holds 64 records (SceneRenderer::State::EnsureInstanceBuffer's minimum).
					const Test::BufferCreationFails failing(StorageBufferOfSize(uint64_t{ 64 } * sizeof(InstanceRecord)));
					const Status rendered = RenderOnce(device, fixture.Renderer(), snapshot);
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(IsGpuOutOfMemory(rendered.error(), device));
					CHECK(failing.GetFailedCount() == 1);
				}
				++snapshot.FrameIndex;
				CHECK(fixture.Render(snapshot).Pixels == reference);
				CHECK(Recorded(fixture.Renderer(), "ForwardOpaque"));
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		// M23 (decisions 5 and 20): the Ambient stage creates the light clusters' buffers; when the mask buffer's first
		// creation runs out of memory, the forward passes and the data views' composite, which bind it, are skipped with a
		// partial Gpu error, never fatal, and the next Render creates it and renders the image a renderer without the failure
		// renders.
		TEST_CASE("SceneRenderer: light cluster masks that run out of memory skip the forward passes, and the next render creates them"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			std::vector<std::byte> reference;
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				reference = fixture.Render(MakeClusteredSnapshot(fixture)).Pixels;
			}
			{
				Test::SceneRendererIntegrationFixture fixture(gpu);
				RenderSnapshot snapshot = MakeClusteredSnapshot(fixture);
				const LightClusterGrid grid = ComputeLightClusterGrid(snapshot.Camera, fixture.Renderer().GetWidth(), fixture.Renderer().GetHeight());
				{
					const Test::BufferCreationFails failing(StorageBufferOfSize(LightClusterPass::GetMaskBufferDesc(GetLightClusterCount(grid), grid.WordCount).byteSize));
					const Status rendered = RenderOnce(device, fixture.Renderer(), snapshot);
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(IsGpuOutOfMemory(rendered.error(), device));
					CHECK(failing.GetFailedCount() == 1);
					CHECK_FALSE(Recorded(fixture.Renderer(), "ForwardOpaque"));
					CHECK_FALSE(Recorded(fixture.Renderer(), "LightClusters"));

					// The LightCount view's composite binds them too.
					RenderSnapshot lightCount = snapshot;
					++lightCount.FrameIndex;
					lightCount.DebugView = RenderDebugView::LightCount;
					const Status composited = RenderOnce(device, fixture.Renderer(), lightCount);
					REQUIRE_FALSE(composited.has_value());
					CHECK(composited.error().GetCode() == ErrorCode::Gpu);
					CHECK(failing.GetFailedCount() == 2);
				}
				snapshot.FrameIndex += 2;
				CHECK(fixture.Render(snapshot).Pixels == reference);
				CHECK(Recorded(fixture.Renderer(), "ForwardOpaque"));
				CHECK(Recorded(fixture.Renderer(), "LightClusters"));
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		// M23 (decisions 5 and 20): a larger view needs a larger mask buffer; when that creation runs out of memory the frame
		// keeps the smaller buffer, which the brute-force loop never reads, and shades every light of the list instead, so the
		// image is the clustered one exactly (decision 5), with a partial Gpu error; the next Render grows the masks.
		TEST_CASE("SceneRenderer: light cluster masks that cannot grow fall back to the brute-force light loop with the same image"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
				static_cast<void>(fixture.Render(MakeClusteredSnapshot(fixture)));
				REQUIRE(fixture.Renderer().Resize(256, 128).has_value());
				RenderSnapshot snapshot = MakeClusteredSnapshot(fixture);
				++snapshot.FrameIndex;
				const LightClusterGrid grid = ComputeLightClusterGrid(snapshot.Camera, 256, 128);
				std::vector<std::byte> fallback;
				{
					const Test::BufferCreationFails failing(StorageBufferOfSize(LightClusterPass::GetMaskBufferDesc(GetLightClusterCount(grid), grid.WordCount).byteSize));
					const Status rendered = RenderOnce(device, fixture.Renderer(), snapshot);
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(failing.GetFailedCount() == 1);
					CHECK(Recorded(fixture.Renderer(), "ForwardOpaque"));
					CHECK_FALSE(Recorded(fixture.Renderer(), "LightClusters"));
					fallback = fixture.Read().Pixels;
				}
				++snapshot.FrameIndex;
				const Image clustered = fixture.Render(snapshot);
				CHECK(Recorded(fixture.Renderer(), "LightClusters"));
				CHECK(clustered.Pixels == fallback);
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}
	}

}
