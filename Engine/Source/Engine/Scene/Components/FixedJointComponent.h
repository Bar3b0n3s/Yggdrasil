#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "FixedJoint" (M37, Architecture §5.3, §9.10; Docs/Decisions/0054-m37-joints.md decision 1): welds the body
	// of the RigidBody at or above the entity to ConnectedBody's body (null: the world) in the pose they have when it is first
	// created, about Anchor (m, in the entity's space). Requires Transform; excludes HingeJoint, SliderJoint and DistanceJoint.
	// With a BreakForce or BreakTorque it is a weld that breaks.
	struct FixedJointComponent
	{
		UUID ConnectedBody{}; // EntityRef
		glm::vec3 Anchor = glm::vec3(0.0f);
		bool CollideConnected = false;
		float BreakForce = 0.0f; // N; 0 never breaks
		float BreakTorque = 0.0f;
		bool Broken = false;
	};

}
