#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"

#include <vector>

// The physics checks of one scene (Architecture §13.7 "PHYSICS_*" codes, §5.3, §9.1): what project.validate reports for a
// scene, and what the editor's diagnostics panel shows (M10). The play session reports the same codes at run time from
// its own composition (PhysicsSystem::GetDiagnostics); both build on ComposePhysicsBodies, so edit-time and run-time
// diagnostics agree.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 15).

namespace Engine {

	class AssetManager;
	class Scene;

	// Every physics diagnostic of `scene` (an edit scene or a scratch copy of a scene file), sorted by (entity UUID, code,
	// subject):
	//   - the composition's (ComposePhysicsBodies with `layers` and the default body limit: PHYSICS_NONCONVEX_DYNAMIC,
	//     PHYSICS_MIXED_TRIGGER, PHYSICS_DYNAMIC_TRIGGER, PHYSICS_ALL_DOFS_LOCKED, PHYSICS_UNKNOWN_LAYER,
	//     PHYSICS_NONUNIFORM_SCALE, PHYSICS_DYNAMIC_UNDER_MOVING_PARENT, PHYSICS_LIMIT_EXCEEDED);
	//   - PHYSICS_INVALID_SHAPE for every creatable body whose shape DescribePhysicsBodyShape or PhysicsShape::Create
	//     refuses (on the collider's entity when the error names one, else on the owner); characters' capsules included
	//     (Height <= 2 * Radius);
	//   - PHYSICS_ADJACENT_STATIC_BODIES for every pair of implicit static bodies whose world AABBs (their shapes' local
	//     bounds around their owners' world poses) are within AdjacentStaticBodiesDistance of each other, overlapping
	//     included: one diagnostic per pair, on the owner that comes first in canonical order, Subject the other owner's
	//     UUID, FixTarget the nearest common ancestor of the two owners (invalid when they have none: two roots), and
	//     AutoFixable only when that ancestor exists and has no trigger collider of its own (a Static RigidBody there would
	//     gather the solid pieces together with its triggers, PHYSICS_MIXED_TRIGGER; the hint then says to move the
	//     triggers to a child entity).
	// Meshes load through `assets` (as DescribePhysicsBodyShape; null: mesh colliders are PHYSICS_INVALID_SHAPE). Building
	// shapes needs the process's PhysicsEngine (ProcessContext); without it, the shape checks are skipped and logged once
	// at Warn. Pure function of its inputs otherwise; main thread.
	[[nodiscard]] std::vector<PhysicsDiagnostic> ValidateScenePhysics(const Scene& scene, const PhysicsLayerTable& layers, AssetManager* assets);

}
