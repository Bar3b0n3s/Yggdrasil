#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

namespace Engine {

	// Registry name "RenderDistance" (Architecture §5.3, §8.3 "Mesh levels"; M46,
	// Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3C): how far and in which level of detail the
	// MeshRenderer on its entity draws, in a category of its own. A MeshRenderer without one takes these defaults, so a mesh
	// with levels switches without it. Not a MeshRenderer field: every MeshRenderer field is written in every scene, which
	// the state hash hashes. Per view, by that view's camera (Renderer/MeshLod.h):
	//   - CullDistance: beyond this distance from the camera, in metres, the item draws and casts nothing; 0 only FarClip
	//     culls. The drawn level fades out across the last FadeWidth of it.
	//   - LodBias: multiplies the item's screen size before the levels' switch sizes compare it (2 keeps each level until
	//     half its switch size).
	//   - ShadowLodOffset: a shadow caster draws its view's level plus this, clamped to the last level.
	//   - FadeWidth: the band around each switch and before CullDistance, as a share of the value, inside which two levels
	//     cross-fade (or the last fades out); 0 switches sharply.
	struct RenderDistanceComponent
	{
		float CullDistance = 0.0f;
		float LodBias = 1.0f;
		uint32_t ShadowLodOffset = 1;
		float FadeWidth = 0.1f;
	};

}
