#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "MassProperties", version 1, category Physics (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3;
	// Architecture §5.3, §9.2 "Body settings"): changes the mass properties the colliders give a Dynamic RigidBody on the same
	// entity (PhysicsMassOverride). Requires Transform and RigidBody. CenterOfMassOffset in metres in the entity's own axes
	// (its rotation, never its scale), each component within PhysicsShape::MaxColliderSize, is added to the colliders' centre
	// of mass: a buggy's or a boat's lowered centre of mass, which keeps it on its wheels or upright. The colliders and the
	// entity stay where they are. InertiaScale scales the body-space inertia per axis, each component in
	// [MinPhysicsInertiaScale, MaxPhysicsInertiaScale] (S^1/2 I S^1/2, so it stays symmetric and positive definite), after the
	// offset moved it (the parallel axis theorem, as Jolt's OffsetCenterOfMassShape gives it). A body that is not Dynamic
	// (Static, Kinematic, a trigger) or makes no body ignores it (PHYSICS_MASS_UNUSED). A write creates the body again with its
	// velocities, as a RigidBody write does.
	struct MassPropertiesComponent
	{
		glm::vec3 CenterOfMassOffset = glm::vec3(0.0f);
		glm::vec3 InertiaScale = glm::vec3(1.0f);
	};

}
