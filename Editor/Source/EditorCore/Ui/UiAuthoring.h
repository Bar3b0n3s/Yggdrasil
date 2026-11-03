#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

// UI authoring (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decisions 33 and 34; stream D): the editor's
// minimal UI tooling as pure functions over UiRect values and layout tables, which the Editor's authoring overlay
// (Editor/Ui/UiAuthoringOverlay), its UiRect drawer (Editor/Drawers/UiRectDrawer, the §5.4 drawer exception) and its UI
// preview call, and which the tests reach without a window. Every change the tools make is submitted as entity.update
// (UiRect values) and every selection as edit.select, through EditorActions, so they need no parity exemption; ui.tree
// gives an agent the rectangles the overlay shows. No snapping or guides (an accepted limit, Backlog §6). Lengths are
// canvas units unless named pixels. Main thread; pure.

namespace Engine {

	// --- Anchor presets (the UiRect drawer's 4 x 4 grid) -----------------------------------------------------------------

	// The anchors of an axis: Left and Top 0, Center and Middle 0.5, Right and Bottom 1 (AnchorMin = AnchorMax), Stretch
	// AnchorMin 0 and AnchorMax 1.
	enum class UiAnchorHorizontal : uint8_t
	{
		Left,
		Center,
		Right,
		Stretch
	};

	enum class UiAnchorVertical : uint8_t
	{
		Top,
		Middle,
		Bottom,
		Stretch
	};

	struct UiAnchorPreset
	{
		UiAnchorHorizontal Horizontal = UiAnchorHorizontal::Center;
		UiAnchorVertical Vertical = UiAnchorVertical::Middle;

		bool operator==(const UiAnchorPreset&) const = default;
	};

	struct UiAnchorPresetOptions
	{
		// The pivot follows the anchors per axis: 0, 0.5 or 1 (0.5 on a stretched axis); else it is kept.
		bool SetPivot = false;
		// The element moves to its anchors: Position 0; on an axis stretched by the preset Size 0 (it fills its parent), and
		// on an axis that is not, Size the element's laid-out size on that axis before the change (so an element that was
		// stretched keeps its width or height, never collapsing to a negative inset). False: its rectangle in its parent is
		// kept, Position and Size recomputed for the new anchors and pivot.
		bool SetPosition = false;
	};

	// `rect` with `preset`'s anchors under `options`, for a parent rectangle of `parentSize` (its laid-out size in canvas
	// units: what keeps the rectangle in place). The element's laid-out size on an axis is max(0, parentSize * (AnchorMax -
	// AnchorMin) + Size) of `rect` (UiRectComponent.h), which an axis that is not stretched after the preset takes as its
	// Size, with SetPosition or without. ClipChildren is kept. Pure.
	[[nodiscard]] UiRectComponent ApplyUiAnchorPreset(const UiRectComponent& rect, UiAnchorPreset preset, glm::vec2 parentSize, UiAnchorPresetOptions options);

	// The preset whose anchors `rect` has on both axes, or nullopt (the grid then highlights none). Pure.
	[[nodiscard]] std::optional<UiAnchorPreset> FindUiAnchorPreset(const UiRectComponent& rect);

	// --- The rect tool ---------------------------------------------------------------------------------------------------

	enum class UiRectHandle : uint8_t
	{
		Move,
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight
	};

	// The handle of a rectangle [min, max] (panel pixels) under `point`: a corner within `grab` pixels of it (on both axes),
	// else an edge within `grab` pixels of it between its corners, else Move inside the rectangle; nullopt outside them. Pure.
	[[nodiscard]] std::optional<UiRectHandle> FindUiRectHandle(glm::vec2 min, glm::vec2 max, glm::vec2 point, float grab);

	// A drag of `handle` by `delta` canvas units from `start`, whose laid-out size is `startSize` canvas units: Move adds
	// `delta` to Position; an edge (or a corner's two edges) moves only that edge, the opposite one staying in place: the
	// max edge by d makes Size + d and Position + Pivot * d, the min edge by d makes Size - d and Position + (1 - Pivot) * d,
	// d clamped so that the laid-out size never goes below 0 on the axis. Anchors, Pivot and ClipChildren are kept. Pure.
	[[nodiscard]] UiRectComponent DragUiRect(const UiRectComponent& start, glm::vec2 startSize, UiRectHandle handle, glm::vec2 delta);

	// --- Selection and the UI preview -------------------------------------------------------------------------------------

	// The element the preview selects at `point` (scene pixels): the topmost element of `table` (the last in draw order)
	// whose rectangle holds the point (Min <= p < Max) and, when Clipped, whose clip does, whether it draws or not; Hidden
	// elements are left out. Null when none. Pure.
	// C22.3 (the review of C22.3; stream W): the elements of a World canvas are left out too, since their rectangles are in
	// that canvas's own pixels, not the frame's; the Editor's authoring overlay likewise outlines, and gives the rect tool's
	// handles to, Screen canvases' elements only. A World canvas is placed by its entity's Transform (the scene view's
	// gizmo), and its elements are edited in the inspector or with entity.update, ui.tree reporting their rectangles.
	[[nodiscard]] UUID PickUiElement(const UiLayoutTable& table, glm::vec2 point);

	// The game frame the UI preview shows (decision 33): the Game view's fixed resolution (`gameResolution`, viewport.setOptions'
	// gameResolution) when it is set, else the project's window size (GetDeterministicViewSize(window), every deterministic
	// session's view). Pure.
	[[nodiscard]] glm::uvec2 GetUiPreviewFrameSize(glm::uvec2 gameResolution, const WindowSettings& window);

	// Where the preview's frame image lands in the scene viewport's panel: the frame fitted into the panel with its aspect
	// kept and centred (letterboxed), Scale panel pixels per scene pixel.
	struct UiPreviewFrame
	{
		glm::vec2 Min = glm::vec2(0.0f);  // panel pixels
		glm::vec2 Size = glm::vec2(0.0f); // panel pixels
		float Scale = 1.0f;

		bool operator==(const UiPreviewFrame&) const = default;
	};

	// The fit of a `frameSize` (each >= 1) frame into a panel at `panelMin` of `panelSize` (a zero-area panel gives a
	// zero-area frame with Scale 0). Pure.
	[[nodiscard]] UiPreviewFrame ComputeUiPreviewFrame(glm::uvec2 frameSize, glm::vec2 panelMin, glm::vec2 panelSize);

	// A panel point in the frame's scene pixels: (point - Min) / Scale, unclamped (a point of the letterbox maps outside the
	// frame); the point itself for a frame with Scale 0. Pure.
	[[nodiscard]] glm::vec2 MapUiPreviewPointToScene(const UiPreviewFrame& frame, glm::vec2 panelPoint);

}
