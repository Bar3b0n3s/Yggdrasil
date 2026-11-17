#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "DistanceJoint" (M37, Architecture §5.3, §9.10; Docs/Decisions/0054-m37-joints.md decision 1): keeps the
	// distance between Anchor (in the entity's space, on the body of the RigidBody at or above it) and ConnectedAnchor (in
	// ConnectedBody's space, world space when ConnectedBody is null) between MinDistance and MaxDistance, a rope or a rod.
	// Requires Transform; excludes HingeJoint, SliderJoint and FixedJoint. Frequency (Hz, 0: rigid) and Damping (ratio) are
	// the spring that softens the limits.
	struct DistanceJointComponent
	{
		UUID ConnectedBody{}; // EntityRef
		glm::vec3 Anchor = glm::vec3(0.0f);
		glm::vec3 ConnectedAnchor = glm::vec3(0.0f);
		float MinDistance = 0.0f; // m, <= MaxDistance
		float MaxDistance = 1.0f;
		float Frequency = 0.0f;
		float Damping = 0.0f;
		bool CollideConnected = false;
		float BreakForce = 0.0f; // N; 0 never breaks
		bool Broken = false;
	};

}
