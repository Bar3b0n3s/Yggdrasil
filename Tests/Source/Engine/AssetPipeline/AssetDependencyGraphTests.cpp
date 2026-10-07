#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetDependencyGraph.h"

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AssetDependencyGraph: dependents and transitive dependents are sorted")
		{
			// texture 1 <- material 2 <- prefab 4; texture 1 <- material 3; material 3 <- prefab 4.
			const AssetHandle texture(1), materialA(2), materialB(3), prefab(4);
			AssetDependencyGraph graph;
			graph.SetDependencies(materialA, std::vector<AssetHandle>{ texture, texture, materialA });
			graph.SetDependencies(materialB, std::vector<AssetHandle>{ texture });
			graph.SetDependencies(prefab, std::vector<AssetHandle>{ materialB, materialA });

			// Duplicates and self-edges are ignored.
			CHECK(graph.GetDependencies(materialA) == std::vector<AssetHandle>{ texture });
			CHECK(graph.GetDependents(texture) == std::vector<AssetHandle>{ materialA, materialB });
			CHECK(graph.GetDependencies(prefab) == std::vector<AssetHandle>{ materialA, materialB });
			CHECK(graph.GetTransitiveDependents(texture) == std::vector<AssetHandle>{ materialA, materialB, prefab });

			graph.SetDependencies(materialB, std::vector<AssetHandle>{});
			CHECK(graph.GetDependents(texture) == std::vector<AssetHandle>{ materialA });
			graph.RemoveAsset(prefab);
			CHECK(graph.GetDependents(materialA).empty());
			graph.Clear();
			CHECK(graph.GetDependencies(materialA).empty());
		}

		TEST_CASE("AssetDependencyGraph: the reimport order puts dependencies first, ties by handle")
		{
			const AssetHandle texture(10), materialA(30), materialB(20), prefab(5), unrelated(7);
			AssetDependencyGraph graph;
			graph.SetDependencies(materialA, std::vector<AssetHandle>{ texture });
			graph.SetDependencies(materialB, std::vector<AssetHandle>{ texture });
			graph.SetDependencies(prefab, std::vector<AssetHandle>{ materialA, materialB });
			graph.SetDependencies(unrelated, std::vector<AssetHandle>{});
			const std::vector<AssetHandle> order = graph.GetReimportOrder(std::vector<AssetHandle>{ texture });
			CHECK(order == std::vector<AssetHandle>{ texture, materialB, materialA, prefab });

			// A cycle (script requires, M13) follows the acyclic part, sorted by handle.
			const AssetHandle scriptA(100), scriptB(101);
			graph.SetDependencies(scriptA, std::vector<AssetHandle>{ scriptB });
			graph.SetDependencies(scriptB, std::vector<AssetHandle>{ scriptA });
			CHECK(graph.GetReimportOrder(std::vector<AssetHandle>{ scriptB }) == std::vector<AssetHandle>{ scriptA, scriptB });
		}
	}

}
