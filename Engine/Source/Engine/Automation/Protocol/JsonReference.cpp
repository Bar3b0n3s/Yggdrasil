#include "EnginePCH.h"
#include "Engine/Automation/Protocol/JsonReference.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements $ref substitution.

namespace Engine {

	Status SubstituteReferences(Json& /*value*/, std::span<const Json> /*results*/, size_t /*indexBase*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SubstituteReferences is an M4 contract stub");
	}

}
