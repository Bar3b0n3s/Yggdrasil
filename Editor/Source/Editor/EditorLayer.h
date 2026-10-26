#pragma once

#include "EditorCore/EditorUiState.h"
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
		// Registers the open-panel settings handler ([EnginePanels][Open] in imgui.ini) in the current Dear ImGui context, if
		// any (OnImGuiRender registers it in a context created later), so construct the layer before that context first
		// loads its ini. The destructor saves the ini once more while it has a file, then unregisters the handler; any later
		// save drops the entry, so the owner stops the ini before destroying the layer (EditorApp::OnShutdown).
		explicit EditorLayer(EditorPanelContext& context);
		~EditorLayer();
		EditorLayer(const EditorLayer&) = delete;
		EditorLayer& operator=(const EditorLayer&) = delete;
		// Once per frame before automation Pump/GPU work: execute queued UI commands, finalize requested play/save
		// gestures and update CPU UI models. The host publishes autosave AFTER this and automation Pump, before GPU work.
		// Apply queued injected recovery decisions only after rechecking offer/project/revision; cancellation or Conflict
		// never overwrites intervening edits. No ImGui calls, so safe in headless/renderer-none mode.
		[[nodiscard]] Status OnSafePoint(double nowSeconds);
		// Inside ImGui BeginFrame/EndFrame. Menu, playback toolbar and status reserve space around the docked panels.
		// First-run docking initializes once per ini path. ImGui 1.92.6+ keeps the original IDs with friendly ### titles,
		// so saved window geometry and selected tabs remain intact without migration.
		// Scene browsing reads the whole project on opening/refresh; create/open/save choices queue existing user actions.
		// Open scenes (M20, Docs/Decisions/0031-m20-contract.md decision 20): with a project open, a bar of scene tabs under the
		// toolbar lists the open scenes in tab order (name, " *" for unsaved changes, the active one highlighted); a click
		// queues scene.activate and a tab's close button scene.close, asking to save or discard a dirty scene first, each
		// naming its scene by the scene target. The New and Open scene dialogs offer "Open in a new tab" (keepOpen), and
		// choosing a scene that is already open switches to its tab. The panels follow the active scene (GetScene()); when it
		// changes, the inspector's gesture ends unwritten.
		// The already-open project recovery modal reads EditorPanelContext::Recovery.GetOffer and queues Accept/Decline;
		// it remains available after ProjectLauncher closes. Errors propagate to the host. No file writes or destructive
		// scene iteration from a drawing callback.
		[[nodiscard]] Status OnImGuiRender();
		// A pending editor.screenshot requests a fresh frame. Minimized host must service this with a null/offscreen
		// UI frame, not stale draw data; serial completed after the request is required before capture.
		void RequestFrame();
		[[nodiscard]] bool IsFrameRequested() const;
		// Opens `panel` (EditorUiState) and gives its window focus on the next frame, selecting its dock tab; the host
		// calls it for feedback outside ImGui traversal, such as files dropped on the window. InvalidArgument for an
		// unknown panel.
		[[nodiscard]] Status RevealPanel(EditorPanel panel);
	private:
		struct State;
		Scope<State> m_State;
	};

}
