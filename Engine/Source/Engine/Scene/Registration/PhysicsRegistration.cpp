#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The Physics category (Docs/Decisions/0006-m3-decisions.md, decision 9; Architecture §5.3, §9). Field bounds keep every
// value Jolt receives valid (§9.1 "nothing invalid reaches Jolt"): positive masses, non-negative friction, damping and
// velocity limits, restitution in [0, 1] and collider dimensions of at least MinColliderDimension. The composition rules
// that span several components or entities (a non-convex MeshCollider on a Dynamic body, Dynamic triggers, all six
// degrees of freedom locked, mixed triggers, unknown layers) are diagnostics of the physics validator (M11,
// PHYSICS_* codes of §13.7), not rules of a single component.

namespace Engine {

	namespace Utils {

		// The smallest mass of a body or character, in kilograms (§5.4: "Mass in kilograms (Dynamic only)", > 0).
		constexpr float MinPhysicsMass = 0.001f;
		// The steepest walkable slope a character can be given, in degrees.
		constexpr double MaxCharacterSlopeAngle = 90.0;

	}

	void RegisterPhysicsComponents(TypeRegistry& registry)
	{
		registry.Enum<BodyType>("BodyType", "How a rigid body moves.")
			.Entry(BodyType::Static, "Static", "Never moves; the cheapest body, used for level geometry.")
			.Entry(BodyType::Kinematic, "Kinematic", "Moved by script or animation; pushes dynamic bodies but is not pushed back.")
			.Entry(BodyType::Dynamic, "Dynamic", "Simulated: moved by gravity, forces and collisions.");

		registry.Enum<MotionQuality>("MotionQuality", "How a moving body is checked for collisions between steps.")
			.Entry(MotionQuality::Discrete, "Discrete", "Collisions are checked at the end of each step (fast bodies can tunnel through thin ones).")
			.Entry(MotionQuality::LinearCast, "LinearCast", "Continuous collision detection along the step's motion; prevents tunnelling.");

		RegisterComponent<RigidBodyComponent>(registry, "RigidBody", "Simulates the entity with Jolt physics.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<CharacterControllerComponent>()
			.Field("Type", &RigidBodyComponent::Type, "Static never moves; Kinematic is moved by script; Dynamic is simulated.")
			.Field("Mass", &RigidBodyComponent::Mass, "Mass in kilograms (Dynamic only).", { .Min = Utils::MinPhysicsMass, .Unit = "kg" })
			.Field("Friction", &RigidBodyComponent::Friction, "The friction coefficient of the body's surfaces.", { .Min = 0.0 })
			.Field("Restitution", &RigidBodyComponent::Restitution, "Bounciness, from no bounce (0) to a perfectly elastic bounce (1).",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("LinearDamping", &RigidBodyComponent::LinearDamping, "How quickly linear velocity decays, per second.", { .Min = 0.0 })
			.Field("AngularDamping", &RigidBodyComponent::AngularDamping, "How quickly angular velocity decays, per second.", { .Min = 0.0 })
			.Field("GravityFactor", &RigidBodyComponent::GravityFactor, "The multiplier of the project's gravity for this body (0 floats).")
			.Field("MotionQuality", &RigidBodyComponent::MotionQuality, "Discrete or continuous (LinearCast) collision detection.")
			.Field("AllowSleeping", &RigidBodyComponent::AllowSleeping, "Whether the body may sleep when it comes to rest.")
			.Field("LockTranslation", &RigidBodyComponent::LockTranslation, "Per world axis: whether the body cannot move along it.")
			.Field("LockRotation", &RigidBodyComponent::LockRotation, "Per world axis: whether the body cannot rotate about it.")
			.Field("Layer", &RigidBodyComponent::Layer, "Physics layer name from project settings.")
			.Field("MaxLinearVelocity", &RigidBodyComponent::MaxLinearVelocity, "The speed the body is clamped to, in metres per second.",
				{ .Min = 0.0, .Unit = "m/s" })
			.Field("MaxAngularVelocity", &RigidBodyComponent::MaxAngularVelocity,
				"The angular speed the body is clamped to, in radians per second; raise it for fast rolling balls.",
				{ .Min = 0.0, .Unit = "rad/s" })
			.Field("EnhancedInternalEdgeRemoval", &RigidBodyComponent::EnhancedInternalEdgeRemoval,
				"Removes ghost collisions with internal edges of compound shapes; set it on rolling bodies.")
			.Field("InitialLinearVelocity", &RigidBodyComponent::InitialLinearVelocity,
				"The world-space velocity the body starts with, in metres per second.", { .Unit = "m/s" })
			.Field("InitialAngularVelocity", &RigidBodyComponent::InitialAngularVelocity,
				"The world-space angular velocity the body starts with, in radians per second.", { .Unit = "rad/s" });

		RegisterComponent<BoxColliderComponent>(registry, "BoxCollider", "A box-shaped collider in the entity's space.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("HalfExtents", &BoxColliderComponent::HalfExtents, "Half the box's size along each local axis, in metres; at least 1 mm.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("Offset", &BoxColliderComponent::Offset, "The box centre relative to the entity, in metres.", { .Unit = "m" })
			.Field("Rotation", &BoxColliderComponent::Rotation, "The box rotation relative to the entity: a unit quaternion.")
			.Field("IsTrigger", &BoxColliderComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<SphereColliderComponent>(registry, "SphereCollider", "A sphere-shaped collider in the entity's space.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Radius", &SphereColliderComponent::Radius, "The sphere radius, in metres; at least 1 mm.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("Offset", &SphereColliderComponent::Offset, "The sphere centre relative to the entity, in metres.", { .Unit = "m" })
			.Field("IsTrigger", &SphereColliderComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<CapsuleColliderComponent>(registry, "CapsuleCollider", "A capsule-shaped collider along the local Y axis.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Radius", &CapsuleColliderComponent::Radius, "The radius of the capsule's hemispheres and cylinder, in metres; at least 1 mm.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("HalfHeight", &CapsuleColliderComponent::HalfHeight, "Half the height of the cylinder between the hemispheres, in metres; at least 1 mm.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("Offset", &CapsuleColliderComponent::Offset, "The capsule centre relative to the entity, in metres.", { .Unit = "m" })
			.Field("Rotation", &CapsuleColliderComponent::Rotation, "The capsule rotation relative to the entity: a unit quaternion.")
			.Field("IsTrigger", &CapsuleColliderComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<MeshColliderComponent>(registry, "MeshCollider",
			"A collider shaped like a mesh: its convex hull, or the exact triangles for Static and Kinematic bodies.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Mesh", &MeshColliderComponent::Mesh, "The collision mesh; null uses the entity's MeshRenderer mesh.")
			.Field("Convex", &MeshColliderComponent::Convex,
				"Whether the convex hull is used; a non-convex mesh collider needs a Static or Kinematic body.")
			.Field("IsTrigger", &MeshColliderComponent::IsTrigger, "Whether the collider only detects overlaps instead of colliding.");

		RegisterComponent<CharacterControllerComponent>(registry, "CharacterController",
			"Moves the entity as a walking character that climbs steps and slopes and collides with the world.")
			.Category("Physics")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<RigidBodyComponent>()
			.Field("Height", &CharacterControllerComponent::Height, "The character's total height, in metres.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("Radius", &CharacterControllerComponent::Radius, "The radius of the character's capsule, in metres.",
				{ .Min = MinColliderDimension, .Unit = "m" })
			.Field("MaxSlopeAngle", &CharacterControllerComponent::MaxSlopeAngle, "The steepest slope the character can walk up, in degrees.",
				{ .Min = 0.0, .Max = Utils::MaxCharacterSlopeAngle, .Unit = "deg" })
			.Field("StepHeight", &CharacterControllerComponent::StepHeight, "The highest step the character climbs without jumping, in metres.",
				{ .Min = 0.0, .Unit = "m" })
			.Field("Mass", &CharacterControllerComponent::Mass, "The character's mass in kilograms, used when it pushes bodies.",
				{ .Min = Utils::MinPhysicsMass, .Unit = "kg" })
			.Field("GravityFactor", &CharacterControllerComponent::GravityFactor, "The multiplier of the project's gravity for the character.")
			.Field("Layer", &CharacterControllerComponent::Layer, "Physics layer name from project settings.");
	}

}
