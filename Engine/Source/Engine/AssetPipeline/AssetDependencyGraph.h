#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"

#include <map>
#include <set>
#include <span>
#include <vector>

namespace Engine {

	// Which assets reference which (Architecture §7.5): material -> textures, prefab -> meshes and materials, glTF ->
	// standalone textures it reuses, and from M13 script -> required scripts. Reimporting a dependency invalidates its
	// dependents ("changing Board.luau re-extracts and recompiles Game.luau"). The edges come from each import's
	// ImportResult::Dependencies; dependency files (§6.4) are tracked through their owner's cache manifest instead.
	//
	// A pure data structure: every answer is sorted by handle (or topologically, ties by handle), so nothing depends on
	// insertion history. Main thread; copyable (dry-run snapshots).
	class AssetDependencyGraph
	{
	public:
		AssetDependencyGraph() = default;

		// Replaces the outgoing edges of `asset` with `dependencies` (duplicates and self-edges ignored).
		void SetDependencies(AssetHandle asset, std::span<const AssetHandle> dependencies);

		// Removes `asset`'s outgoing edges. Edges into it stay (their owners still reference it, now missing).
		void RemoveAsset(AssetHandle asset);

		void Clear();

		// What `asset` references directly, sorted.
		[[nodiscard]] std::vector<AssetHandle> GetDependencies(AssetHandle asset) const;
		// What references `asset` directly, sorted.
		[[nodiscard]] std::vector<AssetHandle> GetDependents(AssetHandle asset) const;
		// Every asset that references `asset` directly or through others, sorted.
		[[nodiscard]] std::vector<AssetHandle> GetTransitiveDependents(AssetHandle asset) const;

		// The reimport order after `changed` changed: the changed assets and every transitive dependent, each once, ordered so
		// every asset comes after the assets it references (Kahn's algorithm, ties broken by handle). Assets on a reference
		// cycle (possible with script requires) follow the acyclic part, sorted by handle.
		[[nodiscard]] std::vector<AssetHandle> GetReimportOrder(std::span<const AssetHandle> changed) const;
	private:
		std::map<AssetHandle, std::set<AssetHandle>> m_Dependencies; // asset -> what it references
		std::map<AssetHandle, std::set<AssetHandle>> m_Dependents;   // asset -> what references it
	};

}
