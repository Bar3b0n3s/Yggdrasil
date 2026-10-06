#include "EnginePCH.h"
#include "Engine/Platform/ProjectLock.h"

// The project lock on Linux and macOS: flock on the whole file. It is advisory, so reading the pid text is unaffected;
// it belongs to the open file description, so a second open of the same file conflicts even in the same process; and the
// OS releases it when the last descriptor closes, which includes the process ending however it ends. The descriptor is
// close-on-exec, so no child process ever keeps the lock alive.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Platform/Process.h"

	#include <fcntl.h>
	#include <sys/file.h>
	#include <unistd.h>

	#include <cerrno>
	#include <system_error>

namespace Engine {

	namespace {

		// Closes a descriptor on destruction unless released.
		class ScopedDescriptor
		{
		public:
			explicit ScopedDescriptor(int descriptor)
				: m_Descriptor(descriptor)
			{
			}

			~ScopedDescriptor()
			{
				if (m_Descriptor >= 0)
					close(m_Descriptor);
			}

			ScopedDescriptor(const ScopedDescriptor&) = delete;
			ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
			ScopedDescriptor(ScopedDescriptor&&) = delete;
			ScopedDescriptor& operator=(ScopedDescriptor&&) = delete;

			[[nodiscard]] int Get() const { return m_Descriptor; }

			[[nodiscard]] int Release()
			{
				return std::exchange(m_Descriptor, -1);
			}
		private:
			int m_Descriptor = -1;
		};

	}

	namespace Utils {

		static std::string DescribeErrno(int code)
		{
			return std::generic_category().message(code);
		}

		// flock, retried when a signal interrupts it. Returns 0 or the errno value.
		static int LockFile(int descriptor, int operation)
		{
			while (flock(descriptor, operation) != 0)
			{
				const int error = errno;
				if (error != EINTR)
					return error;
			}
			return 0;
		}

		// The AlreadyExists error of a lock held by someone else, naming the pid written in the file.
		static std::unexpected<Error> MakeLockedError(const std::filesystem::path& lockFile)
		{
			const Result<uint32_t> pid = ProjectLock::ReadHolderPid(lockFile);
			const std::string holder = pid.has_value() ? std::format("process {}", *pid) : std::string("an unknown process");
			return MakeError(ErrorCode::AlreadyExists, "'{}' is locked by {}", lockFile.native(), holder);
		}

	}

	struct ProjectLock::Impl
	{
		std::filesystem::path Path{};
		int Descriptor = -1;

		Impl() = default;

		~Impl()
		{
			// Closing the only descriptor of the open file description releases the lock.
			if (Descriptor >= 0)
				close(Descriptor);
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
		ScopedDescriptor file(open(lockFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
		if (file.Get() < 0)
		{
			const int error = errno;
			if (error == ENOENT)
				return MakeError(ErrorCode::NotFound, "cannot create the lock file '{}': its directory does not exist", lockFile.native());
			return MakeError(ErrorCode::Io, "cannot open the lock file '{}': {}", lockFile.native(), Utils::DescribeErrno(error));
		}

		const int lockError = Utils::LockFile(file.Get(), LOCK_EX | LOCK_NB);
		if (lockError == EWOULDBLOCK)
			return Utils::MakeLockedError(lockFile);
		if (lockError != 0)
			return MakeError(ErrorCode::Io, "cannot lock '{}': {}", lockFile.native(), Utils::DescribeErrno(lockError));

		// Overwrite from the start, then cut off whatever an older, longer text left behind.
		const std::string text = std::format("{}\n", Process::GetCurrentId());
		const ssize_t written = pwrite(file.Get(), text.data(), text.size(), 0);
		if (written != static_cast<ssize_t>(text.size()))
		{
			// A short write sets no errno; for a lock file of a few bytes it means the disk is full.
			const int error = written < 0 ? errno : ENOSPC;
			return MakeError(ErrorCode::Io, "cannot write the process ID into '{}': {}", lockFile.native(), Utils::DescribeErrno(error));
		}
		if (ftruncate(file.Get(), static_cast<off_t>(text.size())) != 0)
		{
			const int error = errno;
			return MakeError(ErrorCode::Io, "cannot truncate '{}': {}", lockFile.native(), Utils::DescribeErrno(error));
		}

		Scope<Impl> impl = CreateScope<Impl>();
		impl->Path = lockFile;
		impl->Descriptor = file.Release();
		return ProjectLock(std::move(impl));
	}

	Result<bool> ProjectLock::IsHeld(const std::filesystem::path& lockFile)
	{
		ScopedDescriptor file(open(lockFile.c_str(), O_RDONLY | O_CLOEXEC));
		if (file.Get() < 0)
		{
			const int error = errno;
			if (error == ENOENT || error == ENOTDIR)
				return false;
			return MakeError(ErrorCode::Io, "cannot open the lock file '{}': {}", lockFile.native(), Utils::DescribeErrno(error));
		}

		// A shared lock conflicts with a holder's exclusive lock but not with other probes; closing the descriptor
		// releases it.
		const int lockError = Utils::LockFile(file.Get(), LOCK_SH | LOCK_NB);
		if (lockError == EWOULDBLOCK)
			return true;
		if (lockError != 0)
			return MakeError(ErrorCode::Io, "cannot test the lock of '{}': {}", lockFile.native(), Utils::DescribeErrno(lockError));
		return false;
	}

	const std::filesystem::path& ProjectLock::GetPath() const
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "GetPath called on a moved-from ProjectLock");
		return m_Impl->Path;
	}

}

#endif
