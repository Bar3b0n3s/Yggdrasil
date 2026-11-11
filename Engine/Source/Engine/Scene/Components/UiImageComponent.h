#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "UiImageType": how an image fills its element's rectangle.
	enum class UiImageType : uint8_t
	{
		Simple, // the frame stretched over the rectangle
		Sliced  // the frame's 9-slice borders (SpriteFrame::Borders) keep their size, the edges and the centre stretch
	};

	// Registry enum "UiImageFilter": how an image samples its texture (the QuadBatcher's samplers, both clamp to edge).
	enum class UiImageFilter : uint8_t
	{
		Point, // nearest texel: pixel art, and a 1:1 image reproduces its texels exactly
		Linear // bilinear
	};

	// Registry name "UiImage" (M22, C22.1, R-UI-RENDER; Docs/Decisions/0033-m22-contract.md decisions 2 and 7): draws a frame
	// of a sprite sheet (M18's SpriteSheet asset) over the element's UiRect, tinted by Color, through the QuadBatcher's Ldr
	// pipeline. A null Sheet draws a plain rectangle of Color (the QuadBatcher's white texel). A missing or failed sheet, or
	// a Frame the sheet does not have, draws nothing (the Ui pass counts it; project.validate reports
	// UI_IMAGE_FRAME_OUT_OF_RANGE). On an entity with a UiText too, the image draws first. On a widget's element (UiButton,
	// UiToggle, UiSlider, UiScrollView) its Color is multiplied by the tint of the widget's state (UiTheme, C22.2).
	struct UiImageComponent
	{
		TypedAssetHandle<AssetType::SpriteSheet> Sheet{};
		uint32_t Frame = 0;                // an index into the sheet's Frames, 0 to 65,535
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha; multiplies the texels
		UiImageType Type = UiImageType::Simple;
		// Sliced: the canvas units each border texel takes (times the canvas's scale in scene pixels), 0.01 to 1000; borders
		// wider than the rectangle shrink in proportion (ExpandNineSlice).
		float BorderScale = 1.0f;
		UiImageFilter Filter = UiImageFilter::Point;
		// C22.2 (Docs/Decisions/0033-m22-contract.md decision 22): the element is a pointer target (Scene/UiInteraction.h's
		// hit test): the pointer over it reaches its nearest widget at or above it, and is over the UI (Ui.IsPointerOverUi).
		// False lets the pointer pass through the image to what lies under it. Registered by stream A, after Filter.
		bool RaycastTarget = true;
	};

}
