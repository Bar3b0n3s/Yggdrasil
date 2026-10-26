#pragma once

#include <string_view>

namespace Engine {

	// The prefix of a native component's registry name ("Native.Spinner"; Docs/Decisions/0030-m19-contract.md decision 27):
	// a game's native module registers its component types under their own names and files and automation write them with
	// this prefix, so no native name can ever equal a built-in component's (built-in names never contain a dot).
	inline constexpr std::string_view NativeComponentPrefix = "Native.";

	// True for a registry name made of NativeComponentPrefix and a name: the components a native module registers, which no
	// TypeRegistry holds. The scene serializer keeps them as written, as it keeps unknown components, but never reports one
	// as unknown (no SCENE_UNKNOWN_COMPONENT, so no error under LoadOptions::StrictUnknowns either): whether the loaded module
	// registers it is the native layer's check (Native/NativeComponents.h, NATIVE_COMPONENT_UNREGISTERED). Automation's
	// writes of registry components refuse one (MakeNativeComponentWriteError, Automation/Protocol/MethodRegistry.h). Pure.
	[[nodiscard]] constexpr bool IsNativeComponentName(std::string_view name)
	{
		return name.size() > NativeComponentPrefix.size() && name.starts_with(NativeComponentPrefix);
	}

}
