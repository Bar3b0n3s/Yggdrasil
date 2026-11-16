#include "EnginePCH.h"
#include "Engine/Renderer/PointShadowMaps.h"

#include "Engine/Core/Assert.h"
#include "Engine/Renderer/RenderPrepare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

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

	Result<PointShadowSet> AllocatePointShadows(std::span<const LightData> lights, std::span<const uint32_t> visibleLightIndices,
		std::span<const float> importance, const PointShadowSet* previous)
	{
		std::vector<uint32_t> sorted(visibleLightIndices.begin(), visibleLightIndices.end());
		std::ranges::sort(sorted);
		if ((!sorted.empty() && sorted.back() >= lights.size()) || std::ranges::adjacent_find(sorted) != sorted.end())
			return MakeError(ErrorCode::InvalidArgument, "Point shadow visible indices are out of range or duplicated");
		if (!importance.empty() && importance.size() != visibleLightIndices.size())
			return MakeError(ErrorCode::InvalidArgument, "Point shadow importance must be empty or one value per visible light");
		PointShadowSet result;
		// The candidates in CullLights' order, which is the importance order (ties by UUID, then snapshot index).
		struct Candidate
		{
			PointShadowLight Planned{};
			float Importance = 0.0f;
			const PointShadowLight* Previous = nullptr; // the light's place in `previous`, if it had one
		};
		std::vector<Candidate> candidates;
		for (size_t position = 0; position < visibleLightIndices.size(); ++position)
		{
			const uint32_t index = visibleLightIndices[position];
			const LightData& light = lights[index];
			if (light.Type != RenderLightType::Point || !light.CastShadows)
				continue;
			std::optional<PointShadowLight> planned = Utils::PlanPointShadow(light, index);
			if (!planned.has_value())
			{
				++result.Rejected;
				continue;
			}
			planned->Entity = light.Entity;
			Candidate candidate{ .Planned = *planned, .Importance = importance.empty() ? 0.0f : importance[position] };
			if (previous != nullptr && light.Entity.IsValid())
			{
				const auto held = std::ranges::find(previous->Lights, light.Entity, &PointShadowLight::Entity);
				if (held != previous->Lights.end())
					candidate.Previous = &*held;
			}
			candidates.push_back(candidate);
		}

		// M29 (decision 3): the best `count` of `positions` (indices into candidates, in their order), a candidate for which
		// `held` is true ranking with its importance times LightHysteresis; ties, and every rank without importance, keep the
		// order. Returns the chosen positions in their order.
		const auto chooseBest = [&candidates, &importance](std::vector<size_t> positions, size_t count, const auto& held)
		{
			if (positions.size() <= count)
				return positions;
			if (!importance.empty())
			{
				std::ranges::stable_sort(positions, [&candidates, &held](size_t left, size_t right)
				{
					const auto rank = [&candidates, &held](size_t position)
					{
						const Candidate& candidate = candidates[position];
						return held(candidate) ? candidate.Importance * LightHysteresis : candidate.Importance;
					};
					return rank(left) > rank(right);
				});
			}
			positions.resize(count);
			std::ranges::sort(positions);
			return positions;
		};
		std::vector<size_t> all(candidates.size());
		std::iota(all.begin(), all.end(), size_t{ 0 });
		const std::vector<size_t> planned = chooseBest(all, MaxPointShadowLights, [](const Candidate& candidate)
		{
			return candidate.Previous != nullptr;
		});
		result.Dropped = static_cast<uint32_t>(candidates.size() - planned.size());
		const std::vector<size_t> full = chooseBest(planned, MaxFullPointShadowLights, [](const Candidate& candidate)
		{
			return candidate.Previous != nullptr && candidate.Previous->Tier == PointShadowTier::Full;
		});

		// The tiers, then the layers: a light keeps its previous layer in the same tier, the others take the lowest free ones.
		std::array<std::array<bool, MaxPointShadowLights>, 2> taken{};
		for (const size_t position : planned)
		{
			Candidate& candidate = candidates[position];
			candidate.Planned.Tier = std::ranges::binary_search(full, position) ? PointShadowTier::Full : PointShadowTier::Half;
			const uint32_t capacity = candidate.Planned.Tier == PointShadowTier::Full ? MaxFullPointShadowLights : MaxHalfPointShadowLights;
			if (candidate.Previous != nullptr && candidate.Previous->Tier == candidate.Planned.Tier && candidate.Previous->Layer < capacity)
			{
				candidate.Planned.Layer = candidate.Previous->Layer;
				taken[std::to_underlying(candidate.Planned.Tier)][candidate.Planned.Layer] = true;
			}
			else
				candidate.Planned.Layer = MaxPointShadowLights; // unassigned
		}
		for (const size_t position : planned)
		{
			PointShadowLight& light = candidates[position].Planned;
			if (light.Layer != MaxPointShadowLights)
				continue;
			std::array<bool, MaxPointShadowLights>& layers = taken[std::to_underlying(light.Tier)];
			light.Layer = static_cast<uint32_t>(std::ranges::find(layers, false) - layers.begin());
			layers[light.Layer] = true;
		}
		for (const size_t position : planned)
			result.Lights.push_back(candidates[position].Planned);
		return result;
	}

	glm::mat4 ComputePointShadowFaceViewProjection(const glm::vec3& position, uint32_t face, float nearClip, float farClip)
	{
		ENGINE_CORE_ASSERT(face < PointShadowFaceCount, "Point shadow face {} is out of range", face);
		ENGINE_CORE_ASSERT(nearClip > 0.0f && nearClip < farClip, "Point shadow clip planes {} and {} are invalid", nearClip, farClip);
		return glm::mat4(Utils::ComputeFaceViewProjection(glm::dvec3(position), face, nearClip, farClip));
	}

}
