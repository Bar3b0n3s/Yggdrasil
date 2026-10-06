#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/LoadReport.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the scene and prefab formats share inside the Scene module (§6.2, §6.3: "identical entity schema"): the one
// document loader, which SceneSerializer::FromJson and Prefab::FromJson both use; the canonical prefab document, which
// Prefab and PrefabInstantiator (prefab.apply) both build; single-entity creation and replacement with the unknown
// component versions of a document, which PrefabInstantiator uses for members; and the small readers they all need.

namespace Engine {

	class ConstEntity;
	class Entity;
	class Scene;
	class TypeRegistry;

	namespace Utils {

		// Loads a scene or prefab document into the empty `scene` after resetting `report`, exactly as
		// SceneSerializer::FromJson describes for scenes: header check (Format "Scene" or "Prefab"), migration, structural
		// pre-validation of `kind` (PREFAB_INVALID_ROOT and PREFAB_NESTED_INSTANCE for prefabs, unique-per-scene components
		// for scenes), every entity read and validated before any is created, and creation in canonical order. A prefab
		// document's "Root" key replaces "Seed", so the scene's seed stays 0 for a prefab. Errors: as
		// SceneSerializer::FromJson (located, with options.SourcePath); on error the scene is left empty.
		[[nodiscard]] Status LoadEntityDocument(Scene& scene, const Json& document, DocumentKind kind, const LoadOptions& options,
			LoadReport& report);

		// The canonical prefab document named `name` whose root entity has the ID `root` and whose "Entities" are `entities`
		// (canonical entity objects in canonical order, the root first): "Format", "Version", "Name", "Root",
		// "ComponentVersions", "Entities". "ComponentVersions" lists the registered components that appear under some
		// entity's "Components" in registry order with their registered versions, then the unknown ones in first-use order
		// with the version `unknownVersions` records for them (an unknown component without a recorded version, or with
		// version 0, is not listed, as SceneSerializer::ToJson does).
		[[nodiscard]] Json MakePrefabDocument(const TypeRegistry& registry, std::string_view name, UUID root, Json entities,
			std::span<const std::pair<std::string, uint32_t>> unknownVersions);

		// SceneSerializer::EntityFromJson and SceneSerializer::ApplyEntityJson. When `componentVersions` is not null it is a
		// document's "ComponentVersions" object, and each unknown component of the entity takes its version from it (0 when
		// it lists none), so prefab members carry the versions of their prefab. When it is null, the version comes from the
		// entity itself (ApplyEntityJson) or from the first entity of the scene that preserves a component of that name, as
		// the public functions document.
		[[nodiscard]] Result<Entity> EntityFromJson(Scene& scene, const JsonReader& entity, std::optional<uint32_t> siblingIndex,
			const Json* componentVersions, const LoadOptions& options, LoadReport& report);
		[[nodiscard]] Status ApplyEntityJson(Entity entity, const JsonReader& json, const Json* componentVersions, const LoadOptions& options,
			LoadReport& report);

		// Appends the "ComponentVersions" entry of each unknown component `entity` preserves, unless one is listed already
		// (first use wins, as SceneSerializer::ToJson writes them).
		void CollectUnknownVersions(ConstEntity entity, std::vector<std::pair<std::string, uint32_t>>& versions);

		// The entity object's member `key` ("ID" or "Parent") as a UUID; the invalid UUID when it is absent, null or
		// malformed (which a canonical entity object never is, except a root's null "Parent").
		[[nodiscard]] UUID ReadEntityUUID(const Json& entity, std::string_view key);

		// `error` located in the file `path` too, unless it names a file already or `path` is empty.
		[[nodiscard]] Error WithSourceFile(Error error, const std::string& path);

	}

}
