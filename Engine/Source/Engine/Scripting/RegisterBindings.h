#pragma once

#include "Engine/Core/Result.h"

#include <functional>

namespace Engine {

	class ScriptApiRegistry;
	class TypeRegistry;

	// Populates the one registry with every built-in §11.5 module/type, lifecycle callback and non-reflected enum,
	// then configure (when supplied) may append registrations before Freeze(types) derives fields and declarations.
	// The hook is synchronous and borrowed for this call; its error propagates without freezing. Pure/test/load-time availability,
	// RunModes and mutation policy are specified at each registration. Requires frozen types and an empty API registry;
	// returns InvalidState/Validation for invalid setup. Does not open a VM, bind a host or invoke gameplay.
	[[nodiscard]] Status RegisterBindings(ScriptApiRegistry& api, const TypeRegistry& types, std::function<Status(ScriptApiRegistry&)> configure = {});

}
