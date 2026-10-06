#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>

// The single-writer lock of a project (Architecture §4.13): an editor that opens a project holds an exclusive OS lock on
// <Project>/Library/Editor.lock, and the file names the holder's process ID. The OS releases the lock when the process
// ends however it ends, so a crashed editor never leaves a stale lock; the pid text may stay behind and means nothing
// without the lock.
//
// File format: the holder's process ID in decimal ASCII followed by one LF, nothing else. Other processes can read it
// while the lock is held (the MCP bridge reports EditorAlreadyOpen {pid} from it, §13.8): POSIX uses flock on the whole
// file, which is advisory and leaves reads alone; Windows uses LockFileEx on one byte at offset 4096, past the text, so
// the text itself stays readable.

namespace Engine {

	// One held lock. A movable value; a moved-from ProjectLock may only be destroyed or assigned to. Destroying it
	// releases the lock (the file stays). One thread at a time.
	class ProjectLock
	{
	public:
		// Opens or creates `lockFile` (its directory must exist), takes the exclusive lock without waiting, and replaces
		// the file's content with this process's ID. Locks are per opened file, so a second Acquire of the same file fails
		// even in the same process. Errors: AlreadyExists when another holder has the lock, with the message
		// "'<lockFile>' is locked by process <pid>" (the pid read from the file; "an unknown process" when it is
		// unreadable); NotFound when the directory does not exist; Io.
		[[nodiscard]] static Result<ProjectLock> Acquire(const std::filesystem::path& lockFile);

		// The process ID written in `lockFile`, read without taking the lock (by any process, whether or not the lock is
		// held). Errors: NotFound when the file does not exist; Parse when it does not hold the format above; Io.
		[[nodiscard]] static Result<uint32_t> ReadHolderPid(const std::filesystem::path& lockFile);

		// Whether some holder has the lock right now: tries to take it and releases it at once. False when the file does
		// not exist. Errors: Io.
		[[nodiscard]] static Result<bool> IsHeld(const std::filesystem::path& lockFile);

		~ProjectLock();

		ProjectLock(ProjectLock&& other) noexcept;
		ProjectLock& operator=(ProjectLock&& other) noexcept;
		ProjectLock(const ProjectLock&) = delete;
		ProjectLock& operator=(const ProjectLock&) = delete;

		[[nodiscard]] const std::filesystem::path& GetPath() const;
	private:
		struct Impl;

		explicit ProjectLock(Scope<Impl> impl);
	private:
		Scope<Impl> m_Impl;
	};

}
