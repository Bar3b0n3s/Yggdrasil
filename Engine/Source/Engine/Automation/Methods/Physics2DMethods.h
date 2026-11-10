#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/PhysicsMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The 2D physics domain the Editor and the Runtime share (M21, C21.1, R-2D-AUTO and R-2D-PHYS;
// Docs/Decisions/0032-m21-contract.md decision 15): queries of the play session's Physics2DSystem (Scene/Physics2DSystem.h),
// which exists only in a play session (Play or Simulate), so every method reads the play scene. Conventions as in
// MethodRegistry.h.
//
// Frozen by the M21 contract (C21.1): the wire format below; P2 registered raycast and overlap (RegisterSharedMethodTypes and
// RegisterSharedMethods call the functions below). C21.2 (Docs/Decisions/0032-m21-contract.md decision 27) adds
// physics2d.bodyInfo's wire format and handler, which stream P3 registers in these two functions, with its scene-target
// inventory entry (None: the play session's 2D world).

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry enum "Physics2DQueryShape": the shape of physics2d.overlap.
	enum class Physics2DQueryShape : uint8_t
	{
		Circle,
		Box
	};

	// physics2d.raycast {origin, direction, maxDistance, layerMask?, all?}.
	struct Physics2DRaycastParams
	{
		glm::vec2 Origin = glm::vec2(0.0f);          // required, finite, in the plane (metres)
		glm::vec2 Direction = glm::vec2(1.0f, 0.0f); // required, finite and nonzero; normalized
		float MaxDistance = 1000.0f;                 // required, positive finite
		uint32_t LayerMask = AllPhysicsLayers;       // the project's physics layers (bit i: Physics.Layers[i])
		bool All = false;                            // every hit sorted by distance then collider id, not the closest
	};

	// Registry struct "Physics2DRaycastHitSummary": one hit (Physics2DRaycastHit).
	struct Physics2DRaycastHitSummary
	{
		EntitySummary Entity{}; // the collider entity
		EntitySummary Body{};   // its body's owner
		glm::vec2 Point = glm::vec2(0.0f);
		glm::vec2 Normal = glm::vec2(0.0f, 1.0f);
		float Distance = 0.0f;
	};

	struct Physics2DRaycastResult
	{
		std::vector<Physics2DRaycastHitSummary> Hits{}; // empty for a miss; at most one without `all`
		uint32_t Tick = 0;                              // the session's tick when read
	};

	// physics2d.overlap {shape, center, radius?, halfExtents?, angle?, layerMask?}.
	struct Physics2DOverlapParams
	{
		Physics2DQueryShape Shape = Physics2DQueryShape::Circle; // required
		glm::vec2 Center = glm::vec2(0.0f);                      // required, finite
		float Radius = 0.5f;                                     // Circle: required, in a collider's size bounds
		glm::vec2 HalfExtents = glm::vec2(0.5f);                 // Box: required, in a collider's size bounds
		float Angle = 0.0f;                                      // Box: degrees about +Z
		uint32_t LayerMask = AllPhysicsLayers;
	};

	struct Physics2DOverlapResult
	{
		std::vector<EntitySummary> Entities{}; // the overlapping collider entities, each once, sorted by id
		uint32_t Tick = 0;
	};

	// physics2d.bodyInfo {entity} (C21.2, decision 27): physics.bodyInfo's report in the plane (Physics2DSystem::GetBodyInfo's
	// Physics2DBodyReport): the 2D body an entity of the play scene owns, or the one its colliders were gathered into (a tile of
	// a tilemap's compound reports the tilemap's body). Origin takes the registry enum "PhysicsBodyOrigin", whose entries name
	// the 3D components; for a 2D body they mean the 2D ones: RigidBody is the owner's RigidBody2D, ImplicitStatic the solid
	// 2D colliders without a RigidBody2D above them and ImplicitSensor the trigger 2D colliders of an entity without its own
	// RigidBody2D. Stream P3's field description of "origin" says so.
	struct Physics2DBodyInfoParams
	{
		std::string Entity{}; // an EntityRef of the play scene (§13.4)
	};

	struct Physics2DBodyInfoResult
	{
		EntitySummary Entity{};                                  // the entity asked about
		EntitySummary Body{};                                    // the body's owner
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody; // "PhysicsBodyOrigin" meaning RigidBody2D (above); never Character
		BodyType Type = BodyType::Static;                        // the motion type as simulated (a Static trigger body is Kinematic)
		bool IsSensor = false;
		std::string Layer{};                        // the layer's name (PhysicsSettings.Layers)
		std::vector<EntitySummary> Colliders{};     // in sub-shape order, each collider entity once
		glm::vec2 Position = glm::vec2(0.0f);       // the body origin in the plane, metres
		float Angle = 0.0f;                         // degrees about +Z
		glm::vec2 LinearVelocity = glm::vec2(0.0f); // m/s
		float AngularVelocity = 0.0f;               // rad/s about +Z
		bool Sleeping = false;
		glm::vec2 BoundsMin = glm::vec2(0.0f); // the world rectangle of the whole body
		glm::vec2 BoundsMax = glm::vec2(0.0f);
		std::vector<PhysicsContactSummary> Contacts{}; // registry struct "PhysicsContactSummary", sorted by the partner's id
		uint32_t Tick = 0;                             // the session's tick when read
	};

	namespace Automation {

		// physics2d.raycast: Physics2DSystem::Raycast or RaycastAll. Errors: InvalidState "not playing" without a play session
		// (the hint names play.start {mode: "simulate"}); InvalidArgument located at the offending member for a non-finite
		// origin, a zero or non-finite direction or a non-positive or non-finite maxDistance.
		[[nodiscard]] Result<Physics2DRaycastResult> Physics2DRaycast(AutomationMethodContext& context, const Physics2DRaycastParams& params);
		// physics2d.overlap: Physics2DSystem::OverlapCircle or OverlapBox. Errors: as physics2d.raycast's; InvalidArgument at
		// /radius or /halfExtents for the shape's missing or out-of-bounds size.
		[[nodiscard]] Result<Physics2DOverlapResult> Physics2DOverlap(AutomationMethodContext& context, const Physics2DOverlapParams& params);
		// physics2d.bodyInfo (C21.2): Physics2DSystem::GetBodyInfo. Errors: physics.bodyInfo's: InvalidState "not playing" without a play session
		// (the hint names play.start {mode: "simulate"}); the errors of AutomationMethodContext::ResolveEntity at /entity;
		// NotFound at /entity "entity '<name>' (<id>) has no 2D physics body" when it owns none and none of its colliders
		// belongs to one (the hint names project.validate).
		[[nodiscard]] Result<Physics2DBodyInfoResult> Physics2DBodyInfo(AutomationMethodContext& context, const Physics2DBodyInfoParams& params);

	}

	// Registers the enum "Physics2DQueryShape" and the structs above (PhysicsContactSummary and PhysicsBodyOrigin are
	// RegisterPhysicsMethodTypes', which runs first).
	void RegisterPhysics2DMethodTypes(TypeRegistry& registry);

	// Registers physics2d.raycast, physics2d.overlap and physics2d.bodyInfo (required param "entity"): read-only,
	// AllowedInBatch, available in the Runtime; not tools (agents reach them through engine_call, as physics.bodyInfo).
	void RegisterPhysics2DMethods(MethodRegistry& methods);

}
