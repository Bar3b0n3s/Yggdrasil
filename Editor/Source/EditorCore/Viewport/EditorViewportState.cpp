#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorViewportState.h"

#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Engine {

	Result<glm::uvec2> EditorViewportState::GetPixelSize(ViewportView view) const
	{
		glm::uvec2 size(0);
		switch (view)
		{
			case ViewportView::Scene: size = m_ScenePixels; break;
			case ViewportView::Game:  size = m_GamePixels; break;
		}
		if (view != ViewportView::Scene && view != ViewportView::Game)
			return MakeError(ErrorCode::InvalidArgument, "Unknown viewport view");
		if (size.x == 0 || size.y == 0)
			return MakeError(ErrorCode::InvalidState, "The viewport image is unavailable");
		return size;
	}

	Status EditorViewportState::SetPixelSize(ViewportView view, const glm::uvec2& size)
	{
		if ((view != ViewportView::Scene && view != ViewportView::Game)
			|| (size.x == 0) != (size.y == 0) || size.x > 8192 || size.y > 8192)
			return MakeError(ErrorCode::InvalidArgument, "Viewport extents require two zero dimensions or dimensions in [1,8192]");
		if (view == ViewportView::Scene)
		{
			m_ScenePixels = size;
			SetSceneSize(size);
		}
		else
		{
			m_GamePixels = size;
		}
		return {};
	}

	void EditorViewportState::SetSceneSize(const glm::uvec2& size)
	{
		if (size.x > 0 && size.y > 0 && size.x <= 8192 && size.y <= 8192)
			m_SceneSize = size;
	}

	Status EditorViewportState::SetGameResolution(const glm::uvec2& size)
	{
		if ((size.x == 0) != (size.y == 0) || size.x > 8192 || size.y > 8192)
			return MakeError(ErrorCode::InvalidArgument, "Game resolution requires two zero dimensions or dimensions in [1,8192]");
		m_GameResolution = size;
		return {};
	}

	std::optional<EditorViewportPixel> ToViewportPixel(const EditorViewportRect& rectangle, const glm::vec2& point)
	{
		if (rectangle.ImageSize.x == 0 || rectangle.ImageSize.y == 0)
			return std::nullopt;
		glm::dvec2 fraction(0.0);
		for (int axis = 0; axis < 2; ++axis)
		{
			if (!std::isfinite(rectangle.Min[axis]) || !std::isfinite(rectangle.Size[axis])
				|| !std::isfinite(point[axis]) || rectangle.Size[axis] <= 0.0f)
				return std::nullopt;
			fraction[axis] = (static_cast<double>(point[axis]) - rectangle.Min[axis]) / rectangle.Size[axis];
			if (fraction[axis] < 0.0 || fraction[axis] >= 1.0)
				return std::nullopt;
		}
		return EditorViewportPixel{
			static_cast<uint32_t>(std::floor(fraction.x * rectangle.ImageSize.x)),
			static_cast<uint32_t>(std::floor(fraction.y * rectangle.ImageSize.y))
		};
	}

	Status EditorViewportState::SetCamera(const ExplicitRenderCamera& camera)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(camera.Position[axis]) || !std::isfinite(camera.Target[axis]) || !std::isfinite(camera.ClearColor[axis]))
				return MakeError(ErrorCode::InvalidArgument, "Camera vectors must be finite");
		}
		const glm::dvec3 direction = glm::dvec3(camera.Target) - glm::dvec3(camera.Position);
		if (glm::length(direction) == 0.0 || (direction.x == 0.0 && direction.z == 0.0))
			return MakeError(ErrorCode::InvalidArgument, "Camera direction must be nonzero and not parallel to +Y");
		if ((camera.Projection != RenderProjection::Perspective && camera.Projection != RenderProjection::Orthographic)
			|| !std::isfinite(camera.VerticalFov) || camera.VerticalFov <= 0.0f || camera.VerticalFov >= 180.0f
			|| !std::isfinite(camera.OrthographicSize) || camera.OrthographicSize <= 0.0f
			|| !std::isfinite(camera.NearClip) || !std::isfinite(camera.FarClip)
			|| camera.NearClip <= 0.0f || camera.FarClip <= camera.NearClip)
			return MakeError(ErrorCode::InvalidArgument, "Camera projection, field of view, size or clip range is invalid");
		m_Camera = camera;
		return {};
	}

	void EditorViewportState::SetOptions(const EditorViewportOptions& options)
	{
		m_Options = options;
	}

	Status EditorViewportState::SetDebugView(RenderDebugView view)
	{
		if (std::to_underlying(view) >= RenderDebugViewCount)
			return MakeError(ErrorCode::InvalidArgument, "Unknown render debug view");
		m_DebugView = view;
		return {};
	}

}
