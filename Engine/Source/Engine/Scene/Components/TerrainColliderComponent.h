#pragma once

#include "Engine/Core/Base.h"

#include <string>

namespace Engine {

	// Registry name "TerrainCollider", version 1, category Terrain (M26, C26.1, C-TERR; decisions 4 and 21): the entity's
	// Terrain as a static height field in the 3D physics world (PhysicsShape.h's HeightFieldShapeGeometry, from the
	// terrain's heightfield, Size and HeightScale, so it collides with exactly the surface that is drawn). Requires Terrain.
	// It is a solid collider under Architecture §5.3's composition rules (Scene/PhysicsComposition.h): with a RigidBody on its
	// entity or above it, it joins the nearest one's compound, which must then be Static or Kinematic (a height field is not
	// convex: PHYSICS_NONCONVEX_DYNAMIC otherwise, as a triangle mesh's); without one it is gathered as any solid collider is,
	// into the implicit static body of its topmost collider-only ancestor, or of its own entity, which then also gathers its
	// collider-only descendants. Layer, Friction and Restitution apply only when that body is its own entity's implicit static
	// body, which takes them instead of §5.3's layer 0 and default material; under a RigidBody or an ancestor's implicit body
	// they are unused (the body's apply, as for every collider). That is the one departure from §5.3 (collider components
	// have no layer, friction and restitution live on the body) and ADR 0014 decision 10 (implicit bodies take the nearest
	// ancestor RigidBody's layer, else layer 0), recorded in Docs/Decisions/0037-m26-contract.md decision 21: a terrain is
	// nearly always a root-level static body of its own whose layer (a ground layer) and material matter, and without these
	// fields every terrain would need a Static RigidBody only to carry them. Its body is a body of the physics block of the
	// state hash like any other (its body origin appended last, so existing scenes' entries are unchanged, ADR 0028's
	// phase-3 ledger); a scene without a TerrainCollider makes no such body and hashes as before.
	struct TerrainColliderComponent
	{
		std::string Layer = "Default"; // a project physics layer (PhysicsSettings.Layers)
		float Friction = 0.5f;         // >= 0, as RigidBody's
		float Restitution = 0.0f;      // [0, 1]
	};

}
