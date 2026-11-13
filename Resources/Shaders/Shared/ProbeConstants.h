#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The most irradiance volumes and reflection probes one view shades with (M26, C26.1, R-GI-PROBE;
	// Docs/Decisions/0037-m26-contract.md decision 16): the first of the snapshot's, in its order, that the view does not cull.
#if defined(ENGINE_SHADER)
	static const uint MaxViewIrradianceVolumes = 8;
	static const uint MaxViewReflectionProbes = 8;
	// The SH9 coefficients of one probe, IrradianceProbes' entries per probe.
	static const uint IrradianceProbeCoefficientCount = 9;
#else
inline constexpr uint32_t MaxViewIrradianceVolumes = 8;
inline constexpr uint32_t MaxViewReflectionProbes = 8;
inline constexpr uint32_t IrradianceProbeCoefficientCount = 9;
#endif

	// One irradiance volume of ProbeConstants, 112 bytes (Common/Probes.slang). Its box is HalfSize.xyz about the origin of its
	// local space, which WorldToLocal (the inverse of the volume's rendered world matrix) maps world points into; AxisScale.xyz
	// are the world matrix's column lengths, the metres one local unit spans along each axis, which turn the distance to a face
	// into metres. HalfSize.w is BlendDistance (metres) and AxisScale.w Intensity. Probes.xyz are its grid's probe counts
	// (IrradianceVolumeData's CountX, CountY and CountZ, each at least 2) and Probes.w the index in IrradianceProbes (t13 of set
	// 0) of its first probe's first coefficient: probe (x, y, z)'s coefficient k is IrradianceProbes[Probes.w + 9 * (x + CountX
	// * (y + CountY * z)) + k].
	struct ProbeVolumeConstants
	{
		Float4x4 WorldToLocal;
		Float4 HalfSize;
		Float4 AxisScale;
		UInt4 Probes;
	};

	// One reflection probe of ProbeConstants, 192 bytes: its box as a volume's (WorldToLocal, HalfSize with BlendDistance in w,
	// AxisScale with Intensity in w); LocalToWorld, its rendered world matrix, which turns a box-projected hit (a local point,
	// seen from the probe's origin, where its cube was captured) back into a world direction; Lookup.x the lod of perceptual
	// roughness 1 in its cube (the cube's mip count - 1); Indices.x the cube's index in ReflectionProbeCubes (t14 of set 0) and
	// Indices.y 1 with BoxProjection, else 0.
	struct ProbeReflectionConstants
	{
		Float4x4 WorldToLocal;
		Float4x4 LocalToWorld;
		Float4 HalfSize;
		Float4 AxisScale;
		Float4 Lookup;
		UInt4 Indices;
	};

	// The view's probes: b4 of descriptor set 0 (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16),
	// written once per view by the scene renderer from the snapshot's IrradianceVolumes and ReflectionProbes and read by the
	// forward passes' indirect term (Common/Probes.slang). Counts.x volumes and Counts.y reflection probes are set, each list
	// ordered from the largest box (world volume) to the smallest, ties in the snapshot's order, so blending them in order
	// leaves the smallest box on top; the other entries are zeroed. Counts are 0 without a camera, outside the Lit view, in
	// the wireframe view and with RenderQualitySettings::Probes false (a bake's captures). Counts.zw are padding.
	struct ProbeConstants
	{
		ProbeVolumeConstants Volumes[MaxViewIrradianceVolumes];
		ProbeReflectionConstants Reflections[MaxViewReflectionProbes];
		UInt4 Counts;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ProbeVolumeConstants) == 112, "ProbeVolumeConstants must match its constant-buffer layout");
	static_assert(offsetof(ProbeVolumeConstants, HalfSize) == 64 && offsetof(ProbeVolumeConstants, Probes) == 96);
	static_assert(sizeof(ProbeReflectionConstants) == 192, "ProbeReflectionConstants must match its constant-buffer layout");
	static_assert(offsetof(ProbeReflectionConstants, HalfSize) == 128 && offsetof(ProbeReflectionConstants, Indices) == 176);
	static_assert(sizeof(ProbeConstants) == 2448, "ProbeConstants must match its 2448-byte constant-buffer layout");
	static_assert(offsetof(ProbeConstants, Reflections) == 896 && offsetof(ProbeConstants, Counts) == 2432);

}
#endif
