#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry name "IrradianceVolume", version 1, category Probes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md
	// decisions 4, 8 and 9): a box of baked irradiance probes that lights the diffuse surfaces inside it with the light their
	// surroundings bounce (baked GI, plan §6 "M26"; dynamic GI is won't-do, ADR 0028 decision 2). Requires Transform.
	//   - The box: Size metres centred on the entity, in its local axes (its scale applies); ProbeCountX, ProbeCountY and
	//     ProbeCountZ probes along its axes, the grid's corners at the box's corners (Asset/IrradianceVolumeData.h).
	//   - Data is the baked volume, which lighting.bake (EditorCore/Lighting/LightingBake.h) writes as
	//     `Assets/Lighting/<Scene>/<Entity>.irradiance` and assigns here; null (not baked: PROBE_NOT_BAKED) lights nothing.
	//   - Shading: the forward passes replace the Environment's SH9 diffuse with the volume's, trilinear between the eight
	//     probes around the shaded point, times Intensity, fading back to the Environment's over BlendDistance metres inside
	//     the box's faces (Common/Probes.slang, M23's light loop's indirect term, after the direct lights). Where boxes overlap,
	//     the smaller volume wins.
	struct IrradianceVolumeComponent
	{
		TypedAssetHandle<AssetType::IrradianceVolume> Data{};
		glm::vec3 Size{ 10.0f, 4.0f, 10.0f }; // metres, each in [0.01, MaxIrradianceVolumeSize]
		uint32_t ProbeCountX = 4;             // [MinIrradianceProbesPerAxis, MaxIrradianceProbesPerAxis]
		uint32_t ProbeCountY = 3;
		uint32_t ProbeCountZ = 4;
		float Intensity = 1.0f;     // [0, 65504]
		float BlendDistance = 1.0f; // metres, [0, 1000]
	};

}
