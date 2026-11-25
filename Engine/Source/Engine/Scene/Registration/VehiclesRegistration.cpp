#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Physics/PhysicsVehicle.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <string_view>

// The Vehicles category (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; ADR 0060's phase-8 ledger: the
// seventeenth category, after Joints, the first of phase 8's four): WheeledVehicle on a Dynamic RigidBody's entity and
// VehicleWheel on each wheel below it (Architecture §5.3, §9.11). Field bounds are the physics world's (Physics/
// PhysicsVehicle.h), so every value Jolt receives is valid (§9.1). What spans fields, lists or entities (the lists' caps,
// MinRpm above MaxRpm, the gear ratios' signs, the shift points, the differentials' and bars' wheels and torque shares, a
// curve's order, a SuspensionMinLength above SuspensionMaxLength, the values that must be above 0) is PHYSICS_VEHICLE_INVALID's
// (Scene/PhysicsVehicles.h), never a write error, so a script may set the fields in any order.

namespace Engine {

	namespace Utils {

		constexpr double MaxVehicleLength = static_cast<double>(MaxPhysicsVehicleLength);
		constexpr double MaxVehicleTorque = static_cast<double>(MaxPhysicsVehicleTorque);
		constexpr double MinVehicleInertia = static_cast<double>(MinPhysicsVehicleInertia);
		constexpr double MaxVehicleInertia = static_cast<double>(MaxPhysicsVehicleInertia);
		constexpr double MaxVehicleRpm = static_cast<double>(MaxPhysicsVehicleRpm);
		constexpr double MaxVehicleRatio = static_cast<double>(MaxPhysicsVehicleRatio);
		constexpr double MaxVehicleRate = static_cast<double>(MaxPhysicsVehicleRate);
		constexpr double MaxVehicleTime = static_cast<double>(MaxPhysicsVehicleTime);
		constexpr double MaxVehicleSlipRatio = static_cast<double>(MaxPhysicsVehicleSlipRatio);
		constexpr double MaxVehicleBarStiffness = static_cast<double>(MaxPhysicsVehicleBarStiffness);
		constexpr double MaxVehicleGears = static_cast<double>(MaxPhysicsVehicleGears);
		// A curve key's X (a slip ratio, a slip angle in degrees, or the engine's RPM over MaxRpm) and Y.
		constexpr double MaxVehicleCurveX = 1000.0;

		constexpr std::string_view WheelReferenceDescription =
			"A VehicleWheel entity of this vehicle, the nearest WheeledVehicle above it; null for none.";
		constexpr std::string_view LimitedSlipDescription =
			"0 for an open differential, else above 1: the largest ratio of the faster side's speed to the slower's before the torque goes to "
			"the slower one.";

	}

	void RegisterVehiclesComponents(TypeRegistry& registry)
	{
		registry.Enum<VehicleTransmissionMode>("VehicleTransmissionMode", "Whether a WheeledVehicle's gears change by themselves.")
			.Entry(VehicleTransmissionMode::Auto, "Auto",
				"Shifts up above ShiftUpRpm and down below ShiftDownRpm, and into reverse for a negative Throttle, releasing the clutch on its own.")
			.Entry(VehicleTransmissionMode::Manual, "Manual", "Drives in the component's Gear with its Clutch, as a script or graph sets them.");

		registry.Enum<VehicleWheelCast>("VehicleWheelCast", "How a WheeledVehicle's wheels find the ground under them.")
			.Entry(VehicleWheelCast::Ray, "Ray", "One ray down each suspension: the cheapest, and a wheel drops into a crack narrower than it.")
			.Entry(VehicleWheelCast::Sphere, "Sphere", "A sphere of the vehicle's smallest wheel Radius swept down each suspension.")
			.Entry(VehicleWheelCast::Cylinder, "Cylinder", "Each wheel's own cylinder, its Radius and Width, swept down its suspension: the wide tyre.");

		registry.Struct<VehicleCurveKey>("VehicleCurveKey", "One key of a vehicle's curve: Y at X, linear between keys and constant beyond the ends.")
			.Field("X", &VehicleCurveKey::X, "Where the key is on the curve; a curve's keys have strictly increasing X.",
				{ .Min = -Utils::MaxVehicleCurveX, .Max = Utils::MaxVehicleCurveX })
			.Field("Y", &VehicleCurveKey::Y, "The curve's value at X.", { .Min = 0.0, .Max = Utils::MaxVehicleRate });

		registry.Struct<VehicleDifferential>("VehicleDifferential", "A differential through which a WheeledVehicle's engine drives two of its wheels.")
			.Field("LeftWheel", &VehicleDifferential::LeftWheel, Utils::WheelReferenceDescription)
			.Field("RightWheel", &VehicleDifferential::RightWheel, Utils::WheelReferenceDescription)
			.Field("Ratio", &VehicleDifferential::Ratio, "The gear box's rotation over the wheels' (the final drive), above 0.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRatio })
			.Field("LeftRightSplit", &VehicleDifferential::LeftRightSplit, "The engine torque's share of the right wheel: 0 all left, 1 all right.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("LimitedSlipRatio", &VehicleDifferential::LimitedSlipRatio, Utils::LimitedSlipDescription,
				{ .Min = 0.0, .Max = Utils::MaxVehicleSlipRatio })
			.Field("EngineTorqueRatio", &VehicleDifferential::EngineTorqueRatio,
				"This differential's share of the engine torque; the shares of a vehicle's differentials that name a wheel sum to 1.", { .Min = 0.0, .Max = 1.0 });

		registry.Struct<VehicleAntiRollBar>("VehicleAntiRollBar", "An anti-roll bar between two wheels of a WheeledVehicle, which keeps it from leaning.")
			.Field("LeftWheel", &VehicleAntiRollBar::LeftWheel, Utils::WheelReferenceDescription)
			.Field("RightWheel", &VehicleAntiRollBar::RightWheel, Utils::WheelReferenceDescription)
			.Field("Stiffness", &VehicleAntiRollBar::Stiffness,
				"How strongly the bar pushes the less compressed wheel's suspension down, in newtons per metre of difference.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleBarStiffness, .Unit = "N/m" });

		RegisterComponent<WheeledVehicleComponent>(registry, "WheeledVehicle",
			"Makes the entity's Dynamic RigidBody a wheeled vehicle, driven by an engine through a transmission and differentials on the "
			"VehicleWheel entities below it; scripts or graphs set its Throttle, Steering, Brake and HandBrake every tick.")
			.Category("Vehicles")
			.Version(1)
			.Requires<TransformComponent>()
			.Requires<RigidBodyComponent>()
			.Field("Throttle", &WheeledVehicleComponent::Throttle,
				"The accelerator, from -1 to 1. In Auto a negative Throttle reverses (with a reverse gear); in Manual it counts from 0 to 1 and "
				"the Gear gives the direction. Applies at the next physics step.",
				{ .Min = -1.0, .Max = 1.0 })
			.Field("Steering", &WheeledVehicleComponent::Steering, "The steering, from -1 (full left lock) to 1 (full right lock).", { .Min = -1.0, .Max = 1.0 })
			.Field("Brake", &WheeledVehicleComponent::Brake, "The brake pedal, from 0 to 1, on every wheel's MaxBrakeTorque.", { .Min = 0.0, .Max = 1.0 })
			.Field("HandBrake", &WheeledVehicleComponent::HandBrake, "The hand brake, from 0 to 1, on every wheel's MaxHandBrakeTorque.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Gear", &WheeledVehicleComponent::Gear,
				"Manual only: the selected gear, negative for a reverse gear and 0 for neutral, clamped to the gears the vehicle has.",
				{ .Min = -Utils::MaxVehicleGears, .Max = Utils::MaxVehicleGears })
			.Field("Clutch", &WheeledVehicleComponent::Clutch, "Manual only: the clutch, from 0 (released) to 1 (engaged).", { .Min = 0.0, .Max = 1.0 })
			.Field("MaxTorque", &WheeledVehicleComponent::MaxTorque, "The engine's largest torque, in newton metres, which TorqueCurve scales by its RPM.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleTorque, .Unit = "N m" })
			.Field("MinRpm", &WheeledVehicleComponent::MinRpm, "The engine's idle RPM, the least it turns at; at most MaxRpm.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRpm, .Unit = "rpm" })
			.Field("MaxRpm", &WheeledVehicleComponent::MaxRpm, "The engine's largest RPM, which it never exceeds.",
				{ .Min = 1.0, .Max = Utils::MaxVehicleRpm, .Unit = "rpm" })
			.Field("EngineInertia", &WheeledVehicleComponent::EngineInertia, "The moment of inertia of the engine's turning parts.",
				{ .Min = Utils::MinVehicleInertia, .Max = Utils::MaxVehicleInertia, .Unit = "kg m^2" })
			.Field("EngineDamping", &WheeledVehicleComponent::EngineDamping, "How quickly the engine slows down by itself, per second.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRate, .Unit = "1/s" })
			.Field("TorqueCurve", &WheeledVehicleComponent::TorqueCurve,
				"The fraction of MaxTorque by the engine's RPM over MaxRpm (X from 0 to 1, at most eight keys). The torque at X = 1 stays in "
				"force at MaxRpm, so a curve that falls to 0 there is a rev limiter, which caps the speed in each gear.")
			.Field("Transmission", &WheeledVehicleComponent::Transmission, "Whether the gears change by themselves (Auto) or as Gear says (Manual).")
			.Field("GearRatios", &WheeledVehicleComponent::GearRatios,
				"The forward gears' ratios of the engine's rotation over the gear box's, each above 0, first gear first: one to eight gears.")
			.Field("ReverseGearRatios", &WheeledVehicleComponent::ReverseGearRatios,
				"The reverse gears' ratios, each below 0: at most eight; none, and an Auto vehicle never reverses.")
			.Field("SwitchTime", &WheeledVehicleComponent::SwitchTime, "Auto: how long a gear change takes, in seconds.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleTime, .Unit = "s" })
			.Field("ClutchReleaseTime", &WheeledVehicleComponent::ClutchReleaseTime, "Auto: how long the clutch takes to engage after a gear change.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleTime, .Unit = "s" })
			.Field("SwitchLatency", &WheeledVehicleComponent::SwitchLatency, "Auto: the least time between two gear changes, in seconds.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleTime, .Unit = "s" })
			.Field("ShiftUpRpm", &WheeledVehicleComponent::ShiftUpRpm,
				"Auto: the RPM above which it shifts up; above ShiftDownRpm and below MaxRpm.", { .Min = 0.0, .Max = Utils::MaxVehicleRpm, .Unit = "rpm" })
			.Field("ShiftDownRpm", &WheeledVehicleComponent::ShiftDownRpm, "Auto: the RPM below which it shifts down; above 0.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRpm, .Unit = "rpm" })
			.Field("ClutchStrength", &WheeledVehicleComponent::ClutchStrength,
				"How strongly the engaged clutch couples the engine to the wheels, above 0.", { .Min = 0.0, .Max = Utils::MaxVehicleRate, .Unit = "kg m^2/s" })
			.Field("Differentials", &WheeledVehicleComponent::Differentials,
				"The differentials the engine drives wheels through, at most eight, at least one naming a wheel; a vehicle that drives none (a "
				"trailer) has one, in Manual at Gear 0.")
			.Field("DifferentialLimitedSlipRatio", &WheeledVehicleComponent::DifferentialLimitedSlipRatio,
				"Between the differentials: 0 open, else above 1, the largest ratio of the fastest differential's speed to the slowest's.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleSlipRatio })
			.Field("AntiRollBars", &WheeledVehicleComponent::AntiRollBars, "The anti-roll bars between pairs of wheels, at most eight.")
			.Field("MaxPitchRollAngle", &WheeledVehicleComponent::MaxPitchRollAngle,
				"How far the vehicle may tilt from upright, in degrees, before it is held there; 180 never holds it.",
				{ .Min = 0.0, .Max = 180.0, .Unit = "deg" })
			.Field("WheelCast", &WheeledVehicleComponent::WheelCast, "How the wheels find the ground under them.");

		RegisterComponent<VehicleWheelComponent>(registry, "VehicleWheel",
			"Makes the entity a wheel of the nearest WheeledVehicle above it: in play the engine writes its pose at each step, so its mesh rides "
			"the suspension, steers and spins.")
			.Category("Vehicles")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Radius", &VehicleWheelComponent::Radius, "The wheel's radius, in metres (never scaled).",
				{ .Min = static_cast<double>(MinPhysicsVehicleWheelRadius), .Max = Utils::MaxVehicleLength, .Unit = "m" })
			.Field("Width", &VehicleWheelComponent::Width, "The wheel's width, in metres (never scaled); a Cylinder cast uses it.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleLength, .Unit = "m" })
			.Field("SuspensionMinLength", &VehicleWheelComponent::SuspensionMinLength,
				"The suspension's shortest length, fully compressed, in metres; at most SuspensionMaxLength.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleLength, .Unit = "m" })
			.Field("SuspensionMaxLength", &VehicleWheelComponent::SuspensionMaxLength,
				"The suspension's longest length, at full droop, in metres: the wheel hangs at its authored place at this length.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleLength, .Unit = "m" })
			.Field("SuspensionPreload", &VehicleWheelComponent::SuspensionPreload,
				"How far the spring is compressed at full droop, in metres, so it pushes already there.", { .Min = 0.0, .Max = Utils::MaxVehicleLength, .Unit = "m" })
			.Field("SpringFrequency", &VehicleWheelComponent::SpringFrequency, "The suspension spring's natural frequency, in hertz, above 0: higher is stiffer.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRate, .Unit = "Hz" })
			.Field("SpringDamping", &VehicleWheelComponent::SpringDamping, "The suspension's damping ratio: 0 bounces, 1 is critically damped.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRate })
			.Field("MaxSteerAngle", &VehicleWheelComponent::MaxSteerAngle, "The wheel's steer at full lock, in degrees; 0 for a wheel that does not steer.",
				{ .Min = 0.0, .Max = 90.0, .Unit = "deg" })
			.Field("MaxBrakeTorque", &VehicleWheelComponent::MaxBrakeTorque, "The brake's torque at a full Brake, in newton metres.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleTorque, .Unit = "N m" })
			.Field("MaxHandBrakeTorque", &VehicleWheelComponent::MaxHandBrakeTorque,
				"The hand brake's torque at a full HandBrake, in newton metres; 0 for a wheel without one.", { .Min = 0.0, .Max = Utils::MaxVehicleTorque, .Unit = "N m" })
			.Field("Inertia", &VehicleWheelComponent::Inertia, "The wheel's moment of inertia about its axle.",
				{ .Min = Utils::MinVehicleInertia, .Max = Utils::MaxVehicleInertia, .Unit = "kg m^2" })
			.Field("AngularDamping", &VehicleWheelComponent::AngularDamping, "How quickly the wheel's spin slows down by itself, per second.",
				{ .Min = 0.0, .Max = Utils::MaxVehicleRate, .Unit = "1/s" })
			.Field("LongitudinalFriction", &VehicleWheelComponent::LongitudinalFriction,
				"The tyre's grip forward and back by its slip ratio (X), which the ground's friction scales; at most eight keys.")
			.Field("LateralFriction", &VehicleWheelComponent::LateralFriction,
				"The tyre's grip sideways by its slip angle in degrees (X), which the ground's friction scales; at most eight keys.");
	}

}
