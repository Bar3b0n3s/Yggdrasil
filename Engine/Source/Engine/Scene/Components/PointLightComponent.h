#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "PointLight" (Architecture §5.3, §8.7): an omnidirectional light. Range in metres; SourceRadius in metres
	// (the highlight size and the shadow softness). Version 2 (M23, R-PLS) adds CastShadows after Range, as SpotLight orders
	// its fields: at most MaxPointShadowLights point lights of a view cast shadows (Renderer/PointShadowMaps.h).
	struct PointLightComponent
	{
		glm::vec3 Color = glm::vec3(1.0f); // linear
		float Intensity = 10.0f;
		float Range = 10.0f;
		bool CastShadows = false;
		float SourceRadius = 0.05f;
	};

}
