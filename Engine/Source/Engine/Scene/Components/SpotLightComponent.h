#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "SpotLight" (Architecture §5.3, §8.7): a cone light down local -Z. Cone angles in degrees, inner below
	// outer (a type-level validator, with the Generate hook that orders random angles); Range and SourceRadius in metres.
	struct SpotLightComponent
	{
		glm::vec3 Color = glm::vec3(1.0f); // linear
		float Intensity = 10.0f;
		float Range = 15.0f;
		float InnerConeAngle = 20.0f;
		float OuterConeAngle = 30.0f;
		bool CastShadows = false;
		float SourceRadius = 0.05f;
	};

}
