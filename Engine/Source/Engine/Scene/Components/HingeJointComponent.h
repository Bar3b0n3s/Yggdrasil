#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "JointMotorMode" (M37, Docs/Decisions/0054-m37-joints.md decision 1): what a HingeJoint's or SliderJoint's
	// motor does. Shared by both components.
	enum class JointMotorMode : uint8_t
	{
		Off,      // no drive; MaxFrictionTorque or MaxFrictionForce resists the motion
		Velocity, // drives to MotorTargetVelocity
		Position  // drives to MotorTargetAngle or MotorTargetPosition through a spring
	};

	// Registry name "HingeJoint" (M37, Architecture §5.3, §9.10; Docs/Decisions/0054-m37-joints.md decision 1): joins the body
	// of the RigidBody at or above the entity to ConnectedBody's body (null: the world) so it only turns about Axis through
	// Anchor. Requires Transform; excludes SliderJoint, DistanceJoint and FixedJoint. Anchor (m) and Axis in the entity's
	// space; angles in degrees (the authored pose is angle 0); torques in N m. Broken is set by the system when the joint
	// breaks, and writing false mends it.
	struct HingeJointComponent
	{
		UUID ConnectedBody{}; // EntityRef
		glm::vec3 Anchor = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		bool UseLimits = false;
		float LowerAngle = -90.0f; // [-180, 0]
		float UpperAngle = 90.0f;  // [0, 180]
		float MaxFrictionTorque = 0.0f;
		JointMotorMode Motor = JointMotorMode::Off;
		float MotorTargetVelocity = 0.0f; // deg/s
		float MotorTargetAngle = 0.0f;    // [-180, 180]
		float MotorMaxTorque = 1000.0f;
		float MotorFrequency = 2.0f;
		float MotorDamping = 1.0f;
		bool CollideConnected = false;
		float BreakForce = 0.0f; // N; 0 never breaks
		float BreakTorque = 0.0f;
		bool Broken = false;
	};

}
