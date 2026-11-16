#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The SceneVelocity rule of the Particles program (Docs/Decisions/0048-m33-particle-look.md decision 6): a particle's
	// fragment writes its motion where its alpha (the texel's times the colour's times the soft fade) is at least this, and
	// keeps the motion already there elsewhere.
#if defined(ENGINE_SHADER)
	static const float ParticleVelocityAlpha = 0.5;
#else
inline constexpr float ParticleVelocityAlpha = 0.5f;
#endif

	// One vertex of the Particles program (Renderer/ParticlePass.h, Passes/Particles.slang; M33,
	// Docs/Decisions/0048-m33-particle-look.md decision 4): four per particle quad, drawn as two triangles through a static
	// index buffer (0, 1, 2, 0, 2, 3 per quad, corners in the order top-left, top-right, bottom-right, bottom-left). The C++
	// input layout and Particles.slang's vertex input both follow it member by member (locations 0 to 4):
	//   Position:         the corner in world space, transformed by ViewConstants::ViewProjection;
	//   PreviousPosition: the same corner where the particle was drawn in the view's previous render (Position when it has
	//                     no previous position), transformed by ViewConstants::PreviousViewProjection for its motion;
	//   Color:            linear RGB (times the look's Intensity, so above 1 for HDR) and straight alpha, multiplying the
	//                     texel;
	//   TexCoords:        xy the frame's texture coordinate at the corner (normalized, top-left origin), zw the next
	//                     frame's, unused while Params.x is 0;
	//   Params:           x the blend toward the zw texel (0: none), y the inverse of the soft distance in metres (0: a
	//                     hard cut against the scene's depth).
	struct ParticleVertex
	{
		Float3 Position;
		Float3 PreviousPosition;
		Float4 Color;
		Float4 TexCoords;
		Float2 Params;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ParticleVertex) == 64, "ParticleVertex must match its 64-byte vertex layout");
	static_assert(offsetof(ParticleVertex, Position) == 0 && offsetof(ParticleVertex, PreviousPosition) == 12 && offsetof(ParticleVertex, Color) == 24
			&& offsetof(ParticleVertex, TexCoords) == 40 && offsetof(ParticleVertex, Params) == 56,
		"ParticleVertex's members must keep their vertex-attribute offsets");

}
#endif
