#pragma once

#include "EditorCore/Terrain/TerrainEditing.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The scene view's terrain sculpt and paint tool (M26, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decision 23): a
// file of its own that SceneViewportPanel only calls (ADR 0028's phase-3 ledger: SceneViewportPanel is M24's). The tool is
// TerrainBrush, not "TerrainTool", which names terrain.edit's tools (EditorCore/Terrain/TerrainEditing.h). Each stroke,
// press to release, is one terrain.edit with the world points the cursor picked on the surface, the method's own action, so
// a stroke made in the editor writes exactly the bytes terrain.edit writes for those points, with the method's checks and
// its one undo step (Roadmap rule 5).

namespace Engine {

	struct EditorPanelContext;
	struct EditorViewportImage;
	struct EditorViewportRect;

	// The most points one stroke of the brush holds: terrain.edit's limit.
	inline constexpr size_t MaxTerrainBrushPoints = 4096;

	// The point where `ray` (world space, a unit direction) first meets the surface of a terrain whose entity has the world
	// matrix `world`: the ray taken to the terrain's local space, marched across the terrain's box [0, Size.x] x
	// [0, HeightScale] x [0, Size.y] in steps of half a cell until it passes below SampleTerrainHeight, the crossing refined by
	// bisection, then taken back to world space. nullopt when the ray misses the box or the surface, starts below the surface,
	// or the matrix is not invertible. Pure.
	[[nodiscard]] std::optional<glm::vec3> IntersectTerrainSurface(const RenderRay& ray, const glm::mat4& world, const HeightfieldData& heightfield,
		const TerrainExtent& extent);

	// What one stroke of the brush does: terrain.edit's tool, radius, strength, height and layer.
	struct TerrainBrushSettings
	{
		TerrainTool Tool = TerrainTool::Raise;
		float Radius = 2.0f;   // metres, (0, 1000]
		float Strength = 0.5f; // [0, 1]
		float Height = 0.0f;   // Flatten's target, metres above the terrain's origin
		uint32_t Layer = 0;    // Paint's layer, 0 to 3
	};

	// terrain.edit's params of a stroke of `settings` through the world points `points` on the edit scene's `entity`:
	// {entity, tool, points, radius, strength, height, layer}. Pure.
	[[nodiscard]] Json MakeTerrainEditParams(UUID entity, const TerrainBrushSettings& settings, std::span<const glm::vec3> points);

	// The tool. Its target is the one selected entity of the edit scene, shown in the scene view (no run-time edits, Backlog
	// §6), when it holds a Terrain whose Heightfield loads: only then do its controls show in the scene view's toolbar, so no
	// editor-UI golden changes: a tool picker ("Off", Raise, Lower, Smooth, Flatten, Paint) and a Brush popup with the radius,
	// strength, Flatten's height and Paint's layer. The brush is armed while its controls show and a tool is picked; armed, it
	// hides the gizmo and takes the image's left presses (Alt with the left button still orbits; the right and middle buttons
	// still navigate), and draws its circle on the surface under the cursor. A stroke adds the surface point under the cursor
	// (IntersectTerrainSurface through the displayed image's camera, with the target's world matrix) at the press and whenever
	// the cursor has moved a quarter of the radius from the last point, the release's point included, and on release becomes
	// one terrain.edit (MakeTerrainEditParams). A selection, terrain or scene change, play starting or the image going away
	// during a stroke cancels it with a Conflict, editing nothing; a stroke past MaxTerrainBrushPoints points is refused when
	// it crosses the limit. Main thread inside ImGui; keeps only UI presentation state (the settings, the stroke's points), no
	// entity, component or asset pointer across frames.
	class TerrainBrush
	{
	public:
		// What the scene view saw of its image this frame.
		struct PointerInput
		{
			bool ImageShown = false;  // the scene image is displayed
			bool Captured = false;    // text input, another item, a popup, a drag-drop or the gizmo owns the pointer
			bool LeftPressed = false; // the image item was pressed with the left button in this frame
		};

		// Finds the target and, when there is one, draws the controls in the toolbar's row (wrapping before `toolbarRight` as
		// the other tools do), disabled while the editor is read-only or a gizmo drag runs. A new target keeps the settings
		// and turns the tool off.
		void DrawControls(EditorPanelContext& context, float toolbarRight);
		// Whether the brush takes the image's left presses (see the class comment).
		[[nodiscard]] bool IsArmed() const;
		// Whether a stroke holds the pointer.
		[[nodiscard]] bool IsStroking() const { return m_Stroke.has_value(); }
		// Begins, extends and ends strokes and draws the brush's circle; call it once per frame after the image item, with
		// `rectangle` and `image` the displayed image's. Returns a finished stroke's terrain.edit params for the caller to
		// submit, nullopt otherwise. Errors: those of the image's ray (ComputeOutputPixelRayInterval); Conflict for a stroke
		// cancelled by a change of its target; InvalidArgument for a stroke past MaxTerrainBrushPoints points.
		[[nodiscard]] Result<std::optional<Json>> Update(const EditorViewportRect& rectangle, const EditorViewportImage& image, const PointerInput& input);
	private:
		// This frame's target: values only, read again every frame.
		struct Target
		{
			UUID Entity{};
			AssetHandle Heightfield{};
			glm::mat4 World = glm::mat4(1.0f);
			TerrainExtent Extent{};
		};
		struct Stroke
		{
			UUID Entity{};
			AssetHandle Heightfield{};
			TerrainBrushSettings Settings{};
			std::vector<glm::vec3> Points{};      // world space, in stroke order
			std::optional<glm::vec2> LastLocal{}; // the last point in the terrain's local XZ plane
		};

		// The surface point under the cursor, or nullopt.
		[[nodiscard]] Result<std::optional<glm::vec3>> FindCursorPoint(const EditorViewportRect& rectangle, const EditorViewportImage& image) const;
		// Adds the cursor's point to the stroke when it is the first or a quarter of the radius from the last.
		[[nodiscard]] Status ExtendStroke(const EditorViewportRect& rectangle, const EditorViewportImage& image);
		// Draws the brush's circle around the local point `centre` over the image.
		void DrawCircle(const EditorViewportRect& rectangle, const EditorViewportImage& image, const glm::vec3& centre) const;

		std::optional<Target> m_Target{};
		std::optional<Stroke> m_Stroke{};
		// A copy of the target's heightfield as loaded at its version (FindCursorPoint and the circle sample it).
		std::optional<HeightfieldData> m_Heightfield{};
		AssetHandle m_HeightfieldHandle{};
		uint64_t m_HeightfieldVersion = 0;
		UUID m_PickedFor{};    // the target entity the picked tool belongs to
		bool m_Active = false; // a tool is picked (m_Settings.Tool)
		TerrainBrushSettings m_Settings{};
		bool m_Enabled = false; // the controls drew enabled in this frame
	};

}
