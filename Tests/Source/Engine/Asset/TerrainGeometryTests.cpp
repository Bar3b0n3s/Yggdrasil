#include "TestsPCH.h"

#include "Engine/Asset/TerrainGeometry.h"

#include <optional>

// A terrain's surface (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream T-A): the placement and the
// triangulation the drawing, the physics and the queries share, skipped until stream T-A implements it.

namespace Engine {

	TEST_SUITE("Asset")
	{
		// Decision 21: the surface between samples is the plane of Jolt's triangle, split along the cell's diagonal from (x, z)
		// to (x + 1, z + 1), so the drawing, the physics and the queries agree.
		TEST_CASE("TerrainGeometry: the surface between samples is Jolt's triangle, split along the cell's diagonal" * doctest::skip(true))
		{
			// A 3 x 3 heightfield with one raised sample at (1, 1), over 2 x 2 metres, 65535 = 1 metre.
			HeightfieldData bump = MakeFlatHeightfield(3);
			bump.Heights[1 + 3 * 1] = MaxHeightfieldSample;
			const TerrainExtent extent{ .Size = glm::vec2(2.0f), .HeightScale = 1.0f };
			CHECK(GetTerrainSamplePosition(bump, extent, 1, 1) == glm::vec3(1.0f, 1.0f, 1.0f));
			CHECK(SampleTerrainHeight(bump, extent, glm::vec2(1.0f, 1.0f)) == 1.0f);
			// Cell (0, 0)'s diagonal runs from (0, 0) to (1, 1): along it the height rises linearly.
			CHECK(SampleTerrainHeight(bump, extent, glm::vec2(0.5f, 0.5f)) == 0.5f);
			// Off the diagonal, each point takes its triangle's plane: (0.75, 0.25) lies in the triangle (0, 0), (1, 1), (1, 0),
			// whose height there is fz = 0.25; (0.25, 0.75) in (0, 0), (0, 1), (1, 1), whose height is fx = 0.25.
			CHECK(SampleTerrainHeight(bump, extent, glm::vec2(0.75f, 0.25f)) == 0.25f);
			CHECK(SampleTerrainHeight(bump, extent, glm::vec2(0.25f, 0.75f)) == 0.25f);
			CHECK_FALSE(SampleTerrainHeight(bump, extent, glm::vec2(2.5f, 1.0f)).has_value());
			const std::optional<glm::vec3> normal = SampleTerrainNormal(MakeFlatHeightfield(3), extent, glm::vec2(1.0f, 0.5f));
			REQUIRE(normal.has_value());
			CHECK(*normal == glm::vec3(0.0f, 1.0f, 0.0f));
		}
	}

}
