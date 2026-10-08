#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The environment's lighting constants: b2 of descriptor set 0 (§8.4 "b2 EnvironmentConstants (SH9, intensity,
	// rotation)"), written once per view by the scene renderer from the snapshot's RenderEnvironment and its GpuEnvironment,
	// read by the forward passes (diffuse and specular IBL, or the constant ambient) and the skybox pass. Frozen by the M8
	// contract (Docs/Decisions/0013-m8-decisions.md decision 9): the scene renderer writes it, the forward passes and the
	// skybox read it.
	//
	// Rotation (§8.6: "Rotation rotates the lookup vector"): the environment appears turned by Rotation degrees about +Y
	// (counter-clockwise seen from above), so a world direction d is looked up at
	// (d.x * RotationCos - d.z * RotationSin, d.y, d.x * RotationSin + d.z * RotationCos)
	// (RotateEnvironmentDirection, Common/Environment.slang).
	struct EnvironmentConstants
	{
		// xyz: EnvironmentData::IrradianceSH9 (the coefficients of irradiance / pi, EnvironmentData.h) times Intensity; w 0.
		// Zero without an environment map. EvaluateIrradianceSH9 (Common/Environment.slang) evaluates them; negative results
		// are clamped to 0 in the shader (§8.6 step 4).
		Float4 IrradianceSH9[9];
		Float3 AmbientColor;    // without an environment map: FallbackColor * Intensity (§5.3); zero with one
		float Intensity = 1.0f; // scales the specular IBL and the skybox (the SH9 above already include it)
		float RotationSin = 0.0f;
		float RotationCos = 1.0f;
		float SpecularMaxLod = 0.0f; // EnvSpecular's mip count - 1 (the LOD of perceptual roughness 1); 0 without a map
		float SkyboxLod = 0.0f;      // SkyboxBlur * (the skybox's mip count - 1)
		uint32_t HasEnvironment = 0; // 1 when an environment map lights the view (EnvSpecular and EnvSkybox are bound)
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
		uint32_t Padding2 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(EnvironmentConstants) == 192, "EnvironmentConstants must match its 192-byte constant-buffer layout");
	static_assert(offsetof(EnvironmentConstants, AmbientColor) == 144 && offsetof(EnvironmentConstants, RotationSin) == 160
			&& offsetof(EnvironmentConstants, HasEnvironment) == 176,
		"EnvironmentConstants members must sit where the constant-buffer layout puts them");

}
#endif
