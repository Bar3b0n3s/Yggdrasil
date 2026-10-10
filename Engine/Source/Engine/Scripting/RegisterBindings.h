#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	class ScriptApiRegistry;
	class TypeRegistry;

	// Populates the one registry with every built-in §11.5 module/type, lifecycle callback and non-reflected enum,
	// then Freeze(types) derives component fields, shortcuts and declarations. Pure/test/load-time availability,
	// RunModes and mutation policy are specified at each registration. Requires frozen types and an empty API registry;
	// returns InvalidState/Validation for invalid setup. Does not open a VM, bind a host or invoke gameplay.
	// M13 contract returns Unsupported. Binder-file registration calls are added by this file's integration owner only
	// after those functions exist; there are no references to imaginary future Bindings/*.cpp symbols.
	[[nodiscard]] Status RegisterBindings(ScriptApiRegistry& api, const TypeRegistry& types);

}
