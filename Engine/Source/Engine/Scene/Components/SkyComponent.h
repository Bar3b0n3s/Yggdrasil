#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "Sky", version 1, category Rendering, after Environment (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md
	// decision 3): the scene's procedural, physically based sky, unique per scene; script-visible through its generated proxy.
	//   - The atmosphere (Hillaire's LUT method, Renderer/SkyAtmosphere.h) scatters the Sun light's authored Color times its
	//     Intensity, so the sky follows the Sun's direction: blue by day, red at dusk, dark at night. Haze scales its aerosols
	//     and Overcast turns it toward a grey overcast that dims the discs and, with DriveLights, the sun. GroundAlbedo is the
	//     atmosphere's ground.
	//   - Sun names the scene's sun, a DirectionalLight (null: the first DirectionalLight in canonical order); Moon names an
	//     entity turned as a DirectionalLight is, the moon toward its local +Z, normally a dim DirectionalLight (the moonlight;
	//     null: no moon). The moon's phase follows its elongation from the Sun.
	//   - The sky draws the sun's disc (SunDisc times its physical radiance, the Sun's LightAngle wide), the moon (MoonSize
	//     wide), stars (Stars times their brightness, faded by the sky's luminance) turning about the celestial pole that
	//     Latitude and NorthAngle place, by the Sun's hour angle, and a cloud layer of Clouds coverage drifting at CloudSpeed
	//     (metres per second of simulation time, world XZ).
	//   - It lights the scene in the Environment's place (its image-based light and the fog's colour follow it; an Environment
	//     beside it is unused, SKY_ENVIRONMENT_IGNORED), its Sun owns the fog's glow and the shadow cascades by day and its
	//     Moon the cascades by night, and with DriveLights the Sun's and Moon light's rendered colours take the atmosphere's
	//     transmittance (the moon's also its illuminated fraction); the components keep their authored colours, which scripts
	//     read.
	// Presentation only: the component is component data (serialized and in the state hash of a scene that holds it), and
	// nothing the renderer does with it is hashed.
	struct SkyComponent
	{
		UUID Sun{};                                   // EntityRef: a DirectionalLight; null: the first in canonical order
		UUID Moon{};                                  // EntityRef: the moon toward its local +Z; null: no moon
		float Intensity = 1.0f;                       // [0, 65504]
		bool DriveLights = true;                      // the Sun's and Moon light's rendered colours follow the atmosphere
		float Haze = 1.0f;                            // [0, 10], the aerosols' density scale
		float Overcast = 0.0f;                        // [0, 1]
		glm::vec3 GroundAlbedo = glm::vec3(0.3f);     // linear, [0, 1]
		float SunDisc = 1.0f;                         // [0, 1000], 1 physical
		float MoonSize = 0.52f;                       // degrees, [0.1, 10]
		float Stars = 1.0f;                           // [0, 100]
		float Latitude = 45.0f;                       // degrees, [-90, 90]
		float NorthAngle = 0.0f;                      // degrees about +Y, [-360, 360]; 0: north is -Z
		float Clouds = 0.0f;                          // coverage, [0, 1]
		glm::vec2 CloudSpeed = glm::vec2(5.0f, 0.0f); // metres per second in world XZ, each [-100, 100]
	};

}
