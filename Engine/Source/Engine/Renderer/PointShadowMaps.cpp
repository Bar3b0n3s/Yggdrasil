#include "EnginePCH.h"
#include "Engine/Renderer/PointShadowMaps.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// The point-shadow plan (PointShadowMaps.h; Docs/Decisions/0034-m23-contract.md decision 3).

namespace Engine {

	namespace Utils {

		// The axes of Vulkan's cube face selection table, per face in PointShadowFaceCount order: the major axis (ma), and the
		// directions whose components are the face's sc and tc, so that s = sc / |ma| and t = tc / |ma|. Common/Shadows.slang
		// reads the faces through the same table.
		struct PointShadowFaceAxes
		{
			glm::dvec3 Major{};
			glm::dvec3 Sc{};
			glm::dvec3 Tc{};
		};

		static constexpr std::array<PointShadowFaceAxes, PointShadowFaceCount> PointShadowFaces = { {
			{ .Major = { 1, 0, 0 }, .Sc = { 0, 0, -1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { -1, 0, 0 }, .Sc = { 0, 0, 1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, 1 } },
			{ .Major = { 0, -1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, -1 } },
			{ .Major = { 0, 0, 1 }, .Sc = { 1, 0, 0 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 0, -1 }, .Sc = { -1, 0, 0 }, .Tc = { 0, -1, 0 } },
		} };

		[[nodiscard]] static bool IsFinitePointVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The face view-projection in double precision (ComputePointShadowFaceViewProjection).
		[[nodiscard]] static glm::dmat4 ComputeFaceViewProjection(const glm::dvec3& position, uint32_t face, double nearClip, double farClip)
		{
			// View space: x along sc, y against tc (NVRHI's flipped viewport puts NDC +Y on row 0, the face's t = 0), and the
			// camera looking down -z along the major axis, so clip w is the distance along the major axis. The basis is a
			// reflection (every face of a cube is seen from inside), so the casters' winding flips (ShadowPass).
			const PointShadowFaceAxes& axes = PointShadowFaces[face];
			const glm::dvec3 right = axes.Sc;
			const glm::dvec3 up = -axes.Tc;
			const glm::dvec3 back = -axes.Major;
			glm::dmat4 view(1.0);
			for (glm::length_t component = 0; component < 3; ++component)
			{
				view[component][0] = right[component];
				view[component][1] = up[component];
				view[component][2] = back[component];
			}
			view[3] = glm::dvec4(-glm::dot(right, position), -glm::dot(up, position), -glm::dot(back, position), 1.0);
			// A 90-degree reverse-Z perspective: depth 1 at nearClip, 0 at farClip.
			glm::dmat4 projection(0.0);
			projection[0][0] = 1.0;
			projection[1][1] = 1.0;
			projection[2][2] = nearClip / (farClip - nearClip);
			projection[2][3] = -1.0;
			projection[3][2] = projection[2][2] * farClip;
			return projection * view;
		}

		// The planned light of `light` with its faces, or nullopt when its data cannot be shadowed (a non-finite position or
		// range, a negative or non-finite source radius, a range not above twice its near plane, or a projection that a float
		// cannot hold).
		[[nodiscard]] static std::optional<PointShadowLight> PlanPointShadow(const LightData& light, uint32_t index)
		{
			if (!IsFinitePointVector(light.Position) || !std::isfinite(light.Range) || light.Range <= 0.0f || !std::isfinite(light.SourceRadius)
				|| light.SourceRadius < 0.0f)
				return std::nullopt;
			const float nearClip = std::min(std::max(light.SourceRadius, 0.05f), light.Range * 0.5f);
			if (!(light.Range > 2.0f * nearClip))
				return std::nullopt;
			PointShadowLight planned;
			planned.LightIndex = index;
			planned.NearClip = nearClip;
			planned.FarClip = light.Range;
			for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
			{
				const glm::dmat4 viewProjection = ComputeFaceViewProjection(glm::dvec3(light.Position), face, nearClip, light.Range);
				for (glm::length_t column = 0; column < 4; ++column)
				{
					for (glm::length_t row = 0; row < 4; ++row)
					{
						if (!std::isfinite(viewProjection[column][row]) || std::abs(viewProjection[column][row]) > std::numeric_limits<float>::max())
							return std::nullopt;
					}
				}
				planned.FaceViewProjections[face] = glm::mat4(viewProjection);
			}
			return planned;
		}

	}

	Result<PointShadowSet> AllocatePointShadows(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices)
	{
		std::vector<uint32_t> sorted(visibleLightIndices.begin(), visibleLightIndices.end());
		std::ranges::sort(sorted);
		if ((!sorted.empty() && sorted.back() >= lights.size()) || std::ranges::adjacent_find(sorted) != sorted.end())
			return MakeError(ErrorCode::InvalidArgument, "Point shadow visible indices are out of range or duplicated");
		PointShadowSet result;
		// CullLights' order is the importance order (ties by UUID, then snapshot index): the first candidates win.
		for (const uint32_t index : visibleLightIndices)
		{
			const LightData& light = lights[index];
			if (light.Type != RenderLightType::Point || !light.CastShadows)
				continue;
			std::optional<PointShadowLight> planned = Utils::PlanPointShadow(light, index);
			if (!planned.has_value())
			{
				++result.Rejected;
				continue;
			}
			if (result.Lights.size() == MaxPointShadowLights)
			{
				++result.Dropped;
				continue;
			}
			planned->Layer = static_cast<uint32_t>(result.Lights.size());
			result.Lights.push_back(*planned);
		}
		return result;
	}

	glm::mat4 ComputePointShadowFaceViewProjection(const glm::vec3& position, uint32_t face, float nearClip, float farClip)
	{
		ENGINE_CORE_ASSERT(face < PointShadowFaceCount, "Point shadow face {} is out of range", face);
		ENGINE_CORE_ASSERT(nearClip > 0.0f && nearClip < farClip, "Point shadow clip planes {} and {} are invalid", nearClip, farClip);
		return glm::mat4(Utils::ComputeFaceViewProjection(glm::dvec3(position), face, nearClip, farClip));
	}

}
