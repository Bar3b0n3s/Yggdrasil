#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <span>
#include <utility>
#include <vector>

// The one lookup of the prefab instances of a scene, shared by EditorContext's prefab updates and the asset and prefab
// methods (§5.5).

namespace Engine {

	class Scene;

	namespace Utils {

		// The instance roots of `scene` whose PrefabInstanceComponent names one of `prefabs` (sorted; every instance of a valid
		// prefab handle when empty), each with its prefab's handle, in canonical order.
		[[nodiscard]] std::vector<std::pair<UUID, AssetHandle>> FindPrefabInstances(const Scene& scene, std::span<const AssetHandle> prefabs);

	}

}
