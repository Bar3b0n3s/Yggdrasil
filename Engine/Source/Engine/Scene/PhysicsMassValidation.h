#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <array>
#include <string_view>
#include <vector>

// The mass override of one scene's bodies (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §9.2
// "Body settings", §13.7): the MassProperties component read as the physics world's PhysicsMassOverride, shared by the play
// session (Scene/PhysicsSystem's body settings), the edit-time body checks (PhysicsValidation.cpp, ColliderDebugDraw.cpp), and
// what project.validate reports about a MassProperties that changes no body. Its code has an array of its own, appended after
// the earlier landers' codes in both of ProjectValidator.cpp's code tables (ADR 0060's phase-8 ledger, the validation rows).

namespace Engine {

	class ConstEntity;
	class Scene;

	// Warning, not fixable: a MassProperties whose entity's RigidBody makes no Dynamic body: the RigidBody is Static or
	// Kinematic, its colliders are triggers, or the composition refuses the body or makes none (no collider, a composition
	// error). Nothing uses its values.
	inline constexpr std::string_view PhysicsMassUnusedCode = "PHYSICS_MASS_UNUSED";

	// M44's mass code, in the validator's order.
	inline constexpr std::array<std::string_view, 1> PhysicsMassDiagnosticCodes = { PhysicsMassUnusedCode };

	// The mass override `owner`'s MassProperties gives its body (its CenterOfMassOffset and InertiaScale as they are); the
	// identity override when `owner` has none. Main thread.
	[[nodiscard]] PhysicsMassOverride GetPhysicsMassOverride(const ConstEntity& owner);

	// PHYSICS_MASS_UNUSED for every effectively enabled entity of `scene` (an edit scene or a scratch copy of a scene file: an
	// entity and its ancestors without DisabledTag) whose MassProperties changes no body: no creatable body that
	// ComposePhysicsBodies makes of `scene` with `layers` is the entity's own RigidBody's Dynamic, non-sensor body. Component
	// "MassProperties", sorted by entity UUID. Pure function of its inputs; main thread.
	[[nodiscard]] std::vector<PhysicsDiagnostic> ValidateSceneMassProperties(const Scene& scene, const PhysicsLayerTable& layers);

}
