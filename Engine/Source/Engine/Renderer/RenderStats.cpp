#include "EnginePCH.h"
#include "Engine/Renderer/RenderStats.h"

namespace Engine {

	void RenderStatsHistory::PublishCpu(RenderStats)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RenderStatsHistory::PublishGpu(const GpuTimingFrame&)
	{
		ENGINE_CONTRACT_STUB();
	}

	const RenderStats& RenderStatsHistory::GetLatest() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Latest;
	}

	void RenderStatsHistory::Reset()
	{
		ENGINE_CONTRACT_STUB();
	}

	struct RenderRecordingContext::State
	{
	};

	RenderRecordingContext::RenderRecordingContext(RenderStats&, std::function<double()>, GpuProfiler*)
	{
		ENGINE_CONTRACT_STUB();
	}

	RenderRecordingContext::~RenderRecordingContext() = default;

	void RenderRecordingContext::BeginPass(std::string_view, nvrhi::ICommandList*)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RenderRecordingContext::EndPass(const RenderPassCounters&)
	{
		ENGINE_CONTRACT_STUB();
	}

}
