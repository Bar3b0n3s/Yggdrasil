#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Physics/WaterWaves.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

	// Registry enum "WaterShape" (M47, Docs/Decisions/0065-m47-water.md decision 3): what a Water covers.
	enum class WaterShape : uint8_t
	{
		Ocean,    // unbounded in X and Z and below its surface; one a scene (the first effectively enabled one)
		Rectangle // a box: Size along local X and Z centred on the entity, from Depth below its base to Headroom above it
	};

	// The bounds of the Water component's fields (its registry ranges, Scene/Registration/WaterRegistration.cpp).
	inline constexpr float MinWaterSize = 0.1f;
	inline constexpr float MaxWaterExtent = 100000.0f; // Size, Depth and Headroom, metres
	inline constexpr float MinWaterDensity = 1.0f;
	inline constexpr float MaxWaterDensity = 20000.0f;

	// Registry name "Water", version 1, category Water (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §5.3,
	// §9.12): a body of water. Requires Transform; several a scene; script-visible through its generated proxy and its
	// methods (Water:SampleHeight, SampleNormal, SampleVelocity, IsUnderwater).
	//   - The frame: the base height is the entity's world Y. Only its yaw orients it (Physics/WaterWaves.h: MakeWaterFrame);
	//     a Rectangle's footprint is Size times its world X and Z scale, centred on the entity. A pitched, rolled or sheared
	//     Water draws and samples level, which WATER_INVALID reports.
	//   - The shape: an Ocean is unbounded in X and Z and below its surface; one a scene, the first effectively enabled one in
	//     canonical order (a later one is neither drawn nor sampled, WATER_INVALID). A Rectangle's volume is its footprint from
	//     Depth below its base to Headroom above it. The rule for which water holds a point (Scene/WaterQueries.h): the
	//     effectively enabled Rectangle whose volume holds it with the highest surface there, ties by canonical order, else
	//     the Ocean; inside any Rectangle's volume the Ocean is neither sampled nor drawn, so a cave lake below the sea's level
	//     whose Headroom reaches the cave's ceiling is dry above its own surface.
	//   - The waves (Physics/WaterWaves.h): at most MaxWaterWaves Gerstner waves (refused past it on every write path), every
	//     amplitude times WaveScale (the storm), each Q scaled so the surface never folds (WATER_WAVES_TOO_STEEP reports
	//     that), at the world's time: Tick x FixedDelta, held by the world pause, and 0 in edit scenes. Current is a
	//     horizontal flow in local X and Z: it moves floating bodies (the surface velocity) and no wave. Density is the
	//     fluid's, which Buoyancy floats bodies against.
	//   - The look (presentation only; Architecture §8.3 "Water"): ShallowColor over shallow water toward DeepColor over
	//     Clarity metres of water column; Roughness; RefractionStrength; foam on crests and where the column is shallower than
	//     ShoreFoamDepth, by FoamAmount; two detail normal layers of NormalMap (null: the built-in WaterNormal) tiled every
	//     DetailScale metres and drifting at DetailDrift m/s, by DetailNormalStrength; FoamTexture (null: the built-in
	//     WaterFoam); under the surface, a fog toward UnderwaterColor over UnderwaterVisibility metres. Colours are linear.
	// Component data: serialized and in the state hash of a scene that holds it; nothing derived from it is hashed.
	struct WaterComponent
	{
		WaterShape Shape = WaterShape::Ocean;
		glm::vec2 Size = glm::vec2(20.0f);   // Rectangle: local X and Z extent, each [MinWaterSize, MaxWaterExtent]
		float Depth = 5.0f;                  // Rectangle: metres below the base, [0, MaxWaterExtent]
		float Headroom = 10.0f;              // Rectangle: metres above the base, [0, MaxWaterExtent]
		std::vector<WaterWave> Waves{};      // at most MaxWaterWaves; empty is a calm surface
		float WaveScale = 1.0f;              // [0, MaxWaterWaveScale], multiplies every amplitude
		glm::vec2 Current = glm::vec2(0.0f); // local X and Z, m/s, each [-MaxWaterCurrent, MaxWaterCurrent]
		float Density = 1000.0f;             // kg/m^3, [MinWaterDensity, MaxWaterDensity]
		glm::vec3 ShallowColor = glm::vec3(0.10f, 0.55f, 0.50f);
		glm::vec3 DeepColor = glm::vec3(0.01f, 0.07f, 0.12f);
		float Clarity = 4.0f;                          // metres, [0.01, 1000]
		float Roughness = 0.05f;                       // [0.02, 1]
		float RefractionStrength = 0.3f;               // [0, 1]
		float FoamAmount = 0.5f;                       // [0, 1]
		float ShoreFoamDepth = 0.5f;                   // metres, [0, 10]
		float DetailNormalStrength = 0.5f;             // [0, 1]
		float DetailScale = 4.0f;                      // metres, [0.1, 1000]
		glm::vec2 DetailDrift = glm::vec2(2.0f, 1.0f); // m/s in local X and Z, each [-100, 100]
		glm::vec3 UnderwaterColor = glm::vec3(0.02f, 0.10f, 0.12f);
		float UnderwaterVisibility = 15.0f;                 // metres, [0.1, 10000]
		TypedAssetHandle<AssetType::Texture> NormalMap{};   // null: the built-in WaterNormal
		TypedAssetHandle<AssetType::Texture> FoamTexture{}; // null: the built-in WaterFoam
	};

}
