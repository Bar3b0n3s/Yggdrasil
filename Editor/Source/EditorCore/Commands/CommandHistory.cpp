#include "EditorPCH.h"
#include "EditorCore/Commands/CommandHistory.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

namespace Engine {

	struct CommandHistory::State
	{
		struct Record
		{
			CommandHistoryEntry Entry{};
			Scope<Engine::Command> Command{}; // qualified: the member's name would change the meaning of `Command` (GCC)
		};

		std::vector<Record> Records{}; // oldest first; the applied ones, then the redo branch
		size_t Position = 0;           // how many records are applied
		// The Position at which the open scene was last saved (0: before every held record); nullopt when that state can
		// no longer be reached by undo and redo (its records were discarded or bounded away, or merged past).
		std::optional<size_t> SavePosition = 0;
		uint64_t NextSequence = 1;
		size_t MemorySize = 0;

		// Erases the redo branch (the records after Position).
		void DiscardRedoBranch()
		{
			if (Position == Records.size())
				return;
			for (size_t index = Position; index < Records.size(); ++index)
				MemorySize -= Records[index].Entry.MemorySize;
			Records.erase(Records.begin() + static_cast<std::ptrdiff_t>(Position), Records.end());
			if (SavePosition.has_value() && *SavePosition > Position)
				SavePosition.reset(); // the saved state was on the discarded branch
		}

		// Appends an applied record after the current position (the redo branch is already gone).
		uint64_t Append(Scope<Engine::Command> command, uint64_t revisionBefore, uint64_t revisionAfter)
		{
			ENGINE_ASSERT(Position == Records.size(), "CommandHistory: a record is appended only after the redo branch is gone");
			const CommandOrigin origin = command->GetOrigin();
			Record record;
			record.Entry.Sequence = NextSequence++;
			record.Entry.Label = origin == CommandOrigin::Agent ? std::format("[agent] {}", command->GetLabel()) : std::string(command->GetLabel());
			record.Entry.Origin = origin;
			record.Entry.RevisionBefore = revisionBefore;
			record.Entry.RevisionAfter = revisionAfter;
			record.Entry.MemorySize = command->GetMemorySize();
			record.Entry.ChangesScene = command->ChangesScene();
			record.Command = std::move(command);
			MemorySize += record.Entry.MemorySize;
			const uint64_t sequence = record.Entry.Sequence;
			Records.push_back(std::move(record));
			++Position;
			return sequence;
		}

		// Drops the oldest records while the history exceeds a bound. The newest record always stays, so a single command
		// larger than MaxBytes is still undoable.
		void EnforceLimits(const CommandHistoryLimits& limits)
		{
			size_t dropCount = 0;
			size_t droppedBytes = 0;
			while (Records.size() - dropCount > 1 && (Records.size() - dropCount > limits.MaxEntries || MemorySize - droppedBytes > limits.MaxBytes))
			{
				droppedBytes += Records[dropCount].Entry.MemorySize;
				++dropCount;
			}
			if (dropCount == 0)
				return;
			Records.erase(Records.begin(), Records.begin() + static_cast<std::ptrdiff_t>(dropCount));
			MemorySize -= droppedBytes;
			// Dropped applied records stay applied: their effects are now part of the oldest reachable state.
			Position -= std::min(Position, dropCount);
			if (SavePosition.has_value())
			{
				if (*SavePosition < dropCount)
					SavePosition.reset(); // the saved state lay before a dropped record
				else
					*SavePosition -= dropCount;
			}
		}

		[[nodiscard]] const Record* Find(uint64_t sequence) const
		{
			const auto found = std::lower_bound(Records.begin(), Records.end(), sequence, [](const Record& record, uint64_t value)
			{
				return record.Entry.Sequence < value;
			});
			return found != Records.end() && found->Entry.Sequence == sequence ? &*found : nullptr;
		}
	};

	CommandHistory::CommandHistory(CommandHistoryLimits limits)
		: m_Limits(limits), m_State(CreateScope<State>())
	{
		ENGINE_ASSERT(m_Limits.MaxEntries >= 1, "CommandHistory needs room for at least one entry");
	}

	CommandHistory::~CommandHistory() = default;

	CommandHistory::CommandHistory(CommandHistory&& other) noexcept = default;

	CommandHistory& CommandHistory::operator=(CommandHistory&& other) noexcept = default;

	Result<uint64_t> CommandHistory::Execute(Scope<Command> command, EditorContext& context)
	{
		ENGINE_ASSERT(command != nullptr, "CommandHistory::Execute needs a command");
		const uint64_t revisionBefore = context.GetRevisionBeforeCommand();
		ENGINE_TRY(command->Execute(context));
		const uint64_t revisionAfter = context.GetRevision();

		State& state = *m_State;
		state.DiscardRedoBranch();

		// Continuous edits: the newest entry absorbs a command of the same origin and merge key (Command::MergeWith).
		const std::string_view mergeKey = command->GetMergeKey();
		if (state.Position > 0 && !mergeKey.empty())
		{
			State::Record& newest = state.Records[state.Position - 1];
			if (newest.Command->GetOrigin() == command->GetOrigin() && newest.Command->GetMergeKey() == mergeKey && newest.Command->MergeWith(*command))
			{
				state.MemorySize -= newest.Entry.MemorySize;
				newest.Entry.MemorySize = newest.Command->GetMemorySize();
				state.MemorySize += newest.Entry.MemorySize;
				newest.Entry.RevisionAfter = revisionAfter;
				newest.Entry.ChangesScene = newest.Command->ChangesScene();
				// The state between the two merged commands can no longer be reached.
				if (state.SavePosition == state.Position && command->ChangesScene())
					state.SavePosition.reset();
				state.EnforceLimits(m_Limits);
				return newest.Entry.Sequence;
			}
		}

		const uint64_t sequence = state.Append(std::move(command), revisionBefore, revisionAfter);
		state.EnforceLimits(m_Limits);
		return sequence;
	}

	uint64_t CommandHistory::RecordExecuted(Scope<Command> command, EditorContext& context, uint64_t revisionBefore)
	{
		ENGINE_ASSERT(command != nullptr, "CommandHistory::RecordExecuted needs a command");
		State& state = *m_State;
		state.DiscardRedoBranch();
		const uint64_t sequence = state.Append(std::move(command), revisionBefore, context.GetRevision());
		state.EnforceLimits(m_Limits);
		return sequence;
	}

	Result<size_t> CommandHistory::Undo(EditorContext& context, size_t steps)
	{
		ENGINE_ASSERT(steps >= 1, "CommandHistory::Undo needs at least one step");
		State& state = *m_State;
		size_t undone = 0;
		while (undone < steps && state.Position > 0)
		{
			State::Record& record = state.Records[state.Position - 1];
			if (Status status = record.Command->Undo(context); !status)
			{
				return std::unexpected(std::move(status).error().WithContext(
					std::format("undoing '{}' after {} of {} requested step(s) were undone", record.Entry.Label, undone, steps)));
			}
			--state.Position;
			++undone;
		}
		return undone;
	}

	Result<size_t> CommandHistory::Redo(EditorContext& context, size_t steps)
	{
		ENGINE_ASSERT(steps >= 1, "CommandHistory::Redo needs at least one step");
		State& state = *m_State;
		size_t redone = 0;
		while (redone < steps && state.Position < state.Records.size())
		{
			State::Record& record = state.Records[state.Position];
			if (Status status = record.Command->Execute(context); !status)
			{
				return std::unexpected(std::move(status).error().WithContext(
					std::format("redoing '{}' after {} of {} requested step(s) were redone", record.Entry.Label, redone, steps)));
			}
			++state.Position;
			++redone;
		}
		return redone;
	}

	bool CommandHistory::CanUndo() const
	{
		return m_State->Position > 0;
	}

	bool CommandHistory::CanRedo() const
	{
		return m_State->Position < m_State->Records.size();
	}

	std::string CommandHistory::GetUndoLabel() const
	{
		return CanUndo() ? m_State->Records[m_State->Position - 1].Entry.Label : std::string();
	}

	std::string CommandHistory::GetRedoLabel() const
	{
		return CanRedo() ? m_State->Records[m_State->Position].Entry.Label : std::string();
	}

	uint64_t CommandHistory::GetCurrentSequence() const
	{
		return CanUndo() ? m_State->Records[m_State->Position - 1].Entry.Sequence : 0;
	}

	std::vector<CommandHistoryEntry> CommandHistory::GetEntries(size_t limit) const
	{
		const std::vector<State::Record>& records = m_State->Records;
		const size_t first = records.size() - std::min(limit, records.size());
		std::vector<CommandHistoryEntry> entries;
		entries.reserve(records.size() - first);
		for (size_t index = first; index < records.size(); ++index)
			entries.push_back(records[index].Entry);
		return entries;
	}

	size_t CommandHistory::GetUndoCount() const
	{
		return m_State->Position;
	}

	size_t CommandHistory::GetRedoCount() const
	{
		return m_State->Records.size() - m_State->Position;
	}

	const Command* CommandHistory::FindCommand(uint64_t sequence) const
	{
		const State::Record* record = m_State->Find(sequence);
		return record != nullptr ? record->Command.get() : nullptr;
	}

	const CommandHistoryEntry* CommandHistory::FindEntry(uint64_t sequence) const
	{
		const State::Record* record = m_State->Find(sequence);
		return record != nullptr ? &record->Entry : nullptr;
	}

	void CommandHistory::MarkSavePoint()
	{
		m_State->SavePosition = m_State->Position;
	}

	bool CommandHistory::IsDirty() const
	{
		const State& state = *m_State;
		if (!state.SavePosition.has_value())
			return true;
		const size_t from = std::min(*state.SavePosition, state.Position);
		const size_t to = std::max(*state.SavePosition, state.Position);
		for (size_t index = from; index < to; ++index)
		{
			if (state.Records[index].Entry.ChangesScene)
				return true;
		}
		return false;
	}

	void CommandHistory::Clear()
	{
		// Sequences keep increasing: an undo index reported before the clear never names a later command.
		State& state = *m_State;
		state.Records.clear();
		state.Position = 0;
		state.SavePosition = 0;
		state.MemorySize = 0;
	}

	size_t CommandHistory::GetMemorySize() const
	{
		return m_State->MemorySize;
	}

}
