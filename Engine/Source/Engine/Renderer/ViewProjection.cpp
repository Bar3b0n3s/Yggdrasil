#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include "Engine/Core/Assert.h"

#include <array>
#include <cmath>
#include <limits>

namespace Engine {

	namespace {

		bool IsFiniteViewVector(const glm::dvec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

	}

	Result<RenderRay> ComputeViewPixelRay(const CameraData& camera, uint32_t x, uint32_t y)
	{
		if (camera.ViewportWidth == 0 || camera.ViewportHeight == 0 || x >= camera.ViewportWidth || y >= camera.ViewportHeight)
			return MakeError(ErrorCode::InvalidArgument, "pixel is outside the camera framebuffer");
		if (!std::isfinite(camera.NearClip) || !std::isfinite(camera.FarClip) || camera.NearClip <= 0 || camera.FarClip <= camera.NearClip
			|| !IsFiniteViewVector(glm::dvec4(camera.Position, 1)))
			return MakeError(ErrorCode::InvalidArgument, "camera position and clip range must be finite and ordered");
		if (camera.ProjectionKind != RenderProjection::Perspective && camera.ProjectionKind != RenderProjection::Orthographic)
			return MakeError(ErrorCode::InvalidArgument, "unknown camera projection kind");
		for (int column = 0; column < 4; ++column)
			if (!IsFiniteViewVector(camera.View[column]) || !IsFiniteViewVector(camera.Projection[column]))
				return MakeError(ErrorCode::InvalidArgument, "camera matrices must be finite");
		const glm::dmat4 view(camera.View);
		const glm::dmat4 projection(camera.Projection);
		if (glm::determinant(view) == 0 || glm::determinant(projection) == 0)
			return MakeError(ErrorCode::InvalidArgument, "camera matrices must be invertible");
		const glm::dmat4 inverseView = glm::inverse(view);
		const glm::dmat4 inverseProjection = glm::inverse(projection);
		const double ndcX = 2 * (static_cast<double>(x) + 0.5) / camera.ViewportWidth - 1;
		const double ndcY = 1 - 2 * (static_cast<double>(y) + 0.5) / camera.ViewportHeight;
		const glm::dvec4 nearPoint = inverseProjection * glm::dvec4(ndcX, ndcY, 1, 1);
		if (!IsFiniteViewVector(nearPoint) || nearPoint.w == 0)
			return MakeError(ErrorCode::InvalidArgument, "camera near plane cannot be unprojected");
		const glm::dvec3 nearView = glm::dvec3(nearPoint) / nearPoint.w;
		glm::dvec4 origin(camera.Position, 1);
		glm::dvec3 direction(0);
		if (camera.ProjectionKind == RenderProjection::Perspective)
			direction = glm::dmat3(inverseView) * nearView;
		else
		{
			origin = inverseView * glm::dvec4(nearView, 1);
			direction = glm::dmat3(inverseView) * glm::dvec3(0, 0, -1);
		}
		const double length = glm::length(direction);
		if (!IsFiniteViewVector(origin) || origin.w == 0 || !std::isfinite(length) || length <= 0)
			return MakeError(ErrorCode::InvalidArgument, "camera ray is not finite");
		direction /= length;
		if (!(-(view * glm::dvec4(direction, 0)).z > 0))
			return MakeError(ErrorCode::InvalidArgument, "camera ray must point into the view");
		const glm::dvec3 position = glm::dvec3(origin) / origin.w;
		for (int axis = 0; axis < 3; ++axis)
			if (!std::isfinite(position[axis]) || std::abs(position[axis]) > std::numeric_limits<float>::max())
				return MakeError(ErrorCode::InvalidArgument, "camera ray origin is not representable");
		return RenderRay{ glm::vec3(position), glm::vec3(direction) };
	}

	Result<RenderRayInterval> ComputeViewPixelRayInterval(const CameraData& camera, uint32_t x, uint32_t y)
	{
		ENGINE_TRY_ASSIGN(const RenderRay ray, ComputeViewPixelRay(camera, x, y));
		double minimum = 0;
		double maximum = static_cast<double>(camera.FarClip) - camera.NearClip;
		if (camera.ProjectionKind == RenderProjection::Perspective)
		{
			const double forward = -(glm::dmat4(camera.View) * glm::dvec4(glm::normalize(glm::dvec3(ray.Direction)), 0)).z;
			minimum = camera.NearClip / forward;
			maximum = camera.FarClip / forward;
		}
		if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum > std::numeric_limits<float>::max()
			|| minimum < 0 || static_cast<float>(minimum) >= static_cast<float>(maximum))
			return MakeError(ErrorCode::InvalidArgument, "camera ray clipping interval is not representable");
		return RenderRayInterval{ ray, static_cast<float>(minimum), static_cast<float>(maximum) };
	}

	glm::vec3 GetOverdrawDebugColor(uint32_t count)
	{
		constexpr std::array<glm::vec3, 6> Colors{ glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 1),
			glm::vec3(0, 1, 0), glm::vec3(1, 1, 0), glm::vec3(1, 0, 0) };
		return Colors[count < Colors.size() ? count : Colors.size() - 1];
	}

	glm::vec3 GetShadowCascadeDebugColor(uint32_t cascadeIndex)
	{
		constexpr std::array<glm::vec3, 4> Colors{ glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 1, 0) };
		ENGINE_CORE_ASSERT(cascadeIndex < Colors.size(), "invalid shadow cascade index");
		return cascadeIndex < Colors.size() ? Colors[cascadeIndex] : glm::vec3(0);
	}

}
