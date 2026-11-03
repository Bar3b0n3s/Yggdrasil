#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>

// UiRect's anchor presets (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 34): the third custom inspector
// drawer Architecture §5.4 allows (beside Script fields and Prefab overrides), drawn above UiRect's reflected fields, which still
// draw as every component's. A file of M22's own that InspectorPanel.cpp only calls (ADR 0028's phase-2 ledger: M21 owns the
// panel in phase 2).

namespace Engine {

	class EditorContext;
	class Scene;

	// The action a click on a preset submits through EditorActions: entity.update of the UiRect for one entity, an edit.batch
	// of them for several (one undo step), so the drawer needs no parity exemption.
	struct UiRectPresetAction
	{
		std::string Method{};
		Json Params{};
	};

	// Draws the 4 x 4 grid (columns: horizontal Left, Center, Right, Stretch; rows: vertical Top, Middle, Bottom, Stretch; the
	// preset every entity of `selection` has highlighted, FindUiAnchorPreset) into the current window, disabled in a read-only
	// editor. A click applies the cell's preset to the UiRect of every entity of `selection` in `scene` (`target` names it
	// to entity.update) with ApplyUiAnchorPreset: its rectangle kept, Shift also setting the pivot and Alt also moving it to the
	// anchors (UiAnchorPresetOptions). Each element's parent size comes from the layout of `scene` at the UI preview's frame
	// (GetUiPreviewFrameSize of the viewport state and the project's window: the canvas's size, or the parent element's, in
	// canvas units); an element that is not laid out (no canvas above it, or disabled) is placed in a parent of size 0.
	// Returns the action of the cell clicked this frame, or nullopt. Main thread inside ImGui; `selection`'s entities hold a
	// UiRect (the inspector draws UiRect only when every selected entity has one).
	[[nodiscard]] std::optional<UiRectPresetAction> DrawUiRectAnchorPresets(const EditorContext& editor, const Scene& scene,
		std::span<const UUID> selection, SceneTarget target);

}
