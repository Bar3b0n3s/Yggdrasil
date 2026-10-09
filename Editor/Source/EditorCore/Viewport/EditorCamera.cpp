#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorCamera.h"

#include "EditorCore/Viewport/EditorViewportState.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Engine {

	Result<ExplicitRenderCamera> EditorCamera::Navigate(const ExplicitRenderCamera& camera, const EditorCameraInput& input)
	{
		EditorViewportState validator;
		ENGINE_TRY(validator.SetCamera(camera));
		if (!std::isfinite(input.PointerDelta.x) || !std::isfinite(input.PointerDelta.y)
			|| !std::isfinite(input.Wheel) || !std::isfinite(input.DeltaSeconds) || input.DeltaSeconds < 0.0f
			|| !std::isfinite(input.MoveSpeed) || input.MoveSpeed < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "Camera navigation needs finite input and nonnegative time and speed");
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(input.FlyAxis[axis]) || std::abs(input.FlyAxis[axis]) > 1.0f)
				return MakeError(ErrorCode::InvalidArgument, "Camera fly axes must be finite and in [-1,1]");
		}
		ExplicitRenderCamera candidate = camera;
		glm::dvec3 position(camera.Position);
		glm::dvec3 target(camera.Target);
		const double distance = glm::length(target - position);
		glm::dvec3 forward = (target - position) / distance;
		if (input.Orbit || input.Fly)
		{
			const double yaw = std::atan2(forward.x, -forward.z) + static_cast<double>(input.PointerDelta.x) * 0.005;
			const double pitch = std::clamp(std::asin(std::clamp(forward.y, -1.0, 1.0)) - static_cast<double>(input.PointerDelta.y) * 0.005,
				-std::numbers::pi / 2.0 + 0.01, std::numbers::pi / 2.0 - 0.01);
			forward = glm::dvec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
			if (input.Orbit)
				position = target - forward * distance;
			else
				target = position + forward * distance;
		}
		const glm::dvec3 right = glm::normalize(glm::cross(forward, glm::dvec3(0.0, 1.0, 0.0)));
		const glm::dvec3 up = glm::cross(right, forward);
		glm::dvec3 offset(0.0);
		if (input.Pan)
		{
			const double scale = camera.Projection == RenderProjection::Orthographic ? camera.OrthographicSize : distance;
			offset += (-right * static_cast<double>(input.PointerDelta.x) + up * static_cast<double>(input.PointerDelta.y)) * scale * 0.002;
		}
		if (input.Fly)
		{
			glm::dvec3 axes(input.FlyAxis);
			if (glm::length(axes) > 1.0)
				axes = glm::normalize(axes);
			offset += (right * axes.x + glm::dvec3(0.0, 1.0, 0.0) * axes.y + forward * axes.z)
				* static_cast<double>(input.MoveSpeed) * static_cast<double>(input.DeltaSeconds);
		}
		position += offset;
		target += offset;
		if (input.Wheel != 0.0f)
		{
			const double factor = std::exp(std::clamp(-static_cast<double>(input.Wheel) * 0.15, -50.0, 50.0));
			if (camera.Projection == RenderProjection::Orthographic)
				candidate.OrthographicSize = static_cast<float>(std::clamp(camera.OrthographicSize * factor, 0.0001, 1.0e20));
			else
				position = target - forward * std::clamp(distance * factor, 0.0001, 1.0e20);
		}
		candidate.Position = glm::vec3(position);
		candidate.Target = glm::vec3(target);
		ENGINE_TRY(validator.SetCamera(candidate));
		return candidate;
	}

	Result<ExplicitRenderCamera> EditorCamera::FrameBounds(const ExplicitRenderCamera& camera, const glm::vec3& minimum,
		const glm::vec3& maximum, float aspect)
	{
		EditorViewportState validator;
		ENGINE_TRY(validator.SetCamera(camera));
		if (!std::isfinite(aspect) || aspect <= 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "Framing needs a finite positive aspect ratio");
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis] > maximum[axis])
				return MakeError(ErrorCode::InvalidArgument, "Framing bounds must be finite and ordered");
		}
		const glm::dvec3 center = (glm::dvec3(minimum) + glm::dvec3(maximum)) * 0.5;
		const glm::dvec3 half = (glm::dvec3(maximum) - glm::dvec3(minimum)) * 0.5;
		const glm::dvec3 forward = glm::normalize(glm::dvec3(camera.Target) - glm::dvec3(camera.Position));
		const glm::dvec3 right = glm::normalize(glm::cross(forward, glm::dvec3(0.0, 1.0, 0.0)));
		const glm::dvec3 up = glm::cross(right, forward);
		const double tangent = std::tan(static_cast<double>(camera.VerticalFov) * std::numbers::pi / 360.0);
		double distance = 0.0;
		double halfHeight = 0.0001;
		double depthExtent = 0.0;
		for (int corner = 0; corner < 8; ++corner)
		{
			const glm::dvec3 offset = half * glm::dvec3((corner & 1) ? 1.0 : -1.0, (corner & 2) ? 1.0 : -1.0, (corner & 4) ? 1.0 : -1.0);
			const double x = std::abs(glm::dot(offset, right)) * 1.1;
			const double y = std::abs(glm::dot(offset, up)) * 1.1;
			const double z = glm::dot(offset, forward);
			depthExtent = std::max(depthExtent, std::abs(z));
			halfHeight = std::max({ halfHeight, y, x / aspect });
			distance = std::max({ distance, y / tangent - z, x / (tangent * aspect) - z });
		}
		if (half == glm::dvec3(0.0))
		{
			// A point frames a half-metre sphere. Use the narrower padded field of view in portrait views too.
			const double limitingTangent = tangent * std::min(1.0, static_cast<double>(aspect)) / 1.1;
			distance = 0.5 * std::sqrt(1.0 + limitingTangent * limitingTangent) / limitingTangent;
			halfHeight = 0.55 * std::max(1.0, 1.0 / aspect);
			depthExtent = 0.5;
		}
		const double depthPadding = std::max(0.1, glm::length(half) * 0.1);
		distance = std::max(distance, depthExtent + depthPadding);
		if (camera.Projection == RenderProjection::Orthographic)
			distance = depthExtent + std::max(halfHeight, depthPadding);
		ExplicitRenderCamera candidate = camera;
		candidate.Target = glm::vec3(center);
		candidate.Position = glm::vec3(center - forward * distance);
		candidate.OrthographicSize = static_cast<float>(halfHeight);
		candidate.NearClip = static_cast<float>(std::max(0.00001, (distance - depthExtent) * 0.5));
		candidate.FarClip = static_cast<float>(distance + depthExtent + depthPadding);
		ENGINE_TRY(validator.SetCamera(candidate));
		return candidate;
	}

}
