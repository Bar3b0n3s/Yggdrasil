#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"

#include <vector>

// Live nested prefabs (M20, Docs/Decisions/0031-m20-contract.md decision 8): the parts of PrefabInstantiator that
// Prefab::CreateFromEntity reads on a const scene.

namespace Engine {

	namespace Utils {

		// True when `entity` is a nested instance root: it has PrefabInstanceComponent and its PrefabLink names itself with
		// an OuterPrefabEntityID.
		[[nodiscard]] bool IsNestedInstanceRoot(ConstEntity entity);

		// PrefabInstantiator::GetInstanceLevels on a const entity: the instance roots `entity` belongs to, outermost first.
		// Errors: as GetInstanceLevels.
		[[nodiscard]] Result<std::vector<ConstEntity>> FindInstanceLevels(ConstEntity entity);

		// The overrides the outermost instance enclosing the nested instance root `nestedRoot` records within that nested
		// instance, moved onto it: their paths relative to it (an override of its own root at its prefab's root, except its
		// Name and Transform, which are its placement and stay with the enclosing instance), entity references to its own
		// entities mapped to their IDs in its prefab and references to the enclosing instance's other entities to their
		// IDs in the scene, sorted. What the nested instance records once it is outermost (PrefabInstantiator::Unpack of
		// the instance enclosing it, Prefab::CreateFromEntity of a subtree that leaves the enclosing instance out). Script
		// field values are moved verbatim. Errors: InvalidArgument when `nestedRoot` is not a nested instance root; those
		// of GetInstanceLevels.
		[[nodiscard]] Result<std::vector<PrefabOverride>> TakeNestedOverrides(ConstEntity nestedRoot);

	}

}
