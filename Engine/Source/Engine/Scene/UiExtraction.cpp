#include "EnginePCH.h"
#include "Engine/Scene/UiExtraction.h"

// The UI's render extraction (UiExtraction.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md
// decisions 7 and 9) that stream C (the draw items) and stream E (the annotations) implement.

namespace Engine {

	void AppendUiDrawItems(const Scene& /*scene*/, const UiLayoutTable& /*table*/, UiDrawList& /*list*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AppendUiRenderAnnotations(const Scene& /*scene*/, RenderSnapshot& /*snapshot*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
