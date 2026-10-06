#include "EnginePCH.h"
#include "Engine/Graphics/GpuProfiler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream B (swapchain, present, frame pacing, fault handling) implements the pooled
// timer queries and markers of Architecture §8.2.

namespace Engine {

	GpuProfiler::GpuProfiler(GraphicsDevice& /*device*/, uint32_t framesInFlight)
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(framesInFlight >= 1, "GpuProfiler needs at least one frame in flight");
	}

	GpuProfiler::~GpuProfiler() = default;

	void GpuProfiler::BeginFrame(uint32_t /*frameSlot*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuProfiler::BeginScope(nvrhi::ICommandList& /*commandList*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuProfiler::EndScope(nvrhi::ICommandList& /*commandList*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::span<const GpuTimingSample> GpuProfiler::GetLastFrameSamples() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
