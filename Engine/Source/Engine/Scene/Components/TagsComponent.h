#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <vector>

namespace Engine {

	// Registry name "Tags" (Architecture §5.3): free-form gameplay tags ("Player", "Checkpoint"). Entity-level: serialized
	// as the entity key "Tags", reached by scripts through the Entity tag methods, no shortcut property. Every entity has it
	// (an empty list by default) so the key is always written. Tags are unique per entity and keep insertion order
	// (Entity::AddTag ignores a duplicate).
	struct TagsComponent
	{
		std::vector<std::string> Tags;
	};

}
