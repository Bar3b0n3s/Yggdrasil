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
		// An interchangeable target that is not free may be referenced only through an object just the tracker still holds,
		// which Dist destroyed at its drop: the sweep releases those objects first, so the choice below (the first free one
		// in creation order) is Dist's. Only an Acquire that meets such a target pays for the sweep.
		const auto isAcquiredMatch = [this, &desc](const PooledTarget& target)
		{
			return Utils::AreInterchangeable(target.Texture->getDesc(), desc) && !IsFree(target);
		};
		if (std::ranges::any_of(m_Targets, isAcquiredMatch))
			SweepTracker();
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
		SweepTracker();
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
		SweepTracker();
		const uint64_t completed = m_Device->GetCompletedSubmissionID();
		std::erase_if(m_Targets, [this, completed](const PooledTarget& target)
		{
			return IsReleasable(target, completed);
		});
	}

	void RenderTargetPool::ReleaseAll()
	{
		// Only the pool's references go: no submission is asked about (a lost device answers nothing), and a holder's handle
		// keeps its target alive until the holder drops it.
		m_Targets.clear();
	}

	void RenderTargetPool::TrimUnused()
	{
		// ReleaseFree's rule, which ignores the targets' age: a free target goes once every submission that may have used it
		// has completed, and a target acquired since the last EndFrame (PendingSubmission) stays, since an open command list
		// may still clear it without NVRHI referencing it.
		ReleaseFree();
	}

	size_t RenderTargetPool::GetTargetCount() const
	{
		return m_Targets.size();
	}

	size_t RenderTargetPool::GetAcquiredCount()
	{
		SweepTracker();
		return static_cast<size_t>(std::ranges::count_if(m_Targets, [this](const PooledTarget& target)
		{
			return !IsFree(target);
		}));
	}

	void RenderTargetPool::SweepTracker()
	{
		m_Device->GetResourceTracker().Sweep();
	}

	bool RenderTargetPool::IsFree(const PooledTarget& target) const
	{
		// The tracker's own reference to the target is discounted; after a sweep, which releases what only the tracker
		// holds, the answer is the same in every configuration (the file comment).
		return !m_Device->GetResourceTracker().HasOtherReferences(*target.Texture, 1);
	}

	bool RenderTargetPool::IsReleasable(const PooledTarget& target, uint64_t completedSubmission) const
	{
		return target.LastSubmission != PendingSubmission && target.LastSubmission <= completedSubmission && IsFree(target);
	}

}
