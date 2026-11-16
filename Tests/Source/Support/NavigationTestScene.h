#pragma once

#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <string_view>

// The navigation tests' arena (M30, Docs/Decisions/0045-m30-navigation.md; World Tree's brief 1, "How the game will check
// it"): a 20 x 20 m floor whose top is at y = 0, a pillar, a ramp up to a raised platform, a lava channel marked not walkable
// that stops short of the floor's +X edge (so paths go around it), and a costly patch. Shared by the bake input's, the
// builder's, the bake's and the queries' tests.

namespace Engine {

	class AssetManager;
	class Scene;

	namespace Test {

		// The arena's parts, in world space.
		inline constexpr glm::vec3 NavArenaPillarCenter{ -4.0f, 1.5f, 0.0f };
		inline constexpr glm::vec3 NavArenaPillarHalfExtents{ 0.5f, 1.5f, 0.5f };
		inline constexpr glm::vec3 NavArenaPlatformCenter{ 6.0f, 1.0f, 6.0f };
		inline constexpr glm::vec3 NavArenaPlatformHalfExtents{ 3.0f, 1.0f, 3.0f }; // its top at y = 2
		inline constexpr glm::vec3 NavArenaLavaCenter{ -3.0f, 0.0f, -5.0f };
		inline constexpr glm::vec3 NavArenaLavaHalfExtents{ 7.0f, 0.5f, 1.0f }; // x in [-10, 4], z in [-6, -4]
		inline constexpr glm::vec3 NavArenaCostCenter{ 5.0f, 0.0f, -1.0f };
		inline constexpr glm::vec3 NavArenaCostHalfExtents{ 1.0f, 0.5f, 1.0f };
		inline constexpr float NavArenaCost = 10.0f;

		// Fills `scene` with the arena: implicit static box colliders Floor, Pillar, Platform and Ramp (a 6.3 m slab rising
		// 2 m along +X, about 18 degrees, from the floor at x = -3 to the platform's -X edge at x = 3, z in [4.5, 7.5]), the
		// NavModifiers Lava (NotWalkable) and Mud (Cost NavArenaCost), and a Navigation component on an entity "Navigation"
		// with two agent types, "Small" (radius 0.4 m, height 1.8 m) and "Large" (radius 1 m, height 2 m), 0.2 m cells.
		// Returns the Navigation entity.
		Entity AddNavigationArena(Scene& scene);

		// The bake of `scene` with the default project's one layer (CollectNavigationBakeInput, then BuildNavMesh), both
		// required to succeed.
		[[nodiscard]] NavMeshData BakeNavigationScene(const Scene& scene, AssetManager& assets);

		// Whether the XZ point `point` lies inside the XZ footprint of the axis-aligned box at `center` with `halfExtents`,
		// grown by `margin` (negative shrinks it).
		[[nodiscard]] bool IsInsideFootprint(const glm::vec3& point, const glm::vec3& center, const glm::vec3& halfExtents, float margin);

	}

}
