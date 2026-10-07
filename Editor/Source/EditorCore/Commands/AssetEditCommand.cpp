#include "EditorPCH.h"
#include "EditorCore/Commands/AssetEditCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the command.

namespace Engine {

	AssetEditCommand::AssetEditCommand(std::string label, std::vector<AssetFileEdit> edits)
		: m_Label(std::move(label)), m_Edits(std::move(edits))
	{
		ENGINE_ASSERT(!m_Edits.empty(), "AssetEditCommand '{}' needs at least one entry", m_Label);
	}

	Result<Scope<AssetEditCommand>> AssetEditCommand::CreateForWrite(const EditorContext& /*context*/, const VfsPath& /*path*/,
		std::span<const std::byte> /*bytes*/, std::string /*label*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetEditCommand::CreateForWrite is an M6 contract stub");
	}

	Result<Scope<AssetEditCommand>> AssetEditCommand::CreateForWrites(const EditorContext& /*context*/,
		std::span<const std::pair<VfsPath, Buffer>> /*files*/, std::string /*label*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetEditCommand::CreateForWrites is an M6 contract stub");
	}

	Status AssetEditCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetEditCommand::Execute is an M6 contract stub");
	}

	Status AssetEditCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetEditCommand::Undo is an M6 contract stub");
	}

	size_t AssetEditCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
