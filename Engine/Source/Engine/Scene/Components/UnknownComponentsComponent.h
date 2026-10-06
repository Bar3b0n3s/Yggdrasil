#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	// One component of a scene or prefab file whose name the registry does not know, kept verbatim (Architecture §6:
	// unknown components warn and are preserved on save, for forward compatibility).
	struct UnknownComponentData
	{
		std::string Name;     // the key under "Components"
		uint32_t Version = 0; // its "ComponentVersions" entry; 0 when the file lists none
		VariantValue Data;    // the component's JSON value, exactly as read
	};

	// Holds an entity's unknown components so that load -> save keeps them (SceneSerializer). Not a reflected component:
	// invisible to scripts, automation and the inspector, copied with the entity by the serializer (play copy, undo
	// snapshots, prefab instantiation). The serializer writes these components after the known ones, in their original
	// order, and their "ComponentVersions" entries after the known ones likewise, so a file with unknown components last
	// re-saves byte-identically.
	struct UnknownComponentsComponent
	{
		std::vector<UnknownComponentData> Components;
	};

}
