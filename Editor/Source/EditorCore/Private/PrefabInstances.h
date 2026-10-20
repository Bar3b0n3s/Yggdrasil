#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Entity.h"

#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

// The one lookup of the prefab instances of a scene, shared by EditorContext's prefab updates and the asset and prefab
// methods (§5.5), and the one rule for removing their members, which entity.destroy and prefab.create follow.

namespace Engine {

	class Scene;

	namespace Utils {

		// The instance roots of `scene` whose PrefabInstanceComponent names one of `prefabs` (sorted; every instance of a valid
		// prefab handle when empty), each with its prefab's handle, in canonical order.
		[[nodiscard]] std::vector<std::pair<UUID, AssetHandle>> FindPrefabInstances(const Scene& scene, std::span<const AssetHandle> prefabs);

		// A member of a prefab instance that a removal must leave in place, with the root of its instance.
		struct KeptPrefabMember
		{
			ConstEntity Member{};
			ConstEntity InstanceRoot{};
		};

		// Removing members of a prefab instance (§1.2, §5.5): no override records a removed member that is not its instance's
		// root, so the instance's next update (§5.5 "Update") rebuilds it, or fails when another entity took the member's id
		// (prefab.create {replaceWithInstance} gives it to the new instance's root). `root` is removed with its subtree,
		// together with the entities `alsoRemoved` names (the other entities the same edit removes; it may hold `root`'s
		// subtree too). Returns the first entity of `root`'s subtree, in canonical order (parents first), whose
		// PrefabLinkComponent names an instance root other than itself that exists, has PrefabInstanceComponent and is not
		// removed; nullopt when there is none (instance roots, user children without PrefabLinkComponent and entities outside
		// every instance never count). This is the rule's one place: entity.destroy and prefab.create {replaceWithInstance}
		// refuse the removal it finds, until M20's RemoveEntity override records a deletion instead (ADR 0028 decision 16).
		[[nodiscard]] std::optional<KeptPrefabMember> FindKeptPrefabMember(ConstEntity root, const std::set<UUID>& alsoRemoved);

	}

}
