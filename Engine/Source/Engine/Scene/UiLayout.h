#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/UiDrawList.h"

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
		// The element's rectangle in scene pixels (snapped when its canvas snaps): [Min, Max].
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		bool Clipped = false; // Clip holds; false: no clipping ancestor
		UiClipRect Clip{};
		bool Visible = false; // its rectangle, clipped and inside the scene image, covers a pixel

		bool operator==(const UiLayoutNode&) const = default;
	};

	// One laid-out canvas.
	struct UiCanvasLayout
	{
		UUID Canvas{};
		int32_t SortOrder = 0;
		float Scale = 1.0f;               // scene pixels per canvas unit
		glm::vec2 Size = glm::vec2(0.0f); // canvas units: the scene resolution / Scale
		uint32_t FirstNode = 0;           // its elements: UiLayoutTable::Nodes[FirstNode, FirstNode + NodeCount)
		uint32_t NodeCount = 0;

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
