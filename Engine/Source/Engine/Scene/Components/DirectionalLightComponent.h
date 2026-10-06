#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry name "DirectionalLight" (Architecture §5.3, §8.7): a sun shining down local -Z. CascadeCount is 1-4;
	// LightAngle is the angular diameter in degrees (PCSS penumbra); DepthBias and NormalBias are texel-scaled.
	struct DirectionalLightComponent
	{
		glm::vec3 Color = glm::vec3(1.0f); // linear
		float Intensity = 3.0f;
		bool CastShadows = true;
		float ShadowDistance = 100.0f;
		uint32_t CascadeCount = 4;
		float CascadeSplitLambda = 0.75f;
		float LightAngle = 1.0f;
		float DepthBias = 1.0f;
		float NormalBias = 1.0f;
	};

}
