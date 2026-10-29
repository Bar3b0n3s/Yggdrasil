#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// The 2D physics domain the Editor and the Runtime share (M21, C21.1, R-2D-AUTO and R-2D-PHYS;
// Docs/Decisions/0032-m21-contract.md decision 15): queries of the play session's Physics2DSystem (Scene/Physics2DSystem.h),
// which exists only in a play session (Play or Simulate), so every method reads the play scene. Conventions as in
// MethodRegistry.h. physics2d.bodyInfo is W21.2's (C21.2).
//
// Frozen by the M21 contract (C21.1): the wire format below. The types and methods are written here and registered by stream
// P2 (RegisterSharedMethodTypes and RegisterSharedMethods call them) together with their handlers, their C++ round trips,
// their Python test and the regenerated references; until then nothing calls the registration functions.

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

	namespace Automation {

		// physics2d.raycast: Physics2DSystem::Raycast or RaycastAll. Errors: InvalidState "not playing" without a play session
		// (the hint names play.start {mode: "simulate"}); InvalidArgument located at the offending member for a non-finite
		// origin, a zero or non-finite direction or a non-positive or non-finite maxDistance.
		[[nodiscard]] Result<Physics2DRaycastResult> Physics2DRaycast(AutomationMethodContext& context, const Physics2DRaycastParams& params);
		// physics2d.overlap: Physics2DSystem::OverlapCircle or OverlapBox. Errors: as physics2d.raycast's; InvalidArgument at
		// /radius or /halfExtents for the shape's missing or out-of-bounds size.
		[[nodiscard]] Result<Physics2DOverlapResult> Physics2DOverlap(AutomationMethodContext& context, const Physics2DOverlapParams& params);

	}

	// Registers the enum "Physics2DQueryShape" and the structs above.
	void RegisterPhysics2DMethodTypes(TypeRegistry& registry);

	// Registers physics2d.raycast and physics2d.overlap: read-only, AllowedInBatch, available in the Runtime; not tools
	// (agents reach them through engine_call, as physics.bodyInfo).
	void RegisterPhysics2DMethods(MethodRegistry& methods);

}
