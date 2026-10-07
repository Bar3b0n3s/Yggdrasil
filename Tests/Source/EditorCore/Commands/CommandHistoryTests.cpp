#include "TestsPCH.h"

#include "EditorCore/Commands/CommandHistory.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/EditorContext.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	namespace {

		// Adds `Delta` to a counter the test owns; fails when told to. Its label names the delta.
		class CounterCommand final : public Command
		{
		public:
			CounterCommand(int& counter, int delta, std::string mergeKey = {}, size_t memorySize = 64, bool changesScene = true)
				: m_Counter(&counter), m_Delta(delta), m_MergeKey(std::move(mergeKey)), m_MemorySize(memorySize), m_ChangesScene(changesScene), m_Label(std::format("Add {}", delta))
			{
			}

			Status Execute(EditorContext& /*context*/) override
			{
				if (FailExecute)
					return MakeError(ErrorCode::InvalidState, "told to fail");
				*m_Counter += m_Delta;
				return {};
			}

			Status Undo(EditorContext& /*context*/) override
			{
				if (FailUndo)
					return MakeError(ErrorCode::Io, "told to fail undoing");
				*m_Counter -= m_Delta;
				return {};
			}

			std::string_view GetLabel() const override { return m_Label; }
			std::string_view GetMergeKey() const override { return m_MergeKey; }

			bool MergeWith(const Command& next) override
			{
				m_Delta += static_cast<const CounterCommand&>(next).m_Delta;
				return true;
			}

			bool ChangesScene() const override { return m_ChangesScene; }
			size_t GetMemorySize() const override { return m_MemorySize; }
		public:
			bool FailExecute = false;
			bool FailUndo = false;
		private:
			int* m_Counter = nullptr;
			int m_Delta = 0;
			std::string m_MergeKey;
			size_t m_MemorySize = 0;
			bool m_ChangesScene = true;
			std::string m_Label;
		};

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CommandHistory: execute, undo and redo move one position each")
		{
			Test::EditorTestFixture fixture("History");
			EditorContext& editor = fixture.GetEditor();
			CommandHistory history;
			int counter = 0;

			const Result<uint64_t> first = history.Execute(CreateScope<CounterCommand>(counter, 1), editor);
			const Result<uint64_t> second = history.Execute(CreateScope<CounterCommand>(counter, 10), editor);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(*first == 1);
			CHECK(*second == 2);
			CHECK(counter == 11);
			CHECK(history.GetUndoLabel() == "Add 10");
			CHECK(history.GetCurrentSequence() == 2);

			CHECK(history.Undo(editor) == 1);
			CHECK(counter == 1);
			CHECK(history.GetRedoLabel() == "Add 10");
			CHECK(history.GetCurrentSequence() == 1);
			const Result<size_t> redone = history.Redo(editor);
			REQUIRE(redone.has_value());
			CHECK(*redone == 1);
			CHECK(counter == 11);

			CHECK(history.Undo(editor, 5) == 2);
			CHECK(counter == 0);
			CHECK_FALSE(history.CanUndo());
			CHECK(history.CanRedo());
		}

		TEST_CASE("CommandHistory: a new command discards the redo branch and keeps increasing sequences")
		{
			Test::EditorTestFixture fixture("HistoryBranch");
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1), fixture.GetEditor()).has_value());
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 2), fixture.GetEditor()).has_value());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			const Result<uint64_t> branch = history.Execute(CreateScope<CounterCommand>(counter, 5), fixture.GetEditor());
			REQUIRE(branch.has_value());
			CHECK(*branch == 3);
			CHECK_FALSE(history.CanRedo());
			CHECK(history.FindCommand(2) == nullptr);
			CHECK(history.GetEntries(10).size() == 2);
			CHECK(counter == 6);
		}

		TEST_CASE("CommandHistory: the oldest entries are dropped beyond the entry and byte bounds")
		{
			Test::EditorTestFixture fixture("HistoryBounds");
			int counter = 0;
			CommandHistory byEntries(CommandHistoryLimits{ .MaxEntries = 3, .MaxBytes = 1u << 30 });
			for (int index = 1; index <= 5; ++index)
				REQUIRE(byEntries.Execute(CreateScope<CounterCommand>(counter, index), fixture.GetEditor()).has_value());
			CHECK(byEntries.GetUndoCount() == 3);
			CHECK(byEntries.FindEntry(2) == nullptr);
			CHECK(byEntries.FindEntry(3) != nullptr);
			CHECK(byEntries.Undo(fixture.GetEditor(), 10) == 3);
			CHECK(counter == 1 + 2); // the dropped commands stay applied

			CommandHistory byBytes(CommandHistoryLimits{ .MaxEntries = 1000, .MaxBytes = 1000 });
			for (int index = 0; index < 4; ++index)
				REQUIRE(byBytes.Execute(CreateScope<CounterCommand>(counter, 1, std::string(), 400), fixture.GetEditor()).has_value());
			CHECK(byBytes.GetUndoCount() == 2);
			CHECK(byBytes.GetMemorySize() <= 1000);
			CHECK(CommandHistoryLimits{}.MaxEntries == 1000);
			CHECK(CommandHistoryLimits{}.MaxBytes == 256ull * 1024 * 1024);
		}

		TEST_CASE("CommandHistory: the save point decides the dirty flag in both directions")
		{
			Test::EditorTestFixture fixture("HistoryDirty");
			CommandHistory history;
			int counter = 0;
			CHECK_FALSE(history.IsDirty());
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1), fixture.GetEditor()).has_value());
			CHECK(history.IsDirty());
			history.MarkSavePoint();
			CHECK_FALSE(history.IsDirty());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			CHECK(history.IsDirty());
			REQUIRE(history.Redo(fixture.GetEditor()).has_value());
			CHECK_FALSE(history.IsDirty());

			// A save point on a discarded redo branch can never be reached again.
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 2), fixture.GetEditor()).has_value());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			CHECK(history.IsDirty());
		}

		TEST_CASE("CommandHistory: commands that do not change the scene keep it clean")
		{
			Test::EditorTestFixture fixture("HistorySettings");
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1, std::string(), 64, false), fixture.GetEditor()).has_value());
			CHECK_FALSE(history.IsDirty());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			CHECK_FALSE(history.IsDirty());
		}

		TEST_CASE("CommandHistory: consecutive commands with the same merge key merge into one entry")
		{
			Test::EditorTestFixture fixture("HistoryMerge");
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1, "drag:a"), fixture.GetEditor()).has_value());
			const Result<uint64_t> merged = history.Execute(CreateScope<CounterCommand>(counter, 2, "drag:a"), fixture.GetEditor());
			REQUIRE(merged.has_value());
			CHECK(*merged == 1);
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 4, "drag:b"), fixture.GetEditor()).has_value());
			CHECK(history.GetUndoCount() == 2);
			CHECK(counter == 7);
			CHECK(history.Undo(fixture.GetEditor(), 2) == 2);
			CHECK(counter == 0);
		}

		TEST_CASE("CommandHistory: a failing Execute leaves the history unchanged")
		{
			Test::EditorTestFixture fixture("HistoryFailure");
			CommandHistory history;
			int counter = 0;
			Scope<CounterCommand> failing = CreateScope<CounterCommand>(counter, 3);
			failing->FailExecute = true;
			const Result<uint64_t> result = history.Execute(std::move(failing), fixture.GetEditor());
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::InvalidState);
			CHECK(history.GetUndoCount() == 0);
			CHECK(counter == 0);
		}

		TEST_CASE("CommandHistory: agent commands carry the [agent] label prefix")
		{
			Test::EditorTestFixture fixture("HistoryAgent");
			CommandHistory history;
			int counter = 0;
			Scope<CounterCommand> command = CreateScope<CounterCommand>(counter, 1);
			command->SetOrigin(CommandOrigin::Agent);
			REQUIRE(history.Execute(std::move(command), fixture.GetEditor()).has_value());
			CHECK(history.GetUndoLabel() == "[agent] Add 1");
			const std::vector<CommandHistoryEntry> entries = history.GetEntries(10);
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].Origin == CommandOrigin::Agent);
			CHECK(entries[0].Label == "[agent] Add 1");
		}

		TEST_CASE("CommandHistory: undo stops at a command whose Undo fails and leaves it applied")
		{
			Test::EditorTestFixture fixture("HistoryUndoFailure");
			EditorContext& editor = fixture.GetEditor();
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1), editor).has_value());
			Scope<CounterCommand> failing = CreateScope<CounterCommand>(counter, 10);
			failing->FailUndo = true;
			REQUIRE(history.Execute(std::move(failing), editor).has_value());
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 100), editor).has_value());

			const Result<size_t> undone = history.Undo(editor, 3);
			REQUIRE_FALSE(undone.has_value());
			CHECK(undone.error().GetCode() == ErrorCode::Io);
			CHECK(undone.error().ToString().contains("1")); // the number undone before the failure is in the context
			CHECK(counter == 11);                           // the newest was undone; the failing one and the oldest stay
			CHECK(history.GetUndoCount() == 2);
			CHECK(history.GetRedoCount() == 1);
			CHECK(history.GetUndoLabel() == "Add 10");
		}

		TEST_CASE("CommandHistory: RecordExecuted records an applied command without executing it")
		{
			Test::EditorTestFixture fixture("HistoryRecord");
			CommandHistory history;
			int counter = 5; // as if the command had already added 5
			const uint64_t sequence = history.RecordExecuted(CreateScope<CounterCommand>(counter, 5), fixture.GetEditor(), 7);
			CHECK(sequence == 1);
			CHECK(counter == 5);
			const CommandHistoryEntry* entry = history.FindEntry(sequence);
			REQUIRE(entry != nullptr);
			CHECK(entry->RevisionBefore == 7);
			REQUIRE(history.FindCommand(sequence) != nullptr);
			CHECK(history.FindCommand(sequence)->GetLabel() == "Add 5");
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			CHECK(counter == 0);
		}

		TEST_CASE("CommandHistory: entries end at the newest one, redo branch included")
		{
			Test::EditorTestFixture fixture("HistoryEntries");
			CommandHistory history;
			int counter = 0;
			for (int index = 1; index <= 4; ++index)
				REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, index), fixture.GetEditor()).has_value());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			const std::vector<CommandHistoryEntry> entries = history.GetEntries(2);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Label == "Add 3");
			CHECK(entries[1].Label == "Add 4"); // the undone one
			CHECK(history.GetUndoCount() == 3);
			CHECK(history.GetRedoCount() == 1);
			CHECK(history.GetEntries(100).size() == 4);
			CHECK(history.GetMemorySize() == 4 * 64);
		}

		TEST_CASE("CommandHistory: Clear empties the history and keeps sequences increasing")
		{
			Test::EditorTestFixture fixture("HistoryClear");
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1), fixture.GetEditor()).has_value());
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 2), fixture.GetEditor()).has_value());
			history.Clear();
			CHECK_FALSE(history.CanUndo());
			CHECK_FALSE(history.CanRedo());
			CHECK_FALSE(history.IsDirty());
			CHECK(history.GetMemorySize() == 0);
			CHECK(history.GetCurrentSequence() == 0);
			CHECK(history.Execute(CreateScope<CounterCommand>(counter, 3), fixture.GetEditor()) == 3u);
		}

		TEST_CASE("CommandHistory: a saved state that the bounds dropped makes the history dirty")
		{
			Test::EditorTestFixture fixture("HistoryBoundedSave");
			CommandHistory history(CommandHistoryLimits{ .MaxEntries = 2, .MaxBytes = 1u << 30 });
			int counter = 0;
			history.MarkSavePoint(); // the empty history
			for (int index = 1; index <= 3; ++index)
				REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, index), fixture.GetEditor()).has_value());
			CHECK(history.Undo(fixture.GetEditor(), 10) == 2);
			CHECK(history.IsDirty()); // the first command can no longer be undone, so the saved state is out of reach

			// A saved state that stays reachable stays exact when older entries are dropped.
			REQUIRE(history.Redo(fixture.GetEditor()).has_value());
			history.MarkSavePoint(); // after the second command
			REQUIRE(history.Redo(fixture.GetEditor()).has_value());
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 4), fixture.GetEditor()).has_value()); // drops the second
			CHECK(history.IsDirty());
			CHECK(history.Undo(fixture.GetEditor(), 10) == 2);
			CHECK_FALSE(history.IsDirty());
		}

		TEST_CASE("CommandHistory: merging past the save point makes the history dirty")
		{
			Test::EditorTestFixture fixture("HistoryMergeSave");
			CommandHistory history;
			int counter = 0;
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 1, "drag"), fixture.GetEditor()).has_value());
			history.MarkSavePoint();
			REQUIRE(history.Execute(CreateScope<CounterCommand>(counter, 2, "drag"), fixture.GetEditor()).has_value());
			CHECK(history.GetUndoCount() == 1);
			CHECK(history.IsDirty());
			CHECK(history.Undo(fixture.GetEditor()) == 1);
			CHECK(history.IsDirty()); // the saved state (after the first step only) is gone
		}
	}

}
