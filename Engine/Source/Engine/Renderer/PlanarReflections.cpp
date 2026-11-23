#include "EnginePCH.h"
#include "Engine/Renderer/PlanarReflections.h"

#include "Engine/Core/Assert.h"
#include "Shared/PlanarReflectionConstants.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		// A clip-space w at or below this is on or behind a perspective camera's plane through its position.
		constexpr float MinScreenRectClipW = 1.0e-6f;

		[[nodiscard]] static bool IsFinite(const glm::dmat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		[[nodiscard]] static bool IsFinite(const glm::dvec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		[[nodiscard]] static glm::vec4 GetBoxCorner(const Aabb& bounds, uint32_t corner)
		{
			return glm::vec4((corner & 1U) != 0 ? bounds.Max.x : bounds.Min.x, (corner & 2U) != 0 ? bounds.Max.y : bounds.Min.y,
				(corner & 4U) != 0 ? bounds.Max.z : bounds.Min.z, 1.0f);
		}

		// Whether candidate `a` ranks before `b`: the larger finite area first, then the smaller entity, then the smaller item.
		[[nodiscard]] static bool RanksBefore(const PlanarReflectionCandidate& a, const PlanarReflectionCandidate& b)
		{
			const bool aFinite = std::isfinite(a.ScreenArea);
			const bool bFinite = std::isfinite(b.ScreenArea);
			if (aFinite != bFinite)
				return aFinite;
			if (aFinite && a.ScreenArea != b.ScreenArea)
				return a.ScreenArea > b.ScreenArea;
			if (a.Entity != b.Entity)
				return a.Entity < b.Entity;
			return a.Item < b.Item;
		}

	}

	float PlanarReflectionScreenRect::GetPixelArea(uint32_t width, uint32_t height) const
	{
		return (Max.x - Min.x) * 0.5f * static_cast<float>(width) * (Max.y - Min.y) * 0.5f * static_cast<float>(height);
	}

	std::optional<PlanarReflectionPlane> ComputePlanarReflectionPlane(const PlanarReflectionItem& item)
	{
		const glm::dvec3 localNormal(item.Normal);
		const glm::dmat4 world(item.World);
		const double offset = static_cast<double>(item.Offset);
		if (!Utils::IsFinite(localNormal) || !std::isfinite(offset) || !Utils::IsFinite(world))
			return std::nullopt;
		const double localLength = glm::length(localNormal);
		if (!(localLength > 0.0) || !std::isfinite(localLength))
			return std::nullopt;
		const glm::dvec3 unitNormal = localNormal / localLength;

		const glm::dmat3 linear(world);
		const double determinant = glm::determinant(linear);
		if (determinant == 0.0 || !std::isfinite(determinant))
			return std::nullopt;
		const glm::dvec3 worldNormal = glm::transpose(glm::inverse(linear)) * unitNormal;
		const double worldLength = glm::length(worldNormal);
		if (!(worldLength > 0.0) || !std::isfinite(worldLength))
			return std::nullopt;
		const glm::dvec3 normal = worldNormal / worldLength;
		const glm::dvec3 point = glm::dvec3(world * glm::dvec4(unitNormal * offset, 1.0));
		const double distance = -glm::dot(normal, point);
		if (!Utils::IsFinite(normal) || !std::isfinite(distance))
			return std::nullopt;
		return PlanarReflectionPlane{ .Normal = glm::vec3(normal), .Distance = static_cast<float>(distance) };
	}

	glm::mat4 ComputePlanarReflectionMatrix(const PlanarReflectionPlane& plane)
	{
		const glm::vec3& n = plane.Normal;
		glm::mat4 reflection(1.0f);
		for (int column = 0; column < 3; ++column)
		{
			for (int row = 0; row < 3; ++row)
				reflection[column][row] = (column == row ? 1.0f : 0.0f) - 2.0f * n[column] * n[row];
		}
		reflection[3] = glm::vec4(-2.0f * plane.Distance * n, 1.0f);
		return reflection;
	}

	glm::vec4 ComputePlanarReflectionClipPlane(const PlanarReflectionPlane& plane)
	{
		return glm::vec4(plane.Normal, plane.Distance - PlanarReflectionClipBias);
	}

	bool IsPlanarReflectionFacing(const CameraData& camera, const PlanarReflectionPlane& plane)
	{
		if (camera.ProjectionKind == RenderProjection::Orthographic)
		{
			// The camera's local -Z in world space: the negated third row of View's rotation (View is rigid).
			const glm::vec3 viewDirection = -glm::vec3(camera.View[0][2], camera.View[1][2], camera.View[2][2]);
			return glm::dot(plane.Normal, viewDirection) < 0.0f;
		}
		return plane.SignedDistance(camera.Position) > 0.0f;
	}

	CameraData MakePlanarReflectionCamera(const CameraData& camera, const PlanarReflectionPlane& plane, uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width >= 1 && height >= 1, "A planar reflection's size is at least 1 x 1 (got {} x {})", width, height);
		const glm::mat4 reflection = ComputePlanarReflectionMatrix(plane);
		glm::mat4 flip(1.0f);
		flip[0][0] = -1.0f;
		CameraData mirror = camera;
		mirror.View = flip * camera.View * reflection;
		mirror.Position = glm::vec3(reflection * glm::vec4(camera.Position, 1.0f));
		mirror.ViewportWidth = width;
		mirror.ViewportHeight = height;
		mirror.PreviousViewProjection.reset();
		mirror.Jitter = glm::vec2(0.0f);
		mirror.DeltaTime = 0.0f;
		return mirror;
	}

	glm::uvec2 ComputePlanarReflectionSize(uint32_t sceneWidth, uint32_t sceneHeight, float resolutionScale)
	{
		const float scale = std::isfinite(resolutionScale)
			? std::clamp(resolutionScale, MinPlanarReflectionResolutionScale, MaxPlanarReflectionResolutionScale)
			: DefaultPlanarReflectionResolutionScale;
		const auto scaled = [scale](uint32_t size)
		{
			const double value = std::ceil(static_cast<double>(size) * static_cast<double>(scale));
			return std::max(1U, static_cast<uint32_t>(value));
		};
		return glm::uvec2(scaled(sceneWidth), scaled(sceneHeight));
	}

	uint32_t ComputePlanarReflectionMipCount(uint32_t width, uint32_t height, float ssrMaxRoughness)
	{
		uint32_t full = 1;
		for (uint32_t extent = std::max(width, height); extent > 1; extent /= 2)
			++full;
		if (!std::isfinite(ssrMaxRoughness) || !(ssrMaxRoughness > 0.0f))
			return 1;
		const double roughness = std::min(static_cast<double>(ssrMaxRoughness), 1.0);
		const double footprint = roughness * roughness * static_cast<double>(height) * static_cast<double>(PlanarReflectionBlurScale);
		if (!(footprint > 1.0))
			return 1;
		const double levels = std::ceil(std::log2(footprint)) + 1.0;
		return levels < static_cast<double>(full) ? static_cast<uint32_t>(levels) : full;
	}

	glm::uvec2 ComputePlanarReflectionBlurMargin(uint32_t width, uint32_t height, uint32_t mipCount)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0 && mipCount > 0, "a planar reflection has a positive size and at least one level");
		const uint32_t shift = std::min(mipCount - 1, 31U);
		const auto margin = [shift](uint32_t size)
		{
			const uint32_t last = std::max(1U, size >> shift);
			return 2 * (size / last + (size % last != 0 ? 1U : 0U));
		};
		return glm::uvec2(margin(width), margin(height));
	}

	PlanarReflectionScreenRect WidenPlanarReflectionScreenRect(const PlanarReflectionScreenRect& rect, uint32_t width, uint32_t height, const glm::uvec2& margin)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "a planar reflection has a positive size");
		const glm::vec2 widening(static_cast<float>(2.0 * margin.x / width), static_cast<float>(2.0 * margin.y / height));
		return PlanarReflectionScreenRect{ .Min = glm::max(rect.Min - widening, glm::vec2(-1.0f)), .Max = glm::min(rect.Max + widening, glm::vec2(1.0f)) };
	}

	std::optional<PlanarReflectionScreenRect> ComputePlanarReflectionScreenRect(const Aabb& bounds, const glm::mat4& world, const CameraData& camera)
	{
		if (bounds.IsEmpty())
			return std::nullopt;
		const glm::mat4 worldViewProjection = camera.Projection * camera.View * world;
		glm::vec2 min(std::numeric_limits<float>::infinity());
		glm::vec2 max(-std::numeric_limits<float>::infinity());
		uint32_t behind = 0;
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			const glm::vec4 clip = worldViewProjection * Utils::GetBoxCorner(bounds, corner);
			if (!(clip.w > Utils::MinScreenRectClipW))
			{
				++behind;
				continue;
			}
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			min = glm::min(min, ndc);
			max = glm::max(max, ndc);
		}
		if (behind == 8)
			return std::nullopt;
		PlanarReflectionScreenRect rect;
		if (behind == 0)
		{
			rect.Min = glm::max(min, glm::vec2(-1.0f));
			rect.Max = glm::min(max, glm::vec2(1.0f));
		}
		if (!(rect.Min.x < rect.Max.x) || !(rect.Min.y < rect.Max.y))
			return std::nullopt;
		return rect;
	}

	PlanarReflectionScreenRect MirrorPlanarReflectionScreenRect(const PlanarReflectionScreenRect& rect)
	{
		return PlanarReflectionScreenRect{ .Min = glm::vec2(-rect.Max.x, rect.Min.y), .Max = glm::vec2(-rect.Min.x, rect.Max.y) };
	}

	CameraData MakePlanarReflectionCullCamera(const CameraData& reflectionCamera, const PlanarReflectionScreenRect& mainRect)
	{
		const PlanarReflectionScreenRect rect = MirrorPlanarReflectionScreenRect(mainRect);
		ENGINE_CORE_ASSERT(rect.Min.x < rect.Max.x && rect.Min.y < rect.Max.y, "A planar reflection's screen rectangle has an area");
		const glm::vec2 scale = 2.0f / (rect.Max - rect.Min);
		const glm::vec2 centre = 0.5f * (rect.Min + rect.Max);
		// Clip-space x' = scale.x * (x - centre.x * w), and the same for y: the rectangle becomes the whole clip volume.
		glm::mat4 crop(1.0f);
		crop[0][0] = scale.x;
		crop[1][1] = scale.y;
		crop[3][0] = -scale.x * centre.x;
		crop[3][1] = -scale.y * centre.y;
		CameraData cull = reflectionCamera;
		cull.Projection = crop * reflectionCamera.Projection;
		return cull;
	}

	bool IsBehindPlanarReflectionClipPlane(const Aabb& bounds, const glm::mat4& world, const glm::vec4& clipPlane)
	{
		if (bounds.IsEmpty())
			return false;
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			const glm::vec4 point = world * Utils::GetBoxCorner(bounds, corner);
			if (!(glm::dot(clipPlane, glm::vec4(glm::vec3(point), 1.0f)) < 0.0f))
				return false;
		}
		return true;
	}

	glm::vec4 ComputeCascadeDepthPlane(const CameraData& camera)
	{
		return -glm::vec4(camera.View[0][2], camera.View[1][2], camera.View[2][2], camera.View[3][2]);
	}

	PlanarReflectionSelection RankPlanarReflections(std::span<const PlanarReflectionCandidate> candidates, uint32_t cap)
	{
		ENGINE_CORE_ASSERT(cap <= MaxPlanarReflectionsPerView, "At most {} planar reflections a view (got a cap of {})", MaxPlanarReflectionsPerView, cap);
		std::vector<PlanarReflectionCandidate> ranked(candidates.begin(), candidates.end());
		std::ranges::sort(ranked, &Utils::RanksBefore);
		PlanarReflectionSelection selection;
		const size_t kept = std::min(ranked.size(), static_cast<size_t>(cap));
		selection.Slots.reserve(kept);
		for (size_t index = 0; index < kept; ++index)
			selection.Slots.push_back(ranked[index].Item);
		selection.Dropped = static_cast<uint32_t>(ranked.size() - kept);
		return selection;
	}

}
