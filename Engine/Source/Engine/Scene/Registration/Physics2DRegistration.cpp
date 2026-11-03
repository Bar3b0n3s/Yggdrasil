#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <cmath>
#include <cstddef>
#include <format>

// The Physics2D category (M21, Docs/Decisions/0032-m21-contract.md decision 9; ADR 0028's phase-2 ledger: a milestone's new
// components go in categories of their own after Scripting): the 2D world's bodies and colliders, laid into a second Jolt
// world (Scene/Physics2DSystem.h). The bounds are the Physics category's (PhysicsRegistration.cpp), so every value the 2D
// world hands Jolt is valid (§9.1); BodyType and MotionQuality are that category's enums. The composition rules that span
// entities (PHYSICS2D_MIXED_WORLDS, PHYSICS2D_OFF_PLANE and the shared PHYSICS_* codes) are the 2D validator's
// (Scene/Physics2DValidation.h), not rules of a single component. C21.2 appends the tilemap collider (decision 25).

namespace Engine {

	namespace Utils {

		// The smallest mass of a 2D body, in kilograms (RigidBody's MinPhysicsMass).
		constexpr float MinBody2DMass = 0.001f;

		// A PolygonCollider2D holds 3 to MaxPolygonCollider2DPoints finite points.
		static void ValidatePolygonCollider2D(const PolygonCollider2DComponent& polygon, ValidationContext& context)
		{
			if (polygon.Points.size() < 3 || polygon.Points.size() > MaxPolygonCollider2DPoints)
				context.Error("Points", std::format("must hold 3 to {} points, not {}", MaxPolygonCollider2DPoints, polygon.Points.size()));
			for (size_t index = 0; index < polygon.Points.size(); ++index)
			{
				const glm::vec2 point = polygon.Points[index];
				if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > MaxPhysicsCoordinate
					|| std::abs(point.y) > MaxPhysicsCoordinate)
					context.Error("Points", std::format("point {} must be finite and within {} m of the origin", index, MaxPhysicsCoordinate));
			}
		}

		static void MakePolygonCollider2DValid(PolygonCollider2DComponent& polygon, Random& random)
		{
			if (polygon.Points.size() > MaxPolygonCollider2DPoints)
				polygon.Points.resize(MaxPolygonCollider2DPoints);
			while (polygon.Points.size() < 3)
				polygon.Points.push_back(glm::vec2(random.NextFloat(), random.NextFloat()));
			for (glm::vec2& point : polygon.Points)
			{
				if (!std::isfinite(point.x) || std::abs(point.x) > MaxPhysicsCoordinate)
					point.x = 0.0f;
				if (!std::isfinite(point.y) || std::abs(point.y) > MaxPhysicsCoordinate)
					point.y = 0.0f;
			}
		}

	}

	void RegisterPhysics2DComponents(TypeRegistry& registry)
	{
		RegisterComponent<RigidBody2DComponent>(registry, "RigidBody2D",
			"Simulates the entity in the 2D world: moves along world X and Y and rotates about world Z.")
			.Category("Physics2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Type", &RigidBody2DComponent::Type, "Static never moves; Kinematic is moved by script; Dynamic is simulated.")
			.Field("Mass", &RigidBody2DComponent::Mass, "Mass in kilograms (Dynamic only).",
				{ .Min = Utils::MinBody2DMass, .Max = static_cast<double>(MaxPhysicsMass), .Unit = "kg" })
			.Field("Friction", &RigidBody2DComponent::Friction,
				"The friction coefficient of the body's surfaces; a contact uses the geometric mean of both bodies' values.", { .Min = 0.0 })
			.Field("Restitution", &RigidBody2DComponent::Restitution,
				"Bounciness, from no bounce (0) to a perfectly elastic bounce (1); a contact uses the larger of both bodies' values.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("LinearDamping", &RigidBody2DComponent::LinearDamping, "How quickly linear velocity decays, per second.", { .Min = 0.0 })
			.Field("AngularDamping", &RigidBody2DComponent::AngularDamping, "How quickly angular velocity decays, per second.", { .Min = 0.0 })
			.Field("GravityFactor", &RigidBody2DComponent::GravityFactor, "The multiplier of the project's 2D gravity for this body (0 floats).")
			.Field("MotionQuality", &RigidBody2DComponent::MotionQuality, "Discrete or continuous (LinearCast) collision detection.")
			.Field("AllowSleeping", &RigidBody2DComponent::AllowSleeping, "Whether the body may sleep when it comes to rest.")
			.Field("FixedRotation", &RigidBody2DComponent::FixedRotation, "Whether a Dynamic body is kept from rotating.")
			.Field("Layer", &RigidBody2DComponent::Layer, "Physics layer name from project settings (the 3D world's layers).")
			.Field("MaxLinearVelocity", &RigidBody2DComponent::MaxLinearVelocity, "The speed the body is clamped to, in metres per second.",
				{ .Min = 0.0, .Unit = "m/s" })
			.Field("MaxAngularVelocity", &RigidBody2DComponent::MaxAngularVelocity, "The angular speed the body is clamped to, in radians per second.",
				{ .Min = 0.0, .Unit = "rad/s" })
			.Field("EnhancedInternalEdgeRemoval", &RigidBody2DComponent::EnhancedInternalEdgeRemoval,
				"Removes ghost collisions with internal edges of compound shapes; set it on bodies that roll or slide across tiles.")
			.Field("InitialLinearVelocity", &RigidBody2DComponent::InitialLinearVelocity,
				"The velocity along world X and Y the body starts with, in metres per second.", { .Unit = "m/s" })
			.Field("InitialAngularVelocity", &RigidBody2DComponent::InitialAngularVelocity,
				"The angular velocity about world Z the body starts with, in radians per second, counter-clockwise seen from +Z.",
				{ .Unit = "rad/s" });

		RegisterComponent<BoxCollider2DComponent>(registry, "BoxCollider2D", "A rectangle collider in the entity's XY plane.")
			.Category("Physics2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("HalfExtents", &BoxCollider2DComponent::HalfExtents, "Half the rectangle's size along local X and Y; at least 1 mm.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("Offset", &BoxCollider2DComponent::Offset, "The rectangle's centre relative to the entity, in its XY plane.", { .Unit = "m" })
			.Field("Angle", &BoxCollider2DComponent::Angle, "The rectangle's rotation about the entity's Z axis, counter-clockwise, in degrees.",
				{ .Unit = "deg" })
			.Field("IsTrigger", &BoxCollider2DComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<CircleCollider2DComponent>(registry, "CircleCollider2D", "A circle collider in the entity's XY plane.")
			.Category("Physics2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Radius", &CircleCollider2DComponent::Radius, "The circle's radius; at least 1 mm.", { .Min = MinColliderDimension, .Unit = "m" })
			.Field("Offset", &CircleCollider2DComponent::Offset, "The circle's centre relative to the entity, in its XY plane.", { .Unit = "m" })
			.Field("IsTrigger", &CircleCollider2DComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<PolygonCollider2DComponent>(registry, "PolygonCollider2D", "A convex polygon collider in the entity's XY plane.")
			.Category("Physics2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Points", &PolygonCollider2DComponent::Points,
				"The points whose convex hull is the collider, 3 to 64 of them, in the collider's own space before Offset and Angle.", { .Unit = "m" })
			.Field("Offset", &PolygonCollider2DComponent::Offset, "Where the polygon's origin sits relative to the entity, in its XY plane.",
				{ .Unit = "m" })
			.Field("Angle", &PolygonCollider2DComponent::Angle, "The polygon's rotation about the entity's Z axis, counter-clockwise, in degrees.",
				{ .Unit = "deg" })
			.Field("IsTrigger", &PolygonCollider2DComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.")
			.Validate(&Utils::ValidatePolygonCollider2D)
			.Generate(&Utils::MakePolygonCollider2DValid);

		RegisterComponent<TilemapColliderComponent>(registry, "TilemapCollider",
			"The solid cells of a tilemap as 2D colliders: each chunk's solid cells merged into boxes, one static compound without a RigidBody2D.")
			.Category("Physics2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Tilemap", &TilemapColliderComponent::Tilemap, "The tilemap whose solid cells collide; null takes the entity's TilemapRenderer.Tilemap.")
			.Field("IsTrigger", &TilemapColliderComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");
	}

}
