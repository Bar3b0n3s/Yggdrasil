#include "EditorPCH.h"
#include "EditorCore/Private/PrefabInstances.h"

#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		std::vector<std::pair<UUID, AssetHandle>> FindPrefabInstances(const Scene& scene, std::span<const AssetHandle> prefabs)
		{
			std::vector<std::pair<UUID, AssetHandle>> instances;
			scene.ForEachCanonical([&instances, prefabs](ConstEntity entity)
			{
				const PrefabInstanceComponent* instance = entity.TryGetComponent<PrefabInstanceComponent>();
				if (instance == nullptr)
					return;
				const AssetHandle handle = instance->Prefab.GetHandle();
				if (handle.IsValid() && (prefabs.empty() || std::binary_search(prefabs.begin(), prefabs.end(), handle)))
					instances.emplace_back(entity.GetUUID(), handle);
			});
			return instances;
		}

	}

}
