#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <vector>

// The UI draw list of one view (M22, C22.1, R-UI-RENDER; Docs/Decisions/0033-m22-contract.md decision 7): the render
// snapshot's Ui member (RenderSnapshot.h). Scene's render extraction fills it from the scene's UI canvases, laid out in scene
// pixels at the snapshot's scene resolution (Scene/UiLayout.h, Scene/UiExtraction.h); the renderer's Ui pass
// (Renderer/UiPass.h) draws it in the SceneOverlay stage after game Text and before the output stage's upscale, so UI is part
// of the game's image, upscaled with it and under the editor overlays (Docs/Decisions/0029-m18-contract.md decision 8).
//
// Coordinates are scene pixels: x right, y down, origin at the scene image's top-left corner, the scene resolution's size;
// they are also the pixels of the scene-resolution framebuffer the Ui pass draws into. Like the rest of the snapshot, the list
// is plain data that names assets by handle (a sheet and its frame, a font): extraction never touches assets, and the Ui pass
// resolves them through the AssetManager, as the renderer resolves meshes and fonts (§8.2).
//
// One of the Renderer headers Scene may include (Scripts/ModuleRules.json's SnapshotHeaders, §3 rule 3): it includes nothing
// beyond Core, Asset, glm and the standard library, and in particular no NVRHI.
//
// Containers (as RenderSnapshot's, M18's R-ARENA): a list built with UiDrawList(resource) allocates Items, Annotations and
// (C22.3) WorldCanvases from that memory resource, which must outlive it; a default-constructed one, and every copy, from
// the default resource. The nested strings keep the default allocator. A plain value type: copyable, movable,
// thread-compatible.
//
// C22.3 (Docs/Decisions/0033-m22-contract.md decisions 48 and 51): an item of a World canvas (UiCanvasRenderMode::World) is
// in that canvas's own pixels and names it (UiDrawItem::WorldCanvas, an index of WorldCanvases), whose matrix places it in
// the world; its clip is in the same pixels, so the Ui pass clips it on the CPU instead of with a scissor. A focused text
// input's text item carries its caret (UiDrawItem::HasCaret).

namespace Engine {

	// A clip rectangle in whole scene pixels: the pixels (x, y) with MinX <= x < MaxX and MinY <= y < MaxY. A rectangle in
	// scene pixels from Min to Max covers the pixels whose centres it holds (Min <= x + 0.5 < Max, y likewise, the
	// rasterizer's rule), so its clip is MinX = ceil(Min.x - 0.5) and MaxX = ceil(Max.x - 0.5), each within plus or minus 2^30
	// (Scene/UiLayout.h, ComputeUiClipRect). The Ui pass applies it as the draws' scissor, intersected with the framebuffer.
	struct UiClipRect
	{
		int32_t MinX = 0;
		int32_t MinY = 0;
		int32_t MaxX = 0;
		int32_t MaxY = 0;

		// True when the rectangle holds no pixel.
		[[nodiscard]] bool IsEmpty() const { return MaxX <= MinX || MaxY <= MinY; }

		bool operator==(const UiClipRect&) const = default;
	};

	// What a draw item draws.
	enum class UiDrawKind : uint8_t
	{
		Image, // a sprite sheet frame or a plain colour over the item's rectangle (UiImage)
		Text   // text laid out in the item's rectangle (UiText)
	};

	// A text block's alignment along one axis in its rectangle: Start is the left (horizontal) or top (vertical) edge.
	enum class UiDrawAlignment : uint8_t
	{
		Start,
		Center,
		End
	};

	// UiImageType, without including Scene.
	enum class UiDrawImageType : uint8_t
	{
		Simple,
		Sliced
	};

	// UiImageFilter, without including Scene: the QuadBatcher's sampler the image takes (QuadSampler).
	enum class UiDrawFilter : uint8_t
	{
		Point,
		Linear
	};

	// C22.3: UiDrawItem::WorldCanvas of an item of a Screen canvas.
	inline constexpr uint32_t UiScreenCanvas = std::numeric_limits<uint32_t>::max();

	// C22.3 (decision 51): a World canvas of the list: its canvas pixels (x right, y down, origin at its top-left corner, Size
	// wide and high) map to world space by World (UiCanvasLayout::World at the canvas entity's render pose: the
	// interpolated transform meshes draw with, so a moving canvas draws where its parent's meshes draw).
	struct UiWorldCanvas
	{
		UUID Canvas{};
		glm::uvec2 Size = glm::uvec2(1);
		glm::mat4 World = glm::mat4(1.0f);

		bool operator==(const UiWorldCanvas&) const = default;
	};

	// One image or text of a UI element, in draw order. Only the members of its Kind are read.
	struct UiDrawItem
	{
		UiDrawKind Kind = UiDrawKind::Image;
		UUID Entity{}; // the element's entity
		// The element's rectangle in scene pixels (snapped to whole pixels when its canvas snaps).
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha: the image's tint, the text's colour
		// The intersection of the element's clipping ancestors' rectangles (UiRect::ClipChildren); Clipped false: unclipped.
		bool Clipped = false;
		UiClipRect Clip{};

		// Image: the sheet and the index of its frame; a null sheet draws a plain rectangle of Color.
		AssetHandle Sheet{};
		uint32_t Frame = 0;
		UiDrawImageType ImageType = UiDrawImageType::Simple;
		float BorderScale = 1.0f; // Sliced: scene pixels per border texel (UiImage::BorderScale times the canvas's scale)
		UiDrawFilter Filter = UiDrawFilter::Point;

		// Text: UTF-8 with line feeds; a null font is the Default font (BuiltinAssetHandles::DefaultFont).
		std::string Text{};
		AssetHandle Font{};
		float FontSize = 32.0f; // scene pixels per em (UiText::Size times the canvas's scale)
		UiDrawAlignment HorizontalAlignment = UiDrawAlignment::Start;
		UiDrawAlignment VerticalAlignment = UiDrawAlignment::Start;
		bool Wrap = true; // lines break at spaces to fit the rectangle's width
		float LineSpacing = 1.0f;

		// C22.3 (decision 51): UiScreenCanvas, or the index in UiDrawList::WorldCanvases of the World canvas whose pixels the
		// item's rectangle and clip are in.
		uint32_t WorldCanvas = UiScreenCanvas;
		// C22.3 (decision 48): Text of a focused text input's TextElement: the caret is drawn before the byte CaretByte of
		// Text (Text's size: after its end), and the text moves the least that keeps it inside the rectangle.
		bool HasCaret = false;
		uint32_t CaretByte = 0;
	};

	// One capture annotation of a UI element (viewport.screenshot's bounds and labels, Scene/UiExtraction.h): with Outline a
	// one-pixel outline inside its rectangle, and when Label is not empty the label above its top-left corner, both in Color.
	// Drawn after every item, unclipped.
	struct UiAnnotation
	{
		UUID Entity{};
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha
		bool Outline = false;
		std::string Label{};
	};

	// The UI of one view (see the file comment).
	struct UiDrawList
	{
		// The containers allocate from the default memory resource.
		UiDrawList() = default;
		// The containers allocate from `resource` (not null, asserted), which outlives the list.
		explicit UiDrawList(std::pmr::memory_resource* resource);

		// Every canvas's items: canvases in ascending SortOrder (ties in canonical entity order), each canvas's elements in
		// canonical entity order, an element's image before its text.
		std::pmr::vector<UiDrawItem> Items{};
		std::pmr::vector<UiAnnotation> Annotations{};
		// C22.3: the World canvases its items name, in the layout table's order.
		std::pmr::vector<UiWorldCanvas> WorldCanvases{};
	};

}
