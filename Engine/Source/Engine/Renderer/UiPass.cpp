#include "EnginePCH.h"
#include "Engine/Renderer/UiPass.h"

// The Ui pass (UiPass.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md decision 7) that stream C
// implements.

namespace Engine {

	UiPassPlan PlanUiPass(const UiDrawList& /*list*/, AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<uint32_t> RecordUiPass(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*quadBindings*/, PassBindingCache& /*textBindings*/,
		const UiPassInputs& /*inputs*/, RenderPassCounters& /*counters*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the Ui pass (Docs/Decisions/0033-m22-contract.md decision 7, stream C)");
	}

}
