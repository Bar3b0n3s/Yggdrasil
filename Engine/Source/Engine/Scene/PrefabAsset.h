#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabInstantiator.h"

#include <map>
#include <vector>

// Prefabs by handle (Architecture §5.5: "Scene::InstantiatePrefab(handle, transform, parent, rootID), the single path used
// by the editor, automation and scripts"). M3 instantiates in-memory Prefab objects (PrefabInstantiator); this is the
// Scene-level asset resolution on top of it that ADR 0006 decision 10 assigned to M6: the prefab asset comes from the
// AssetManager (PrefabData, the cooked canonical document) and is turned into a Prefab through Prefab::FromJson.

namespace Engine {

	class AssetManager;
	class IFieldSchemaSource;
	class TypeRegistry;

	// The prefab asset `handle` as a Prefab: AssetManager::Load, the PrefabData's document, Prefab::FromJson in Strict mode
	// (the cooked document passed strict loading at import). `schemas` are the script field schemas that resolve the
	// prefab's Script.Fields values (as PrefabOptions::Schemas; the caller's snapshot, passed explicitly so no caller loses
	// them): with them a valid value loads without a diagnostic; null only where no schemas exist, the values then being
	// kept with REFLECTION_VARIANT_UNRESOLVED. `report` receives the load's diagnostics. Main thread (the manager).
	// Errors: InvalidArgument for a null handle; NotFound for an unknown handle; Validation "'<path>' is a <Type>, not a
	// Prefab" for an asset of another type; the load's errors with the handle and path as context.
	[[nodiscard]] Result<Prefab> LoadPrefabAsset(AssetManager& assets, AssetHandle handle, const TypeRegistry& registry,
		const IFieldSchemaSource* schemas, LoadReport& report);

	// Instantiates the prefab asset `instance.PrefabHandle` into `scene` (LoadPrefabAsset with options.Schemas, then
	// PrefabInstantiator::Instantiate with `instance` and `options`), so the instance root's PrefabInstanceComponent names the
	// asset and PREFAB_MISSING_ASSET can be checked later. Atomic: on error the scene is unchanged. Errors: those of
	// LoadPrefabAsset and of PrefabInstantiator::Instantiate.
	[[nodiscard]] Result<Entity> InstantiatePrefabAsset(Scene& scene, AssetManager& assets, const PrefabInstantiateOptions& instance,
		const PrefabOptions& options, LoadReport& report);

	// Live nested prefabs (M20, Docs/Decisions/0031-m20-contract.md decision 8): `prefab` with every instance outermost in
	// its document whose prefab is a key of `versions` rebuilt from that version (PrefabInstantiator::UpdateInstance from the
	// overrides the document records for it, which RefreshOverrides then makes exactly what still differs from the version,
	// as an update of an instance in a scene does), the instances nested in it following it; the document's other entities
	// and instances stay as they are. What the editor writes when a prefab that `prefab` nests gets a new version. Errors:
	// those of reading the document and of PrefabInstantiator::UpdateInstance; Validation when the rewritten document fails
	// to load as a prefab. Stale overrides are reported in `report` (PREFAB_STALE_OVERRIDE).
	[[nodiscard]] Result<Prefab> UpdateNestedPrefabInstances(const Prefab& prefab, const TypeRegistry& registry,
		const std::map<AssetHandle, Prefab>& versions, const PrefabOptions& options, LoadReport& report);

	// `prefab` with the record of every instance outermost in its document whose prefab is a key of `versions` recomputed
	// against that version (PrefabInstantiator::RefreshOverrides), its entities left as they are: what a document written
	// from entities records for an instance that became outermost there (decision 8: prefab.create recomputes the overrides
	// of an instance it keeps live against its prefab, since Prefab::CreateFromEntity can only move the ones its enclosing
	// instance recorded for it). Errors: those of reading the document; Validation when the result fails to load as a
	// prefab.
	[[nodiscard]] Result<Prefab> RefreshNestedPrefabOverrides(const Prefab& prefab, const TypeRegistry& registry,
		const std::map<AssetHandle, Prefab>& versions, const PrefabOptions& options);

	// The prefab assets the instances in `prefab`'s document name (the Prefab components of its nested instance roots),
	// sorted and unique: the prefabs it nests directly.
	[[nodiscard]] std::vector<AssetHandle> GetNestedPrefabAssets(const Prefab& prefab, const TypeRegistry& registry);

}
