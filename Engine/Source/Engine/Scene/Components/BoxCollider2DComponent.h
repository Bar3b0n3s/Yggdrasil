#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "BoxCollider2D" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 9): a rectangle in
	// the entity's local XY plane centred on Offset, rotated by Angle degrees about local +Z (counter-clockwise seen from +Z).
	// Requires Transform. Each HalfExtents component is at least MinColliderDimension (BoxColliderComponent.h). A trigger
	// detects overlaps without colliding.
	//
	// The 2D colliders (BoxCollider2D, CircleCollider2D, PolygonCollider2D) are laid into the 2D world's Jolt world as the
	// existing shape variant (Physics/Physics2DWorld.h: a circle is a sphere; a box and a polygon are extruded along Z on
	// each side by the diagonal of their bounding rectangle, GetPhysics2DHalfDepth, so contact normals stay in the plane), so
	// the frozen M11 PhysicsShape.h does not change. Their composition rules are the 3D
	// ones (Architecture §5.3) with RigidBody2D in RigidBody's place: solid 2D colliders gather into the nearest ancestor
	// RigidBody2D's compound or form an implicit static 2D body, and triggers get implicit sensor bodies. Offsets are in the
	// entity's local units; the entity's world scale applies in X and Y (a circle takes the larger of the two,
	// PHYSICS_NONUNIFORM_SCALE).
	struct BoxCollider2DComponent
	{
		glm::vec2 HalfExtents = glm::vec2(0.5f);
		glm::vec2 Offset = glm::vec2(0.0f);
		float Angle = 0.0f; // degrees
		bool IsTrigger = false;
	};

}
