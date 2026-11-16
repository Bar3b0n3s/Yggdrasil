#pragma once

#include "Engine/Core/Base.h"

#include <string_view>

// The editor's Bake Navigation action (M30, Docs/Decisions/0045-m30-navigation.md decision 8): the Tools menu's "Bake
// navigation" item, which bakes the active scene's navigation mesh by queueing navigation.bake, the action's one method
// (EditorCore/Navigation/NavigationBake.h's BakeNavigation behind it), so the editor bakes exactly as automation does (Roadmap
// rule 5). A file of its own that EditorLayer only calls (ADR 0042's phase-5 ledger). It sits between Reload shaders and Bake
// lighting.

namespace Engine {

	class EditorContext;

	// The method the action queues, with no params: the active scene.
	inline constexpr std::string_view NavigationBakeActionMethod = "navigation.bake";

	// Whether the action can run: a writable project's active scene saved to a file, no play session or test run. The bake
	// needs no renderer. navigation.bake refuses each of the others with its reason, and a scene without a Navigation
	// component with NotFound.
	[[nodiscard]] bool IsNavigationBakeAvailable(const EditorContext& editor);

	// Draws the "Bake navigation" item into the open menu, disabled unless IsNavigationBakeAvailable, with a tooltip saying what
	// it writes. Returns true when the user chose it: the caller queues NavigationBakeActionMethod. ImGui frame only.
	[[nodiscard]] bool DrawNavigationBakeMenuItem(const EditorContext& editor);

}
