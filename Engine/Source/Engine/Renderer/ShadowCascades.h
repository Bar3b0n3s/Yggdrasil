#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

// §8.7 pure camera/light math. Thread-safe, no retained references. Reverse-Z for perspective and orthographic views.
namespace Engine {

	inline constexpr uint32_t MaxShadowCascades = 4;
	inline constexpr float ShadowCascadeBlendFraction = 0.1f;

	struct ShadowCascade
	{
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		std::array<glm::vec3, 8> Corners{}; // slice world corners: near then far, each bottom-left, bottom-right, top-right, top-left
		float SplitNear = 0.0f;             // positive camera view distances
		float SplitFar = 0.0f;
		float BlendStart = 0.0f;
		float TexelWorldSize = 0.0f;
		float Radius = 0.0f; // bounding sphere's stabilized radius
		float LightNear = 0.0f;
		float LightFar = 0.0f;
		float PenumbraUvPerMetre = 0.0f;
	};

	struct ShadowCascadeSet
	{
		std::array<ShadowCascade, MaxShadowCascades> Cascades{};
		uint32_t Count = 0;
		uint32_t LightIndex = 0; // snapshot light index of first shadowed visible directional light in canonical order
	};

	// split_i = lerp(near + (far-near)*i/count, near*pow(far/near, i/count), lambda), i=1..count.
	// Unused entries = far. InvalidArgument for count outside [1,4], invalid clip range or lambda outside [0,1].
	[[nodiscard]] Result<std::array<float, MaxShadowCascades>> ComputeCascadeSplits(float nearClip, float farClip,
		uint32_t count, float lambda);

	// Slice corners through inverse projection/view, with finite slice distances even for infinite perspective far.
	// Orthographic corners form a box. InvalidArgument for singular/nonfinite matrices or invalid slice range.
	[[nodiscard]] Result<std::array<glm::vec3, 8>> ComputeFrustumSliceCorners(const CameraData& camera, float sliceNear, float sliceFar);

	// Sphere-fit each slice, use ComputeStabilizedCascadeRadius, then snap the light-space centre to world texels. Extend the
	// light near plane toward casters by ShadowDistance; depth-clamp devices additionally pancake near geometry.
	// Quantize the light-space Z centre too, so all matrix entries stay unchanged inside a three-axis snap cell.
	// Uses min(camera.FarClip, light.ShadowDistance), 10% blend/fade; LightAngle controls world-to-UV PCSS scale.
	// A movement staying inside one snap cell leaves the stabilized XY transform unchanged. Z fitting uses the same
	// stable sphere, not an unstable scene AABB. No temporal jitter. First-party math, no external shadow implementation.
	// InvalidArgument for a non-directional light, zero/nonfinite direction, invalid light/settings/camera data.
	[[nodiscard]] Result<ShadowCascadeSet> BuildShadowCascades(const CameraData& camera, const LightData& light,
		uint32_t lightIndex, uint32_t shadowMapSize);

	// For unsnapped sphere radius r and resolution N, R = ceil(16*r/(1-1/N))/16 (compute in double). Texel = 2*R/N.
	// Round each light-space centre coordinate to the nearest texel (ties toward +infinity). Each coordinate moves by
	// <= R/N, hence the fixed [-R,+R] box contains every slice corner after snapping. Extend Z toward casters only
	// after that box fit. R depends on slice shape, not translation: do not refit R from snapped corners. Round the
	// returned float upward if conversion would shrink R; use that final R consistently for texels and bounds.
	// InvalidArgument for nonpositive/nonfinite r, invalid power-of-two N [256,8192], or unrepresentable R.
	[[nodiscard]] Result<float> ComputeStabilizedCascadeRadius(float sphereRadius, uint32_t shadowMapSize);

}
