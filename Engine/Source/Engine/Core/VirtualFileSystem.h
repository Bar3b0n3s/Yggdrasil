#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The virtual file system (Architecture §4.10). Engine code reads and writes engine://, project://, user://, cache://
// and enginecache:// paths through it; each scheme is backed by one mount.

namespace Engine {

	enum class MountAccess : uint8_t
	{
		ReadWrite,
		ReadOnly // every mutating call fails with PermissionDenied
	};

	// One listed file or directory.
	struct VfsEntry
	{
		VfsPath Path; // in the scheme of the listed directory
		FileInfo Info;
	};

	// A sequential reader over one file (audio streaming). Used by one thread at a time. It reads the content the file
	// had when Open returned, on every mount and host: mounts whose files can change (native, memory, overlay) stream a
	// snapshot held in memory, so a stream never keeps a host file open, and rewriting, moving or removing the file while
	// a stream is open succeeds everywhere (on Windows an open handle without FILE_SHARE_DELETE would make the atomic
	// replace fail). A read-only PakMount (M6), whose archive never changes while it is mounted, may read the archive
	// instead. A stream does not depend on its mount: it stays valid after the mount is unmounted or destroyed.
	class IFileStream
	{
	public:
		virtual ~IFileStream() = default;

		// Reads up to destination.size() bytes at the current position and advances it; returns the number read, 0 at
		// the end of the file. Errors: Io.
		[[nodiscard]] virtual Result<size_t> Read(std::span<std::byte> destination) = 0;

		// Moves to `position` (<= GetSize()). Errors: InvalidArgument beyond the end, Io.
		[[nodiscard]] virtual Status Seek(uint64_t position) = 0;

		[[nodiscard]] virtual uint64_t GetPosition() const = 0;
		[[nodiscard]] virtual uint64_t GetSize() const = 0;
	};

	// The backing store of one scheme. Paths are VfsPaths whose scheme the mount ignores; an empty GetPath() is the mount
	// root. Every mount follows the same rules, so tests on MemoryMount predict native behaviour:
	//   - case policy (§4.10): paths are case-sensitive on every host, and no directory ever holds two entries whose names
	//     differ only in ASCII letter case. Every path component is compared with the stored spelling:
	//       - reads, and the existing directories along any path: a component that matches an entry only when case is
	//         ignored is Validation "case mismatch" naming the stored spelling; a complete miss is NotFound;
	//       - the final component of a write, CreateDirectories or the destination of a Move: a name that matches an
	//         existing entry only when case is ignored ("level1.scene" next to "Level1.scene") is the same Validation
	//         error and changes nothing, so no host silently overwrites the other spelling or creates a second file;
	//       - the one exception is a case-only rename: Move whose destination differs from its source only in case
	//         renames that entry in place, which is how a case mismatch is fixed;
	//       - other host aliases (non-ASCII case folding, Unicode normalization, 8.3 short names) exist only behind a
	//         NativeDirectoryMount, which rejects a new name its host resolves to an existing entry with Validation;
	//         where the host does not alias the name (Linux, MemoryMount) it is a new entry;
	//   - read-only mounts reject mutations with PermissionDenied;
	//   - List returns entries sorted by path (byte-wise); recursive listings include directories and their contents;
	//   - every member is thread-safe, and each call is atomic with respect to the others on the same mount.
	class IMount
	{
	public:
		virtual ~IMount() = default;

		// Errors: NotFound, Validation (case), Io (also for a directory).
		[[nodiscard]] virtual Result<Buffer> ReadFile(const VfsPath& path) const = 0;

		// A stream over the file's current content (see IFileStream). Errors: as ReadFile.
		[[nodiscard]] virtual Result<Scope<IFileStream>> Open(const VfsPath& path) const = 0;

		// Atomically creates or replaces the file (FileSystem::WriteFileAtomic semantics for native mounts). The parent
		// directory must exist. Errors: NotFound (parent), Validation (case, including a file name that differs from an
		// existing entry only in case), PermissionDenied, Io.
		[[nodiscard]] virtual Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) = 0;

		// Errors: NotFound, Validation (case), Io.
		[[nodiscard]] virtual Result<FileInfo> GetInfo(const VfsPath& path) const = 0;

		// Errors: NotFound, Validation (case), Io (also when `directory` is a file).
		[[nodiscard]] virtual Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const = 0;

		// Creates the directory and missing parents; succeeds when it exists. Errors: AlreadyExists (a file is in the way),
		// Validation (case), PermissionDenied, Io.
		[[nodiscard]] virtual Status CreateDirectories(const VfsPath& directory) = 0;

		// Removes a file, or a directory with everything below it. Removing the root is InvalidArgument. Errors: NotFound,
		// Validation (case), PermissionDenied, Io.
		[[nodiscard]] virtual Status Remove(const VfsPath& path) = 0;

		// Renames within this mount. The destination's parent must exist and the destination must not, except for a
		// case-only rename of the source itself (see the case policy). Errors: NotFound, AlreadyExists, InvalidArgument
		// (the mount root as source or destination, or a destination inside the source), Validation (case),
		// PermissionDenied, Io.
		[[nodiscard]] virtual Status Move(const VfsPath& from, const VfsPath& to) = 0;

		[[nodiscard]] virtual MountAccess GetAccess() const = 0;
	};

	// Routes each VfsPath to the mount of its scheme. Owned per context by EngineContext (never a global).
	//
	// Thread safety: every member may be called from any thread, concurrently. The mount table is guarded by a
	// reader-writer lock: each forwarding call (ReadFile ... Move) holds it shared until its mount call has returned,
	// and Mount and Unmount hold it exclusively. Unmount therefore waits for every call in flight, and no call uses a
	// mount after it was removed; a call that starts while its scheme is unmounted fails with NotFound. Mounts never
	// call back into the VirtualFileSystem (that could deadlock against a waiting Unmount).
	//
	// Dry runs (§13.4) swap project:// for an OverlayMount around the original mount (Unmount, wrap, Mount, and the
	// reverse afterwards). The swap is visible to every user of this VirtualFileSystem: during the dry run a job that
	// reads project:// would see the would-be files (and could cook them into cache://), and a job that writes there
	// would write into the overlay and lose its write. The dry-run path (EditorCore, M4) therefore swaps only while no
	// job uses project://: it submits no import, cook or file-watcher job during the call and waits for the running ones
	// (JobSystem::WaitIdle) before the swap.
	class VirtualFileSystem
	{
	public:
		VirtualFileSystem();
		~VirtualFileSystem();

		VirtualFileSystem(const VirtualFileSystem&) = delete;
		VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

		// Mounts `mount` (non-null, asserted) at `scheme`. Errors: Validation (scheme syntax, VfsPath::ValidateScheme),
		// AlreadyExists (the scheme is mounted).
		[[nodiscard]] Status Mount(std::string_view scheme, Scope<IMount> mount);

		// Removes and returns the mount of `scheme`, for example to wrap it in an OverlayMount for a dry run and mount it
		// back afterwards. Waits until no call is using any mount (see the class comment). Errors: NotFound.
		[[nodiscard]] Result<Scope<IMount>> Unmount(std::string_view scheme);

		[[nodiscard]] bool IsMounted(std::string_view scheme) const;

		// Mounted schemes, sorted.
		[[nodiscard]] std::vector<std::string> GetSchemes() const;

		// Each of these resolves the path's scheme and forwards to its mount (see IMount for the errors). A scheme that is
		// not mounted, or the empty path, is NotFound.
		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const;
		// The file as UTF-8 text; invalid UTF-8 is a Validation error with the byte offset.
		[[nodiscard]] Result<std::string> ReadText(const VfsPath& path) const;
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const;
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data);
		// True when GetInfo succeeds.
		[[nodiscard]] bool Exists(const VfsPath& path) const;
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const;
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive = false) const;
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory);
		[[nodiscard]] Status Remove(const VfsPath& path);
		// Both paths must have the same scheme (InvalidArgument otherwise).
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to);
	private:
		mutable std::shared_mutex m_MountsMutex; // shared by forwarding calls, exclusive in Mount and Unmount
		std::map<std::string, Scope<IMount>, std::less<>> m_Mounts;
	};

}
