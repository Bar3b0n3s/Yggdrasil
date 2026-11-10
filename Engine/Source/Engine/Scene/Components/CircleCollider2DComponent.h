#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "CircleCollider2D" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 9): a circle in
	// the entity's local XY plane centred on Offset; a sphere of Radius in the 2D world's Jolt world (BoxCollider2DComponent.h
	// states the 2D colliders' common rules). Requires Transform. Radius is at least MinColliderDimension.
	struct CircleCollider2DComponent
	{
		float Radius = 0.5f;
		glm::vec2 Offset = glm::vec2(0.0f);
		bool IsTrigger = false;
	};

}
