#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "UiScaleMode": how a canvas maps its units to scene pixels (UiLayout.h).
	enum class UiScaleMode : uint8_t
	{
		ConstantPixelSize, // ScaleFactor scene pixels per canvas unit, whatever the scene resolution
		MatchHeight        // the canvas is ReferenceHeight units high: scene height / ReferenceHeight scene pixels per unit
	};

	// Registry enum "UiCanvasRenderMode" (M22, C22.3, R-UI-WORLD; Docs/Decisions/0033-m22-contract.md decision 49): where a
	// canvas is drawn.
	enum class UiCanvasRenderMode : uint8_t
	{
		Screen, // over the scene image, after game Text (the default; every canvas before C22.3)
		World   // a rectangle in the world at the entity's transform, depth-tested against the scene
	};

	// Registry struct "UiTheme" (M22, C22.2, R-UI-THEME; Docs/Decisions/0033-m22-contract.md decision 25): the state tints of
	// a canvas's widgets (UiButton, UiToggle, UiSlider, UiScrollView). The tint of a widget's state multiplies the colour of
	// the widget element's own UiImage when it is drawn (Scene/UiExtraction.h), never its texts or its children's images;
	// a widget without a UiImage shows no tint. The state, first match: Disabled (Interactable false), Pressed (the pointer
	// holds it), Focused, Hovered, else Normal. Linear RGB with straight alpha, each component 0 to 1. Outside a play session
	// (the edit scene, its screenshots and the UI preview) every widget is Normal, or Disabled when not interactable.
	struct UiTheme
	{
		glm::vec4 Normal = glm::vec4(1.0f);
		glm::vec4 Hovered = glm::vec4(0.85f, 0.85f, 0.85f, 1.0f);
		glm::vec4 Pressed = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
		glm::vec4 Focused = glm::vec4(0.75f, 0.85f, 1.0f, 1.0f);
		glm::vec4 Disabled = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);

		bool operator==(const UiTheme&) const = default;
	};

	// Registry name "UiCanvas" (M22, C22.1, R-UI-CORE; Docs/Decisions/0033-m22-contract.md decisions 2 and 4): the root of a
	// UI tree, laid out over the whole scene image and drawn in the Ui pass after game Text, before the output stage's
	// upscale (Architecture §8.3). The UI elements below it (descendants with a UiRect, down to the next canvas) belong to it.
	// A canvas has no UiRect of its own (the two components exclude each other): its rectangle is the scene image, (0, 0) to the scene
	// resolution in scene pixels, x right and y down, which is Size = scene resolution / Scale canvas units. Canvases draw in
	// ascending SortOrder, ties in canonical entity order, each after the ones before it. The entity's Transform is ignored
	// (Screen canvases; C22.3's World canvases take it, below).
	// C22.2 (decisions 23 and 25) adds the canvas's theme, Font (the font of its UiTexts whose Font is null) and Theme (its
	// widgets' state tints), and DefaultFocus, where keyboard and gamepad navigation starts; stream C registers the three
	// fields after PixelSnap, in that order, with the UiTheme struct.
	struct UiCanvasComponent
	{
		int32_t SortOrder = 0;
		UiScaleMode ScaleMode = UiScaleMode::ConstantPixelSize;
		float ScaleFactor = 1.0f;        // ConstantPixelSize: scene pixels per canvas unit, 0.01 to 100
		float ReferenceHeight = 1080.0f; // MatchHeight: the canvas's height in canvas units, 1 to 100,000
		// Every laid-out rectangle's corners round to whole scene pixels (floor(v + 0.5)), so images and clips land on pixel
		// corners (a 1:1 image reproduces its texels).
		bool PixelSnap = true;
		// C22.2: the font of every UiText of the canvas whose own Font is null (an element below a nested canvas takes that
		// canvas's); null: the default font.
		TypedAssetHandle<AssetType::Font> Font{};
		// C22.2: the state tints of the canvas's widgets (UiTheme).
		UiTheme Theme{};
		// C22.2 (decision 23): EntityRef, the widget a navigation command focuses while nothing has focus (the last canvas in
		// draw order whose DefaultFocus is focusable names it, Scene/UiInteraction.h); null, or an element that is not
		// focusable: navigation starts only from a pointer press on a focusable widget or Ui.SetFocus.
		UUID DefaultFocus{};
		// C22.3 (decision 49; registered by stream W after DefaultFocus, in this order, with the UiCanvasRenderMode enum): a
		// World canvas is laid out over WorldWidth x WorldHeight canvas pixels instead of the scene image (its "scene
		// resolution": the scale modes apply to it, MatchHeight reading WorldHeight), and drawn as a rectangle of that size
		// over PixelsPerUnit (canvas pixels per world unit) in the entity's local XY plane, centred on its origin, x right and
		// y up (canvas y down maps to local -Y), facing +Z like a world Text, transformed by the entity's world matrix (its
		// scale included), depth-tested against the scene without depth writes, in every view that draws the world (the
		// editor's scene view included). Its elements take the pointer along a ray through the primary camera
		// (Scene/UiInteraction.h). Screen canvases ignore the three.
		UiCanvasRenderMode RenderMode = UiCanvasRenderMode::Screen;
		uint32_t WorldWidth = 1024;   // canvas pixels, 1 to 16,384
		uint32_t WorldHeight = 576;   // canvas pixels, 1 to 16,384
		float PixelsPerUnit = 512.0f; // canvas pixels per world unit, 0.001 to 1,000,000
	};

}
