#pragma once

#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

// Terrain levels of detail (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22): the chunks a terrain is drawn
// in, the level each is drawn at, and the triangles of a chunk at its level, stitched to its neighbours' levels so the
// surface has no cracks (the acceptance "LOD seams are crack-free"). Pure CPU geometry over heightfield sample coordinates:
// the terrain's vertex shaders (stream T-C's Passes/Terrain.slang) place each vertex by Asset/TerrainGeometry.h's rule from
// the heightfield texture (GpuResourceCache::GetHeightfield), so a vertex shared by two chunks lands at one position.
//   - Chunks: the heightfield's Resolution - 1 cells along each axis cut into chunks of TerrainChunkCells cells (the last
//     row and column of chunks smaller when the cells do not divide), chunk (cx, cz) row-major from (0, 0).
//   - Levels: at level l a chunk's vertices are every 2^l samples (and always its own corner samples), at most
//     MaxTerrainLod (2^MaxTerrainLod = TerrainChunkCells: a chunk's two triangles). SelectTerrainLods picks each chunk's
//     level from the camera's distance to its bounds in the terrain's local space, scaled by LodBias, then lowers levels until
//     neighbouring chunks (sharing an edge) differ by at most one level.
//   - Stitching: a chunk next to a coarser neighbour (one level higher) uses only the neighbour's vertices along that edge,
//     fanning its own edge triangles to them, so both sides of every edge use the same vertex sequence: no T-junction, no
//     crack. Triangles wind counter-clockwise seen from above (+Y), each cell split along its diagonal from (x, z) to
//     (x + 1, z + 1) at level 0, as the physics and the queries split it.
//
// Frozen by the M26 contract C26.1; stream T-C implements it.

namespace Engine {

	inline constexpr uint32_t TerrainChunkCells = 32;
	inline constexpr uint32_t MaxTerrainLod = 5;

	// The chunk grid of a heightfield of `Resolution` samples a side.
	struct TerrainLodLayout
	{
		uint32_t Resolution = 0;
		uint32_t ChunksPerSide = 0;
	};

	// The levels of a chunk's four neighbours: -X, +X, -Z, +Z (a missing neighbour, at the terrain's edge, as the chunk's own).
	using TerrainNeighbourLods = std::array<uint32_t, 4>;

	// One chunk's geometry at a level: its vertices as heightfield sample coordinates (x, z) and a triangle list into them.
	struct TerrainChunkMesh
	{
		std::vector<glm::uvec2> Samples{};
		std::vector<uint32_t> Indices{};
	};

	// The chunk grid of `resolution` samples a side (MinHeightfieldResolution to MaxHeightfieldResolution, asserted). Pure.
	[[nodiscard]] TerrainLodLayout ComputeTerrainLodLayout(uint32_t resolution);

	// Each chunk's level, row-major (cz * ChunksPerSide + cx), for a camera at `cameraLocal` (the terrain's local space) and
	// `lodBias` (> 0): level floor(log2(max(1, distance / (TerrainChunkCells cell size * lodBias)))) through DetMath, at most
	// MaxTerrainLod, then neighbouring levels limited to one apart. Pure; deterministic.
	[[nodiscard]] std::vector<uint32_t> SelectTerrainLods(const TerrainLodLayout& layout, const TerrainExtent& extent, const glm::vec3& cameraLocal,
		float lodBias);

	// Chunk (chunkX, chunkZ)'s mesh at `lod` (at most MaxTerrainLod) with its neighbours at `neighbours` (each within one level
	// of `lod`, asserted), stitched as the file comment states. Pure.
	[[nodiscard]] TerrainChunkMesh BuildTerrainChunkMesh(const TerrainLodLayout& layout, uint32_t chunkX, uint32_t chunkZ, uint32_t lod,
		const TerrainNeighbourLods& neighbours);

}
