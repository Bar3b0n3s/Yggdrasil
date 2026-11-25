#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"

#include <vector>

namespace Engine {

	// Registry name "VehicleWheel", version 1, category Vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3;
	// Architecture §5.3, §9.11): makes the entity a wheel of the nearest WheeledVehicle above it, with no other RigidBody or
	// CharacterController on the way. Requires Transform. Wheels take Jolt's wheel index in canonical order below the vehicle.
	//   - The wheel's authored pose is its neutral one: it hangs at its authored centre at full droop (its suspension's
	//     attachment point is that centre plus the vehicle's up times SuspensionMaxLength), its authored rotation gives camber
	//     and toe with the axle along its local X, and its suspension runs along the vehicle's -Y and steers about its +Y.
	//   - In play the engine writes the entity's local Translation and Rotation at every step (suspension, steer and spin), so
	//     a mesh on it rides the wheel; a solid collider on or below it is PHYSICS_VEHICLE_INVALID.
	//   - A write to a field makes the vehicle again, keeping its motion.
	// Lengths in metres, never scaled; angles in degrees. Field ranges are the world's bounds (Physics/PhysicsVehicle.h);
	// SuspensionMinLength above SuspensionMaxLength, a SpringFrequency of 0 and a curve whose X does not increase are
	// PHYSICS_VEHICLE_INVALID's.
	struct VehicleWheelComponent
	{
		float Radius = 0.3f;
		float Width = 0.1f;
		float SuspensionMinLength = 0.3f;
		float SuspensionMaxLength = 0.5f;
		float SuspensionPreload = 0.0f;
		float SpringFrequency = 1.5f; // Hz
		float SpringDamping = 0.5f;
		float MaxSteerAngle = 70.0f;        // [0, 90]
		float MaxBrakeTorque = 1500.0f;     // N m
		float MaxHandBrakeTorque = 4000.0f; // N m
		float Inertia = 0.9f;               // kg m^2
		float AngularDamping = 0.2f;
		// The tyre's grip forward by slip ratio, and sideways by slip angle in degrees; the ground's friction combines with each.
		std::vector<VehicleCurveKey> LongitudinalFriction{ { 0.0f, 0.0f }, { 0.06f, 1.2f }, { 0.2f, 1.0f } };
		std::vector<VehicleCurveKey> LateralFriction{ { 0.0f, 0.0f }, { 3.0f, 1.2f }, { 20.0f, 1.0f } };

		bool operator==(const VehicleWheelComponent&) const = default;
	};

}
