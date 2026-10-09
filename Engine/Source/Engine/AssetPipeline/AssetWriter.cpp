#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetWriter.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/PollingFileWatcher.h"

#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Utils {

		// The suffix of the one backup a mount that keeps backups writes beside a file it replaces (FileSystem::WriteFileAtomic,
		// §4.10).
		static constexpr std::string_view BackupSuffix = ".bak";

	}

	struct AssetWriter::State
	{
		VirtualFileSystem* Vfs = nullptr;      // documented back-reference
		PollingFileWatcher* Watcher = nullptr; // documented back-reference, cleared by its owner
		Listener WriteListener;
		MutationGuard Guard{};
		bool IsDryRun = false;

		// The watcher that describes the disk, or nullptr: none is set, or a dry run writes into the overlay.
		[[nodiscard]] PollingFileWatcher* GetActiveWatcher() const { return IsDryRun ? nullptr : Watcher; }

		// The files at or below `path` (a file is itself, a directory every file below it), sorted; empty when `path` does not
		// exist or cannot be listed (the operation that follows then reports the error).
		[[nodiscard]] std::vector<VfsPath> ListFiles(const VfsPath& path) const
		{
			const Result<FileInfo> info = Vfs->GetInfo(path);
			if (!info)
				return {};
			if (!info->IsDirectory)
				return { path };
			std::vector<VfsPath> files;
			const Result<std::vector<VfsEntry>> entries = Vfs->List(path, true);
			if (!entries)
				return {};
			for (const VfsEntry& entry : *entries)
			{
				if (!entry.Info.IsDirectory)
					files.push_back(entry.Path);
			}
			return files;
		}

		// Records the current state of each of `paths` under the watcher's root as known (race rule 1). A failure is logged
		// at Warn: the change then comes back once as an external change, which is harmless.
		void MarkKnown(std::span<const VfsPath> paths) const
		{
			PollingFileWatcher* watcher = GetActiveWatcher();
			if (watcher == nullptr)
				return;
			const VfsPath& root = watcher->GetSpecification().Root;
			for (const VfsPath& path : paths)
			{
				if (!path.IsUnder(root))
					continue;
				if (Status marked = watcher->MarkKnown(path); !marked)
				{
					ENGINE_CORE_WARN("The hot-reload watcher could not record '{}' as written by the editor ({}); it will be reimported once",
						path.ToString(), marked.error().ToString());
				}
			}
		}

		void Report(const AssetWriteEvent& event)
		{
			if (WriteListener)
				WriteListener(event);
		}
	};

	AssetWriter::AssetWriter(VirtualFileSystem& vfs)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
	}

	AssetWriter::~AssetWriter() = default;

	void AssetWriter::SetWatcher(PollingFileWatcher* watcher)
	{
		m_State->Watcher = watcher;
	}

	void AssetWriter::SetListener(Listener listener)
	{
		m_State->WriteListener = std::move(listener);
	}

	void AssetWriter::SetMutationGuard(MutationGuard guard)
	{
		m_State->Guard = std::move(guard);
	}

	void AssetWriter::SetDryRun(bool dryRun)
	{
		m_State->IsDryRun = dryRun;
	}

	bool AssetWriter::IsDryRun() const
	{
		return m_State->IsDryRun;
	}

	Status AssetWriter::Write(const VfsPath& path, std::span<const std::byte> data)
	{
		if (m_State->Guard)
			ENGINE_TRY(m_State->Guard());
		const bool replaces = m_State->Vfs->Exists(path);
		ENGINE_TRY(m_State->Vfs->WriteFileAtomic(path, data));

		// A mount that keeps backups (project://, §4.10) copied the replaced file to "<path>.bak": the editor wrote that file
		// too, so it must not come back as an external change either.
		std::vector<VfsPath> written = { path };
		VfsPath backup;
		if (replaces)
		{
			Result<VfsPath> candidate = VfsPath::Create(path.GetScheme(), std::string(path.GetPath()) + std::string(Utils::BackupSuffix));
			if (candidate && m_State->Vfs->Exists(*candidate))
			{
				backup = *candidate;
				written.push_back(std::move(*candidate));
			}
		}
		m_State->MarkKnown(written);
		m_State->Report(AssetWriteEvent{
			.Kind = AssetWriteKind::Written,
			.Path = path,
			.From = {},
			.ContentHash = XXH64(data),
			.Backup = std::move(backup),
		});
		return {};
	}

	Status AssetWriter::Remove(const VfsPath& path)
	{
		if (m_State->Guard)
			ENGINE_TRY(m_State->Guard());
		const std::vector<VfsPath> files = m_State->ListFiles(path);
		ENGINE_TRY(m_State->Vfs->Remove(path));
		m_State->MarkKnown(files);
		m_State->Report(AssetWriteEvent{ .Kind = AssetWriteKind::Removed, .Path = path, .From = {}, .ContentHash = 0, .Backup = {} });
		return {};
	}

	Status AssetWriter::Move(const VfsPath& from, const VfsPath& to)
	{
		if (m_State->Guard)
			ENGINE_TRY(m_State->Guard());
		if (from.GetScheme() != to.GetScheme())
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' to '{}': both paths must have the same scheme", from.ToString(), to.ToString());
		const std::vector<VfsPath> files = m_State->ListFiles(from);
		if (const VfsPath parent = to.GetParent(); !parent.IsRoot() && !m_State->Vfs->Exists(parent))
			ENGINE_TRY(m_State->Vfs->CreateDirectories(parent));
		ENGINE_TRY(m_State->Vfs->Move(from, to));

		// The old and the new path of every file moved: each old path is now absent, each new one holds the file.
		std::vector<VfsPath> moved;
		moved.reserve(files.size() * 2);
		const size_t fromLength = from.GetPath().size();
		for (const VfsPath& file : files)
		{
			moved.push_back(file);
			if (file == from)
			{
				moved.push_back(to);
				continue;
			}
			// A file below the moved directory: its path relative to the directory, after the separator.
			Result<VfsPath> destination = to.Join(file.GetPath().substr(fromLength + 1));
			if (destination)
				moved.push_back(std::move(*destination));
		}
		m_State->MarkKnown(moved);
		m_State->Report(AssetWriteEvent{ .Kind = AssetWriteKind::Moved, .Path = to, .From = from, .ContentHash = 0, .Backup = {} });
		return {};
	}

	Status AssetWriter::CreateDirectories(const VfsPath& directory)
	{
		if (m_State->Guard)
			ENGINE_TRY(m_State->Guard());
		ENGINE_TRY(m_State->Vfs->CreateDirectories(directory));
		m_State->Report(AssetWriteEvent{ .Kind = AssetWriteKind::DirectoryCreated, .Path = directory, .From = {}, .ContentHash = 0, .Backup = {} });
		return {};
	}

}
