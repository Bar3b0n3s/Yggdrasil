#include "TestsPCH.h"

#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

// The terrain's height field shape (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream T-B): M11's
// frozen PhysicsShape.h gains HeightFieldShapeGeometry, appended last to the variant, and a body rests on it at the surface
// TerrainGeometry.h states (the acceptance "terrain rest height matches the analytic value"). Skipped until stream T-B
// builds the shape; the variant's order is the contract's (live).

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;

		// A 33 x 33 ramp over 32 x 32 metres rising along +X to 8 metres, with a bump at its middle, as a height field.
		HeightFieldShapeGeometry MakeRampField(HeightfieldData& heightfield, const TerrainExtent& extent)
		{
			heightfield = MakeFlatHeightfield(33);
			for (uint32_t z = 0; z < 33; ++z)
			{
				for (uint32_t x = 0; x < 33; ++x)
					heightfield.Heights[x + 33 * z] = static_cast<uint16_t>((x * MaxHeightfieldSample) / 32);
			}
			heightfield.Heights[16 + 33 * 16] = static_cast<uint16_t>(heightfield.Heights[16 + 33 * 16] + 4096);
			HeightFieldShapeGeometry geometry;
			geometry.SampleCount = heightfield.Resolution;
			geometry.CellSize = extent.Size / 32.0f;
			for (uint32_t z = 0; z < 33; ++z)
			{
				for (uint32_t x = 0; x < 33; ++x)
					geometry.Heights.push_back(GetTerrainSamplePosition(heightfield, extent, x, z).y);
			}
			return geometry;
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsShape: the height field is the variant's last alternative, after the six of M11")
		{
			static_assert(std::variant_size_v<PhysicsShapeGeometry> == 7);
			static_assert(std::is_same_v<std::variant_alternative_t<5, PhysicsShapeGeometry>, SharedShapeGeometry>);
			static_assert(std::is_same_v<std::variant_alternative_t<6, PhysicsShapeGeometry>, HeightFieldShapeGeometry>);
			CHECK(PhysicsShapeGeometry(BoxShapeGeometry{}).index() == 0);
			CHECK(PhysicsShapeGeometry(HeightFieldShapeGeometry{}).index() == 6);
		}

		// The acceptance: a sphere dropped onto the terrain rests with its centre one radius (less the penetration slop, §9.1)
		// above the terrain's analytic surface, the height of Jolt's triangle under it, at a point inside a cell off its
		// diagonal, and a Dynamic body cannot take the shape.
		TEST_CASE("PhysicsShape: a sphere comes to rest on a height field at the terrain's analytic surface height" * doctest::skip(true))
		{
			const TerrainExtent extent{ .Size = glm::vec2(32.0f), .HeightScale = 8.0f };
			HeightfieldData heightfield;
			const HeightFieldShapeGeometry field = MakeRampField(heightfield, extent);
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = field } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			CHECK((*shape)->CheckDynamicBody(1.0f, PhysicsDofs::All).error().GetCode() == ErrorCode::Validation);

			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			REQUIRE((*world)->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static }).has_value());
			// A point of cell (16, 16) below its diagonal, where the bump's triangle slopes.
			const glm::vec2 point(16.75f, 16.25f);
			const std::optional<float> surface = SampleTerrainHeight(heightfield, extent, point);
			REQUIRE(surface.has_value());
			Result<Ref<const PhysicsShape>> sphere = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = SphereShapeGeometry{ .Radius = 0.25f } } } });
			REQUIRE(sphere.has_value());
			Result<BodyHandle> ball = (*world)->CreateBody({ .Shape = *sphere,
				.MotionType = PhysicsMotionType::Dynamic,
				.Pose = { .Position = glm::vec3(point.x, *surface + 2.0f, point.y) },
				.AllowedDofs = PhysicsDofs::TranslationY });
			REQUIRE(ball.has_value());
			for (uint32_t step = 0; step < 300; ++step)
				REQUIRE((*world)->Step(FixedDelta, 1).has_value());
			// The sphere touches the sloped plane at its normal, so its centre rests 0.25 / n.y above the surface at the point
			// (its X and Z locked), less the slop along the normal.
			const std::optional<glm::vec3> normal = SampleTerrainNormal(heightfield, extent, point);
			REQUIRE(normal.has_value());
			const float expected = *surface + (0.25f - 0.02f) / normal->y;
			CHECK((*world)->GetPose(*ball).Position.y == doctest::Approx(expected).epsilon(0.01));
		}
	}

}
