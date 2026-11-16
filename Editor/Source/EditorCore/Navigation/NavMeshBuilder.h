#pragma once

#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>

// The navigation bake's builder (M30, Docs/Decisions/0045-m30-navigation.md decision 7): Recast, which only this file's
// translation unit includes (Scripts/ModuleRules.json), so the editor's bake is the only code that links it and the Runtime
// never does. BuildNavMesh turns a scene's bake input (Scene/NavigationBakeInput.h) into the NavMesh asset's payload, one mesh
// per agent type, each built by Recast's steps in order:
//   1. The walkable triangles, marked first-party: a triangle is walkable when its normal's Y (cross(b - a, c - a),
//      normalized) exceeds DetMath::Cos of the agent type's MaxSlope, as rcMarkWalkableTriangles would mark it with the C
//      runtime's cosf, which the bake never calls (Vendor/Recast/VENDOR.md's audit).
//   2. The voxels: a heightfield of CellSize by CellHeight cells over the input's bounds (at most MaxNavBakeGridCells
//      along X and Z), the triangles rasterized with the agent type's step as the span-merge threshold, then Recast's three
//      span filters (low-hanging obstacles, ledges, low headroom) and the compact heightfield.
//   3. The modifiers: each NotWalkable box cut out (rcMarkConvexPolyArea with the null area) before the walkable area is
//      eroded by the agent type's radius, so agents keep their radius from it as from a wall; then each Cost box, in
//      ascending order of cost (canonical order among equal costs), so the highest cost wins where boxes overlap, as area
//      1 + i of the mesh's AreaCosts, the distinct costs sorted.
//   4. The mesh: the distance field and the watershed regions (no border, MinRegionArea / CellSize^2 cells as the smallest
//      region, NavBakeMergeRegionCells as the size below which regions merge), the contours (NavBakeMaxEdgeError cells of
//      simplification, edges at most NavBakeMaxEdgeLength metres), the polygons (NavMeshMaxVerticesPerPolygon vertices
//      each) and the detail mesh (samples NavBakeDetailSampleCells cells apart, NavBakeDetailMaxErrorCells cells of
//      height error).
// The agent type's settings in voxels: walkable height ceil(Height / CellHeight), climb floor(MaxStep / CellHeight), radius
// ceil(Radius / CellSize). An agent type over an input without triangles gets a mesh without polygons. Equal inputs build
// equal meshes in every configuration of one compiler (Recast's precise floating point), so two bakes write byte-identical
// files.
//
// Recast's process-level hooks (decision 2; Architecture §3 rule 5), the counterpart of Detour's
// (Engine/Navigation/NavigationRuntime.h): the editor (ApplicationSpecification::NavMeshBuildHooks) and the Tests main
// (ProcessContextSpecification::NavMeshBuildHooks) hand InstallNavMeshBuildHooks and RemoveNavMeshBuildHooks to the
// ProcessContext's Navigation step, which calls them after Detour's and before Detour's teardown.
//   - Allocator: rcAlloc and rcFree go to the C runtime's heap, as Recast's defaults do, through a counting hook, so the
//     removal verifies that no Recast allocation outlived a bake.
//   - Assert: a failed rcAssert is a programmer error (the bake's input is checked first-party before Recast reads it),
//     reported to the engine's assert handler with Recast's file and line. The editor has no Dist build, so Recast's
//     asserts are always compiled.

namespace Engine {

	struct NavigationBakeInput;

	// The bake's fixed Recast settings (see the file comment), and the largest grid it voxelizes: a heightfield of
	// 4,096 x 4,096 columns (819 metres at the default 0.2-metre cells) keeps Recast's memory within a few hundred
	// megabytes.
	inline constexpr int NavBakeMergeRegionCells = 400;
	inline constexpr float NavBakeMaxEdgeError = 1.3f;
	inline constexpr float NavBakeMaxEdgeLength = 12.0f;
	inline constexpr float NavBakeDetailSampleCells = 6.0f;
	inline constexpr float NavBakeDetailMaxErrorCells = 1.0f;
	inline constexpr int MaxNavBakeGridCells = 4096;
	// The most vertices and triangles a bake input holds (Recast counts them in int).
	inline constexpr size_t MaxNavBakeTriangles = 1U << 24;

	// The navigation mesh of `input` (see the file comment): one NavMeshAgent per agent type of input.Settings, in its
	// order, the area costs of its Cost modifiers, and InputHash = HashNavigationBakeInput(input). The result passes
	// ValidateNavMeshData. Errors: Validation when the input holds more than MaxNavBakeTriangles triangles, when its bounds
	// span more than MaxNavBakeGridCells cells along X or Z (the hint names a larger CellSize), when its modifiers have more
	// than MaxNavMeshAreaCosts distinct costs, when a Recast step fails (naming the step and the agent type, with Recast's
	// message) and when the built mesh is outside the format's limits (ValidateNavMeshData's message: too many vertices or
	// polygons, the hint names a larger CellSize); OutOfMemory when Recast cannot allocate its working data. Recast's working
	// data is freed on every path. Any thread (Recast's hooks are thread-safe).
	[[nodiscard]] Result<NavMeshData> BuildNavMesh(const NavigationBakeInput& input);

	// Sets Recast's allocator and assert hooks. A second call without RemoveNavMeshBuildHooks is a programmer error
	// (asserted). Main thread, before any bake.
	void InstallNavMeshBuildHooks();

	// Verifies (ENGINE_CORE_VERIFY) that no Recast allocation is live, then puts Recast's default hooks back. Does nothing
	// when not installed. Main thread.
	void RemoveNavMeshBuildHooks();

	// Whether InstallNavMeshBuildHooks ran without a RemoveNavMeshBuildHooks since.
	[[nodiscard]] bool AreNavMeshBuildHooksInstalled();

	// The number of Recast allocations not yet freed (rcAlloc minus rcFree of non-null blocks). Thread-safe.
	[[nodiscard]] size_t GetLiveNavMeshBuildAllocationCount();

}
