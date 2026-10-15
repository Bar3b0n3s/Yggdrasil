#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabInstantiator.h"

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

}
