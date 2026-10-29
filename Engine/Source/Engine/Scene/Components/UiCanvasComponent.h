#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

namespace Engine {

	// Registry enum "UiScaleMode": how a canvas maps its units to scene pixels (UiLayout.h).
	enum class UiScaleMode : uint8_t
	{
		ConstantPixelSize, // ScaleFactor scene pixels per canvas unit, whatever the scene resolution
		MatchHeight        // the canvas is ReferenceHeight units high: scene height / ReferenceHeight scene pixels per unit
	};

	// Registry name "UiCanvas" (M22, C22.1, R-UI-CORE; Docs/Decisions/0033-m22-contract.md decisions 2 and 4): the root of a
	// UI tree, laid out over the whole scene image and drawn in the Ui pass after game Text, before the output stage's
	// upscale (Architecture §8.3). The UI elements below it (descendants with a UiRect, down to the next canvas) belong to it.
	// A canvas has no UiRect of its own (the two components exclude each other): its rectangle is the scene image, (0, 0) to the scene
	// resolution in scene pixels, x right and y down, which is Size = scene resolution / Scale canvas units. Canvases draw in
	// ascending SortOrder, ties in canonical entity order, each after the ones before it. The entity's Transform is ignored
	// (Screen canvases; C22.3's world canvases take it).
	struct UiCanvasComponent
	{
		int32_t SortOrder = 0;
		UiScaleMode ScaleMode = UiScaleMode::ConstantPixelSize;
		float ScaleFactor = 1.0f;        // ConstantPixelSize: scene pixels per canvas unit, 0.01 to 100
		float ReferenceHeight = 1080.0f; // MatchHeight: the canvas's height in canvas units, 1 to 100,000
		// Every laid-out rectangle's corners round to whole scene pixels (floor(v + 0.5)), so images and clips land on pixel
		// corners (a 1:1 image reproduces its texels).
		bool PixelSnap = true;
	};

}
