#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// One vertex of the QuadBatcher (Renderer/QuadBatcher.h, Passes/Quad.slang; M18, C18.1, R-2D-BATCH,
	// Docs/Decisions/0029-m18-contract.md decision 10): four per quad, drawn as two triangles through a static index buffer
	// (0, 1, 2, 0, 2, 3 per quad, corners in the order top-left, top-right, bottom-right, bottom-left). The vertex buffer's
	// layout, which the C++ input layout and Quad.slang's vertex input both follow member by member (locations 0, 1, 2):
	//   Position: the corner in the batch's space (scene pixels with MakeQuadPixelTransform, or world space), transformed to
	//             clip space by QuadConstants::Transform;
	//   TexCoord: normalized, top-left origin;
	//   Color:    linear RGB and straight alpha, multiplying the sampled texel.
	struct QuadVertex
	{
		Float3 Position;
		Float2 TexCoord;
		Float4 Color;
	};

	// QuadConstants::FogMode values (M32, Docs/Decisions/0047-m32-fog-ssr.md decision 5): how the Hdr, HdrLit and HdrAdditive
	// entries meet the view's fog (Common/Fog.slang) when its FogConstants::Enabled is set: not at all, blended toward the fog's
	// colour, or faded by its amount (additive quads, whose fog colour would otherwise be added twice).
#if defined(ENGINE_SHADER)
	static const uint QuadFogModeNone = 0;
	static const uint QuadFogModeBlend = 1;
	static const uint QuadFogModeFade = 2;
#else
inline constexpr uint32_t QuadFogModeNone = 0;
inline constexpr uint32_t QuadFogModeBlend = 1;
inline constexpr uint32_t QuadFogModeFade = 2;
#endif

	// The push constants of the Quad program: the batch's space to clip space (clip = mul(Transform, float4(Position, 1))) and,
	// for M21's Normals entry (C21.2, Docs/Decisions/0032-m21-contract.md decision 22), whether the batch's quads sample their
	// normal map: 1 then, 0 for the flat normal; and, since M23's milestone review (Docs/Decisions/0034-m23-contract.md,
	// "Milestone review" item 1), whether that normal map stores x and y only (1: a BC5 texture, or the decode fallback's
	// RGBA8 copy of one, whose z the entry reconstructs as Scene.slang does), 0 for an RGB normal map. The other entries
	// ignore both, and the batcher leaves them 0. M32: FogMode, a QuadFogMode value for the Hdr, HdrLit and HdrAdditive
	// entries, whose batch space is then world space (the Sprites stage's); the other entries ignore it, and the batcher
	// leaves it QuadFogModeNone for a record without the view's fog (QuadBatchInputs::Fog).
	struct QuadConstants
	{
		Float4x4 Transform;
		uint32_t HasNormalMap = 0;
		uint32_t NormalMapTwoChannel = 0;
		uint32_t FogMode = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(QuadVertex) == 36, "QuadVertex must match its 36-byte vertex layout");
	static_assert(offsetof(QuadVertex, Position) == 0 && offsetof(QuadVertex, TexCoord) == 12 && offsetof(QuadVertex, Color) == 20,
		"QuadVertex's members must keep their vertex-attribute offsets");
	static_assert(sizeof(QuadConstants) == 80, "QuadConstants must match its 80-byte push-constant layout");
	static_assert(offsetof(QuadConstants, HasNormalMap) == 64 && offsetof(QuadConstants, NormalMapTwoChannel) == 68
			&& offsetof(QuadConstants, FogMode) == 72,
		"QuadConstants members must sit where the push-constant layout puts them");

}
#endif
