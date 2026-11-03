#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/UiDrawList.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// UI layout (M22, C22.1, R-UI-CORE and R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decisions 4 and 5): the pure function
// from a scene's UI components and a scene resolution to the layout table, every UI element's rectangle in scene pixels, its
// clip and its draw order. The UiLayout and FrameUiLayout phases lay the session's scene out with it (Scene/UiSystem.h), and
// render extraction lays out each view at its snapshot's scene resolution (Scene/UiExtraction.h), so drawing and hit testing
// (W22.2's UiInput) read the same rectangles.
//
// The space is M18's UI coordinate space (Docs/Decisions/0029-m18-contract.md decision 8): scene pixels, x right, y down,
// origin at the scene image's top-left corner, the scene resolution's size. A canvas (UiCanvas) covers it; its elements
// (entities with a UiRect below it, down to the next canvas) are placed in canvas units, mapped to scene pixels by the
// canvas's scale:
//   - Scale: ConstantPixelSize ScaleFactor; MatchHeight sceneHeight / ReferenceHeight. The canvas's size in units is the scene
//     resolution / Scale.
//   - An element's rectangle in its parent element's (UiRectComponent.h's rule; an entity without a UiRect in between is
//     transparent), or, when its parent element has a UiLayout, that container's placement of it (Scene/UiContainerLayout.h:
//     the container places the elements whose parent element it is, in canonical order), in canvas units, then times Scale
//     in scene pixels from the canvas's origin.
//   - PixelSnap: each corner rounds to floor(v + 0.5) scene pixels, after the scale, before the children are placed in it.
//   - Clip: the intersection of the ComputeUiClipRect of every ancestor element with ClipChildren (the element's own flag
//     clips only its descendants).
//   - Visible: the element's rectangle, intersected with its clip and with the scene image, covers a pixel.
// The widgets' rules (C22.2, Docs/Decisions/0033-m22-contract.md decision 26; stream B the slider and toggle rules, stream C
// the scroll view's), each applied only to a part that is an element below its widget in the same canvas:
//   - UiSlider: its Fill's and Handle's anchors along its Direction follow its value (UiSliderComponent.h), in place of
//     their own, before they are placed in their parent element.
//   - UiToggle: while IsOn is false, its Graphic element and that element's subtree are Hidden (and so not Visible): laid
//     out as usual, neither drawn, hit nor focusable.
//   - UiScrollView: it clips its descendants as ClipChildren does; its Content is moved, with its subtree, by -offset
//     canvas units (times Scale in scene pixels) after it is placed, offset being ScrollPosition clamped to the content's
//     overflow (UiScrollViewComponent.h, ClampUiScrollPosition in Scene/UiWidgets.h; the sizes are the scroll view's and
//     the content's laid-out sizes in canvas units). With PixelSnap the moved corners are snapped again.
// C22.3's rules (Docs/Decisions/0033-m22-contract.md decisions 48 and 49; stream T the text input's, stream W the world
// canvases'), the text input's applied only to a part that is an element below its input in the same canvas:
//   - UiTextInput: it clips its descendants as ClipChildren does, whatever ClipChildren says; its Placeholder element and
//     that element's subtree are Hidden while its Text is not empty.
//   - A World canvas (UiCanvasComponent::RenderMode) is laid out as a Screen canvas would be over a scene image of its own
//     WorldWidth x WorldHeight canvas pixels, whatever the scene resolution: its scale mode applies to that size, its
//     elements' rectangles, clips and visibility are in its canvas pixels (Visible: inside that rectangle), and
//     UiCanvasLayout records the size and the matrix that places it in the world, from the canvas entity's world transform
//     (the scene's simulated pose; render extraction draws it at the render pose). A Screen canvas below a World canvas's
//     element starts a canvas of its own over the scene image, as before, and a World canvas below a Screen canvas's
//     element one of its own in the world.
// Only IEEE-exact operations (+, -, *, /, comparisons, floor, ceil) in float on the components' values: a deterministic scene gives
// the identical table in every configuration (Roadmap M22's acceptance, "UI layout tables are byte-identical across
// configurations"). No asset access: rectangles never follow their content (decision 6), so layout needs no font.
// Scene is on the simulation path's lint scope (§4.12). Main thread, like the scene (§4.11).

namespace Engine {

	class Scene;

	// One laid-out UI element.
	struct UiLayoutNode
	{
		UUID Entity{};
		UUID Canvas{};      // its canvas's entity
		UUID Parent{};      // its parent element, or its canvas for a top-level element
		uint32_t Depth = 1; // 1 for a top-level element, its parent's plus one below it
		// The element's rectangle in scene pixels (snapped when its canvas snaps): [Min, Max]. C22.3: an element of a World
		// canvas has its rectangle, and its Clip, in that canvas's own pixels (UiCanvasLayout::SceneSize).
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		bool Clipped = false; // Clip holds; false: no clipping ancestor
		UiClipRect Clip{};
		// Its rectangle, clipped and inside the scene image (a World canvas's element: inside its canvas's rectangle, (0, 0) to
		// SceneSize), covers a pixel, and it is not Hidden.
		bool Visible = false;
		// C22.2 (decision 26): inside an off toggle's Graphic subtree, so neither drawn, hit nor focusable (never Visible).
		bool Hidden = false;

		bool operator==(const UiLayoutNode&) const = default;
	};

	// One laid-out canvas.
	struct UiCanvasLayout
	{
		UUID Canvas{};
		int32_t SortOrder = 0;
		float Scale = 1.0f;               // scene pixels per canvas unit (a World canvas: its canvas pixels per canvas unit)
		glm::vec2 Size = glm::vec2(0.0f); // canvas units: the scene resolution / Scale (a World canvas: SceneSize / Scale)
		uint32_t FirstNode = 0;           // its elements: UiLayoutTable::Nodes[FirstNode, FirstNode + NodeCount)
		uint32_t NodeCount = 0;
		// C22.3 (decision 49; stream W): the canvas's render mode; a World canvas's own resolution in canvas pixels
		// (WorldWidth x WorldHeight; zero for a Screen canvas, which covers the table's SceneSize) and the matrix from its
		// canvas pixels to world space: translate(-SceneSize / 2), then the y flip, then 1 / PixelsPerUnit, then the canvas
		// entity's world matrix (identity for a Screen canvas). Its nodes' rectangles are in its canvas pixels.
		UiCanvasRenderMode RenderMode = UiCanvasRenderMode::Screen;
		glm::uvec2 SceneSize = glm::uvec2(0);
		glm::mat4 World = glm::mat4(1.0f);

		bool operator==(const UiCanvasLayout&) const = default;
	};

	// A scene's UI laid out at one scene resolution: every effectively enabled canvas and element, in draw order.
	struct UiLayoutTable
	{
		glm::uvec2 SceneSize = glm::uvec2(0);
		// Effectively enabled canvases (no HierarchyDisabledTag) in draw order: ascending SortOrder, ties in canonical order.
		std::vector<UiCanvasLayout> Canvases{};
		// Each canvas's effectively enabled elements in canonical order (a parent before its children, siblings in order),
		// canvas after canvas. An effectively disabled element and its subtree are not in the table: they are neither laid out
		// nor drawn, and take no space in a container.
		std::vector<UiLayoutNode> Nodes{};

		bool operator==(const UiLayoutTable&) const = default;
	};

	// The layout of `scene`'s UI at `sceneSize` scene pixels (each component >= 1, asserted): see the file comment. A UiRect
	// with no canvas ancestor is not laid out (project.validate reports UI_ELEMENT_WITHOUT_CANVAS). Pure.
	[[nodiscard]] UiLayoutTable ComputeUiLayout(const Scene& scene, glm::uvec2 sceneSize);

	// The magnitude ComputeUiClipRect clamps a pixel coordinate to before it converts it to int32: 2^30, exact in float and far
	// beyond any scene resolution, so a rectangle that passes int32's range (large lengths, scales and nesting) clips like
	// one that ends at the limit.
	inline constexpr float UiClipCoordinateLimit = 1073741824.0f;

	// The pixels a rectangle from `min` to `max` in scene pixels covers, those whose centres it holds (min <= x + 0.5 < max):
	// x from ceil(min.x - 0.5) to ceil(max.x - 0.5), exclusive, y likewise (UiClipRect), each clamped to
	// [-UiClipCoordinateLimit, UiClipCoordinateLimit] before it converts (a NaN, which no valid component value produces, to
	// -UiClipCoordinateLimit), so the conversion is always defined. Pure.
	[[nodiscard]] UiClipRect ComputeUiClipRect(glm::vec2 min, glm::vec2 max);

	// The node of `entity` in `table`, or nullptr. Linear in the table's size.
	[[nodiscard]] const UiLayoutNode* FindUiLayoutNode(const UiLayoutTable& table, UUID entity);

	// The canvas `canvas` in `table`, or nullptr. Linear in the canvas count.
	[[nodiscard]] const UiCanvasLayout* FindUiCanvasLayout(const UiLayoutTable& table, UUID canvas);

}
