#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// Registry name "PrefabLink" (Architecture §5.3, §5.5): links an instance member, the root included, to its prefab
	// entity. Hidden. PrefabEntityID is the prefab-local ID; the member's own ID is Hash64(InstanceRoot, PrefabEntityID).
	// InstanceRoot must name an entity of the same scene that has PrefabInstanceComponent (checked by structural
	// pre-validation, §6). Entities under an instance without this component are user children and are preserved by
	// prefab updates.
	struct PrefabLinkComponent
	{
		UUID PrefabEntityID;
		UUID InstanceRoot;
	};

}
