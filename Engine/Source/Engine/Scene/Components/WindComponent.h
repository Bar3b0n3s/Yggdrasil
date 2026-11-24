#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "Wind" (Architecture §5.3, §8.3 "Wind"; M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md
	// decision 3F): the scene's wind, unique per scene, in a category of its own. It bends, sways and flutters the meshes
	// whose material has an enabled wind block (MaterialWind) in every pass alike, and scripts read it with Wind:Sample
	// (Scene/Wind.h). Direction is where the wind blows, in world axes; only its horizontal part (X and Z) counts,
	// normalized when used, and a zero horizontal part is refused. Strength is the steady speed in m/s (a gale is about 20),
	// GustStrength the speed gusts add at their peak, GustFrequency how many gusts pass a point per second (they travel with
	// the wind, so their wavelength is Strength / GustFrequency), and Turbulence how much the gusts vary across positions and
	// turn the wind sideways.
	struct WindComponent
	{
		glm::vec3 Direction = glm::vec3(1.0f, 0.0f, 0.0f);
		float Strength = 5.0f;
		float GustStrength = 3.0f;
		float GustFrequency = 0.2f;
		float Turbulence = 0.25f;
	};

}
