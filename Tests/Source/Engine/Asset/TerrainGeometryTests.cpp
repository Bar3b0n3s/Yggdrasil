#include "TestsPCH.h"

#include "Engine/Asset/TerrainGeometry.h"

#include <limits>
#include <optional>

// A terrain's surface (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream T-A): the placement and the
// triangulation the drawing, the physics and the queries share.

namespace Engine {

	TEST_SUITE("Asset")
	{
		// Decision 21: the surface between samples is the plane of Jolt's triangle, split along the cell's diagonal from (x, z)
		// to (x + 1, z + 1), so the drawing, the physics and the queries agree.
		TEST_CASE("TerrainGeometry: the surface between samples is Jolt's triangle, split along the cell's diagonal")
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

		// Decision 21: the samples sit at their placement, the far edges are inside, and a point off the terrain or a heightfield
		// that breaks its format's rules has no surface.
		TEST_CASE("TerrainGeometry: samples sit at their placement, the edges are inside and the outside has no surface")
		{
			// A 5 x 5 heightfield whose sample (x, z) is x * 1000 + z * 7000, over 8 x 4 metres, 65535 = 13.107 metres.
			HeightfieldData slope = MakeFlatHeightfield(5);
			for (uint32_t z = 0; z < 5; ++z)
			{
				for (uint32_t x = 0; x < 5; ++x)
					slope.Heights[x + 5 * z] = static_cast<uint16_t>(x * 1000 + z * 7000);
			}
			const TerrainExtent extent{ .Size = glm::vec2(8.0f, 4.0f), .HeightScale = 13.107f };
			for (uint32_t z = 0; z < 5; ++z)
			{
				for (uint32_t x = 0; x < 5; ++x)
				{
					CAPTURE(x);
					CAPTURE(z);
					const glm::vec3 position = GetTerrainSamplePosition(slope, extent, x, z);
					CHECK(position.x == static_cast<float>(x) * 2.0f);
					CHECK(position.z == static_cast<float>(z));
					CHECK(position.y == static_cast<float>(slope.Heights[x + 5 * z]) / 65535.0f * 13.107f);
					// At a sample the surface is the sample's height (the placements here are exact in floats).
					CHECK(SampleTerrainHeight(slope, extent, glm::vec2(position.x, position.z)) == position.y);
				}
			}
			// The surface is one plane (the samples are linear in x and z), so every triangle's normal is the plane's.
			const glm::vec3 expected = glm::normalize(glm::vec3(-(1000.0f / 65535.0f * 13.107f) / 2.0f, 1.0f, -(7000.0f / 65535.0f * 13.107f) / 1.0f));
			for (const glm::vec2 point : { glm::vec2(0.5f, 0.25f), glm::vec2(0.25f, 0.75f), glm::vec2(7.0f, 3.9f), glm::vec2(8.0f, 4.0f) })
			{
				CAPTURE(point.x);
				CAPTURE(point.y);
				const std::optional<glm::vec3> normal = SampleTerrainNormal(slope, extent, point);
				REQUIRE(normal.has_value());
				CHECK(normal->x == doctest::Approx(expected.x).epsilon(1e-5));
				CHECK(normal->y == doctest::Approx(expected.y).epsilon(1e-5));
				CHECK(normal->z == doctest::Approx(expected.z).epsilon(1e-5));
			}
			// The far corner is inside; just past any edge, a NaN coordinate and a broken heightfield or extent are not.
			CHECK(SampleTerrainHeight(slope, extent, glm::vec2(8.0f, 4.0f)) == GetTerrainSamplePosition(slope, extent, 4, 4).y);
			for (const glm::vec2 point : { glm::vec2(-0.001f, 1.0f), glm::vec2(8.001f, 1.0f), glm::vec2(1.0f, -0.001f), glm::vec2(1.0f, 4.001f),
					 glm::vec2(std::numeric_limits<float>::quiet_NaN(), 1.0f) })
			{
				CHECK_FALSE(SampleTerrainHeight(slope, extent, point).has_value());
				CHECK_FALSE(SampleTerrainNormal(slope, extent, point).has_value());
			}
			HeightfieldData broken = slope;
			broken.Heights.pop_back();
			CHECK_FALSE(SampleTerrainHeight(broken, extent, glm::vec2(1.0f)).has_value());
			CHECK_FALSE(SampleTerrainHeight(slope, TerrainExtent{ .Size = glm::vec2(0.0f, 4.0f), .HeightScale = 1.0f }, glm::vec2(0.0f)).has_value());
		}
	}

}
