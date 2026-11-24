#include "EnginePCH.h"
#include "Engine/Renderer/WaterPass.h"

// The Water pass (M47, Docs/Decisions/0065-m47-water.md decision 3). C47 freezes the interface; W47.2's render stream
// implements it.

namespace Engine {

	struct WaterPass::State
	{
	};

	WaterPass::WaterPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	WaterPass::~WaterPass() = default;

	Result<Scope<WaterPass>> WaterPass::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/, nvrhi::IBindingLayout& /*forwardViewLayout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "WaterPass::Create is not implemented yet (M47's contract C47)");
	}

	std::vector<PipelineLayoutDescription> WaterPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t WaterPass::GetPipelineCount() const
	{
		return PipelineCount;
	}

	Status WaterPass::RecordSurface(nvrhi::ICommandList& /*commandList*/, GpuResourceCache& /*cache*/, WaterViewResources& /*resources*/,
		const WaterSurfaceInputs& /*inputs*/, RenderPassCounters& /*counters*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "WaterPass::RecordSurface is not implemented yet (M47's contract C47)");
	}

}
