#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>

namespace Engine {

	struct EditorPanelContext;

	// Windowed and null-platform UI use this same dockspace and panels. context's services outlive the layer.
	// Main thread with an active ImGui context. EditorApp retains render ownership; this replaces the demo only after
	// integration. No second application/layer stack; this is a single UI owner.
	class EditorLayer
	{
	public:
		explicit EditorLayer(EditorPanelContext& context);
		~EditorLayer();
		EditorLayer(const EditorLayer&) = delete;
		EditorLayer& operator=(const EditorLayer&) = delete;
		// Once per frame before automation Pump/GPU work: execute queued UI commands, finalize requested play/save
		// gestures and update CPU UI models. The host publishes autosave AFTER this and automation Pump, before GPU work.
		// Apply queued injected recovery decisions only after rechecking offer/project/revision; cancellation or Conflict
		// never overwrites intervening edits. No ImGui calls, so safe in headless/renderer-none mode.
		[[nodiscard]] Status OnSafePoint(double nowSeconds);
		// Inside ImGui BeginFrame/EndFrame. Dockspace first, menu/toolbar, SceneChangedOnDisk reload banner, panels.
		// A first-run layout is deterministic; user layout loads through existing ImGuiLayer::SetIniFilePath.
		// The already-open project recovery modal reads EditorPanelContext::Recovery.GetOffer and queues Accept/Decline;
		// it remains available after ProjectLauncher closes. Errors propagate to the host. No file writes or destructive
		// scene iteration from a drawing callback.
		[[nodiscard]] Status OnImGuiRender();
		// A pending editor.screenshot requests a fresh frame. Minimized host must service this with a null/offscreen
		// UI frame, not stale draw data; serial completed after the request is required before capture.
		void RequestFrame();
		[[nodiscard]] bool IsFrameRequested() const;
	};

}
