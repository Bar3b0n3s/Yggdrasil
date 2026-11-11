#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): the capture cube's face sizes a reflection
	// probe bakes at.
	inline constexpr uint32_t MinReflectionProbeResolution = 32;
	inline constexpr uint32_t MaxReflectionProbeResolution = 512;

	// Registry name "ReflectionProbe", version 1, category Probes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md
	// decisions 4, 8 and 9): a baked reflection of the probe's surroundings that glossy surfaces inside its box reflect.
	// Requires Transform.
	//   - The bake: lighting.bake renders the scene around the entity's position into a cube of Resolution texels a face,
	//     writes it as an equirectangular Radiance HDR image, `Assets/Lighting/<Scene>/<Entity>.hdr`, and imports it as an
	//     Environment asset (EnvironmentImporter's prefiltered specular cube; a reflection probe has no asset type of its
	//     own, decision 3), which Data then references. An Environment imported any other way works too; null (not baked:
	//     PROBE_NOT_BAKED) reflects nothing.
	//   - The box: Size metres centred on the entity, in its local axes (its scale applies), the region whose surfaces
	//     reflect the probe instead of the scene's Environment, fading back to it over BlendDistance metres inside the faces;
	//     with BoxProjection the reflected direction is corrected for the box (parallax-corrected cube mapping), so a room's
	//     reflection lines up with its walls. Where boxes overlap, the smaller probe wins.
	//   - Shading: the forward passes' split-sum specular samples the probe's prefiltered cube instead of the Environment's,
	//     times Intensity (Common/Probes.slang); screen-space reflections, where enabled, replace it where their rays hit and
	//     fall back to it where they miss (decision 18).
	struct ReflectionProbeComponent
	{
		TypedAssetHandle<AssetType::Environment> Data{};
		glm::vec3 Size{ 10.0f, 5.0f, 10.0f }; // metres, each in [0.01, 10000]
		bool BoxProjection = true;
		uint32_t Resolution = 128;  // the capture cube's face, a power of two in [MinReflectionProbeResolution, MaxReflectionProbeResolution]
		float Intensity = 1.0f;     // [0, 65504]
		float BlendDistance = 1.0f; // metres, [0, 1000]
	};

}
