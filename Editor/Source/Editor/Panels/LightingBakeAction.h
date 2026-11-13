#pragma once

#include "Engine/Core/Base.h"

#include <string_view>

// The editor's Bake Lighting action (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): the Tools menu's
// "Bake lighting" item, which bakes the active scene's IrradianceVolumes and ReflectionProbes by queueing lighting.bake, the
// action's one method (EditorCore/Lighting/LightingBake.h's BakeLighting behind it), so the editor bakes exactly as automation
// does (Roadmap rule 5). A file of its own that EditorLayer only calls (ADR 0028's phase-3 ledger: EditorLayer is M24's).

namespace Engine {

	class EditorContext;

	// The method the action queues, with no params: every probe of the active scene.
	inline constexpr std::string_view LightingBakeActionMethod = "lighting.bake";

	// Whether the action can run: a writable project's active scene saved to a file, no play session or test run, and an
	// editor that renders (EditorContext::GetProbeCapture). lighting.bake refuses each of the others with its reason.
	[[nodiscard]] bool IsLightingBakeAvailable(const EditorContext& editor);

	// Draws the "Bake lighting" item into the open menu, disabled unless IsLightingBakeAvailable, with a tooltip saying what it
	// writes. Returns true when the user chose it: the caller queues LightingBakeActionMethod. ImGui frame only.
	[[nodiscard]] bool DrawLightingBakeMenuItem(const EditorContext& editor);

}
