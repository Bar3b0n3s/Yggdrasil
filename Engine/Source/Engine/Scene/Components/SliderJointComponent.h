#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/HingeJointComponent.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "SliderJoint" (M37, Architecture §5.3, §9.10; Docs/Decisions/0054-m37-joints.md decision 1): joins the
	// body of the RigidBody at or above the entity to ConnectedBody's body (null: the world) so it only moves along Axis
	// through Anchor, without turning. Requires Transform; excludes HingeJoint, DistanceJoint and FixedJoint. Anchor (m) and
	// Axis in the entity's space; positions in metres (the authored pose is position 0); forces in N. JointMotorMode is
	// HingeJointComponent.h's.
	struct SliderJointComponent
	{
		UUID ConnectedBody{}; // EntityRef
		glm::vec3 Anchor = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		bool UseLimits = false;
		float LowerLimit = -1.0f; // <= 0
		float UpperLimit = 1.0f;  // >= 0
		float MaxFrictionForce = 0.0f;
		JointMotorMode Motor = JointMotorMode::Off;
		float MotorTargetVelocity = 0.0f; // m/s
		float MotorTargetPosition = 0.0f;
		float MotorMaxForce = 1000.0f;
		float MotorFrequency = 2.0f;
		float MotorDamping = 1.0f;
		bool CollideConnected = false;
		float BreakForce = 0.0f; // N; 0 never breaks
		float BreakTorque = 0.0f;
		bool Broken = false;
	};

}
