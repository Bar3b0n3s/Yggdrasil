#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <span>

// Per-view asynchronous EntityId readback (§8.10). Owns a bounded pool of staging textures, command lists and event
// queries. Never uses Readback::ReadTexture, waits for the GPU, or reuses a slot before its submission completes.
// Main thread only; noncopyable. All returned values own their data; no ECS or Scene dependency.
namespace Engine {

	class GraphicsDevice;

	struct PickRequest
	{
		uint32_t X = 0; // integer framebuffer pixel, top-left origin, pixel centre (X + .5, Y + .5), as ComputeViewPixelRay
		uint32_t Y = 0;
		uint64_t FrameIndex = 0;
		uint64_t SceneRevision = 0;  // opaque host revision, echoed; renderer never interprets it
		uint64_t Sequence = 0;       // opaque host click sequence, echoed so a later click can supersede this one
		uint64_t ViewGeneration = 0; // clicked image generation, must match Request's nonzero viewGeneration
	};

	struct PickTicket
	{
		uint64_t Value = 0; // monotonically increasing in this picker; 0 invalid, never reused
		bool operator==(const PickTicket&) const = default;
	};

	struct PickResult
	{
		UUID Entity{}; // invalid UUID: background (id 0) or invalid table entry; no scene pointer is retained
		uint32_t PickId = 0;
		uint64_t FrameIndex = 0;
		uint64_t SceneRevision = 0;
		uint64_t Sequence = 0;
		uint64_t ViewGeneration = 0;
	};

	class AsyncPicker
	{
	public:
		static constexpr uint32_t MaxPendingPicks = 8;
		static constexpr uint32_t MinimumFrameDelay = 2;

		// device is a documented back-reference and outlives this object.
		explicit AsyncPicker(GraphicsDevice& device);
		// Host retires submissions or waits for idle before destruction (§8.14 shutdown). CancelAll is not a GPU barrier.
		~AsyncPicker();
		AsyncPicker(const AsyncPicker&) = delete;
		AsyncPicker& operator=(const AsyncPicker&) = delete;

		// Call AFTER submitting the rendering command list. Copies one R32_UINT pixel into an owned staging slot,
		// submits that copy, and records its submission id/event query. Copies pickTable now (PickId - 1 -> UUID).
		// Does not keep the source texture beyond the submitted command's lifetime. A table change cannot remap a result.
		// InvalidArgument: non-R32_UINT/non-2D/multisampled target, out-of-range pixel, generation 0/mismatch, invalid table
		// (duplicate/invalid UUID, too many entries). Conflict: all slots outstanding. Gpu: resource creation failure.
		[[nodiscard]] Result<PickTicket> Request(nvrhi::ITexture& entityIds, std::span<const UUID> pickTable,
			uint64_t viewGeneration, const PickRequest& request);

		// nullopt until both two host frames have elapsed and the copy's query/submission completed. Never maps an
		// unfinished slot. Once ready, returns and consumes the result. NotFound: unknown/consumed ticket.
		// Cancelled: ticket explicitly cancelled, generation changed, or current frame moved backwards; consumes ticket.
		// Gpu: mapping failure. Polls use GraphicsDevice completion/query APIs, never a spin loop or blocking wait.
		// The host MUST discard a result with an old Sequence/revision and re-resolve Entity in its current scene.
		[[nodiscard]] Result<std::optional<PickResult>> Poll(PickTicket ticket, uint64_t currentFrameIndex, uint64_t viewGeneration);

		// Marks all outstanding tickets cancelled, retaining in-flight GPU objects until retirement. Request and Poll
		// reclaim completed cancelled slots even if their tickets are never polled. Keep at most MaxPendingPicks cancellation
		// tombstones (oldest evicted): Poll returns Cancelled once while remembered, NotFound after eviction/consumption.
		// Resize/scene replacement and camera changes call this through SceneRenderer::CancelPicks.
		void CancelAll();
	private:
		struct State;
		Scope<State> m_State;
	};

}
