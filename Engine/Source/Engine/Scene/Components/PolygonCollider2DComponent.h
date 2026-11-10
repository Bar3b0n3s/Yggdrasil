#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

	// The most points a PolygonCollider2D holds.
	inline constexpr uint32_t MaxPolygonCollider2DPoints = 64;

	// Registry name "PolygonCollider2D" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 9): the convex
	// hull of Points (3 to MaxPolygonCollider2DPoints points in the collider's own space), placed at Offset and rotated by
	// Angle degrees about local +Z, extruded along Z in the 2D world's Jolt world (BoxCollider2DComponent.h states the 2D
	// colliders' common rules). Requires Transform. Points that span no area are PHYSICS_INVALID_SHAPE. The default is the
	// triangle of the unit square's base and its top centre. The hull is Jolt's convex hull shape, whose corners Jolt rounds
	// by its default convex radius (up to 0.05 m, less on a hull too small for it): contacts and overlaps meet the rounded
	// corners, so a box slides off a square polygon's corner earlier than off an equal BoxCollider2D's sharp one, while rays
	// meet the sharp hull (an accepted limit, Backlog §6).
	struct PolygonCollider2DComponent
	{
		std::vector<glm::vec2> Points = { glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, -0.5f), glm::vec2(0.0f, 0.5f) };
		glm::vec2 Offset = glm::vec2(0.0f);
		float Angle = 0.0f; // degrees
		bool IsTrigger = false;
	};

}
