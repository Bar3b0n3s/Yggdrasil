#include "EnginePCH.h"
#include "Engine/Platform/ProjectLock.h"

// The project lock on Windows: LockFileEx on one byte at offset 0x7FFFFFFF'FFFFFFFE, far past the pid text, so other
// processes can still read the text while the lock is held. Locks belong to the file handle, and the OS releases them
// when the handle closes or the process ends. The handle that holds the lock has read access only (LockFileEx needs no
// more), so readers that deny writers, such as .NET's File.ReadAllText (FileShare.Read), can open the file while it is
// held; the pid is written through a second handle that is closed right away.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Platform/Process.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>

	#include <system_error>

namespace Engine {

	namespace {

		// Closes a file handle on destruction unless released.
		class ScopedFile
		{
		public:
			explicit ScopedFile(HANDLE handle)
				: m_Handle(handle)
			{
			}

			~ScopedFile()
			{
				if (m_Handle != INVALID_HANDLE_VALUE)
					CloseHandle(m_Handle);
			}

			ScopedFile(const ScopedFile&) = delete;
			ScopedFile& operator=(const ScopedFile&) = delete;
			ScopedFile(ScopedFile&&) = delete;
			ScopedFile& operator=(ScopedFile&&) = delete;

			[[nodiscard]] HANDLE Get() const { return m_Handle; }

			[[nodiscard]] HANDLE Release()
			{
				return std::exchange(m_Handle, INVALID_HANDLE_VALUE);
			}
		private:
			HANDLE m_Handle = INVALID_HANDLE_VALUE;
		};

	}

	namespace Utils {

		// The locked byte. Windows checks the whole byte range a read requests against locks, not only the bytes the file
		// holds, so a byte just past the pid text (at most 11 bytes) would fail any reader that asks for more than that
		// in one read (a 64 KiB buffer is common). The highest offset whose range stays below 2^63 is out of every
		// reader's reach; locking past the end of the file is allowed.
		constexpr uint64_t LockOffset = 0x7FFF'FFFF'FFFF'FFFEull;

		static std::string DescribeWindowsError(DWORD code)
		{
			return std::system_category().message(static_cast<int>(code));
		}

		// Locks or tests the lock byte of `file` without waiting. Returns false with ERROR_LOCK_VIOLATION in GetLastError
		// when another handle holds a conflicting lock.
		static bool TryLockByte(HANDLE file, bool exclusive)
		{
			OVERLAPPED overlapped{};
			overlapped.Offset = static_cast<DWORD>(LockOffset & 0xFFFF'FFFFull);
			overlapped.OffsetHigh = static_cast<DWORD>(LockOffset >> 32);
			const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
			return LockFileEx(file, flags, 0, 1, 0, &overlapped) != FALSE;
		}

		static void UnlockByte(HANDLE file)
		{
			OVERLAPPED overlapped{};
			overlapped.Offset = static_cast<DWORD>(LockOffset & 0xFFFF'FFFFull);
			overlapped.OffsetHigh = static_cast<DWORD>(LockOffset >> 32);
			UnlockFileEx(file, 0, 1, 0, &overlapped);
		}

		// The AlreadyExists error of a lock held by someone else, naming the pid written in the file.
		static std::unexpected<Error> MakeLockedError(const std::filesystem::path& lockFile)
		{
			const Result<uint32_t> pid = ProjectLock::ReadHolderPid(lockFile);
			const std::string holder = pid.has_value() ? std::format("process {}", *pid) : std::string("an unknown process");
			return MakeError(ErrorCode::AlreadyExists, "'{}' is locked by {}", NativePathToUtf8(lockFile), holder);
		}

		// Replaces the content of the locked `lockFile` with this process's ID through a handle of its own, closed before
		// returning. The holder's handle shares writing and denies deletion, so the path still names the locked file.
		static Status WriteHolderPid(const std::filesystem::path& lockFile)
		{
			ScopedFile file(CreateFileW(lockFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr));
			if (file.Get() == INVALID_HANDLE_VALUE)
			{
				const DWORD error = GetLastError();
				return MakeError(ErrorCode::Io, "cannot open '{}' to write the process ID: {}", NativePathToUtf8(lockFile),
					DescribeWindowsError(error));
			}

			// Overwrite from the start, then cut off whatever an older, longer text left behind.
			const std::string text = std::format("{}\n", Process::GetCurrentId());
			DWORD written = 0;
			LARGE_INTEGER length{};
			length.QuadPart = static_cast<LONGLONG>(text.size());
			const bool isWritten = WriteFile(file.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
				&& written == text.size();
			const bool isTruncated = isWritten && SetFilePointerEx(file.Get(), length, nullptr, FILE_BEGIN) != FALSE
				&& SetEndOfFile(file.Get()) != FALSE;
			if (!isTruncated)
			{
				const DWORD error = GetLastError();
				return MakeError(ErrorCode::Io, "cannot write the process ID into '{}': {}", NativePathToUtf8(lockFile),
					DescribeWindowsError(error));
			}
			return {};
		}

	}

	struct ProjectLock::Impl
	{
		std::filesystem::path Path{};
		HANDLE File = INVALID_HANDLE_VALUE;

		Impl() = default;

		~Impl()
		{
			if (File == INVALID_HANDLE_VALUE)
				return;
			// Locks are released when the handle closes, but possibly later than this call; unlocking first makes the
			// release immediate, so the next Acquire never races it.
			Utils::UnlockByte(File);
			CloseHandle(File);
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;
		Impl(Impl&&) = delete;
		Impl& operator=(Impl&&) = delete;
	};

	ProjectLock::ProjectLock(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	ProjectLock::~ProjectLock() = default;

	ProjectLock::ProjectLock(ProjectLock&& other) noexcept = default;

	ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept = default;

	Result<ProjectLock> ProjectLock::Acquire(const std::filesystem::path& lockFile)
	{
		// Readers (ReadHolderPid, other processes) open the file while it is held, and the pid is written through a second
		// handle, so it is shared for reading and writing.
		ScopedFile file(CreateFileW(lockFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_PATH_NOT_FOUND)
			{
				return MakeError(ErrorCode::NotFound, "cannot create the lock file '{}': its directory does not exist",
					Utils::NativePathToUtf8(lockFile));
			}
			return MakeError(ErrorCode::Io, "cannot open the lock file '{}': {}", Utils::NativePathToUtf8(lockFile),
				Utils::DescribeWindowsError(error));
		}

		if (!Utils::TryLockByte(file.Get(), true))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_LOCK_VIOLATION)
				return Utils::MakeLockedError(lockFile);
			return MakeError(ErrorCode::Io, "cannot lock '{}': {}", Utils::NativePathToUtf8(lockFile), Utils::DescribeWindowsError(error));
		}

		const Status written = Utils::WriteHolderPid(lockFile);
		if (!written.has_value())
		{
			Utils::UnlockByte(file.Get());
			return std::unexpected(written.error());
		}

		Scope<Impl> impl = CreateScope<Impl>();
		impl->Path = lockFile;
		impl->File = file.Release();
		return ProjectLock(std::move(impl));
	}

	Result<bool> ProjectLock::IsHeld(const std::filesystem::path& lockFile)
	{
		ScopedFile file(CreateFileW(lockFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
				return false;
			return MakeError(ErrorCode::Io, "cannot open the lock file '{}': {}", Utils::NativePathToUtf8(lockFile),
				Utils::DescribeWindowsError(error));
		}

		// A shared lock conflicts with a holder's exclusive lock but not with other probes.
		if (Utils::TryLockByte(file.Get(), false))
		{
			Utils::UnlockByte(file.Get());
			return false;
		}
		const DWORD error = GetLastError();
		if (error == ERROR_LOCK_VIOLATION)
			return true;
		return MakeError(ErrorCode::Io, "cannot test the lock of '{}': {}", Utils::NativePathToUtf8(lockFile),
			Utils::DescribeWindowsError(error));
	}

	const std::filesystem::path& ProjectLock::GetPath() const
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "GetPath called on a moved-from ProjectLock");
		return m_Impl->Path;
	}

}

#endif
