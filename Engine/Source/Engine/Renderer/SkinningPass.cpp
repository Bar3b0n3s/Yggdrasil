#include "EnginePCH.h"
#include "Engine/Renderer/SkinningPass.h"

// The Skinning pass (SkinningPass.h). The class's construction is the contract's; its creation, layout, buffer descriptions
// and recording are contract stubs of M23's C23.3, implemented by stream C (Docs/Decisions/0034-m23-contract.md decision 27):
// until then no pipeline set creates the pass and nothing is deformed.

namespace Engine {

	struct SkinningPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
	};

	SkinningPass::SkinningPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkinningPass::~SkinningPass() = default;

	Result<Scope<SkinningPass>> SkinningPass::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 27, stream C)");
	}

	std::vector<PipelineLayoutDescription> SkinningPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::BufferDesc SkinningPass::GetVerticesDesc(uint32_t /*vertexCount*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::BufferDesc SkinningPass::GetPreviousPositionsDesc(uint32_t /*vertexCount*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::BufferDesc SkinningPass::GetPaletteDesc(uint32_t /*jointCount*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::BufferDesc SkinningPass::GetMorphWeightsDesc()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status SkinningPass::Record(nvrhi::ICommandList& /*commandList*/, RenderRecordingContext& /*recording*/, PassBindingCache& /*bindings*/,
		const SkinningPassInputs& /*inputs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 27, stream C)");
	}

}
