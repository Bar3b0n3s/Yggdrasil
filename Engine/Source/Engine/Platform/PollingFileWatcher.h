#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

// File-change detection for hot reload (Architecture §7.5): polling, not native watchers, so one deterministic
// implementation serves every host and tests drive it through a MemoryMount.

namespace Engine {

	enum class FileChangeKind : uint8_t
	{
		Created,
		Modified,
		Deleted
	};

	struct FileChange
	{
		VfsPath Path{};
		FileChangeKind Kind = FileChangeKind::Modified;

		[[nodiscard]] bool operator==(const FileChange& other) const = default;
	};

	struct PollingFileWatcherSpecification
	{
		// The directory watched recursively, such as project://Assets.
		VfsPath Root{};
		// How long a change must stay unchanged before it is reported (§7.5: 200 ms).
		double DebounceSeconds = 0.2;
	};

	// Detects created, modified and deleted files under one VFS directory by comparing scans (§7.5). Each scan lists the
	// directory recursively and compares every file's size and FileInfo::ModificationTime with the last known state; a
	// file whose size or time differs is read and its content hash (XXH64) decides whether its content really changed, so
	// touching a file without changing it reports nothing. Directories are not reported, only the files in them.
	//
	// Time is supplied by the caller: Poll takes the current time in seconds on any monotonic scale (the hot-reload job
	// passes a steady-clock reading, tests pass chosen values), so behaviour never depends on the wall clock and tests are
	// exact. Polling every 500 ms on a job (§7.5) is the caller's schedule.
	//
	// Debounce: a scan that sees a change starts (or, for a file that changed again, restarts) that file's debounce. A
	// change is reported by the first Poll whose time is at least DebounceSeconds after the scan that last saw the file
	// change, and is reported once. What is reported is the net change between the last reported (or baseline) state and
	// the current one: created and then modified reports Created; created and deleted within the debounce reports
	// nothing; modified and then restored to the same content reports nothing; deleted and recreated reports Modified (or
	// nothing for identical content).
	//
	// No echo (§7.5 race rule 1): AssetWriter calls MarkKnown after each write it makes under the watched root, so the
	// watcher records that state as known and never reports the editor's own writes.
	//
	// Thread-safe: Poll runs on a job while MarkKnown runs on the main thread; calls are serialized internally. The watcher
	// keeps a reference to the VFS, which must outlive it (documented back-reference, §4.7).
	class PollingFileWatcher
	{
	public:
		PollingFileWatcher(const VirtualFileSystem& vfs, PollingFileWatcherSpecification specification);

		PollingFileWatcher(const PollingFileWatcher&) = delete;
		PollingFileWatcher& operator=(const PollingFileWatcher&) = delete;

		// Scans the root and records every file as the baseline; changes are reported relative to it. Calling Start again
		// takes a new baseline and drops pending changes. Errors: those of VirtualFileSystem::List and ReadFile (NotFound
		// when the root does not exist).
		[[nodiscard]] Status Start();

		// Scans once at time `nowSeconds` (finite and not earlier than the previous call; asserted) and returns the changes
		// whose debounce has expired, sorted by path (byte-wise) so the result never depends on listing order. A file that
		// cannot be read during a scan (being written) keeps its previous state and is examined again by the next scan.
		// Errors: InvalidState before Start; the errors of VirtualFileSystem::List on the root (the root itself vanished).
		[[nodiscard]] Result<std::vector<FileChange>> Poll(double nowSeconds);

		// Records the current state of `path` (a file under the root; it may no longer exist, or exist only under another
		// spelling, as the old path of a case-only rename does: absent either way) as known: a change made by this process
		// that must not come back as an external change. Drops a pending change of that path. Errors: InvalidArgument when
		// `path` is not under the root; Io when it exists but cannot be read.
		[[nodiscard]] Status MarkKnown(const VfsPath& path);

		[[nodiscard]] const PollingFileWatcherSpecification& GetSpecification() const;
	private:
		// What a scan, or MarkKnown, saw of one file.
		struct FileState
		{
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			uint64_t ContentHash = 0; // XXH64
		};

		// One file the watcher knows about; erased once it is absent, reported and not pending.
		struct TrackedFile
		{
			std::optional<uint64_t> KnownHash{};      // the last reported (or baseline, or MarkKnown) content; empty: absent
			std::optional<FileState> Observed{};      // what the latest scan (or MarkKnown) saw; empty: absent
			std::optional<double> ChangedAtSeconds{}; // pending: the time of the scan that last saw Observed change
		};

		// One file of a scan's listing, with its content hash once read (empty: not read, or unreadable).
		struct ScannedFile
		{
			VfsPath Path{};
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			std::optional<uint64_t> ContentHash{};
		};

		// Opens and closes a scan, under m_StateMutex: while a scan is open, MarkKnown records the paths it touches.
		void BeginScan();
		void EndScan();

		// Lists the files under the root, sorted by path. Errors: those of VirtualFileSystem::List.
		[[nodiscard]] Result<std::vector<ScannedFile>> ListFiles() const;

		// Folds one Poll's scan into m_Files and returns the changes whose debounce expired (under m_StateMutex).
		[[nodiscard]] std::vector<FileChange> MergeScan(std::span<const ScannedFile> files, double nowSeconds);
	private:
		// Documented back-reference (§4.7): the VFS outlives the watcher.
		const VirtualFileSystem* m_Vfs = nullptr;
		PollingFileWatcherSpecification m_Specification;

		// Serializes Start and Poll for their whole duration, including their VFS I/O; guards m_UnreadablePaths.
		std::mutex m_ScanMutex;
		std::set<VfsPath> m_UnreadablePaths; // files whose last read failed, so each failure is logged once

		// Guards the members below. Held only between VFS calls, never across them, so MarkKnown on the main thread never
		// waits for a scan's I/O: paths it records while a scan is open are left alone by that scan, whose listing of them
		// may be older than MarkKnown's state.
		std::mutex m_StateMutex;
		std::map<VfsPath, TrackedFile> m_Files; // sorted by path, so changes come out in canonical order
		std::set<VfsPath> m_MarkedDuringScan;
		double m_LastPollSeconds = 0.0;
		bool m_HasPolled = false;
		bool m_IsStarted = false;
		bool m_IsScanning = false;
	};

	// "Created", "Modified" or "Deleted".
	[[nodiscard]] std::string_view FileChangeKindToString(FileChangeKind kind);

}
