#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

// Visual mesh and terrain raycasting (§8.10), independent of a physics world/GPU. Main thread: scene and asset access.
// Physics masks use project layer names solely for filtering; intersections always use CPU mesh triangles and, since
// M26's W26.2 review, the triangles of terrain surfaces (Asset/TerrainGeometry.h).
namespace Engine {

	class AssetManager;
	class Scene;

	struct SceneRaycastRequest
	{
		RenderRay Ray{};
		float MaxDistance = 1000.0f; // positive, finite, metres along the normalized ray
		float MinDistance = 0.0f;    // finite [0,MaxDistance); both interval endpoints inclusive
		PhysicsLayerMask LayerMask = AllPhysicsLayers;
		float Alpha = 1.0f; // [0,1], rendered pose; callers refresh scene transforms as for extraction
	};

	struct SceneRaycastHit
	{
		UUID Entity{};
		float Distance = 0.0f;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);      // unit geometric world normal, oriented against the incoming ray
		glm::vec3 Barycentric = glm::vec3(0.0f); // weights of the three triangle vertices
		uint32_t Submesh = 0;
		// Index within the submesh. A terrain's (submesh 0): 2 * (z * (Resolution - 1) + x) for cell (x, z)'s triangle
		// (x, z), (x, z + 1), (x + 1, z + 1), one more for its triangle (x, z), (x + 1, z + 1), (x + 1, z).
		uint32_t Triangle = 0;
	};

	// Broad phase: transformed mesh AABBs; narrow phase: two-sided triangles, no texture alpha sampling. Transparent and
	// Mask geometry is geometric, intentionally not pixel-identical to GPU picking. Skips disabled, pending-destruction,
	// invisible and null-mesh entities, singular transforms and degenerate triangles. Missing meshes use the same CPU
	// placeholder as rendering. Chooses smallest distance in [MinDistance,MaxDistance], ties by UUID, submesh, triangle.
	// Rejects clipped triangles during traversal, not just after finding the nearest hit.
	// M26 (W26.2's review; Docs/Decisions/0037-m26-contract.md decisions 21 and 22): an effectively enabled entity's
	// Terrain is hit on the surface the renderer draws and its TerrainCollider collides with: each cell's two triangles of
	// Asset/TerrainGeometry.h (split along the cell's (x, z) to (x + 1, z + 1) diagonal) at the entity's rendered world
	// matrix, as submesh 0 and the Triangle numbering above, under the same distance, clipping and tie rules as meshes. A
	// terrain whose heightfield does not load as one, or whose Size or HeightScale is not positive and finite, draws
	// nothing and is skipped. IEEE-exact double arithmetic with floor and sqrt only (Scene is a simulation-path module).
	// Layer: nearest effectively enabled ancestor-or-self RigidBody.Layer, else own CharacterController.Layer, else (M26)
	// own TerrainCollider.Layer on a Terrain's entity (the layer of the implicit static body it holds), else Default.
	// Unknown names use Default as physics does. Parent accepted this visual mask policy in ADR0017;
	// it adds no render-layer field and requires no physics world.
	// InvalidArgument for nonfinite/zero direction, nonfinite origin, invalid distance interval or Alpha.
	// Empty optional is a successful miss. Uses the existing synchronous CPU AssetManager lookup and rendering's
	// loaded-asset/placeholder policy; it may load, cook or wait for a CPU mesh. Never waits for GPU or steps physics,
	// and does not mutate the scene.
	[[nodiscard]] Result<std::optional<SceneRaycastHit>> RaycastScene(const Scene& scene, AssetManager& assets,
		const PhysicsLayerTable& layers, const SceneRaycastRequest& request);

}
