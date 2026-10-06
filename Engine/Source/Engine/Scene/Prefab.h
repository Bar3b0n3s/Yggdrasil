#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/LoadReport.h"

#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class ConstEntity;
	class Scene;
	class TypeRegistry;
	class VirtualFileSystem;

	// A prefab asset (Architecture §5.5, §6.3): a serialized entity subtree with exactly one root and prefab-local UUIDs.
	// The document is the scene entity schema with "Format": "Prefab", a "Root" key naming the root's local ID, and no
	// "Seed":
	//     { "Format": "Prefab", "Version": 1, "Name": <string>, "Root": <hex>, "ComponentVersions": {...},
	//       "Entities": [ ...canonical order, the root first... ] }
	// Prefabs never contain PrefabInstance or PrefabLink components: nested instances are flattened when a prefab is
	// created (live nested prefabs are a non-goal, §1.2).
	//
	// A Prefab holds its validated canonical entity array; it is immutable after creation, cheap to copy (the array is
	// shared, §4.7) and safe to read from any thread.
	class Prefab
	{
	public:
		static constexpr std::string_view FormatName = "Prefab";

		// An empty prefab (no entities; GetRootID() is invalid). Only useful as an assignment target.
		Prefab() = default;

		// Reads a prefab document: header (Format "Prefab", Version 0..Migrations::CurrentVersion), migration, structural
		// pre-validation as DocumentKind::Prefab (PREFAB_INVALID_ROOT included), and every component validated against the
		// registry as the scene loader does (unknown components are preserved). Errors: Validation (located, with issues),
		// UnsupportedVersion.
		[[nodiscard]] static Result<Prefab> FromJson(const Json& document, const TypeRegistry& registry, const LoadOptions& options,
			LoadReport& report);
		[[nodiscard]] static Result<Prefab> LoadFromString(std::string_view text, const TypeRegistry& registry, const LoadOptions& options,
			LoadReport& report);
		[[nodiscard]] static Result<Prefab> LoadFromFile(const VirtualFileSystem& vfs, const VfsPath& path, const TypeRegistry& registry,
			const LoadOptions& options, LoadReport& report);

		// Creates a prefab from the subtree rooted at `root`: entities keep their current IDs as prefab-local IDs and their
		// canonical JSON, the root's "Parent" becomes null, and nested instances are flattened (their PrefabInstance and
		// PrefabLink components dropped, their entities kept). Errors: as SceneSerializer::EntityToJson.
		[[nodiscard]] static Result<Prefab> CreateFromEntity(ConstEntity root, std::string name);

		// The canonical document. Errors: none for a prefab built by this class; Validation otherwise.
		[[nodiscard]] Result<Json> ToJson() const;
		[[nodiscard]] Result<std::string> SaveToString(JsonStyle style = JsonStyle::Pretty) const;
		[[nodiscard]] Status SaveToFile(VirtualFileSystem& vfs, const VfsPath& path) const;

		[[nodiscard]] const std::string& GetName() const { return m_Name; }
		[[nodiscard]] UUID GetRootID() const { return m_Root; }
		[[nodiscard]] bool IsEmpty() const { return !m_Root.IsValid(); }

		// The canonical entity array (an empty array for an empty prefab), parents before children.
		[[nodiscard]] const Json& GetEntities() const;
		// The canonical JSON of the entity with prefab-local ID `id`, or nullptr.
		[[nodiscard]] const Json* FindEntity(UUID id) const;
		// Every prefab-local ID in canonical order.
		[[nodiscard]] std::vector<UUID> GetEntityIDs() const;
	private:
		std::string m_Name;
		UUID m_Root;
		Ref<const Json> m_Entities; // shared immutable canonical array; null for an empty prefab
	};

}
