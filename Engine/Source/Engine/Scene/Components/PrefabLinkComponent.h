#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// Registry name "PrefabLink" (Architecture §5.3, §5.5): links an entity of a prefab instance, its root included, to its
	// entity in the prefab of its innermost instance. Hidden. InstanceRoot names that innermost instance's root (a root,
	// nested or not, names itself) and PrefabEntityID is the entity's ID in that root's prefab; a member's own ID is
	// Hash64(InstanceRoot, PrefabEntityID). InstanceRoot must name an entity of the same scene that has
	// PrefabInstanceComponent (checked by structural pre-validation, §6). Entities under an instance without this component
	// are user children and are preserved by prefab updates.
	//
	// OuterPrefabEntityID (M20, Docs/Decisions/0031-m20-contract.md decision 8) is set on a nested instance root only: its
	// ID in the prefab of the instance that encloses it, the nearest ancestor instance root R for which Hash64(R,
	// OuterPrefabEntityID) is the nested root's own ID. It is null on members, on outermost instance roots and on the roots
	// of instances a user placed under a member (which belong to no prefab); a set value without such an ancestor, or on an
	// entity without PrefabInstanceComponent, is SCENE_INCONSISTENT_PREFAB_LINK. Always written, null included.
	struct PrefabLinkComponent
	{
		UUID PrefabEntityID;
		UUID InstanceRoot;
		UUID OuterPrefabEntityID;
	};

}
