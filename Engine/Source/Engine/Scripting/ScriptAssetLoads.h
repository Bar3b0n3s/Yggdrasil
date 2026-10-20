#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The literal Assets.Load scan (M19, Docs/Decisions/0030-m19-contract.md decision 19): what a script asks Assets.Load for
// by a string literal, so an export packages it although no asset references it (Engine.pak's built-ins, decision 20;
// Export.PruneUnreferenced's Game.pak, decision 21), and where a script computes the reference instead, which the export
// cannot follow (the report's computed loads; project.validate's SCRIPT_COMPUTED_ASSET_LOAD). It reads the source with
// Luau's parser only (no compiler, no VM, no type checker), so it is pure, thread-safe and independent of the project.

namespace Engine {

	// The validation code of a computed Assets.Load (decision 21, Warning, not fixable): reported by project.validate, with
	// Export.PruneUnreferenced on, for each computed load of a Script asset that the export would package. Stream R adds it
	// to ProjectValidator's codes, with its fixture and §13.7's row.
	inline constexpr std::string_view ScriptComputedAssetLoadCode = "SCRIPT_COMPUTED_ASSET_LOAD";

	// One use of Assets.Load in a script.
	struct ScriptAssetLoad
	{
		// A literal load: the string literal's text, as Assets.Load would read it (a project path "Assets/Audio/Beep.sfx", a
		// sub-asset reference "Assets/Models/Ship.glb#mesh:0:Hull", an engine path "engine://Audio/Coin" or a 16-hex handle),
		// unvalidated: the export resolves it as Assets.Load does and leaves out one that resolves to nothing. Empty for a
		// computed load.
		std::string Reference{};
		bool Literal = false;
		uint32_t Line = 0;   // 1-based, of the call (a literal or computed load) or of the use (any other computed use)
		uint32_t Column = 0; // 1-based

		[[nodiscard]] bool operator==(const ScriptAssetLoad& other) const = default;
	};

	// Every use of Assets.Load in `source`, in source order. Only the global Assets counts (a local or a field named Assets
	// does not), and type annotations are not read:
	// - a literal load is a call Assets.Load(<string literal>), the literal being a plain string, a long string or an
	//   interpolated string without expressions, possibly in parentheses or under a type assertion, with no further
	//   argument;
	// - every other use is a computed load: Assets.Load called with any other argument or none, called as a method
	//   (Assets:Load), read as a value (local load = Assets.Load), Assets["Load"] or Assets[<expression>], and the global
	//   Assets used other than by a field name (local a = Assets; f(Assets)), since the scan cannot follow where such a value
	//   goes.
	// `chunkName` names the source in errors ("Assets/Scripts/Game.luau"). Errors: CompileFailed located at the first syntax
	// error (line and column), as ScriptCompiler reports it; Validation for invalid UTF-8.
	[[nodiscard]] Result<std::vector<ScriptAssetLoad>> FindScriptAssetLoads(std::string_view source, std::string_view chunkName);

}
