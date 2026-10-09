#include "EnginePCH.h"
#include "Engine/Renderer/SpotShadowAtlas.h"

#include "Engine/Renderer/ShadowCascades.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsFiniteSpotVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		struct RankedShadowSpot
		{
			float Importance = 0.0f;
			UUID Entity{};
			uint32_t Index = 0;
		};

	}

	Result<SpotShadowAtlas> AllocateSpotShadowAtlas(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices,
		const CameraData& camera)
	{
		if (!Utils::IsFiniteSpotVector(camera.Position) || camera.ViewportWidth == 0 || camera.ViewportHeight == 0
			|| (camera.ProjectionKind != RenderProjection::Perspective && camera.ProjectionKind != RenderProjection::Orthographic))
			return MakeError(ErrorCode::InvalidArgument, "Spot shadow camera is invalid");
		ENGINE_TRY(ComputeFrustumSliceCorners(camera, camera.NearClip, camera.FarClip));
		std::vector<uint32_t> indices(visibleLightIndices.begin(), visibleLightIndices.end());
		std::ranges::sort(indices);
		if ((!indices.empty() && indices.back() >= lights.size()) || std::ranges::adjacent_find(indices) != indices.end())
			return MakeError(ErrorCode::InvalidArgument, "Spot shadow visible indices are out of range or duplicated");
		std::vector<Utils::RankedShadowSpot> ranked;
		for (uint32_t index : indices)
		{
			const LightData& light = lights[index];
			if (light.Type != RenderLightType::Spot || !light.CastShadows)
				continue;
			const float directionLength = glm::length(light.Direction);
			if (!Utils::IsFiniteSpotVector(light.Direction) || !std::isfinite(directionLength) || directionLength <= 0.0f
				|| !Utils::IsFiniteSpotVector(light.Position) || !Utils::IsFiniteSpotVector(light.Color) || !std::isfinite(light.Intensity)
				|| light.Intensity < 0.0f || !Utils::IsFiniteSpotVector(light.Color * light.Intensity) || !std::isfinite(light.Range)
				|| light.Range <= 0.0f || !std::isfinite(light.SourceRadius) || light.SourceRadius < 0.0f
				|| !std::isfinite(light.InnerConeAngle) || light.InnerConeAngle < 0.0f || !std::isfinite(light.OuterConeAngle)
				|| light.OuterConeAngle <= 0.0f || light.OuterConeAngle >= 90.0f || light.InnerConeAngle > light.OuterConeAngle)
				return MakeError(ErrorCode::InvalidArgument, "Invalid spot shadow light at index {}", index);
			// Keep these float operations identical to CullLights: atlas budgeting must not reorder the visible lights.
			const float halfAngle = glm::radians(light.OuterConeAngle);
			const float cosine = std::cos(halfAngle);
			const glm::vec3 direction = glm::normalize(light.Direction);
			const float radius = halfAngle > glm::quarter_pi<float>() ? light.Range * std::sin(halfAngle) : light.Range / (2.0f * cosine);
			const glm::vec3 centre = light.Position + direction * (halfAngle > glm::quarter_pi<float>() ? light.Range * cosine : radius);
			const float distance = std::max(glm::length(centre - camera.Position) - radius, 1.0f);
			const float importance = light.Intensity * std::max({ light.Color.r, light.Color.g, light.Color.b }) / (distance * distance);
			if (!Utils::IsFiniteSpotVector(centre) || !std::isfinite(importance))
				return MakeError(ErrorCode::InvalidArgument, "Spot shadow importance cannot be represented at index {}", index);
			ranked.push_back({ .Importance = importance, .Entity = light.Entity, .Index = index });
		}
		std::ranges::sort(ranked, [](const Utils::RankedShadowSpot& left, const Utils::RankedShadowSpot& right)
		{
			if (left.Importance != right.Importance)
				return left.Importance > right.Importance;
			return std::tie(left.Entity, left.Index) < std::tie(right.Entity, right.Index);
		});
		SpotShadowAtlas result;
		const size_t count = std::min(ranked.size(), static_cast<size_t>(MaxSpotShadowLights));
		result.Dropped = static_cast<uint32_t>(ranked.size() - count);
		for (size_t index = 0; index < count; ++index)
		{
			const LightData& light = lights[ranked[index].Index];
			SpotShadowTile tile;
			tile.LightIndex = ranked[index].Index;
			tile.X = static_cast<uint32_t>(index % (SpotShadowAtlasSize / SpotShadowTileSize)) * SpotShadowTileSize;
			tile.Y = static_cast<uint32_t>(index / (SpotShadowAtlasSize / SpotShadowTileSize)) * SpotShadowTileSize;
			tile.UvScaleBias = glm::vec4(SpotShadowTileSize - 2 * SpotShadowGuardTexels, SpotShadowTileSize - 2 * SpotShadowGuardTexels,
								   tile.X + SpotShadowGuardTexels, tile.Y + SpotShadowGuardTexels)
				/ static_cast<float>(SpotShadowAtlasSize);
			tile.NearClip = std::min(0.01f, light.Range * 0.01f);
			tile.FarClip = light.Range;
			if (tile.NearClip <= 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "Spot shadow near plane cannot be represented");
			const glm::dvec3 direction = glm::normalize(glm::dvec3(light.Direction));
			const glm::dvec3 up = std::abs(direction.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
			const glm::dvec3 right = glm::normalize(glm::cross(direction, up));
			const glm::dvec3 vertical = glm::cross(right, direction);
			glm::dmat4 view(1.0);
			for (glm::length_t component = 0; component < 3; ++component)
			{
				view[component][0] = right[component];
				view[component][1] = vertical[component];
				view[component][2] = -direction[component];
			}
			view[3] = glm::dvec4(-glm::dot(right, glm::dvec3(light.Position)), -glm::dot(vertical, glm::dvec3(light.Position)),
				glm::dot(direction, glm::dvec3(light.Position)), 1.0);
			glm::dmat4 projection(0.0);
			projection[0][0] = projection[1][1] = 1.0 / std::tan(glm::radians(static_cast<double>(light.OuterConeAngle)));
			projection[2][2] = static_cast<double>(tile.NearClip) / (static_cast<double>(tile.FarClip) - tile.NearClip);
			projection[2][3] = -1.0;
			projection[3][2] = projection[2][2] * tile.FarClip;
			const glm::dmat4 viewProjection = projection * view;
			for (glm::length_t column = 0; column < 4; ++column)
				for (glm::length_t row = 0; row < 4; ++row)
					if (!std::isfinite(viewProjection[column][row]) || std::abs(viewProjection[column][row]) > std::numeric_limits<float>::max())
						return MakeError(ErrorCode::InvalidArgument, "Spot shadow matrix cannot be represented");
			tile.ViewProjection = glm::mat4(viewProjection);
			result.Tiles.push_back(tile);
		}
		return result;
	}

}
