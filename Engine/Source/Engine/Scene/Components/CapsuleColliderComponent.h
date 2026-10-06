#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

	// Registry name "CapsuleCollider" (Architecture §5.3, §9.2): a capsule along local Y in the entity's space. Requires
	// Transform. Radius and HalfHeight (the cylinder half) are at least MinColliderDimension (BoxColliderComponent.h).
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity
		bool IsTrigger = false;
	};

}
