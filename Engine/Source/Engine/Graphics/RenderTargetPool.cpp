#include "EnginePCH.h"
#include "Engine/Graphics/RenderTargetPool.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the pool
// keyed by TextureDesc of Architecture §8.2.

namespace Engine {

	RenderTargetPool::RenderTargetPool(GraphicsDevice& /*device*/, uint32_t /*keepFrames*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	RenderTargetPool::~RenderTargetPool() = default;

	Result<nvrhi::TextureHandle> RenderTargetPool::Acquire(const nvrhi::TextureDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "RenderTargetPool::Acquire is not implemented yet");
	}

	void RenderTargetPool::EndFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	void RenderTargetPool::ReleaseFree()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t RenderTargetPool::GetTargetCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t RenderTargetPool::GetAcquiredCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
