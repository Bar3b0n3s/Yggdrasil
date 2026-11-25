#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsJoint.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The physics domain the Editor and the Runtime share (Architecture §13.5 "physics.bodyInfo {entity} (velocity, sleeping,
// contacts, layer)", §13.7 "physics reasoning", Runtime subset). It reads the play session's PhysicsSystem
// (PhysicsSystem::GetBodyInfo). Conventions as in MethodRegistry.h.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 18): the wire format below. M36 adds the
// character's ground collider, its squeeze and the body that squeezes it after groundNormal
// (Docs/Decisions/0053-m36-contact-surfaces.md decisions 7 and 14), M37 appends physics.jointInfo
// (Docs/Decisions/0054-m37-joints.md decision 10), and M44 adds physics.bodyInfo's centerOfMass before tick and appends
// physics.vehicleInfo (Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3), each reviewed as a change to the frozen wire
// format.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// physics.bodyInfo {entity}: the body an entity of the play scene owns, or the body its colliders were gathered into
	// (a track piece under a level root with a Static RigidBody reports the level root's compound): pose, velocities,
	// sleeping state, layer, colliders and active contacts (PhysicsSystem::GetBodyInfo's PhysicsBodyReport). Bodies exist
	// only in a play session (Play or Simulate), so the method always reads the play scene.
	struct PhysicsBodyInfoParams
	{
		std::string Entity{}; // an EntityRef of the play scene (§13.4)
	};

	// Registry struct "PhysicsContactSummary": one active contact pair of the body (§9.4 pairs, sorted by the partner's id).
	struct PhysicsContactSummary
	{
		EntitySummary Other{};         // the partner's body owner
		EntitySummary Collider{};      // this body's collider entity of the first sub-shape pair that touched
		EntitySummary OtherCollider{}; // the partner's
		bool IsTrigger = false;        // a sensor pair (OnTrigger*), not a collision (OnCollision*)
		uint32_t SinceTick = 0;        // the tick whose PostStep reported the enter
	};

	struct PhysicsBodyInfoResult
	{
		EntitySummary Entity{};                                  // the entity asked about
		EntitySummary Body{};                                    // the body's owner (Entity itself unless its colliders belong to an ancestor's body)
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody; // registry enum "PhysicsBodyOrigin"
		// Registry enum "BodyType" (§5.3): the body's motion type (a Static RigidBody whose colliders are triggers is
		// Kinematic, §9.2).
		BodyType Type = BodyType::Static;
		bool IsSensor = false;
		std::string Layer{};                    // the layer's name (PhysicsSettings.Layers)
		std::vector<EntitySummary> Colliders{}; // in sub-shape order; empty for a character
		glm::vec3 Position = glm::vec3(0.0f);   // the body origin's world position (a character's base)
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 LinearVelocity = glm::vec3(0.0f);  // m/s
		glm::vec3 AngularVelocity = glm::vec3(0.0f); // rad/s
		bool Sleeping = false;
		glm::vec3 BoundsMin = glm::vec3(0.0f); // the world AABB of the whole body
		glm::vec3 BoundsMax = glm::vec3(0.0f);
		std::vector<PhysicsContactSummary> Contacts{};
		// Characters (Origin Character) only; false and (0, 1, 0) otherwise.
		bool Grounded = false;
		glm::vec3 GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 7): a grounded character's ground collider
		// (PhysicsCharacterState::GroundCollider); empty otherwise. M36 W36.2 (decision 14): whether the character is squeezed,
		// and the owner of the body that squeezes it (empty otherwise).
		EntitySummary GroundCollider{};
		bool Squeezed = false;
		EntitySummary SqueezedBy{};
		// M44 (Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3): the world position of the body's centre of mass, in
		// metres (PhysicsBodyReport::CenterOfMass).
		glm::vec3 CenterOfMass = glm::vec3(0.0f);
		// M47 (Docs/Decisions/0065-m47-water.md decision 3; phase 8's ledger: after M44's centerOfMass): a body whose owner holds a
		// Buoyancy, {water, submergedVolume, totalVolume} (PhysicsBodyReport::Buoyancy, water as {id, name, path} or null);
		// null for any other body.
		VariantValue Buoyancy{};
		uint32_t Tick = 0; // the session's tick when read
	};

	// physics.jointInfo {entity} (M37, Docs/Decisions/0054-m37-joints.md decision 10): the HingeJoint, SliderJoint,
	// DistanceJoint or FixedJoint of an entity of the play scene as the session holds it (PhysicsSystem::GetJointInfo):
	// which bodies it joins, whether it is in force or broken, its reading after the last physics step and its motor. Joints
	// are in force only in a play session (Play or Simulate), so the method always reads the play scene.
	struct PhysicsJointInfoParams
	{
		std::string Entity{}; // an EntityRef of the play scene (§13.4)
	};

	struct PhysicsJointInfoResult
	{
		EntitySummary Entity{};                          // the joint's entity
		PhysicsJointType Type = PhysicsJointType::Hinge; // registry enum "JointType"
		// The owner of the joint's body as an EntitySummary's {id, name, path}, null when it has none; the owner of the connected
		// body, null for the world (or none). VariantValues, as the registry has no optional struct field.
		VariantValue Body{};
		VariantValue ConnectedBody{};
		bool Created = false; // in force: false for a refused, broken, disabled or not yet created joint
		bool Broken = false;  // the component's Broken
		// The type's reading after the last physics step while Created, 0 for the other types and otherwise.
		float Angle = 0.0f;                         // a hinge's, degrees from its authored pose
		float Position = 0.0f;                      // a slider's, metres along its axis
		float Distance = 0.0f;                      // a distance joint's, metres between its anchors
		float Force = 0.0f;                         // N, the last collision step's, as the break test measures it
		float Torque = 0.0f;                        // N m
		JointMotorMode Motor = JointMotorMode::Off; // registry enum "JointMotorMode"; Off for distance and fixed joints
		uint32_t Tick = 0;                          // the session's tick when read
	};

	// physics.vehicleInfo {entity} (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3): the WheeledVehicle an entity
	// of the play scene holds, or that it is a wheel of (a VehicleWheel's, the nearest WheeledVehicle above it), as the session
	// holds it (PhysicsSystem::GetVehicleInfo): its body, whether it is in force, its speed and drivetrain, and each wheel's
	// contact, suspension, spin, steer and slip after the last physics step. Vehicles are in force only in a play session (Play
	// or Simulate), so the method always reads the play scene.
	struct PhysicsVehicleInfoParams
	{
		std::string Entity{}; // an EntityRef of the play scene (§13.4)
	};

	// Registry struct "PhysicsVehicleWheelSummary": one wheel of a vehicle, after the last physics step (its readings 0 while
	// the vehicle is not in force).
	struct PhysicsVehicleWheelSummary
	{
		EntitySummary Entity{}; // the wheel's entity
		bool Touching = false;
		glm::vec3 Point = glm::vec3(0.0f);  // world, m; zero in the air
		glm::vec3 Normal = glm::vec3(0.0f); // the ground's; zero in the air
		// The collider entity the wheel stands on as {id, name, path}, null in the air or when that collider or its body's owner
		// is disabled or destroyed. A VariantValue, as the registry has no optional struct field.
		VariantValue Collider{};
		float SuspensionLength = 0.0f; // m, from the attachment point to the wheel's centre
		float SuspensionForce = 0.0f;  // N
		float Rotation = 0.0f;         // degrees, [0, 360)
		float SteerAngle = 0.0f;       // degrees, positive to the right
		float AngularVelocity = 0.0f;  // degrees per second, positive rolling forward
		float LongitudinalSlip = 0.0f; // the slip ratio
		float LateralSlip = 0.0f;      // the slip angle, degrees
	};

	struct PhysicsVehicleInfoResult
	{
		EntitySummary Entity{}; // the vehicle's entity (the WheeledVehicle's), whichever of its entities was asked about
		// The owner of the vehicle's body (its own entity) as an EntitySummary's {id, name, path}, null when its RigidBody makes no
		// Dynamic body. A VariantValue, as the registry has no optional struct field.
		VariantValue Body{};
		bool Created = false; // in force: false for a refused, disabled or not yet created vehicle
		// While Created, the reading after the last physics step; 0 otherwise.
		float Speed = 0.0f;     // m/s along its forward (local -Z), signed
		float EngineRpm = 0.0f; // revolutions per minute
		int32_t Gear = 0;       // negative reverse, 0 neutral
		float Clutch = 0.0f;    // the clutch's friction, [0, 1]
		bool Shifting = false;  // a gear change in progress
		// Its wheels in the world's wheel order (canonical order below the vehicle).
		std::vector<PhysicsVehicleWheelSummary> Wheels{};
		uint32_t Tick = 0; // the session's tick when read
	};

	namespace Automation {

		// physics.bodyInfo. Errors: InvalidState "not playing" without a play session (the hint names play.start {mode:
		// "simulate"}); the errors of AutomationMethodContext::ResolveEntity at /entity; NotFound at /entity "entity '<name>'
		// (<id>) has no physics body" when it owns none and none of its colliders belongs to one (no collider, disabled, or
		// a body a diagnostic refused: the hint names project.validate).
		[[nodiscard]] Result<PhysicsBodyInfoResult> PhysicsBodyInfo(AutomationMethodContext& context, const PhysicsBodyInfoParams& params);

		// physics.jointInfo. Errors: InvalidState "not playing" without a play session (the hint names play.start); the errors
		// of AutomationMethodContext::ResolveEntity at /entity; NotFound at /entity "entity '<name>' (<id>) has no joint" when it
		// holds no HingeJoint, SliderJoint, DistanceJoint or FixedJoint.
		[[nodiscard]] Result<PhysicsJointInfoResult> PhysicsJointInfo(AutomationMethodContext& context, const PhysicsJointInfoParams& params);

		// physics.vehicleInfo (M44). Errors: InvalidState "not playing" without a play session (the hint names play.start); the
		// errors of AutomationMethodContext::ResolveEntity at /entity; NotFound at /entity "entity '<name>' (<id>) is neither a
		// vehicle nor a wheel" when it holds no WheeledVehicle and no VehicleWheel below one.
		[[nodiscard]] Result<PhysicsVehicleInfoResult> PhysicsVehicleInfo(AutomationMethodContext& context, const PhysicsVehicleInfoParams& params);

	}

	// Registers the enum "PhysicsBodyOrigin" and the structs above (the joint result's enums, "JointType" and "JointMotorMode",
	// are the Joints category's, registered with the components).
	void RegisterPhysicsMethodTypes(TypeRegistry& registry);

	// Registers physics.bodyInfo, (M37) physics.jointInfo and (M44) physics.vehicleInfo: read-only, AllowedInBatch, available in
	// the Runtime; not tools (§13.8 lists none for the physics domain), so agents reach them through engine_call.
	void RegisterPhysicsMethods(MethodRegistry& methods);

}
