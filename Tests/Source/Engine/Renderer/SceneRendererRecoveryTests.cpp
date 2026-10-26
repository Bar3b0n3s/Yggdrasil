#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A scene renderer's recovery from a device loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 26 and 27):
// the placement rule's SceneRenderer::State, with its RenderTargetPool, through the shared loss test.

namespace Engine {

	namespace Test {

		// Renders `snapshot` with `renderer` into a command list of its own and reads the final image back, returning every
		// error instead of failing the test, so it also runs on a lost device.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderAndRead(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
			list->open();
			const Status rendered = renderer.Render(*list, snapshot);
			list->close();
			renderer.OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
			ENGINE_TRY(rendered);
			nvrhi::ITexture* image = renderer.GetFinalTexture();
			if (image == nullptr)
				return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
			Readback readback(device);
			ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
			return pixels.Pixels;
		}

		// RenderAndRead with `setup`'s renderer.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderAndRead(GraphicsDevice& device, SceneRendererIntegrationFixture& setup,
			const RenderSnapshot& snapshot)
		{
			return RenderAndRead(device, setup.Renderer(), snapshot);
		}

		// The fixture's lit quad with every per-view object a render at the view's resolution creates besides the targets of
		// Create: picking's EntityId, the selection outline's mask, GTAO's targets, the shadow cascades of a shadow-casting
		// directional light, bloom and FXAA.
		[[nodiscard]] static RenderSnapshot MakeFullSnapshot(const SceneRendererIntegrationFixture& setup, uint64_t frameIndex)
		{
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.FrameIndex = frameIndex;
			snapshot.Flags = RenderViewFlags::Picking | RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(900) };
			snapshot.Post.SsaoEnabled = true;
			snapshot.Post.BloomEnabled = true;
			snapshot.Post.FxaaEnabled = true;
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
				.Intensity = 2.0f,
				.Direction = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)),
				.CastShadows = true });
			return snapshot;
		}

		// MakeFullSnapshot at a 20 x 20 scene resolution, upscaled 3 times into the 64 x 64 view with a letterbox: the
		// output stage's targets as well.
		[[nodiscard]] static RenderSnapshot MakeScaledSnapshot(const SceneRendererIntegrationFixture& setup, uint64_t frameIndex)
		{
			RenderSnapshot snapshot = MakeFullSnapshot(setup, frameIndex);
			snapshot.Output = RenderOutputSettings{ .SceneWidth = 20, .SceneHeight = 20 };
			snapshot.Camera.ViewportWidth = 20;
			snapshot.Camera.ViewportHeight = 20;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 2.0f, 0.1f, 20.0f, 20, 20);
			return snapshot;
		}

		// Submits `list`, closed with a render the renderer released before its submission (a loss in the middle of a frame),
		// and waits for it: NVRHI's command buffer references its command list until the list is executed, so a closed list
		// that is never executed keeps itself and everything it recorded alive. Nothing collected garbage since the release, so
		// every object the list uses still exists (the device's tracker references even those that it only cleared), and the
		// renderer awaits no OnSubmitted for it.
		static void RetireUnsubmittedList(GraphicsDevice& device, nvrhi::ICommandList& list)
		{
			device.ExecuteCommandList(list);
			device.WaitForIdle();
		}

		// The tracker's live counts once the device is idle and its garbage collected, with their description for messages.
		struct LiveCounts
		{
			std::array<uint64_t, GpuResourceTypeCount> ByType{};
			uint64_t Total = 0;
			std::string Description{};
		};

		[[nodiscard]] static LiveCounts ReadLiveCounts(GraphicsDevice& device)
		{
			device.WaitForIdle();
			device.RunGarbageCollection();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			LiveCounts counts;
			for (size_t type = 0; type < GpuResourceTypeCount; ++type)
				counts.ByType[type] = tracker.GetLiveCount(static_cast<GpuResourceType>(type));
			counts.Total = tracker.GetTotalLiveCount();
			counts.Description = tracker.DescribeLiveCounts();
			return counts;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: a device loss recreates its per-view objects and renders the same image"
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			Test::RetireUnsubmittedList(device, **list);
			list->Reset();
		}

		TEST_CASE("SceneRenderer: Release leaves none of the renderer's GPU objects alive" * doctest::test_suite(Test::GpuSuite))
		{
			// The placement rule's place 2 (C18.3, decision 27): once its renderers released, the recovery finds no tracked
			// object alive, so a released renderer holds nothing its destruction would free: not its targets, its pool's free
			// targets, the output stage's targets, its binding and material sets, its per-view buffers, its picker's staging
			// copies or its profiler's queries.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<SceneRenderer>> created = SceneRenderer::Create(device, setup.Pipelines(), setup.Cache(), setup.Assets(), { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Scope<SceneRenderer> renderer = std::move(*created);

			const RenderSnapshot full = Test::MakeFullSnapshot(setup, 1);
			const Result<std::vector<std::byte>> fullImage = Test::RenderAndRead(device, *renderer, full);
			REQUIRE_MESSAGE(fullImage.has_value(), fullImage.error().ToString());
			// A pick in flight: its staging copy, command list and event query belong to the renderer's picker.
			const Result<PickTicket> ticket = renderer->RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = full.FrameIndex,
				.SceneRevision = full.SceneRevision,
				.ViewGeneration = renderer->GetViewGeneration() });
			REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
			// The scaled render leaves the view-size targets free in the pool and adds the output stage's.
			const Result<std::vector<std::byte>> scaledImage = Test::RenderAndRead(device, *renderer, Test::MakeScaledSnapshot(setup, 2));
			REQUIRE_MESSAGE(scaledImage.has_value(), scaledImage.error().ToString());
			const Test::LiveCounts rendered = Test::ReadLiveCounts(device);

			renderer->ReleaseGpuObjects();
			const Test::LiveCounts released = Test::ReadLiveCounts(device);
			INFO("live after the render: ", rendered.Description);
			INFO("live after the release: ", released.Description);
			CHECK(released.Total < rendered.Total);
			// The released picker's ticket resolves without a result.
			CHECK_FALSE(renderer->PollPick(*ticket, 10).has_value());

			renderer.reset();
			const Test::LiveCounts destroyed = Test::ReadLiveCounts(device);
			INFO("live after the destruction: ", destroyed.Description);
			CHECK(released.ByType == destroyed.ByType);
		}

		TEST_CASE("SceneRenderer: a pick pending at the release resolves as cancelled, and no ticket is reused after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// C18.3, decision 27 (SceneRenderer.h, ReleaseGpuObjects: "every pending pick resolves as cancelled"): the release
			// drops the picker's GPU objects but keeps its tickets, so a host's pending ticket answers Cancelled, and a ticket
			// after the recovery never equals one from before, whose holder would otherwise read the new pick's result.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			SceneRenderer& renderer = setup.Renderer();
			const RenderSnapshot before = Test::MakeFullSnapshot(setup, 1);
			const Result<std::vector<std::byte>> beforeImage = Test::RenderAndRead(device, renderer, before);
			REQUIRE_MESSAGE(beforeImage.has_value(), beforeImage.error().ToString());
			const Result<PickTicket> pending = renderer.RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = before.FrameIndex,
				.SceneRevision = before.SceneRevision,
				.ViewGeneration = renderer.GetViewGeneration() });
			REQUIRE_MESSAGE(pending.has_value(), pending.error().ToString());

			device.WaitForIdle();
			renderer.ReleaseGpuObjects();
			const Result<std::optional<PickResult>> cancelled = renderer.PollPick(*pending, before.FrameIndex + 10);
			REQUIRE_FALSE(cancelled.has_value());
			CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
			const Status recreated = renderer.RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());

			const RenderSnapshot after = Test::MakeFullSnapshot(setup, 20);
			const Result<std::vector<std::byte>> afterImage = Test::RenderAndRead(device, renderer, after);
			REQUIRE_MESSAGE(afterImage.has_value(), afterImage.error().ToString());
			const Result<PickTicket> next = renderer.RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = after.FrameIndex,
				.SceneRevision = after.SceneRevision,
				.ViewGeneration = renderer.GetViewGeneration() });
			REQUIRE_MESSAGE(next.has_value(), next.error().ToString());
			CHECK(next->Value > pending->Value);
			// The picker created its GPU objects again for the new pick, which resolves.
			device.WaitForIdle();
			const Result<std::optional<PickResult>> resolved = renderer.PollPick(*next, after.FrameIndex + 2);
			REQUIRE_MESSAGE(resolved.has_value(), resolved.error().ToString());
			CHECK(resolved->has_value());
		}

		TEST_CASE("SceneRenderer: after Release and Recreate it renders the same images, and its live counts return"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The renderer's half of M18's acceptance (C18.3, decision 27): with its set and its cache kept, Release and Recreate
			// leave the renderer as Create made it, so the same renders produce byte-identical images and the same live GPU
			// objects; a render recorded but never submitted before the release (a loss in the middle of a frame) awaits no
			// OnSubmitted.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			SceneRenderer& renderer = setup.Renderer();
			uint64_t frame = 0;
			const auto renderBoth = [&device, &setup, &renderer, &frame]() -> Result<std::array<std::vector<std::byte>, 2>>
			{
				std::array<std::vector<std::byte>, 2> images{};
				ENGINE_TRY_ASSIGN(images[0], Test::RenderAndRead(device, renderer, Test::MakeFullSnapshot(setup, ++frame)));
				ENGINE_TRY_ASSIGN(images[1], Test::RenderAndRead(device, renderer, Test::MakeScaledSnapshot(setup, ++frame)));
				return images;
			};
			const Result<std::array<std::vector<std::byte>, 2>> before = renderBoth();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			const Test::LiveCounts countsBefore = Test::ReadLiveCounts(device);

			Result<nvrhi::CommandListHandle> pending = device.CreateCommandList();
			REQUIRE_MESSAGE(pending.has_value(), pending.error().ToString());
			(*pending)->open();
			REQUIRE(renderer.Render(**pending, Test::MakeFullSnapshot(setup, ++frame)).has_value());
			(*pending)->close();
			renderer.ReleaseGpuObjects();
			Test::RetireUnsubmittedList(device, **pending);
			pending->Reset();
			const Status recreated = renderer.RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			CHECK(renderer.GetWidth() == 64);
			CHECK(renderer.GetHeight() == 64);

			const Result<std::array<std::vector<std::byte>, 2>> after = renderBoth();
			REQUIRE_MESSAGE(after.has_value(), after.error().ToString());
			CHECK((*after)[0] == (*before)[0]);
			CHECK((*after)[1] == (*before)[1]);
			const Test::LiveCounts countsAfter = Test::ReadLiveCounts(device);
			INFO("live before the release: ", countsBefore.Description);
			INFO("live after the recreation and the same renders: ", countsAfter.Description);
			CHECK(countsAfter.ByType == countsBefore.ByType);
		}

		TEST_CASE("SceneRenderer: a debug view first asked for on a lost device returns the loss and is created after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28): "a Gpu error on a lost device is the loss, never out-of-memory". The Albedo view's
			// pipelines cannot be created on the lost device, so Render returns the creation's Gpu error instead of ending the
			// process with FatalError(OutOfMemory) (§8.14 item 7), and the first Albedo render after the recovery creates them.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			RenderSnapshot lit = setup.Snapshot();
			setup.Submit(lit);
			REQUIRE(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount());

			RenderSnapshot albedo = lit;
			albedo.FrameIndex = lit.FrameIndex + 1;
			albedo.DebugView = RenderDebugView::Albedo;
			{
				// The command list exists before the loss (a lost device creates none), as a frame's list does.
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				device.GetDiagnostics().SetDeviceLost(true);
				device.ReportDeviceLost("SceneRendererRecoveryTests");
				(*list)->open();
				const Status lost = setup.Renderer().Render(**list, albedo);
				(*list)->close();
				setup.Renderer().OnSubmitted(albedo.FrameIndex, device.ExecuteCommandList(**list));
				REQUIRE_FALSE(lost.has_value());
				CHECK(lost.error().GetCode() == ErrorCode::Gpu);
				CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount());
			}

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			albedo.FrameIndex = lit.FrameIndex + 2;
			const Result<std::vector<std::byte>> image = Test::RenderAndRead(device, setup, albedo);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount() + SceneRendererPipelines::DebugViewPipelineCount);
		}
	}

}
