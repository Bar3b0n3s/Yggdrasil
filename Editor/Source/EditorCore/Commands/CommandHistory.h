#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class EditorContext;

	// The bounds of the undo history (§12.3: "bounded at 1,000 entries or 256 MB").
	struct CommandHistoryLimits
	{
		size_t MaxEntries = 1000;
		size_t MaxBytes = 256ull * 1024 * 1024;
	};

	// One recorded command, as edit.history and the UndoHistoryPanel show it.
	struct CommandHistoryEntry
	{
		// The command's position in the history's sequence: 1 for the first command ever executed in this history, then
		// increasing by one per executed command, never reused (also after undo, redo, bounding or a new branch). This is
		// the "undoIndex" automation results report (§13.4).
		uint64_t Sequence = 0;
		std::string Label{}; // the display label: "[agent] " + Command::GetLabel() for Agent commands
		CommandOrigin Origin = CommandOrigin::User;
		uint64_t RevisionBefore = 0; // EditorContext::GetRevision before and after the command executed (§12.3)
		uint64_t RevisionAfter = 0;
		size_t MemorySize = 0;
		bool ChangesScene = true;
	};

	// The linear undo history of one open scene (§12.3). Owned by EditorContext, which clears it whenever another scene
	// opens. Main thread only.
	//
	// Execute runs a command and, on success, records it after the current position (discarding the redo branch) or merges
	// it into the newest entry (Command::MergeWith). The oldest entries are dropped while the history holds more than
	// MaxEntries or MaxBytes. The save point remembers where the open scene was last saved, so IsDirty tells whether a
	// scene-changing command lies between it and the current position (in either direction).
	class CommandHistory
	{
	public:
		explicit CommandHistory(CommandHistoryLimits limits = {});
		~CommandHistory();

		CommandHistory(const CommandHistory&) = delete;
		CommandHistory& operator=(const CommandHistory&) = delete;
		// Movable, so a dry run can swap a sandbox history in and the real one back (EditorDryRunScope).
		CommandHistory(CommandHistory&& other) noexcept;
		CommandHistory& operator=(CommandHistory&& other) noexcept;

		// Executes `command` on `context` and records it. Returns the entry's Sequence (of the merged entry when it merged).
		// Errors: the command's own error; the history and the editor are unchanged then.
		[[nodiscard]] Result<uint64_t> Execute(Scope<Command> command, EditorContext& context);

		// Records a command that has already been applied (a CompositeCommand closed by an EditorTransaction, whose children
		// executed one by one), without calling Execute. `revisionBefore` is EditorContext::GetRevision before its first
		// child ran. Returns its Sequence.
		[[nodiscard]] uint64_t RecordExecuted(Scope<Command> command, EditorContext& context, uint64_t revisionBefore);

		// Undoes up to `steps` (>= 1, asserted) commands, newest first; returns how many were undone (fewer when the history
		// runs out). Stops at the first command whose Undo fails (Command::Undo) and returns its error with the number
		// already undone in the error's context; the failed command and everything older stay applied (mirroring Redo).
		[[nodiscard]] Result<size_t> Undo(EditorContext& context, size_t steps = 1);

		// Redoes up to `steps` (>= 1, asserted) undone commands, oldest first, by executing them again. Stops at the first
		// that fails and returns its error with the number already redone in the error's context; the failed command and
		// the rest of the redo branch stay undone. Returns how many were redone.
		[[nodiscard]] Result<size_t> Redo(EditorContext& context, size_t steps = 1);

		[[nodiscard]] bool CanUndo() const;
		[[nodiscard]] bool CanRedo() const;
		// The display label of the command Undo would undo or Redo would redo; empty when there is none.
		[[nodiscard]] std::string GetUndoLabel() const;
		[[nodiscard]] std::string GetRedoLabel() const;

		// The Sequence of the newest applied command (0 when none is applied).
		[[nodiscard]] uint64_t GetCurrentSequence() const;

		// Up to `limit` entries ending at the newest one (redo branch included), oldest first, with the number of applied
		// ones among them at GetUndoCount().
		[[nodiscard]] std::vector<CommandHistoryEntry> GetEntries(size_t limit) const;
		// The entries currently applied (undoable) and undone (redoable).
		[[nodiscard]] size_t GetUndoCount() const;
		[[nodiscard]] size_t GetRedoCount() const;

		// The recorded command with `sequence`, applied or undone; nullptr when it is not held (dropped, or never existed).
		// scene.diff {against: "revision"} reads SceneEditCommand changes through it.
		[[nodiscard]] const Command* FindCommand(uint64_t sequence) const;
		[[nodiscard]] const CommandHistoryEntry* FindEntry(uint64_t sequence) const;

		// Marks the current position as the save point (the open scene was just saved).
		void MarkSavePoint();
		// True when a scene-changing command lies between the save point and the current position, or when the save point's
		// entry was dropped by the bounds or discarded with a redo branch (the saved state is no longer reachable).
		[[nodiscard]] bool IsDirty() const;

		// Drops every entry and sets the save point to the empty history (a scene was opened or created).
		void Clear();

		[[nodiscard]] const CommandHistoryLimits& GetLimits() const { return m_Limits; }
		[[nodiscard]] size_t GetMemorySize() const;
	private:
		// The entries with their commands, the current position, the save point, the next sequence number and the byte
		// total (CommandHistory.cpp).
		struct State;
	private:
		CommandHistoryLimits m_Limits;
		Scope<State> m_State;
	};

}
