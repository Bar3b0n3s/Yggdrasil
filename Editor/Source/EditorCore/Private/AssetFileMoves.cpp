#include "EditorPCH.h"
#include "EditorCore/Private/AssetFileMoves.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <algorithm>
#include <functional>

namespace Engine {

	namespace Utils {

		// Removes each of `directories` (deepest first) while it is empty: the folders a set of renames created.
		static void RemoveCreatedDirectories(EditorContext& context, std::span<const VfsPath> directories, std::string_view label)
		{
			for (const VfsPath& directory : directories)
			{
				const Result<std::vector<VfsEntry>> entries = context.GetVfs().List(directory, false);
				if (!entries.has_value() || !entries->empty())
					continue; // gone already, or something else was put into it: it stays
				if (Status removed = context.RemoveProjectFile(directory); !removed)
					ENGINE_WARN("'{}' could not remove the folder '{}' it created: {}", label, directory.ToString(), removed.error().ToString());
			}
		}

		Result<std::vector<VfsPath>> ApplyAssetFileMoves(EditorContext& context, std::span<const AssetFileMove> moves, std::string_view label)
		{
			std::vector<VfsPath> created;
			for (size_t index = 0; index < moves.size(); ++index)
			{
				// The destination's missing folders, which MoveProjectFile creates.
				std::vector<VfsPath> missing;
				for (VfsPath directory = moves[index].To.GetParent(); !directory.IsRoot() && !context.GetVfs().Exists(directory); directory = directory.GetParent())
					missing.push_back(directory);
				Status moved = context.MoveProjectFile(moves[index].From, moves[index].To);
				if (moved)
				{
					created.insert(created.end(), missing.begin(), missing.end());
					continue;
				}
				for (size_t restored = index; restored > 0; --restored)
				{
					const AssetFileMove& move = moves[restored - 1];
					if (Status back = context.MoveProjectFile(move.To, move.From); !back)
						ENGINE_ERROR("'{}' could not move '{}' back after a failed move: {}", label, move.To.ToString(), back.error().ToString());
				}
				std::ranges::stable_sort(created, std::greater<>(), [](const VfsPath& directory)
				{
					return directory.GetPath().size();
				});
				RemoveCreatedDirectories(context, created, label);
				return std::unexpected(std::move(moved).error().WithContext(std::format("executing '{}'", label)));
			}
			// Deepest first: a folder's path is longer than its parent's.
			std::ranges::stable_sort(created, std::greater<>(), [](const VfsPath& directory)
			{
				return directory.GetPath().size();
			});
			return created;
		}

		Status RevertAssetFileMoves(EditorContext& context, std::span<const AssetFileMove> moves, std::span<const VfsPath> createdDirectories,
			std::string_view label)
		{
			for (size_t index = moves.size(); index > 0; --index)
			{
				Status moved = context.MoveProjectFile(moves[index - 1].To, moves[index - 1].From);
				if (moved)
					continue;
				for (size_t reapplied = index; reapplied < moves.size(); ++reapplied)
				{
					const AssetFileMove& move = moves[reapplied];
					if (Status again = context.MoveProjectFile(move.From, move.To); !again)
						ENGINE_ERROR("'{}' could not move '{}' again after a failed undo: {}", label, move.From.ToString(), again.error().ToString());
				}
				return std::unexpected(std::move(moved).error().WithContext(std::format("undoing '{}'", label)));
			}
			RemoveCreatedDirectories(context, createdDirectories, label);
			return {};
		}

		std::string FormatAssetList(const EditorAssetManager& assets, std::span<const AssetHandle> handles)
		{
			std::string text;
			for (const AssetHandle handle : handles)
			{
				if (!text.empty())
					text += ", ";
				const std::string path = assets.GetReferencePath(handle);
				text += std::format("'{}'", path.empty() ? handle.ToString() : path);
			}
			return text;
		}

		std::string GetAssetSourceText(const EditorAssetManager& assets, AssetHandle handle)
		{
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (record == nullptr)
				return handle.ToString();
			return std::string(record->SourcePath.GetPath());
		}

	}

}
