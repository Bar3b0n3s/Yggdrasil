#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsJoint.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <vector>

// The joint composition rules (M37, Docs/Decisions/0054-m37-joints.md decisions 1, 3 and 9; Architecture §5.3, §9.10):
// which bodies each HingeJoint, SliderJoint, DistanceJoint and FixedJoint joins, and which joints cannot be made as
// authored. One pure function over ComposePhysicsBodies' result, never changing it (PhysicsComposition.h stays as M11 froze
// it), used by the play session's PhysicsSystem (which creates the joints), the project validator (which reports them) and
// the joint debug draw (which draws them), so the rules exist once.
//
// The rules, over effectively enabled entities only (DisabledTag on the entity or an ancestor excludes it, as the body
// composition reads it):
//   - A joint's body is the body of the RigidBody at or above its entity: its own, else its nearest ancestor's, with no
//     CharacterController on the way (§5.3's gathering rule), so a joint sits on the body's entity or on a child of it, and
//     a body may hold several joints. ConnectedBody names the other end by the same rule or, without a RigidBody at or
//     above that entity, the implicit static body its own solid colliders form (the one it owns, or the one an ancestor
//     gathers them into); null joins the body to the world.
//   - One joint per entity: the four components exclude each other. An entity that holds several anyway (a hand-written
//     file) is read through the first in BuiltinComponents order.
//   - PHYSICS_JOINT_INVALID (Error, not fixable; one per joint, the first reason in this order, each its own Subject): the
//     joint's end first: no RigidBody at or above the joint's entity, or its RigidBody makes no body, having no collider
//     ("body"), or the end under a CharacterController, a character being no body ("character"), or under a RigidBody2D,
//     which belongs to the 2D world ("2d"); then the connected end: a ConnectedBody that names no entity or resolves to no
//     body ("connected"), or under a CharacterController ("character") or a RigidBody2D ("2d"); then both ends the same body
//     ("same"); a zero Axis ("axis"); UseLimits with equal limits ("limits"); a MinDistance above MaxDistance
//     ("distance"). Such a joint is listed not creatable, and its bodies simulate unjoined. A joint whose end a body
//     diagnostic refused (PHYSICS_MIXED_TRIGGER and the others: the plan is not creatable while it has colliders), or
//     whose connected entity is disabled, is listed not creatable without a second diagnostic. Neither end Dynamic is
//     valid: Jolt solves a joint only while one of its bodies is Dynamic and awake.
//   - Joints are listed in canonical order of their entities (§5.1).
//
// Determinism: a pure function of the scene and the body composition; canonical order throughout. Main thread (the scene).

namespace Engine {

	class ConstEntity;
	class Entity;
	class Scene;

	// Error, not fixable: a joint that cannot be made as authored (the rules above). The joint is not created and its
	// bodies simulate unjoined.
	inline constexpr std::string_view PhysicsJointInvalidCode = "PHYSICS_JOINT_INVALID";

	// M37's codes, in the validator's order: an array of their own, appended at the end of ProjectValidator.cpp's tables
	// (M11's PhysicsDiagnosticCodes is frozen and must stay contiguous; ADR 0052's ledger, the validation row).
	inline constexpr std::array<std::string_view, 1> PhysicsJointDiagnosticCodes = { PhysicsJointInvalidCode };

	// One joint the scene makes.
	struct PhysicsJointPlan
	{
		UUID Entity{}; // the entity holding the joint component
		PhysicsJointType Type = PhysicsJointType::Hinge;
		// The owner of the joint's body (a RigidBody plan's Owner); the invalid UUID when there is none.
		UUID Body{};
		// The owner of the connected body and its origin (RigidBody or ImplicitStatic); the invalid UUID for the world.
		UUID Connected{};
		PhysicsBodyOrigin ConnectedOrigin = PhysicsBodyOrigin::RigidBody;
		// False when PHYSICS_JOINT_INVALID refuses it, or when an end's body plan is not creatable or its connected entity
		// is disabled (no diagnostic then).
		bool IsCreatable = true;
	};

	struct PhysicsJointComposition
	{
		std::vector<PhysicsJointPlan> Joints{};
		// PHYSICS_JOINT_INVALID diagnostics, on the joint's entity and its component, sorted by (entity UUID, code, subject).
		std::vector<PhysicsDiagnostic> Diagnostics{};
	};

	// The joints of `scene` under the rules above, `bodies` being ComposePhysicsBodies' result for the same scene. Works on edit
	// and runtime scenes.
	[[nodiscard]] PhysicsJointComposition ComposePhysicsJoints(const Scene& scene, const PhysicsComposition& bodies);

	// A joint component read as one shape (ReadPhysicsJoint), in the world's units: what the session, the validator and the
	// debug draw read, so none of them reads the four components field by field.
	struct PhysicsJointFields
	{
		PhysicsJointType Type = PhysicsJointType::Hinge;
		UUID ConnectedBody{};
		glm::vec3 Anchor = glm::vec3(0.0f);
		// Hinge and slider: the component's Axis; fixed: the entity's X axis (1, 0, 0); distance: unused.
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		// Distance only.
		glm::vec3 ConnectedAnchor = glm::vec3(0.0f);
		// The component's limits, friction, motor, spring and CollideConnected in PhysicsJointSettings' units: degrees become
		// radians (JointDegreesToRadians), MaxFrictionTorque and MotorMaxTorque become MaxFriction and MotorMaxForce,
		// MotorTargetAngle becomes MotorTargetPosition, a distance's MinDistance, MaxDistance, Frequency and Damping become
		// LowerLimit, UpperLimit, LimitFrequency and LimitDamping.
		PhysicsJointSettings Settings{};
		float BreakForce = 0.0f;
		float BreakTorque = 0.0f; // 0 for a DistanceJoint
		bool Broken = false;
	};

	// "HingeJoint", "SliderJoint", "DistanceJoint" or "FixedJoint": the registry name of the component of `type`.
	[[nodiscard]] std::string_view GetPhysicsJointComponentName(PhysicsJointType type);

	// The joint component of `entity` (the first of HingeJoint, SliderJoint, DistanceJoint and FixedJoint it holds), or nullopt
	// when it holds none or is not readable (a runtime entity marked for destruction is read until the flush).
	[[nodiscard]] std::optional<PhysicsJointFields> ReadPhysicsJoint(ConstEntity entity);
	// Writes `broken` into the Broken field of `entity`'s joint component (present, asserted) directly, as a runtime system
	// writes its scene (§5.1): the play session's PhysicsSystem when a joint breaks.
	void WritePhysicsJointBroken(Entity entity, bool broken);

	// The joint's frame in world space from its entity's world matrix `entityWorld`: Anchor through the matrix (scale baked in,
	// as for colliders), the axis through its 3x3 part and normalized (a fixed joint's: the entity's X axis), and a normal
	// perpendicular to it built by cross products with the world axis least aligned with it (no CRT transcendental, §9.1).
	// A distance joint, which has no axis, gets the world X axis and Y normal whatever the matrix. nullopt when the axis
	// vanishes (a zero Axis, PHYSICS_JOINT_INVALID's "axis", or one a vanishing scale shrinks) or the frame is not finite.
	[[nodiscard]] std::optional<PhysicsJointFrame> MakePhysicsJointWorldFrame(const glm::mat4& entityWorld, const PhysicsJointFields& fields);
	// `frame` (world space) in the space of a body whose origin is at `pose`, and back. The axes are normalized, and the
	// normal made perpendicular to the axis again, after the rotation.
	[[nodiscard]] PhysicsJointFrame ToPhysicsJointBodyFrame(const PhysicsPose& pose, const PhysicsJointFrame& frame);
	[[nodiscard]] PhysicsJointFrame ToPhysicsJointWorldFrame(const PhysicsPose& pose, const PhysicsJointFrame& frame);

}
