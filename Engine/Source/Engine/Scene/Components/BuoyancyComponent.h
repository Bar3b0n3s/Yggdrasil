#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// The bounds of the Buoyancy component's fields (its registry ranges, Scene/Registration/WaterRegistration.cpp).
	inline constexpr float MaxBuoyancyScale = 10.0f;
	inline constexpr float MaxBuoyancyDrag = 100.0f;

	// Registry name "Buoyancy", version 1, category Water (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12):
	// floats the Dynamic body its entity owns on water. Requires RigidBody; script-visible through its generated proxy.
	//   - The force: each solid collider of the body is submerged against the water plane under its world bounds' centre (the
	//     surface point there, its normal and its velocity) at the tick's world time, through Jolt's submerged volume (exact
	//     for a box, a sphere and a convex mesh; a capsule floats as its bounding box), and the body takes the physical
	//     buoyancy rho g V_submerged of the water's Density, times BuoyancyScale, with Jolt's drag (LinearDrag, AngularDrag)
	//     against the water's velocity (Physics/PhysicsBuoyancy.h). So one hull pitches with the local slope, and four pontoon
	//     colliders ride a swell shorter than the boat. LinearDrag's default 2 settles a dropped hull within 2 cm of its draft
	//     in 5 s.
	//   - The water: null takes, per collider, the water that holds its centre (Scene/WaterQueries.h's rule); an entity with a
	//     Water takes that water's own surface (a Rectangle's only within its footprint and volume).
	//   - Where it runs: PhysicsSystem's PreStep item 4c, in Play and Simulate, for an effectively enabled Buoyancy whose owner
	//     holds a created Dynamic body; any other is skipped and reported (PHYSICS_BUOYANCY_INVALID). A body on calm,
	//     unchanged water may sleep.
	// Component data: serialized and in the state hash of a scene that holds it; its effect is in the body's state.
	struct BuoyancyComponent
	{
		UUID Water{};               // EntityRef; null: the water that holds each collider
		float BuoyancyScale = 1.0f; // [0, MaxBuoyancyScale]; 1 is physical
		float LinearDrag = 2.0f;    // [0, MaxBuoyancyDrag], Jolt's linear drag coefficient
		float AngularDrag = 0.05f;  // [0, MaxBuoyancyDrag], Jolt's angular drag coefficient
	};

}
