#include "EditorPCH.h"
#include "EditorCore/Commands/AssetEditCommand.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

namespace Engine {

	namespace Utils {

		// Brings `edit`'s path to `state` through the context's write path: a file is written (its folder must exist: a
		// command creates folders as entries of their own, so undo removes exactly what it created) or removed; a folder is
		// created or removed (it must be empty then). Nothing happens when the path is already in that state of existence and
		// `state` holds no bytes to write.
		static Status ApplyAssetFileState(EditorContext& context, const AssetFileEdit& edit, const std::optional<Buffer>& state)
		{
			VirtualFileSystem& vfs = context.GetVfs();
			if (edit.Kind == AssetFileKind::Directory)
			{
				if (state.has_value())
					return vfs.Exists(edit.Path) ? Status() : context.CreateProjectDirectory(edit.Path);
				return vfs.Exists(edit.Path) ? context.RemoveProjectFile(edit.Path) : Status();
			}
			if (!state.has_value())
				return vfs.Exists(edit.Path) ? context.RemoveProjectFile(edit.Path) : Status();
			if (const VfsPath parent = edit.Path.GetParent(); !parent.IsRoot() && !vfs.Exists(parent))
			{
				return std::unexpected(Error(ErrorCode::NotFound, std::format("cannot write '{}': the folder '{}' does not exist", edit.Path.ToString(), parent.ToString()))
						.WithHint("create the folder first, in the same command"));
			}
			return context.WriteProjectFile(edit.Path, *state);
		}

		// Reads the current state of `path` into `state` as an AssetFileEdit records it: the bytes of a file, an empty buffer
		// for a folder, nullopt when nothing is there. It fills the edit's member in place rather than returning an optional
		// buffer through a Result, which GCC's -O2 flow analysis misreads as a use of an uninitialized vector
		// (-Wmaybe-uninitialized). Errors: the read errors other than NotFound (an operating-system access failure as Io).
		static Status ReadAssetFileState(const VirtualFileSystem& vfs, const VfsPath& path, AssetFileKind& kind, std::optional<Buffer>& state)
		{
			state.reset();
			Result<FileInfo> info = vfs.GetInfo(path);
			if (!info)
			{
				if (info.error().GetCode() == ErrorCode::NotFound)
					return Status();
				return std::unexpected(ToEditorFileError(std::move(info).error()));
			}
			if (info->IsDirectory)
			{
				kind = AssetFileKind::Directory;
				state.emplace();
				return Status();
			}
			Result<Buffer> bytes = vfs.ReadFile(path);
			if (!bytes)
				return std::unexpected(ToEditorFileError(std::move(bytes).error()));
			state.emplace(std::move(*bytes));
			return Status();
		}

	}

	AssetEditCommand::AssetEditCommand(std::string label, std::vector<AssetFileEdit> edits)
		: m_Label(std::move(label)), m_Edits(std::move(edits))
	{
		ENGINE_ASSERT(!m_Edits.empty(), "AssetEditCommand '{}' needs at least one entry", m_Label);
	}

	Result<Scope<AssetEditCommand>> AssetEditCommand::CreateForWrite(const EditorContext& context, const VfsPath& path,
		std::span<const std::byte> bytes, std::string label)
	{
		const std::pair<VfsPath, Buffer> files[] = { { path, Buffer(bytes.begin(), bytes.end()) } };
		return CreateForWrites(context, files, std::move(label));
	}

	Result<Scope<AssetEditCommand>> AssetEditCommand::CreateForWrites(const EditorContext& context,
		std::span<const std::pair<VfsPath, Buffer>> files, std::string label)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "'{}' cannot run: no project is open", label);
		ENGINE_ASSERT(!files.empty(), "AssetEditCommand::CreateForWrites '{}' needs at least one file", label);
		std::vector<AssetFileEdit> edits;
		edits.reserve(files.size());
		for (const auto& [path, bytes] : files)
		{
			AssetFileEdit& edit = edits.emplace_back();
			edit.Path = path;
			edit.After.emplace(bytes);
			AssetFileKind kind = AssetFileKind::File;
			ENGINE_TRY(Utils::ReadAssetFileState(context.GetVfs(), path, kind, edit.Before));
			if (kind == AssetFileKind::Directory)
			{
				return std::unexpected(Error(ErrorCode::AlreadyExists, std::format("cannot write '{}': a folder has that name", path.ToString()))
						.WithHint("choose another file name"));
			}
		}
		return CreateScope<AssetEditCommand>(std::move(label), std::move(edits));
	}

	Status AssetEditCommand::Execute(EditorContext& context)
	{
		for (size_t index = 0; index < m_Edits.size(); ++index)
		{
			Status applied = Utils::ApplyAssetFileState(context, m_Edits[index], m_Edits[index].After);
			if (applied)
				continue;
			// Atomic (Command.h): the entries already written go back to their Before state, newest first.
			for (size_t restored = index; restored > 0; --restored)
			{
				const AssetFileEdit& edit = m_Edits[restored - 1];
				if (Status undone = Utils::ApplyAssetFileState(context, edit, edit.Before); !undone)
					ENGINE_ERROR("'{}' could not restore '{}' after a failed write: {}", m_Label, edit.Path.ToString(), undone.error().ToString());
			}
			return std::unexpected(std::move(applied).error().WithContext(std::format("executing '{}'", m_Label)));
		}
		return {};
	}

	Status AssetEditCommand::Undo(EditorContext& context)
	{
		for (size_t index = m_Edits.size(); index > 0; --index)
		{
			Status restored = Utils::ApplyAssetFileState(context, m_Edits[index - 1], m_Edits[index - 1].Before);
			if (restored)
				continue;
			// Atomic: the entries already restored get their After state again, so the command stays applied.
			for (size_t reapplied = index; reapplied < m_Edits.size(); ++reapplied)
			{
				const AssetFileEdit& edit = m_Edits[reapplied];
				if (Status redone = Utils::ApplyAssetFileState(context, edit, edit.After); !redone)
					ENGINE_ERROR("'{}' could not re-apply '{}' after a failed undo: {}", m_Label, edit.Path.ToString(), redone.error().ToString());
			}
			return std::unexpected(std::move(restored).error().WithContext(std::format("undoing '{}'", m_Label)));
		}
		return {};
	}

	size_t AssetEditCommand::GetMemorySize() const
	{
		size_t size = sizeof(*this) + m_Label.size();
		for (const AssetFileEdit& edit : m_Edits)
		{
			size += sizeof(AssetFileEdit) + edit.Path.ToString().size();
			size += edit.Before.has_value() ? edit.Before->size() : 0;
			size += edit.After.has_value() ? edit.After->size() : 0;
		}
		return size;
	}

}
