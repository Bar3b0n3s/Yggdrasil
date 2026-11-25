#include "EnginePCH.h"
#include "Engine/Renderer/MeshLod.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	MeshLodSphere ComputeMeshLodSphere(const Aabb& bounds, const glm::mat4& world)
	{
		const glm::vec3 center = (bounds.Min + bounds.Max) * 0.5f;
		const float halfDiagonal = glm::length(bounds.Max - bounds.Min) * 0.5f;
		const float scale = std::max({ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])) });
		return MeshLodSphere{ .Center = glm::vec3(world * glm::vec4(center, 1.0f)), .Radius = halfDiagonal * scale };
	}

	float ComputeMeshScreenSize(float radius, float distance, float projectionScale, bool orthographic)
	{
		if (orthographic)
			return radius * projectionScale;
		if (!(distance > radius))
			return std::numeric_limits<float>::infinity();
		return radius * projectionScale / distance;
	}

	MeshLodSelection SelectMeshLod(std::span<const float> switchSizes, float screenSize, float distance, const MeshLodParameters& parameters)
	{
		MeshLodSelection selection;
		const float width = std::max(parameters.FadeWidth, 0.0f);
		if (parameters.CullDistance > 0.0f && !(distance < parameters.CullDistance))
		{
			selection.DistanceCulled = true;
			return selection;
		}

		const float biased = screenSize * parameters.LodBias;
		uint32_t level = 0;
		while (level < switchSizes.size() && biased < switchSizes[level])
			++level;
		selection.Level = level;

		// Fading out before the cull distance: the view's level alone.
		const float fadeStart = parameters.CullDistance * (1.0f - width);
		if (parameters.CullDistance > 0.0f && width > 0.0f && distance > fadeStart)
		{
			const float t = (distance - fadeStart) / (parameters.CullDistance * width);
			selection.Draws[0] = MeshLodDraw{ .Level = level, .Fade = -std::clamp(t, 0.0f, 1.0f) };
			selection.DrawCount = 1;
			return selection;
		}

		// A cross-fade band: the switch below the level (to the next coarser one), then the switch above it (from the next
		// finer one). The coarser level takes t, the finer -t.
		if (width > 0.0f)
		{
			const auto band = [&selection, biased, width](uint32_t finer, float switchSize)
			{
				const float upper = switchSize * (1.0f + width * 0.5f);
				const float lower = switchSize * (1.0f - width * 0.5f);
				if (!(biased > lower) || !(biased < upper))
					return false;
				const float t = (upper - biased) / (switchSize * width);
				selection.Draws[0] = MeshLodDraw{ .Level = finer, .Fade = -t };
				selection.Draws[1] = MeshLodDraw{ .Level = finer + 1, .Fade = t };
				selection.DrawCount = 2;
				selection.CrossFade = true;
				return true;
			};
			if (level < switchSizes.size() && band(level, switchSizes[level]))
				return selection;
			if (level > 0 && band(level - 1, switchSizes[level - 1]))
				return selection;
		}

		selection.Draws[0] = MeshLodDraw{ .Level = level, .Fade = 0.0f };
		selection.DrawCount = 1;
		return selection;
	}

	std::optional<uint32_t> SelectShadowLod(std::span<const float> switchSizes, uint32_t viewLevel, float distance, const MeshLodParameters& parameters)
	{
		if (parameters.CullDistance > 0.0f && !(distance < parameters.CullDistance))
			return std::nullopt;
		const uint64_t level = static_cast<uint64_t>(viewLevel) + parameters.ShadowLodOffset;
		return static_cast<uint32_t>(std::min<uint64_t>(level, switchSizes.size()));
	}

	MeshLodParameters ToMeshLodParameters(const MeshDrawDistance& distance)
	{
		return { .CullDistance = distance.CullDistance, .LodBias = distance.LodBias, .ShadowLodOffset = distance.ShadowLodOffset, .FadeWidth = distance.FadeWidth };
	}

	namespace Utils {

		// The item's screen size and distance in `camera`'s view.
		[[nodiscard]] static std::pair<float, float> MeasureViewItem(const Aabb& bounds, const glm::mat4& world, const CameraData& camera)
		{
			const MeshLodSphere sphere = ComputeMeshLodSphere(bounds, world);
			const float distance = glm::length(sphere.Center - camera.Position);
			const bool orthographic = camera.ProjectionKind == RenderProjection::Orthographic;
			return { ComputeMeshScreenSize(sphere.Radius, distance, std::abs(camera.Projection[1][1]), orthographic), distance };
		}

	}

	MeshLodSelection SelectViewMeshLod(std::span<const float> switchSizes, const Aabb& bounds, const glm::mat4& world, const MeshDrawDistance& distance,
		const CameraData& camera)
	{
		const auto [screenSize, cameraDistance] = Utils::MeasureViewItem(bounds, world, camera);
		return SelectMeshLod(switchSizes, screenSize, cameraDistance, ToMeshLodParameters(distance));
	}

	std::optional<uint32_t> SelectViewShadowLod(std::span<const float> switchSizes, const Aabb& bounds, const glm::mat4& world, const MeshDrawDistance& distance,
		const CameraData& camera)
	{
		const auto [screenSize, cameraDistance] = Utils::MeasureViewItem(bounds, world, camera);
		const MeshLodParameters parameters = ToMeshLodParameters(distance);
		const MeshLodSelection selection = SelectMeshLod(switchSizes, screenSize, cameraDistance, parameters);
		return SelectShadowLod(switchSizes, selection.Level, cameraDistance, parameters);
	}

}
