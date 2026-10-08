#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
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
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 18): the wire format below.

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

	namespace Automation {

		// physics.bodyInfo. Errors: InvalidState "not playing" without a play session (the hint names play.start {mode:
		// "simulate"}); the errors of AutomationMethodContext::ResolveEntity at /entity; NotFound at /entity "entity '<name>'
		// (<id>) has no physics body" when it owns none and none of its colliders belongs to one (no collider, disabled, or
		// a body a diagnostic refused: the hint names project.validate).
		[[nodiscard]] Result<PhysicsBodyInfoResult> PhysicsBodyInfo(AutomationMethodContext& context, const PhysicsBodyInfoParams& params);

	}

	// Registers the enum "PhysicsBodyOrigin" and the structs above.
	void RegisterPhysicsMethodTypes(TypeRegistry& registry);

	// Registers physics.bodyInfo: read-only, AllowedInBatch, available in the Runtime; not a tool (§13.8 lists none for the
	// physics domain), so agents reach it through engine_call.
	void RegisterPhysicsMethods(MethodRegistry& methods);

}
