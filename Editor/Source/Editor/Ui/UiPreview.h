#pragma once

#include "EditorCore/Ui/UiAuthoring.h"

#include <glm/glm.hpp>

#include <optional>

// The scene view's UI preview (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 33): the scene view shows
// the scene it shows as the game frames it, through the scene's primary camera (the Primary camera source, whose extraction
// draws the UI, decision 7), without the editor overlays, the selection outline or the picking image, at the game frame
// (GetUiPreviewFrameSize: the Game view's fixed resolution when set, else the project's window size), fitted and letterboxed in
// the panel, under the UI authoring overlay (UiAuthoringOverlay.h). A scene without a primary camera shows its UI over the
// default clear colour, as the game view would. What it shows is what viewport.screenshot {view: "game", target: "edit"}
// captures at the same size and ui.tree reports. M22's files of their own: the editor host and the scene viewport panel only
// call them, one line each (ADR 0028's phase-2 ledger; the review of W22.2 moved the panel's preview logic here; decision 33
// lists the call sites). Main thread.

namespace Engine {

	class EditorContext;
	struct EditorViewportOptions;
	struct EditorViewportRect;
	struct RenderExtractionRequest;

	// Whether the scene view shows the UI preview: EditorViewportOptions::UiPreview with a project open (its frame is the
	// project's).
	[[nodiscard]] bool IsUiPreviewShown(const EditorContext& editor);

	// The scene view's Overlays popup's "UI preview" checkbox, with its tooltip, over `options` (inside ImGui, in the popup).
	void DrawUiPreviewOption(EditorViewportOptions& options);

	// The scene view's image placement while the UI preview is shown: the game frame the image holds (`rectangle.ImageSize`)
	// fitted and letterboxed in the panel's `rectangle`, whose Min and Size then become the frame's, so the bars lie outside
	// it and the image, its hover and the pointer's mapping follow the frame. Returns the frame; nullopt, leaving `rectangle`
	// as it is, when the preview is not shown.
	[[nodiscard]] std::optional<UiPreviewFrame> FitUiPreviewImage(const EditorContext& editor, EditorViewportRect& rectangle);

	// The size the scene view renders at while the UI preview is shown (the host's requested size for a displayed scene view):
	// GetUiPreviewFrameSize of the viewport state's game resolution and the project's window settings, at most 8192 a side
	// like every view; nullopt when the preview is not shown.
	[[nodiscard]] std::optional<glm::uvec2> GetUiPreviewRenderSize(const EditorContext& editor);

	// Makes `request`, the scene view's extraction request, the preview's when it is shown: the Primary camera source, no view
	// flags (no editor overlays, selection or picking) and no selected entities; returns whether it did (so the caller keeps
	// the scene view's debug view off it, as the game view's).
	[[nodiscard]] bool ConfigureUiPreviewRequest(const EditorContext& editor, RenderExtractionRequest& request);

}
