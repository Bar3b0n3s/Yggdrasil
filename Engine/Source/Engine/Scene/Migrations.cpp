#include "EnginePCH.h"
#include "Engine/Scene/Migrations.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// A known component whose "ComponentVersions" entry is older than its registered version.
		struct ComponentMigrationStep
		{
			const ComponentInfo* Component = nullptr;
			uint32_t FromVersion = 0;
		};

	}

	namespace Utils {

		static std::string_view DocumentFormatName(DocumentKind kind)
		{
			switch (kind)
			{
				case DocumentKind::Scene:  return "Scene";
				case DocumentKind::Prefab: return "Prefab";
			}

			ENGINE_CORE_ASSERT(false, "Unknown DocumentKind {}", std::to_underlying(kind));
			return "Scene";
		}

		// `error` with `prefix` put in front of its JSON pointer and of every issue's pointer: a component migration
		// locates its errors within the component, the document locates the component.
		static Error PrefixPointers(const Error& error, std::string_view prefix)
		{
			ErrorLocation location = error.GetLocation();
			location.JsonPointer = std::string(prefix) + location.JsonPointer.value_or(std::string());

			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;

			Error result = Error(error.GetCode(), error.GetMessageText())
							   .WithLocation(std::move(location))
							   .WithHint(error.GetHint())
							   .WithIssues(std::move(issues));
			for (const std::string& context : error.GetContexts())
			{
				Error next = std::move(result).WithContext(context);
				result = std::move(next);
			}
			return result;
		}

		// The version 0 "Components" array of one entity as a version 1 object keyed by type (array order kept). Every type
		// used is added to `versions` at version 1, in order of first use.
		static Result<Json> UpgradeComponents(const JsonReader& components, Json& versions)
		{
			ENGINE_TRY_ASSIGN(const size_t count, components.GetArraySize());
			Json result = Json::object();
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader component, components.GetElement(index));
				ENGINE_TRY(component.ExpectType(JsonType::Object));
				ENGINE_TRY_ASSIGN(const JsonReader typeReader, component.GetMember("Type"));
				ENGINE_TRY_ASSIGN(const std::string type, typeReader.ReadString());
				if (result.contains(type))
				{
					return std::unexpected(typeReader.MakeLocatedError(ErrorCode::Validation,
						std::format("component type '{}' is listed twice on this entity", type)));
				}

				Json fields = Json::object();
				for (auto member = component.GetValue().begin(); member != component.GetValue().end(); ++member)
				{
					if (member.key() != "Type")
						fields[member.key()] = *member;
				}
				result[type] = std::move(fields);
				if (!versions.contains(type))
					versions[type] = 1;
			}
			return result;
		}

		// One version 0 entity as a version 1 entity: "Enabled" becomes "Active" in place, "Components" becomes an object.
		static Result<Json> UpgradeEntity(const JsonReader& entity, Json& versions)
		{
			ENGINE_TRY(entity.ExpectType(JsonType::Object));
			if (const std::optional<JsonReader> active = entity.FindMember("Active"))
			{
				return std::unexpected(active->MakeLocatedError(ErrorCode::Validation,
					"a version 0 entity spells its active state \"Enabled\", not \"Active\""));
			}

			Json result = Json::object();
			for (auto member = entity.GetValue().begin(); member != entity.GetValue().end(); ++member)
			{
				if (member.key() == "Enabled")
				{
					result["Active"] = *member;
				}
				else if (member.key() == "Components")
				{
					ENGINE_TRY_ASSIGN(Json components,
						UpgradeComponents(JsonReader(*member, JsonReader::AppendPointer(entity.GetPointer(), "Components")), versions));
					result["Components"] = std::move(components);
				}
				else
				{
					result[member.key()] = *member;
				}
			}
			return result;
		}

		// Checks the "ComponentVersions" entries of a current-version document and lists the known components that need a
		// migration, in the order of the entries. Unknown components are left to the serializer.
		static Result<std::vector<ComponentMigrationStep>> PlanComponentMigrations(const Json& document, const TypeRegistry& registry)
		{
			std::vector<ComponentMigrationStep> steps;
			const JsonReader root(document);
			const std::optional<JsonReader> versions = root.FindMember("ComponentVersions");
			if (!versions)
				return steps;

			ENGINE_TRY_ASSIGN(const std::vector<std::string> names, versions->GetMemberNames());
			for (const std::string& name : names)
			{
				ENGINE_TRY_ASSIGN(const JsonReader entry, versions->GetMember(name));
				ENGINE_TRY_ASSIGN(const uint32_t version, entry.ReadUInt32());
				if (version == 0)
				{
					return std::unexpected(
						entry.MakeLocatedError(ErrorCode::Validation, std::format("component '{}' has version 0; versions start at 1", name)));
				}

				const ComponentInfo* component = registry.FindComponent(name);
				if (component == nullptr)
					continue;
				if (version > component->GetVersion())
				{
					return std::unexpected(entry.MakeLocatedError(ErrorCode::UnsupportedVersion,
						std::format("component '{}' version {} is newer than this build supports (version {})", name, version,
							component->GetVersion())));
				}
				if (version < component->GetVersion())
					steps.push_back(ComponentMigrationStep{ component, version });
			}
			return steps;
		}

		// Runs `steps` on every entity of `document` that has the component, then records the new versions.
		static Status ApplyComponentMigrations(Json& document, std::span<const ComponentMigrationStep> steps)
		{
			const auto entities = document.find("Entities");
			if (entities != document.end())
			{
				const JsonReader entitiesReader(*entities, "/Entities");
				ENGINE_TRY_ASSIGN(const size_t count, entitiesReader.GetArraySize());
				for (size_t index = 0; index < count; ++index)
				{
					const std::string entityPointer = JsonReader::AppendPointer("/Entities", index);
					Json& entity = (*entities)[index];
					ENGINE_TRY(JsonReader(entity, entityPointer).ExpectType(JsonType::Object));
					const auto components = entity.find("Components");
					if (components == entity.end())
						continue;
					const std::string componentsPointer = JsonReader::AppendPointer(entityPointer, "Components");
					ENGINE_TRY(JsonReader(*components, componentsPointer).ExpectType(JsonType::Object));

					for (const ComponentMigrationStep& step : steps)
					{
						const auto component = components->find(step.Component->GetName());
						if (component == components->end())
							continue;
						if (Status migrated = step.Component->Migrate(step.FromVersion, *component); !migrated)
						{
							const std::string componentPointer = JsonReader::AppendPointer(componentsPointer, step.Component->GetName());
							return std::unexpected(PrefixPointers(migrated.error(), componentPointer));
						}
					}
				}
			}

			Json& versions = document["ComponentVersions"];
			for (const ComponentMigrationStep& step : steps)
				versions[step.Component->GetName()] = step.Component->GetVersion();
			return {};
		}

	}

	Status Migrations::UpgradeDocument(Json& document, DocumentKind kind, const TypeRegistry& registry, LoadReport& report)
	{
		static_assert(MinimumVersion == 0, "Reject versions below MinimumVersion here once the oldest readable version rises");

		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY_ASSIGN(const JsonReader versionReader, root.GetMember("Version"));
		ENGINE_TRY_ASSIGN(const uint64_t version, versionReader.ReadUInt64());
		if (version > CurrentVersion)
		{
			return std::unexpected(versionReader.MakeLocatedError(ErrorCode::UnsupportedVersion,
				std::format("'{}' version {} is newer than this build supports (version {})", Utils::DocumentFormatName(kind), version,
					CurrentVersion)));
		}
		const uint32_t fileVersion = static_cast<uint32_t>(version);
		report.FileVersion = fileVersion;

		// Work on a copy only when something changes, so that a current document is never copied and a failed migration
		// leaves `document` as it was.
		Json upgraded;
		bool changed = false;
		if (fileVersion == 0)
		{
			upgraded = document;
			ENGINE_TRY(UpgradeVersion0To1(upgraded));
			changed = true;
		}

		ENGINE_TRY_ASSIGN(const std::vector<ComponentMigrationStep> steps, Utils::PlanComponentMigrations(changed ? upgraded : document, registry));
		if (!steps.empty())
		{
			if (!changed)
				upgraded = document;
			ENGINE_TRY(Utils::ApplyComponentMigrations(upgraded, steps));
			changed = true;
		}

		if (changed)
			document = std::move(upgraded);
		report.Migrated = changed;
		return {};
	}

	Status Migrations::UpgradeVersion0To1(Json& document)
	{
		constexpr uint32_t UpgradedVersion = 1;
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		if (const std::optional<JsonReader> versions = root.FindMember("ComponentVersions"))
			return std::unexpected(versions->MakeLocatedError(ErrorCode::Validation, "a version 0 document has no \"ComponentVersions\""));

		Json versions = Json::object();
		Json entities;
		const std::optional<JsonReader> entitiesReader = root.FindMember("Entities");
		if (entitiesReader)
		{
			ENGINE_TRY_ASSIGN(const size_t count, entitiesReader->GetArraySize());
			entities = Json::array();
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader entity, entitiesReader->GetElement(index));
				ENGINE_TRY_ASSIGN(Json upgradedEntity, Utils::UpgradeEntity(entity, versions));
				entities.push_back(std::move(upgradedEntity));
			}
		}

		// The members keep their order; "ComponentVersions" goes right before "Entities", where version 1 writes it.
		Json upgraded = Json::object();
		for (auto member = document.begin(); member != document.end(); ++member)
		{
			if (member.key() == "Version")
			{
				upgraded["Version"] = UpgradedVersion;
			}
			else if (member.key() == "Entities")
			{
				upgraded["ComponentVersions"] = versions;
				upgraded["Entities"] = std::move(entities);
			}
			else
			{
				upgraded[member.key()] = *member;
			}
		}
		if (!entitiesReader)
			upgraded["ComponentVersions"] = std::move(versions);
		if (!upgraded.contains("Version"))
			upgraded["Version"] = UpgradedVersion;

		document = std::move(upgraded);
		return {};
	}

}
