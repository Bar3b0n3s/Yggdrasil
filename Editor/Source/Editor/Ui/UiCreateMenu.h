#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <optional>

// The Create UI menu (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 32): the hierarchy's entity context
// menu shows it as "Create UI child" and its background context menu as "Create UI". Each of its eight items (the kinds of
// UiWidgetKind: Canvas, Panel, Image, Text, Button, Toggle, Slider, Scroll view) is ui.createWidget's action, which the caller
// submits through EditorActions, so the menu needs no parity exemption; a widget without a canvas above it goes below a new
// canvas (UiAuthoringMethods.h). A file of M22's own that SceneHierarchyPanel.cpp only calls. Main thread inside ImGui.

namespace Engine {

	// Draws the menu `label` (an ImGui::BeginMenu in the open popup or menu), disabled when `enabled` is false. Returns the
	// params of ui.createWidget for the kind chosen this frame, below `parent` (the invalid UUID: none, so a root canvas), or
	// nullopt when nothing was chosen.
	[[nodiscard]] std::optional<Json> DrawUiCreateMenu(const char* label, UUID parent, bool enabled);

}
