#include "EnginePCH.h"
#include "Engine/Renderer/SelectionPass.h"

namespace Engine {

	struct SelectionPass::State
	{
	};

	SelectionPass::SelectionPass(ConstructionKey)
	{
		ENGINE_CONTRACT_STUB();
	}

	SelectionPass::~SelectionPass() = default;

	Result<Scope<SelectionPass>> SelectionPass::Create(GraphicsDevice&, PipelineFactory&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	std::vector<PipelineLayoutDescription> SelectionPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::TextureDesc SelectionPass::GetMaskDesc(uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status SelectionPass::Record(nvrhi::ICommandList&, RenderRecordingContext&, PassBindingCache&, GpuResourceCache&, AssetManager&, const SelectionRenderInputs&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

}
