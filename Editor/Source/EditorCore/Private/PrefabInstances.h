#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <span>
#include <utility>
#include <vector>

// The one lookup of the prefab instances of a scene, and of the open scenes, shared by EditorContext's prefab updates, the
// asset and prefab methods and the inspector (§5.5).

namespace Engine {

	class ConstEntity;
	class EditorContext;
	class Scene;

	namespace Utils {

		// True when `entity` is the root of an outermost prefab instance: it has PrefabInstanceComponent and is not nested
		// in another instance (its PrefabLink has no OuterPrefabEntityID; Docs/Decisions/0031-m20-contract.md decision 8).
		// Only an outermost instance records overrides and is rebuilt; its nested instances follow it.
		[[nodiscard]] bool IsOutermostPrefabInstance(ConstEntity entity);

		// True when `entity` is a nested instance root (it has PrefabInstanceComponent, and its PrefabLink names itself with an
		// OuterPrefabEntityID) that no ancestor instance root R derives its id from (Hash64(R, OuterPrefabEntityID)): an edit
		// moved it, or a subtree holding it, out of the instance that enclosed it. Strict loading refuses such a key
		// (SCENE_INCONSISTENT_PREFAB_LINK), so the edit's commit clears it and the instance becomes outermost, as the repair
		// of a file does (decision 8).
		[[nodiscard]] bool HasLostEnclosingInstance(ConstEntity entity);

		// The outermost instance roots of `scene` (IsOutermostPrefabInstance) whose PrefabInstanceComponent names one of
		// `prefabs` (sorted; every outermost instance of a valid prefab handle when empty), each with its prefab's handle, in
		// canonical order. A nested instance is never listed: an update rebuilds it with its outermost instance.
		[[nodiscard]] std::vector<std::pair<UUID, AssetHandle>> FindPrefabInstances(const Scene& scene, std::span<const AssetHandle> prefabs);

		// Whether an open scene of `editor`, the addressed one or another (Docs/Decisions/0031-m20-contract.md decision 15),
		// has an outermost instance of one of `prefabs` (sorted; FindPrefabInstances), so a new version of them must update
		// instances. False without an open scene.
		[[nodiscard]] bool IsInstancedInOpenScenes(const EditorContext& editor, std::span<const AssetHandle> prefabs);

	}

}
