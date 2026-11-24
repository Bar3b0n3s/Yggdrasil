#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <vector>

namespace Engine {

	// Registry enum "VehicleTransmissionMode" (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3): whether a
	// WheeledVehicle's gears change by themselves (the world's PhysicsVehicleTransmissionMode, Physics/PhysicsVehicle.h).
	enum class VehicleTransmissionMode : uint8_t
	{
		Auto,  // shifts up above ShiftUpRpm and down below ShiftDownRpm, and into reverse for a negative Throttle
		Manual // the component's Gear and Clutch
	};

	// Registry enum "VehicleWheelCast" (M44): how a WheeledVehicle's wheels find the ground (the world's PhysicsVehicleCast).
	enum class VehicleWheelCast : uint8_t
	{
		Ray,     // one ray down each suspension
		Sphere,  // a sphere of the vehicle's smallest wheel Radius swept down each suspension
		Cylinder // each wheel's own cylinder (its Radius and Width) swept down its suspension
	};

	// Registry struct "VehicleCurveKey" (M44): one key of a piecewise linear curve, Y at X, linear in between and constant
	// beyond the ends (Jolt's LinearCurve). A curve's keys have strictly increasing X (PHYSICS_VEHICLE_INVALID, Subject
	// "curve", otherwise).
	struct VehicleCurveKey
	{
		float X = 0.0f; // [-1000, 1000]
		float Y = 0.0f; // [0, 1000]

		bool operator==(const VehicleCurveKey&) const = default;
	};

	// Registry struct "VehicleDifferential" (M44): a differential between two wheels of the vehicle, through which the engine
	// drives them. A wheel is a VehicleWheel entity of the vehicle (an EntityRef, which prefab instantiation remaps); null for
	// none.
	struct VehicleDifferential
	{
		UUID LeftWheel{};               // EntityRef
		UUID RightWheel{};              // EntityRef
		float Ratio = 3.42f;            // (0, 100]
		float LeftRightSplit = 0.5f;    // [0, 1]: the engine torque's share of the right wheel
		float LimitedSlipRatio = 1.4f;  // 0 open, else (1, 1000]
		float EngineTorqueRatio = 1.0f; // [0, 1]; the ratios of the vehicle's differentials that name a wheel sum to 1

		bool operator==(const VehicleDifferential&) const = default;
	};

	// Registry struct "VehicleAntiRollBar" (M44): an anti-roll bar between two wheels of the vehicle (EntityRefs, both set and
	// different).
	struct VehicleAntiRollBar
	{
		UUID LeftWheel{};          // EntityRef
		UUID RightWheel{};         // EntityRef
		float Stiffness = 1000.0f; // N/m, [0, MaxPhysicsVehicleBarStiffness]

		bool operator==(const VehicleAntiRollBar&) const = default;
	};

	// Registry name "WheeledVehicle", version 1, category Vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3;
	// Architecture §5.3, §9.11): makes the entity's Dynamic RigidBody a wheeled vehicle (Jolt's VehicleConstraint with its
	// WheeledVehicleController) on the VehicleWheel entities below it. Requires Transform and RigidBody (which excludes
	// CharacterController, so a vehicle is never a character). The axes are the entity's: forward its local -Z, up its +Y.
	//   - Driver input (Throttle to Clutch), written by scripts or graphs at every tick, applies in place at the next PreStep.
	//     Throttle reverses only in Auto with a reverse gear; Manual drives by a Throttle in [0, 1] in its Gear's direction.
	//   - The engine, transmission, differentials, limited slip, the anti-roll bars' Stiffness and MaxPitchRollAngle apply in
	//     place; a change of WheelCast or of the bars' wheels (a bar added, removed or moved) makes the vehicle again (keeping
	//     its motion).
	// Field ranges are the world's bounds (Physics/PhysicsVehicle.h); what spans fields, lists or entities (the caps of the
	// lists, MinRpm above MaxRpm, gear ratios of the wrong sign, the shift points, the differentials' and bars' wheels, the
	// torque shares, the curves' order) is PHYSICS_VEHICLE_INVALID's (Scene/PhysicsVehicles.h), never a write error, so a script
	// may set the fields in any order. Angles in degrees.
	struct WheeledVehicleComponent
	{
		// Driver input.
		float Throttle = 0.0f;  // [-1, 1]
		float Steering = 0.0f;  // [-1, 1], +1 full right lock
		float Brake = 0.0f;     // [0, 1]
		float HandBrake = 0.0f; // [0, 1]
		int32_t Gear = 0;       // Manual: [-8, 8], 0 neutral, clamped to the gears
		float Clutch = 1.0f;    // Manual: [0, 1], 1 engaged
		// Engine.
		float MaxTorque = 500.0f; // N m
		float MinRpm = 1000.0f;
		float MaxRpm = 6000.0f;
		float EngineInertia = 0.5f; // kg m^2
		float EngineDamping = 0.2f;
		std::vector<VehicleCurveKey> TorqueCurve{ { 0.0f, 0.8f }, { 0.66f, 1.0f }, { 1.0f, 0.8f } };
		// Transmission.
		VehicleTransmissionMode Transmission = VehicleTransmissionMode::Auto;
		std::vector<float> GearRatios{ 2.66f, 1.78f, 1.3f, 1.0f, 0.74f };
		std::vector<float> ReverseGearRatios{ -2.9f };
		float SwitchTime = 0.5f;        // s
		float ClutchReleaseTime = 0.3f; // s
		float SwitchLatency = 0.5f;     // s
		float ShiftUpRpm = 4000.0f;
		float ShiftDownRpm = 2000.0f;
		float ClutchStrength = 10.0f;
		// Drivetrain. At least one differential names a wheel (Jolt drives the wheels through one), so the default, which names
		// none, is PHYSICS_VEHICLE_INVALID ("differential") until the vehicle's wheels are named; a vehicle that drives none (a
		// trailer) has one, in Manual at Gear 0.
		std::vector<VehicleDifferential> Differentials{};
		float DifferentialLimitedSlipRatio = 1.4f; // 0 open, else (1, 1000]
		std::vector<VehicleAntiRollBar> AntiRollBars{};
		float MaxPitchRollAngle = 180.0f; // [0, 180], 180 off
		VehicleWheelCast WheelCast = VehicleWheelCast::Cylinder;

		bool operator==(const WheeledVehicleComponent&) const = default;
	};

}
