#include "EnginePCH.h"
#include "Engine/Platform/Process.h"

// Child processes and current-process queries on Linux and macOS. A child starts through posix_spawn with standard input
// on /dev/null and standard output and standard error on close-on-exec pipes. One I/O thread per child polls both pipes,
// a wake-up pipe that stops it, and the child's exit (a pidfd on Linux, a kqueue NOTE_EXIT on macOS; a 10 ms poll where
// neither is available), and reaps the child when it exits.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Core/Utf8.h"
	#include "Engine/Platform/Private/ProcessOutput.h"

	#include <fcntl.h>
	#include <poll.h>
	#include <signal.h>
	#include <spawn.h>
	#include <sys/resource.h>
	#include <sys/wait.h>
	#include <unistd.h>
	#if defined(ENGINE_PLATFORM_LINUX)
		#include <sys/syscall.h>
	#elif defined(ENGINE_PLATFORM_MACOS)
		#include <crt_externs.h>
		#include <mach-o/dyld.h>
		#include <sys/event.h>
		#include <sys/sysctl.h>
		#include <sys/types.h>
	#endif

	#include <cerrno>
	#include <limits>
	#include <system_error>
	#include <thread>

namespace Engine {

	namespace {

		// Closes a file descriptor on destruction.
		class ScopedDescriptor
		{
		public:
			ScopedDescriptor() = default;

			~ScopedDescriptor()
			{
				Reset();
			}

			ScopedDescriptor(const ScopedDescriptor&) = delete;
			ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
			ScopedDescriptor(ScopedDescriptor&&) = delete;
			ScopedDescriptor& operator=(ScopedDescriptor&&) = delete;

			void Reset(int descriptor = -1)
			{
				if (m_Descriptor >= 0)
					close(m_Descriptor);
				m_Descriptor = descriptor;
			}

			[[nodiscard]] int Release()
			{
				return std::exchange(m_Descriptor, -1);
			}

			[[nodiscard]] int Get() const { return m_Descriptor; }
		private:
			int m_Descriptor = -1;
		};

		class ScopedFileActions
		{
		public:
			ScopedFileActions()
				: m_Error(posix_spawn_file_actions_init(&m_Actions))
			{
			}

			~ScopedFileActions()
			{
				if (m_Error == 0)
					posix_spawn_file_actions_destroy(&m_Actions);
			}

			ScopedFileActions(const ScopedFileActions&) = delete;
			ScopedFileActions& operator=(const ScopedFileActions&) = delete;
			ScopedFileActions(ScopedFileActions&&) = delete;
			ScopedFileActions& operator=(ScopedFileActions&&) = delete;

			[[nodiscard]] int GetInitError() const { return m_Error; }
			[[nodiscard]] posix_spawn_file_actions_t* Get() { return &m_Actions; }
		private:
			posix_spawn_file_actions_t m_Actions{};
			int m_Error = 0;
		};

		class ScopedSpawnAttributes
		{
		public:
			ScopedSpawnAttributes()
				: m_Error(posix_spawnattr_init(&m_Attributes))
			{
			}

			~ScopedSpawnAttributes()
			{
				if (m_Error == 0)
					posix_spawnattr_destroy(&m_Attributes);
			}

			ScopedSpawnAttributes(const ScopedSpawnAttributes&) = delete;
			ScopedSpawnAttributes& operator=(const ScopedSpawnAttributes&) = delete;
			ScopedSpawnAttributes(ScopedSpawnAttributes&&) = delete;
			ScopedSpawnAttributes& operator=(ScopedSpawnAttributes&&) = delete;

			[[nodiscard]] int GetInitError() const { return m_Error; }
			[[nodiscard]] posix_spawnattr_t* Get() { return &m_Attributes; }
		private:
			posix_spawnattr_t m_Attributes{};
			int m_Error = 0;
		};

	}

	namespace Utils {

		// How often the I/O thread checks for the child's exit when the OS offers no exit notification.
		constexpr int ExitPollMilliseconds = 10;

		static std::string DescribeErrno(std::string_view what, int code)
		{
			return std::format("{} failed: {}", what, std::generic_category().message(code));
		}

		static std::unexpected<Error> MakeErrnoError(std::string_view what, int code)
		{
			return MakeError(ErrorCode::Io, "{}", DescribeErrno(what, code));
		}

		// Whether `executable` names an existing file, and why not.
		static Status CheckExecutableExists(const std::filesystem::path& executable)
		{
			if (executable.empty())
				return MakeError(ErrorCode::NotFound, "no executable was given");
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(executable, error);
			if (status.type() == std::filesystem::file_type::not_found)
				return MakeError(ErrorCode::NotFound, "the executable '{}' does not exist", executable.native());
			if (error)
				return MakeError(ErrorCode::Io, "cannot inspect the executable '{}': {}", executable.native(), error.message());
			if (std::filesystem::is_directory(status))
				return MakeError(ErrorCode::NotFound, "the executable '{}' is a directory", executable.native());
			return {};
		}

		// InvalidArgument unless `text` is valid UTF-8 without NUL characters, which no argv or environ entry can carry.
		static Status CheckSpecificationText(std::string_view text, std::string_view what)
		{
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::InvalidArgument, "the {} is not valid UTF-8", what);
			if (text.find('\0') != std::string_view::npos)
				return MakeError(ErrorCode::InvalidArgument, "the {} contains a NUL character", what);
			return {};
		}

		// A pipe whose ends are closed on exec, so children spawned concurrently by other threads never inherit them.
		static Status CreateCloseOnExecPipe(ScopedDescriptor& readEnd, ScopedDescriptor& writeEnd)
		{
			std::array<int, 2> descriptors = { -1, -1 };
	#if defined(ENGINE_PLATFORM_LINUX)
			if (pipe2(descriptors.data(), O_CLOEXEC) != 0)
				return MakeErrnoError("pipe2", errno);
			readEnd.Reset(descriptors[0]);
			writeEnd.Reset(descriptors[1]);
	#elif defined(ENGINE_PLATFORM_MACOS)
			// macOS has no pipe2; POSIX_SPAWN_CLOEXEC_DEFAULT (below) keeps these out of children spawned here meanwhile.
			if (pipe(descriptors.data()) != 0)
				return MakeErrnoError("pipe", errno);
			readEnd.Reset(descriptors[0]);
			writeEnd.Reset(descriptors[1]);
			if (fcntl(readEnd.Get(), F_SETFD, FD_CLOEXEC) != 0 || fcntl(writeEnd.Get(), F_SETFD, FD_CLOEXEC) != 0)
				return MakeErrnoError("fcntl(FD_CLOEXEC)", errno);
	#endif
			return {};
		}

		static char** GetEnvironment()
		{
	#if defined(ENGINE_PLATFORM_MACOS)
			return *_NSGetEnviron();
	#elif defined(ENGINE_PLATFORM_LINUX)
			return environ;
	#endif
		}

		// The child's environment entries: the parent's variables, minus those the specification sets, plus the
		// specification's entries (the last one of a name wins). Empty when the specification sets nothing, so the child
		// gets the parent's environment unchanged.
		static Result<std::vector<std::string>> BuildEnvironment(const std::vector<std::pair<std::string, std::string>>& overrides)
		{
			if (overrides.empty())
				return std::vector<std::string>();

			std::vector<std::pair<std::string_view, std::string_view>> variables;
			for (const std::pair<std::string, std::string>& entry : overrides)
			{
				const std::string_view name = entry.first;
				const std::string_view value = entry.second;
				if (name.empty())
					return MakeError(ErrorCode::InvalidArgument, "an environment variable has an empty name");
				if (name.find('=') != std::string_view::npos)
					return MakeError(ErrorCode::InvalidArgument, "the environment variable name '{}' contains '='", name);
				ENGINE_TRY(CheckSpecificationText(name, "environment variable name"));
				ENGINE_TRY(CheckSpecificationText(value, std::format("value of the environment variable '{}'", name)));

				const auto existing = std::ranges::find_if(variables, [name](const std::pair<std::string_view, std::string_view>& variable)
				{
					return variable.first == name;
				});
				if (existing != variables.end())
					existing->second = value;
				else
					variables.emplace_back(name, value);
			}

			std::vector<std::string> entries;
			for (char** entry = GetEnvironment(); entry != nullptr && *entry != nullptr; ++entry)
			{
				const std::string_view text(*entry);
				const std::string_view name = text.substr(0, text.find('='));
				const bool isOverridden = std::ranges::any_of(variables, [name](const auto& variable)
				{
					return variable.first == name;
				});
				if (!isOverridden)
					entries.emplace_back(text);
			}
			for (const auto& [name, value] : variables)
				entries.push_back(std::format("{}={}", name, value));
			return entries;
		}

		static int DecodeWaitStatus(int status)
		{
			if (WIFEXITED(status))
				return WEXITSTATUS(status);
			if (WIFSIGNALED(status))
				return 128 + WTERMSIG(status);
			return -1;
		}

		// A descriptor that becomes readable when the child exits, or -1 when the OS offers none (the I/O thread then
		// polls).
		static int OpenExitNotifier(pid_t pid)
		{
	#if defined(ENGINE_PLATFORM_LINUX)
		#if defined(SYS_pidfd_open)
			// pidfd_open (Linux 5.3) returns a close-on-exec descriptor that polls readable once the process exits.
			const long descriptor = syscall(SYS_pidfd_open, pid, 0);
			return descriptor >= 0 ? static_cast<int>(descriptor) : -1;
		#else
			static_cast<void>(pid);
			return -1;
		#endif
	#elif defined(ENGINE_PLATFORM_MACOS)
			// A kqueue polls readable once its NOTE_EXIT event fired; kqueues are never inherited by children.
			const int queue = kqueue();
			if (queue < 0)
				return -1;
			struct kevent change{};
			change.ident = static_cast<uintptr_t>(pid);
			change.filter = EVFILT_PROC;
			change.flags = static_cast<uint16_t>(EV_ADD | EV_ONESHOT);
			change.fflags = NOTE_EXIT;
			if (kevent(queue, &change, 1, nullptr, 0, nullptr) != 0)
			{
				close(queue);
				return -1;
			}
			return queue;
	#endif
		}

	}

	struct Process::Impl
	{
		pid_t Pid = -1;
		std::string ExecutableText{}; // for messages
		ScopedDescriptor OutputRead;
		ScopedDescriptor ErrorRead;
		ScopedDescriptor WakeRead; // the destructor writes to WakeWrite to stop the I/O thread
		ScopedDescriptor WakeWrite;
		ScopedDescriptor ExitNotifier;
		Utils::ProcessOutput Output;
		// waitpid runs only under this lock, and kill only while it is held and the child is not reaped, so the pid is
		// never signalled after the OS could have given it to another process.
		std::mutex ReapMutex;
		bool IsReaped = false;
		std::thread IoThread;

		Impl() = default;

		~Impl()
		{
			// No child outlives its Process.
			if (Pid > 0)
				static_cast<void>(KillAndReap());
			if (IoThread.joinable())
			{
				const char wake = 1;
				while (write(WakeWrite.Get(), &wake, 1) < 0 && errno == EINTR)
				{
				}
				IoThread.join();
			}
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;
		Impl(Impl&&) = delete;
		Impl& operator=(Impl&&) = delete;

		// Reaps the child if it has exited, without blocking. True once it is reaped.
		[[nodiscard]] bool TryReap()
		{
			std::scoped_lock lock(ReapMutex);
			if (IsReaped)
				return true;
			int status = 0;
			pid_t waited = 0;
			do
			{
				waited = waitpid(Pid, &status, WNOHANG);
			} while (waited < 0 && errno == EINTR);
			if (waited == 0)
				return false;
			// ECHILD means the child was reaped elsewhere (SIGCHLD ignored by the process): its exit code is lost.
			IsReaped = true;
			Output.SetExitCode(waited == Pid ? Utils::DecodeWaitStatus(status) : -1);
			return true;
		}

		[[nodiscard]] Status KillAndReap()
		{
			std::scoped_lock lock(ReapMutex);
			if (IsReaped)
				return {};
			if (kill(Pid, SIGKILL) != 0)
			{
				const int error = errno;
				return MakeError(ErrorCode::Io, "cannot kill '{}': {}", ExecutableText, Utils::DescribeErrno("kill", error));
			}
			int status = 0;
			pid_t waited = 0;
			do
			{
				waited = waitpid(Pid, &status, 0);
			} while (waited < 0 && errno == EINTR);
			IsReaped = true;
			Output.SetExitCode(waited == Pid ? Utils::DecodeWaitStatus(status) : -1);
			return {};
		}

		// The I/O thread: drains both pipes into the output until they reach end of file, reaps the child when it exits,
		// and returns when both are done or the wake-up pipe is written.
		void RunIo()
		{
			std::array<bool, 2> isOpen = { true, true };
			const std::array<int, 2> pipes = { OutputRead.Get(), ErrorRead.Get() };
			constexpr std::array<ProcessStream, 2> Streams = { ProcessStream::StandardOutput, ProcessStream::StandardError };
			std::array<char, 4096> buffer{};
			bool isExited = TryReap();
			bool isStopping = false;
			while (!isStopping && (isOpen[0] || isOpen[1] || !isExited))
			{
				// Slot 0 is the wake-up pipe, 1 and 2 the output pipes, 3 the exit notifier; closed ones are skipped by
				// giving poll a negative descriptor.
				const bool isWatchingExit = !isExited && ExitNotifier.Get() >= 0;
				std::array<pollfd, 4> descriptors = {
					pollfd{ WakeRead.Get(), POLLIN, 0 },
					pollfd{ isOpen[0] ? pipes[0] : -1, POLLIN, 0 },
					pollfd{ isOpen[1] ? pipes[1] : -1, POLLIN, 0 },
					pollfd{ isWatchingExit ? ExitNotifier.Get() : -1, POLLIN, 0 },
				};
				const int timeout = !isExited && !isWatchingExit ? Utils::ExitPollMilliseconds : -1;
				const int ready = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout);
				if (ready < 0)
				{
					if (errno == EINTR)
						continue;
					break; // nothing sensible remains to wait on; the streams are marked as ended below
				}

				if (descriptors[0].revents != 0)
					isStopping = true;
				for (size_t index = 0; index < pipes.size(); ++index)
				{
					if (!isOpen[index] || descriptors[index + 1].revents == 0)
						continue;
					const ssize_t count = read(pipes[index], buffer.data(), buffer.size());
					if (count > 0)
					{
						Output.Append(Streams[index], std::string_view(buffer.data(), static_cast<size_t>(count)));
					}
					else if (count == 0 || (errno != EINTR && errno != EAGAIN))
					{
						isOpen[index] = false; // end of file, or a broken pipe
						Output.MarkEnded(Streams[index]);
					}
				}
	#if defined(ENGINE_PLATFORM_MACOS)
				if (isWatchingExit && descriptors[3].revents != 0)
				{
					// Consume the fired event so the kqueue stops polling readable.
					struct kevent event{};
					const timespec noWait{};
					static_cast<void>(kevent(ExitNotifier.Get(), nullptr, 0, &event, 1, &noWait));
				}
	#endif
				if (!isExited && (!isWatchingExit || descriptors[3].revents != 0))
					isExited = TryReap();
			}

			for (size_t index = 0; index < pipes.size(); ++index)
			{
				if (isOpen[index])
					Output.MarkEnded(Streams[index]);
			}
		}
	};

	Process::Process(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Process::~Process() = default;

	Process::Process(Process&& other) noexcept = default;

	Process& Process::operator=(Process&& other) noexcept = default;

	Result<Process> Process::Spawn(const ProcessSpecification& specification)
	{
		ENGINE_TRY(Utils::CheckExecutableExists(specification.Executable));
		// posix_spawn resolves a relative path after the child changed its directory, so the path is made absolute first.
		std::error_code error;
		const std::filesystem::path executable = std::filesystem::absolute(specification.Executable, error);
		if (error)
		{
			return MakeError(ErrorCode::Io, "cannot make the executable path '{}' absolute: {}", specification.Executable.native(),
				error.message());
		}

		std::string executableText = executable.native();
		std::vector<std::string> argumentStorage = specification.Arguments;
		for (size_t index = 0; index < argumentStorage.size(); ++index)
			ENGINE_TRY(Utils::CheckSpecificationText(argumentStorage[index], std::format("argument {}", index + 1)));
		std::vector<char*> argv;
		argv.reserve(argumentStorage.size() + 2);
		argv.push_back(executableText.data());
		for (std::string& argument : argumentStorage)
			argv.push_back(argument.data());
		argv.push_back(nullptr);

		ENGINE_TRY_ASSIGN(std::vector<std::string> environmentStorage, Utils::BuildEnvironment(specification.Environment));
		std::vector<char*> environment;
		if (!environmentStorage.empty())
		{
			environment.reserve(environmentStorage.size() + 1);
			for (std::string& entry : environmentStorage)
				environment.push_back(entry.data());
			environment.push_back(nullptr);
		}

		ScopedDescriptor outputRead;
		ScopedDescriptor outputWrite;
		ScopedDescriptor errorRead;
		ScopedDescriptor errorWrite;
		ScopedDescriptor wakeRead;
		ScopedDescriptor wakeWrite;
		ENGINE_TRY(Utils::CreateCloseOnExecPipe(outputRead, outputWrite));
		ENGINE_TRY(Utils::CreateCloseOnExecPipe(errorRead, errorWrite));
		ENGINE_TRY(Utils::CreateCloseOnExecPipe(wakeRead, wakeWrite));

		ScopedFileActions actions;
		if (actions.GetInitError() != 0)
			return Utils::MakeErrnoError("posix_spawn_file_actions_init", actions.GetInitError());
		// dup2 clears close-on-exec on the duplicate, so these three descriptors survive the exec. Every other descriptor
		// of the parent is closed, including those a library opened without close-on-exec (spdlog's log file): on Linux
		// with closefrom (glibc 2.34), on macOS with POSIX_SPAWN_CLOEXEC_DEFAULT below.
		int setupError = posix_spawn_file_actions_addopen(actions.Get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		if (setupError == 0)
			setupError = posix_spawn_file_actions_adddup2(actions.Get(), outputWrite.Get(), STDOUT_FILENO);
		if (setupError == 0)
			setupError = posix_spawn_file_actions_adddup2(actions.Get(), errorWrite.Get(), STDERR_FILENO);
	#if defined(ENGINE_PLATFORM_LINUX)
		if (setupError == 0)
			setupError = posix_spawn_file_actions_addclosefrom_np(actions.Get(), STDERR_FILENO + 1);
	#endif
		if (setupError == 0 && !specification.WorkingDirectory.empty())
			setupError = posix_spawn_file_actions_addchdir_np(actions.Get(), specification.WorkingDirectory.c_str());
		if (setupError != 0)
			return Utils::MakeErrnoError("posix_spawn_file_actions", setupError);

		ScopedSpawnAttributes attributes;
		if (attributes.GetInitError() != 0)
			return Utils::MakeErrnoError("posix_spawnattr_init", attributes.GetInitError());
		// The child starts with no blocked signals, whatever the spawning thread blocks, and with the default action for
		// the signals a parent commonly ignores (an ignored disposition would otherwise survive the exec).
		sigset_t noSignals{};
		sigemptyset(&noSignals);
		sigset_t defaultSignals{};
		sigemptyset(&defaultSignals);
		for (const int signalNumber : { SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD })
			sigaddset(&defaultSignals, signalNumber);
	#if defined(ENGINE_PLATFORM_MACOS)
		const auto flags = static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
	#elif defined(ENGINE_PLATFORM_LINUX)
		const auto flags = static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	#endif
		setupError = posix_spawnattr_setsigmask(attributes.Get(), &noSignals);
		if (setupError == 0)
			setupError = posix_spawnattr_setsigdefault(attributes.Get(), &defaultSignals);
		if (setupError == 0)
			setupError = posix_spawnattr_setflags(attributes.Get(), flags);
		if (setupError != 0)
			return Utils::MakeErrnoError("posix_spawnattr", setupError);

		pid_t pid = 0;
		const int spawnError = posix_spawn(&pid, executableText.c_str(), actions.Get(), attributes.Get(), argv.data(),
			environment.empty() ? Utils::GetEnvironment() : environment.data());
		if (spawnError != 0)
		{
			// The executable was checked above, so ENOENT here usually means a missing working directory.
			const std::string where = specification.WorkingDirectory.empty()
				? std::string()
				: std::format(" in '{}'", specification.WorkingDirectory.native());
			return MakeError(ErrorCode::Io, "cannot start '{}'{}: {}", executableText, where,
				Utils::DescribeErrno("posix_spawn", spawnError));
		}
		// The child holds its own copies now; ours must close, or the pipes would never reach end of file.
		outputWrite.Reset();
		errorWrite.Reset();

		Scope<Impl> impl = CreateScope<Impl>();
		impl->Pid = pid;
		impl->ExecutableText = std::move(executableText);
		impl->OutputRead.Reset(outputRead.Release());
		impl->ErrorRead.Reset(errorRead.Release());
		impl->WakeRead.Reset(wakeRead.Release());
		impl->WakeWrite.Reset(wakeWrite.Release());
		impl->ExitNotifier.Reset(Utils::OpenExitNotifier(pid));
		Impl* state = impl.get();
		impl->IoThread = std::thread([state]()
		{
			state->RunIo();
		});
		return Process(std::move(impl));
	}

	uint32_t Process::GetId() const
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "GetId called on a moved-from Process");
		return static_cast<uint32_t>(m_Impl->Pid);
	}

	bool Process::HasExited()
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "HasExited called on a moved-from Process");
		return m_Impl->TryReap();
	}

	Result<ProcessResult> Process::Wait(std::chrono::milliseconds timeout)
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "Wait called on a moved-from Process");
		Impl& impl = *m_Impl;
		if (impl.Output.IsTaken())
			return MakeError(ErrorCode::InvalidState, "the process '{}' was already waited for", impl.ExecutableText);

		const Utils::ProcessClock::time_point deadline = Utils::MakeProcessDeadline(timeout);
		if (!impl.Output.WaitUntilFinished(deadline, true))
		{
			if (!impl.Output.GetExitCode().has_value())
				return MakeError(ErrorCode::Timeout, "'{}' is still running after {} ms", impl.ExecutableText, timeout.count());
			return MakeError(ErrorCode::Timeout, "'{}' exited, but a process it started kept its output open for {} ms", impl.ExecutableText,
				timeout.count());
		}

		std::array<std::string, 2> text = impl.Output.TakeText();
		ProcessResult result;
		result.ExitCode = impl.Output.GetExitCode().value_or(-1);
		result.StandardOutput = std::move(text[0]);
		result.StandardError = std::move(text[1]);
		return result;
	}

	Status Process::WaitForOutput(ProcessStream stream, std::string_view text, std::chrono::milliseconds timeout)
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "WaitForOutput called on a moved-from Process");
		return m_Impl->Output.WaitForText(stream, text, Utils::MakeProcessDeadline(timeout));
	}

	Status Process::Kill()
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "Kill called on a moved-from Process");
		return m_Impl->KillAndReap();
	}

	uint32_t Process::GetCurrentId()
	{
		return static_cast<uint32_t>(getpid());
	}

	bool Process::IsRunning(uint32_t processId)
	{
		// kill() with a pid that is 0 or negative signals process groups, so those values are never passed on.
		if (processId == 0 || processId > static_cast<uint32_t>(std::numeric_limits<pid_t>::max()))
			return false;
		if (kill(static_cast<pid_t>(processId), 0) == 0)
			return true;
		return errno == EPERM; // it exists, but belongs to another user
	}

	Result<std::filesystem::path> Process::GetCurrentExecutablePath()
	{
		std::error_code error;
	#if defined(ENGINE_PLATFORM_LINUX)
		std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot read /proc/self/exe: {}", error.message());
	#elif defined(ENGINE_PLATFORM_MACOS)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size); // reports the size the buffer needs
		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return MakeError(ErrorCode::Io, "_NSGetExecutablePath failed");
		buffer.resize(std::char_traits<char>::length(buffer.c_str()));
		std::filesystem::path resolved = std::filesystem::canonical(std::filesystem::path(buffer), error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot resolve the running executable '{}': {}", buffer, error.message());
	#endif
		return resolved;
	}

	Result<double> Process::GetCurrentCpuSeconds()
	{
		rusage usage{};
		if (getrusage(RUSAGE_SELF, &usage) != 0)
			return Utils::MakeErrnoError("getrusage", errno);
		const auto toSeconds = [](const timeval& time)
		{
			return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1.0e6;
		};
		return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
	}

	bool Process::IsDebuggerAttached()
	{
	#if defined(ENGINE_PLATFORM_LINUX)
		// "TracerPid:\t<pid>" in /proc/self/status names the tracing process; 0 means none.
		const int file = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
		if (file < 0)
			return false;
		std::array<char, 8192> buffer{};
		size_t size = 0;
		while (size < buffer.size())
		{
			const ssize_t count = read(file, buffer.data() + size, buffer.size() - size);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			size += static_cast<size_t>(count);
		}
		close(file);

		constexpr std::string_view Key = "TracerPid:";
		const std::string_view status(buffer.data(), size);
		const size_t key = status.find(Key);
		if (key == std::string_view::npos)
			return false;
		size_t index = key + Key.size();
		while (index < status.size() && (status[index] == ' ' || status[index] == '\t'))
			++index;
		return index < status.size() && status[index] >= '1' && status[index] <= '9';
	#elif defined(ENGINE_PLATFORM_MACOS)
		std::array<int, 4> name = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
		kinfo_proc information{};
		size_t size = sizeof(information);
		if (sysctl(name.data(), static_cast<u_int>(name.size()), &information, &size, nullptr, 0) != 0)
			return false;
		return (information.kp_proc.p_flag & P_TRACED) != 0;
	#endif
	}

	void Process::BreakIntoDebugger()
	{
		raise(SIGTRAP);
	}

}

#endif
