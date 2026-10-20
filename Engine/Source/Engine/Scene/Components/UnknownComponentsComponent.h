#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// The prefix of a native component's registry name ("Native.Spinner"; Docs/Decisions/0030-m19-contract.md decision 27):
	// a game's native module registers its component types under their own names and files and automation write them with
	// this prefix, so no native name can ever equal a built-in component's (built-in names never contain a dot).
	inline constexpr std::string_view NativeComponentPrefix = "Native.";

	// True for a registry name made of NativeComponentPrefix and a name: the components a native module registers. The scene
	// serializer keeps them as written, as it keeps unknown components, but never reports one as unknown (no
	// SCENE_UNKNOWN_COMPONENT, so no error under LoadOptions::StrictUnknowns either): whether the loaded module registers it is
	// the native layer's check (Native/NativeComponents.h, NATIVE_COMPONENT_UNREGISTERED). Pure.
	[[nodiscard]] constexpr bool IsNativeComponentName(std::string_view name)
	{
		return name.size() > NativeComponentPrefix.size() && name.starts_with(NativeComponentPrefix);
	}

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
	// component: invisible to scripts, automation and the inspector (native components are reached through the native layer,
	// Native/NativeComponents.h, and the native.* methods), copied with the entity by the serializer (play copy, undo
	// snapshots, prefab instantiation). The serializer writes these components after the known ones, in their stored order,
	// and their "ComponentVersions" entries after the known ones likewise, so a file with unknown components last re-saves
	// byte-identically. Canonicalizing an entity's native components (CanonicalizeNativeComponents) sorts them by name ahead
	// of its unknown ones.
	struct UnknownComponentsComponent
	{
		std::vector<UnknownComponentData> Components;
	};

}
