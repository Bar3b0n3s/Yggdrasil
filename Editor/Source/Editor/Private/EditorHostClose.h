#pragma once

#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class Autosave;
	class EditorActions;
	class EditorContext;

	enum class EditorCloseChoice : uint8_t
	{
		Save,    // save every dirty open scene to its file, then close
		Discard, // close without saving; a final recovery generation keeps the changes recoverable at the next launch
		Cancel   // keep the editor open
	};

	// What the close prompt shows: an owned, memory-only copy for the UI.
	struct EditorClosePrompt
	{
		// The names of every open scene with unsaved changes, in tab order (M20, Docs/Decisions/0031-m20-contract.md
		// decision 15), joined with ", " for the prompt.
		std::string SceneName{};
		std::vector<std::string> SceneNames{};
		bool CanSave = false;           // every one has a file; an untitled scene can only be discarded or kept open
		bool Pending = false;           // a Save or Discard choice runs at the next safe point
		std::optional<Error> Failure{}; // why the last choice left the editor open
	};

	// Main-thread owner of the window-close confirmation, shared by EditorApp and its tests; the referenced editor,
	// actions and autosave service outlive it. A window close request (WindowCloseEvent) with unsaved changes in any open
	// scene of a writable editor is held and asks Save / Don't save / Cancel, listing every such scene; a clean, scene-less
	// or read-only editor closes at once. Save runs scene.save through EditorActions (User origin, the handler automation
	// uses) when the active scene is the only one with unsaved changes, and project.save, which writes every dirty open
	// scene, otherwise. Don't save writes a final recovery generation per dirty open scene (AutosaveReason::Exit) before
	// closing, so the discarded changes stay recoverable at the next
	// launch; a failed final write is logged and the editor still closes, as the user chose. A host with no UI to show the
	// prompt (a windowed --renderer none editor has no ImGui layer) never holds a close nobody could answer: it closes at
	// once and keeps the unsaved changes recoverable the way Don't save does. Choices run at safe points, never inside
	// ImGui traversal. Automation's session.shutdown and headless runs never reach this prompt.
	class EditorHostClose
	{
	public:
		EditorHostClose(EditorContext& editor, EditorActions& actions, Autosave& saves);
		// For a window close request: true when the host must keep running (the prompt is now, or already, open), false
		// when it may close now. `canPrompt`: the host draws the prompt (EditorApp: it has an ImGui layer). Without it, a
		// close with unsaved changes logs a warning, writes the final recovery generation, drops any open prompt and its
		// queued save, and returns false.
		[[nodiscard]] bool RequestClose(bool canPrompt);
		[[nodiscard]] std::optional<EditorClosePrompt> GetPrompt() const;
		// Memory-only. Save queues scene.save or project.save (InvalidState when a listed scene is untitled, Submit's
		// errors), Discard queues the
		// final recovery and the close, Cancel dismisses the prompt and drops a queued save. NotFound without a prompt;
		// InvalidState for Save or Discard while a choice is pending. A refusal stays in the prompt's Failure.
		[[nodiscard]] Status Choose(EditorCloseChoice choice);
		// At the safe point, after EditorActions::Pump: completes a pending choice. True when the host should exit now:
		// after a successful save, after Discard, or once no open scene has unsaved changes left (saved meanwhile). A
		// failed save keeps the prompt open with its Failure.
		[[nodiscard]] bool Pump();
		// Draws the modal prompt while one is open and no other popup is shown, and queues the clicked choice.
		void Draw();
	private:
		[[nodiscard]] bool NeedsConfirmation() const;
		void Fail(const Error& error);
		// Closes the prompt and drops a queued save without side effects, consuming its completion.
		void Dismiss();
		// The final recovery generations of the scenes the user is closing without saving; a failure is only logged.
		void WriteExitRecovery(std::string_view sceneNames);
		// The prompt for the open scenes with unsaved changes now.
		[[nodiscard]] EditorClosePrompt MakePrompt() const;
	private:
		EditorContext& m_Editor;
		EditorActions& m_Actions;
		Autosave& m_Saves;
		std::optional<EditorClosePrompt> m_Prompt{};
		std::optional<EditorCloseChoice> m_Pending{};
		std::optional<uint64_t> m_SaveTicket{};
	};

}
