#include "EditorPCH.h"
#include "EditorCore/Lighting/LightingBake.h"

// lighting.bake's work (LightingBake.h): a contract stub of M26's C26.1, implemented by stream Q-A
// (Docs/Decisions/0037-m26-contract.md decision 8).

namespace Engine {

	Result<LightingBakeResult> BakeLighting(EditorContext& /*editor*/, OpenSceneId /*scene*/, const LightingBakeOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

}
