#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "PointLight" (Architecture §5.3): an omnidirectional light; no shadows in v1. Range in metres;
	// SourceRadius in metres (specular highlight size).
	struct PointLightComponent
	{
		glm::vec3 Color = glm::vec3(1.0f); // linear
		float Intensity = 10.0f;
		float Range = 10.0f;
		float SourceRadius = 0.05f;
	};

}
