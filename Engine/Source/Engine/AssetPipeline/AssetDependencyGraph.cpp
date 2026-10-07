#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the dependency graph.

namespace Engine {

	void AssetDependencyGraph::SetDependencies(AssetHandle /*asset*/, std::span<const AssetHandle> /*dependencies*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetDependencyGraph::RemoveAsset(AssetHandle /*asset*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AssetDependencyGraph::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetDependencies(AssetHandle /*asset*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetDependents(AssetHandle /*asset*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetTransitiveDependents(AssetHandle /*asset*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetReimportOrder(std::span<const AssetHandle> /*changed*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
