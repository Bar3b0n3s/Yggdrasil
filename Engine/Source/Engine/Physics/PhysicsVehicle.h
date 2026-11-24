#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>
#include <numbers>
#include <vector>

// The vocabulary of the physics world's wheeled vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3;
// Architecture §9.11): Jolt's VehicleConstraint with its WheeledVehicleController on a Dynamic body of a PhysicsWorld, in
// engine types. PhysicsWorld's vehicles section (PhysicsWorld.h) creates, drives, changes, reads and destroys them;
// Scene/PhysicsSystem makes them from the WheeledVehicle and VehicleWheel components. Plain value types; thread-compatible.
//
// Units are Jolt's: metres, radians, newton metres, kilograms, seconds and revolutions per minute. Positions and directions
// are in the vehicle body's own space, relative to its origin (its PhysicsPose, not its centre of mass), so they survive a
// shape change. The axes are the engine's: forward is the body's local -Z, up its +Y and right its +X. A wheel's suspension
// runs along the body's -Y and it steers about the body's +Y.

namespace Engine {

	// One vehicle of a PhysicsWorld: a value the world hands out in creation order, from 1, never reused while the world
	// lives, so a handle of a destroyed vehicle never names another one. The default handle is invalid.
	class VehicleHandle
	{
	public:
		static constexpr uint32_t InvalidValue = 0;

		constexpr VehicleHandle() = default;
		explicit constexpr VehicleHandle(uint32_t value)
			: m_Value(value)
		{
		}

		[[nodiscard]] constexpr uint32_t GetValue() const { return m_Value; }
		[[nodiscard]] constexpr bool IsValid() const { return m_Value != InvalidValue; }

		constexpr auto operator<=>(const VehicleHandle&) const = default;
	private:
		uint32_t m_Value = InvalidValue;
	};

	// How a vehicle's wheels find the ground (Jolt's VehicleCollisionTesterRay, CastSphere and CastCylinder). Registry enum
	// "VehicleWheelCast" (Scene/Registration/VehiclesRegistration.cpp) has the same values.
	enum class PhysicsVehicleCast : uint8_t
	{
		Ray,     // one ray down each suspension: the cheapest, and a wheel drops into a crack narrower than it
		Sphere,  // a sphere of the vehicle's smallest wheel radius swept down each suspension
		Cylinder // each wheel's own cylinder (its radius and width) swept down its suspension: the wide tyre
	};

	// Whether a vehicle's gears change by themselves (Jolt's ETransmissionMode). Registry enum "VehicleTransmissionMode" has the
	// same values.
	enum class PhysicsVehicleTransmissionMode : uint8_t
	{
		Auto,  // shifts up above ShiftUpRpm and down below ShiftDownRpm, and into reverse for a negative Forward input
		Manual // the gear and clutch of the last PhysicsVehicleInput
	};

	// One key of a piecewise linear curve (Jolt's LinearCurve::Point): Y at X, linear in between and constant beyond the ends.
	// A curve's keys have strictly increasing X.
	struct PhysicsVehicleCurveKey
	{
		float X = 0.0f;
		float Y = 0.0f;

		bool operator==(const PhysicsVehicleCurveKey&) const = default;
	};

	// The caps of one vehicle (the world refuses more): wheels, differentials, anti-roll bars, forward and reverse gears, and
	// the keys of each curve.
	inline constexpr uint32_t MaxPhysicsVehicleWheels = 16;
	inline constexpr uint32_t MaxPhysicsVehicleDifferentials = 8;
	inline constexpr uint32_t MaxPhysicsVehicleAntiRollBars = 8;
	inline constexpr uint32_t MaxPhysicsVehicleGears = 8;
	inline constexpr uint32_t MaxPhysicsVehicleCurveKeys = 8;

	// The bounds the world checks a vehicle's values against (the components' registry ranges are the same), each far beyond
	// any real vehicle and small enough that Jolt's drivetrain and tyre impulses stay finite:
	//   - a wheel's radius, width and suspension lengths, metres;
	inline constexpr float MaxPhysicsVehicleLength = 100.0f;
	//   - the smallest wheel radius, metres (the registry's 1 mm minimum dimension);
	inline constexpr float MinPhysicsVehicleWheelRadius = 0.001f;
	//   - torques (the engine's, the brakes'), N m;
	inline constexpr float MaxPhysicsVehicleTorque = 1.0e6f;
	//   - the smallest and largest moment of inertia of a wheel or the engine, kg m^2 (Jolt divides torques by it);
	inline constexpr float MinPhysicsVehicleInertia = 0.001f;
	inline constexpr float MaxPhysicsVehicleInertia = 1.0e6f;
	//   - engine revolutions, rpm;
	inline constexpr float MaxPhysicsVehicleRpm = 100000.0f;
	//   - a gear's or a differential's ratio, in magnitude;
	inline constexpr float MaxPhysicsVehicleRatio = 100.0f;
	//   - damping rates, spring frequencies (Hz) and damping ratios, the clutch's strength, a bar's stiffness divided by 1000 (so
	//     a bar's is at most 1e6 N/m) and a curve's Y;
	inline constexpr float MaxPhysicsVehicleRate = 1000.0f;
	//   - a transmission's switch, release and latency times, seconds;
	inline constexpr float MaxPhysicsVehicleTime = 100.0f;
	//   - a differential's limited-slip ratio (0 is open, else above 1);
	inline constexpr float MaxPhysicsVehicleSlipRatio = 1000.0f;

	// One wheel (Jolt's WheelSettingsWV), in the body's space. The suspension runs from Position along the body's -Y for
	// between SuspensionMinLength and SuspensionMaxLength, so the wheel's centre at full droop is Position minus
	// SuspensionMaxLength along Y. Every float finite.
	struct PhysicsVehicleWheelDescription
	{
		// The suspension's attachment point, relative to the body origin; within MaxPhysicsCoordinate.
		glm::vec3 Position = glm::vec3(0.0f);
		// The wheel's up and forward at no steer (unit, Jolt's mWheelUp and mWheelForward; tilting them gives camber and toe).
		// Its axle is Forward x Up, the body's +X for an upright wheel.
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Forward = glm::vec3(0.0f, 0.0f, -1.0f);
		// Metres: Radius in [MinPhysicsVehicleWheelRadius, MaxPhysicsVehicleLength], Width and the lengths in
		// [0, MaxPhysicsVehicleLength], SuspensionMinLength <= SuspensionMaxLength. The spring's natural length is
		// SuspensionMaxLength + SuspensionPreload.
		float Radius = 0.3f;
		float Width = 0.1f;
		float SuspensionMinLength = 0.3f;
		float SuspensionMaxLength = 0.5f;
		float SuspensionPreload = 0.0f;
		// The suspension spring: Hz in (0, MaxPhysicsVehicleRate] and the damping ratio in [0, MaxPhysicsVehicleRate].
		float SpringFrequency = 1.5f;
		float SpringDamping = 0.5f;
		// Radians in [0, pi / 2] (Jolt asserts above).
		float MaxSteerAngle = 70.0f * std::numbers::pi_v<float> / 180.0f;
		// N m in [0, MaxPhysicsVehicleTorque].
		float MaxBrakeTorque = 1500.0f;
		float MaxHandBrakeTorque = 4000.0f;
		// kg m^2 in [MinPhysicsVehicleInertia, MaxPhysicsVehicleInertia], and the spin's damping rate in [0, MaxPhysicsVehicleRate].
		float Inertia = 0.9f;
		float AngularDamping = 0.2f;
		// The tyre's grip forward by slip ratio and sideways by slip angle in degrees (Jolt's curves, at most
		// MaxPhysicsVehicleCurveKeys keys each, X strictly increasing, Y in [0, MaxPhysicsVehicleRate]); the ground's friction
		// combines with each (PhysicsWorld.h). An empty curve grips nothing.
		std::vector<PhysicsVehicleCurveKey> LongitudinalFriction{ { 0.0f, 0.0f }, { 0.06f, 1.2f }, { 0.2f, 1.0f } };
		std::vector<PhysicsVehicleCurveKey> LateralFriction{ { 0.0f, 0.0f }, { 3.0f, 1.2f }, { 20.0f, 1.0f } };

		bool operator==(const PhysicsVehicleWheelDescription&) const = default;
	};

	// The engine (Jolt's VehicleEngineSettings).
	struct PhysicsVehicleEngine
	{
		// N m in [0, MaxPhysicsVehicleTorque].
		float MaxTorque = 500.0f;
		// rpm, 0 <= MinRpm <= MaxRpm <= MaxPhysicsVehicleRpm.
		float MinRpm = 1000.0f;
		float MaxRpm = 6000.0f;
		// The fraction of MaxTorque by RPM over MaxRpm (Jolt evaluates it there): X strictly increasing in [0, 1], Y in
		// [0, MaxPhysicsVehicleRate], at most MaxPhysicsVehicleCurveKeys keys.
		std::vector<PhysicsVehicleCurveKey> TorqueCurve{ { 0.0f, 0.8f }, { 0.66f, 1.0f }, { 1.0f, 0.8f } };
		// kg m^2 in [MinPhysicsVehicleInertia, MaxPhysicsVehicleInertia], and the damping rate in [0, MaxPhysicsVehicleRate].
		float Inertia = 0.5f;
		float AngularDamping = 0.2f;

		bool operator==(const PhysicsVehicleEngine&) const = default;
	};

	// The transmission (Jolt's VehicleTransmissionSettings).
	struct PhysicsVehicleTransmission
	{
		PhysicsVehicleTransmissionMode Mode = PhysicsVehicleTransmissionMode::Auto;
		// Forward ratios each in (0, MaxPhysicsVehicleRatio] and reverse ratios each in [-MaxPhysicsVehicleRatio, 0), at least
		// one forward gear and at most MaxPhysicsVehicleGears of each.
		std::vector<float> GearRatios{ 2.66f, 1.78f, 1.3f, 1.0f, 0.74f };
		std::vector<float> ReverseGearRatios{ -2.9f };
		// Seconds in [0, MaxPhysicsVehicleTime] (Auto only).
		float SwitchTime = 0.5f;
		float ClutchReleaseTime = 0.3f;
		float SwitchLatency = 0.5f;
		// rpm (Auto): 0 < ShiftDownRpm < ShiftUpRpm < the engine's MaxRpm.
		float ShiftUpRpm = 4000.0f;
		float ShiftDownRpm = 2000.0f;
		// (0, MaxPhysicsVehicleRate], kg m^2 / s.
		float ClutchStrength = 10.0f;

		bool operator==(const PhysicsVehicleTransmission&) const = default;
	};

	// A differential between two wheels (Jolt's VehicleDifferentialSettings): which wheels the engine drives through it.
	struct PhysicsVehicleDifferential
	{
		// Wheel indices of the vehicle (its PhysicsVehicleDescription::Wheels), -1 for none; not the same wheel twice.
		int32_t LeftWheel = -1;
		int32_t RightWheel = -1;
		// (0, MaxPhysicsVehicleRatio]: the gear box's rotation over the wheels'.
		float Ratio = 3.42f;
		// [0, 1]: the engine torque's share of the right wheel (0 all left, 1 all right).
		float LeftRightSplit = 0.5f;
		// 0 for an open differential (Jolt's FLT_MAX), else in (1, MaxPhysicsVehicleSlipRatio]: the largest ratio of the faster
		// wheel's speed to the slower's before all torque goes to the slower one.
		float LimitedSlipRatio = 1.4f;
		// [0, 1]: this differential's share of the engine torque; a vehicle's shares sum to 1 within 1e-3.
		float EngineTorqueRatio = 1.0f;

		bool operator==(const PhysicsVehicleDifferential&) const = default;
	};

	// An anti-roll bar between two wheels (Jolt's VehicleAntiRollBar).
	struct PhysicsVehicleAntiRollBar
	{
		// Wheel indices of the vehicle, both valid and different.
		int32_t LeftWheel = 0;
		int32_t RightWheel = 1;
		// N/m in [0, 1000 * MaxPhysicsVehicleRate].
		float Stiffness = 1000.0f;

		bool operator==(const PhysicsVehicleAntiRollBar&) const = default;
	};

	// What a vehicle drives with, changeable in place (PhysicsWorld::SetVehicleSettings): every member but the wheels and the
	// cast.
	struct PhysicsVehicleSettings
	{
		PhysicsVehicleEngine Engine{};
		PhysicsVehicleTransmission Transmission{};
		// At most MaxPhysicsVehicleDifferentials; none drives no wheel.
		std::vector<PhysicsVehicleDifferential> Differentials{};
		// The ratio between the differentials' speeds (Jolt's mDifferentialLimitedSlipRatio): 0 open, else in
		// (1, MaxPhysicsVehicleSlipRatio].
		float DifferentialLimitedSlipRatio = 1.4f;
		// At most MaxPhysicsVehicleAntiRollBars.
		std::vector<PhysicsVehicleAntiRollBar> AntiRollBars{};
		// Radians in [0, pi]: how far the body's up may tilt from the world's (the opposite of gravity) before the vehicle holds
		// it; pi is off.
		float MaxPitchRollAngle = std::numbers::pi_v<float>;

		bool operator==(const PhysicsVehicleSettings&) const = default;
	};

	// Everything a vehicle is created from.
	struct PhysicsVehicleDescription
	{
		// A live Dynamic body that holds no vehicle yet.
		BodyHandle Body{};
		// 1 to MaxPhysicsVehicleWheels wheels; a wheel's index is its place here.
		std::vector<PhysicsVehicleWheelDescription> Wheels{};
		PhysicsVehicleSettings Settings{};
		PhysicsVehicleCast Cast = PhysicsVehicleCast::Cylinder;
		// The solver order among the world's constraints (Jolt's constraint priority; higher solves later). Scene/PhysicsSystem
		// gives its vehicles the ranks after its joints' in UUID order (PhysicsWorld.h).
		uint32_t Priority = 0;
	};

	// The driver's input of one vehicle (PhysicsWorld::SetVehicleInput), in force from the next step until it is set again.
	// Every float finite; each is clamped to its range.
	struct PhysicsVehicleInput
	{
		// [-1, 1]: the accelerator, negative to reverse (Auto) or with a reverse gear (Manual).
		float Forward = 0.0f;
		// [-1, 1]: the steering, +1 a full right lock.
		float Right = 0.0f;
		// [0, 1] each.
		float Brake = 0.0f;
		float HandBrake = 0.0f;
		// Manual only: the gear (-reverse gear count to forward gear count, 0 neutral; clamped to the counts) and the clutch's
		// friction in [0, 1] (1 engaged).
		int32_t Gear = 0;
		float Clutch = 1.0f;

		bool operator==(const PhysicsVehicleInput&) const = default;
	};

	// One wheel's reading after the last step (PhysicsVehicleState::Wheels).
	struct PhysicsVehicleWheelState
	{
		// Whether the wheel found ground in the last step, what it touched (the body, a Jolt SubShapeID value and the collider,
		// ColliderShapeDescription::UserData, as a contact would name it; the invalid handle and 0 when the ground's body has
		// been destroyed since) and where, in world space (zero without a touch).
		bool Touching = false;
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		BodyHandle ContactBody{};
		uint32_t ContactSubShape = 0;
		uint32_t ContactCollider = 0;
		// Metres from the attachment point to the wheel's centre; the force (N) the suspension applied in the last collision
		// step (Jolt's suspension lambda over that step's duration).
		float SuspensionLength = 0.0f;
		float SuspensionForce = 0.0f;
		// Radians: the spin in [0, 2 pi) and the steer (Jolt's: positive to the left); rad/s, positive rolling forward.
		float Angle = 0.0f;
		float SteerAngle = 0.0f;
		float AngularVelocity = 0.0f;
		// The slip ratio and the slip angle in radians (Jolt's WheelWV::mLongitudinalSlip and mLateralSlip).
		float LongitudinalSlip = 0.0f;
		float LateralSlip = 0.0f;
		// The wheel's pose in the body's space relative to its origin (Jolt's GetWheelLocalTransform with the wheel model's
		// right +X and up +Y): suspension, steer and spin; the identity rotation at the wheel's authored up and forward.
		glm::mat4 LocalPose = glm::mat4(1.0f);
	};

	// A vehicle's reading after the last step (PhysicsWorld::GetVehicleState).
	struct PhysicsVehicleState
	{
		float EngineRpm = 0.0f;
		// -reverse count to forward count, 0 neutral; the clutch's friction in [0, 1]; whether a gear change is in progress.
		int32_t Gear = 0;
		float ClutchFriction = 0.0f;
		bool Shifting = false;
		std::vector<PhysicsVehicleWheelState> Wheels{};
	};

}
