#include "TestsPCH.h"

#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <format>
#include <limits>
#include <string>
#include <string_view>

// Collision shapes (Architecture §9.1 "Shapes are built only through ShapeSettings::Create", "Scales are checked with
// Shape::IsValidScale"; §9.2 "Scale is baked into the shape", "Collider identity in compounds", "Mesh shapes are cached").

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
		TEST_CASE("PhysicsShape: a single collider is placed in body space with its scale baked in")
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
			// Boxes have no convex radius (§9.2 "Seams"), so the bounds are the box itself.
			CHECK(Test::ApproxEqual(bounds.Min, glm::vec3(-1.0f, 2.0f, -1.0f), 1.0e-4f));
			CHECK(Test::ApproxEqual(bounds.Max, glm::vec3(1.0f, 4.0f, 1.0f), 1.0e-4f));
			REQUIRE((*shape)->GetColliderLocalBounds(7).has_value());
			CHECK_FALSE((*shape)->GetColliderLocalBounds(8).has_value());
		}

		TEST_CASE("PhysicsShape: compound sub-shapes keep their collider user data and bounds")
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

		TEST_CASE("PhysicsShape: a shared collider shape placed several times keeps each placement's user data and bounds")
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

		TEST_CASE("PhysicsShape: Jolt's shape errors become PHYSICS_INVALID_SHAPE with its message")
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

		TEST_CASE("PhysicsShape: non-finite, non-positive and invalidly scaled shapes are refused before Jolt sees them")
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

		TEST_CASE("PhysicsShape: colliders smaller than MinColliderSize, larger than MaxColliderSize or beyond MaxPhysicsCoordinate are refused")
		{
			// The registry's smallest box (1 mm half extents) at scale 1 is exactly MinColliderSize across.
			const auto boxAt = [](const glm::vec3& halfExtents, const glm::vec3& scale, const glm::vec3& position)
			{
				BodyShapeDescription description;
				description.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents }, .Scale = scale, .Position = position });
				return description;
			};
			CHECK(RefusalCodeOf(boxAt(glm::vec3(0.001f), glm::vec3(1.0f), glm::vec3(0.0f))) == "<created>");
			CHECK(RefusalCodeOf(boxAt(glm::vec3(0.001f, 1.0f, 1.0f), glm::vec3(0.5f), glm::vec3(0.0f))) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(boxAt(glm::vec3(1.0f), glm::vec3(1.0e-4f), glm::vec3(0.0f))) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(MakeSingleCollider(SphereShapeGeometry{ .Radius = 0.0009f })) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(MakeSingleCollider(CapsuleShapeGeometry{ .HalfHeight = 1.0f, .Radius = 0.0009f })) == PhysicsInvalidShapeCode);
			// A flat mesh is measured on its largest extent: a 1 m plane is fine, a 1 mm one is not.
			const MeshShapeGeometry plane{ .Vertices = { glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, -0.5f), glm::vec3(0.0f, 0.0f, 0.5f) }, .Indices = { 0, 2, 1 } };
			CHECK(RefusalCodeOf(MakeSingleCollider(plane)) == "<created>");
			BodyShapeDescription tinyPlane = MakeSingleCollider(plane);
			tinyPlane.Colliders.front().Scale = glm::vec3(0.001f);
			CHECK(RefusalCodeOf(tinyPlane) == PhysicsInvalidShapeCode);

			// A collider larger than MaxColliderSize never reaches Jolt's penetration depth, whose products would overflow:
			// measured on its largest extent after scaling, whatever its geometry.
			const float largest = PhysicsShape::MaxColliderSize / 2.0f;
			CHECK(RefusalCodeOf(boxAt(glm::vec3(1.0f, largest, 1.0f), glm::vec3(1.0f), glm::vec3(0.0f))) == "<created>");
			CHECK(RefusalCodeOf(boxAt(glm::vec3(1.0f, largest, 1.0f), glm::vec3(1.0f, 1.01f, 1.0f), glm::vec3(0.0f))) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(MakeSingleCollider(SphereShapeGeometry{ .Radius = largest })) == "<created>");
			CHECK(RefusalCodeOf(MakeSingleCollider(SphereShapeGeometry{ .Radius = 1.01f * largest })) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(MakeSingleCollider(CapsuleShapeGeometry{ .HalfHeight = largest, .Radius = 1.0f })) == PhysicsInvalidShapeCode);
			BodyShapeDescription widePlane = MakeSingleCollider(plane);
			widePlane.Colliders.front().Scale = glm::vec3(2.0f * PhysicsShape::MaxColliderSize);
			CHECK(RefusalCodeOf(widePlane) == PhysicsInvalidShapeCode);
			const Result<Ref<const PhysicsShape>> huge = PhysicsShape::Create(MakeSingleCollider(SphereShapeGeometry{ .Radius = 1.0e7f }));
			REQUIRE_FALSE(huge.has_value());
			CHECK(huge.error().GetMessageText().contains("larger than"));

			// Geometry or placement beyond the physics range never reaches Jolt's broad phase.
			CHECK(RefusalCodeOf(boxAt(glm::vec3(2.0e9f), glm::vec3(1.0f), glm::vec3(0.0f))) == PhysicsInvalidShapeCode);
			CHECK(RefusalCodeOf(boxAt(glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(0.0f, -2.0e9f, 0.0f))) == PhysicsInvalidShapeCode);
			const Result<Ref<const PhysicsShape>> tiny = PhysicsShape::Create(MakeSingleCollider(SphereShapeGeometry{ .Radius = 0.0005f }));
			REQUIRE_FALSE(tiny.has_value());
			CHECK(tiny.error().GetMessageText().contains("collider 0"));
		}

		TEST_CASE("PhysicsShape: IsValidScale follows Jolt's rule per geometry")
		{
			CHECK(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(-1.0f, 1.0f, 1.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(0.0f, 1.0f, 1.0f)));
			CHECK(PhysicsShape::IsValidScale(SphereShapeGeometry{}, glm::vec3(2.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(SphereShapeGeometry{}, glm::vec3(1.0f, 2.0f, 1.0f)));
			CHECK(PhysicsShape::IsValidScale(CapsuleShapeGeometry{}, glm::vec3(0.5f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(CapsuleShapeGeometry{}, glm::vec3(1.0f, 1.0f, 2.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(BoxShapeGeometry{}, glm::vec3(std::numeric_limits<float>::quiet_NaN())));
			CHECK(PhysicsShape::IsValidScale(ConvexHullShapeGeometry{}, glm::vec3(1.0f, -2.0f, 3.0f)));
			CHECK(PhysicsShape::IsValidScale(MeshShapeGeometry{}, glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK_FALSE(PhysicsShape::IsValidScale(MeshShapeGeometry{}, glm::vec3(1.0f, 1.0e-7f, 1.0f)));
		}

		TEST_CASE("PhysicsShape: scales are baked into spheres, capsules and hulls, and a shared shape takes its own scale")
		{
			const auto boundsOf = [](const ColliderShapeDescription& collider) -> std::optional<Aabb>
			{
				Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { collider } });
				return shape.has_value() ? std::optional<Aabb>((*shape)->GetLocalBounds()) : std::nullopt;
			};
			// A capsule along Y: half height 0.5 plus radius 0.25, scaled by 2.
			const std::optional<Aabb> capsule = boundsOf({ .Geometry = CapsuleShapeGeometry{ .HalfHeight = 0.5f, .Radius = 0.25f }, .Scale = glm::vec3(2.0f) });
			REQUIRE(capsule.has_value());
			CHECK(Test::ApproxEqual(capsule->Max, glm::vec3(0.5f, 1.5f, 0.5f), 1.0e-4f));
			// A sphere under a mirroring uniform scale keeps a positive radius.
			const std::optional<Aabb> sphere = boundsOf({ .Geometry = SphereShapeGeometry{ .Radius = 0.5f }, .Scale = glm::vec3(-3.0f) });
			REQUIRE(sphere.has_value());
			CHECK(Test::ApproxEqual(sphere->Max, glm::vec3(1.5f), 1.0e-4f));
			// A unit cube hull stretched along X and mirrored along Y.
			const ConvexHullShapeGeometry cube{ .Points = { glm::vec3(-0.5f, -0.5f, -0.5f), glm::vec3(0.5f, -0.5f, -0.5f), glm::vec3(-0.5f, 0.5f, -0.5f),
													glm::vec3(0.5f, 0.5f, -0.5f), glm::vec3(-0.5f, -0.5f, 0.5f), glm::vec3(0.5f, -0.5f, 0.5f),
													glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(0.5f, 0.5f, 0.5f) } };
			const std::optional<Aabb> hull = boundsOf({ .Geometry = cube, .Scale = glm::vec3(4.0f, -1.0f, 1.0f) });
			REQUIRE(hull.has_value());
			CHECK(Test::ApproxEqual(hull->Min, glm::vec3(-2.0f, -0.5f, -0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(hull->Max, glm::vec3(2.0f, 0.5f, 0.5f), 1.0e-4f));

			// A shared shape is used as it is at scale 1 and wrapped in a scaled shape otherwise.
			Result<Ref<const PhysicsShape>> piece = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = cube } } });
			REQUIRE(piece.has_value());
			const std::optional<Aabb> shared = boundsOf({ .Geometry = SharedShapeGeometry{ *piece }, .Scale = glm::vec3(1.0f, 3.0f, 1.0f), .UserData = 4 });
			REQUIRE(shared.has_value());
			CHECK(Test::ApproxEqual(shared->Max, glm::vec3(0.5f, 1.5f, 0.5f), 1.0e-4f));
			// A shared shape of several colliders is refused (the cache shares one-collider shapes only).
			BodyShapeDescription pair;
			pair.Colliders.push_back(MakeBoxCollider(glm::vec3(0.5f), glm::vec3(0.0f), 0));
			pair.Colliders.push_back(MakeBoxCollider(glm::vec3(0.5f), glm::vec3(2.0f, 0.0f, 0.0f), 1));
			Result<Ref<const PhysicsShape>> compound = PhysicsShape::Create(pair);
			REQUIRE(compound.has_value());
			CHECK(RefusalCodeOf(MakeSingleCollider(SharedShapeGeometry{ *compound })) == PhysicsInvalidShapeCode);
		}

		TEST_CASE("PhysicsShape: a mirroring scale keeps a mesh's faces pointing outwards")
		{
			// A floor of two triangles facing up (+Y), mirrored along X: without reversing each triangle the faces would point
			// down, and a ball would fall through them (Jolt collides with front faces only).
			const MeshShapeGeometry floor{ .Vertices = { glm::vec3(-5.0f, 0.0f, -5.0f), glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(5.0f, 0.0f, 5.0f),
											   glm::vec3(-5.0f, 0.0f, 5.0f) },
				.Indices = { 0, 2, 1, 0, 3, 2 } };
			BodyShapeDescription mirrored;
			mirrored.Colliders.push_back(ColliderShapeDescription{ .Geometry = floor, .Scale = glm::vec3(-1.0f, 1.0f, 1.0f) });
			Result<Ref<const PhysicsShape>> floorShape = PhysicsShape::Create(mirrored);
			REQUIRE_MESSAGE(floorShape.has_value(), floorShape.error().ToString());
			Result<Ref<const PhysicsShape>> ballShape = PhysicsShape::Create(MakeSingleCollider(SphereShapeGeometry{ .Radius = 0.5f }));
			REQUIRE(ballShape.has_value());

			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE(world.has_value());
			REQUIRE((*world)->CreateBody({ .Shape = *floorShape, .MotionType = PhysicsMotionType::Static }).has_value());
			Result<BodyHandle> ball = (*world)->CreateBody({ .Shape = *ballShape, .Pose = { .Position = glm::vec3(0.0f, 2.0f, 0.0f) } });
			REQUIRE(ball.has_value());
			for (int step = 0; step < 120; ++step)
				REQUIRE((*world)->Step(1.0f / 60.0f, 1).has_value());
			CHECK((*world)->GetPose(*ball).Position.y == doctest::Approx(0.48f).epsilon(0.02));
		}
	}

}
