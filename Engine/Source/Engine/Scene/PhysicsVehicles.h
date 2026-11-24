#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsVehicle.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The vehicle composition rules (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §5.3, §9.11): which
// body each WheeledVehicle drives and which VehicleWheel entities are its wheels, which vehicles cannot be made as authored,
// and the components read in the physics world's units. One pure function over ComposePhysicsBodies' result, never changing it,
// used by the play session's PhysicsSystem (which creates the vehicles), the project validator (which reports them) and the
// vehicle debug draw (which draws them), so the rules exist once.
//
// The rules, over effectively enabled entities only (DisabledTag on the entity or an ancestor excludes it, as the body
// composition reads it):
//   - A vehicle's body is its own entity's RigidBody's (WheeledVehicle requires one), which must be a creatable, Dynamic body
//     that is not a trigger.
//   - A VehicleWheel belongs to the nearest WheeledVehicle at or above its entity. Its wheels are listed in canonical order
//     (§5.1), which is the world's wheel index (PhysicsVehicleDescription::Wheels).
//   - PHYSICS_VEHICLE_INVALID (Error, not fixable; one per vehicle, the first reason in this order, each its own Subject):
//     its RigidBody makes no Dynamic body (no solid collider, Static, Kinematic or a trigger: "body"); no wheel, or more than
//     MaxPhysicsVehicleWheels ("wheels"); a wheel on the vehicle's own entity, or with a RigidBody or CharacterController on
//     it or between it and the vehicle ("wheel"); a solid collider on or below a wheel, which the vehicle's body gathers
//     ("collider"); a wheel's SuspensionMinLength above its SuspensionMaxLength, or a SpringFrequency of 0 ("suspension");
//     MinRpm above MaxRpm ("engine"); no forward gear, more gears of a kind than MaxPhysicsVehicleGears, a forward ratio not
//     above 0 or a reverse ratio not below 0, ShiftDownRpm 0, ShiftUpRpm not above ShiftDownRpm, an Auto ShiftUpRpm not below
//     MaxRpm, or a ClutchStrength of 0 ("transmission"); more differentials than MaxPhysicsVehicleDifferentials, a
//     differential's wheel that is not a wheel of the vehicle, one wheel twice, a Ratio of 0, a limited-slip ratio in (0, 1]
//     (its own or the vehicle's DifferentialLimitedSlipRatio), no differential that names a wheel, or shares of the engine
//     torque of those that do that do not sum to 1 within 1e-6, summed in float in list order (Jolt's own check, at every
//     step; "differential"); more bars than MaxPhysicsVehicleAntiRollBars, a bar without two different wheels of the vehicle
//     ("bar"); a curve with more than MaxPhysicsVehicleCurveKeys keys, whose X does not increase strictly, or a TorqueCurve
//     key's X outside [0, 1] ("curve"). Such a vehicle is listed not creatable, and its body drives as a plain body. A vehicle
//     whose body a body diagnostic refused (the plan is not creatable while it has colliders) is listed not creatable without
//     a second diagnostic.
//   - A differential's or bar's wheel that names a VehicleWheel entity that is effectively disabled is no wheel (the
//     differential's side drives nothing, the bar is left out), without a diagnostic, so a disabled wheel never makes its
//     vehicle invalid; a vehicle whose disabled wheels leave it no driven wheel, or shares that do not sum to 1, is listed not
//     creatable without a diagnostic until its wheels are enabled again.
//
// Determinism: a pure function of the scene and the body composition; canonical order throughout. Main thread (the scene).

namespace Engine {

	class ConstEntity;
	class Scene;
	struct VehicleWheelComponent;
	struct WheeledVehicleComponent;

	// Error, not fixable: a WheeledVehicle that cannot be made as authored (the rules above). The vehicle is not created and its
	// body simulates as a plain body.
	inline constexpr std::string_view PhysicsVehicleInvalidCode = "PHYSICS_VEHICLE_INVALID";

	// M44's vehicle code, in the validator's order: an array of its own, appended after the mass code in both of
	// ProjectValidator.cpp's code tables (ADR 0060's phase-8 ledger, the validation rows).
	inline constexpr std::array<std::string_view, 1> PhysicsVehicleDiagnosticCodes = { PhysicsVehicleInvalidCode };

	// One vehicle the scene makes.
	struct PhysicsVehiclePlan
	{
		UUID Entity{}; // the entity holding the WheeledVehicle
		// The owner of the vehicle's body (its own entity) when its RigidBody makes a Dynamic body; the invalid UUID otherwise.
		UUID Body{};
		// Its wheels' entities in canonical order: the world's wheel indices.
		std::vector<UUID> Wheels{};
		// False when PHYSICS_VEHICLE_INVALID refuses it, or when its body's plan is not creatable (no diagnostic then).
		bool IsCreatable = true;
	};

	struct PhysicsVehicleComposition
	{
		std::vector<PhysicsVehiclePlan> Vehicles{};
		// PHYSICS_VEHICLE_INVALID diagnostics, on the vehicle's entity and its WheeledVehicle, sorted by (entity UUID, code,
		// subject).
		std::vector<PhysicsDiagnostic> Diagnostics{};
	};

	// The vehicles of `scene` under the rules above, `bodies` being ComposePhysicsBodies' result for the same scene. Works on
	// edit and runtime scenes.
	[[nodiscard]] PhysicsVehicleComposition ComposePhysicsVehicles(const Scene& scene, const PhysicsComposition& bodies);

	// The entity holding the WheeledVehicle that `entity` is or belongs to as a wheel: `entity` itself when it holds one, else,
	// when it holds a VehicleWheel, the nearest WheeledVehicle above it; an invalid entity otherwise. Works on readable entities.
	[[nodiscard]] ConstEntity FindPhysicsVehicleEntity(ConstEntity entity);

	// A wheel's neutral frame relative to its vehicle's body origin (the body's space, Physics/PhysicsVehicle.h): its authored
	// centre, its up (its local +Y) and its forward (its local -Z), from the chain of local matrices from the vehicle's entity
	// (its own scale included, since the body pose has none) down to the wheel (its own transform included, its scale not), so
	// it does not depend on world matrices. Up and Forward are unit and perpendicular.
	struct PhysicsVehicleWheelFrame
	{
		glm::vec3 Center = glm::vec3(0.0f);
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Forward = glm::vec3(0.0f, 0.0f, -1.0f);

		bool operator==(const PhysicsVehicleWheelFrame&) const = default;
	};

	// The vehicle's own scale times the local matrices of the entities below `vehicle` down to `entity`, included: what turns
	// `entity`'s space into the vehicle's body space (the body pose has no scale). `entity` is `vehicle` or a descendant of it;
	// for `vehicle` itself, its scale alone.
	[[nodiscard]] glm::mat4 GetPhysicsVehicleChainMatrix(ConstEntity vehicle, ConstEntity entity);
	// The frame of the wheel `wheel` below `vehicle` from the scene's local transforms now; nullopt when the chain does not
	// decompose (TransformSystem::DecomposeMatrix) or is not finite.
	[[nodiscard]] std::optional<PhysicsVehicleWheelFrame> MakePhysicsVehicleWheelFrame(ConstEntity vehicle, ConstEntity wheel);
	// The entities from `wheel` up to `vehicle`, both included: what a kept frame was formed through.
	[[nodiscard]] std::vector<UUID> GetPhysicsVehicleWheelChain(ConstEntity vehicle, ConstEntity wheel);

	// The components in the world's units (degrees become radians, EntityRefs wheel indices; PhysicsVehicle.h):
	//   - the wheel `component` at `frame` (its attachment point the frame's centre plus SuspensionMaxLength up);
	//   - the vehicle's settings over its wheels `wheels` (a plan's), a differential's side naming no wheel of them -1 and a bar
	//     naming one left out;
	//   - its driver input and its cast.
	[[nodiscard]] PhysicsVehicleWheelDescription MakePhysicsVehicleWheel(const VehicleWheelComponent& component, const PhysicsVehicleWheelFrame& frame);
	[[nodiscard]] PhysicsVehicleSettings MakePhysicsVehicleSettings(const WheeledVehicleComponent& component, std::span<const UUID> wheels);
	[[nodiscard]] PhysicsVehicleInput MakePhysicsVehicleInput(const WheeledVehicleComponent& component);
	[[nodiscard]] PhysicsVehicleCast MakePhysicsVehicleCast(const WheeledVehicleComponent& component);
	// The component without its driver input (Throttle to Clutch at their defaults): what the session compares to decide
	// whether a write changed anything but the input.
	[[nodiscard]] WheeledVehicleComponent StripPhysicsVehicleInput(const WheeledVehicleComponent& component);

	// Radians to degrees and back, as the components read them (the steer angle the world reads positive to the left, the
	// components positive to the right).
	inline constexpr float VehicleDegreesToRadians = 0.017453292519943295f;
	inline constexpr float VehicleRadiansToDegrees = 57.29577951308232f;

}
