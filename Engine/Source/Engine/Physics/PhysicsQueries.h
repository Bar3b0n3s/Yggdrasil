#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstdint>

// The query vocabulary of a PhysicsWorld (Architecture §9.5): rays, shape casts and overlaps through Jolt's
// NarrowPhaseQuery on the main thread, filtered by a layer mask. Results name the body and the collider that was hit (the
// sub-shape user data of §9.2); Scene/PhysicsSystem turns them into entities and sorts them by distance, then by UUID.
// Plain value types; thread-compatible.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 7).

namespace Engine {

	// Which bodies a query sees.
	struct PhysicsQueryFilter
	{
		// The project layers a body must be on (PhysicsLayerTable::MakeMask).
		PhysicsLayerMask Layers = AllPhysicsLayers;
		// Whether sensors (triggers, implicit sensor bodies) are hit. Physics.Raycast and the overlaps of scripts include
		// them (§9.5 does not exclude them), so a script can ask whether a point is inside a checkpoint.
		bool IncludeSensors = true;
		// A body to ignore (the caster's own body); the invalid handle ignores none.
		BodyHandle IgnoreBody{};
	};

	// A ray from Origin along Direction (unit length) up to MaxDistance (> 0). Every value finite.
	struct PhysicsRay
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, -1.0f, 0.0f);
		float MaxDistance = 1000.0f;
	};

	// One ray or shape-cast hit.
	struct PhysicsQueryHit
	{
		BodyHandle Body{};
		// The collider index of the sub-shape that was hit (§9.2, resolved as ContactEvent's colliders are).
		uint32_t Collider = 0;
		// World space. Point is where the ray or the cast shape first touches; Normal is the surface normal there, unit,
		// pointing against the query direction.
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		// Metres along the direction, in [0, MaxDistance].
		float Distance = 0.0f;
	};

	// One body sub-shape that overlaps a query shape.
	struct PhysicsOverlap
	{
		BodyHandle Body{};
		uint32_t Collider = 0;
	};

}
