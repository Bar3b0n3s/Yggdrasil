#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <array>
#include <string>
#include <string_view>

// The physics diagnostics (Architecture §9.1, §9.2, §5.3 composition rules, §13.7 "PHYSICS_*" codes): what the physics
// composition, the play session's PhysicsSystem and the project validator report about content that cannot be simulated
// as authored. Invalid content never reaches Jolt and never asserts (§9.1 "nothing invalid reaches Jolt"); it gives one of
// these diagnostics instead, and the body concerned is created differently or not at all, as each code says.
//
// The same codes travel in Error messages: a PhysicsWorld or PhysicsShape error caused by content starts its message with
// "<CODE>: " (the convention of Scene/StructuralValidator.h), and GetPhysicsDiagnosticCode reads it back.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 9, which also lists severities and run-time
// behaviour). Header-only.

namespace Engine {

	// Error, not fixable: a MeshCollider with Convex false on a Dynamic RigidBody (§5.3: a non-convex mesh needs a Static or
	// Kinematic body; Jolt's MeshShape has no mass properties). The body is not created.
	inline constexpr std::string_view PhysicsNonconvexDynamicCode = "PHYSICS_NONCONVEX_DYNAMIC";
	// Error, not fixable: colliders on one entity that has its own RigidBody disagree on IsTrigger (§5.3). The body is not
	// created.
	inline constexpr std::string_view PhysicsMixedTriggerCode = "PHYSICS_MIXED_TRIGGER";
	// Error, not fixable: a Dynamic RigidBody whose colliders are triggers (§9.1; Jolt sensors are Kinematic). The body is
	// not created.
	inline constexpr std::string_view PhysicsDynamicTriggerCode = "PHYSICS_DYNAMIC_TRIGGER";
	// Error, not fixable: a Dynamic RigidBody with all six degrees of freedom locked (§9.1). The body is not created.
	inline constexpr std::string_view PhysicsAllDofsLockedCode = "PHYSICS_ALL_DOFS_LOCKED";
	// Error, not fixable: Jolt's ShapeSettings::Create refused a shape (a degenerate convex hull, an empty or degenerate
	// mesh), or a scale is not valid for a shape (Shape::IsValidScale); the message carries Jolt's text (§9.1). The body is
	// not created.
	inline constexpr std::string_view PhysicsInvalidShapeCode = "PHYSICS_INVALID_SHAPE";
	// Warning, auto-fixable: two implicit static bodies (solid colliders without a RigidBody on themselves or an ancestor)
	// whose world AABBs are within AdjacentStaticBodiesDistance of each other: a rolling body bumps at their seam (§5.3,
	// §9.2). The fix adds a Static RigidBody to the entities' nearest common ancestor, which joins both into one static
	// compound. It is not fixable when there is no such ancestor (two roots: the hint says to group them) or when the
	// ancestor has trigger colliders of its own, which the new RigidBody would mix with the solid colliders it gathers
	// (PHYSICS_MIXED_TRIGGER: the hint says to move the triggers to a child entity first).
	inline constexpr std::string_view PhysicsAdjacentStaticBodiesCode = "PHYSICS_ADJACENT_STATIC_BODIES";
	// Error, not fixable: a RigidBody or CharacterController names a layer that PhysicsSettings.Layers does not declare.
	// The body is created on layer 0 ("Default"), so the scene still plays.
	inline constexpr std::string_view PhysicsUnknownLayerCode = "PHYSICS_UNKNOWN_LAYER";
	// Warning, not fixable: a sphere or capsule collider under a non-uniform world scale; it uses the largest axis (§9.2).
	inline constexpr std::string_view PhysicsNonuniformScaleCode = "PHYSICS_NONUNIFORM_SCALE";
	// Warning, not fixable: a Dynamic RigidBody below an ancestor that moves on its own (a Kinematic or Dynamic RigidBody, or
	// a CharacterController). The body simulates in world space and does not follow its parent (§9.3): the parent's motion
	// never moves it, and PostStep rewrites the entity's local transform so the entity stays where the body is
	// (PhysicsSystem.h).
	inline constexpr std::string_view PhysicsDynamicUnderMovingParentCode = "PHYSICS_DYNAMIC_UNDER_MOVING_PARENT";
	// Error when a limit is reached (bodies beyond PhysicsWorldLimits::MaxBodies are not created; a step whose body pairs or
	// contact constraints overflow), Warning when a count first passes PhysicsWorldLimits::LimitWarningFraction of its limit
	// (§9.1). The validator reports a scene that would create more bodies than the default limit as an Error.
	inline constexpr std::string_view PhysicsLimitExceededCode = "PHYSICS_LIMIT_EXCEEDED";

	// The physics codes, in §13.7 order.
	inline constexpr std::array<std::string_view, 10> PhysicsDiagnosticCodes = {
		PhysicsNonconvexDynamicCode,
		PhysicsMixedTriggerCode,
		PhysicsDynamicTriggerCode,
		PhysicsAllDofsLockedCode,
		PhysicsInvalidShapeCode,
		PhysicsAdjacentStaticBodiesCode,
		PhysicsUnknownLayerCode,
		PhysicsNonuniformScaleCode,
		PhysicsDynamicUnderMovingParentCode,
		PhysicsLimitExceededCode,
	};

	// The distance, in metres, below which two implicit static bodies count as adjacent (§5.3: "within 1 mm").
	inline constexpr float AdjacentStaticBodiesDistance = 0.001f;

	// One physics diagnostic about one entity. Plain value; thread-compatible.
	struct PhysicsDiagnostic
	{
		std::string Code{}; // one of PhysicsDiagnosticCodes
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		UUID Entity{};           // the entity concerned (the body owner, or the collider entity for shape problems)
		std::string Component{}; // registry name ("RigidBody", "BoxCollider"); empty when no single component is at fault
		std::string Field{};     // registry field name ("Layer", "LockRotation"); empty when none
		std::string Message{};
		std::string Hint{};
		// Tells apart diagnostics of one code on one entity (PHYSICS_ADJACENT_STATIC_BODIES: the other entity's 16 hex
		// digits; PHYSICS_LIMIT_EXCEEDED: "bodies", "bodyPairs" or "contactConstraints"); empty when unique. The project
		// validator's diagnostic id uses it (ProjectValidator::MakeDiagnosticId).
		std::string Subject{};
		bool AutoFixable = false;
		// PHYSICS_ADJACENT_STATIC_BODIES: the nearest common ancestor that the fix gives a Static RigidBody; the invalid UUID
		// when the two entities have none. AutoFixable is false without one and when it has trigger colliders of its own.
		UUID FixTarget{};

		bool operator==(const PhysicsDiagnostic&) const = default;
	};

	// The physics code that `error`'s message starts with ("PHYSICS_INVALID_SHAPE: ..." gives PHYSICS_INVALID_SHAPE), or
	// an empty view when it starts with none.
	[[nodiscard]] inline std::string_view GetPhysicsDiagnosticCode(const Error& error)
	{
		const std::string_view message = error.GetMessageText();
		for (const std::string_view code : PhysicsDiagnosticCodes)
		{
			if (message.size() > code.size() + 1 && message.starts_with(code) && message[code.size()] == ':' && message[code.size() + 1] == ' ')
				return code;
		}
		return {};
	}

}
