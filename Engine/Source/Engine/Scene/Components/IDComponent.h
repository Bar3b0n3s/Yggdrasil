#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// Registry name "ID" (Architecture §5.3): the entity's identity. Required, hidden, entity-level (serialized as the
	// entity key "ID"), read-only for scripts and automation. Set once at creation (Scene::CreateEntity draws it from the
	// scene's UUIDGenerator, loaders pass the file's ID) and never changed afterwards; the scene's UUID index depends on it.
	struct IDComponent
	{
		UUID ID;
	};

}
