#include "EnginePCH.h"
#include "Engine/Reflection/ComponentInfo.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Engine {

	ComponentInfo::ComponentInfo(std::string name, std::string description, const TypeRegistry& registry, size_t index)
		: StructInfo(std::move(name), std::move(description), registry), m_Index(index)
	{
	}

	Status ComponentInfo::Migrate(uint32_t fromVersion, Json& component) const
	{
		if (fromVersion == m_Version)
			return {};
		if (fromVersion > m_Version)
		{
			return MakeError(ErrorCode::UnsupportedVersion, "component '{}' has version {}, newer than the supported version {}", GetName(),
				fromVersion, m_Version);
		}
		if (fromVersion == 0)
			return MakeError(ErrorCode::Validation, "component '{}' has the invalid version 0 (versions start at 1)", GetName());

		// Migrate a copy, so a failing step leaves `component` unchanged.
		Json upgraded = component;
		for (uint32_t version = fromVersion; version < m_Version; ++version)
		{
			const auto step = std::find_if(m_Migrations.begin(), m_Migrations.end(), [version](const std::pair<uint32_t, ComponentMigration>& migration)
			{
				return migration.first == version;
			});
			if (step == m_Migrations.end())
				return MakeError(ErrorCode::Validation, "component '{}' has no migration from version {} to {}", GetName(), version, version + 1);
			ENGINE_TRY(WithContext(step->second(upgraded), std::format("while migrating component '{}' from version {} to {}", GetName(), version, version + 1)));
		}
		component = std::move(upgraded);
		return {};
	}

	void ComponentInfo::SetCategory(std::string category)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		ENGINE_CORE_ASSERT(!category.empty(), "Component '{}' needs a non-empty category", GetName());
		m_Category = std::move(category);
	}

	void ComponentInfo::SetVersion(uint32_t version)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		ENGINE_CORE_ASSERT(version >= 1, "Component '{}' needs a version of at least 1", GetName());
		m_Version = version;
	}

	void ComponentInfo::SetFlags(ComponentFlags flags)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		m_Flags = flags;
	}

	void ComponentInfo::AddRequires(TypeKey component)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		ENGINE_CORE_ASSERT(component != nullptr, "Component '{}' requires a null type", GetName());
		if (std::find(m_RequiresKeys.begin(), m_RequiresKeys.end(), component) == m_RequiresKeys.end())
			m_RequiresKeys.push_back(component);
	}

	void ComponentInfo::AddExcludes(TypeKey component)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		ENGINE_CORE_ASSERT(component != nullptr, "Component '{}' excludes a null type", GetName());
		if (std::find(m_ExcludesKeys.begin(), m_ExcludesKeys.end(), component) == m_ExcludesKeys.end())
			m_ExcludesKeys.push_back(component);
	}

	void ComponentInfo::AddMigration(uint32_t fromVersion, ComponentMigration migration)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		ENGINE_CORE_ASSERT(fromVersion >= 1, "Component '{}' has a migration from version 0 (versions start at 1)", GetName());
		ENGINE_CORE_ASSERT(migration != nullptr, "Component '{}' has an empty migration from version {}", GetName(), fromVersion);
		const auto position = std::lower_bound(m_Migrations.begin(), m_Migrations.end(), fromVersion,
			[](const std::pair<uint32_t, ComponentMigration>& entry, uint32_t version)
		{
			return entry.first < version;
		});
		ENGINE_CORE_ASSERT(position == m_Migrations.end() || position->first != fromVersion, "Component '{}' has two migrations from version {}",
			GetName(), fromVersion);
		m_Migrations.insert(position, { fromVersion, migration });
	}

	void ComponentInfo::SetHostOps(const ComponentHostOps* hostOps)
	{
		ENGINE_CORE_ASSERT(!GetRegistry().IsFrozen(), "Component '{}' is configured after TypeRegistry::Freeze", GetName());
		m_HostOps = hostOps;
	}

	void ComponentInfo::Resolve(const TypeRegistry& registry)
	{
		ENGINE_CORE_ASSERT(!m_Category.empty(), "Component '{}' needs a category (ComponentBuilder::Category)", GetName());

		m_Requires.clear();
		for (const TypeKey key : m_RequiresKeys)
		{
			const ComponentInfo* required = registry.FindComponentByKey(key);
			ENGINE_CORE_ASSERT(required != nullptr, "Component '{}' requires a type that is not a registered component", GetName());
			ENGINE_CORE_ASSERT(required != this, "Component '{}' requires itself", GetName());
			if (required != nullptr)
				m_Requires.push_back(required);
		}

		m_Excludes.clear();
		for (const TypeKey key : m_ExcludesKeys)
		{
			const ComponentInfo* excluded = registry.FindComponentByKey(key);
			ENGINE_CORE_ASSERT(excluded != nullptr, "Component '{}' excludes a type that is not a registered component", GetName());
			ENGINE_CORE_ASSERT(excluded != this, "Component '{}' excludes itself", GetName());
			if (excluded != nullptr)
				m_Excludes.push_back(excluded);
		}

		for (uint32_t version = 1; version < m_Version; ++version)
		{
			const bool hasStep = std::any_of(m_Migrations.begin(), m_Migrations.end(), [version](const std::pair<uint32_t, ComponentMigration>& migration)
			{
				return migration.first == version;
			});
			ENGINE_CORE_ASSERT(hasStep, "Component '{}' (version {}) has no migration from version {}", GetName(), m_Version, version);
		}
		for (const std::pair<uint32_t, ComponentMigration>& migration : m_Migrations)
		{
			ENGINE_CORE_ASSERT(migration.first < m_Version, "Component '{}' (version {}) has a migration from version {}, which is not older",
				GetName(), m_Version, migration.first);
		}
	}

}
