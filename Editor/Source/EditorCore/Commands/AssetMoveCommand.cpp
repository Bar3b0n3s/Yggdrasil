#include "EditorPCH.h"
#include "EditorCore/Commands/AssetMoveCommand.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/AssetFileMoves.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Assert.h"

namespace Engine {

	AssetMoveCommand::AssetMoveCommand(std::string label, std::vector<AssetFileMove> moves)
		: m_Label(std::move(label)), m_Moves(std::move(moves))
	{
		ENGINE_ASSERT(!m_Moves.empty(), "AssetMoveCommand '{}' needs at least one move", m_Label);
	}

	Result<Scope<AssetMoveCommand>> AssetMoveCommand::Create(const EditorContext& context, AssetHandle handle, const VfsPath& newSourcePath)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "no asset can move: no project is open");
		const EditorAssetManager& assets = context.GetAssets();
		const std::string source = Utils::GetAssetSourceText(assets, handle);
		// A reference by path (a glTF's URI to this standalone texture) would break: such a move is refused (ADR 0010
		// decision 19).
		if (const std::vector<AssetHandle> dependents = assets.GetPathDependents(handle); !dependents.empty())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' cannot move: {} {} it by its path", source, Utils::FormatAssetList(assets, dependents), dependents.size() == 1 ? "finds" : "find"))
					.WithHint("move the glTF together with it, or re-export it"));
		}
		ENGINE_TRY_ASSIGN(std::vector<AssetFileMove> moves, assets.GetRegistry().PlanMove(handle, newSourcePath));
		return CreateScope<AssetMoveCommand>(std::format("Move Asset '{}'", source), std::move(moves));
	}

	Status AssetMoveCommand::Execute(EditorContext& context)
	{
		ENGINE_TRY_ASSIGN(m_CreatedDirectories, Utils::ApplyAssetFileMoves(context, m_Moves, m_Label));
		return {};
	}

	Status AssetMoveCommand::Undo(EditorContext& context)
	{
		ENGINE_TRY(Utils::RevertAssetFileMoves(context, m_Moves, m_CreatedDirectories, m_Label));
		m_CreatedDirectories.clear();
		return {};
	}

	size_t AssetMoveCommand::GetMemorySize() const
	{
		size_t size = sizeof(*this) + m_Label.size();
		for (const AssetFileMove& move : m_Moves)
			size += sizeof(AssetFileMove) + move.From.ToString().size() + move.To.ToString().size();
		for (const VfsPath& directory : m_CreatedDirectories)
			size += sizeof(VfsPath) + directory.ToString().size();
		return size;
	}

}
