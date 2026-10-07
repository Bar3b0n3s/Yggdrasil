#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

#include <vector>

// The asset dependencies of an entity document (SceneImporter, PrefabImporter): the handles its components reference
// (§7.5 "prefab -> meshes and materials").

namespace Engine {

	class TypeRegistry;

	namespace Utils {

		// Every valid asset handle the components of a canonical scene or prefab document ("Entities", each with
		// "Components") reference through AssetRef fields, found through `registry` (frozen): AssetRef fields at any depth of
		// arrays, maps and structs, and Variant values whose schema resolves to an AssetRef without external schemas (a
		// PrefabOverride's Value targeting a MeshRenderer's Mesh). Unknown components, which have no schema, contribute
		// nothing. Sorted and unique. Pure.
		[[nodiscard]] std::vector<AssetHandle> CollectDocumentAssetReferences(const Json& document, const TypeRegistry& registry);

	}

}
