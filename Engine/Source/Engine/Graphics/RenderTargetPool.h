#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>

// Transient render targets keyed by their TextureDesc (Architecture §8.2: "Pass targets come from RenderTargetPool
// keyed by TextureDesc"). The pool owns every target (§8.14 item 1); a pass holds the handle it acquired for as long as
// it uses the target, and a target nobody besides the pool references (GpuResourceTracker::HasOtherReferences(target,
// 1), which discounts the tracker's own reference, so reuse behaves the same in every configuration; an unretired
// submission that used the target still counts as a reference) is free for the next Acquire with an equal key.

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

		// Once per frame after the frame's submission: ages the free targets and releases those free for more than
		// keepFrames frames.
		void EndFrame();

		// Releases every free target (on resize, when every size changes).
		void ReleaseFree();

		// Targets the pool holds, free or acquired.
		[[nodiscard]] size_t GetTargetCount() const;
		// Targets someone besides the pool references (HasOtherReferences).
		[[nodiscard]] size_t GetAcquiredCount() const;
	};

}
