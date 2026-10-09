#include "EnginePCH.h"
#include "Engine/Renderer/ShadowCascades.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		template<glm::length_t L>
		[[nodiscard]] static bool IsFiniteShadowVector(const glm::vec<L, double>& value)
		{
			for (glm::length_t component = 0; component < L; ++component)
				if (!std::isfinite(value[component]))
					return false;
			return true;
		}

		[[nodiscard]] static bool IsFiniteShadowMatrix(const glm::dmat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
				if (!IsFiniteShadowVector(matrix[column]))
					return false;
			return true;
		}

		[[nodiscard]] static bool IsShadowMapSize(uint32_t size)
		{
			return size >= 256 && size <= 8192 && (size & (size - 1)) == 0;
		}

		[[nodiscard]] static float RoundShadowRadiusUp(double radius)
		{
			float result = static_cast<float>(radius);
			if (static_cast<double>(result) < radius)
				result = std::nextafter(result, std::numeric_limits<float>::infinity());
			return result;
		}

		// Intersect a homogeneous inverse-projection line with z = -distance. Its far endpoint may have w=0:
		// dehomogenizing the endpoints first would lose infinite-far perspective projections.
		[[nodiscard]] static Result<std::array<glm::dvec3, 8>> ShadowViewCorners(const CameraData& camera, float sliceNear, float sliceFar)
		{
			if (!std::isfinite(sliceNear) || !std::isfinite(sliceFar) || sliceNear <= 0.0f || sliceFar <= sliceNear
				|| !IsFiniteShadowMatrix(glm::dmat4(camera.Projection)) || !IsFiniteShadowMatrix(glm::dmat4(camera.View)))
				return MakeError(ErrorCode::InvalidArgument, "Shadow slice needs finite nonsingular matrices and 0 < near < far");
			const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.Projection));
			if (!IsFiniteShadowMatrix(inverse) || !IsFiniteShadowMatrix(glm::inverse(glm::dmat4(camera.View))))
				return MakeError(ErrorCode::InvalidArgument, "Shadow camera matrix is singular");
			constexpr std::array<glm::dvec2, 4> Coordinates = { glm::dvec2(-1, -1), glm::dvec2(1, -1), glm::dvec2(1, 1),
				glm::dvec2(-1, 1) };
			std::array<glm::dvec3, 8> corners{};
			for (size_t index = 0; index < corners.size(); ++index)
			{
				const glm::dvec2 xy = Coordinates[index % 4];
				const glm::dvec4 nearPoint = inverse * glm::dvec4(xy, 1.0, 1.0);
				const glm::dvec4 delta = inverse * glm::dvec4(xy, 0.0, 1.0) - nearPoint;
				const double distance = index < 4 ? sliceNear : sliceFar;
				const double divisor = delta.z + distance * delta.w;
				if (divisor == 0.0)
					return MakeError(ErrorCode::InvalidArgument, "Shadow projection ray does not cross the slice plane");
				const glm::dvec4 point = nearPoint - delta * ((nearPoint.z + distance * nearPoint.w) / divisor);
				corners[index] = glm::dvec3(point) / point.w;
				if (!IsFiniteShadowVector(corners[index]))
					return MakeError(ErrorCode::InvalidArgument, "Shadow slice corner is not finite");
			}
			return corners;
		}

	}

	Result<std::array<float, MaxShadowCascades>> ComputeCascadeSplits(float nearClip, float farClip, uint32_t count, float lambda)
	{
		if (!std::isfinite(nearClip) || !std::isfinite(farClip) || !std::isfinite(lambda) || nearClip <= 0.0f || farClip <= nearClip
			|| count == 0 || count > MaxShadowCascades || lambda < 0.0f || lambda > 1.0f)
			return MakeError(ErrorCode::InvalidArgument, "Cascade splits need 0 < near < far, 1..4 cascades and lambda in [0,1]");
		std::array<float, MaxShadowCascades> splits{};
		splits.fill(farClip);
		for (uint32_t index = 1; index < count; ++index)
		{
			const double fraction = static_cast<double>(index) / count;
			const double uniform = nearClip + (static_cast<double>(farClip) - nearClip) * fraction;
			const double logarithmic = nearClip * std::pow(static_cast<double>(farClip) / nearClip, fraction);
			splits[index - 1] = static_cast<float>(std::lerp(uniform, logarithmic, static_cast<double>(lambda)));
		}
		return splits;
	}

	Result<std::array<glm::vec3, 8>> ComputeFrustumSliceCorners(const CameraData& camera, float sliceNear, float sliceFar)
	{
		ENGINE_TRY_ASSIGN(auto viewCorners, Utils::ShadowViewCorners(camera, sliceNear, sliceFar));
		const glm::dmat4 inverseView = glm::inverse(glm::dmat4(camera.View));
		std::array<glm::vec3, 8> result{};
		for (size_t index = 0; index < result.size(); ++index)
		{
			const glm::dvec4 world = inverseView * glm::dvec4(viewCorners[index], 1.0);
			const glm::dvec3 position = glm::dvec3(world) / world.w;
			if (!Utils::IsFiniteShadowVector(position) || glm::any(glm::greaterThan(glm::abs(position), glm::dvec3(std::numeric_limits<float>::max()))))
				return MakeError(ErrorCode::InvalidArgument, "Shadow world corner cannot be represented");
			result[index] = glm::vec3(position);
		}
		return result;
	}

	Result<ShadowCascadeSet> BuildShadowCascades(const CameraData& camera, const LightData& light, uint32_t lightIndex,
		uint32_t shadowMapSize)
	{
		const glm::dvec3 direction(light.Direction);
		if (light.Type != RenderLightType::Directional || !Utils::IsFiniteShadowVector(direction) || glm::dot(direction, direction) <= 0.0
			|| !std::isfinite(light.ShadowDistance) || light.ShadowDistance <= camera.NearClip || !std::isfinite(light.LightAngle)
			|| light.LightAngle < 0.0f || light.LightAngle >= 180.0f || !std::isfinite(light.DepthBias) || light.DepthBias < 0.0f
			|| !std::isfinite(light.NormalBias) || light.NormalBias < 0.0f || !std::isfinite(camera.FarClip)
			|| camera.FarClip <= camera.NearClip || !Utils::IsShadowMapSize(shadowMapSize) || camera.ViewportWidth == 0
			|| camera.ViewportHeight == 0
			|| (camera.ProjectionKind != RenderProjection::Perspective && camera.ProjectionKind != RenderProjection::Orthographic))
			return MakeError(ErrorCode::InvalidArgument, "Invalid directional shadow light, camera or map size");
		ENGINE_TRY_ASSIGN(auto splits,
			ComputeCascadeSplits(camera.NearClip, std::min(camera.FarClip, light.ShadowDistance), light.CascadeCount,
				light.CascadeSplitLambda));
		const glm::dmat4 inverseView = glm::inverse(glm::dmat4(camera.View));
		const glm::dvec3 back = -glm::normalize(direction);
		const glm::dvec3 reference = std::abs(back.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
		const glm::dvec3 right = glm::normalize(glm::cross(reference, back));
		const glm::dvec3 up = glm::cross(back, right);
		ShadowCascadeSet result{ .Count = light.CascadeCount, .LightIndex = lightIndex };
		for (uint32_t index = 0; index < result.Count; ++index)
		{
			ShadowCascade& cascade = result.Cascades[index];
			cascade.SplitNear = index == 0 ? camera.NearClip : splits[index - 1];
			cascade.SplitFar = splits[index];
			cascade.BlendStart = std::lerp(cascade.SplitFar, cascade.SplitNear, ShadowCascadeBlendFraction);
			// The next cascade contains the previous cascade's blend region as well as its own logical split.
			const float fittedNear = index == 0 ? cascade.SplitNear : result.Cascades[index - 1].BlendStart;
			ENGINE_TRY_ASSIGN(auto corners, Utils::ShadowViewCorners(camera, fittedNear, cascade.SplitFar));
			ENGINE_TRY_ASSIGN(cascade.Corners, ComputeFrustumSliceCorners(camera, fittedNear, cascade.SplitFar));
			glm::dvec3 viewCentre(0.0);
			for (const glm::dvec3& corner : corners)
				viewCentre += corner / 8.0;
			const glm::dvec4 centreWorld = inverseView * glm::dvec4(viewCentre, 1.0);
			const glm::dvec3 worldCentre = glm::dvec3(centreWorld) / centreWorld.w;
			// Fit offsets, not translated float world corners: radius must not change as the camera translates.
			double radius = 0.0;
			for (const glm::dvec3& corner : corners)
				radius = std::max(radius, glm::length(glm::dvec3(inverseView * glm::dvec4(corner - viewCentre, 0.0))));
			if (!std::isfinite(radius) || radius > std::numeric_limits<float>::max())
				return MakeError(ErrorCode::InvalidArgument, "Shadow sphere radius cannot be represented");
			ENGINE_TRY_ASSIGN(cascade.Radius, ComputeStabilizedCascadeRadius(Utils::RoundShadowRadiusUp(radius), shadowMapSize));
			const double extent = cascade.Radius;
			const double texel = 2.0 * extent / shadowMapSize;
			const auto snap = [texel](double coordinate)
			{
				return std::floor(coordinate / texel + 0.5) * texel;
			};
			const glm::dvec3 centre(snap(glm::dot(right, worldCentre)), snap(glm::dot(up, worldCentre)), snap(glm::dot(back, worldCentre)));
			const double depthRange = 2.0 * extent + light.ShadowDistance;
			glm::dmat4 projection(0.0);
			for (glm::length_t component = 0; component < 3; ++component)
			{
				projection[component][0] = right[component] / extent;
				projection[component][1] = up[component] / extent;
				projection[component][2] = back[component] / depthRange;
			}
			projection[3] = glm::dvec4(-centre.x / extent, -centre.y / extent, (extent - centre.z) / depthRange, 1.0);
			if (!Utils::IsFiniteShadowMatrix(projection) || depthRange > std::numeric_limits<float>::max())
				return MakeError(ErrorCode::InvalidArgument, "Directional shadow depth range cannot be represented");
			for (glm::length_t column = 0; column < 4; ++column)
				if (glm::any(glm::greaterThan(glm::abs(projection[column]), glm::dvec4(std::numeric_limits<float>::max()))))
					return MakeError(ErrorCode::InvalidArgument, "Directional shadow matrix cannot be represented");
			cascade.ViewProjection = glm::mat4(projection);
			cascade.TexelWorldSize = static_cast<float>(texel);
			cascade.LightNear = 0.0f;
			cascade.LightFar = static_cast<float>(depthRange);
			cascade.PenumbraUvPerMetre =
				static_cast<float>(std::tan(glm::radians(static_cast<double>(light.LightAngle)) * 0.5) / (2.0 * extent));
			if (!Utils::IsFiniteShadowMatrix(glm::dmat4(cascade.ViewProjection)) || !std::isfinite(cascade.LightFar)
				|| !std::isfinite(cascade.PenumbraUvPerMetre))
				return MakeError(ErrorCode::InvalidArgument, "Directional shadow transform cannot be represented");
		}
		return result;
	}

	Result<float> ComputeStabilizedCascadeRadius(float sphereRadius, uint32_t shadowMapSize)
	{
		if (!std::isfinite(sphereRadius) || sphereRadius <= 0.0f || !Utils::IsShadowMapSize(shadowMapSize))
			return MakeError(ErrorCode::InvalidArgument,
				"Shadow sphere needs a positive finite radius and power-of-two map size in [256,8192]");
		const double radius = std::ceil(16.0 * sphereRadius / (1.0 - 1.0 / shadowMapSize)) / 16.0;
		if (radius > std::numeric_limits<float>::max())
			return MakeError(ErrorCode::InvalidArgument, "Stabilized shadow radius cannot be represented");
		return Utils::RoundShadowRadiusUp(radius);
	}

}
