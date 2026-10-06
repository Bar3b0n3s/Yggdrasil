#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

// GPU-to-CPU copies of textures (Architecture §8.1 "Headless", §8.13): a staging texture with CpuAccessMode::Read,
// copyTexture, a bounded wait for the copy, mapStagingTexture. The same path serves editor screenshots,
// viewport.screenshot, editor.screenshot, golden tests, thumbnails and Runtime --screenshot-at.

namespace Engine {

	class GraphicsDevice;

	// Not copyable or movable; main thread only (§4.11).
	class Readback
	{
	public:
		// `device` is a documented back-reference and must outlive the readback.
		explicit Readback(GraphicsDevice& device);
		~Readback();

		Readback(const Readback&) = delete;
		Readback& operator=(const Readback&) = delete;

		// Copies mip `mipLevel` of array slice `arraySlice` of `texture` into an Image of the texture's format, rows top
		// first and tightly packed (the staging texture's row pitch is removed). The texture's prior writes are submitted
		// first: the copy goes into its own command list, executed with GraphicsDevice::ExecuteCommandList, and the call
		// waits for it with WaitForSubmission (bounded; a hang or device loss is fatal, FramePacer.h). Blocks the caller
		// until the pixels are on the CPU, so it is for screenshots and tests, never for per-frame work. Errors:
		// InvalidArgument for a depth or block-compressed format, a multisampled texture, or a mip or slice out of range;
		// Gpu when the staging texture or command list cannot be created, or mapping fails.
		[[nodiscard]] Result<Image> ReadTexture(nvrhi::ITexture& texture, uint32_t mipLevel = 0, uint32_t arraySlice = 0);
	};

}
