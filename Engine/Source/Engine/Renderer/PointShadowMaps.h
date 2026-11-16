#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
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

	// At most this many point lights of a view cast shadows (Shared/ShadowConstants.h's MaxPointShadows; M29,
	// Docs/Decisions/0044-m29-light-limits.md decision 2: 16, from M23's 4): the MaxFullPointShadowLights most important at
	// full face size, the rest at half.
	inline constexpr uint32_t MaxPointShadowLights = MaxPointShadows;
	inline constexpr uint32_t MaxFullPointShadowLights = MaxFullPointShadows;
	inline constexpr uint32_t MaxHalfPointShadowLights = MaxPointShadowLights - MaxFullPointShadowLights;
	static_assert(MaxPointShadowLights == 16 && MaxFullPointShadowLights == 4);

	// The faces of one point light's cube, in Vulkan's cube face order (the layer order of a cube texture): +X, -X, +Y, -Y,
	// +Z, -Z (EnvironmentData.h's convention).
	inline constexpr uint32_t PointShadowFaceCount = 6;

	// The renderer's warning, once per renderer, and ProjectValidator's: more than MaxPointShadowLights visible
	// shadow-casting point lights (the least important ones stay lit, unshadowed), as RENDER_SPOT_SHADOW_BUDGET does for
	// spots.
	inline constexpr std::string_view RenderPointShadowBudgetCode = "RENDER_POINT_SHADOW_BUDGET";

	// The edge of a full-tier point-shadow cube face in texels: a quarter of RenderQualitySettings::ShadowMapSize (512 at the
	// default 2048), at least 128, so the 24 faces of four lights cost what one 2048 x 2048 map and a half do.
	[[nodiscard]] constexpr uint32_t GetPointShadowFaceSize(uint32_t shadowMapSize)
	{
		const uint32_t quarter = shadowMapSize / 4;
		return quarter < 128 ? 128 : quarter;
	}

	// M29 (decision 2): the edge of a half-tier face, half the full tier's (256 at the default 2048, at least 64).
	[[nodiscard]] constexpr uint32_t GetPointShadowHalfFaceSize(uint32_t shadowMapSize)
	{
		return GetPointShadowFaceSize(shadowMapSize) / 2;
	}

	// M29 (decision 2): which cube array holds a planned light's faces: PointShadowMaps (t12, GetPointShadowFaceSize) or
	// PointShadowMapsHalf (t15, GetPointShadowHalfFaceSize). Its value is PointShadowConstants::Indices.z.
	enum class PointShadowTier : uint8_t
	{
		Full = PointShadowTierFull,
		Half = PointShadowTierHalf
	};

	// One shadowed point light of the view.
	struct PointShadowLight
	{
		uint32_t LightIndex = 0; // into the snapshot's Lights
		UUID Entity{};           // M29: the light's entity, which the next render's hysteresis matches
		PointShadowTier Tier = PointShadowTier::Full;
		uint32_t Layer = 0; // its faces are its tier's cube array's layers 6 * Layer to 6 * Layer + 5
		// ComputePointShadowFaceViewProjection of each face, in PointShadowFaceCount order.
		std::array<glm::mat4, PointShadowFaceCount> FaceViewProjections{};
		float NearClip = 0.05f; // max(SourceRadius, 0.05), at most Range / 2
		float FarClip = 0.0f;   // the light's Range
	};

	// A view's point-shadow plan.
	struct PointShadowSet
	{
		// At most MaxPointShadowLights (at most MaxFullPointShadowLights of them Full) in CullLights' order (descending
		// importance, ties by UUID then snapshot index); the Layers of one tier are distinct and below its capacity.
		std::vector<PointShadowLight> Lights{};
		uint32_t Dropped = 0;  // visible shadow-casting points beyond the limit (lit, unshadowed)
		uint32_t Rejected = 0; // visible shadow-casting points whose data cannot be shadowed (lit, unshadowed)
	};

	// The plan of the view: the visible point lights (CullLights' Visible, in its importance order) whose CastShadows is
	// set. A candidate whose data cannot be shadowed (a non-finite position, a range not above twice its near plane) is left
	// out and counted in Rejected, and never costs another light its shadow. M29 (decisions 2 and 3): of the others, at most
	// MaxPointShadowLights are planned, the rest Dropped; when more compete, a candidate that `previous` (the view's previous
	// plan, null after a reset) planned ranks with its `importance` (CullLights' LightCullResult::Importance, one per visible
	// index; empty ranks by the visible order alone) times LightHysteresis. Of the planned, the MaxFullPointShadowLights best
	// are Full, a light Full in `previous` ranking with its importance times LightHysteresis, and the rest Half. A light
	// `previous` planned in the same tier keeps its Layer; the others take the lowest free layers of their tier in order.
	// Without `previous` (or when every candidate fits) the plan is CullLights' order, the first four Full at layers 0 to 3,
	// as M23 planned them. InvalidArgument only for an out-of-range or duplicate index, or an `importance` that is neither
	// empty nor one per visible index. Pure.
	[[nodiscard]] Result<PointShadowSet> AllocatePointShadows(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices,
		std::span<const float> importance = {}, const PointShadowSet* previous = nullptr);

	// The view-projection of cube face `face` (< PointShadowFaceCount) of a light at `position`: a 90-degree perspective
	// reverse-Z projection whose depth is 1 at nearClip and 0 at farClip, looking down the face's major axis with the up
	// vector of Vulkan's cube face table, so a direction from the light lands on the face and texel a cube sampler reads
	// for it (EnvironmentData.h). Pure; asserts face < 6 and 0 < nearClip < farClip.
	[[nodiscard]] glm::mat4 ComputePointShadowFaceViewProjection(const glm::vec3& position, uint32_t face, float nearClip, float farClip);

}
