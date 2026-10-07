#include "EditorPCH.h"
#include "EditorCore/Commands/AssetDeleteCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the command.

namespace Engine {

	AssetDeleteCommand::AssetDeleteCommand(std::string label, std::vector<AssetFileMove> moves)
		: m_Label(std::move(label)), m_Moves(std::move(moves))
	{
		ENGINE_ASSERT(!m_Moves.empty(), "AssetDeleteCommand '{}' needs at least one entry", m_Label);
	}

	Result<Scope<AssetDeleteCommand>> AssetDeleteCommand::Create(const EditorContext& /*context*/, AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetDeleteCommand::Create is an M6 contract stub");
	}

	Result<VfsPath> AssetDeleteCommand::ChooseTrashDirectory(const EditorContext& /*context*/, AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetDeleteCommand::ChooseTrashDirectory is an M6 contract stub");
	}

	Status AssetDeleteCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetDeleteCommand::Execute is an M6 contract stub");
	}

	Status AssetDeleteCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetDeleteCommand::Undo is an M6 contract stub");
	}

	size_t AssetDeleteCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
