#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// An atomic group of commands (§12.3): edit.batch, validator fixes and every transaction (EditorTransaction). One undo
	// step for all of them.
	//
	// Execute runs the children in order; if child k fails, children k-1 .. 0 are undone in reverse order, the composite
	// returns child k's error (with the context "in step <k> '<label>'") and the editor is exactly as before (Roadmap M4
	// "CompositeCommand: a failing child undoes executed children").
	//
	// Undo undoes the children in reverse order. If child k's Undo fails (an external failure, Command.h), the children it
	// already undid are executed again, so the composite stays applied, and child k's error is returned with the context
	// "undoing step <k> '<label>'". If a compensating step fails too (an Undo during Execute's rollback, an Execute during
	// Undo's compensation), the composite stops there and remembers which children are applied (GetAppliedCount), so a
	// later Execute or Undo continues from that point; the returned error carries both failures and is logged at Error.
	// Every child is atomic, so the scene is valid throughout.
	class CompositeCommand final : public Command
	{
	public:
		explicit CompositeCommand(std::string label);

		// Appends a child that has not been executed (Execute will run it).
		void Add(Scope<Command> child);
		// Appends a child that has already been executed on the current state (a transaction executes children one by one);
		// a composite holding executed children is in its applied state, and its first Execute changes nothing. A composite
		// holds only unexecuted or only executed children (asserted).
		void AddExecuted(Scope<Command> child);

		[[nodiscard]] Status Execute(EditorContext& context) override;
		[[nodiscard]] Status Undo(EditorContext& context) override;
		[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
		// True when any child changes the scene.
		[[nodiscard]] bool ChangesScene() const override;
		// The sum of the children's sizes.
		[[nodiscard]] size_t GetMemorySize() const override;
		// Replays every child in order (`after` true) or in reverse order (`after` false). Errors: a child's, with the context
		// "replaying step <k> '<label>'"; InvalidState when only some children are applied (after a failed compensating step).
		[[nodiscard]] Status ReplayOnSceneCopy(Scene& scene, bool after) const override;

		[[nodiscard]] size_t GetChildCount() const { return m_Children.size(); }
		[[nodiscard]] const Command& GetChild(size_t index) const;
		[[nodiscard]] bool IsEmpty() const { return m_Children.empty(); }
		// How many children, from the first, are applied: none for unexecuted children (Add) and after a successful Undo,
		// all of them after AddExecuted and a successful Execute, and a number in between only after a failed compensating
		// step (see the class comment).
		[[nodiscard]] size_t GetAppliedCount() const { return m_AppliedCount; }
	private:
		std::string m_Label;
		std::vector<Scope<Command>> m_Children;
		size_t m_AppliedCount = 0;
	};

}
