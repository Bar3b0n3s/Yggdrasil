#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Engine {

	class EditorContext;

	// One level the inspector's Apply offers (Docs/Decisions/0031-m20-contract.md decision 9): an instance root the
	// inspected entity belongs to.
	struct PrefabLevelView
	{
		UUID Root{};
		AssetHandle Prefab{};
		std::string Label{}; // "<prefab file name> (<the root's entity path>)"
	};

	// One row of the inspector's override list: an override its outermost instance records, named for a person, with the
	// prefab.revert key that selects it alone.
	struct PrefabOverrideRow
	{
		PrefabOverride Override{};
		// The entity the override applies to: its scene path; for a RemoveEntity, which has no entity in the scene, the
		// removed entity's name in the outermost prefab (its id when the prefab cannot be read).
		std::string Target{};
		// What the override changes: "MeshRenderer.Materials", "Name", "Added Light", "Removed Light", "Removed", or for one
		// script slot its slot, the script the target holds there and the member, "Script slot 1 (Fire.luau): Fields"
		// (Docs/Decisions/0031-m20-contract.md decision 19).
		std::string Description{};
		Json RevertKey{}; // a PrefabOverrideKey: {path, kind, component?, field?}
	};

	// What the inspector shows about the prefab instance an entity belongs to (Architecture §5.5; Docs/Decisions/
	// 0031-m20-contract.md decisions 7 to 9 and 19). Plain presentation data: ids and text, no entity or asset pointers.
	struct PrefabInstanceView
	{
		UUID Instance{};                // the entity's innermost instance root: prefab.apply's `instance`
		UUID Outermost{};               // its outermost instance root, which records every override
		std::vector<UUID> EntityPath{}; // the entity's override path (PrefabInstantiator::GetOverridePath)
		// The levels of Instance (PrefabInstantiator::GetInstanceLevels), outermost first: the instances whose prefab can
		// receive Instance's overrides, the outermost being prefab.apply's default.
		std::vector<PrefabLevelView> Levels{};
		// The overrides of Outermost whose paths lie within Instance (every one when Instance is outermost), in record order:
		// those prefab.apply {instance: Instance} writes.
		std::vector<PrefabOverrideRow> Overrides{};
		AssetHandle OutermostPrefab{}; // the outermost root's prefab asset, whose document names removed entities
	};

	// Describes the instance `entity` belongs to: `entity` is an instance root or member (it has a PrefabLink component) of
	// the scene it lives in, which the editor addresses. Errors: InvalidArgument for an entity outside every instance;
	// InvalidState for a broken link chain (PrefabInstantiator::GetInstanceLevels).
	[[nodiscard]] Result<PrefabInstanceView> DescribePrefabInstance(EditorContext& editor, Entity entity);

	// The prefab.revert key (registry struct PrefabOverrideKey) that selects exactly `prefabOverride` among its instance's
	// overrides: its path, its kind's registered name, and its component and field when it has them.
	[[nodiscard]] Json MakePrefabOverrideKey(const EditorContext& editor, const PrefabOverride& prefabOverride);

}
