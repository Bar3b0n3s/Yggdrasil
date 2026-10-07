#include "EditorPCH.h"
#include "EditorCore/Commands/AssetMoveCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the command.

namespace Engine {

	AssetMoveCommand::AssetMoveCommand(std::string label, std::vector<AssetFileMove> moves)
		: m_Label(std::move(label)), m_Moves(std::move(moves))
	{
		ENGINE_ASSERT(!m_Moves.empty(), "AssetMoveCommand '{}' needs at least one entry", m_Label);
	}

	Result<Scope<AssetMoveCommand>> AssetMoveCommand::Create(const EditorContext& /*context*/, AssetHandle /*handle*/,
		const VfsPath& /*newSourcePath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetMoveCommand::Create is an M6 contract stub");
	}

	Status AssetMoveCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetMoveCommand::Execute is an M6 contract stub");
	}

	Status AssetMoveCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetMoveCommand::Undo is an M6 contract stub");
	}

	size_t AssetMoveCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
