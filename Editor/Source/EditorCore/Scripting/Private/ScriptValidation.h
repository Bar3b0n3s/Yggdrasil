#pragma once

#include "Engine/Asset/IScriptDiagnosticsProvider.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Detail {

		// Synchronous validator bridge. Keeps every Luau header in ScriptTypeChecker.cpp; no VM, reads or writes.
		// Reports syntax and literal Input.*Action*/GetAxis findings with authored source ranges. Local Input bindings,
		// comments and computed action names are excluded. Process scripting initialization precedes calls.
		[[nodiscard]] std::vector<ScriptDiagnostic> ValidateScriptSource(std::string_view file, std::string_view source,
			std::span<const std::string> inputActions);

	}

}
