#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Transient render targets keyed by their TextureDesc (Architecture §8.2: "Pass targets come from RenderTargetPool
// keyed by TextureDesc"). The pool owns every target (§8.14 item 1); a pass holds the handle it acquired for as long as
// it uses the target, and a target nobody besides the pool references (GpuResourceTracker::HasOtherReferences(target,
// 1), which discounts the tracker's own reference, so reuse behaves the same in every configuration) is free for the
// next Acquire with an equal key. Reusing a target while a frame that used it is still in flight is safe: the GPU
// orders the work of one queue, and NVRHI's barriers order the accesses.
//
// Releasing one is not: NVRHI's command lists do not reference textures used only by clears (ADR 0009 decision 31), so
// an unretired submission that cleared a target does not count as a reference. The pool therefore stamps each target
// with the last submission of the frames that acquired it (EndFrame) and releases a free target only once the GPU has
// completed that submission (GraphicsDevice::GetCompletedSubmissionID), never one acquired since the last EndFrame.

namespace Engine {

	class GraphicsDevice;

	// Not copyable or movable; main thread only (§4.11).
	class RenderTargetPool
	{
	public:
		// Frames a free target is kept before EndFrame releases it.
		static constexpr uint32_t DefaultKeepFrames = 3;

		// `device` is a documented back-reference and must outlive the pool.
		explicit RenderTargetPool(GraphicsDevice& device, uint32_t keepFrames = DefaultKeepFrames);
		// Releases every target; acquired handles still held elsewhere stay valid (NVRHI reference counting).
		~RenderTargetPool();

		RenderTargetPool(const RenderTargetPool&) = delete;
		RenderTargetPool& operator=(const RenderTargetPool&) = delete;

		// A texture created with `desc`: a free pooled target whose desc matches in every field that affects creation
		// (dimension, size, depth, array size, mips, sample count and quality, format, the usage flags, the initial state,
		// keepInitialState, and the clear value), otherwise a new one created through GraphicsDevice::CreateTexture. The
		// debug name does not take part in matching, and a reused target keeps the name it was created with. Errors: Gpu
		// when the creation fails (render targets are created at startup or resize, so callers treat it as
		// FatalError(OutOfMemory), §8.14 item 7).
		[[nodiscard]] Result<nvrhi::TextureHandle> Acquire(const nvrhi::TextureDesc& desc);

		// Once per frame after the frame's submission: stamps the targets acquired during the frame, or still held, with
		// the device's last submission, ages the free targets, and releases those free for more than keepFrames frames
		// whose last submission has completed.
		void EndFrame();

		// Releases every free target whose last submission has completed (on resize, when every size changes). A target
		// acquired since the last EndFrame, or used by a frame still in flight, stays until a later EndFrame releases it.
		void ReleaseFree();

		// Targets the pool holds, free or acquired.
		[[nodiscard]] size_t GetTargetCount() const;
		// Targets someone besides the pool references (HasOtherReferences).
		[[nodiscard]] size_t GetAcquiredCount() const;
	private:
		// LastSubmission of a target acquired since the last EndFrame: its submission is not known yet.
		static constexpr uint64_t PendingSubmission = std::numeric_limits<uint64_t>::max();

		struct PooledTarget
		{
			nvrhi::TextureHandle Texture{};
			uint32_t IdleFrames = 0; // EndFrame calls since the target was last acquired or seen in use
			// The device's last submission at the EndFrame of the last frame that acquired or held the target.
			uint64_t LastSubmission = PendingSubmission;
		};

		// Whether nobody besides the pool references `target`.
		[[nodiscard]] bool IsFree(const PooledTarget& target) const;
		// Whether `target` may be released: free, and every submission that may have used it has completed.
		[[nodiscard]] bool IsReleasable(const PooledTarget& target, uint64_t completedSubmission) const;
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference
		uint32_t m_KeepFrames = DefaultKeepFrames;
		std::vector<PooledTarget> m_Targets; // in creation order, so Acquire's choice is deterministic
	};

}
