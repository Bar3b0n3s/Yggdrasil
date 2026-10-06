#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Host file system access (Architecture §4.10). Engine code normally goes through the VirtualFileSystem; FileSystem is
// what NativeDirectoryMount, the log file, crash reports and tools build on. Every std::filesystem call uses its
// std::error_code overload (lint-enforced); OS errors become Io, NotFound, AlreadyExists or PermissionDenied errors
// whose message names the path and the OS reason. A path below a file names nothing on every host (NotFound; for
// CreateDirectories, AlreadyExists naming the file in the way). Paths are UTF-8. All functions are safe to call from any
// thread; concurrent writes to the same file are not coordinated.

namespace Engine {

	// What a file or directory looks like now.
	struct FileInfo
	{
		uint64_t Size = 0; // bytes; 0 for directories
		// Changes whenever the file is rewritten (at the host's timestamp resolution for native files; a logical counter
		// for memory mounts). Opaque: only equality is meaningful (PollingFileWatcher compares size and this, then
		// confirms with a content hash, §7.5).
		uint64_t ModificationTime = 0;
		bool IsDirectory = false;
	};

	// The steps of WriteFileAtomic, in order. Tests inject a failure at one of them to prove that the target keeps its
	// previous content (Roadmap M1 "FileSystem: atomic write survives an injected failure").
	enum class AtomicWriteStep : uint8_t
	{
		None,            // no injected failure
		CreateTemporary, // create "<name>.tmp-<unique>" next to the target
		Write,           // write the data to it
		Flush,           // flush and close it
		Backup,          // copy the existing target to "<name>.bak", replacing an older backup
		Replace          // rename the temporary over the target
	};

	struct AtomicWriteOptions
	{
		// Keep the previous content as "<name>.bak" (one backup per file, §4.10).
		bool KeepBackup = true;
		// Fail at this step as if the OS had failed it (Io error naming the step). Production code leaves it at None.
		AtomicWriteStep InjectFailure = AtomicWriteStep::None;
	};

	class FileSystem
	{
	public:
		FileSystem() = delete;

		// The whole file. Errors: NotFound, PermissionDenied, Io (also for a directory).
		[[nodiscard]] static Result<Buffer> ReadFile(const std::filesystem::path& path);

		// The whole file as text, validated as UTF-8 (Validation error with the byte offset otherwise). Bytes are
		// returned unchanged: no BOM stripping, no newline conversion.
		[[nodiscard]] static Result<std::string> ReadText(const std::filesystem::path& path);

		// Replaces the file's content so that a crash or failure at any instant leaves either the complete old content
		// or the complete new content at `path` (§4.10): write a temporary file in the same directory, flush and close
		// it, copy the old file to "<name>.bak" (KeepBackup), then rename the temporary over the target (an atomic
		// replace on one volume: rename(2) on POSIX, MoveFileExW with MOVEFILE_REPLACE_EXISTING behind
		// std::filesystem::rename on Windows). On failure the temporary is removed, the target is untouched and the
		// error names the failed step. The parent directory must exist (NotFound otherwise).
		[[nodiscard]] static Status WriteFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data,
			const AtomicWriteOptions& options = {});

		// True when a file or directory exists at `path`; false when it does not or cannot be determined.
		[[nodiscard]] static bool Exists(const std::filesystem::path& path);

		// Errors: NotFound, PermissionDenied, Io.
		[[nodiscard]] static Result<FileInfo> GetInfo(const std::filesystem::path& path);

		// Creates the directory and any missing parents; succeeds when it already exists. Errors: AlreadyExists when a
		// file is in the way, PermissionDenied, Io.
		[[nodiscard]] static Status CreateDirectories(const std::filesystem::path& path);

		// Removes a file, or a directory with everything below it. Errors: NotFound, PermissionDenied, Io.
		[[nodiscard]] static Status Remove(const std::filesystem::path& path);

		// Renames a file or directory. A destination that differs from `from` only in letter case names the source
		// itself on a case-insensitive host; it is renamed in place on every host (a case-only rename), never reported
		// as existing. Errors: NotFound (source), AlreadyExists (destination exists), InvalidArgument (moving a directory
		// into its own subtree), PermissionDenied, Io.
		[[nodiscard]] static Status Move(const std::filesystem::path& from, const std::filesystem::path& to);

		// The entries directly in `directory` (recursive: everything below it), as paths that start with `directory`,
		// sorted byte-wise by their generic UTF-8 form so the order is the same on every host. Symbolic links are listed
		// but not followed. Errors: NotFound, PermissionDenied, Io (also when `directory` is a file).
		[[nodiscard]] static Result<std::vector<std::filesystem::path>> ListDirectory(const std::filesystem::path& directory,
			bool recursive = false);

		// Case policy (§4.10): checks that `relativePath` (forward slashes, below `root`) exists under `root` with exactly
		// this spelling, comparing every component with the directory entries on disk. Errors: NotFound when no entry
		// matches even ignoring case; Validation "case mismatch: '<given>' is '<on disk>' on disk" when a component
		// matches only ignoring case (ASCII case folding). Behaves the same on case-sensitive and case-insensitive hosts.
		// NativeDirectoryMount also checks the target of every write, directory creation and move with it: Validation
		// means another spelling of it exists; NotFound for the final component means that no entry spells it, and the
		// mount then asks the host whether it still resolves the name to an existing entry (non-ASCII case folding,
		// Unicode normalization, 8.3 short names), which this function does not detect.
		[[nodiscard]] static Status VerifyCase(const std::filesystem::path& root, std::string_view relativePath);
	};

}
