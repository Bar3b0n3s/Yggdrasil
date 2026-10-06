#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "SphereCollider" (Architecture §5.3, §9.2): a sphere shape in the entity's space. Requires Transform.
	// Radius is at least MinColliderDimension (BoxColliderComponent.h).
	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = glm::vec3(0.0f);
		bool IsTrigger = false;
	};

}
