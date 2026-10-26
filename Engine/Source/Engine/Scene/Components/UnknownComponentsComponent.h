#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/NativeComponentName.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	// One component of a scene or prefab file whose name the registry does not know, kept verbatim (Architecture §6:
	// unknown components warn and are preserved on save, for forward compatibility), or a native component (decision 27),
	// held here as its JSON whether or not its module is loaded.
	struct UnknownComponentData
	{
		std::string Name;     // the key under "Components"
		uint32_t Version = 0; // its "ComponentVersions" entry; 0 when the file lists none
		VariantValue Data;    // the component's JSON value, exactly as read
	};

	// Holds an entity's unknown and native components so that load -> save keeps them (SceneSerializer). Not a reflected
	// component: invisible to scripts and the inspector; automation's scene reads report the native ones as held here
	// (Automation/Methods/SharedMethodSupport.h), and the native.* methods reach them through the native layer
	// (Native/NativeComponents.h). Copied with the entity by the serializer (play copy, undo snapshots, prefab
	// instantiation). The serializer writes these components after the known ones, in their stored order, and their
	// "ComponentVersions" entries after the known ones likewise, so a file with unknown components last re-saves
	// byte-identically. Canonicalizing an entity's native components (CanonicalizeNativeComponents) sorts them by name ahead
	// of its unknown ones.
	struct UnknownComponentsComponent
	{
		std::vector<UnknownComponentData> Components;
	};

}
