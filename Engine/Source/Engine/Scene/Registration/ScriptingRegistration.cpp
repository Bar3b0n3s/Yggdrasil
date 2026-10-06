#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <optional>
#include <string>
#include <string_view>

// The Scripting category (Docs/Decisions/0006-m3-decisions.md, decisions 6 and 9; Architecture §5.3, §11.2): the Script
// component, whose Fields map holds overrides of the script's declared fields, each a Variant resolved against the
// script's field schema.

namespace Engine {

	namespace Utils {

		// The name of ScriptComponent::Script in the registry and in files, which the resolver reads from a JSON owner.
		constexpr std::string_view ScriptHandleFieldName = "Script";

		// The Script handle of the owner of a Fields value: from the ScriptComponent object or, without one (the JSON of an
		// AddComponent prefab override), from its "Script" member in the file spelling (a 16-digit hex string, null for no
		// script; an absent member reads as the default, no script).
		static Result<UUID> ReadScriptHandle(const ResolveContext& context)
		{
			if (context.Owner != nullptr)
				return static_cast<const ScriptComponent*>(context.Owner)->Script.GetHandle();
			if (context.OwnerJson == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "script field '{}' resolves only with its Script component object or JSON", context.Key);

			const std::optional<JsonReader> member = context.OwnerJson->FindMember(ScriptHandleFieldName);
			if (!member.has_value() || member->IsNull())
				return UUID();
			return member->ReadUUID();
		}

		// The VariantSchemaResolver of ScriptComponent.Fields (§5.4): the field `Key` declared by the assigned script,
		// looked up through ResolveContext::Schemas (ScriptImporter's schemas from M13, a fixture source in tests).
		static Result<const FieldInfo*> ResolveScriptField(const ResolveContext& context)
		{
			if (context.OwnerType == nullptr || context.OwnerType->GetType().GetKey() != TypeKeyOf<ScriptComponent>())
				return MakeError(ErrorCode::InvalidArgument, "script field '{}' resolves only on a Script component (owner type '{}')", context.Key,
					context.OwnerType != nullptr ? context.OwnerType->GetName() : std::string("none"));

			ENGINE_TRY_ASSIGN(const UUID script, ReadScriptHandle(context));
			if (!script.IsValid())
				return MakeError(ErrorCode::NotFound, "script field '{}' cannot be resolved: no script is assigned", context.Key);
			if (context.Schemas == nullptr)
				return MakeError(ErrorCode::NotFound, "script field '{}' cannot be resolved: no script field schemas are available", context.Key);
			return context.Schemas->FindField(script, context.Key);
		}

	}

	void RegisterScriptingComponents(TypeRegistry& registry)
	{
		RegisterComponent<ScriptComponent>(registry, "Script", "Attaches a Behaviour script to the entity (one per entity).")
			.Category("Scripting")
			.Version(1)
			.Flags(ComponentFlags::NoShortcut)
			.Field(Utils::ScriptHandleFieldName, &ScriptComponent::Script, "The Behaviour script to run.")
			.VariantField("Fields", &ScriptComponent::Fields,
				"Overrides of the script's declared fields by name, each checked against the field's declared type; an unknown "
				"or mismatching override is kept and reported, and the field uses its declared default.",
				&Utils::ResolveScriptField)
			.Field("ExecutionOrder", &ScriptComponent::ExecutionOrder,
				"The script's place in callback order: lower values run first, ties in canonical entity order.");
	}

}
