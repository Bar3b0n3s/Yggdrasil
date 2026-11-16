#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "Fog" (Architecture §5.3, §8.6; M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 2 and 3): the scene's
	// exponential distance and height fog, unique per scene. A view's fog runs along each pixel's view ray over its view depth
	// (distance along the camera's forward axis), from StartDistance on, with Density per metre plus height fog of
	// HeightDensity per metre at BaseHeight, falling off by HeightFalloff per metre above it, up to MaxOpacity. Its colour is
	// Color (linear) tinted by the environment's ambient light by AmbientTint, plus a glow toward the first directional light
	// (SunGlow times that light's colour and intensity, gathered by SunGlowExponent). FogSkybox fogs the skybox too.
	struct FogComponent
	{
		glm::vec3 Color = glm::vec3(0.5f, 0.55f, 0.6f);
		float AmbientTint = 0.0f;
		float Density = 0.02f;
		float StartDistance = 0.0f;
		float MaxOpacity = 1.0f;
		float HeightDensity = 0.0f;
		float HeightFalloff = 0.5f;
		float BaseHeight = 0.0f;
		float SunGlow = 0.0f;
		float SunGlowExponent = 8.0f;
		bool FogSkybox = true;
	};

}
