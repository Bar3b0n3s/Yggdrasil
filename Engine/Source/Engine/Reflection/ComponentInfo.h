#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	// Defined by the module that hosts components: Scene/ComponentHostOps.h (Has/GetConst/Get/Add/Remove/Patch on an
	// entity).
	// Reflection stores the pointer and never dereferences it, because it cannot include EnTT or Scene (§3, ADR 0006).
	struct ComponentHostOps;

	// Component flags (Architecture §5.4). Default is what a component without special rules gets.
	//   Serializable    its data is part of scene and prefab files;
	//   ScriptVisible   reachable by name from scripts (GetComponent, proxies);
	//   EditorVisible   listed by the inspector and the add-component menu;
	//   Removable       may be removed by users, scripts and automation;
	//   UniquePerScene  at most one per scene (Environment, PostProcess; a duplicate is a structural defect, §6);
	//   Required        every entity has it from creation (ID, Name, Relationship, Transform);
	//   Hidden          not shown in the inspector or as a script shortcut; maintained by the engine (ID, Relationship,
	//                   Prefab, PrefabLink), so the by-name write paths (Scene's ComponentAccess) reject writes to it;
	//   EntityLevel     serialized as entity keys instead of under "Components" (ID -> "ID", Name -> "Name",
	//                   Relationship -> "Parent", Tags -> "Tags") and reached by scripts through Entity members, never as
	//                   a shortcut property (§5.3, §11.4);
	//   NoShortcut      no entity.<Name> shortcut property (Script: GetScript() returns the instance, §11.4).
	enum class ComponentFlags : uint16_t
	{
		None = 0,
		Serializable = 1 << 0,
		ScriptVisible = 1 << 1,
		EditorVisible = 1 << 2,
		Removable = 1 << 3,
		UniquePerScene = 1 << 4,
		Required = 1 << 5,
		Hidden = 1 << 6,
		EntityLevel = 1 << 7,
		NoShortcut = 1 << 8,
		Default = Serializable | ScriptVisible | EditorVisible | Removable
	};

	template<>
	inline constexpr bool EnableFlagOperators<ComponentFlags> = true;

	// Upgrades one component's JSON object in place from version N to N + 1 (§6 "Migrations ... per component"). A pure
	// function of its input: no registry, scene or file access. Errors: Validation when the input is not a valid version-N
	// object (located at its pointer within the component).
	using ComponentMigration = Status (*)(Json& component);

	// A reflected component (Architecture §5.4 "ComponentInfo"): a StructInfo plus its registry name semantics, mandatory
	// category, version, flags, Requires/Excludes relations, migrations and the type-erased ECS operations Scene installs.
	// Its index is its position in registration order, which is the canonical order of components in files (§6 "keys in
	// registry order") and of "ComponentVersions".
	//
	// Configured only before TypeRegistry::Freeze, on one thread; Freeze resolves Requires and Excludes. Afterwards every
	// const member is thread-safe.
	class ComponentInfo : public StructInfo
	{
	public:
		ComponentInfo(std::string name, std::string description, const TypeRegistry& registry, size_t index);

		[[nodiscard]] size_t GetIndex() const { return m_Index; }
		[[nodiscard]] const std::string& GetCategory() const { return m_Category; }
		[[nodiscard]] uint32_t GetVersion() const { return m_Version; }
		[[nodiscard]] ComponentFlags GetFlags() const { return m_Flags; }
		[[nodiscard]] bool HasFlag(ComponentFlags flag) const { return Engine::HasFlag(m_Flags, flag); }

		// The components an entity must also have (Requires<T>) and must not have (Excludes<T>), in registration order of
		// the declarations. Valid after Freeze.
		[[nodiscard]] std::span<const ComponentInfo* const> GetRequires() const { return m_Requires; }
		[[nodiscard]] std::span<const ComponentInfo* const> GetExcludes() const { return m_Excludes; }

		// The ECS operations, or nullptr for a component registered without a host (a Reflection-only test).
		[[nodiscard]] const ComponentHostOps* GetHostOps() const { return m_HostOps; }

		// Upgrades `component` from `fromVersion` to GetVersion() by running the registered migrations in order. Success
		// without change when fromVersion == GetVersion(). Errors: UnsupportedVersion naming both versions when fromVersion
		// is newer than GetVersion(); Validation when fromVersion is 0 or a step has no migration; the migration's own
		// error. Atomic: on error `component` is unchanged.
		[[nodiscard]] Status Migrate(uint32_t fromVersion, Json& component) const;

		// Registration only (before Freeze; asserted).
		void SetCategory(std::string category);
		// `version` >= 1 (asserted). Every version above 1 needs a migration from each lower version (checked by Freeze).
		void SetVersion(uint32_t version);
		void SetFlags(ComponentFlags flags);
		void AddRequires(TypeKey component);
		void AddExcludes(TypeKey component);
		// Registers the migration from `fromVersion` to fromVersion + 1 (fromVersion >= 1, unique; asserted).
		void AddMigration(uint32_t fromVersion, ComponentMigration migration);
		void SetHostOps(const ComponentHostOps* hostOps);
		// Resolves Requires/Excludes keys to registered components (asserting that each is registered) and checks the
		// migration chain; called by TypeRegistry::Freeze.
		void Resolve(const TypeRegistry& registry);
	private:
		size_t m_Index = 0;
		std::string m_Category;
		uint32_t m_Version = 1;
		ComponentFlags m_Flags = ComponentFlags::Default;
		std::vector<TypeKey> m_RequiresKeys;
		std::vector<TypeKey> m_ExcludesKeys;
		std::vector<const ComponentInfo*> m_Requires;
		std::vector<const ComponentInfo*> m_Excludes;
		std::vector<std::pair<uint32_t, ComponentMigration>> m_Migrations; // sorted by fromVersion
		const ComponentHostOps* m_HostOps = nullptr;
	};

}
