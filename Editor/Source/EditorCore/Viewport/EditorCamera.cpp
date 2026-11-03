#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorCamera.h"

#include "EditorCore/Viewport/EditorViewportState.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Engine {

	namespace Utils {

		// The input rules Navigate and Navigate2D share: finite pointer delta and wheel, nonnegative finite time and speed,
		// fly axes finite and in [-1, 1].
		static Status ValidateEditorCameraInput(const EditorCameraInput& input)
		{
			if (!std::isfinite(input.PointerDelta.x) || !std::isfinite(input.PointerDelta.y)
				|| !std::isfinite(input.Wheel) || !std::isfinite(input.DeltaSeconds) || input.DeltaSeconds < 0.0f
				|| !std::isfinite(input.MoveSpeed) || input.MoveSpeed < 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "Camera navigation needs finite input and nonnegative time and speed");
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(input.FlyAxis[axis]) || std::abs(input.FlyAxis[axis]) > 1.0f)
					return MakeError(ErrorCode::InvalidArgument, "Camera fly axes must be finite and in [-1,1]");
			}
			return {};
		}

		// The 2D camera's zoom range: its orthographic half height in metres.
		constexpr double MinEditorCamera2DSize = 0.01;
		constexpr double MaxEditorCamera2DSize = 100000.0;
		// The 2D camera's zoom factor per wheel notch.
		constexpr double EditorCamera2DZoomStep = 1.2;

	}

	Result<ExplicitRenderCamera> EditorCamera::Navigate(const ExplicitRenderCamera& camera, const EditorCameraInput& input)
	{
		EditorViewportState validator;
		ENGINE_TRY(validator.SetCamera(camera));
		ENGINE_TRY(Utils::ValidateEditorCameraInput(input));
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

	Result<ExplicitRenderCamera> EditorCamera::Make2D(const ExplicitRenderCamera& camera)
	{
		EditorViewportState validator;
		ENGINE_TRY(validator.SetCamera(camera));
		ExplicitRenderCamera candidate = camera;
		if (camera.Projection == RenderProjection::Perspective)
		{
			// The half height the perspective view shows at its target, so the 2D view keeps about the same scale.
			const double distance = glm::length(glm::dvec3(camera.Target) - glm::dvec3(camera.Position));
			const double tangent = std::tan(static_cast<double>(camera.VerticalFov) * std::numbers::pi / 360.0);
			candidate.OrthographicSize = static_cast<float>(distance * tangent);
		}
		candidate.Projection = RenderProjection::Orthographic;
		candidate.Target = glm::vec3(camera.Target.x, camera.Target.y, 0.0f);
		candidate.Position = glm::vec3(camera.Target.x, camera.Target.y, Make2DDistance);
		candidate.NearClip = 0.1f;
		candidate.FarClip = 2.0f * Make2DDistance;
		ENGINE_TRY(validator.SetCamera(candidate));
		return candidate;
	}

	Result<ExplicitRenderCamera> EditorCamera::Navigate2D(const ExplicitRenderCamera& camera, const EditorCameraInput& input, float viewHeight)
	{
		ENGINE_TRY(Utils::ValidateEditorCameraInput(input));
		if (!std::isfinite(viewHeight) || viewHeight < 1.0f)
			return MakeError(ErrorCode::InvalidArgument, "2D camera navigation needs a view at least one pixel high");
		// A camera Make2D made is unchanged by it; any other becomes the 2D camera of its target first, so the result is
		// always a 2D camera.
		ENGINE_TRY_ASSIGN(ExplicitRenderCamera candidate, Make2D(camera));
		if (input.Pan)
		{
			// The content follows the pointer: the view moves against the pointer's motion, by the units the pixels span
			// (+Y up in the world, down on the screen).
			const double unitsPerPixel = 2.0 * static_cast<double>(candidate.OrthographicSize) / static_cast<double>(viewHeight);
			const glm::dvec2 offset(-static_cast<double>(input.PointerDelta.x) * unitsPerPixel, static_cast<double>(input.PointerDelta.y) * unitsPerPixel);
			const glm::dvec2 target = glm::dvec2(candidate.Target.x, candidate.Target.y) + offset;
			candidate.Target = glm::vec3(static_cast<float>(target.x), static_cast<float>(target.y), 0.0f);
			candidate.Position = glm::vec3(candidate.Target.x, candidate.Target.y, Make2DDistance);
		}
		if (input.Wheel != 0.0f)
		{
			// Positive notches zoom in. The exponent is clamped so an extreme wheel value cannot overflow the factor.
			const double factor = std::pow(Utils::EditorCamera2DZoomStep, std::clamp(-static_cast<double>(input.Wheel), -1000.0, 1000.0));
			candidate.OrthographicSize = static_cast<float>(std::clamp(static_cast<double>(candidate.OrthographicSize) * factor,
				Utils::MinEditorCamera2DSize, Utils::MaxEditorCamera2DSize));
		}
		EditorViewportState validator;
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
		// Widen the clip range just enough to contain the framed bounds and keep the rest of the user's range: framing
		// must never clip the surrounding scene, and later zooming out must not clip the framed bounds either.
		const double fittedNear = std::max(0.00001, (distance - depthExtent) * 0.5);
		const double fittedFar = distance + depthExtent + depthPadding;
		candidate.NearClip = static_cast<float>(std::min(static_cast<double>(camera.NearClip), fittedNear));
		candidate.FarClip = static_cast<float>(std::max(static_cast<double>(camera.FarClip), fittedFar));
		ENGINE_TRY(validator.SetCamera(candidate));
		return candidate;
	}

}
