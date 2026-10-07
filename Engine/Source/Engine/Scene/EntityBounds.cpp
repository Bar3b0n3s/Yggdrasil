#include "EnginePCH.h"
#include "Engine/Scene/EntityBounds.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements entity bounds.

namespace Engine {

	std::optional<Aabb> ComputeEntityWorldBounds(ConstEntity /*entity*/, AssetManager& /*assets*/, const EntityBoundsOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
