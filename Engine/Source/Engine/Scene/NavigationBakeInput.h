#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/NavModifierComponent.h"
#include "Engine/Scene/Components/NavigationComponent.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

// The navigation bake's input (M30, Docs/Decisions/0045-m30-navigation.md decision 6): a pure function of a scene that
// gathers what navigation.bake voxelizes, in world space and canonical order, and its hash, which the baked NavMesh stores
// and project.validate recomputes (NAV_MESH_STALE). One function, so the bake and the staleness check read the same input.
//   - The scene's Navigation component: the first, in canonical order, on an effectively enabled entity (a second one is
//     SCENE_DUPLICATE_UNIQUE_COMPONENT's to report), its settings checked against the component's ranges.
//   - The geometry: the bodies ComposePhysicsBodies (PhysicsComposition.h) plans for the scene, in its canonical order,
//     that are creatable, Static (a Static RigidBody or an implicit static body, never a Kinematic or Dynamic one), not
//     sensors, and on a layer the component's Layers names (every layer when empty). Each body's colliders are described by
//     DescribePhysicsBodyShape and triangulated first-party, in its collider order, then placed at the owner's world pose
//     (its world position and rotation; every scale is in the description):
//       - a box: its 12 triangles;
//       - a sphere: a UV sphere of NavBakeSphereSegments longitudes and NavBakeSphereRings latitudes, a capsule: two such
//         hemispheres joined by a cylinder, its vertices on the surface (DetMath's sines and cosines, so every configuration
//         places them alike);
//       - a mesh collider: its mesh's triangles, convex or not (a convex collider's hull is the mesh itself for a convex mesh;
//         a non-convex mesh marked Convex keeps its hollows here, Docs/Backlog.md §6);
//       - a terrain collider: its height field's cells, two triangles each split as Asset/TerrainGeometry.h splits them.
//     Triangles face outward (counter-clockwise seen from outside, so a floor's normal points up); a collider whose
//     placement mirrors it (a negative scale) has its triangles' winding turned back. A body whose shape cannot be described
//     (PHYSICS_INVALID_SHAPE, which the validator reports and a play session never creates) adds no triangle and is listed
//     in SkippedBodies.
//   - The modifiers: every effectively enabled entity's NavModifier, in canonical order: its box's four corners in the XZ
//     plane (the entity's world position, its world rotation's yaw, its world scale's magnitude times HalfExtents) and its
//     vertical extent.
// Determinism: canonical order throughout, IEEE operations and DetMath only, so the input, and its hash, are identical in
// every configuration. Main thread (the scene and the asset manager).

namespace Engine {

	class AssetManager;
	class PhysicsLayerTable;
	class Scene;

	// The tessellation of spheres and capsules (see the file comment).
	inline constexpr uint32_t NavBakeSphereSegments = 16;
	inline constexpr uint32_t NavBakeSphereRings = 8; // latitude bands of a whole sphere; a capsule's hemispheres take half each
	// The version of the hash's layout (HashNavigationBakeInput), raised whenever what the bake reads or how it is laid out
	// changes, so every committed mesh reads as stale until baked again.
	inline constexpr uint32_t NavigationBakeInputVersion = 1;

	// One NavModifier's box, in world space.
	struct NavBakeModifier
	{
		UUID Entity{};
		NavModifierKind Kind = NavModifierKind::NotWalkable;
		float Cost = 1.0f;
		// The corners in the XZ plane (x, z), going around the box: +X +Z, +X -Z, -X -Z, -X +Z in its yawed axes.
		std::array<glm::vec2, 4> Corners{};
		float MinY = 0.0f;
		float MaxY = 0.0f;
	};

	struct NavigationBakeInput
	{
		UUID NavigationEntity{};        // the entity that holds the Navigation component
		NavigationComponent Settings{}; // a copy; its NavMesh is never read by the bake or the hash
		std::vector<glm::vec3> Vertices{};
		std::vector<uint32_t> Indices{}; // 3 per triangle, each below Vertices.size()
		std::vector<NavBakeModifier> Modifiers{};
		std::vector<UUID> SkippedBodies{}; // owners of bodies whose shape could not be described, in canonical order
	};

	// The bake input of `scene` (see the file comment), its layer names resolved against `layers` and its colliders' meshes
	// and heightfields loaded through `assets`. Errors: NotFound when no effectively enabled entity holds a Navigation
	// component (the hint names the component); Validation naming the Navigation entity for settings outside the component's
	// ranges (an agent type count, name or size, a cell size, MinRegionArea) and for a layer name the project does not
	// define, and naming the body's owner or the NavModifier's entity for a placement that is not finite (a world transform
	// that overflows float's range, which Recast cannot voxelize).
	[[nodiscard]] Result<NavigationBakeInput> CollectNavigationBakeInput(const Scene& scene, const PhysicsLayerTable& layers, AssetManager& assets);

	// The bytes the hash covers, little-endian, floats as their bits: NavigationBakeInputVersion; the settings (each agent
	// type's name, radius, height, maximum slope and step; the layer names; CellSize, CellHeight and MinRegionArea); the
	// vertices and the indices; the modifiers (kind, a Cost box's cost, corners, vertical extent). Not the Navigation entity,
	// the skipped bodies, NavMesh or a NotWalkable box's Cost, which the bake never reads. Pure.
	[[nodiscard]] Buffer SerializeNavigationBakeInput(const NavigationBakeInput& input);

	// XXH64 of SerializeNavigationBakeInput: the NavMesh asset's InputHash. Pure.
	[[nodiscard]] uint64_t HashNavigationBakeInput(const NavigationBakeInput& input);

}
