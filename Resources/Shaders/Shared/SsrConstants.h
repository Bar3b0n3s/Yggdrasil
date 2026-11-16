#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The SSR pass's push constants (M26, C26.1, R-SSR-IN, R-SSR; Docs/Decisions/0037-m26-contract.md decisions 14 and 18;
	// Renderer/SsrPass.h, Passes/Ssr.slang), shared by its trace and its composite: the scene resolution SceneColor, SceneDepth
	// and the prepass targets have (ImageSize, and its inverse), the depth pyramid's mip count (PyramidLevels, ViewDepth's: 1 to
	// DepthPyramidPass::MaxMipCount) and PostProcessSettings' SSR settings made usable (SsrPass.h's MakeSsrConstants): the
	// distance a reflected ray travels (MaxDistance, metres), the thickness a surface is assumed behind its depth (Thickness,
	// metres), the perceptual roughness above which only the probes reflect (MaxRoughness, [0, 1]) and the share of the probes'
	// reflection the traced one replaces (Intensity, [0, 1]). M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 11): whether the
	// trace runs at half resolution (HalfResolution, 1 with RenderQualitySettings::SsrHalfResolution), in the first padding
	// word.
	struct SsrConstants
	{
		UInt2 ImageSize;
		Float2 InverseImageSize;
		float MaxDistance = 50.0f;
		float Thickness = 0.2f;
		float MaxRoughness = 0.6f;
		float Intensity = 1.0f;
		uint32_t PyramidLevels = 1;
		uint32_t HalfResolution = 0;
		uint32_t Padding1 = 0;
		uint32_t Padding2 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SsrConstants) == 48, "SsrConstants must match its 48-byte push-constant layout");
	static_assert(offsetof(SsrConstants, ImageSize) == 0 && offsetof(SsrConstants, InverseImageSize) == 8
		&& offsetof(SsrConstants, MaxDistance) == 16 && offsetof(SsrConstants, Thickness) == 20
		&& offsetof(SsrConstants, MaxRoughness) == 24 && offsetof(SsrConstants, Intensity) == 28
		&& offsetof(SsrConstants, PyramidLevels) == 32 && offsetof(SsrConstants, HalfResolution) == 36
		&& offsetof(SsrConstants, Padding1) == 40 && offsetof(SsrConstants, Padding2) == 44);

}
#endif
