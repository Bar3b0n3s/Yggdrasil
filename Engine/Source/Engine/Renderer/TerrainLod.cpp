#include "EnginePCH.h"
#include "Engine/Renderer/TerrainLod.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Terrain levels of detail (TerrainLod.h; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C).
//
// A chunk's level-l mesh is the grid of its level's coordinates along each axis (every 2^l-th sample from its first, then its
// far corner) with each grid quad split along its (x, z) to (x + 1, z + 1) diagonal, as the cells are at level 0. Stitching
// collapses each of the chunk's own edge vertices that its coarser neighbour lacks onto the vertex before it along that edge
// (an edge collapse along a straight boundary, so the quads beside it fan to the coarser vertices) and drops the triangles
// that became degenerate: the edge's vertex sequence is then exactly the neighbour's, every 2^(l+1)-th sample and the far
// corner. A collapsed vertex is never a corner, and the vertex before an odd one is always one the neighbour has, so one
// collapse per vertex suffices; "TerrainLod: a chunk's triangles cover it exactly once at every level and stitch" checks that
// the result tiles the chunk. The level of a chunk compares its squared distance with squared powers of two, which is
// floor(log2(...)) exactly, with IEEE-exact operations only.

namespace Engine {

	namespace Utils {

		// The cells a chunk spans along one axis: from sample First over Cells cells (TerrainChunkCells, or fewer for the last).
		struct TerrainChunkSpan
		{
			uint32_t First = 0;
			uint32_t Cells = 0;
		};

		[[nodiscard]] static TerrainChunkSpan GetTerrainChunkSpan(const TerrainLodLayout& layout, uint32_t chunk)
		{
			const uint32_t cells = layout.Resolution - 1;
			const uint32_t first = chunk * TerrainChunkCells;
			return { .First = first, .Cells = std::min(TerrainChunkCells, cells - first) };
		}

		// The sample coordinates of a level's vertices along one axis: every `step`-th from the span's first, then its far end.
		[[nodiscard]] static std::vector<uint32_t> GetLevelCoordinates(const TerrainChunkSpan& span, uint32_t step)
		{
			std::vector<uint32_t> coordinates;
			for (uint32_t offset = 0; offset < span.Cells; offset += step)
				coordinates.push_back(span.First + offset);
			coordinates.push_back(span.First + span.Cells);
			return coordinates;
		}

		// Whether the edge coordinate `coordinate` of a level-`step` edge over `span` is one a coarser neighbour lacks: not a
		// multiple of twice the step from the span's start, and not the edge's far corner.
		[[nodiscard]] static bool IsStitchedAway(const TerrainChunkSpan& span, uint32_t coordinate, uint32_t step)
		{
			return coordinate != span.First + span.Cells && (coordinate - span.First) % (2 * step) != 0;
		}

		[[nodiscard]] static bool IsValidLayout(const TerrainLodLayout& layout)
		{
			return layout.Resolution >= MinHeightfieldResolution && layout.Resolution <= MaxHeightfieldResolution
				&& layout.ChunksPerSide == (layout.Resolution - 1 + TerrainChunkCells - 1) / TerrainChunkCells;
		}

	}

	TerrainLodLayout ComputeTerrainLodLayout(uint32_t resolution)
	{
		ENGINE_CORE_ASSERT(resolution >= MinHeightfieldResolution && resolution <= MaxHeightfieldResolution,
			"a heightfield of {} samples a side is outside [{}, {}]", resolution, MinHeightfieldResolution, MaxHeightfieldResolution);
		if (resolution < MinHeightfieldResolution || resolution > MaxHeightfieldResolution)
			return {};
		const uint32_t cells = resolution - 1;
		return { .Resolution = resolution, .ChunksPerSide = (cells + TerrainChunkCells - 1) / TerrainChunkCells };
	}

	std::vector<uint32_t> SelectTerrainLods(const TerrainLodLayout& layout, const TerrainExtent& extent, const glm::vec3& cameraLocal, float lodBias)
	{
		ENGINE_CORE_ASSERT(Utils::IsValidLayout(layout), "SelectTerrainLods needs a layout of ComputeTerrainLodLayout");
		ENGINE_CORE_ASSERT(lodBias > 0.0f, "SelectTerrainLods needs a positive LOD bias, got {}", lodBias);
		if (!Utils::IsValidLayout(layout))
			return {};
		const uint32_t chunks = layout.ChunksPerSide;
		const auto cells = static_cast<float>(layout.Resolution - 1);
		const float cellX = extent.Size.x / cells;
		const float cellZ = extent.Size.y / cells;
		// A chunk's nominal side scaled by the bias, the distance at which log2's argument is 1; a chunk leaves level 0 at twice it.
		const float levelDistance = static_cast<float>(TerrainChunkCells) * std::max(cellX, cellZ) * lodBias;
		const float levelDistanceSquared = levelDistance * levelDistance;

		std::vector<uint32_t> lods(static_cast<size_t>(chunks) * chunks, 0);
		for (uint32_t chunkZ = 0; chunkZ < chunks; ++chunkZ)
		{
			const Utils::TerrainChunkSpan spanZ = Utils::GetTerrainChunkSpan(layout, chunkZ);
			const float minZ = static_cast<float>(spanZ.First) * cellZ;
			const float maxZ = static_cast<float>(spanZ.First + spanZ.Cells) * cellZ;
			for (uint32_t chunkX = 0; chunkX < chunks; ++chunkX)
			{
				const Utils::TerrainChunkSpan spanX = Utils::GetTerrainChunkSpan(layout, chunkX);
				const float minX = static_cast<float>(spanX.First) * cellX;
				const float maxX = static_cast<float>(spanX.First + spanX.Cells) * cellX;
				// The camera's distance to the chunk's bounds, every height of the terrain included ([0, HeightScale]).
				const float dx = std::max({ minX - cameraLocal.x, 0.0f, cameraLocal.x - maxX });
				const float dy = std::max({ 0.0f - cameraLocal.y, 0.0f, cameraLocal.y - extent.HeightScale });
				const float dz = std::max({ minZ - cameraLocal.z, 0.0f, cameraLocal.z - maxZ });
				const float distanceSquared = dx * dx + dy * dy + dz * dz;
				// floor(log2(max(1, distance / levelDistance))): the largest l with distance >= 2^l levelDistance, at most
				// MaxTerrainLod. A comparison with a non-finite value is false, so such a camera keeps level 0.
				uint32_t lod = 0;
				float threshold = 4.0f * levelDistanceSquared; // (2^(lod + 1) levelDistance)^2
				while (lod < MaxTerrainLod && distanceSquared >= threshold)
				{
					++lod;
					threshold *= 4.0f;
				}
				lods[static_cast<size_t>(chunkZ) * chunks + chunkX] = lod;
			}
		}

		// Neighbours (sharing an edge) at most one level apart: lower each level to its neighbours' plus one until nothing
		// changes. Levels only decrease, so it ends.
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (uint32_t chunkZ = 0; chunkZ < chunks; ++chunkZ)
			{
				for (uint32_t chunkX = 0; chunkX < chunks; ++chunkX)
				{
					uint32_t& lod = lods[static_cast<size_t>(chunkZ) * chunks + chunkX];
					const auto limit = [&lod, &changed](uint32_t neighbour)
					{
						if (lod > neighbour + 1)
						{
							lod = neighbour + 1;
							changed = true;
						}
					};
					if (chunkX > 0)
						limit(lods[static_cast<size_t>(chunkZ) * chunks + chunkX - 1]);
					if (chunkX + 1 < chunks)
						limit(lods[static_cast<size_t>(chunkZ) * chunks + chunkX + 1]);
					if (chunkZ > 0)
						limit(lods[static_cast<size_t>(chunkZ - 1) * chunks + chunkX]);
					if (chunkZ + 1 < chunks)
						limit(lods[static_cast<size_t>(chunkZ + 1) * chunks + chunkX]);
				}
			}
		}
		return lods;
	}

	TerrainChunkMesh BuildTerrainChunkMesh(const TerrainLodLayout& layout, uint32_t chunkX, uint32_t chunkZ, uint32_t lod, const TerrainNeighbourLods& neighbours)
	{
		ENGINE_CORE_ASSERT(Utils::IsValidLayout(layout), "BuildTerrainChunkMesh needs a layout of ComputeTerrainLodLayout");
		ENGINE_CORE_ASSERT(chunkX < layout.ChunksPerSide && chunkZ < layout.ChunksPerSide, "chunk ({}, {}) of a {} x {} grid", chunkX, chunkZ,
			layout.ChunksPerSide, layout.ChunksPerSide);
		ENGINE_CORE_ASSERT(lod <= MaxTerrainLod, "level {} is above {}", lod, MaxTerrainLod);
		ENGINE_CORE_ASSERT(std::ranges::all_of(neighbours, [lod](uint32_t neighbour)
		{
			return neighbour + 1 >= lod && neighbour <= lod + 1;
		}),
			"a chunk's neighbours are within one level of its own");
		if (!Utils::IsValidLayout(layout) || chunkX >= layout.ChunksPerSide || chunkZ >= layout.ChunksPerSide || lod > MaxTerrainLod)
			return {};

		const uint32_t step = 1U << lod;
		const Utils::TerrainChunkSpan spanX = Utils::GetTerrainChunkSpan(layout, chunkX);
		const Utils::TerrainChunkSpan spanZ = Utils::GetTerrainChunkSpan(layout, chunkZ);
		const std::vector<uint32_t> xs = Utils::GetLevelCoordinates(spanX, step);
		const std::vector<uint32_t> zs = Utils::GetLevelCoordinates(spanZ, step);
		const auto columns = static_cast<uint32_t>(xs.size());
		const auto rows = static_cast<uint32_t>(zs.size());
		// -X, +X, -Z, +Z: whether that side's neighbour is coarser, so the side takes its vertices.
		std::array<bool, 4> stitched{};
		for (size_t side = 0; side < stitched.size(); ++side)
			stitched[side] = neighbours[side] > lod;

		// Each grid vertex's index after the stitching's collapses (row-major, row j of x coordinates).
		const auto gridIndex = [columns](uint32_t column, uint32_t row)
		{
			return row * columns + column;
		};
		std::vector<uint32_t> collapsed(static_cast<size_t>(columns) * rows);
		for (uint32_t row = 0; row < rows; ++row)
		{
			for (uint32_t column = 0; column < columns; ++column)
			{
				uint32_t target = gridIndex(column, row);
				if (column == 0 && stitched[0] && Utils::IsStitchedAway(spanZ, zs[row], step))
					target = gridIndex(column, row - 1);
				else if (column == columns - 1 && stitched[1] && Utils::IsStitchedAway(spanZ, zs[row], step))
					target = gridIndex(column, row - 1);
				else if (row == 0 && stitched[2] && Utils::IsStitchedAway(spanX, xs[column], step))
					target = gridIndex(column - 1, row);
				else if (row == rows - 1 && stitched[3] && Utils::IsStitchedAway(spanX, xs[column], step))
					target = gridIndex(column - 1, row);
				collapsed[gridIndex(column, row)] = target;
			}
		}

		// The triangles of every grid quad, split along its (x, z) to (x + 1, z + 1) diagonal and wound counter-clockwise
		// seen from +Y, after the collapses, without the degenerate ones.
		std::vector<uint32_t> triangles;
		triangles.reserve(static_cast<size_t>(columns - 1) * (rows - 1) * 6);
		const auto addTriangle = [&triangles, &collapsed](uint32_t a, uint32_t b, uint32_t c)
		{
			a = collapsed[a];
			b = collapsed[b];
			c = collapsed[c];
			if (a == b || b == c || a == c)
				return;
			triangles.insert(triangles.end(), { a, b, c });
		};
		for (uint32_t row = 0; row + 1 < rows; ++row)
		{
			for (uint32_t column = 0; column + 1 < columns; ++column)
			{
				const uint32_t corner = gridIndex(column, row);
				const uint32_t side = gridIndex(column, row + 1);
				const uint32_t diagonal = gridIndex(column + 1, row + 1);
				const uint32_t other = gridIndex(column + 1, row);
				addTriangle(corner, side, diagonal);
				addTriangle(corner, diagonal, other);
			}
		}

		// The vertices the triangles use, in grid order, and the triangles re-indexed into them.
		TerrainChunkMesh mesh;
		constexpr uint32_t Unused = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> compacted(collapsed.size(), Unused);
		for (const uint32_t index : triangles)
			compacted[index] = 0;
		for (uint32_t row = 0; row < rows; ++row)
		{
			for (uint32_t column = 0; column < columns; ++column)
			{
				uint32_t& slot = compacted[gridIndex(column, row)];
				if (slot == Unused)
					continue;
				slot = static_cast<uint32_t>(mesh.Samples.size());
				mesh.Samples.emplace_back(xs[column], zs[row]);
			}
		}
		mesh.Indices.reserve(triangles.size());
		for (const uint32_t index : triangles)
			mesh.Indices.push_back(compacted[index]);
		return mesh;
	}

}
