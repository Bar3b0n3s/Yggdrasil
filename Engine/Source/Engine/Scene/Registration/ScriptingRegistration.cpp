#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

// The Scripting category (Docs/Decisions/0006-m3-decisions.md, decisions 6 and 9; Architecture §5.3, §11.2): the Script
// component, an ordered list of slots (version 2, Docs/Decisions/0031-m20-contract.md decision 17), each slot's Fields map
// holding overrides of its own script's declared fields, each a Variant resolved against that script's field schema.

namespace Engine {

	namespace Utils {

		// The name of ScriptSlot::Script in the registry and in files, which the resolver reads from a JSON owner.
		constexpr std::string_view ScriptHandleFieldName = "Script";

		// The members of a version 1 Script component, which version 2 keeps inside its only slot.
		constexpr std::array<std::string_view, 3> ScriptSlotMemberNames = { "Script", "Fields", "ExecutionOrder" };

		// The Script handle of the owner of a Fields value: from the ScriptSlot object or, without one (the slot's JSON in a
		// file, in the whole-list value of a prefab override or in an AddComponent override), from its "Script" member in
		// the file spelling (a 16-digit hex string, null for no script; an absent member reads as the default, no script).
		static Result<UUID> ReadScriptHandle(const ResolveContext& context)
		{
			if (context.Owner != nullptr)
				return static_cast<const ScriptSlot*>(context.Owner)->Script.GetHandle();
			if (context.OwnerJson == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "script field '{}' resolves only with its script slot object or JSON", context.Key);

			const std::optional<JsonReader> member = context.OwnerJson->FindMember(ScriptHandleFieldName);
			if (!member.has_value() || member->IsNull())
				return UUID();
			return member->ReadUUID();
		}

		// The VariantSchemaResolver of ScriptSlot.Fields (§5.4): the field `Key` declared by the slot's script, looked up
		// through ResolveContext::Schemas (ScriptImporter's schemas from M13, a fixture source in tests).
		static Result<const FieldInfo*> ResolveScriptField(const ResolveContext& context)
		{
			if (context.OwnerType == nullptr || context.OwnerType->GetType().GetKey() != TypeKeyOf<ScriptSlot>())
				return MakeError(ErrorCode::InvalidArgument, "script field '{}' resolves only on a script slot (owner type '{}')", context.Key,
					context.OwnerType != nullptr ? context.OwnerType->GetName() : std::string("none"));

			ENGINE_TRY_ASSIGN(const UUID script, ReadScriptHandle(context));
			if (!script.IsValid())
				return MakeError(ErrorCode::NotFound, "script field '{}' cannot be resolved: no script is assigned to its slot", context.Key);
			if (context.Schemas == nullptr)
				return MakeError(ErrorCode::NotFound, "script field '{}' cannot be resolved: no script field schemas are available", context.Key);
			return context.Schemas->FindField(script, context.Key);
		}

		// The Script component from version 1 to 2 (Docs/Decisions/0031-m20-contract.md decision 17): its one script becomes
		// the list's only slot, {"Script", "Fields", "ExecutionOrder"} moved into it in their order, an absent member staying
		// absent there; any other member stays on the component, where the reader reports it as before. Errors: Validation
		// (located within the component) for a component that is not an object.
		static Status MigrateScriptV1(Json& component)
		{
			ENGINE_TRY(JsonReader(component).ExpectType(JsonType::Object));
			Json slot = Json::object();
			Json others = Json::object();
			for (auto member = component.begin(); member != component.end(); ++member)
			{
				if (std::ranges::find(ScriptSlotMemberNames, member.key()) != ScriptSlotMemberNames.end())
					slot[member.key()] = *member;
				else
					others[member.key()] = *member;
			}
			Json migrated = Json::object();
			migrated["Scripts"] = Json::array({ std::move(slot) });
			for (auto member = others.begin(); member != others.end(); ++member)
				migrated[member.key()] = std::move(*member);
			component = std::move(migrated);
			return {};
		}

	}

	void RegisterScriptingComponents(TypeRegistry& registry)
	{
		registry.Struct<ScriptSlot>("ScriptSlot", "One behaviour of a Script component.")
			.Field(Utils::ScriptHandleFieldName, &ScriptSlot::Script, "The Behaviour script.")
			.VariantField("Fields", &ScriptSlot::Fields, "Overrides of the script's declared fields, by name.", &Utils::ResolveScriptField)
			.Field("ExecutionOrder", &ScriptSlot::ExecutionOrder, "Callbacks run in (ExecutionOrder, canonical entity order, slot) order.");

		RegisterComponent<ScriptComponent>(registry, "Script", "Attaches Behaviour scripts to the entity, an ordered list of slots.")
			.Category("Scripting")
			.Version(2)
			.Flags(ComponentFlags::NoShortcut)
			.Migration(1, &Utils::MigrateScriptV1)
			.Field("Scripts", &ScriptComponent::Scripts, "The behaviours of the entity, in slot order; each slot is its own instance.");
	}

}
