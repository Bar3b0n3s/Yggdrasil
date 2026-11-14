#include "TestsPCH.h"

#include "Engine/Renderer/TerrainLod.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

// Terrain levels of detail (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the acceptance
// "LOD seams are crack-free" as CPU geometry. Its GPU half, "SceneRenderer: a terrain whose chunks change level shows no gap",
// is TerrainPassTests.cpp's.

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

		// Twice the signed area of the triangle (a, b, c) of sample coordinates seen from +Y: positive when it winds
		// counter-clockwise there (+X to the right, +Z down).
		int64_t GetWinding(const glm::uvec2& a, const glm::uvec2& b, const glm::uvec2& c)
		{
			const int64_t abX = static_cast<int64_t>(b.x) - a.x;
			const int64_t abZ = static_cast<int64_t>(b.y) - a.y;
			const int64_t acX = static_cast<int64_t>(c.x) - a.x;
			const int64_t acZ = static_cast<int64_t>(c.y) - a.y;
			return abZ * acX - abX * acZ;
		}

		// Whether the point (x, z) lies strictly inside the counter-clockwise triangle (a, b, c).
		bool IsInside(const glm::uvec2& a, const glm::uvec2& b, const glm::uvec2& c, double x, double z)
		{
			const auto side = [x, z](const glm::uvec2& from, const glm::uvec2& to)
			{
				return (static_cast<double>(to.y) - from.y) * (x - from.x) - (static_cast<double>(to.x) - from.x) * (z - from.y);
			};
			return side(a, b) > 0.0 && side(b, c) > 0.0 && side(c, a) > 0.0;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TerrainLod: a heightfield is cut into chunks of 32 cells, and neighbouring levels differ by at most one")
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
		TEST_CASE("TerrainLod: neighbouring chunks at different levels share every edge vertex")
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

		// Beyond the shared edges: a chunk's mesh tiles exactly its cells at every level, against every combination of coarser
		// neighbours, for whole chunks and for the smaller last chunks of a heightfield whose cells 32 does not divide. Two
		// probe points in every cell (one in each of its level-0 triangles) each lie inside exactly one triangle, every triangle
		// winds counter-clockwise seen from +Y with a positive area, and each side holds the vertices of its level (every 2^l-th
		// sample and the far corner) or, next to a coarser neighbour, the neighbour's (every 2^(l+1)-th and the far corner).
		TEST_CASE("TerrainLod: a chunk's triangles cover it exactly once at every level and stitch")
		{
			struct ChunkCase
			{
				uint32_t Resolution = 0;
				uint32_t ChunkX = 0;
				uint32_t ChunkZ = 0;
			};
			// Whole chunks, the 3-cell chunks of 99 cells along X, along Z and both, the 1-cell chunks of 33 cells, the one
			// 2-cell chunk of the smallest heightfield and the last chunk of the largest.
			const std::array<ChunkCase, 8> cases = { {
				{ .Resolution = 100, .ChunkX = 1, .ChunkZ = 2 },
				{ .Resolution = 100, .ChunkX = 3, .ChunkZ = 0 },
				{ .Resolution = 100, .ChunkX = 0, .ChunkZ = 3 },
				{ .Resolution = 100, .ChunkX = 3, .ChunkZ = 3 },
				{ .Resolution = 34, .ChunkX = 1, .ChunkZ = 0 },
				{ .Resolution = 34, .ChunkX = 1, .ChunkZ = 1 },
				{ .Resolution = 3, .ChunkX = 0, .ChunkZ = 0 },
				{ .Resolution = 1025, .ChunkX = 31, .ChunkZ = 31 },
			} };
			for (const ChunkCase& chunk : cases)
			{
				const TerrainLodLayout layout = ComputeTerrainLodLayout(chunk.Resolution);
				const uint32_t cells = chunk.Resolution - 1;
				const uint32_t firstX = chunk.ChunkX * TerrainChunkCells;
				const uint32_t firstZ = chunk.ChunkZ * TerrainChunkCells;
				const uint32_t lastX = std::min(firstX + TerrainChunkCells, cells);
				const uint32_t lastZ = std::min(firstZ + TerrainChunkCells, cells);
				for (uint32_t lod = 0; lod <= MaxTerrainLod; ++lod)
				{
					for (uint32_t stitch = 0; stitch < 16; ++stitch)
					{
						if (lod == MaxTerrainLod && stitch != 0)
							continue; // no level is coarser than the last
						CAPTURE(chunk.Resolution);
						CAPTURE(chunk.ChunkX);
						CAPTURE(chunk.ChunkZ);
						CAPTURE(lod);
						CAPTURE(stitch);
						TerrainNeighbourLods neighbours{};
						for (uint32_t side = 0; side < 4; ++side)
							neighbours[side] = (stitch & (1U << side)) != 0 ? lod + 1 : lod;
						const TerrainChunkMesh mesh = BuildTerrainChunkMesh(layout, chunk.ChunkX, chunk.ChunkZ, lod, neighbours);
						REQUIRE_FALSE(mesh.Indices.empty());
						REQUIRE(mesh.Indices.size() % 3 == 0);
						for (const uint32_t index : mesh.Indices)
							REQUIRE(index < mesh.Samples.size());
						// Every vertex is a sample of the chunk, used by a triangle, and listed once.
						std::set<std::pair<uint32_t, uint32_t>> distinct;
						for (const glm::uvec2& sample : mesh.Samples)
						{
							CHECK(sample.x >= firstX);
							CHECK(sample.x <= lastX);
							CHECK(sample.y >= firstZ);
							CHECK(sample.y <= lastZ);
							distinct.insert({ sample.x, sample.y });
						}
						CHECK(distinct.size() == mesh.Samples.size());
						CHECK(std::set<uint32_t>(mesh.Indices.begin(), mesh.Indices.end()).size() == mesh.Samples.size());

						// The probes of each cell, counted per triangle over the cells of the triangle's bounding box.
						const uint32_t width = lastX - firstX;
						const uint32_t depth = lastZ - firstZ;
						std::vector<uint32_t> hits(static_cast<size_t>(width) * depth * 2, 0);
						bool counterClockwise = true;
						for (size_t first = 0; first < mesh.Indices.size(); first += 3)
						{
							const glm::uvec2& a = mesh.Samples[mesh.Indices[first]];
							const glm::uvec2& b = mesh.Samples[mesh.Indices[first + 1]];
							const glm::uvec2& c = mesh.Samples[mesh.Indices[first + 2]];
							counterClockwise = counterClockwise && GetWinding(a, b, c) > 0;
							for (uint32_t z = std::min({ a.y, b.y, c.y }); z < std::max({ a.y, b.y, c.y }); ++z)
							{
								for (uint32_t x = std::min({ a.x, b.x, c.x }); x < std::max({ a.x, b.x, c.x }); ++x)
								{
									const size_t cell = (static_cast<size_t>(z - firstZ) * width + (x - firstX)) * 2;
									// One probe in each of the cell's level-0 triangles: above its diagonal (z > x) and below.
									hits[cell] += IsInside(a, b, c, x + 0.3137, z + 0.7219) ? 1U : 0U;
									hits[cell + 1] += IsInside(a, b, c, x + 0.7219, z + 0.3137) ? 1U : 0U;
								}
							}
						}
						CHECK(counterClockwise);
						CHECK(std::ranges::all_of(hits, [](uint32_t count)
						{
							return count == 1;
						}));

						// Each side's vertices: its level's, or its coarser neighbour's.
						const auto expectedSide = [lod, stitch](uint32_t side, uint32_t first, uint32_t last)
						{
							const uint32_t step = 1U << ((stitch & (1U << side)) != 0 ? lod + 1 : lod);
							std::set<uint32_t> coordinates;
							for (uint32_t coordinate = first; coordinate < last; coordinate += step)
								coordinates.insert(coordinate);
							coordinates.insert(last);
							return coordinates;
						};
						const auto actualSide = [&mesh](uint32_t side, uint32_t edge)
						{
							std::set<uint32_t> coordinates;
							for (const auto& [x, z] : GetEdgeVertices(mesh, side, edge))
								coordinates.insert(side < 2 ? z : x);
							return coordinates;
						};
						CHECK(actualSide(0, firstX) == expectedSide(0, firstZ, lastZ));
						CHECK(actualSide(1, lastX) == expectedSide(1, firstZ, lastZ));
						CHECK(actualSide(2, firstZ) == expectedSide(2, firstX, lastX));
						CHECK(actualSide(3, lastZ) == expectedSide(3, firstX, lastX));
					}
				}
			}
		}

		// A chunk's level is the floor of the base-2 logarithm of its distance over its side times the bias: it steps exactly at
		// the powers of two, and never above MaxTerrainLod.
		TEST_CASE("TerrainLod: a chunk's level is floor(log2) of its distance over its scaled side, at most the last level")
		{
			// 33 samples over 32 m: one chunk of 1 m cells, so level 0 ends at 64 m (twice the chunk's 32 m side) times the bias.
			const TerrainLodLayout layout = ComputeTerrainLodLayout(33);
			REQUIRE(layout.ChunksPerSide == 1);
			const TerrainExtent extent{ .Size = glm::vec2(32.0f), .HeightScale = 8.0f };
			const auto levelAt = [&layout, &extent](float distance, float bias)
			{
				// Straight out along +X from the chunk's far side, at a height inside its bounds.
				const std::vector<uint32_t> lods = SelectTerrainLods(layout, extent, glm::vec3(32.0f + distance, 4.0f, 16.0f), bias);
				REQUIRE(lods.size() == 1);
				return lods.front();
			};
			CHECK(levelAt(0.0f, 1.0f) == 0);
			CHECK(levelAt(63.99f, 1.0f) == 0);
			CHECK(levelAt(64.0f, 1.0f) == 1);
			CHECK(levelAt(127.99f, 1.0f) == 1);
			CHECK(levelAt(128.0f, 1.0f) == 2);
			CHECK(levelAt(256.0f, 1.0f) == 3);
			CHECK(levelAt(512.0f, 1.0f) == 4);
			CHECK(levelAt(1024.0f, 1.0f) == 5);
			CHECK(levelAt(1.0e9f, 1.0f) == MaxTerrainLod);
			// The bias scales the distances: 4 keeps level 0 four times as far, 0.25 leaves it four times as soon.
			CHECK(levelAt(255.0f, 4.0f) == 0);
			CHECK(levelAt(256.0f, 4.0f) == 1);
			CHECK(levelAt(15.99f, 0.25f) == 0);
			CHECK(levelAt(16.0f, 0.25f) == 1);
			// Above the terrain's height range, the vertical distance counts too.
			const std::vector<uint32_t> above = SelectTerrainLods(layout, extent, glm::vec3(16.0f, 8.0f + 64.0f, 16.0f), 1.0f);
			REQUIRE(above.size() == 1);
			CHECK(above.front() == 1);
		}
	}

}
