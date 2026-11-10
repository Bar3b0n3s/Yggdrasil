#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Engine {

	struct EditorPanelContext;
	struct EditorViewportImage;
	struct EditorViewportRect;

	// The scene view's tile brush (M21, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 26; Architecture §12.2). Its
	// target is the one selected entity of the displayed scene when it holds a TilemapRenderer with a Tilemap: only then do
	// its controls show in the scene view's toolbar, after the 2D toggle, so no editor-UI golden changes: a palette entry
	// picker ("Off" or an entry of the tilemap's palette) and an Erase toggle. The brush is armed while its controls show and
	// an entry is picked or Erase is on; armed, it takes the image's left and right presses, which neither pick, orbit nor fly
	// (the gizmo is hidden, so its handles never stand over a cell; Alt with the left button still orbits or pans). A left
	// stroke paints the picked entry (Erase on: erases) into every cell the cursor crosses, a right stroke erases them: the
	// cell under the cursor through the displayed image's camera (IntersectTilemapCell, with the target's rendered world
	// matrix at the image's interpolation alpha, the pose the image shows, and its tilemap's cell size), the cells between two
	// frames' cells joined (GetTilemapStrokeCells), each cell once.
	// On release the stroke becomes one tilemap.paint (MakeTilemapPaintParams), the method's own action, so its checks and its
	// one undo step are the method's; the tilemap draws the stroke's cells once it is applied, never before. A selection,
	// tilemap or scene change during a stroke cancels it with a Conflict, painting nothing; so does the image going away.
	// A stroke of more than MaxTilemapPaintCells cells is refused when it crosses the limit. Main thread inside ImGui; keeps
	// only UI presentation state (the picked entry, the toggle, the stroke's cells), no entity, component or asset pointer
	// across frames; the tilemap is read through the editor's asset manager while the controls draw.
	class TilemapBrush
	{
	public:
		// What the scene view saw of its image this frame.
		struct PointerInput
		{
			bool ImageShown = false;   // the scene image is displayed
			bool Captured = false;     // text input, another item, a popup, a drag-drop or the gizmo owns the pointer
			bool LeftPressed = false;  // the image item was pressed with the left button in this frame
			bool RightPressed = false; // the image item was pressed with the right button in this frame
		};

		// Finds the target and, when there is one, draws the controls in the toolbar's row (wrapping before `toolbarRight` as
		// the other tools do), disabled while the editor is read-only or a gizmo drag runs. `image` is the displayed scene
		// image, whose interpolation alpha (EditorViewportImage::Alpha) poses the target. A tilemap that does not load shows
		// its error instead and leaves the brush unarmed. A new target resets the picked entry and the toggle.
		void DrawControls(EditorPanelContext& context, const EditorViewportImage& image, float toolbarRight);
		// Whether the brush takes the image's left and right presses (see the class comment).
		[[nodiscard]] bool IsArmed() const;
		// Whether a stroke holds the pointer.
		[[nodiscard]] bool IsStroking() const { return m_Stroke.has_value(); }
		// Begins, extends and ends strokes; call it once per frame after the image item, with `rectangle` and `image` the
		// displayed image's. Returns a finished stroke's tilemap.paint params for the caller to submit, nullopt otherwise.
		// Errors: those of the image's ray (ComputeOutputPixelRayInterval) and MakeTilemapPaintParams; Conflict for a stroke
		// cancelled by a change of its target; InvalidArgument for a stroke past MaxTilemapPaintCells cells.
		[[nodiscard]] Result<std::optional<Json>> Update(const EditorViewportRect& rectangle, const EditorViewportImage& image,
			const PointerInput& input);
	private:
		// This frame's target: values only, read again every frame.
		struct Target
		{
			UUID Entity{};
			AssetHandle Tilemap{};
			bool PlayScene = false;
			glm::mat4 World = glm::mat4(1.0f);
			glm::vec2 CellSize = glm::vec2(1.0f);
			uint32_t PaletteSize = 0;
		};
		struct Stroke
		{
			UUID Entity{};
			AssetHandle Tilemap{};
			bool PlayScene = false;
			bool RightButton = false;
			uint32_t Tile = 0;
			std::vector<glm::ivec2> Cells{};              // each cell once, in the order the cursor crossed them
			std::set<std::pair<int32_t, int32_t>> Seen{}; // the cells of Cells
			std::optional<glm::ivec2> Last{};             // the cell under the cursor at the stroke's last update
		};

		// Adds the cell under the cursor, and those between it and the last one, to the stroke.
		[[nodiscard]] Status ExtendStroke(const EditorViewportRect& rectangle, const EditorViewportImage& image);

		std::optional<Target> m_Target{};
		std::optional<Stroke> m_Stroke{};
		UUID m_PickedFor{};     // the target entity the picked entry and the toggle belong to
		uint32_t m_Tile = 0;    // the picked palette entry k (paints k), 0 for none
		bool m_Erase = false;   // the left button erases too
		bool m_Enabled = false; // the controls drew enabled in this frame
	};

}
