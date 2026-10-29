#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/ShadowConstants.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Point-light shadows (M23, C23.1, R-PLS; Docs/Decisions/0034-m23-contract.md decision 3): the CPU plan of the
// PointShadows pass (the Shadows stage, after SpotShadows), which renders each planned light's six cube faces with the
// Shadow program's pipelines into the PointShadowMaps cube array, and the face convention the forward pass samples with
// (Common/Shadows.slang). Pure, thread-safe and deterministic, like SpotShadowAtlas.h.

namespace Engine {

	// At most this many point lights of a view cast shadows (Shared/ShadowConstants.h's MaxPointShadows).
	inline constexpr uint32_t MaxPointShadowLights = MaxPointShadows;
	static_assert(MaxPointShadowLights == 4);

	// The faces of one point light's cube, in Vulkan's cube face order (the layer order of a cube texture): +X, -X, +Y, -Y,
	// +Z, -Z (EnvironmentData.h's convention).
	inline constexpr uint32_t PointShadowFaceCount = 6;

	// The renderer's warning, once per renderer, and ProjectValidator's: more than MaxPointShadowLights visible
	// shadow-casting point lights (the least important ones stay lit, unshadowed), as RENDER_SPOT_SHADOW_BUDGET does for
	// spots.
	inline constexpr std::string_view RenderPointShadowBudgetCode = "RENDER_POINT_SHADOW_BUDGET";

	// The edge of a point-shadow cube face in texels: a quarter of RenderQualitySettings::ShadowMapSize (512 at the default
	// 2048), at least 128, so the 24 faces of four lights cost what one 2048 x 2048 map and a half do.
	[[nodiscard]] constexpr uint32_t GetPointShadowFaceSize(uint32_t shadowMapSize)
	{
		const uint32_t quarter = shadowMapSize / 4;
		return quarter < 128 ? 128 : quarter;
	}

	// One shadowed point light of the view.
	struct PointShadowLight
	{
		uint32_t LightIndex = 0; // into the snapshot's Lights
		uint32_t Layer = 0;      // its faces are the cube array's layers 6 * Layer to 6 * Layer + 5
		// ComputePointShadowFaceViewProjection of each face, in PointShadowFaceCount order.
		std::array<glm::mat4, PointShadowFaceCount> FaceViewProjections{};
		float NearClip = 0.05f; // max(SourceRadius, 0.05), at most Range / 2
		float FarClip = 0.0f;   // the light's Range
	};

	// A view's point-shadow plan.
	struct PointShadowSet
	{
		std::vector<PointShadowLight> Lights{}; // at most MaxPointShadowLights, descending importance then UUID; Layer = index
		uint32_t Dropped = 0;                   // visible shadow-casting points beyond the limit (lit, unshadowed)
		uint32_t Rejected = 0;                  // visible shadow-casting points whose data cannot be shadowed (lit, unshadowed)
	};

	// The plan of the view: the visible point lights (CullLights' Visible, in its importance order) whose CastShadows is
	// set, the first MaxPointShadowLights of them in that order, ties by UUID then snapshot index as CullLights breaks
	// them. A candidate whose data cannot be shadowed (a non-finite position, a range not above twice its near plane) is
	// left out and counted in Rejected, and never costs another light its shadow. InvalidArgument only for an out-of-range
	// or duplicate index. Pure.
	[[nodiscard]] Result<PointShadowSet> AllocatePointShadows(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices);

	// The view-projection of cube face `face` (< PointShadowFaceCount) of a light at `position`: a 90-degree perspective
	// reverse-Z projection whose depth is 1 at nearClip and 0 at farClip, looking down the face's major axis with the up
	// vector of Vulkan's cube face table, so a direction from the light lands on the face and texel a cube sampler reads
	// for it (EnvironmentData.h). Pure; asserts face < 6 and 0 < nearClip < farClip.
	[[nodiscard]] glm::mat4 ComputePointShadowFaceViewProjection(const glm::vec3& position, uint32_t face, float nearClip, float farClip);

}
