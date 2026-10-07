#include "EnginePCH.h"
#include "Engine/Platform/PollingFileWatcher.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"

#include <cmath>

// Each file has three states: the known one (last reported, baseline or MarkKnown), the one the latest scan observed,
// and, while the two may differ, the time of the scan that last saw the observed state change. A scan reads a file only
// when its size or modification time differs from the observed state, and moves the observed state on only when the
// content hash differs, so touching a file restarts nothing. When a file's debounce expires, the known and observed
// states are compared once and the net change, if any, is reported.
//
// Start and Poll hold m_ScanMutex throughout, so scans never overlap, but take m_StateMutex only between their VFS
// calls. MarkKnown does its own reads unlocked and then records the state under m_StateMutex. A path it records while a
// scan is open is skipped by that scan's merge, because the scan may have listed or read the file before the write
// that MarkKnown records; the next scan compares against MarkKnown's state. The result is the same as if MarkKnown had
// run before the scan, except that an external change made between the engine's write and that scan is reported one
// scan later instead of being missed.

namespace Engine {

	namespace Utils {

		static Result<uint64_t> ReadContentHash(const VirtualFileSystem& vfs, const VfsPath& path)
		{
			ENGINE_TRY_ASSIGN(const Buffer content, vfs.ReadFile(path));
			return XXH64(content);
		}

	}

	PollingFileWatcher::PollingFileWatcher(const VirtualFileSystem& vfs, PollingFileWatcherSpecification specification)
		: m_Vfs(&vfs), m_Specification(std::move(specification))
	{
		ENGINE_CORE_ASSERT(!m_Specification.Root.IsEmpty(), "PollingFileWatcher needs a root directory");
		ENGINE_CORE_ASSERT(std::isfinite(m_Specification.DebounceSeconds) && m_Specification.DebounceSeconds >= 0.0,
			"PollingFileWatcher needs a finite, non-negative debounce, got {} s", m_Specification.DebounceSeconds);
	}

	Status PollingFileWatcher::Start()
	{
		const std::lock_guard scanLock(m_ScanMutex);
		{
			const std::lock_guard stateLock(m_StateMutex);
			BeginScan();
		}

		Result<std::vector<ScannedFile>> files = ListFiles();
		Status readStatus;
		if (files)
		{
			for (ScannedFile& file : *files)
			{
				Result<uint64_t> hash = Utils::ReadContentHash(*m_Vfs, file.Path);
				if (hash)
				{
					file.ContentHash = *hash;
				}
				else if (hash.error().GetCode() != ErrorCode::NotFound) // a file removed since the listing is simply absent
				{
					readStatus = std::unexpected(std::move(hash).error().WithContext(
						std::format("while taking the baseline of '{}'", m_Specification.Root.ToString())));
					break;
				}
			}
		}

		const std::lock_guard stateLock(m_StateMutex);
		if (!files)
		{
			EndScan();
			return std::unexpected(std::move(files).error());
		}
		if (!readStatus)
		{
			EndScan();
			return readStatus;
		}

		std::map<VfsPath, TrackedFile> baseline;
		for (ScannedFile& file : *files)
		{
			if (!file.ContentHash || m_MarkedDuringScan.contains(file.Path))
				continue;
			const FileState state{ .Size = file.Size, .ModificationTime = file.ModificationTime, .ContentHash = *file.ContentHash };
			baseline.insert_or_assign(std::move(file.Path), TrackedFile{ .KnownHash = state.ContentHash, .Observed = state });
		}
		// MarkKnown's state is newer than the listing for the paths it recorded during the scan.
		for (const VfsPath& path : m_MarkedDuringScan)
		{
			const auto marked = m_Files.find(path);
			if (marked != m_Files.end())
				baseline.insert_or_assign(path, marked->second);
			else
				baseline.erase(path);
		}

		m_Files = std::move(baseline);
		m_UnreadablePaths.clear();
		m_IsStarted = true;
		EndScan();
		return {};
	}

	Result<std::vector<FileChange>> PollingFileWatcher::Poll(double nowSeconds)
	{
		const std::lock_guard scanLock(m_ScanMutex);
		{
			const std::lock_guard stateLock(m_StateMutex);
			if (!m_IsStarted)
				return MakeError(ErrorCode::InvalidState, "cannot poll the watcher of '{}' before Start", m_Specification.Root.ToString());
			ENGINE_CORE_ASSERT(std::isfinite(nowSeconds) && (!m_HasPolled || nowSeconds >= m_LastPollSeconds),
				"PollingFileWatcher::Poll needs a finite time not earlier than the previous one ({} s), got {} s", m_LastPollSeconds,
				nowSeconds);
			m_LastPollSeconds = nowSeconds;
			m_HasPolled = true;
			BeginScan();
		}

		Result<std::vector<ScannedFile>> files = ListFiles();
		if (!files)
		{
			const std::lock_guard stateLock(m_StateMutex);
			EndScan();
			return std::unexpected(std::move(files).error());
		}

		// Only the files whose size or modification time moved since the last scan are read.
		std::vector<ScannedFile*> filesToRead;
		{
			const std::lock_guard stateLock(m_StateMutex);
			for (ScannedFile& file : *files)
			{
				const auto tracked = m_Files.find(file.Path);
				const bool isUnchanged = tracked != m_Files.end() && tracked->second.Observed
					&& tracked->second.Observed->Size == file.Size && tracked->second.Observed->ModificationTime == file.ModificationTime;
				if (!isUnchanged)
					filesToRead.push_back(&file);
			}
		}

		for (ScannedFile* file : filesToRead)
		{
			Result<uint64_t> hash = Utils::ReadContentHash(*m_Vfs, file->Path);
			if (hash)
			{
				file->ContentHash = *hash;
				m_UnreadablePaths.erase(file->Path);
			}
			else if (hash.error().GetCode() != ErrorCode::NotFound && m_UnreadablePaths.insert(file->Path).second)
			{
				// Typically a file another program is still writing; a file removed since the listing is absent next time.
				ENGINE_CORE_WARN("File watcher cannot read '{}' and examines it again on the next scan: {}", file->Path.ToString(),
					hash.error().ToString());
			}
		}
		std::erase_if(m_UnreadablePaths, [&files](const VfsPath& path)
		{
			return !std::ranges::binary_search(*files, path, {}, &ScannedFile::Path);
		});

		const std::lock_guard stateLock(m_StateMutex);
		std::vector<FileChange> changes = MergeScan(*files, nowSeconds);
		EndScan();
		return changes;
	}

	Status PollingFileWatcher::MarkKnown(const VfsPath& path)
	{
		const VfsPath& root = m_Specification.Root;
		if (path == root || !path.IsUnder(root))
		{
			return MakeError(ErrorCode::InvalidArgument, "cannot record '{}' as known: it is not a file under the watched directory '{}'",
				path.ToString(), root.ToString());
		}

		// Size and time first, then the content: a write in between makes the next scan read the file again and compare
		// hashes, whereas the other order could pair the new time with the old content and miss that write for good.
		std::optional<FileState> state;
		Result<FileInfo> info = m_Vfs->GetInfo(path);
		if (info)
		{
			if (info->IsDirectory)
				return MakeError(ErrorCode::Io, "cannot record '{}' as known: it is a directory", path.ToString());
			Result<uint64_t> hash = Utils::ReadContentHash(*m_Vfs, path);
			if (hash)
				state = FileState{ .Size = info->Size, .ModificationTime = info->ModificationTime, .ContentHash = *hash };
			else if (hash.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(std::move(hash).error().WithContext(std::format("while recording '{}' as known", path.ToString())));
		}
		// NotFound, and Validation, the case policy's mismatch (§4.10): only another spelling exists, so this spelling,
		// which is how a listing would name the file, is absent (the old path of a case-only rename).
		else if (info.error().GetCode() != ErrorCode::NotFound && info.error().GetCode() != ErrorCode::Validation)
		{
			return std::unexpected(std::move(info).error().WithContext(std::format("while recording '{}' as known", path.ToString())));
		}

		const std::lock_guard stateLock(m_StateMutex);
		if (m_IsScanning)
			m_MarkedDuringScan.insert(path);
		if (state)
			m_Files.insert_or_assign(path, TrackedFile{ .KnownHash = state->ContentHash, .Observed = state });
		else
			m_Files.erase(path);
		return {};
	}

	const PollingFileWatcherSpecification& PollingFileWatcher::GetSpecification() const
	{
		return m_Specification;
	}

	void PollingFileWatcher::BeginScan()
	{
		m_IsScanning = true;
		m_MarkedDuringScan.clear();
	}

	void PollingFileWatcher::EndScan()
	{
		m_IsScanning = false;
		m_MarkedDuringScan.clear();
	}

	Result<std::vector<PollingFileWatcher::ScannedFile>> PollingFileWatcher::ListFiles() const
	{
		ENGINE_TRY_ASSIGN(std::vector<VfsEntry> entries, m_Vfs->List(m_Specification.Root, true));
		std::vector<ScannedFile> files;
		files.reserve(entries.size());
		for (VfsEntry& entry : entries)
		{
			if (entry.Info.IsDirectory)
				continue;
			const FileInfo& info = entry.Info;
			files.push_back(ScannedFile{ .Path = std::move(entry.Path), .Size = info.Size, .ModificationTime = info.ModificationTime });
		}
		ENGINE_CORE_ASSERT(std::ranges::is_sorted(files, {}, &ScannedFile::Path), "VirtualFileSystem::List must return sorted entries");
		return files;
	}

	std::vector<FileChange> PollingFileWatcher::MergeScan(std::span<const ScannedFile> files, double nowSeconds)
	{
		for (const ScannedFile& file : files)
		{
			if (m_MarkedDuringScan.contains(file.Path))
				continue;
			const auto [tracked, isNew] = m_Files.try_emplace(file.Path);
			std::optional<FileState>& observed = tracked->second.Observed;
			if (observed && observed->Size == file.Size && observed->ModificationTime == file.ModificationTime)
				continue;
			if (!file.ContentHash)
			{
				// Unreadable: keep the previous state, and the next scan reads it again.
				if (isNew)
					m_Files.erase(tracked);
				continue;
			}
			if (!observed || observed->ContentHash != *file.ContentHash)
				tracked->second.ChangedAtSeconds = nowSeconds;
			observed = FileState{ .Size = file.Size, .ModificationTime = file.ModificationTime, .ContentHash = *file.ContentHash };
		}

		// Both sequences are sorted by path, so one pass finds the files that are gone.
		std::vector<FileChange> changes;
		auto listed = files.begin();
		for (auto tracked = m_Files.begin(); tracked != m_Files.end();)
		{
			const VfsPath& path = tracked->first;
			TrackedFile& file = tracked->second;
			while (listed != files.end() && listed->Path < path)
				++listed;
			const bool isListed = listed != files.end() && listed->Path == path;
			if (!isListed && file.Observed && !m_MarkedDuringScan.contains(path))
			{
				file.Observed.reset();
				file.ChangedAtSeconds = nowSeconds;
			}

			if (file.ChangedAtSeconds && nowSeconds - *file.ChangedAtSeconds >= m_Specification.DebounceSeconds)
			{
				std::optional<uint64_t> currentHash;
				if (file.Observed)
					currentHash = file.Observed->ContentHash;
				if (!file.KnownHash && currentHash)
					changes.push_back(FileChange{ .Path = path, .Kind = FileChangeKind::Created });
				else if (file.KnownHash && !currentHash)
					changes.push_back(FileChange{ .Path = path, .Kind = FileChangeKind::Deleted });
				else if (file.KnownHash && currentHash && *file.KnownHash != *currentHash)
					changes.push_back(FileChange{ .Path = path, .Kind = FileChangeKind::Modified });
				file.KnownHash = currentHash;
				file.ChangedAtSeconds.reset();
			}

			if (!file.KnownHash && !file.Observed && !file.ChangedAtSeconds)
				tracked = m_Files.erase(tracked);
			else
				++tracked;
		}
		return changes;
	}

	std::string_view FileChangeKindToString(FileChangeKind kind)
	{
		switch (kind)
		{
			case FileChangeKind::Created:  return "Created";
			case FileChangeKind::Modified: return "Modified";
			case FileChangeKind::Deleted:  return "Deleted";
		}

		ENGINE_CORE_ASSERT(false, "Unknown FileChangeKind {}", std::to_underlying(kind));
		return "Unknown";
	}

}
