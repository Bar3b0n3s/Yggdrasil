#include "TestsPCH.h"

#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Support/GlmApprox.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The terrain's height field shape (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream T-B): M11's
// frozen PhysicsShape.h gains HeightFieldShapeGeometry, appended last to the variant, and a body rests on it at the surface
// TerrainGeometry.h states (the acceptance "terrain rest height matches the analytic value"). Its scale is baked in as a
// mesh's (a negative component mirrors it, keeping its triangles), and it passes a mesh's size and range checks. That it never
// meets a mesh or another height field (Jolt collides it with convex shapes only) is PhysicsWorldTests.cpp's.

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

		// A field of `count` x `count` samples, `cell` metres apart, every height `height`.
		HeightFieldShapeGeometry MakeField(uint32_t count, float cell, float height = 0.0f)
		{
			return HeightFieldShapeGeometry{ .Heights = std::vector<float>(static_cast<size_t>(count) * count, height), .SampleCount = count, .CellSize = glm::vec2(cell) };
		}

		// The physics code that starts the message of a refusal of `geometry` at `scale`, or the empty string when it is built.
		std::string CreateRefusalCode(const HeightFieldShapeGeometry& geometry, const glm::vec3& scale = glm::vec3(1.0f))
		{
			const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = geometry, .Scale = scale } } });
			if (shape.has_value())
				return {};
			return std::string(GetPhysicsDiagnosticCode(shape.error()));
		}

		// A Static body of `geometry` at `scale` in `world`.
		BodyHandle CreateFieldBody(PhysicsWorld& world, const HeightFieldShapeGeometry& geometry, const glm::vec3& scale)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = geometry, .Scale = scale } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
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
		TEST_CASE("PhysicsShape: a sphere comes to rest on a height field at the terrain's analytic surface height")
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

		// Every sample count from Jolt's fewest (two blocks of 2) to the ledger's most, every height finite, positive cells,
		// and a mesh's size and range checks on the scaled field (its largest extent, so a flat field is valid).
		TEST_CASE("PhysicsShape: a height field needs 3 to 1025 samples a side, finite heights and cells, and a collider's size")
		{
			CHECK(CreateRefusalCode(MakeField(3, 1.0f)).empty());
			CHECK(CreateRefusalCode(MakeField(1025, 0.05f, 2.0f)).empty());
			CHECK(CreateRefusalCode(MakeField(2, 1.0f)) == PhysicsInvalidShapeCode);
			CHECK(CreateRefusalCode(MakeField(1026, 0.05f)) == PhysicsInvalidShapeCode);
			HeightFieldShapeGeometry missing = MakeField(4, 1.0f);
			missing.Heights.pop_back();
			CHECK(CreateRefusalCode(missing) == PhysicsInvalidShapeCode);
			HeightFieldShapeGeometry notFinite = MakeField(4, 1.0f);
			notFinite.Heights[5] = std::nanf("");
			CHECK(CreateRefusalCode(notFinite) == PhysicsInvalidShapeCode);
			notFinite.Heights[5] = 1.0e30f;
			CHECK(CreateRefusalCode(notFinite, glm::vec3(1.0f, 1.0e10f, 1.0f)) == PhysicsInvalidShapeCode);
			CHECK(CreateRefusalCode(MakeField(4, 0.0f)) == PhysicsInvalidShapeCode);
			HeightFieldShapeGeometry negativeCell = MakeField(4, 1.0f);
			negativeCell.CellSize.y = -1.0f;
			CHECK(CreateRefusalCode(negativeCell) == PhysicsInvalidShapeCode);
			// 3 mm across is a collider; 1.5 mm is not, and neither is a field more than 100 km across or beyond the range.
			CHECK(CreateRefusalCode(MakeField(4, 0.001f)).empty());
			CHECK(CreateRefusalCode(MakeField(4, 0.0005f)) == PhysicsInvalidShapeCode);
			CHECK(CreateRefusalCode(MakeField(1025, 100.0f)) == PhysicsInvalidShapeCode);
			CHECK(CreateRefusalCode(MakeField(4, 1.0f, 2.0e9f)) == PhysicsInvalidShapeCode);
			// A flat field is not convex either: a Dynamic body cannot take it.
			Result<Ref<const PhysicsShape>> flat = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = MakeField(3, 1.0f) } } });
			REQUIRE(flat.has_value());
			const Status dynamic = (*flat)->CheckDynamicBody(1.0f, PhysicsDofs::All);
			REQUIRE_FALSE(dynamic.has_value());
			CHECK(GetPhysicsDiagnosticCode(dynamic.error()) == PhysicsNonconvexDynamicCode);
		}

		// The scale is a mesh's: its magnitude stretches the cells and the heights, and a negative component mirrors the field,
		// which keeps its triangles (each cell split along the diagonal from (x, z) to (x + 1, z + 1) in its own samples) and
		// faces the way the scale turns it.
		TEST_CASE("PhysicsShape: a height field's scale stretches its cells and heights, and a mirrored one keeps its triangles")
		{
			// One raised sample, (1, 1) at 1 m: in cell (0, 0) the triangle with fz >= fx rises from 0 at (0, 1) to 1 at (1, 1),
			// so at (fx, fz) = (0.25, 0.75) the surface is 0.25 m high, where the other diagonal's triangulation would give 0.
			HeightFieldShapeGeometry field = MakeField(3, 1.0f);
			field.Heights[1 + 3 * 1] = 1.0f;

			// An even sample count, which Jolt does not pad (PhysicsShape.h), so its bounds are exactly the scaled field's.
			HeightFieldShapeGeometry even = MakeField(4, 1.0f);
			even.Heights[1 + 4 * 1] = 1.0f;
			Result<Ref<const PhysicsShape>> stretched =
				PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = even, .Scale = glm::vec3(2.0f, 0.5f, 3.0f) } } });
			REQUIRE(stretched.has_value());
			const Aabb bounds = (*stretched)->GetLocalBounds();
			CHECK(Test::ApproxEqual(bounds.Min, glm::vec3(0.0f), 1.0e-3f));
			CHECK(Test::ApproxEqual(bounds.Max, glm::vec3(6.0f, 0.5f, 9.0f), 1.0e-3f));

			for (const glm::vec3& scale : { glm::vec3(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f), glm::vec3(1.0f, 1.0f, -1.0f), glm::vec3(-1.0f, 1.0f, -1.0f) })
			{
				CAPTURE(scale);
				Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
				REQUIRE(world.has_value());
				static_cast<void>(CreateFieldBody(**world, field, scale));
				const glm::vec3 target(0.25f * scale.x, 0.0f, 0.75f * scale.z);
				const std::optional<PhysicsQueryHit> hit =
					(*world)->Raycast({ .Origin = target + glm::vec3(0.0f, 5.0f, 0.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .MaxDistance = 10.0f }, {});
				REQUIRE(hit.has_value());
				CHECK(hit->Point.y == doctest::Approx(0.25f).epsilon(1.0e-3));
				// The triangle rises along the field's own +X, which a mirrored X turns towards -X.
				CHECK(Test::ApproxEqual(hit->Normal, glm::normalize(glm::vec3(-scale.x, 1.0f, 0.0f)), 1.0e-3f));

				// Mirrored, the field still faces up: a sphere dropped on its flat corner rests on it.
				Result<Ref<const PhysicsShape>> sphere = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = SphereShapeGeometry{ .Radius = 0.25f } } } });
				REQUIRE(sphere.has_value());
				const glm::vec3 corner(1.75f * scale.x, 1.0f, 0.25f * scale.z);
				Result<BodyHandle> ball =
					(*world)->CreateBody({ .Shape = *sphere, .MotionType = PhysicsMotionType::Dynamic, .Pose = { .Position = corner }, .AllowedDofs = PhysicsDofs::TranslationY });
				REQUIRE(ball.has_value());
				for (uint32_t step = 0; step < 120; ++step)
					REQUIRE((*world)->Step(FixedDelta, 1).has_value());
				CHECK((*world)->GetPose(*ball).Position.y == doctest::Approx(0.25f - 0.02f).epsilon(0.05));
			}
		}
	}

}
