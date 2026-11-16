#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <span>

// Files that one process publishes for other processes to read while it runs: the automation session files (Architecture
// §13.2, Docs/Decisions/0040-session-file-sharing.md). The writer renames a complete file over the published name, and
// readers in other processes open it at any moment, also while a rename publishes or replaces it.
//
// On Windows every rename (MoveFileExW's behind std::filesystem::rename included) renames the file through a handle
// with DELETE access, which stays open until the rename returns: for that moment the file has its new name and an open
// handle with DELETE access, and a reader that opens it without FILE_SHARE_DELETE (the standard C++ streams, the C
// runtime and Python's open() share only reading and writing) fails with a sharing violation, which the C runtime
// reports as EACCES (PermissionDenied). MoveFileExW also cannot replace a file that any reader has open. Read opens with
// full sharing, so it never conflicts with a rename, and Replace renames with POSIX semantics, which replace a file that
// readers hold open with FILE_SHARE_DELETE. On Linux and macOS no open conflicts with a rename: Read is
// FileSystem::ReadFile and Replace is rename(2), as before.

namespace Engine {

	class PublishedFile
	{
	public:
		PublishedFile() = delete;

		// The whole file, read through a handle that shares reading, writing and deletion (Windows), so a rename or removal
		// of the file in progress never fails the read, and the read never fails them. Errors: NotFound, PermissionDenied,
		// Io (also for a directory).
		[[nodiscard]] static Result<Buffer> Read(const std::filesystem::path& path);

		// Renames the file `from` to `to` (on one volume) in one step, replacing `to` when it exists, also while readers
		// that opened it with Read hold it open: they keep reading the content they opened, and every later open finds the
		// new file. On Windows a reader that opened `to` without FILE_SHARE_DELETE still makes it fail, and on a volume or
		// Windows release without POSIX rename semantics (FAT, exFAT, a network share) it falls back to MoveFileExW's
		// replace, which fails while any reader has `to` open. On failure `from` and `to` are unchanged. Errors: NotFound
		// (`from`), PermissionDenied, Io.
		[[nodiscard]] static Status Replace(const std::filesystem::path& from, const std::filesystem::path& to);

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 8; the owner's answer 5 of ADR 0042): writes `data` to `path`
		// so that, once it returns, the new content survives a power loss: a temporary file beside `path` is written, flushed
		// to the disk (FlushFileBuffers) and closed, then renamed over `path` (Replace). A crash or failure at any instant
		// leaves either the old or the new content, and the temporary is removed on failure. The parent directory must
		// exist. Windows only (Docs/Decisions/0028-backlog-program.md decision 10): Linux and macOS report Unsupported, and the
		// caller falls back to FileSystem::WriteFileAtomic. Errors: NotFound (the parent), PermissionDenied, Io naming the
		// failed step.
		[[nodiscard]] static Status WriteDurably(const std::filesystem::path& path, std::span<const std::byte> data);
	};

}
