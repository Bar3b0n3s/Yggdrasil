#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Components/TextComponent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Engine {

	// Registry enum "UiTextVerticalAlignment": where a text block sits in its element's rectangle.
	enum class UiTextVerticalAlignment : uint8_t
	{
		Top,
		Middle,
		Bottom
	};

	// Registry name "UiText" (M22, C22.1, R-UI-TEXT; Docs/Decisions/0033-m22-contract.md decisions 2 and 6): draws text in the
	// element's UiRect as signed-distance-field glyphs (the Text pipelines), laid out by the text-measurement layer
	// (Renderer/TextMeasurement.h): lines wrap at the rectangle's width when Wrap is set, align within it by Alignment (the
	// Text component's registry enum) and the block sits in it by VerticalAlignment. A null Font is the default font; a
	// missing one draws with its placeholder. Characters the font lacks are skipped. The text is not clipped by its own
	// rectangle (only by clipping ancestors, UiRect::ClipChildren). Scripts set Text from string tables with Loc.Get.
	struct UiTextComponent
	{
		std::string Text{}; // UTF-8; a line feed starts a new line
		TypedAssetHandle<AssetType::Font> Font{};
		float Size = 32.0f;                // canvas units per em, 1 to 100,000
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha
		TextAlignment Alignment = TextAlignment::Left;
		UiTextVerticalAlignment VerticalAlignment = UiTextVerticalAlignment::Top;
		bool Wrap = true;         // break lines at spaces to fit the rectangle's width (a word wider than it breaks anywhere)
		float LineSpacing = 1.0f; // multiplies the font's line height, 0.1 to 10
	};

}
