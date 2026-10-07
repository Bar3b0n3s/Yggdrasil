#include "EditorPCH.h"
#include "EditorCore/Automation/ComponentMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		static ComponentSummary MakeComponentSummary(const ComponentInfo& info)
		{
			ComponentSummary summary;
			summary.Name = info.GetName();
			summary.Description = info.GetDescription();
			summary.Category = info.GetCategory();
			summary.Removable = info.HasFlag(ComponentFlags::Removable);
			summary.UniquePerScene = info.HasFlag(ComponentFlags::UniquePerScene);
			summary.EntityLevel = info.HasFlag(ComponentFlags::EntityLevel);
			summary.Hidden = info.HasFlag(ComponentFlags::Hidden);
			for (const ComponentInfo* required : info.GetRequires())
				summary.Requires.push_back(required->GetName());
			for (const ComponentInfo* excluded : info.GetExcludes())
				summary.Excludes.push_back(excluded->GetName());
			return summary;
		}

		// The enumerator names of an Enum field, or of the elements of an array of enums.
		static std::vector<std::string> GetEnumValues(const TypeInfo& type)
		{
			const TypeInfo* enumType = type.GetKind() == FieldType::Array ? type.GetElement() : &type;
			std::vector<std::string> names;
			if (enumType == nullptr || enumType->GetKind() != FieldType::Enum || enumType->GetEnum() == nullptr)
				return names;
			for (const EnumEntry& entry : enumType->GetEnum()->GetEntries())
				names.push_back(entry.Name);
			return names;
		}

		static ComponentFieldSummary MakeFieldSummary(const FieldInfo& field, const Json& defaults)
		{
			const FieldMeta& meta = field.GetMeta();
			ComponentFieldSummary summary;
			summary.Name = field.GetName();
			summary.Description = field.GetDescription();
			summary.Type = std::string(FieldTypeToString(field.GetKind()));
			summary.Unit = meta.Unit;
			// Virtual fields are computed from the entity (Transform.WorldPosition): they have no default of their own.
			const auto defaultValue = defaults.find(field.GetName());
			if (defaultValue != defaults.end())
				summary.Default = VariantValue(defaultValue.value());
			if (meta.Min.has_value())
				summary.Min = VariantValue(Json(*meta.Min));
			if (meta.Max.has_value())
				summary.Max = VariantValue(Json(*meta.Max));
			summary.EnumValues = GetEnumValues(field.GetType());
			summary.AssetType = meta.AssetFilter;
			summary.ReadOnly = field.IsReadOnly();
			summary.Virtual = meta.Virtual;
			summary.Serialized = meta.Serialized;
			return summary;
		}

	}

	namespace Automation {

		Result<ComponentListResult> ComponentList(EditorMethodContext& context, const NoParams& /*params*/)
		{
			ComponentListResult result;
			for (const ComponentInfo* info : context.GetEditor().GetTypeRegistry().GetComponents())
			{
				if (info->HasFlag(ComponentFlags::Serializable))
					result.Components.push_back(Utils::MakeComponentSummary(*info));
			}
			return result;
		}

		Result<ComponentSchemaResult> ComponentSchema(EditorMethodContext& context, const ComponentSchemaParams& params)
		{
			const TypeRegistry& registry = context.GetEditor().GetTypeRegistry();
			const ComponentInfo* info = registry.FindComponent(params.Name);
			if (info == nullptr || !info->HasFlag(ComponentFlags::Serializable))
			{
				std::vector<std::string> suggestions = registry.SuggestComponentNames(params.Name);
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/name", std::format("no component '{}'", params.Name),
					suggestions.empty() ? std::string("component.list lists every component") : MakeDidYouMeanHint(suggestions)));
			}

			ComponentSchemaResult result;
			result.Component = Utils::MakeComponentSummary(*info);
			const Json defaults = info->MakeDefaultJson();
			for (const Scope<FieldInfo>& field : info->GetFields())
				result.Fields.push_back(Utils::MakeFieldSummary(*field, defaults));
			result.Schema = VariantValue(JsonSchema::ForStruct(*info));
			return result;
		}

	}

	void RegisterComponentMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ComponentSummary>("ComponentSummary", "A component type: its name, description, category, flags and relations.")
			.Field("name", &ComponentSummary::Name, "The component's registry name, such as \"RigidBody\".")
			.Field("description", &ComponentSummary::Description, "What the component does.")
			.Field("category", &ComponentSummary::Category, "\"Core\", \"Rendering\", \"Physics\", \"Audio\" or \"Scripting\".")
			.Field("removable", &ComponentSummary::Removable, "Whether it may be removed from an entity.")
			.Field("uniquePerScene", &ComponentSummary::UniquePerScene, "Whether at most one entity of a scene may have it.")
			.Field("entityLevel", &ComponentSummary::EntityLevel,
				"Set through entity.create and entity.update members (name, tags, parent), not as a component.")
			.Field("hidden", &ComponentSummary::Hidden, "Maintained by the engine (prefab links): readable, never written by automation.")
			.Field("requires", &ComponentSummary::Requires, "The components an entity must also have.")
			.Field("excludes", &ComponentSummary::Excludes, "The components an entity must not have with it.");

		registry.Struct<ComponentListResult>("ComponentListResult", "Every serializable component type, in registry order.")
			.Field("components", &ComponentListResult::Components, "The components, in the order scene files list them.");

		registry.Struct<ComponentSchemaParams>("ComponentSchemaParams", "The params of component.schema.")
			.Field("name", &ComponentSchemaParams::Name, "The component's registry name, such as \"RigidBody\".");

		registry.Struct<ComponentFieldSummary>("ComponentFieldSummary", "One field of a component, as an agent sets it.")
			.Field("name", &ComponentFieldSummary::Name, "The field's registry name, such as \"Mass\".")
			.Field("description", &ComponentFieldSummary::Description, "What the field does.")
			.Field("type", &ComponentFieldSummary::Type, "The field's value type, such as \"Float\", \"Vec3\", \"Enum\" or \"AssetRef\".")
			.Field("unit", &ComponentFieldSummary::Unit, "The unit, such as \"kg\", \"m\" or \"deg\"; empty when none.")
			.Field("default", &ComponentFieldSummary::Default, "The default value's JSON; null for a virtual field.")
			.Field("min", &ComponentFieldSummary::Min, "The inclusive minimum of each numeric component; null when unbounded.")
			.Field("max", &ComponentFieldSummary::Max, "The inclusive maximum of each numeric component; null when unbounded.")
			.Field("enumValues", &ComponentFieldSummary::EnumValues, "The canonical enumerator names of an enum field.")
			.Field("assetType", &ComponentFieldSummary::AssetType, "The asset type an AssetRef accepts; empty for any.")
			.Field("readOnly", &ComponentFieldSummary::ReadOnly, "Readable but never written by automation.")
			.Field("virtual", &ComponentFieldSummary::Virtual, "Accepted on writes but never saved (Transform.WorldPosition).")
			.Field("serialized", &ComponentFieldSummary::Serialized, "Written to scene files.");

		registry.Struct<ComponentSchemaResult>("ComponentSchemaResult", "A component type described for agents.")
			.Field("component", &ComponentSchemaResult::Component, "The component type.")
			.Field("fields", &ComponentSchemaResult::Fields, "Every field, in registry order.")
			.Field("schema", &ComponentSchemaResult::Schema, "The JSON Schema (2020-12) of the component's serialized object.");
	}

	void RegisterComponentMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "component.list",
				.Description = "Lists every serializable component type in registry order, with its category, flags, requirements and exclusions.",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "List the components.", .Params = Json::object() } },
			},
			&Automation::ComponentList);

		Json example = Json::object();
		example["name"] = "RigidBody";
		methods.Add(
			{
				.Name = "component.schema",
				.Description = "Describes one component type: every field with its type, unit, range, default and enum values, and the JSON "
							   "Schema of the component.",
				.RequiredParams = { "name" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Describe the RigidBody component.", .Params = example } },
			},
			&Automation::ComponentSchema);
	}

}
