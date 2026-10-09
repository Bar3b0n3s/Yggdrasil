#include "TestsPCH.h"
#include "Engine/Renderer/AsyncPicker.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <array>
#include <limits>
#include <vector>

namespace Engine {

	namespace Utils {

		static nvrhi::TextureHandle MakeAsyncTestImage(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = 2;
			desc.height = 2;
			desc.format = nvrhi::Format::R32_UINT;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "AsyncPickerTests.Ids";
			const auto texture = device.CreateTexture(desc);
			REQUIRE(texture.has_value());
			const auto commands = device.CreateCommandList();
			REQUIRE(commands.has_value());
			const std::array<uint32_t, 4> ids{ 1, 2, 0, 99 };
			(*commands)->open();
			(*commands)->writeTexture(*texture, 0, 0, ids.data(), 2 * sizeof(uint32_t));
			(*commands)->close();
			device.ExecuteCommandList(**commands);
			return *texture;
		}

		static PickTicket QueueAsyncTestPick(AsyncPicker& picker, nvrhi::ITexture& image, std::span<const UUID> table, uint32_t x = 0, uint32_t y = 0, uint64_t frame = 100)
		{
			const auto ticket = picker.Request(image, table, 4, { .X = x, .Y = y, .FrameIndex = frame, .SceneRevision = 77, .Sequence = 9, .ViewGeneration = 4 });
			REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
			return *ticket;
		}

		static PickResult ResolveAsyncTestPick(AsyncPicker& picker, PickTicket ticket, uint64_t frame = 102)
		{
			const auto result = picker.Poll(ticket, frame, 4);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			REQUIRE(result->has_value());
			return **result;
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Picking: retains the submitted table when a later snapshot reuses PickIds")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			auto image = Utils::MakeAsyncTestImage(device);
			AsyncPicker picker(device);
			std::array table{ UUID(11), UUID(22) };
			const auto first = Utils::QueueAsyncTestPick(picker, *image, table);
			table[0] = UUID(33);
			const auto second = Utils::QueueAsyncTestPick(picker, *image, table);
			table = {};
			const auto commands = device.CreateCommandList();
			REQUIRE(commands.has_value());
			const std::array<uint32_t, 4> replacement{ 2, 0, 0, 0 };
			(*commands)->open();
			(*commands)->writeTexture(image, 0, 0, replacement.data(), 2 * sizeof(uint32_t));
			(*commands)->close();
			device.ExecuteCommandList(**commands); // Later rendering cannot change either copied pixel.
			device.WaitForIdle();
			CHECK(Utils::ResolveAsyncTestPick(picker, first).Entity == UUID(11));
			CHECK(Utils::ResolveAsyncTestPick(picker, second).Entity == UUID(33));
			const auto consumed = picker.Poll(first, 102, 4);
			REQUIRE_FALSE(consumed.has_value());
			CHECK(consumed.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Picking: stale revisions and click sequences are echoed for host rejection")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11) };
			const auto ticket = Utils::QueueAsyncTestPick(picker, *image, table);
			gpu.GetDevice().WaitForIdle();
			const auto result = Utils::ResolveAsyncTestPick(picker, ticket);
			CHECK(result.FrameIndex == 100);
			CHECK(result.SceneRevision == 77);
			CHECK(result.Sequence == 9);
			CHECK(result.ViewGeneration == 4);
			CHECK(result.Entity == UUID(11)); // Renderer never resolves a UUID through a different scene.
		}

		TEST_CASE("Picking: resize cancels requests without mapping in-flight memory")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11) };
			const auto ticket = Utils::QueueAsyncTestPick(picker, *image, table);
			image = nullptr; // Only the submitted copy may retain the old source.
			const auto stale = picker.Poll(ticket, 102, 5);
			REQUIRE_FALSE(stale.has_value());
			CHECK(stale.error().GetCode() == ErrorCode::Cancelled);
			gpu.GetDevice().WaitForIdle();
			const auto consumed = picker.Poll(ticket, 102, 5);
			REQUIRE_FALSE(consumed.has_value());
			CHECK(consumed.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Picking: unpolled cancellations do not exhaust the staging pool")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11) };
			PickTicket oldest;
			PickTicket newest;
			for (int cycle = 0; cycle < 4; ++cycle)
			{
				for (uint32_t slot = 0; slot < AsyncPicker::MaxPendingPicks; ++slot)
				{
					newest = Utils::QueueAsyncTestPick(picker, *image, table);
					if (cycle == 0 && slot == 0)
						oldest = newest;
				}
				picker.CancelAll();
				picker.CancelAll(); // Idempotent: no duplicate tombstones.
				gpu.GetDevice().WaitForIdle();
			}
			const auto fresh = Utils::QueueAsyncTestPick(picker, *image, table);
			const auto evicted = picker.Poll(oldest, 102, 4);
			REQUIRE_FALSE(evicted.has_value());
			CHECK(evicted.error().GetCode() == ErrorCode::NotFound);
			const auto cancelled = picker.Poll(newest, 102, 4);
			REQUIRE_FALSE(cancelled.has_value());
			CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
			CHECK(picker.Poll(newest, 102, 4).error().GetCode() == ErrorCode::NotFound);
			gpu.GetDevice().WaitForIdle();
			CHECK(Utils::ResolveAsyncTestPick(picker, fresh).Entity == UUID(11));
		}

		TEST_CASE("Picking: full queue and out-of-range pixels return located errors")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11) };
			const PickRequest valid{ .FrameIndex = 1, .ViewGeneration = 4 };
			for (const std::array badTable : { std::array{ UUID(11), UUID(11) }, std::array{ UUID(11), UUID{} } })
			{
				const auto invalid = picker.Request(*image, badTable, 4, valid);
				REQUIRE_FALSE(invalid.has_value());
				CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			}
			for (uint32_t axis = 0; axis < 2; ++axis)
			{
				PickRequest outside = valid;
				(axis == 0 ? outside.X : outside.Y) = 2;
				const auto invalid = picker.Request(*image, table, 4, outside);
				REQUIRE_FALSE(invalid.has_value());
				CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(invalid.error().GetLocation().JsonPointer == (axis == 0 ? "/x" : "/y"));
			}
			CHECK(picker.Request(*image, table, 0, valid).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(picker.Request(*image, table, 5, valid).error().GetCode() == ErrorCode::InvalidArgument);
			std::vector<PickTicket> tickets;
			for (uint32_t slot = 0; slot < AsyncPicker::MaxPendingPicks; ++slot)
				tickets.push_back(Utils::QueueAsyncTestPick(picker, *image, table));
			gpu.GetDevice().WaitForIdle(); // Completed, but still unconsumed: never silently overwritten.
			const auto full = picker.Request(*image, table, 4, valid);
			REQUIRE_FALSE(full.has_value());
			CHECK(full.error().GetCode() == ErrorCode::Conflict);
			CHECK(full.error().GetLocation().JsonPointer == "");
			CHECK(Utils::ResolveAsyncTestPick(picker, tickets.front()).Entity == UUID(11));
			const auto next = Utils::QueueAsyncTestPick(picker, *image, table);
			CHECK(next.Value > tickets.back().Value);
			picker.CancelAll();
			gpu.GetDevice().WaitForIdle();
		}

		TEST_CASE("Picking: returns the UUID at known pixels")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11), UUID(22) };
			std::array<PickTicket, 4> tickets{};
			for (uint32_t index = 0; index < 4; ++index)
				tickets[index] = Utils::QueueAsyncTestPick(picker, *image, table, index % 2, index / 2);
			gpu.GetDevice().WaitForIdle();
			const std::array expected{ UUID(11), UUID(22), UUID{}, UUID{} };
			const std::array<uint32_t, 4> ids{ 1, 2, 0, 99 };
			for (size_t index = 0; index < tickets.size(); ++index)
			{
				const auto result = Utils::ResolveAsyncTestPick(picker, tickets[index]);
				CHECK(result.Entity == expected[index]);
				CHECK(result.PickId == ids[index]);
			}
		}

		TEST_CASE("Picking: polling never waits and does not map unfinished submissions")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto image = Utils::MakeAsyncTestImage(gpu.GetDevice());
			AsyncPicker picker(gpu.GetDevice());
			const std::array table{ UUID(11) };
			const auto ticket = Utils::QueueAsyncTestPick(picker, *image, table);
			for (uint64_t frame : { 100ULL, 101ULL })
			{
				const auto pending = picker.Poll(ticket, frame, 4);
				REQUIRE(pending.has_value());
				CHECK_FALSE(pending->has_value());
			}
			gpu.GetDevice().WaitForIdle();
			CHECK_FALSE(picker.Poll(ticket, 101, 4)->has_value());
			CHECK(Utils::ResolveAsyncTestPick(picker, ticket).Entity == UUID(11));
			const auto backwards = Utils::QueueAsyncTestPick(picker, *image, table);
			CHECK(picker.Poll(backwards, 99, 4).error().GetCode() == ErrorCode::Cancelled);
			const auto nearLimit = Utils::QueueAsyncTestPick(picker, *image, table, 0, 0, std::numeric_limits<uint64_t>::max() - 1);
			gpu.GetDevice().WaitForIdle();
			const auto last = picker.Poll(nearLimit, std::numeric_limits<uint64_t>::max(), 4);
			REQUIRE(last.has_value());
			CHECK_FALSE(last->has_value()); // No overflow of frame + 2.
			picker.CancelAll();
		}

		TEST_CASE("Picking: unsubmitted frames and mismatched image identities are refused")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { 8, 8 });
			REQUIRE(renderer.has_value());
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.ViewportWidth = snapshot.Camera.ViewportHeight = 8;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 5.0f, 0.1f, 100.0f, 8, 8);
			snapshot.Flags = RenderViewFlags::Picking;
			snapshot.FrameIndex = 7;
			snapshot.SceneRevision = 19;
			PickRequest request{ .FrameIndex = 7, .SceneRevision = 19, .ViewGeneration = (*renderer)->GetViewGeneration() };
			const auto initial = (*renderer)->RequestPick(request);
			REQUIRE_FALSE(initial.has_value());
			CHECK(initial.error().GetCode() == ErrorCode::InvalidState);
			auto commands = gpu.GetDevice().CreateCommandList();
			REQUIRE(commands.has_value());
			(*commands)->open();
			const auto recorded = (*renderer)->Render(**commands, snapshot);
			const auto unsubmitted = (*renderer)->RequestPick(request);
			(*commands)->close();
			(*renderer)->OnSubmitted(7, gpu.GetDevice().ExecuteCommandList(**commands));
			REQUIRE(recorded.has_value());
			REQUIRE_FALSE(unsubmitted.has_value());
			CHECK(unsubmitted.error().GetCode() == ErrorCode::InvalidState);
			for (int field = 0; field < 3; ++field)
			{
				PickRequest mismatch = request;
				if (field == 0)
					++mismatch.FrameIndex;
				if (field == 1)
					++mismatch.SceneRevision;
				if (field == 2)
					++mismatch.ViewGeneration;
				const auto refused = (*renderer)->RequestPick(mismatch);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Conflict);
			}
			const auto accepted = (*renderer)->RequestPick(request);
			REQUIRE(accepted.has_value());
			gpu.GetDevice().WaitForIdle();
			const auto ready = (*renderer)->PollPick(*accepted, 9);
			REQUIRE(ready.has_value());
			REQUIRE(ready->has_value());
			CHECK((**ready).FrameIndex == 7);
			CHECK((**ready).SceneRevision == 19);
			CHECK_FALSE((**ready).Entity.IsValid());
			snapshot.FrameIndex = 8;
			(*commands)->open();
			const auto nextRecorded = (*renderer)->Render(**commands, snapshot);
			const auto priorImage = (*renderer)->RequestPick(request);
			(*commands)->close();
			(*renderer)->OnSubmitted(8, gpu.GetDevice().ExecuteCommandList(**commands));
			REQUIRE(nextRecorded.has_value());
			REQUIRE_FALSE(priorImage.has_value());
			CHECK(priorImage.error().GetCode() == ErrorCode::InvalidState);
			const auto superseded = (*renderer)->RequestPick(request);
			REQUIRE_FALSE(superseded.has_value());
			CHECK(superseded.error().GetCode() == ErrorCode::Conflict);
			gpu.GetDevice().WaitForIdle();
		}

		TEST_CASE("Picking: camera scene and unavailable-view changes invalidate pending clicks")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { 8, 8 });
			REQUIRE(renderer.has_value());
			for (int reason = 0; reason < 3; ++reason)
			{
				const uint64_t before = (*renderer)->GetViewGeneration();
				CHECK(before != 0);
				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				snapshot.Flags = RenderViewFlags::Picking;
				snapshot.FrameIndex = static_cast<uint64_t>(reason) * 3 + 1;
				snapshot.SceneRevision = 19;
				snapshot.Camera.ViewportWidth = snapshot.Camera.ViewportHeight = 8;
				snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 5.0f, 0.1f, 100.0f, 8, 8);
				auto commands = gpu.GetDevice().CreateCommandList();
				REQUIRE(commands.has_value());
				(*commands)->open();
				const auto recorded = (*renderer)->Render(**commands, snapshot);
				(*commands)->close();
				(*renderer)->OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**commands));
				REQUIRE(recorded.has_value());
				const PickRequest request{ .FrameIndex = snapshot.FrameIndex, .SceneRevision = 19, .ViewGeneration = before };
				const auto ticket = (*renderer)->RequestPick(request);
				REQUIRE(ticket.has_value());
				(*renderer)->CancelPicks(); // Host calls this for each of the three invalidations.
				CHECK((*renderer)->GetViewGeneration() > before);
				const auto cancelled = (*renderer)->PollPick(*ticket, snapshot.FrameIndex + 2);
				REQUIRE_FALSE(cancelled.has_value());
				CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
				const auto consumed = (*renderer)->PollPick(*ticket, snapshot.FrameIndex + 2);
				REQUIRE_FALSE(consumed.has_value());
				CHECK(consumed.error().GetCode() == ErrorCode::NotFound);
				gpu.GetDevice().WaitForIdle();
			}
			const uint64_t before = (*renderer)->GetViewGeneration();
			REQUIRE((*renderer)->Resize(8, 8).has_value());
			CHECK((*renderer)->GetViewGeneration() == before);
			REQUIRE((*renderer)->Resize(16, 8).has_value());
			CHECK((*renderer)->GetViewGeneration() > before);
			gpu.GetDevice().WaitForIdle();
		}
	}

}
