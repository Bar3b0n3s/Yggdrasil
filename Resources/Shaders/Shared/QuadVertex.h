#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>

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

	// The push constants of the Quad program: the batch's space to clip space (clip = mul(Transform, float4(Position, 1))).
	struct QuadConstants
	{
		Float4x4 Transform;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(QuadVertex) == 36, "QuadVertex must match its 36-byte vertex layout");
	static_assert(offsetof(QuadVertex, Position) == 0 && offsetof(QuadVertex, TexCoord) == 12 && offsetof(QuadVertex, Color) == 20,
		"QuadVertex's members must keep their vertex-attribute offsets");
	static_assert(sizeof(QuadConstants) == 64, "QuadConstants must match its 64-byte push-constant layout");

}
#endif
