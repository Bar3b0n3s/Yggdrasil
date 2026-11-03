#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// The 2D composition rules (Docs/Decisions/0032-m21-contract.md decision 9): PhysicsComposition.h's rules in the plane, with
// RigidBody2D in RigidBody's place and BoxCollider2D, CircleCollider2D and PolygonCollider2D in the collider components'.
// One pure function, used by the play session's Physics2DSystem (which creates the bodies) and by ValidateScenePhysics2D
// (which reports them), so edit-time and run-time diagnostics agree, as for the 3D world. Private to Scene.
//
// The rules, over effectively enabled entities (DisabledTag on the entity or an ancestor excludes its subtree):
//   - One world per entity. An entity that holds a 2D physics component (RigidBody2D or a 2D collider) and a 3D one
//     (RigidBody, BoxCollider, SphereCollider, CapsuleCollider, MeshCollider, CharacterController) is PHYSICS2D_MIXED_WORLDS
//     (Error) and counts as holding no 2D component: it neither owns, carries nor gives a collider to a 2D body. The 3D
//     composition (PhysicsComposition.h) never looks at 2D components, so its bodies are those of the scene without them.
//   - The rest are PhysicsComposition.h's: a RigidBody2D owns a body (PhysicsBodyOrigin::RigidBody) of its Type, gathering
//     the solid 2D colliders of descendants without a RigidBody2D of their own (none between); its colliders agree on
//     IsTrigger (PHYSICS_MIXED_TRIGGER), a sensor is not Dynamic (PHYSICS_DYNAMIC_TRIGGER), a sensor of Type Static is
//     planned Kinematic; solid 2D colliders without a RigidBody2D above form an implicit static body owned by the topmost
//     such entity; trigger 2D colliders on an entity without its own RigidBody2D form its implicit sensor body; implicit
//     bodies take the nearest ancestor RigidBody2D's layer, else layer 0; the collision group is the nearest entity at or
//     above the owner with a RigidBody2D; a body's colliders are the owner's own (BoxCollider2D, CircleCollider2D,
//     PolygonCollider2D order), then each gathered descendant's in canonical order; bodies are listed in canonical order of
//     their owners, an implicit static body before an implicit sensor body; PHYSICS_UNKNOWN_LAYER,
//     PHYSICS_DYNAMIC_UNDER_MOVING_PARENT (an ancestor owns a Kinematic or Dynamic RigidBody2D), PHYSICS_NONUNIFORM_SCALE
//     (a CircleCollider2D whose entity's world scale differs in X and Y; Z does not count) and PHYSICS_LIMIT_EXCEEDED.
//   - The plane. PHYSICS2D_OFF_PLANE (Warning) on every creatable body's owner, and every entity whose colliders it
//     gathers, whose world rotation is not a rotation about Z alone: its swing (the rotation left once its twist about Z is
//     taken out) turns by more than IsOffPlaneRotation's tolerance. The body takes the twist, and the gathered colliders
//     their placement's twist relative to the owner (DescribePhysics2DBodyShape).
//
// Determinism: a pure function of the scene and its arguments; canonical order throughout; DetMath only. Main thread.

namespace Engine {

	class ConstEntity;
	class Scene;

	// The 2D collider component a collider comes from.
	enum class Physics2DColliderType : uint8_t
	{
		Box,
		Circle,
		Polygon
	};

	struct Physics2DColliderPlan
	{
		UUID Entity{};
		Physics2DColliderType Type = Physics2DColliderType::Box;
		bool IsTrigger = false;
	};

	// One 2D body the scene makes; its members mean PhysicsBodyPlan's (Origin is never Character).
	struct Physics2DBodyPlan
	{
		UUID Owner{};
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		PhysicsMotionType MotionType = PhysicsMotionType::Static;
		bool IsSensor = false;
		uint32_t Layer = 0;
		UUID CollisionGroup{};
		std::vector<Physics2DColliderPlan> Colliders{};
		bool IsCreatable = true;
	};

	struct Physics2DComposition
	{
		std::vector<Physics2DBodyPlan> Bodies{};
		// Sorted by (entity UUID, code, subject).
		std::vector<PhysicsDiagnostic> Diagnostics{};
	};

	// Whether `entity` holds a 2D physics component, and whether it holds a 3D one.
	[[nodiscard]] bool HasPhysics2DComponent(ConstEntity entity);
	[[nodiscard]] bool HasPhysics3DComponent(ConstEntity entity);

	// Whether a unit rotation is not a rotation about Z alone: the sine of half its swing angle, sqrt(x^2 + y^2), exceeds
	// PhysicsUnitRotationTolerance (a swing of about 0.11 degrees), so the rounding of a rotation read back from a matrix never
	// counts.
	[[nodiscard]] bool IsOffPlaneRotation(const glm::quat& rotation);

	// The 2D bodies of `scene` under the rules above (layer names resolved against `layers`, at most `maxBodies` creatable).
	// Works on edit and runtime scenes; walks the parent chain, so it needs no TransformSystem::Update first.
	[[nodiscard]] Physics2DComposition ComposePhysics2DBodies(const Scene& scene, const PhysicsLayerTable& layers,
		uint32_t maxBodies = PhysicsWorldLimits::DefaultMaxBodies);

	// The shape description of `plan` (a plan of ComposePhysics2DBodies for `scene`), relative to its owner: each collider's
	// geometry from its component (BoxCollider2D.HalfExtents, CircleCollider2D.Radius, PolygonCollider2D.Points), placed by
	// its Offset and Angle in its entity's local XY plane, then by that entity's transform relative to the owner, with every
	// world scale on the way baked in (as DescribePhysicsBodyShape); the placement's position in the plane, its twist about Z
	// and its scale along the collider's X and Y axes give the collider's Position, Angle and Scale. A circle under a
	// non-uniform scale uses the larger of X and Y (PHYSICS_NONUNIFORM_SCALE is the composition's). UserData is the collider's
	// index in plan.Colliders. Errors: Validation "PHYSICS_INVALID_SHAPE: ..." naming the collider entity for a placement
	// that cannot be decomposed; MakeBody2DShape and PhysicsShape::Create find the rest.
	[[nodiscard]] Result<Body2DShapeDescription> DescribePhysics2DBodyShape(const Scene& scene, const Physics2DBodyPlan& plan);

	// "BoxCollider2D", "CircleCollider2D" or "PolygonCollider2D".
	[[nodiscard]] std::string_view GetCollider2DComponentName(Physics2DColliderType type);

}
