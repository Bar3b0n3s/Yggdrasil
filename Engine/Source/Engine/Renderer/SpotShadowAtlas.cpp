#include "EnginePCH.h"
#include "Engine/Renderer/SpotShadowAtlas.h"

#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/ShadowCascades.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace Engine {

	namespace {

		// A visible shadow-casting spot whose data can be shadowed, with its projection, ranked by importance.
		struct RankedShadowSpot
		{
			float Importance = 0.0f;
			UUID Entity{};
			uint32_t Index = 0;
			glm::mat4 ViewProjection{ 1.0f };
			float NearClip = 0.0f;
		};

	}

	namespace Utils {

		[[nodiscard]] static bool IsFiniteSpotVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The ranked candidate of `light`, or nullopt when its data cannot be shadowed: the renderer still lights it (CullLights
		// and the shading path clamp its values), but this atlas leaves it unshadowed instead of failing every spot's shadow.
		[[nodiscard]] static std::optional<RankedShadowSpot> RankShadowSpot(const LightData& light, uint32_t index, const CameraData& camera)
		{
			const float directionLength = glm::length(light.Direction);
			if (!IsFiniteSpotVector(light.Direction) || !std::isfinite(directionLength) || directionLength <= 0.0f
				|| !IsFiniteSpotVector(light.Position) || !IsFiniteSpotVector(light.Color) || !std::isfinite(light.Intensity)
				|| light.Intensity < 0.0f || !IsFiniteSpotVector(light.Color * light.Intensity) || !std::isfinite(light.Range)
				|| light.Range <= 0.0f || !std::isfinite(light.SourceRadius) || light.SourceRadius < 0.0f
				|| !std::isfinite(light.InnerConeAngle) || light.InnerConeAngle < 0.0f || !std::isfinite(light.OuterConeAngle)
				|| light.OuterConeAngle <= 0.0f || light.OuterConeAngle >= 90.0f || light.InnerConeAngle > light.OuterConeAngle)
				return std::nullopt;
			// Keep these float operations identical to CullLights: atlas budgeting must not reorder the visible lights.
			const float halfAngle = glm::radians(light.OuterConeAngle);
			const float cosine = std::cos(halfAngle);
			const glm::vec3 direction = glm::normalize(light.Direction);
			const float radius = halfAngle > glm::quarter_pi<float>() ? light.Range * std::sin(halfAngle) : light.Range / (2.0f * cosine);
			const glm::vec3 centre = light.Position + direction * (halfAngle > glm::quarter_pi<float>() ? light.Range * cosine : radius);
			const float distance = std::max(glm::length(centre - camera.Position) - radius, 1.0f);
			const float importance = light.Intensity * std::max({ light.Color.r, light.Color.g, light.Color.b }) / (distance * distance);
			if (!IsFiniteSpotVector(centre) || !std::isfinite(importance))
				return std::nullopt;

			const float nearClip = std::min(0.01f, light.Range * 0.01f);
			if (nearClip <= 0.0f)
				return std::nullopt;
			const glm::dvec3 forward = glm::normalize(glm::dvec3(light.Direction));
			const glm::dvec3 up = std::abs(forward.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
			const glm::dvec3 right = glm::normalize(glm::cross(forward, up));
			const glm::dvec3 vertical = glm::cross(right, forward);
			glm::dmat4 view(1.0);
			for (glm::length_t component = 0; component < 3; ++component)
			{
				view[component][0] = right[component];
				view[component][1] = vertical[component];
				view[component][2] = -forward[component];
			}
			view[3] = glm::dvec4(-glm::dot(right, glm::dvec3(light.Position)), -glm::dot(vertical, glm::dvec3(light.Position)),
				glm::dot(forward, glm::dvec3(light.Position)), 1.0);
			glm::dmat4 projection(0.0);
			projection[0][0] = projection[1][1] = 1.0 / std::tan(glm::radians(static_cast<double>(light.OuterConeAngle)));
			projection[2][2] = static_cast<double>(nearClip) / (static_cast<double>(light.Range) - nearClip);
			projection[2][3] = -1.0;
			projection[3][2] = projection[2][2] * light.Range;
			const glm::dmat4 viewProjection = projection * view;
			for (glm::length_t column = 0; column < 4; ++column)
				for (glm::length_t row = 0; row < 4; ++row)
					if (!std::isfinite(viewProjection[column][row]) || std::abs(viewProjection[column][row]) > std::numeric_limits<float>::max())
						return std::nullopt;
			return RankedShadowSpot{ .Importance = importance, .Entity = light.Entity, .Index = index, .ViewProjection = glm::mat4(viewProjection), .NearClip = nearClip };
		}

	}

	Result<SpotShadowAtlas> AllocateSpotShadowAtlas(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices,
		const CameraData& camera, const SpotShadowAtlas* previous)
	{
		if (!Utils::IsFiniteSpotVector(camera.Position) || camera.ViewportWidth == 0 || camera.ViewportHeight == 0
			|| (camera.ProjectionKind != RenderProjection::Perspective && camera.ProjectionKind != RenderProjection::Orthographic))
			return MakeError(ErrorCode::InvalidArgument, "Spot shadow camera is invalid");
		ENGINE_TRY(ComputeFrustumSliceCorners(camera, camera.NearClip, camera.FarClip));
		std::vector<uint32_t> indices(visibleLightIndices.begin(), visibleLightIndices.end());
		std::ranges::sort(indices);
		if ((!indices.empty() && indices.back() >= lights.size()) || std::ranges::adjacent_find(indices) != indices.end())
			return MakeError(ErrorCode::InvalidArgument, "Spot shadow visible indices are out of range or duplicated");
		SpotShadowAtlas result;
		std::vector<RankedShadowSpot> ranked;
		for (uint32_t index : indices)
		{
			const LightData& light = lights[index];
			if (light.Type != RenderLightType::Spot || !light.CastShadows)
				continue;
			std::optional<RankedShadowSpot> candidate = Utils::RankShadowSpot(light, index, camera);
			if (candidate.has_value())
				ranked.push_back(*candidate);
			else
				++result.Rejected;
		}
		const auto byImportance = [](const RankedShadowSpot& left, const RankedShadowSpot& right)
		{
			if (left.Importance != right.Importance)
				return left.Importance > right.Importance;
			return std::tie(left.Entity, left.Index) < std::tie(right.Entity, right.Index);
		};
		// M29 (decision 3): the previous atlas's tile of a spot, if it had one.
		const auto previousTile = [previous](const RankedShadowSpot& spot) -> const SpotShadowTile*
		{
			if (previous == nullptr || !spot.Entity.IsValid())
				return nullptr;
			const auto held = std::ranges::find(previous->Tiles, spot.Entity, &SpotShadowTile::Entity);
			return held != previous->Tiles.end() ? &*held : nullptr;
		};
		if (ranked.size() > MaxSpotShadowLights)
		{
			std::ranges::sort(ranked, [&byImportance, &previousTile](const RankedShadowSpot& left, const RankedShadowSpot& right)
			{
				const float leftRank = previousTile(left) != nullptr ? left.Importance * LightHysteresis : left.Importance;
				const float rightRank = previousTile(right) != nullptr ? right.Importance * LightHysteresis : right.Importance;
				if (leftRank != rightRank)
					return leftRank > rightRank;
				return byImportance(left, right);
			});
			result.Dropped = static_cast<uint32_t>(ranked.size() - MaxSpotShadowLights);
			ranked.resize(MaxSpotShadowLights);
		}
		std::ranges::sort(ranked, byImportance);
		// The slots: a spot keeps its previous one, the others take the lowest free ones in importance order.
		std::array<bool, MaxSpotShadowLights> taken{};
		std::vector<uint32_t> slots(ranked.size(), MaxSpotShadowLights);
		for (size_t index = 0; index < ranked.size(); ++index)
		{
			const SpotShadowTile* held = previousTile(ranked[index]);
			if (held != nullptr && held->Slot < MaxSpotShadowLights && !taken[held->Slot])
			{
				slots[index] = held->Slot;
				taken[held->Slot] = true;
			}
		}
		for (uint32_t& slot : slots)
		{
			if (slot != MaxSpotShadowLights)
				continue;
			slot = static_cast<uint32_t>(std::ranges::find(taken, false) - taken.begin());
			taken[slot] = true;
		}
		for (size_t index = 0; index < ranked.size(); ++index)
		{
			const RankedShadowSpot& spot = ranked[index];
			SpotShadowTile tile;
			tile.LightIndex = spot.Index;
			tile.Entity = spot.Entity;
			tile.Slot = slots[index];
			tile.X = tile.Slot % (SpotShadowAtlasSize / SpotShadowTileSize) * SpotShadowTileSize;
			tile.Y = tile.Slot / (SpotShadowAtlasSize / SpotShadowTileSize) * SpotShadowTileSize;
			tile.UvScaleBias = glm::vec4(SpotShadowTileSize - 2 * SpotShadowGuardTexels, SpotShadowTileSize - 2 * SpotShadowGuardTexels,
								   tile.X + SpotShadowGuardTexels, tile.Y + SpotShadowGuardTexels)
				/ static_cast<float>(SpotShadowAtlasSize);
			tile.NearClip = spot.NearClip;
			tile.FarClip = lights[spot.Index].Range;
			tile.ViewProjection = spot.ViewProjection;
			result.Tiles.push_back(tile);
		}
		return result;
	}

}
