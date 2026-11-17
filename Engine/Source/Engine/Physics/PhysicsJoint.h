#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>
#include <numbers>
#include <string_view>

// The vocabulary of the physics world's joints (M37, Docs/Decisions/0054-m37-joints.md decision 2; Architecture §9.10):
// Jolt's hinge, slider, distance and fixed constraints between two bodies of a PhysicsWorld, or a body and the world, in
// engine types. PhysicsWorld's joint section (PhysicsWorld.h) creates, changes, measures and destroys them; Scene/PhysicsSystem
// makes them from the HingeJoint, SliderJoint, DistanceJoint and FixedJoint components. Plain value types;
// thread-compatible.
//
// Units are Jolt's: radians, metres, newtons and newton metres. Frames are given in each body's own space, relative to the
// body's origin (its PhysicsPose, not its centre of mass), so they survive a body's shape change; the world's frame is
// world space.

namespace Engine {

	// One joint of a PhysicsWorld: a value the world hands out in creation order, never reused while the world lives, so a
	// handle of a destroyed joint never names another one. The default handle is invalid.
	class JointHandle
	{
	public:
		static constexpr uint32_t InvalidValue = 0;

		constexpr JointHandle() = default;
		explicit constexpr JointHandle(uint32_t value)
			: m_Value(value)
		{
		}

		[[nodiscard]] constexpr uint32_t GetValue() const { return m_Value; }
		[[nodiscard]] constexpr bool IsValid() const { return m_Value != InvalidValue; }

		constexpr auto operator<=>(const JointHandle&) const = default;
	private:
		uint32_t m_Value = InvalidValue;
	};

	// Which Jolt constraint a joint is. Registry enum "JointType" (physics.jointInfo's type, Scene/Registration/
	// JointsRegistration.cpp).
	enum class PhysicsJointType : uint8_t
	{
		Hinge,    // one rotation about an axis (JPH::HingeConstraint)
		Slider,   // one translation along an axis (JPH::SliderConstraint)
		Distance, // the distance between two points within limits (JPH::DistanceConstraint)
		Fixed     // no relative motion (JPH::FixedConstraint)
	};

	// A hinge's or slider's motor (Jolt's EMotorState without PositionAndVelocity).
	enum class PhysicsJointMotor : uint8_t
	{
		Off,      // no drive; MaxFriction resists the motion instead
		Velocity, // drives to MotorTargetVelocity with at most MotorMaxForce
		Position  // drives to MotorTargetPosition through a spring of MotorFrequency and MotorDamping, with at most MotorMaxForce
	};

	// A frame on one side of a joint, in that side's body space relative to the body origin (world space for the world):
	// a point and two perpendicular unit axes. Axis is the hinge's or slider's axis (a fixed joint's X axis); Normal is
	// perpendicular to it and fixes the hinge's angle 0 (a fixed joint's Y axis). A distance joint reads Point alone.
	struct PhysicsJointFrame
	{
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);

		bool operator==(const PhysicsJointFrame&) const = default;
	};

	// What a joint does besides joining its frames, changeable in place (PhysicsWorld::SetJointSettings). Each member says
	// which types read it; the others ignore it. Every float finite.
	struct PhysicsJointSettings
	{
		// Every type: whether the two bodies still collide with each other; false keeps them apart, so no contact and no
		// collision event is ever reported between them while the joint lives.
		bool CollideConnected = false;
		// Hinge and slider: whether LowerLimit and UpperLimit hold. Hinge limits in radians, LowerLimit in [-pi, 0] and
		// UpperLimit in [0, pi] (values beyond are clamped to Jolt's range); slider limits in metres, LowerLimit <= 0 <=
		// UpperLimit. Equal limits are refused (a FixedJoint is the joint that does not move). Distance: LowerLimit and
		// UpperLimit are the minimum and maximum distance in metres, 0 <= LowerLimit <= UpperLimit, always in force
		// (UseLimits ignored).
		bool UseLimits = false;
		float LowerLimit = 0.0f;
		float UpperLimit = 0.0f;
		// Distance: the spring that softens the limits, Hz (0: rigid) and damping ratio, each in [0, MaxPhysicsJointSpring].
		float LimitFrequency = 0.0f;
		float LimitDamping = 0.0f;
		// Hinge and slider: the friction torque (N m) or force (N) resisting the motion while the motor is Off, >= 0.
		float MaxFriction = 0.0f;
		// Hinge and slider: the motor. Targets in rad/s or m/s and rad or m, each at most MaxPhysicsJointMotorTarget in
		// magnitude (a position target is clamped to the limits while UseLimits holds, as Jolt clamps it); MotorMaxForce the
		// torque (N m) or force (N) it may apply, in [0, MaxPhysicsJointMotorForce]; the position drive's spring in Hz, in
		// [MinPhysicsJointMotorFrequency, MaxPhysicsJointSpring], and its damping ratio in [0, MaxPhysicsJointSpring].
		PhysicsJointMotor Motor = PhysicsJointMotor::Off;
		float MotorTargetVelocity = 0.0f;
		float MotorTargetPosition = 0.0f;
		float MotorMaxForce = 1000.0f;
		float MotorFrequency = 2.0f;
		float MotorDamping = 1.0f;

		bool operator==(const PhysicsJointSettings&) const = default;
	};

	// Everything a joint is created from.
	struct PhysicsJointDescription
	{
		PhysicsJointType Type = PhysicsJointType::Hinge;
		// The joint's body (required, live) and the body it is joined to (the invalid handle joins it to the world). They
		// differ.
		BodyHandle Body{};
		BodyHandle ConnectedBody{};
		// The joint's frame on Body and on ConnectedBody (Point within MaxPhysicsCoordinate; Axis and Normal unit and
		// perpendicular, within PhysicsUnitRotationTolerance). A hinge's angle and a slider's position are 0 where the two
		// frames coincide; a hinge turns Body about Axis, positive by the right-hand rule, and a slider moves it along Axis.
		PhysicsJointFrame Frame{};
		PhysicsJointFrame ConnectedFrame{};
		PhysicsJointSettings Settings{};
		// The solver order among joints (Jolt's constraint priority; higher solves later). Scene/PhysicsSystem gives each
		// joint its entity's rank in UUID order, so the order is a function of the scene and never of the world's history.
		uint32_t Priority = 0;
	};

	// A joint's reading after the last step (PhysicsWorld::GetJointState).
	struct PhysicsJointState
	{
		// Hinge: the angle in radians in (-pi, pi], Body's rotation about the axis from where the frames coincide; slider: the
		// position in metres along the axis; distance: the distance in metres between the two points; fixed: 0.
		float Value = 0.0f;
		// The force (N) and torque (N m) the joint applied in the last collision step: the length of its linear impulses
		// (the point and the limit parts) and of its angular ones (the rotation and the limit parts, never the motor's or the
		// friction's), divided by that collision step's duration. 0 before the first step and for a joint created since.
		float Force = 0.0f;
		float Torque = 0.0f;
		// Where the joint is now, in world space: its point on Body and on ConnectedBody (apart while the solver has not
		// closed a gap, or a distance joint's two ends), and a hinge's or slider's axis as ConnectedBody holds it (zero for a
		// distance or fixed joint). What the debug view draws in play.
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 ConnectedPoint = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(0.0f);
	};

	// The bounds the world checks a joint's settings against (PhysicsJointSettings); the joint components' registry
	// ranges are the same.
	//   - The fastest motor target, rad/s or m/s, and the farthest position target, rad or m (a hinge's is clamped to
	//     [-pi, pi] anyway): far beyond any game, so the motor's impulses stay finite.
	inline constexpr float MaxPhysicsJointMotorTarget = 1.0e6f;
	//   - The strongest motor, N m or N (the components' MotorMaxTorque and MotorMaxForce).
	inline constexpr float MaxPhysicsJointMotorForce = 1.0e12f;
	//   - The softest position drive, Hz: Jolt needs a spring with a frequency above 0.
	inline constexpr float MinPhysicsJointMotorFrequency = 0.001f;
	//   - The largest spring frequency (Hz) and damping ratio: far stiffer than a 60 Hz step can follow, and small enough
	//     that the spring's constants stay finite.
	inline constexpr float MaxPhysicsJointSpring = 1000.0f;

	// The constant factors between degrees and radians the joints convert with (no CRT transcendental on the simulation
	// path, §9.1).
	inline constexpr float JointDegreesToRadians = std::numbers::pi_v<float> / 180.0f;
	inline constexpr float JointRadiansToDegrees = 180.0f / std::numbers::pi_v<float>;

	// "Hinge", "Slider", "Distance" or "Fixed".
	[[nodiscard]] constexpr std::string_view PhysicsJointTypeToString(PhysicsJointType type)
	{
		switch (type)
		{
			case PhysicsJointType::Hinge:    return "Hinge";
			case PhysicsJointType::Slider:   return "Slider";
			case PhysicsJointType::Distance: return "Distance";
			case PhysicsJointType::Fixed:    return "Fixed";
		}
		return "Unknown";
	}

}
