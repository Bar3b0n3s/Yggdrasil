#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <string>
#include <vector>

// component.* (Architecture §13.5): the reflection registry, described for agents. Conventions as in MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "ComponentSummary".
	struct ComponentSummary
	{
		std::string Name{};
		std::string Description{};
		std::string Category{};
		bool Removable = true;
		bool UniquePerScene = false;
		bool EntityLevel = false; // Name and Tags: set through entity.create and entity.update, not as components
		bool Hidden = false;      // engine-maintained (Prefab, PrefabLink): readable, never written by automation
		std::vector<std::string> Requires{};
		std::vector<std::string> Excludes{};
	};

	// component.list {}: every serializable component in registry order (the order files use).
	struct ComponentListResult
	{
		std::vector<ComponentSummary> Components{};
	};

	// component.schema {name}.
	struct ComponentSchemaParams
	{
		std::string Name{};
	};

	// Registry struct "ComponentFieldSummary": one field as an agent needs it (§13.5: fields, types, ranges, defaults, enum
	// values, docs).
	struct ComponentFieldSummary
	{
		std::string Name{};
		std::string Description{};
		std::string Type{};     // FieldTypeToString ("Vec3", "Enum", "AssetRef" ...)
		std::string Unit{};     // "kg", "m", "deg"; empty when none
		VariantValue Default{}; // the default value's JSON
		VariantValue Min{};     // null when unbounded
		VariantValue Max{};
		std::vector<std::string> EnumValues{}; // canonical names, for enums (and enum arrays)
		std::string AssetType{};               // the AssetFilter of an AssetRef; empty for any type
		bool ReadOnly = false;
		bool Virtual = false; // accepted on writes, never serialized (Transform.WorldPosition)
		bool Serialized = true;
	};

	struct ComponentSchemaResult
	{
		ComponentSummary Component{};
		std::vector<ComponentFieldSummary> Fields{};
		VariantValue Schema{}; // JsonSchema::ForStruct of the component
	};

	namespace Automation {

		[[nodiscard]] Result<ComponentListResult> ComponentList(EditorMethodContext& context, const NoParams& params);
		// component.schema. Errors: NotFound with "did you mean" suggestions.
		[[nodiscard]] Result<ComponentSchemaResult> ComponentSchema(EditorMethodContext& context, const ComponentSchemaParams& params);

	}

	void RegisterComponentMethodTypes(TypeRegistry& registry);

	// Registers component.list and component.schema: tools, read-only, AllowedInBatch. They describe the registry, which
	// exists without a project, but §12.1 limits the launcher state to session.*, rpc.discover, docs.get, project.create and
	// project.open, so they need an open project like every other method.
	void RegisterComponentMethods(MethodRegistry& methods);

}
