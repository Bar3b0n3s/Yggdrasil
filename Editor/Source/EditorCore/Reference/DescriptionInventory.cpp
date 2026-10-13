#include "EditorPCH.h"
#include "EditorCore/Reference/DescriptionInventory.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Error.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>
#include <format>
#include <tuple>

namespace Engine {

	namespace Utils {

		static void AddDescription(std::vector<DescriptionEntry>& entries, std::string_view kind, std::string name, std::string_view description)
		{
			entries.push_back(DescriptionEntry{ .Kind = std::string(kind), .Name = std::move(name), .Description = std::string(description) });
		}

		static void AddStructDescriptions(std::vector<DescriptionEntry>& entries, const StructInfo& type)
		{
			AddDescription(entries, "type", type.GetName(), type.GetDescription());
			for (const Scope<FieldInfo>& field : type.GetFields())
				AddDescription(entries, "field", std::format("{}.{}", type.GetName(), field->GetName()), field->GetDescription());
		}

		static bool IsBlank(std::string_view text)
		{
			return text.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
		}

	}

	Result<std::vector<DescriptionEntry>> CollectDescriptions(const TypeRegistry& types, const ScriptApiRegistry& scripts, const MethodRegistry& methods)
	{
		if (!types.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "the type registry is not frozen; descriptions are collected from frozen registries");
		if (!scripts.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "the script API registry is not frozen; descriptions are collected from frozen registries");
		if (!methods.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "the method registry is not frozen; descriptions are collected from frozen registries");

		std::vector<DescriptionEntry> entries;
		for (const ComponentInfo* component : types.GetComponents())
			Utils::AddStructDescriptions(entries, *component);
		for (const StructInfo* type : types.GetStructs())
			Utils::AddStructDescriptions(entries, *type);
		for (const EnumInfo* enumeration : types.GetEnums())
		{
			Utils::AddDescription(entries, "type", enumeration->GetName(), enumeration->GetDescription());
			for (const EnumEntry& entry : enumeration->GetEntries())
				Utils::AddDescription(entries, "enum value", std::format("{}.{}", enumeration->GetName(), entry.Name), entry.Description);
		}

		for (const ScriptApiGroup& module : scripts.GetModules())
		{
			Utils::AddDescription(entries, "script module", module.Name, module.Description);
			for (const ScriptApiMember& member : module.Members)
				Utils::AddDescription(entries, "script member", std::format("{}.{}", module.Name, member.Name), member.Description);
		}
		for (const ScriptApiGroup& type : scripts.GetTypes())
		{
			Utils::AddDescription(entries, "script type", type.Name, type.Description);
			for (const ScriptApiMember& member : type.Members)
				Utils::AddDescription(entries, "script member", std::format("{}.{}", type.Name, member.Name), member.Description);
		}
		for (const ScriptApiEnum& enumeration : scripts.GetEnums())
		{
			Utils::AddDescription(entries, "script enum", enumeration.Name, enumeration.Description);
			for (const EnumEntry& entry : enumeration.Values)
				Utils::AddDescription(entries, "script enum value", std::format("{}.{}", enumeration.Name, entry.Name), entry.Description);
		}
		for (const ScriptApiAlias& alias : scripts.GetAliases())
			Utils::AddDescription(entries, "script alias", alias.Name, alias.Description);

		for (const MethodDescriptor* method : methods.GetMethods())
			Utils::AddDescription(entries, "method", method->Specification.Name, method->Specification.Description);

		std::ranges::sort(entries, {}, [](const DescriptionEntry& entry)
		{
			return std::tie(entry.Kind, entry.Name);
		});
		return entries;
	}

	std::vector<std::string> FindMissingDescriptions(std::span<const DescriptionEntry> entries)
	{
		std::vector<std::string> missing;
		for (const DescriptionEntry& entry : entries)
		{
			if (Utils::IsBlank(entry.Description))
				missing.push_back(std::format("{} {}", entry.Kind, entry.Name));
		}
		std::ranges::sort(missing);
		missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
		return missing;
	}

}
