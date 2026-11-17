#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"

#include <array>
#include <string_view>
#include <vector>

// The collider surface check of one scene (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 6; Architecture §13.7):
// what project.validate reports about ColliderSurface components that apply to nothing. Its code has an array of its own,
// appended after the earlier landers' codes in both of ProjectValidator.cpp's code tables (ADR 0052's phase-6 ledger, the
// validation row), never in M11's PhysicsDiagnosticCodes, whose header is frozen and whose order the validator's tests
// pin right after PREFAB_MISSING_ASSET.

namespace Engine {

	class Scene;

	// Warning, not fixable: a ColliderSurface whose entity holds no solid collider of a creatable 3D body (no collider
	// component, triggers only, a character's capsule, 2D colliders only, or colliders of a body a composition error refuses:
	// PHYSICS_MIXED_TRIGGER, PHYSICS_DYNAMIC_TRIGGER, PHYSICS_NONCONVEX_DYNAMIC, PHYSICS_ALL_DOFS_LOCKED,
	// PHYSICS_LIMIT_EXCEEDED): it changes no contact. A body whose built shape is refused (PHYSICS_INVALID_SHAPE) still
	// counts, as the composition alone decides.
	inline constexpr std::string_view PhysicsSurfaceUnusedCode = "PHYSICS_SURFACE_UNUSED";

	// M36's codes, in the validator's order.
	inline constexpr std::array<std::string_view, 1> PhysicsSurfaceDiagnosticCodes = { PhysicsSurfaceUnusedCode };

	// PHYSICS_SURFACE_UNUSED for every effectively enabled entity of `scene` (an edit scene or a scratch copy of a scene file:
	// an entity and its ancestors without DisabledTag) whose ColliderSurface applies to no collider: none of the creatable
	// bodies ComposePhysicsBodies makes of `scene` with `layers` holds a solid collider of the entity. Component
	// "ColliderSurface", sorted by entity UUID. Pure function of its inputs; main thread.
	[[nodiscard]] std::vector<PhysicsDiagnostic> ValidateSceneSurfaces(const Scene& scene, const PhysicsLayerTable& layers);

}
