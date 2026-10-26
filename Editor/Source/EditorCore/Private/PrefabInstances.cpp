#include "EditorPCH.h"
#include "EditorCore/Private/PrefabInstances.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		bool IsOutermostPrefabInstance(ConstEntity entity)
		{
			if (!entity.HasComponent<PrefabInstanceComponent>())
				return false;
			const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
			return link == nullptr || link->InstanceRoot != entity.GetUUID() || !link->OuterPrefabEntityID.IsValid();
		}

		bool HasLostEnclosingInstance(ConstEntity entity)
		{
			if (!entity.HasComponent<PrefabInstanceComponent>() || IsOutermostPrefabInstance(entity))
				return false;
			const UUID outer = entity.GetComponent<PrefabLinkComponent>().OuterPrefabEntityID;
			for (ConstEntity ancestor = entity.GetParent(); ancestor.IsValid(); ancestor = ancestor.GetParent())
			{
				if (ancestor.HasComponent<PrefabInstanceComponent>() && PrefabInstantiator::DeriveInstanceID(ancestor.GetUUID(), outer) == entity.GetUUID())
					return false;
			}
			return true;
		}

		std::vector<std::pair<UUID, AssetHandle>> FindPrefabInstances(const Scene& scene, std::span<const AssetHandle> prefabs)
		{
			std::vector<std::pair<UUID, AssetHandle>> instances;
			scene.ForEachCanonical([&instances, prefabs](ConstEntity entity)
			{
				if (!IsOutermostPrefabInstance(entity))
					return;
				const AssetHandle handle = entity.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
				if (handle.IsValid() && (prefabs.empty() || std::binary_search(prefabs.begin(), prefabs.end(), handle)))
					instances.emplace_back(entity.GetUUID(), handle);
			});
			return instances;
		}

		bool IsInstancedInOpenScenes(const EditorContext& editor, std::span<const AssetHandle> prefabs)
		{
			if (editor.HasScene() && !FindPrefabInstances(editor.GetScene(), prefabs).empty())
				return true;
			const OpenSceneId addressed = editor.GetAddressedSceneId();
			return std::ranges::any_of(editor.GetOpenScenes(), [&editor, addressed, prefabs](OpenSceneId id)
			{
				const Scene* scene = id != addressed ? editor.FindScene(id) : nullptr;
				return scene != nullptr && !FindPrefabInstances(*scene, prefabs).empty();
			});
		}

	}

}
