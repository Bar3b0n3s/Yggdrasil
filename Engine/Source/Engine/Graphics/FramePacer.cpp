#include "EnginePCH.h"
#include "Engine/Graphics/FramePacer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream B (swapchain, present, frame pacing, fault handling) implements the bounded
// waits of Architecture §8.1 (pollEventQuery, vkWaitSemaphores slices on the graphics queue's timeline, GpuHang after
// 10 s) and the frame slots.

namespace Engine {

	GpuWaitResult RunBoundedGpuWait(const std::function<GpuWaitSliceResult()>& /*slice*/, uint32_t /*maxSlices*/)
	{
		ENGINE_CONTRACT_STUB();
		return GpuWaitResult::Failed;
	}

	void WaitForSubmission(GraphicsDevice& /*device*/, uint64_t submissionID, std::string_view context)
	{
		ENGINE_CONTRACT_STUB();
		FatalError(FatalErrorKind::Gpu, std::format("{}: waiting for submission {} is not implemented yet", context, submissionID));
	}

	FramePacer::FramePacer(GraphicsDevice& /*device*/, uint32_t framesInFlight)
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(framesInFlight >= 1, "FramePacer needs at least one frame in flight");
	}

	FramePacer::~FramePacer() = default;

	void FramePacer::BeginFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	void FramePacer::EndFrame(uint64_t /*lastSubmissionID*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	uint32_t FramePacer::GetFrameSlot() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint64_t FramePacer::GetFrameIndex() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t FramePacer::GetFramesInFlight() const
	{
		ENGINE_CONTRACT_STUB();
		return 1;
	}

	std::string_view GpuWaitResultToString(GpuWaitResult result)
	{
		switch (result)
		{
			case GpuWaitResult::Signaled:   return "Signaled";
			case GpuWaitResult::Hang:       return "Hang";
			case GpuWaitResult::DeviceLost: return "DeviceLost";
			case GpuWaitResult::Failed:     return "Failed";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuWaitResult {}", std::to_underlying(result));
		return "Unknown";
	}

}
