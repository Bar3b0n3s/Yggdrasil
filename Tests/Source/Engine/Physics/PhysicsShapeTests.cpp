#include "TestsPCH.h"

#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Physics/PhysicsDiagnostics.h"

#include <format>
#include <limits>
#include <string>
#include <string_view>

// Collision shapes (Architecture §9.1 "Shapes are built only through ShapeSettings::Create", "Scales are checked with
// Shape::IsValidScale"; §9.2 "Collider identity in compounds"). Skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream A implements the shapes and removes the skips.

namespace Engine {

	namespace {

		ColliderShapeDescription MakeBoxCollider(const glm::vec3& halfExtents, const glm::vec3& position, uint32_t userData)
		{
			return ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents }, .Position = position, .UserData = userData };
		}

		// The PHYSICS_* code of a refused description, "<created>" when it was not refused, or "<ErrorCode>" without a code;
		// no assertion inside, so it can sit in a CHECK.
		std::string RefusalCodeOf(const BodyShapeDescription& description)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			if (shape.has_value())
				return "<created>";
			const std::string_view code = GetPhysicsDiagnosticCode(shape.error());
			return code.empty() ? std::format("<{}>", shape.error().GetCode()) : std::string(code);
		}

		BodyShapeDescription MakeSingleCollider(const PhysicsShapeGeometry& geometry)
		{
			BodyShapeDescription description;
			description.Colliders.push_back(ColliderShapeDescription{ .Geometry = geometry });
			return description;
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsShape: a single collider is placed in body space with its scale baked in" * doctest::skip(true))
		{
			BodyShapeDescription description;
			description.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f, 1.0f, 2.0f) },
				.Scale = glm::vec3(2.0f, 1.0f, 0.5f),
				.Position = glm::vec3(0.0f, 3.0f, 0.0f),
				.UserData = 7 });
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			CHECK((*shape)->GetColliderCount() == 1);
			const Aabb bounds = (*shape)->GetLocalBounds();
			// Jolt's convex radius rounds box corners inside the box, so the bounds are the box itself.
			CHECK(Test::ApproxEqual(bounds.Min, glm::vec3(-1.0f, 2.0f, -1.0f), 1.0e-4f));
			CHECK(Test::ApproxEqual(bounds.Max, glm::vec3(1.0f, 4.0f, 1.0f), 1.0e-4f));
			REQUIRE((*shape)->GetColliderLocalBounds(7).has_value());
			CHECK_FALSE((*shape)->GetColliderLocalBounds(8).has_value());
		}

		TEST_CASE("PhysicsShape: compound sub-shapes keep their collider user data and bounds" * doctest::skip(true))
		{
			BodyShapeDescription description;
			for (uint32_t index = 0; index < 20; ++index)
				description.Colliders.push_back(MakeBoxCollider(glm::vec3(0.5f), glm::vec3(static_cast<float>(index), 0.0f, 0.0f), index));
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			CHECK((*shape)->GetColliderCount() == 20);
			const Aabb whole = (*shape)->GetLocalBounds();
			CHECK(whole.Min.x == doctest::Approx(-0.5f).epsilon(1.0e-4));
			CHECK(whole.Max.x == doctest::Approx(19.5f).epsilon(1.0e-4));
			// Each collider is found by its user data, at its own place.
			for (const uint32_t index : { 0u, 7u, 19u })
			{
				CAPTURE(index);
				const std::optional<Aabb> collider = (*shape)->GetColliderLocalBounds(index);
				REQUIRE(collider.has_value());
				CHECK(collider->GetCenter().x == doctest::Approx(static_cast<float>(index)).epsilon(1.0e-4));
			}
		}

		TEST_CASE("PhysicsShape: a shared collider shape placed several times keeps each placement's user data and bounds" * doctest::skip(true))
		{
			// What the mesh shape cache does (§9.2): one built shape, placed by three colliders of one compound.
			const MeshShapeGeometry square{ .Vertices = { glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f),
												glm::vec3(-0.5f, 0.0f, 0.5f) },
				.Indices = { 0, 2, 1, 0, 3, 2 } };
			Result<Ref<const PhysicsShape>> piece = PhysicsShape::Create(MakeSingleCollider(square));
			REQUIRE_MESSAGE(piece.has_value(), piece.error().ToString());
			BodyShapeDescription track;
			for (uint32_t index = 0; index < 3; ++index)
			{
				track.Colliders.push_back(ColliderShapeDescription{ .Geometry = SharedShapeGeometry{ *piece },
					.Position = glm::vec3(static_cast<float>(index), 0.0f, 0.0f),
					.UserData = 20 + index });
			}
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(track);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			CHECK((*shape)->GetColliderCount() == 3);
			for (const uint32_t index : { 0u, 1u, 2u })
			{
				CAPTURE(index);
				const std::optional<Aabb> collider = (*shape)->GetColliderLocalBounds(20 + index);
				REQUIRE(collider.has_value());
				CHECK(collider->GetCenter().x == doctest::Approx(static_cast<float>(index)).epsilon(1.0e-4));
			}
			// A shared shape takes the scales its own geometry takes.
			CHECK(PhysicsShape::IsValidScale(SharedShapeGeometry{ *piece }, glm::vec3(2.0f, 1.0f, 3.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(SharedShapeGeometry{}, glm::vec3(1.0f)));
		}

		TEST_CASE("PhysicsShape: Jolt's shape errors become PHYSICS_INVALID_SHAPE with its message" * doctest::skip(true))
		{
			// Four coplanar points span no volume: Jolt's hull builder refuses them.
			const ConvexHullShapeGeometry flatHull{ .Points = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
														glm::vec3(1.0f, 0.0f, 1.0f) } };
			Result<Ref<const PhysicsShape>> hull = PhysicsShape::Create(MakeSingleCollider(flatHull));
			REQUIRE_FALSE(hull.has_value());
			CHECK(hull.error().GetCode() == ErrorCode::Validation);
			CHECK(GetPhysicsDiagnosticCode(hull.error()) == PhysicsInvalidShapeCode);
			CHECK(hull.error().GetMessageText().contains("collider 0"));

			// A mesh whose every triangle is degenerate leaves no triangle.
			const MeshShapeGeometry degenerateMesh{ .Vertices = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f) },
				.Indices = { 0, 1, 2 } };
			CHECK(RefusalCodeOf(MakeSingleCollider(degenerateMesh)) == PhysicsInvalidShapeCode);

			// An index out of range never reaches Jolt.
			const MeshShapeGeometry badIndex{ .Vertices = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) },
				.Indices = { 0, 1, 3 } };
			CHECK(RefusalCodeOf(MakeSingleCollider(badIndex)) == PhysicsInvalidShapeCode);

			// A shared shape must exist and hold one collider.
			CHECK(RefusalCodeOf(MakeSingleCollider(SharedShapeGeometry{})) == PhysicsInvalidShapeCode);
		}

		TEST_CASE("PhysicsShape: non-finite, non-positive and invalidly scaled shapes are refused before Jolt sees them" * doctest::skip(true))
		{
			CHECK(RefusalCodeOf(BodyShapeDescription{}) == PhysicsInvalidShapeCode);
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float infinity = std::numeric_limits<float>::infinity();
			for (const glm::vec3 halfExtents : { glm::vec3(0.0f, 1.0f, 1.0f), glm::vec3(-1.0f), glm::vec3(nan, 1.0f, 1.0f), glm::vec3(infinity) })
			{
				CAPTURE(halfExtents);
				BodyShapeDescription description;
				description.Colliders.push_back(MakeBoxCollider(halfExtents, glm::vec3(0.0f), 0));
				CHECK(RefusalCodeOf(description) == PhysicsInvalidShapeCode);
			}
			BodyShapeDescription zeroScale;
			zeroScale.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{}, .Scale = glm::vec3(1.0f, 0.0f, 1.0f) });
			CHECK(RefusalCodeOf(zeroScale) == PhysicsInvalidShapeCode);
			// Spheres take uniform scales only (the composition passes the largest axis first, §9.2).
			BodyShapeDescription stretchedSphere;
			stretchedSphere.Colliders.push_back(ColliderShapeDescription{ .Geometry = SphereShapeGeometry{}, .Scale = glm::vec3(1.0f, 2.0f, 1.0f) });
			CHECK(RefusalCodeOf(stretchedSphere) == PhysicsInvalidShapeCode);
			BodyShapeDescription badRotation;
			badRotation.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{}, .Rotation = glm::quat(2.0f, 0.0f, 0.0f, 0.0f) });
			CHECK(RefusalCodeOf(badRotation) == PhysicsInvalidShapeCode);
		}

		TEST_CASE("PhysicsShape: IsValidScale follows Jolt's rule per geometry" * doctest::skip(true))
		{
			CHECK(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(-1.0f, 1.0f, 1.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(0.0f, 1.0f, 1.0f)));
			CHECK(PhysicsShape::IsValidScale(SphereShapeGeometry{}, glm::vec3(2.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(SphereShapeGeometry{}, glm::vec3(1.0f, 2.0f, 1.0f)));
			CHECK(PhysicsShape::IsValidScale(CapsuleShapeGeometry{}, glm::vec3(0.5f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(CapsuleShapeGeometry{}, glm::vec3(1.0f, 1.0f, 2.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(std::numeric_limits<float>::quiet_NaN())));
		}
	}

}
