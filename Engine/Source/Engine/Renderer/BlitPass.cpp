#include "EnginePCH.h"
#include "Engine/Renderer/BlitPass.h"

namespace Engine {

	struct BlitPass::State
	{
	};

	BlitPass::BlitPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	BlitPass::~BlitPass() = default;

	Result<Scope<BlitPass>> BlitPass::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/, const nvrhi::FramebufferInfo& /*framebuffer*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the blit pass is not implemented yet (M7 stream B)");
	}

	Status BlitPass::Record(nvrhi::ICommandList& /*commandList*/, nvrhi::ITexture& /*source*/, nvrhi::IFramebuffer& /*framebuffer*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the blit pass is not implemented yet (M7 stream B)");
	}

	PipelineLayoutDescription BlitPass::GetLayoutDescription()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
