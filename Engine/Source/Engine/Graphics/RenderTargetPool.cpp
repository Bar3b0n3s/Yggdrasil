#include "EnginePCH.h"
#include "Engine/Graphics/RenderTargetPool.h"

#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	namespace Utils {

		// Whether a texture created with `a` serves every use of one created with `b`: every field that affects creation
		// matches. The debug name does not take part.
		[[nodiscard]] static bool AreInterchangeable(const nvrhi::TextureDesc& a, const nvrhi::TextureDesc& b)
		{
			const bool clearValuesMatch = a.useClearValue == b.useClearValue && (!a.useClearValue || a.clearValue == b.clearValue);
			return a.dimension == b.dimension && a.width == b.width && a.height == b.height && a.depth == b.depth && a.arraySize == b.arraySize
				&& a.mipLevels == b.mipLevels && a.sampleCount == b.sampleCount && a.sampleQuality == b.sampleQuality && a.format == b.format
				&& a.defaultComponentMapping == b.defaultComponentMapping && a.isShaderResource == b.isShaderResource
				&& a.isRenderTarget == b.isRenderTarget && a.isUAV == b.isUAV && a.isTypeless == b.isTypeless
				&& a.isShadingRateSurface == b.isShadingRateSurface && a.sharedResourceFlags == b.sharedResourceFlags
				&& a.isVirtual == b.isVirtual && a.isTiled == b.isTiled && a.initialState == b.initialState
				&& a.keepInitialState == b.keepInitialState && clearValuesMatch;
		}

	}

	RenderTargetPool::RenderTargetPool(GraphicsDevice& device, uint32_t keepFrames)
		: m_Device(&device), m_KeepFrames(keepFrames)
	{
	}

	RenderTargetPool::~RenderTargetPool() = default;

	Result<nvrhi::TextureHandle> RenderTargetPool::Acquire(const nvrhi::TextureDesc& desc)
	{
		for (PooledTarget& target : m_Targets)
		{
			if (Utils::AreInterchangeable(target.Texture->getDesc(), desc) && IsFree(target))
			{
				target.IdleFrames = 0;
				target.LastSubmission = PendingSubmission;
				return target.Texture;
			}
		}

		ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, m_Device->CreateTexture(desc));
		m_Targets.push_back({ .Texture = texture, .IdleFrames = 0, .LastSubmission = PendingSubmission });
		return texture;
	}

	void RenderTargetPool::EndFrame()
	{
		// The frame's submission is the device's last one: the targets the frame acquired, or that someone still holds,
		// may have been used by it.
		const uint64_t lastSubmission = m_Device->GetLastSubmissionID();
		for (PooledTarget& target : m_Targets)
		{
			const bool isFree = IsFree(target);
			if (!isFree || target.LastSubmission == PendingSubmission)
				target.LastSubmission = lastSubmission;
			target.IdleFrames = isFree ? target.IdleFrames + 1 : 0;
		}
		if (m_Targets.empty())
			return;
		const uint64_t completed = m_Device->GetCompletedSubmissionID();
		std::erase_if(m_Targets, [this, completed](const PooledTarget& target)
		{
			return target.IdleFrames > m_KeepFrames && IsReleasable(target, completed);
		});
	}

	void RenderTargetPool::ReleaseFree()
	{
		if (m_Targets.empty())
			return;
		const uint64_t completed = m_Device->GetCompletedSubmissionID();
		std::erase_if(m_Targets, [this, completed](const PooledTarget& target)
		{
			return IsReleasable(target, completed);
		});
	}

	size_t RenderTargetPool::GetTargetCount() const
	{
		return m_Targets.size();
	}

	size_t RenderTargetPool::GetAcquiredCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(m_Targets, [this](const PooledTarget& target)
		{
			return !IsFree(target);
		}));
	}

	bool RenderTargetPool::IsFree(const PooledTarget& target) const
	{
		// The tracker's own reference is discounted, so the answer is the same in every configuration (GpuResourceTracker.h).
		return !m_Device->GetResourceTracker().HasOtherReferences(*target.Texture, 1);
	}

	bool RenderTargetPool::IsReleasable(const PooledTarget& target, uint64_t completedSubmission) const
	{
		return target.LastSubmission != PendingSubmission && target.LastSubmission <= completedSubmission && IsFree(target);
	}

}
