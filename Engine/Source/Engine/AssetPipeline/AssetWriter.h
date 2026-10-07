#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <span>

// No-echo writes (Architecture §7.5 race rule 1): every editor write under the project (script.write, scene.save,
// asset.setProperties, imports, project.upgrade, the asset commands) goes through the AssetWriter. It writes atomically,
// updates the watcher's known (size, mtime, hash) for that path and tells the EditorAssetManager, which updates its
// registry and schedules the reimport itself, so the watcher never sees the write as an external change. Autosave writes
// under the unwatched Library/ and does not need it. EditorContext::WriteProjectFile (and its move and remove
// counterparts) route through it, and EditorContext keeps provenance for every write it reports, the asset manager's own
// .meta writes included (EditorAssetManager::SetWriteObserver).

namespace Engine {

	class PollingFileWatcher;
	class VirtualFileSystem;

	enum class AssetWriteKind : uint8_t
	{
		Written,         // a file was created or replaced
		Removed,         // a file or directory was removed
		Moved,           // a file or directory was renamed (From -> Path)
		DirectoryCreated // a directory (and missing parents) was created
	};

	// What the writer just did, reported to its listener after the VFS call succeeded.
	struct AssetWriteEvent
	{
		AssetWriteKind Kind = AssetWriteKind::Written;
		VfsPath Path{};           // the written, removed or created path, or a move's destination
		VfsPath From{};           // a move's source; empty otherwise
		uint64_t ContentHash = 0; // Written: XXH64 (seed 0) of the bytes written (provenance records it); 0 otherwise
	};

	// The writer of one editor. Main thread only (§4.11). Not copyable.
	class AssetWriter
	{
	public:
		using Listener = UniqueFunction<void(const AssetWriteEvent&)>;

		// `vfs` is a documented back-reference that outlives the writer.
		explicit AssetWriter(VirtualFileSystem& vfs);
		~AssetWriter();

		AssetWriter(const AssetWriter&) = delete;
		AssetWriter& operator=(const AssetWriter&) = delete;

		// The hot-reload watcher whose known state each write under its root updates (PollingFileWatcher::MarkKnown); null
		// (the default, and while no project is open) marks nothing. A documented back-reference the caller clears before the
		// watcher goes away.
		void SetWatcher(PollingFileWatcher* watcher);

		// Receives every event after its VFS call succeeded (the EditorAssetManager: registry updates and reimports). Empty:
		// no listener.
		void SetListener(Listener listener);

		// During a dry run (§13.4) project:// is an overlay: writes still go through the VFS (into the overlay) and reach the
		// listener, but never the watcher, which describes the disk.
		void SetDryRun(bool dryRun);
		[[nodiscard]] bool IsDryRun() const;

		// VirtualFileSystem::WriteFileAtomic, then MarkKnown (paths under the watcher's root), then the listener. Errors: the
		// VFS's (nothing marked or reported then); a failed MarkKnown after a successful write is logged at Warn (the change
		// then comes back once as an external change, which reimports what was just written) and is not an error.
		[[nodiscard]] Status Write(const VfsPath& path, std::span<const std::byte> data);

		// VirtualFileSystem::Remove, then MarkKnown of every file it removed under the watcher's root, then the listener.
		// Errors: the VFS's.
		[[nodiscard]] Status Remove(const VfsPath& path);

		// VirtualFileSystem::Move (same scheme), creating the destination's parent directories first, then MarkKnown of the
		// old and new paths of every file it moved, then the listener. Errors: the VFS's.
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to);

		// VirtualFileSystem::CreateDirectories, then the listener (directories are not watched). Errors: the VFS's.
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory);
	private:
		// The VFS and watcher back-references, the listener and the dry-run flag (AssetWriter.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
