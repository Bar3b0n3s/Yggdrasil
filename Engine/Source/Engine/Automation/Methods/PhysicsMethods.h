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
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 18): the wire format below. M37 appends
// physics.jointInfo (Docs/Decisions/0054-m37-joints.md decision 10), reviewed as a change to a frozen interface.

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

	}

	// Registers the enum "PhysicsBodyOrigin" and the structs above (the joint result's enums, "JointType" and "JointMotorMode",
	// are the Joints category's, registered with the components).
	void RegisterPhysicsMethodTypes(TypeRegistry& registry);

	// Registers physics.bodyInfo and (M37) physics.jointInfo: read-only, AllowedInBatch, available in the Runtime; not tools
	// (§13.8 lists none for the physics domain), so agents reach them through engine_call.
	void RegisterPhysicsMethods(MethodRegistry& methods);

}
