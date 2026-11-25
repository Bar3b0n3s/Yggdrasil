#include "EnginePCH.h"
#include "Engine/Renderer/RenderStaticDraws.h"

#include <algorithm>
#include <array>
#include <cmath>

// The cell tests of the static path (RenderStaticDraws.h). Each counts, per plane of the view's test, the corners of the
// cell's box outside it: all eight outside one plane is Outside, none outside any plane is Inside. The box is the inflated
// one, so both answers hold for every point of the cell's draws by the margin, well beyond the rounding of the per-draw
// tests, which see each draw's own corners through its own world matrix.

namespace Engine {

	namespace Utils {

		[[nodiscard]] static float GetLargestMagnitude(const glm::vec3& value)
		{
			return std::max({ std::abs(value.x), std::abs(value.y), std::abs(value.z) });
		}

		// The visibility of corner counts against `planes` planes: outside[p] corners outside plane p, of eight.
		template<size_t Planes>
		[[nodiscard]] static StaticCellVisibility ClassifyCorners(const std::array<uint32_t, Planes>& outside)
		{
			if (std::ranges::any_of(outside, [](uint32_t count)
			{
				return count == 8;
			}))
				return StaticCellVisibility::Outside;
			if (std::ranges::all_of(outside, [](uint32_t count)
			{
				return count == 0;
			}))
				return StaticCellVisibility::Inside;
			return StaticCellVisibility::Partial;
		}

	}

	Aabb InflateStaticCellBox(const Aabb& box, const glm::vec3& reference)
	{
		if (box.IsEmpty())
			return box;
		const float magnitude = std::max({ Utils::GetLargestMagnitude(box.Min), Utils::GetLargestMagnitude(box.Max), Utils::GetLargestMagnitude(reference) });
		const float margin = magnitude * 1e-4f + 1e-3f;
		return Aabb{ .Min = box.Min - glm::vec3(margin), .Max = box.Max + glm::vec3(margin) };
	}

	StaticCellVisibility ClassifyStaticCell(const Aabb& box, const CameraData& camera)
	{
		if (box.IsEmpty())
			return StaticCellVisibility::Partial;
		const glm::mat4 viewProjection = camera.Projection * camera.View;
		std::array<uint32_t, 7> outside{};
		for (const glm::vec3& corner : box.GetCorners())
		{
			const glm::vec4 point(corner, 1.0f);
			const glm::vec4 clip = viewProjection * point;
			const float viewDistance = -(camera.View * point).z;
			outside[0] += clip.x < -clip.w ? 1U : 0U;
			outside[1] += clip.x > clip.w ? 1U : 0U;
			outside[2] += clip.y < -clip.w ? 1U : 0U;
			outside[3] += clip.y > clip.w ? 1U : 0U;
			outside[4] += clip.z > clip.w ? 1U : 0U;
			outside[5] += clip.z < 0.0f ? 1U : 0U;
			outside[6] += viewDistance > camera.FarClip ? 1U : 0U;
		}
		return Utils::ClassifyCorners(outside);
	}

	StaticCellVisibility ClassifyStaticCellClipPlane(const Aabb& box, const glm::vec4& clipPlane)
	{
		if (box.IsEmpty())
			return StaticCellVisibility::Partial;
		std::array<uint32_t, 1> behind{};
		for (const glm::vec3& corner : box.GetCorners())
			behind[0] += glm::dot(clipPlane, glm::vec4(corner, 1.0f)) < 0.0f ? 1U : 0U;
		return Utils::ClassifyCorners(behind);
	}

	StaticCellVisibility ClassifyStaticCellShadow(const Aabb& box, const glm::mat4& viewProjection, bool pancake)
	{
		if (box.IsEmpty())
			return StaticCellVisibility::Partial;
		std::array<uint32_t, 6> outside{};
		for (const glm::vec3& corner : box.GetCorners())
		{
			const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
			outside[0] += clip.x < -clip.w ? 1U : 0U;
			outside[1] += clip.x > clip.w ? 1U : 0U;
			outside[2] += clip.y < -clip.w ? 1U : 0U;
			outside[3] += clip.y > clip.w ? 1U : 0U;
			outside[4] += clip.z < 0.0f ? 1U : 0U;
			// Pancaking keeps the casters nearer the light than the map's near plane (ShadowPass.cpp's IsOutsideShadow).
			outside[5] += !pancake && clip.z > clip.w ? 1U : 0U;
		}
		return Utils::ClassifyCorners(outside);
	}

	StaticCellVisibility ClassifyStaticCellLight(const Aabb& box, const glm::vec3& lightCentre, const glm::vec3& lightHalfExtent)
	{
		if (box.IsEmpty())
			return StaticCellVisibility::Partial;
		const glm::vec3 centre = (box.Min + box.Max) * 0.5f;
		const glm::vec3 halfExtent = (box.Max - box.Min) * 0.5f;
		const glm::vec3 distance = glm::abs(centre - lightCentre);
		// ShadowBoxesOverlap's reach (ShadowPass.cpp), for the inflated box: beyond it on one axis, no box inside the cell's
		// reaches the light's either.
		const glm::vec3 reach = (halfExtent + lightHalfExtent) * 1.001f + (glm::abs(centre) + glm::abs(lightCentre)) * 1e-5f + glm::vec3(1e-4f);
		if (glm::any(glm::greaterThan(distance, reach)))
			return StaticCellVisibility::Outside;
		if (glm::all(glm::lessThanEqual(distance + halfExtent, lightHalfExtent)))
			return StaticCellVisibility::Inside;
		return StaticCellVisibility::Partial;
	}

}
