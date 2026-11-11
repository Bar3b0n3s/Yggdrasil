#include "TestsPCH.h"

#include "Engine/Renderer/TerrainLod.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

// Terrain levels of detail (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the acceptance
// "LOD seams are crack-free" as CPU geometry, skipped until stream T-C implements it.

namespace Engine {

	namespace {

		// The vertices a chunk's triangles use on its edge `side` (0 -X, 1 +X, 2 -Z, 3 +Z), as sample coordinates.
		std::set<std::pair<uint32_t, uint32_t>> GetEdgeVertices(const TerrainChunkMesh& mesh, uint32_t side, uint32_t edgeCoordinate)
		{
			std::set<std::pair<uint32_t, uint32_t>> used;
			for (const uint32_t index : mesh.Indices)
			{
				const glm::uvec2 sample = mesh.Samples[index];
				const bool onEdge = side < 2 ? sample.x == edgeCoordinate : sample.y == edgeCoordinate;
				if (onEdge)
					used.insert({ sample.x, sample.y });
			}
			return used;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TerrainLod: a heightfield is cut into chunks of 32 cells, and neighbouring levels differ by at most one" * doctest::skip(true))
		{
			const TerrainLodLayout layout = ComputeTerrainLodLayout(257);
			CHECK(layout.Resolution == 257);
			CHECK(layout.ChunksPerSide == 8);
			CHECK(ComputeTerrainLodLayout(100).ChunksPerSide == 4); // 99 cells: three whole chunks and one of 3 cells
			const TerrainExtent extent{ .Size = glm::vec2(512.0f), .HeightScale = 64.0f };
			const std::vector<uint32_t> lods = SelectTerrainLods(layout, extent, glm::vec3(0.0f, 10.0f, 0.0f), 1.0f);
			REQUIRE(lods.size() == 64);
			CHECK(lods.front() == 0); // the chunk under the camera
			CHECK(*std::max_element(lods.begin(), lods.end()) <= MaxTerrainLod);
			for (uint32_t z = 0; z < 8; ++z)
			{
				for (uint32_t x = 0; x < 8; ++x)
				{
					const uint32_t lod = lods[z * 8 + x];
					if (x + 1 < 8)
						CHECK((lod > lods[z * 8 + x + 1] ? lod - lods[z * 8 + x + 1] : lods[z * 8 + x + 1] - lod) <= 1);
					if (z + 1 < 8)
						CHECK((lod > lods[(z + 1) * 8 + x] ? lod - lods[(z + 1) * 8 + x] : lods[(z + 1) * 8 + x] - lod) <= 1);
				}
			}
			// A larger bias keeps detail farther.
			const std::vector<uint32_t> biased = SelectTerrainLods(layout, extent, glm::vec3(0.0f, 10.0f, 0.0f), 4.0f);
			CHECK(biased.back() <= lods.back());
		}

		// The acceptance: two chunks that share an edge use the same vertices along it, whatever their levels, so the surface
		// has no T-junction and no crack.
		TEST_CASE("TerrainLod: neighbouring chunks at different levels share every edge vertex" * doctest::skip(true))
		{
			const TerrainLodLayout layout = ComputeTerrainLodLayout(129);
			REQUIRE(layout.ChunksPerSide == 4);
			for (uint32_t fineLod = 0; fineLod < MaxTerrainLod; ++fineLod)
			{
				CAPTURE(fineLod);
				const uint32_t coarseLod = fineLod + 1;
				// Chunk (0, 0) at the finer level next to chunk (1, 0) at the coarser one, across the edge x = 32.
				const TerrainChunkMesh fine = BuildTerrainChunkMesh(layout, 0, 0, fineLod, { fineLod, coarseLod, fineLod, fineLod });
				const TerrainChunkMesh coarse = BuildTerrainChunkMesh(layout, 1, 0, coarseLod, { fineLod, coarseLod, coarseLod, coarseLod });
				REQUIRE_FALSE(fine.Indices.empty());
				REQUIRE_FALSE(coarse.Indices.empty());
				CHECK(fine.Indices.size() % 3 == 0);
				CHECK(GetEdgeVertices(fine, 1, TerrainChunkCells) == GetEdgeVertices(coarse, 0, TerrainChunkCells));
				// The coarser chunk's edge holds every 2^coarseLod-th sample and both corners.
				CHECK(GetEdgeVertices(coarse, 0, TerrainChunkCells).size() == TerrainChunkCells / (1U << coarseLod) + 1);
			}
		}
	}

}
