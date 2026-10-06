#pragma once

#include "Engine/Core/Base.h"

#include <string>

namespace Engine {

	// Registry name "CharacterController" (Architecture §5.3, §9.6): a Jolt character with an inner body. Excludes
	// RigidBody. Height and Radius in metres, MaxSlopeAngle in degrees, StepHeight in metres, Mass in kilograms; Layer names
	// a project physics layer.
	struct CharacterControllerComponent
	{
		float Height = 1.8f;
		float Radius = 0.3f;
		float MaxSlopeAngle = 45.0f;
		float StepHeight = 0.3f;
		float Mass = 70.0f;
		float GravityFactor = 1.0f;
		std::string Layer = "Default";
	};

}
