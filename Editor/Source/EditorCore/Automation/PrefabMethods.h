#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

// prefab.* (Architecture §5.5, §13.5, five methods), frozen by the M6 contract. Conventions as in MethodRegistry.h. Entity
// params are EntityRefs (§13.4: 16 hex digits, a unique prefix of at least 6, or a path); prefab params are AssetReferences
// of Prefab assets (§7.1). Every method changes the edit scene and/or a .prefab file through commands executed by
// EditorContext::Execute, as one undo step labelled "[agent] ..." (a CompositeCommand of a SceneEditCommand and an
// AssetEditCommand where both change), reporting undoIndex. Instances are always rebuilt through PrefabInstantiator, the
// single path (§5.5).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// prefab.create {entity, path, replaceWithInstance?}: writes the entity's subtree as a new prefab asset at
	// project-relative `path` (below Assets/, extension .prefab) with its .meta (Prefab::CreateFromEntity: nested instances
	// are flattened, §1.2, §5.5). With replaceWithInstance (default false) the entity is replaced by an instance of the new
	// prefab at the same place in the hierarchy, keeping its root ID (so references to it survive). Supports dry runs
	// (§13.4: the files go to the overlay and the scene change to the dry run's copy).
	struct PrefabCreateParams
	{
		std::string Entity{};
		std::string Path{};
		bool ReplaceWithInstance = false;
	};

	struct PrefabCreateResult
	{
		AssetSummary Prefab{};
		EntitySummary Instance{}; // the instance root with replaceWithInstance; empty otherwise
		uint32_t UndoIndex = 0;
	};

	// prefab.instantiate {prefab, parent?, index?, transform?, name?}: InstantiatePrefabAsset with a fresh root ID from the
	// editor's generator, under `parent` (empty: a root) at sibling `index` (absent: last). `transform` is a partial
	// Transform component value (registry PascalCase, virtual fields such as EulerAngles allowed, ADR 0008 decision 35)
	// over the prefab root's; `name` renames the root (absent: the prefab root's name). Supports dry runs.
	struct PrefabInstantiateParams
	{
		std::string Prefab{};
		std::string Parent{};
		uint32_t Index = 0;       // only when given (MethodContext::HasParam)
		VariantValue Transform{}; // an object or null
		std::string Name{};       // only when given
	};

	struct PrefabInstantiateResult
	{
		EntitySummary Entity{}; // the instance root
		AssetSummary Prefab{};
		uint32_t UndoIndex = 0;
	};

	// prefab.apply {instance}: writes the instance's overrides back into its prefab asset (PrefabInstantiator::
	// ApplyOverrides; user children are not added) and rebuilds every instance of that prefab in the open scene from the new
	// prefab (§5.5 "Update", EditorContext::CreatePrefabUpdateCommand), as one undo step (the .prefab file edit and the
	// scene edit). The other ways a prefab gets a new version follow the same rule (EditorContext.h "Prefab updates"):
	// asset.setImportSettings and asset.reimport of an instanced glTF compose the same update, and an external change
	// raises SceneChangedOnDisk, adopted by scene.open {reload: true}, which rebuilds the instances.
	struct PrefabApplyParams
	{
		std::string Instance{};
	};

	struct PrefabApplyResult
	{
		AssetSummary Prefab{};
		uint32_t UpdatedInstances = 0; // instances of the prefab in the open scene that were rebuilt, this one included
		uint32_t UndoIndex = 0;
	};

	// Registry struct "PrefabOverrideKey": selects overrides of an instance (prefab.revert {overrides}), matching
	// PrefabOverride's PrefabEntityID, Kind, Component and Field (§5.5; ADR 0006 decision 18). An empty Kind, Component or
	// Field matches any value of it.
	struct PrefabOverrideKey
	{
		std::string PrefabEntityId{}; // the prefab-local entity id, 16 hex digits
		std::string Kind{};           // "Field", "AddComponent", "RemoveComponent", "EntityKey" (case-insensitive) or empty
		std::string Component{};
		std::string Field{};
	};

	// prefab.revert {instance, overrides?}: clears the instance's overrides and rebuilds it from its prefab
	// (PrefabInstantiator::Revert; user children kept). With `overrides` only the matching overrides are removed and the
	// others are kept. Supports dry runs.
	struct PrefabRevertParams
	{
		std::string Instance{};
		std::vector<PrefabOverrideKey> Overrides{}; // only when given; absent: every override
	};

	struct PrefabRevertResult
	{
		EntitySummary Instance{};
		uint32_t RemovedOverrides = 0;
		uint32_t UndoIndex = 0;
	};

	// prefab.unpack {instance}: PrefabInstantiator::Unpack; the entities and their data stay, the links go. Supports dry runs.
	struct PrefabUnpackParams
	{
		std::string Instance{};
	};

	struct PrefabUnpackResult
	{
		EntitySummary Entity{};
		uint32_t UndoIndex = 0;
	};

	namespace Automation {

		// prefab.create. Errors: NotFound (entity); InvalidArgument for a path outside Assets/ or without .prefab;
		// AlreadyExists for an existing file; InvalidState for an ambiguous reference (ADR 0006 decision 32).
		[[nodiscard]] Result<PrefabCreateResult> PrefabCreate(EditorMethodContext& context, const PrefabCreateParams& params);
		// prefab.instantiate. Errors: NotFound (prefab or parent); Validation for an invalid transform (located under
		// /transform) or a unique-per-scene collision; those of InstantiatePrefabAsset.
		[[nodiscard]] Result<PrefabInstantiateResult> PrefabInstantiate(EditorMethodContext& context, const PrefabInstantiateParams& params);
		// prefab.apply. Errors: NotFound; InvalidArgument when `instance` is not an instance root; NotFound for its missing
		// prefab asset (PREFAB_MISSING_ASSET); InvalidState for an ambiguous reference.
		[[nodiscard]] Result<PrefabApplyResult> PrefabApply(EditorMethodContext& context, const PrefabApplyParams& params);
		// prefab.revert. Errors: as prefab.apply; InvalidArgument for a malformed override key.
		[[nodiscard]] Result<PrefabRevertResult> PrefabRevert(EditorMethodContext& context, const PrefabRevertParams& params);
		// prefab.unpack. Errors: NotFound; InvalidArgument when `instance` is not an instance root.
		[[nodiscard]] Result<PrefabUnpackResult> PrefabUnpack(EditorMethodContext& context, const PrefabUnpackParams& params);

	}

	// Registers PrefabOverrideKey and the params and result structs above.
	void RegisterPrefabMethodTypes(TypeRegistry& registry);

	// Registers the five methods. Tools (§13.8): prefab.create, prefab.instantiate, prefab.apply. All mutate and are
	// AllowedInBatch (every effect goes through EditorContext::Execute). SupportsDryRun: create, instantiate, revert, unpack
	// (apply rewrites the prefab and every instance and is not dry-runnable, §13.4's list).
	void RegisterPrefabMethods(MethodRegistry& methods);

}
