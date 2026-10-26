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
		// M19 (Docs/Decisions/0030-m19-contract.md decision 36, P-CARRY): while grounded, the character moves with the
		// horizontal velocity of what it stands on (CharacterControllerDescription::InheritGroundVelocity), so a moving platform
		// carries it. Opt-in; a field added with its default, so the component stays at version 1 (no migration, the ledger's
		// "v2 only if it needs a migration").
		bool InheritGroundVelocity = false;
	};

}
