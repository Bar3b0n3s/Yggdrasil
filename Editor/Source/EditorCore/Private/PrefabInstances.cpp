#include "EditorPCH.h"
#include "EditorCore/Private/PrefabInstances.h"

#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
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

		std::optional<KeptPrefabMember> FindKeptPrefabMember(ConstEntity root, const std::set<UUID>& alsoRemoved)
		{
			// The subtree in canonical order (parents first) and its ids. `alsoRemoved` is read in place, never copied: entity.destroy
			// passes the same set, every listed root's subtree, for each of its roots.
			const Scene& scene = *root.GetScene();
			std::vector<ConstEntity> subtree;
			std::set<UUID> subtreeIds;
			std::vector<ConstEntity> stack = { root };
			while (!stack.empty())
			{
				const ConstEntity entity = stack.back();
				stack.pop_back();
				subtree.push_back(entity);
				subtreeIds.insert(entity.GetUUID());
				const std::span<const UUID> children = entity.GetChildren();
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					const ConstEntity childEntity = scene.FindEntityByID(*child);
					if (childEntity.IsValid())
						stack.push_back(childEntity);
				}
			}

			for (const ConstEntity entity : subtree)
			{
				const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
				if (link == nullptr || link->InstanceRoot == entity.GetUUID() || subtreeIds.contains(link->InstanceRoot) || alsoRemoved.contains(link->InstanceRoot))
					continue;
				const ConstEntity instanceRoot = scene.FindEntityByID(link->InstanceRoot);
				if (instanceRoot.IsValid() && instanceRoot.HasComponent<PrefabInstanceComponent>())
					return KeptPrefabMember{ .Member = entity, .InstanceRoot = instanceRoot };
			}
			return std::nullopt;
		}

	}

}
