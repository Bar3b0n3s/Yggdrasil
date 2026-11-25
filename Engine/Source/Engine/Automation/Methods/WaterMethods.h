#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The water domain (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §13.5, §9.12): water.sample, the surface
// of a scene's waters under world points, shared by the Editor and the Runtime (§13.5 "Runtime subset"), so an agent reads
// the height, normal, velocity and depth a script or the physics would read without a script. Conventions as in
// MethodRegistry.h.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The most points one water.sample call takes.
	inline constexpr uint32_t MaxWaterSamplePoints = 4096;

	// water.sample {points, entity?, time?, target?}: per world point, the surface of the water that holds it
	// (Scene/WaterQueries.h's rule: the Rectangle whose volume holds it with the highest surface there, else the scene's Ocean),
	// or with `entity` that water's own surface (masking aside; none outside a Rectangle's footprint), at `time` seconds of
	// world time: the edit scene at `time` or 0, the play scene at the world's time now (the world tick times the fixed delta,
	// what the next tick's physics samples) unless `time` is given.
	struct WaterSampleParams
	{
		std::vector<glm::vec3> Points{}; // required: 1 to MaxWaterSamplePoints world points
		std::string Entity{};            // an EntityRef of an entity with a Water; empty: the rule
		float Time = 0.0f;               // seconds of world time, at least 0; absent: see above
		SceneTarget Target = SceneTarget::Edit;
	};

	// Registry struct "WaterSamplePoint": the surface under one point. Water is the water's entity as {id, name, path}, null
	// where none holds the point (then Normal is +Y, Underwater false and the others 0).
	struct WaterSamplePoint
	{
		VariantValue Water{};
		float Height = 0.0f;                            // the surface's world height under the point
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f); // the surface's world unit normal there
		glm::vec3 Velocity = glm::vec3(0.0f);           // m/s: the waves' motion there plus the water's current
		float Depth = 0.0f;                             // Height minus the point's y: positive under water
		bool Underwater = false;                        // the point lies under the surface (and within a Rectangle's volume)
	};

	struct WaterSampleResult
	{
		SceneSummary Scene{};
		float Time = 0.0f;                      // the world time sampled at
		std::vector<WaterSamplePoint> Points{}; // in the order of the params' points
	};

	namespace Automation {

		// water.sample. Errors: those of the target (ResolveTargetScene) and of `entity` (ResolveEntity at /entity);
		// InvalidArgument at /points for no point or more than MaxWaterSamplePoints, at /points/<i> for a point beyond the world
		// (a coordinate past MaxPhysicsCoordinate), at /entity for an entity without a Water.
		[[nodiscard]] Result<WaterSampleResult> WaterSample(AutomationMethodContext& context, const WaterSampleParams& params);

	}

	// Registers WaterSamplePoint and the params and result structs above (after RegisterSceneMethodTypes, whose SceneTarget and
	// SceneSummary they use).
	void RegisterWaterMethodTypes(TypeRegistry& registry);

	void RegisterWaterMethods(MethodRegistry& methods);

}
