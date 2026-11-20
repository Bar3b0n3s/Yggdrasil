#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The planar reflections a view draws (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3;
	// Renderer/PlanarReflections.h): b7 of the forward view layout (Renderer/Private/ForwardPipelines.h), written by the scene
	// renderer with the other per-view constants and read by Common/PlanarReflections.slang for an instance with
	// DrawFlagPlanarReflection. Slot k < Count (InstanceRecord::PlanarReflectionSlot) is the reflection PlanarReflections<k>
	// (t17 + k), rendered by the mirror camera whose unjittered view-projection is ViewProjections[k] (the main view's
	// projection times S * View * R: the camera mirrored in the slot's plane), with MipCounts[k] mip levels and Heights[k]
	// texels at level 0. SsrMaxRoughness: the view's PostProcessSettings::SsrMaxRoughness, the roughness at which a planar
	// sample has faded into the probes' or environment's term, as SSR's does. A view without a drawn reflection has Count 0
	// (and a mirror view always: its records never carry the flag).
	struct PlanarReflectionConstants
	{
		Float4x4 ViewProjections[4];
		Float4 MipCounts;
		Float4 Heights;
		float SsrMaxRoughness = 0.6f;
		uint32_t Count = 0;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

	// The push constants of the PlanarReflection program's mip chain (Passes/PlanarReflection.slang,
	// Renderer/PlanarReflectionPass.h): the level read (SourceSize, level k - 1) and the level written (DestinationSize, level
	// k), in texels.
	struct PlanarReflectionMipConstants
	{
		UInt2 SourceSize;
		UInt2 DestinationSize;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(PlanarReflectionConstants) == 304, "PlanarReflectionConstants must match its 304-byte constant-buffer layout");
	static_assert(offsetof(PlanarReflectionConstants, MipCounts) == 256 && offsetof(PlanarReflectionConstants, Heights) == 272
			&& offsetof(PlanarReflectionConstants, SsrMaxRoughness) == 288 && offsetof(PlanarReflectionConstants, Count) == 292,
		"PlanarReflectionConstants members must sit where the constant-buffer layout puts them");
	static_assert(sizeof(PlanarReflectionMipConstants) == 16 && offsetof(PlanarReflectionMipConstants, DestinationSize) == 8,
		"PlanarReflectionMipConstants must match its 16-byte push-constant layout");

}
#endif
