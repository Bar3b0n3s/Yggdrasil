#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// prefab.* (Architecture §5.5, §13.5, five methods), frozen by the M6 contract. Conventions as in MethodRegistry.h. Entity
// params are EntityRefs (§13.4: 16 hex digits, a unique prefix of at least 6, or a path); prefab params are AssetReferences
// of Prefab assets (§7.1). Every method changes the edit scene and/or a .prefab file through commands executed by
// EditorContext::Execute, as one undo step labelled "[agent] ..." (a CompositeCommand of a SceneEditCommand and an
// AssetEditCommand where both change), reporting undoIndex. Instances are always rebuilt through PrefabInstantiator, the
// single path (§5.5).
//
// Live nested prefabs (M20, Docs/Decisions/0031-m20-contract.md decisions 8 and 9): an instance root is outermost, or
// nested in the instance of a prefab that nests its prefab; PrefabInstantiator::GetInstanceLevels lists the instance roots
// an instance root belongs to, from the outermost down to itself. Only the outermost instance records overrides, each
// naming its entity by a path (PrefabOverride::Path), so prefab.apply, prefab.revert and prefab.unpack accept a nested
// instance root and act through its outermost instance as each describes.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// prefab.create {entity, path, replaceWithInstance?}: writes the entity's subtree as a new prefab asset at
	// project-relative `path` (below Assets/, extension .prefab) with its .meta (Prefab::CreateFromEntity: every instance
	// wholly inside the subtree stays live in the new prefab, outermost there, while the instance of `entity` itself and the
	// instances it is a member of are unpacked in the copy; Docs/Decisions/0031-m20-contract.md decision 8). With
	// replaceWithInstance (default false) the entity is replaced by an instance of the new prefab at the same place in the
	// hierarchy, keeping its root ID (so references to it survive). Replacing a member of another instance deletes it from
	// that instance, which records a RemoveEntity override as entity.destroy does, and the new instance stays a user child
	// of it (decision 7). Supports dry runs (§13.4: the files go to the overlay and the scene change to the dry run's copy).
	// The result names the instance root `instance`, empty without replaceWithInstance (prefab.instantiate names its new
	// root `entity`).
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

	// prefab.instantiate {prefab, parent?, index?, transform?, name?, components?}: InstantiatePrefabAsset with a fresh root
	// ID from the editor's generator, under `parent` (empty: a root) at sibling `index` (absent: last). `transform` is a
	// partial Transform component value (registry PascalCase, virtual fields such as EulerAngles allowed, ADR 0008 decision
	// 35) over the prefab root's; `name` renames the root (absent: the prefab root's name). `components` are per-instance
	// values for the root, written as entity.update writes them after `transform` (merged into the prefab's components,
	// missing ones added; script fields through {"Script": {"Fields": {...}}}, whose Script handle defaults to the prefab
	// root's) and recorded as the instance's overrides when the step commits, so one call equals prefab.instantiate
	// followed by entity.update of the new root. Supports dry runs and batches. The result names the new root `entity`
	// (prefab.create names its instance `instance`).
	struct PrefabInstantiateParams
	{
		std::string Prefab{};
		std::string Parent{};
		uint32_t Index = 0;       // only when given (MethodContext::HasParam)
		VariantValue Transform{}; // an object or null
		std::string Name{};       // only when given
		std::map<std::string, VariantValue> Components{};
	};

	struct PrefabInstantiateResult
	{
		EntitySummary Entity{}; // the instance root
		AssetSummary Prefab{};
		uint32_t UndoIndex = 0;
	};

	// prefab.apply {instance, level?}: writes overrides of an instance back into a prefab asset (user children are not
	// added) and rebuilds the instances that follow it (§5.5 "Update"), as one undo step. `instance` is an instance root,
	// outermost or nested; the overrides written are those of its outermost instance that target `instance` or an entity
	// below it in the instance (every override of an outermost root). `level` names the instance whose prefab receives them:
	// one of PrefabInstantiator::GetInstanceLevels(instance), from the outermost root down to `instance` itself; absent, the
	// outermost (decision 9: PrefabInstantiator::ApplyOverrides re-bases their paths onto that prefab and keeps the level
	// root's own Name, Transform and Parent recorded on the outermost root). Then the prefab assets nesting the written one
	// are rewritten in nesting order (EditorContext::GetNestingPrefabs and CreateNestingPrefabUpdateCommand), and every
	// outermost instance in the open scene of a prefab written or rewritten is rebuilt (EditorContext::
	// CreatePrefabUpdateCommand), all in the same undo step. The other ways a prefab gets a new version (EditorContext.h
	// "Prefab updates"): asset.setImportSettings and asset.reimport of an instanced glTF rebuild its outermost instances and
	// those of the prefabs nesting it in the same undo step, but rewrite the nesting prefab documents only from W20.2
	// (Docs/Decisions/0031-m20-contract.md decisions 8 and 15, stream S-B); an external change raises SceneChangedOnDisk,
	// adopted by scene.open {reload: true}, which rebuilds the instances.
	struct PrefabApplyParams
	{
		std::string Instance{};
		std::string Level{}; // only when given (MethodContext::HasParam); absent: the outermost instance root
	};

	struct PrefabApplyResult
	{
		AssetSummary Prefab{};                      // the level's prefab asset, written
		uint32_t UpdatedInstances = 0;              // outermost instances in the open scene rebuilt, this one's included
		std::vector<AssetSummary> UpdatedPrefabs{}; // the prefab assets nesting it that were rewritten, in nesting order
		uint32_t UndoIndex = 0;
	};

	// Registry struct "PrefabOverrideKey": selects overrides of an instance (prefab.revert {overrides}), matching
	// PrefabOverride's Path, Kind, Component and Field (§5.5; ADR 0006 decision 18; Docs/Decisions/0031-m20-contract.md
	// decision 10). Path is required; an empty Kind, Component or Field matches any value of it.
	struct PrefabOverrideKey
	{
		std::vector<std::string> Path{}; // the override's path as the instance's Prefab component records it: 16 hex digits each
		std::string Kind{};              // a PrefabOverrideKind name (case-insensitive), or empty
		std::string Component{};
		std::string Field{};
	};

	// prefab.revert {instance, overrides?}: removes overrides of an instance and rebuilds it from its prefab (user children
	// kept). On an outermost instance root, its overrides (every one: PrefabInstantiator::Revert); on a nested instance
	// root, the overrides its outermost instance records whose paths lie within the nested instance, its root included
	// (decision 8), and the outermost instance is rebuilt. With `overrides` only the matching ones among those are removed
	// and the others are kept. Supports dry runs.
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

	// prefab.unpack {instance}: PrefabInstantiator::Unpack on an outermost instance root; the entities and their data stay,
	// the links of the instance's own entities go, and each instance nested directly in it becomes outermost, keeping its
	// state as its own overrides (decision 8: one level per call). A nested instance root is refused: its outermost
	// instance is unpacked first. Supports dry runs.
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
		// /transform) or a unique-per-scene collision; those of InstantiatePrefabAsset; those of entity.update's components
		// (located under /components). Nothing is created on error.
		[[nodiscard]] Result<PrefabInstantiateResult> PrefabInstantiate(EditorMethodContext& context, const PrefabInstantiateParams& params);
		// prefab.apply. Errors: NotFound for a missing entity (located at /instance or /level) or a missing prefab asset
		// (PREFAB_MISSING_ASSET); InvalidArgument when `instance` is not an instance root, when `level` is not one of its
		// levels (the hint lists them by path) and when the level's prefab is imported rather than a .prefab file; Validation
		// PREFAB_CYCLE when the propagation meets a nesting cycle, one through the prefab or among the prefabs nesting it
		// (EditorContext::GetNestingPrefabs; one issue per prefab on it; nothing is written);
		// InvalidState for an ambiguous reference.
		[[nodiscard]] Result<PrefabApplyResult> PrefabApply(EditorMethodContext& context, const PrefabApplyParams& params);
		// prefab.revert. Errors: as prefab.apply's for `instance`; InvalidArgument for a malformed override key (located at
		// /overrides/<i>/path, /overrides/<i>/path/<j> or /overrides/<i>/kind).
		[[nodiscard]] Result<PrefabRevertResult> PrefabRevert(EditorMethodContext& context, const PrefabRevertParams& params);
		// prefab.unpack. Errors: NotFound; InvalidArgument when `instance` is not an instance root; InvalidState for a nested
		// instance root (located at /instance; the hint unpacks its outermost instance).
		[[nodiscard]] Result<PrefabUnpackResult> PrefabUnpack(EditorMethodContext& context, const PrefabUnpackParams& params);

	}

	// Registers PrefabOverrideKey and the params and result structs above.
	void RegisterPrefabMethodTypes(TypeRegistry& registry);

	// Registers the five methods. Tools (§13.8): prefab.create, prefab.instantiate, prefab.apply. All mutate and are
	// AllowedInBatch (every effect goes through EditorContext::Execute). SupportsDryRun: create, instantiate, revert, unpack
	// (apply rewrites the prefab and every instance and is not dry-runnable, §13.4's list).
	void RegisterPrefabMethods(MethodRegistry& methods);

}
