#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The water checks of one scene (M47, Docs/Decisions/0065-m47-water.md decision 3): what project.validate reports about a
// scene's Water and Buoyancy components. The three codes are appended to both of ProjectValidator.cpp's code tables after
// M46's ASSET_LOD_INVALID (phase 8's ledger; after M42's RENDER_REFLECTION_INVALID on M47's branch), each with its
// Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill's validation-codes page. None is fixable: the
// engine draws, samples and floats as each code says without reporting it again, but for PreStep's run-time report of a
// Buoyancy it skips (PHYSICS_BUOYANCY_INVALID, once an entity, through the physics diagnostics).

namespace Engine {

	class PhysicsLayerTable;
	class Scene;

	// Warning: a Water whose world matrix is not a yaw, a positive X and Z scale and a translation (pitched, rolled, sheared
	// or mirrored), which draws and samples level at its yaw; or an Ocean after the scene's first, which is neither drawn nor
	// sampled.
	inline constexpr std::string_view WaterInvalidCode = "WATER_INVALID";
	// Warning: a Water whose waves at its WaveScale would fold the surface (the sum of Steepness x wave number x amplitude
	// past MaxWaterSteepnessSum), so every Steepness is scaled down by the factor the finding names.
	inline constexpr std::string_view WaterWavesTooSteepCode = "WATER_WAVES_TOO_STEEP";
	// Warning: a Buoyancy that floats nothing: its owner's body is not Dynamic or has no solid collider (no body is made), or
	// its Water names no entity with a Water.
	inline constexpr std::string_view PhysicsBuoyancyInvalidCode = "PHYSICS_BUOYANCY_INVALID";
	// The water codes, an array of their own (phase 8's ledger; M36's and M37's pattern).
	inline constexpr std::array<std::string_view, 3> WaterDiagnosticCodes = { WaterInvalidCode, WaterWavesTooSteepCode, PhysicsBuoyancyInvalidCode };

	// How far a Water's world matrix may stray from a yaw and a scale, relative to its axes' lengths: the rounding of a
	// quaternion turned into a matrix, never a visible tilt.
	inline constexpr double WaterFrameTolerance = 1.0e-4;

	// One water validation finding.
	struct WaterSceneIssue
	{
		std::string Code{}; // one of WaterDiagnosticCodes
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the Water's or the Buoyancy's entity
		std::string Component{}; // "Water", "Buoyancy" or "RigidBody"
		std::string Field{};     // "Shape", "Waves", "Water", "Type" or empty
		std::string Message{};
		std::string Hint{};

		bool operator==(const WaterSceneIssue&) const = default;
	};

	// Whether `world` is a yaw, a positive X and Z scale (and any Y scale along +Y or -Y) and a translation, within
	// WaterFrameTolerance: its X and Z columns level, perpendicular and right-handed about +Y, its Y column vertical.
	[[nodiscard]] bool IsLevelWaterFrame(const glm::mat4& world);

	// The water checks over `scene` (effectively enabled entities, canonical order; at most one finding of each code and field an
	// entity, so a pitched second Ocean has two WATER_INVALID findings):
	//   - WATER_INVALID: a Water whose world matrix fails IsLevelWaterFrame (no field); an Ocean after the first effectively
	//     enabled one (Water.Shape, naming the first);
	//   - WATER_WAVES_TOO_STEEP: a Water whose ComputeWaterSteepnessSum at its WaveScale exceeds MaxWaterSteepnessSum
	//     (Water.Waves), naming the factor MaxWaterSteepnessSum / sum every Steepness is scaled by;
	//   - PHYSICS_BUOYANCY_INVALID: a Buoyancy whose Water names no live entity holding a Water (Buoyancy.Water); else, with
	//     `layers`, one whose RigidBody is not Dynamic (RigidBody.Type), or whose body the composition (ComposePhysicsBodies)
	//     does not make or makes with no solid collider (no field). Without `layers` (a project whose physics settings do not
	//     load) the body is not checked.
	// Pure function of its inputs; main thread.
	[[nodiscard]] std::vector<WaterSceneIssue> FindWaterIssues(const Scene& scene, const PhysicsLayerTable* layers);

}
