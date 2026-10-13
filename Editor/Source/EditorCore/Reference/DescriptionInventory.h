#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <span>
#include <string>
#include <vector>

// Coverage gate 7 (Architecture §15.6 "every type, field, function, method, property and automation method has a
// non-empty description"), frozen by the M14 contract (Docs/Decisions/0022-m14-contract.md decision 6). Mode-independent
// and editor-only: the editor's registries are the superset of every mode's types, script API and automation methods, so
// the inventory lives in EditorCore and never reaches a Dist build. GenerateReferences (ReferenceGenerator.h) reuses it to
// refuse an empty description (§5.4). Pure apart from the registries it reads.

namespace Engine {

	class MethodRegistry;
	class ScriptApiRegistry;
	class TypeRegistry;

	// One documented thing.
	struct DescriptionEntry
	{
		// "type", "field", "enum value" (TypeRegistry); "script module", "script type", "script enum", "script enum value",
		// "script alias", "script member" (ScriptApiRegistry); "method" (MethodRegistry)
		std::string Kind{};
		std::string Name{};        // "Transform", "Transform.Position", "Tonemapper.AgX", "Physics.Raycast", "scene.tree"
		std::string Description{}; // as registered
	};

	// Every description of `types` (structs, components, enums, fields and enum entries), of `scripts` (modules, types,
	// enums, aliases and members) and of `methods` (each automation method), sorted by (Kind, Name). The registries are
	// frozen. Errors: InvalidState naming a registry that is not frozen.
	[[nodiscard]] Result<std::vector<DescriptionEntry>> CollectDescriptions(const TypeRegistry& types, const ScriptApiRegistry& scripts,
		const MethodRegistry& methods);

	// "<Kind> <Name>" for every entry whose description is empty or only whitespace, sorted: gate 7's Missing list.
	[[nodiscard]] std::vector<std::string> FindMissingDescriptions(std::span<const DescriptionEntry> entries);

}
