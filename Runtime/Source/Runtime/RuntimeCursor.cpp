#include "Runtime/RuntimeCursor.h"

#include "Engine/Session/PlaySession.h"

namespace Engine {

	std::optional<glm::vec2> MapRuntimeCursorToView(const glm::vec2& windowPosition, const glm::vec2& windowSize, const glm::uvec2& imageSize,
		const std::optional<glm::uvec2>& fixedViewSize, const RenderOutputSettings& output)
	{
		return MapImageToViewCoordinates(windowPosition, glm::vec2(0.0f), windowSize, imageSize, fixedViewSize.value_or(imageSize), output);
	}

}
