#include "EnginePCH.h"
#include "Engine/Renderer/TerrainLod.h"

// Terrain levels of detail (TerrainLod.h): contract stubs of M26's C26.1, implemented by stream T-C
// (Docs/Decisions/0037-m26-contract.md decision 22).

namespace Engine {

	TerrainLodLayout ComputeTerrainLodLayout(uint32_t /*resolution*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<uint32_t> SelectTerrainLods(const TerrainLodLayout& /*layout*/, const TerrainExtent& /*extent*/, const glm::vec3& /*cameraLocal*/,
		float /*lodBias*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	TerrainChunkMesh BuildTerrainChunkMesh(const TerrainLodLayout& /*layout*/, uint32_t /*chunkX*/, uint32_t /*chunkZ*/, uint32_t /*lod*/,
		const TerrainNeighbourLods& /*neighbours*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
