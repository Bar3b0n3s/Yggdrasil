#include "EnginePCH.h"
#include "Engine/Renderer/TextLayout.h"

#include "Engine/Asset/FontData.h"

namespace Engine {

	TextLayoutResult LayoutText(const FontData& /*font*/, std::string_view /*text*/, RenderTextAlignment /*alignment*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::vec2 PlaceScreenText(const TextItem& /*item*/, const glm::vec2& /*blockSize*/, uint32_t /*viewportWidth*/, uint32_t /*viewportHeight*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

}
