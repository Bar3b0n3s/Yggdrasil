#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

	// The smallest collider dimension (Architecture §5.3): box half extents, sphere and capsule radii and capsule half
	// heights are at least 1 mm, so no degenerate shape reaches Jolt. Registered as FieldMeta::Min of those fields.
	inline constexpr float MinColliderDimension = 0.001f;

	// Registry name "BoxCollider" (Architecture §5.3, §9.2): a box shape in the entity's space, offset and rotated. Requires
	// Transform. Each HalfExtents component is at least MinColliderDimension. A trigger detects overlaps without
	// colliding.
	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = glm::vec3(0.5f);
		glm::vec3 Offset = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity
		bool IsTrigger = false;
	};

}
