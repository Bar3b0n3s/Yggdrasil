#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/WaterConstants.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The Water pass's geometry and records (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record; Renderer/
// WaterPass.h), pure CPU: what WaterPass draws a water with, and the snapshot water's record its shaders read
// (Shared/WaterConstants.h).
//
// The clipmap. WaterLevelCount levels of one grid of WaterGridCells x WaterGridCells square cells around the camera: level 0
// a whole grid of WaterFinestCellSize-metre cells, each level i above it a ring of cells twice level i - 1's whose hole of
// WaterHoleCells x WaterHoleCells cells holds level i - 1 exactly. Each level's corner is snapped to twice its own cell
// (PlaceWaterLevels), so its vertices keep their world points while the camera moves (they never swim), and each level's
// outer edge keeps only every other vertex, collapsing the ones between onto their predecessors as TerrainLod's stitching
// does, so it meets the next level's coarser hole edge without a crack. Past the last level a flat skirt of quads reaches
// out to at least WaterHorizonDistance from the camera in every direction, its inner edge on the last level's outer one.
// A level's mesh is one of WaterMeshCount (BuildWaterMesh): level 0's grid, a ring for each of the four places its hole can
// take, and the skirt. Each is a list of cells (x, z) from the level's corner, wound counter-clockwise seen from +Y; a vertex's
// rest point is the level's corner plus its cell times the level's cell size.
//
// The record (MakeWaterRecord). The renderer may not include Physics, so the water's frame (its base point and its local X
// axis's horizontal direction) follows Physics/WaterWaves.h's MakeWaterFrame here, in double: the translation, the X column's
// (x, z) normalized, the Z column's turned when X is within a millionth of vertical, the world's axes when both are. A
// Rectangle's half extents are Size over two times the lengths of World's X and Z columns (Scene/WaterQueries.h). The waves'
// phases at the record's origin are computed in double from the item's float wave numbers, directions and phases, wrapped
// into [0, 2 pi), so the shader's float phases stay exact to a few millionths of a radian however far the water's own origin
// lies.
//
// Deterministic and pure: every value is a function of its arguments, the same in every configuration.

namespace Engine {

	inline constexpr uint32_t WaterHoleCells = WaterGridCells / 2;
	// The skirt's reach, metres from the camera.
	inline constexpr double WaterHorizonDistance = 100000.0;
	// The meshes: level 0's grid, the four rings (hole first cell (16 + (m - 1) % 2, 16 + (m - 1) / 2) for mesh m), the skirt.
	inline constexpr uint32_t WaterMeshCount = 6;
	inline constexpr uint32_t WaterSkirtMesh = 5;

	// One clipmap level in the world: the world (x, z) of its corner (its cell (0, 0)), its cell size in metres, and for a
	// ring the first cell of its hole (16 or 17 on each axis).
	struct WaterLevelPlacement
	{
		glm::dvec2 Corner = glm::dvec2(0.0);
		double CellSize = 0.0;
		glm::uvec2 Hole = glm::uvec2(0);
	};

	// The levels around the camera's world (x, z): level 0's corner floor((camera - 32 c0) / 2 c0) * 2 c0 per axis, level i's
	// floor((corner(i - 1) - 32 c(i - 1)) / 2 c(i)) * 2 c(i), its hole (corner(i - 1) - corner(i)) / c(i). Every corner is
	// a multiple of twice its level's cell, and the camera lies at least 30 cells of each level inside it (Shared/
	// WaterConstants.h's fade). Pure; exact in double.
	[[nodiscard]] std::array<WaterLevelPlacement, WaterLevelCount> PlaceWaterLevels(const glm::dvec2& camera);

	// The mesh level `level` of `levels` draws with: 0 for level 0, a ring's by its hole.
	[[nodiscard]] uint32_t GetWaterLevelMesh(const WaterLevelPlacement& level, uint32_t index);

	// The skirt's outer edge, in the last level's cells: 32 plus the inner edge's offset from the level's centre times
	// WaterSkirtScale, which puts it at least WaterHorizonDistance beyond the camera.
	[[nodiscard]] int32_t GetWaterSkirtScale();

	// A mesh's cells and its triangle list (WaterMeshCount: the file comment).
	struct WaterGridMesh
	{
		std::vector<glm::i16vec2> Cells{};
		std::vector<uint32_t> Indices{};
	};

	// Mesh `mesh` (below WaterMeshCount, asserted; empty past it): its vertices in row-major cell order, each used by a
	// triangle, and its triangles without degenerate ones.
	[[nodiscard]] WaterGridMesh BuildWaterMesh(uint32_t mesh);

	// The world (x, z) square level `index` of `levels` covers (the skirt: index WaterLevelCount, out to its outer edge).
	struct WaterSquare
	{
		glm::dvec2 Min = glm::dvec2(0.0);
		glm::dvec2 Max = glm::dvec2(0.0);
	};
	[[nodiscard]] WaterSquare GetWaterLevelSquare(const std::array<WaterLevelPlacement, WaterLevelCount>& levels, uint32_t index);

	// The world (x, z) bounds of a Rectangle water's footprint; the whole plane for an Ocean.
	[[nodiscard]] WaterSquare GetWaterFootprintBounds(const WaterItem& water);

	// The record of `water` (Shared/WaterConstants.h) in a view whose level 0 corner is `origin` and whose camera is at world
	// (x, z) `camera`, among `waterCount` waters, sampling planar reflection slot `planarSlot` (WaterNoPlanarSlot for none).
	[[nodiscard]] WaterRecord MakeWaterRecord(const WaterItem& water, const glm::dvec2& origin, const glm::dvec2& camera, uint32_t waterCount,
		uint32_t planarSlot = WaterNoPlanarSlot);

	// W47.3: whether the Underwater pass records for `camera` over `waters`, a conservative test on the snapshot alone: the
	// near plane's rectangle (its four corners, through the inverse of the projection times the view) meets a water's region
	// that may lie under its surface: below its base plus the sum of its waves' amplitudes, and for a Rectangle inside its
	// footprint widened by the sum of its waves' Q A and above its floor (Depth below its base). A camera whose projection
	// cannot be inverted is never under water.
	[[nodiscard]] bool IsNearPlaneUnderWater(std::span<const WaterItem> waters, const CameraData& camera);

	// W47.3 (the planar opt-in, after W47.3's review, findings 1 and 7): the screen rectangle, in `camera`'s normalized device
	// coordinates clamped to [-1, 1], that a planar reflection of `water` covers, or nullopt when its surface is not on screen.
	//   - A Rectangle: its footprint box (ComputePlanarReflectionScreenRect), raised and lowered by the sum of its waves'
	//     amplitudes and widened in X and Z by WaterPlanarDistortion (the surface's sampling offset) plus the sum of its
	//     waves' Q A (how far its drawn edge moves sideways), in metres through the World's column lengths.
	//   - An Ocean: its base plane below the horizon: the square of the plane WaterHorizonDistance around the camera's (x, z),
	//     clipped in clip space to w > 0 (in front of the camera) and bounded by its clipped corners' projections, so a camera
	//     looking up at the sky has none and a level camera's rectangle tops out at the horizon.
	[[nodiscard]] std::optional<PlanarReflectionScreenRect> ComputeWaterPlanarReflectionScreenRect(const WaterItem& water, const CameraData& camera);

}
