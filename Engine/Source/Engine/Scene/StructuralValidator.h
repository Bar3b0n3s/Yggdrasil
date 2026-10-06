#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/LoadReport.h"

namespace Engine {

	class TypeRegistry;

	// Structural pre-validation of scene and prefab documents (Architecture §6): the whole entity graph is checked before
	// any entity is created. Merge results and hand edits are expected inputs, so every defect is a located Result (JSON
	// pointer into the document), never an assert; asserts (Scene::CreateEntityWithID) are reserved for in-memory API
	// misuse. Each defect has a fixture under Tests/Data/Scenes/Invalid/.
	//
	// Defects and their deterministic repairs (LoadMode::Repair):
	//   - an entity without "ID", with a malformed one, or with the zero ID (SCENE_INVALID_ID): a fresh ID;
	//   - two entities with the same ID (SCENE_DUPLICATE_ID): the later one in file order gets a fresh ID; children naming
	//     the ID stay with the first;
	//   - a "Parent" naming no entity (SCENE_DANGLING_PARENT): the orphan is reparented to the root;
	//   - parents forming a cycle (SCENE_PARENT_CYCLE): the cycle is cut at its first back-edge in file order (that entity
	//     becomes a root);
	//   - a "PrefabLink" whose InstanceRoot is missing or lacks the "Prefab" component, i.e. a member whose root is not an
	//     instance root (SCENE_INCONSISTENT_PREFAB_LINK): the member is unpacked (its PrefabLink is removed);
	//   - a UniquePerScene component on a second entity (SCENE_DUPLICATE_UNIQUE_COMPONENT, scenes only): every extra after
	//     the first in canonical order is dropped, its JSON kept in LoadRepair::Removed;
	//   - a registered component that is entity-level or not Serializable under "Components" (SCENE_MISPLACED_COMPONENT,
	//     for example "Name" or "Relationship" put there by a hand edit or merge): it is dropped, its JSON kept in
	//     LoadRepair::Removed. Such a component is never added to an entity (adding a Required one would assert);
	//   - prefabs only (PREFAB_INVALID_ROOT): "Root" missing, naming no entity, or not the only root entity; not
	//     repairable.
	// A child listed before its parent is not an error: the result is normalized to canonical order with the warning
	// SCENE_NONCANONICAL_ORDER, in both modes.
	//
	// Pure function of its inputs (and of the repair generator's state); thread-safe for distinct arguments. Component
	// contents are not validated here (the serializer does that while creating entities).
	class StructuralValidator
	{
	public:
		// Validates `document` (a migrated, current-version scene or prefab document whose header the caller has read) and
		// returns a copy whose "Entities" array is in canonical order and, in Repair mode, has every fix applied (each
		// recorded in report.Repairs). Errors (Strict mode, or a defect that cannot be repaired): Validation carrying one
		// ErrorIssue per defect in document order, located at the first one, each with its code in the message prefix
		// ("SCENE_DUPLICATE_ID: ..."); the same defects are also recorded in report.Diagnostics as errors.
		[[nodiscard]] static Result<Json> Validate(const Json& document, DocumentKind kind, const TypeRegistry& registry,
			const LoadOptions& options, LoadReport& report);
	};

}
