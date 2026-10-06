#pragma once

#include "Engine/Core/Base.h"

#include <string>

namespace Engine {

	// Registry name "Name" (Architecture §5.3): the entity's display name, used by entity paths ("/Game/Board") and
	// Scene.FindByName. Required, entity-level: serialized as the entity key "Name", reached by scripts as Entity.Name, no
	// shortcut property. Names need not be unique.
	struct NameComponent
	{
		std::string Name = "Entity";
	};

}
