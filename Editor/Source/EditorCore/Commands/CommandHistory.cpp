#include "EditorPCH.h"
#include "EditorCore/Commands/CommandHistory.h"

#include "EditorCore/EditorContext.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements the undo history.

namespace Engine {

	struct CommandHistory::State
	{
		struct Record
		{
			CommandHistoryEntry Entry{};
			Scope<Command> Command{};
		};

		std::vector<Record> Records{}; // oldest first; the applied ones, then the redo branch
		size_t Position = 0;           // how many records are applied
		// The Sequence of the newest applied record when the scene was saved (0: none applied); nullopt when that state is
		// no longer reachable.
		std::optional<uint64_t> SavePoint = 0;
		uint64_t NextSequence = 1;
		size_t MemorySize = 0;
	};

	CommandHistory::CommandHistory(CommandHistoryLimits limits)
		: m_Limits(limits), m_State(CreateScope<State>())
	{
	}

	CommandHistory::~CommandHistory() = default;

	CommandHistory::CommandHistory(CommandHistory&& other) noexcept = default;

	CommandHistory& CommandHistory::operator=(CommandHistory&& other) noexcept = default;

	Result<uint64_t> CommandHistory::Execute(Scope<Command> /*command*/, EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CommandHistory::Execute is an M4 contract stub");
	}

	uint64_t CommandHistory::RecordExecuted(Scope<Command> /*command*/, EditorContext& /*context*/, uint64_t /*revisionBefore*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<size_t> CommandHistory::Undo(EditorContext& /*context*/, size_t /*steps*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CommandHistory::Undo is an M4 contract stub");
	}

	Result<size_t> CommandHistory::Redo(EditorContext& /*context*/, size_t /*steps*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CommandHistory::Redo is an M4 contract stub");
	}

	bool CommandHistory::CanUndo() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool CommandHistory::CanRedo() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::string CommandHistory::GetUndoLabel() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string CommandHistory::GetRedoLabel() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t CommandHistory::GetCurrentSequence() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<CommandHistoryEntry> CommandHistory::GetEntries(size_t /*limit*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	size_t CommandHistory::GetUndoCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t CommandHistory::GetRedoCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const Command* CommandHistory::FindCommand(uint64_t /*sequence*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const CommandHistoryEntry* CommandHistory::FindEntry(uint64_t /*sequence*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	void CommandHistory::MarkSavePoint()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool CommandHistory::IsDirty() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void CommandHistory::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t CommandHistory::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
