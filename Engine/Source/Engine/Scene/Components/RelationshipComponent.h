#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <vector>

namespace Engine {

	// Registry name "Relationship" (Architecture §5.2, §5.3): the hierarchy link. Required, hidden, entity-level; edited only
	// through hierarchy operations (Scene::SetParent, CreateEntity with a parent, DestroyEntity). Files store only "Parent"
	// (null for a root); the array order of entities defines sibling order, and Children is rebuilt on load. Children keeps
	// the explicit sibling order, which is part of the canonical order (§5.1).
	struct RelationshipComponent
	{
		UUID Parent;
		std::vector<UUID> Children;
	};

}
