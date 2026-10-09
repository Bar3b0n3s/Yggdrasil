#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	struct SceneRaycastParams
	{
		glm::vec3 Origin = glm::vec3(0.0f);                 // required, finite
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f); // required, finite and nonzero; normalized
		float MaxDistance = 1000.0f;                        // required, positive finite
		uint32_t LayerMask = AllPhysicsLayers;
		SceneTarget Target = SceneTarget::Edit; // presence-aware default: play while running, edit otherwise
	};

	struct SceneRaycastResult
	{
		bool Hit = false;
		EntitySummary Entity{}; // empty for a miss
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		glm::vec3 Barycentric = glm::vec3(0.0f);
		float Distance = 0.0f;
		uint32_t Submesh = 0;
		uint32_t Triangle = 0;
	};

	namespace Automation {

		// Deterministic CPU visual-mesh raycast through RaycastScene, no GPU or physics stepping. Uses alpha 1 for edit,
		// session.GetViewAlpha for play after refreshing transforms. Empty mask is a miss. InvalidArgument with located
		// field for invalid ray; InvalidState without target scene; Unsupported without AssetManager; other errors from
		// target resolution. Geometry, not texture alpha, determines hits (including Blend/Mask triangles).
		[[nodiscard]] Result<SceneRaycastResult> SceneRaycast(AutomationMethodContext& context, const SceneRaycastParams& params);

	}

	void RegisterRaycastMethodTypes(TypeRegistry& registry);
	// scene.raycast: required origin/direction/maxDistance; read-only, batchable, SupportsDryRun, Runtime subset,
	// not available in launcher, not an MCP tool (engine_call). Register through RegisterSharedMethods.
	void RegisterRaycastMethods(MethodRegistry& methods);

}
