#include "EditorPCH.h"
#include "EditorCore/Commands/AssetDeleteCommand.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/AssetFileMoves.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

namespace Engine {

	AssetDeleteCommand::AssetDeleteCommand(std::string label, std::vector<AssetFileMove> moves)
		: m_Label(std::move(label)), m_Moves(std::move(moves))
	{
		ENGINE_ASSERT(!m_Moves.empty(), "AssetDeleteCommand '{}' needs at least one move", m_Label);
	}

	Result<Scope<AssetDeleteCommand>> AssetDeleteCommand::Create(const EditorContext& context, AssetHandle handle)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "no asset can be deleted: no project is open");
		const EditorAssetManager& assets = context.GetAssets();
		const std::string source = Utils::GetAssetSourceText(assets, handle);
		ENGINE_TRY_ASSIGN(const VfsPath trash, ChooseTrashDirectory(context, handle));
		ENGINE_TRY_ASSIGN(std::vector<AssetFileMove> moves, assets.GetRegistry().PlanTrash(handle, trash));
		// A reference by path does not survive the delete: say so, and delete anyway (ADR 0010 decision 19).
		if (const std::vector<AssetHandle> dependents = assets.GetPathDependents(handle); !dependents.empty())
		{
			ENGINE_WARN("Deleting '{}': {} {} it by its path and will fail to import until the delete is undone", source,
				Utils::FormatAssetList(assets, dependents), dependents.size() == 1 ? "finds" : "find");
		}
		return CreateScope<AssetDeleteCommand>(std::format("Delete Asset '{}'", source), std::move(moves));
	}

	Result<VfsPath> AssetDeleteCommand::ChooseTrashDirectory(const EditorContext& context, AssetHandle handle)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "there is no trash: no project is open");
		const VirtualFileSystem& vfs = context.GetVfs();
		const std::string entry = std::format("Library/Trash/{}", handle.ToString());
		ENGINE_TRY_ASSIGN(VfsPath directory, VfsPath::Create("project", entry));
		for (uint32_t suffix = 2; vfs.Exists(directory); ++suffix)
		{
			ENGINE_TRY_ASSIGN(VfsPath next, VfsPath::Create("project", std::format("{}-{}", entry, suffix)));
			directory = std::move(next);
		}
		return directory;
	}

	Status AssetDeleteCommand::Execute(EditorContext& context)
	{
		ENGINE_TRY_ASSIGN(m_CreatedDirectories, Utils::ApplyAssetFileMoves(context, m_Moves, m_Label));
		return {};
	}

	Status AssetDeleteCommand::Undo(EditorContext& context)
	{
		ENGINE_TRY(Utils::RevertAssetFileMoves(context, m_Moves, m_CreatedDirectories, m_Label));
		m_CreatedDirectories.clear();
		return {};
	}

	size_t AssetDeleteCommand::GetMemorySize() const
	{
		size_t size = sizeof(*this) + m_Label.size();
		for (const AssetFileMove& move : m_Moves)
			size += sizeof(AssetFileMove) + move.From.ToString().size() + move.To.ToString().size();
		for (const VfsPath& directory : m_CreatedDirectories)
			size += sizeof(VfsPath) + directory.ToString().size();
		return size;
	}

}
