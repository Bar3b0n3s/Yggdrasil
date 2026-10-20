#include "EnginePCH.h"
#include "Engine/Renderer/UpscalePass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/RenderStats.h"

namespace Engine {

	struct UpscalePass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
	};

	UpscalePass::UpscalePass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	UpscalePass::~UpscalePass() = default;

	Result<Scope<UpscalePass>> UpscalePass::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 output stage contract (Docs/Decisions/0029-m18-contract.md decision 6, stream F)"));
	}

	std::vector<PipelineLayoutDescription> UpscalePass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::FramebufferInfo UpscalePass::GetColorFramebufferInfo()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::FramebufferInfo UpscalePass::GetDepthFramebufferInfo()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t UpscalePass::GetPipelineCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Status UpscalePass::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const UpscalePassInputs& /*inputs*/,
		RenderPassCounters& /*counters*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 output stage contract (Docs/Decisions/0029-m18-contract.md decision 6, stream F)"));
	}

}
