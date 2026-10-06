#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Engine {

	// Registry enum "TextSpace".
	enum class TextSpace : uint8_t
	{
		Screen,
		World
	};

	// Registry enum "TextAlignment".
	enum class TextAlignment : uint8_t
	{
		Left,
		Center,
		Right
	};

	// Registry name "Text" (Architecture §5.3, §8.10): SDF text, the only UI element in v1. A null Font is the default
	// font. Size in pixels at the 1080p reference height. Anchor is viewport-normalized (Screen space); Pivot is normalized
	// to the text block; Offset in pixels; Billboard applies in World space.
	struct TextComponent
	{
		std::string Text;
		TypedAssetHandle<AssetType::Font> Font;
		float Size = 32.0f;
		glm::vec4 Color = glm::vec4(1.0f); // linear
		TextSpace Space = TextSpace::Screen;
		glm::vec2 Anchor = glm::vec2(0.5f, 0.5f);
		glm::vec2 Pivot = glm::vec2(0.5f, 0.5f);
		glm::vec2 Offset = glm::vec2(0.0f, 0.0f);
		TextAlignment Alignment = TextAlignment::Center;
		bool Billboard = false;
	};

}
