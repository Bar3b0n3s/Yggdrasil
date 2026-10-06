#include "TestsPCH.h"
#include "Support/ChildProcess.h"

#if defined(ENGINE_PLATFORM_WINDOWS)
	#include <windows.h>
#elif defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
	#include <fcntl.h>
	#include <poll.h>
	#include <signal.h>
	#include <spawn.h>
	#include <sys/wait.h>
	#include <unistd.h>
	#if defined(ENGINE_PLATFORM_MACOS)
		#include <crt_externs.h>
		#include <mach-o/dyld.h>
	#endif
#endif

#include <cerrno>
#include <climits>
#include <system_error>
#include <thread>

// The M1 stand-in for Platform::Process (Docs/Decisions/0003-m1-contract-decisions.md, decision 1): the only Tests file
// with OS headers. Both implementations read standard output and standard error concurrently until both reach end of
// file, so a child that fills one pipe while the parent waits on the other cannot deadlock, and both enforce one deadline
// for the whole run.

namespace Engine {

	namespace Test {

		namespace Utils {

			static std::string PathToUtf8(const std::filesystem::path& path)
			{
				const std::u8string text = path.u8string();
				return std::string(reinterpret_cast<const char*>(text.data()), text.size());
			}

			// Whether `executable` names an existing file, and why not.
			static Status CheckExecutableExists(const std::filesystem::path& executable)
			{
				std::error_code error;
				const std::filesystem::file_status status = std::filesystem::status(executable, error);
				if (status.type() == std::filesystem::file_type::not_found || executable.empty())
					return MakeError(ErrorCode::NotFound, "the executable '{}' does not exist", PathToUtf8(executable));
				if (error)
					return MakeError(ErrorCode::Io, "cannot inspect the executable '{}': {}", PathToUtf8(executable), error.message());
				if (std::filesystem::is_directory(status))
					return MakeError(ErrorCode::NotFound, "the executable '{}' is a directory", PathToUtf8(executable));
				return {};
			}

		}

#if defined(ENGINE_PLATFORM_WINDOWS)

		namespace {

			// Closes a Win32 handle on destruction.
			class ScopedHandle
			{
			public:
				ScopedHandle() = default;

				explicit ScopedHandle(HANDLE handle)
					: m_Handle(handle)
				{
				}

				~ScopedHandle()
				{
					Reset();
				}

				ScopedHandle(const ScopedHandle&) = delete;
				ScopedHandle& operator=(const ScopedHandle&) = delete;
				ScopedHandle(ScopedHandle&&) = delete;
				ScopedHandle& operator=(ScopedHandle&&) = delete;

				void Reset(HANDLE handle = nullptr)
				{
					if (m_Handle != nullptr && m_Handle != INVALID_HANDLE_VALUE)
						CloseHandle(m_Handle);
					m_Handle = handle;
				}

				[[nodiscard]] HANDLE Get() const { return m_Handle; }
				[[nodiscard]] HANDLE* Out() { return &m_Handle; }
				[[nodiscard]] bool IsValid() const { return m_Handle != nullptr && m_Handle != INVALID_HANDLE_VALUE; }
			private:
				HANDLE m_Handle = nullptr;
			};

			// Frees a PROC_THREAD_ATTRIBUTE_LIST initialized in caller-owned storage.
			class ScopedAttributeList
			{
			public:
				explicit ScopedAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST list)
					: m_List(list)
				{
				}

				~ScopedAttributeList()
				{
					DeleteProcThreadAttributeList(m_List);
				}

				ScopedAttributeList(const ScopedAttributeList&) = delete;
				ScopedAttributeList& operator=(const ScopedAttributeList&) = delete;
				ScopedAttributeList(ScopedAttributeList&&) = delete;
				ScopedAttributeList& operator=(ScopedAttributeList&&) = delete;
			private:
				LPPROC_THREAD_ATTRIBUTE_LIST m_List = nullptr;
			};

		}

		namespace Utils {

			// Call it right after the failing call: anything in between may overwrite the thread's last-error value.
			static std::string DescribeLastError(std::string_view what)
			{
				const DWORD code = GetLastError();
				return std::format("{} failed: {}", what, std::system_category().message(static_cast<int>(code)));
			}

			static std::unexpected<Error> MakeLastError(std::string_view what)
			{
				return MakeError(ErrorCode::Io, "{}", DescribeLastError(what));
			}

			static Result<std::wstring> WideFromUtf8(std::string_view text)
			{
				if (text.empty())
					return std::wstring();
				if (text.size() > static_cast<size_t>(INT_MAX))
					return MakeError(ErrorCode::Io, "an argument of {} bytes is too long for a command line", text.size());

				const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
					nullptr, 0);
				if (size <= 0)
					return MakeError(ErrorCode::Io, "the argument '{}' is not valid UTF-8", text);
				std::wstring wide(static_cast<size_t>(size), L'\0');
				MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
				return wide;
			}

			// Appends `argument` so that the C runtime's command-line parser (and CommandLineToArgvW) reproduces it exactly:
			// backslashes are literal unless they precede a quote, where each one and the quote are escaped.
			static void AppendQuotedArgument(std::wstring& commandLine, const std::wstring& argument)
			{
				if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
				{
					commandLine += argument;
					return;
				}

				commandLine += L'"';
				size_t backslashes = 0;
				for (const wchar_t character : argument)
				{
					if (character == L'\\')
					{
						++backslashes;
						continue;
					}
					if (character == L'"')
						commandLine.append(backslashes * 2 + 1, L'\\');
					else
						commandLine.append(backslashes, L'\\');
					backslashes = 0;
					commandLine += character;
				}
				commandLine.append(backslashes * 2, L'\\'); // the closing quote must not be escaped
				commandLine += L'"';
			}

			// Reads `pipe` until the writer closes it (or the read fails), appending everything to `output`.
			static void ReadPipeToEnd(HANDLE pipe, std::string& output)
			{
				std::array<char, 4096> buffer{};
				DWORD bytesRead = 0;
				while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
					output.append(buffer.data(), bytesRead);
			}

			static Status CreateInheritablePipe(ScopedHandle& readEnd, ScopedHandle& writeEnd)
			{
				SECURITY_ATTRIBUTES attributes{};
				attributes.nLength = sizeof(attributes);
				attributes.bInheritHandle = TRUE;
				if (!CreatePipe(readEnd.Out(), writeEnd.Out(), &attributes, 0))
					return MakeLastError("CreatePipe");
				// Only the child's end is inheritable, and only by the child it is listed for (see the handle list).
				if (!SetHandleInformation(readEnd.Get(), HANDLE_FLAG_INHERIT, 0))
					return MakeLastError("SetHandleInformation");
				return {};
			}

		}

		Result<ChildProcessResult> RunChildProcess(const std::filesystem::path& executable, std::span<const std::string> arguments,
			std::chrono::milliseconds timeout)
		{
			ENGINE_TRY(Utils::CheckExecutableExists(executable));

			std::wstring commandLine = L"\"" + executable.native() + L"\""; // argv[0]: quotes delimit, no escapes
			for (const std::string& argument : arguments)
			{
				ENGINE_TRY_ASSIGN(const std::wstring wide, Utils::WideFromUtf8(argument));
				commandLine += L' ';
				Utils::AppendQuotedArgument(commandLine, wide);
			}

			ScopedHandle outputRead;
			ScopedHandle outputWrite;
			ScopedHandle errorRead;
			ScopedHandle errorWrite;
			ENGINE_TRY(Utils::CreateInheritablePipe(outputRead, outputWrite));
			ENGINE_TRY(Utils::CreateInheritablePipe(errorRead, errorWrite));

			SECURITY_ATTRIBUTES inheritable{};
			inheritable.nLength = sizeof(inheritable);
			inheritable.bInheritHandle = TRUE;
			ScopedHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!input.IsValid())
				return Utils::MakeLastError("Opening NUL for the child's standard input");

			// The child inherits exactly its three standard handles, not every inheritable handle of this process, so pipes
			// created concurrently for another child never stay open in this one (which would delay their end of file).
			std::array<HANDLE, 3> inherited = { input.Get(), outputWrite.Get(), errorWrite.Get() };
			SIZE_T attributeListSize = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize); // fails by design: it reports the size
			std::vector<std::byte> attributeListStorage(attributeListSize);
			const LPPROC_THREAD_ATTRIBUTE_LIST attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
			if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize))
				return Utils::MakeLastError("InitializeProcThreadAttributeList");
			const ScopedAttributeList attributeListGuard(attributeList);
			const BOOL isListUpdated = UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
				inherited.size() * sizeof(HANDLE), nullptr, nullptr);
			if (!isListUpdated)
				return Utils::MakeLastError("UpdateProcThreadAttribute");

			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(startup);
			startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdInput = input.Get();
			startup.StartupInfo.hStdOutput = outputWrite.Get();
			startup.StartupInfo.hStdError = errorWrite.Get();
			startup.lpAttributeList = attributeList;

			// CREATE_NO_WINDOW: the child's standard handles are the pipes, so it needs no console of its own.
			constexpr DWORD CreationFlags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
			PROCESS_INFORMATION information{};
			if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CreationFlags, nullptr, nullptr,
					&startup.StartupInfo, &information))
			{
				const std::string reason = Utils::DescribeLastError("CreateProcessW");
				return MakeError(ErrorCode::Io, "cannot start '{}': {}", Utils::PathToUtf8(executable), reason);
			}

			const ScopedHandle process(information.hProcess);
			const ScopedHandle thread(information.hThread);
			// The child holds its own copies now; ours must close, or the pipes would never reach end of file.
			outputWrite.Reset();
			errorWrite.Reset();
			input.Reset();

			ChildProcessResult result;
			std::thread outputReader([&outputRead, &result]()
			{
				Utils::ReadPipeToEnd(outputRead.Get(), result.StandardOutput);
			});
			std::thread errorReader([&errorRead, &result]()
			{
				Utils::ReadPipeToEnd(errorRead.Get(), result.StandardError);
			});

			const int64_t timeoutMilliseconds = std::clamp<int64_t>(timeout.count(), 0, static_cast<int64_t>(INFINITE) - 1);
			const DWORD waited = WaitForSingleObject(process.Get(), static_cast<DWORD>(timeoutMilliseconds));
			const std::string waitFailure = waited == WAIT_FAILED ? Utils::DescribeLastError("WaitForSingleObject") : std::string();
			if (waited != WAIT_OBJECT_0)
			{
				// The pipes reach end of file only once the child is gone, so it is ended before the readers are joined.
				TerminateProcess(process.Get(), static_cast<UINT>(ERROR_TIMEOUT));
				WaitForSingleObject(process.Get(), INFINITE);
			}
			outputReader.join();
			errorReader.join();

			if (waited == WAIT_FAILED)
				return MakeError(ErrorCode::Io, "{}", waitFailure);
			if (waited != WAIT_OBJECT_0)
			{
				return MakeError(ErrorCode::Timeout, "'{}' ran longer than {} ms and was killed", Utils::PathToUtf8(executable),
					timeout.count());
			}

			DWORD exitCode = 0;
			if (!GetExitCodeProcess(process.Get(), &exitCode))
				return Utils::MakeLastError("GetExitCodeProcess");
			result.ExitCode = static_cast<int>(exitCode);
			return result;
		}

		Result<std::filesystem::path> GetCurrentExecutablePath()
		{
			constexpr size_t MaxPathCharacters = 32768; // the longest path Windows supports, with the terminator
			std::wstring buffer(MAX_PATH, L'\0');
			while (true)
			{
				const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0)
					return Utils::MakeLastError("GetModuleFileNameW");
				if (length < buffer.size())
				{
					buffer.resize(length);
					break;
				}
				if (buffer.size() >= MaxPathCharacters)
					return MakeError(ErrorCode::Io, "the path of the running executable is longer than {} characters", MaxPathCharacters);
				buffer.resize(std::min(buffer.size() * 2, MaxPathCharacters));
			}

			const std::filesystem::path reported(buffer);
			std::error_code error;
			std::filesystem::path resolved = std::filesystem::canonical(reported, error);
			if (error)
			{
				return MakeError(ErrorCode::Io, "cannot resolve the running executable '{}': {}", Utils::PathToUtf8(reported),
					error.message());
			}
			return resolved;
		}

#elif defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

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

				[[nodiscard]] int Get() const { return m_Descriptor; }
			private:
				int m_Descriptor = -1;
			};

			class ScopedFileActions
			{
			public:
				ScopedFileActions()
				{
					m_Error = posix_spawn_file_actions_init(&m_Actions);
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
				{
					m_Error = posix_spawnattr_init(&m_Attributes);
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

			static std::string DescribeErrno(std::string_view what, int code)
			{
				return std::format("{} failed: {}", what, std::generic_category().message(code));
			}

			// A pipe whose ends are closed on exec, so children spawned concurrently by other threads never inherit them.
			static Status CreateCloseOnExecPipe(ScopedDescriptor& readEnd, ScopedDescriptor& writeEnd)
			{
				std::array<int, 2> descriptors = { -1, -1 };
	#if defined(ENGINE_PLATFORM_LINUX)
				if (pipe2(descriptors.data(), O_CLOEXEC) != 0)
					return MakeError(ErrorCode::Io, "{}", DescribeErrno("pipe2", errno));
				readEnd.Reset(descriptors[0]);
				writeEnd.Reset(descriptors[1]);
	#elif defined(ENGINE_PLATFORM_MACOS)
				// macOS has no pipe2; POSIX_SPAWN_CLOEXEC_DEFAULT (below) keeps these out of children spawned here meanwhile.
				if (pipe(descriptors.data()) != 0)
					return MakeError(ErrorCode::Io, "{}", DescribeErrno("pipe", errno));
				readEnd.Reset(descriptors[0]);
				writeEnd.Reset(descriptors[1]);
				if (fcntl(readEnd.Get(), F_SETFD, FD_CLOEXEC) != 0 || fcntl(writeEnd.Get(), F_SETFD, FD_CLOEXEC) != 0)
					return MakeError(ErrorCode::Io, "{}", DescribeErrno("fcntl(FD_CLOEXEC)", errno));
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

			static int DecodeWaitStatus(int status)
			{
				if (WIFEXITED(status))
					return WEXITSTATUS(status);
				if (WIFSIGNALED(status))
					return 128 + WTERMSIG(status);
				return -1;
			}

			// Waits for `pid` to end, retrying when a signal interrupts the wait.
			static int WaitForExit(pid_t pid)
			{
				int status = 0;
				while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				{
				}
				return status;
			}

			static void KillAndReap(pid_t pid)
			{
				kill(pid, SIGKILL);
				static_cast<void>(WaitForExit(pid));
			}

			static int MillisecondsUntil(std::chrono::steady_clock::time_point deadline)
			{
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				return static_cast<int>(std::clamp<int64_t>(remaining.count() + 1, 0, INT_MAX));
			}

		}

		Result<ChildProcessResult> RunChildProcess(const std::filesystem::path& executable, std::span<const std::string> arguments,
			std::chrono::milliseconds timeout)
		{
			ENGINE_TRY(Utils::CheckExecutableExists(executable));
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;

			std::string executableText = executable.native();
			std::vector<std::string> argumentStorage(arguments.begin(), arguments.end());
			std::vector<char*> argv;
			argv.reserve(argumentStorage.size() + 2);
			argv.push_back(executableText.data());
			for (std::string& argument : argumentStorage)
				argv.push_back(argument.data());
			argv.push_back(nullptr);

			ScopedDescriptor outputRead;
			ScopedDescriptor outputWrite;
			ScopedDescriptor errorRead;
			ScopedDescriptor errorWrite;
			ENGINE_TRY(Utils::CreateCloseOnExecPipe(outputRead, outputWrite));
			ENGINE_TRY(Utils::CreateCloseOnExecPipe(errorRead, errorWrite));

			ScopedFileActions actions;
			if (actions.GetInitError() != 0)
				return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("posix_spawn_file_actions_init", actions.GetInitError()));
			// dup2 clears close-on-exec on the duplicate, so exactly these three descriptors survive the exec.
			int setupError = posix_spawn_file_actions_addopen(actions.Get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
			if (setupError == 0)
				setupError = posix_spawn_file_actions_adddup2(actions.Get(), outputWrite.Get(), STDOUT_FILENO);
			if (setupError == 0)
				setupError = posix_spawn_file_actions_adddup2(actions.Get(), errorWrite.Get(), STDERR_FILENO);
			if (setupError != 0)
				return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("posix_spawn_file_actions", setupError));

			ScopedSpawnAttributes attributes;
			if (attributes.GetInitError() != 0)
				return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("posix_spawnattr_init", attributes.GetInitError()));
			// The child starts with no blocked signals, whatever the spawning thread blocks.
			sigset_t noSignals{};
			sigemptyset(&noSignals);
	#if defined(ENGINE_PLATFORM_MACOS)
			const short flags = static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
	#elif defined(ENGINE_PLATFORM_LINUX)
			const short flags = static_cast<short>(POSIX_SPAWN_SETSIGMASK);
	#endif
			setupError = posix_spawnattr_setsigmask(attributes.Get(), &noSignals);
			if (setupError == 0)
				setupError = posix_spawnattr_setflags(attributes.Get(), flags);
			if (setupError != 0)
				return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("posix_spawnattr", setupError));

			pid_t pid = 0;
			const int spawnError = posix_spawn(&pid, executableText.c_str(), actions.Get(), attributes.Get(), argv.data(),
				Utils::GetEnvironment());
			if (spawnError != 0)
			{
				return MakeError(spawnError == ENOENT ? ErrorCode::NotFound : ErrorCode::Io, "cannot start '{}': {}", executableText,
					Utils::DescribeErrno("posix_spawn", spawnError));
			}
			// The child holds its own copies now; ours must close, or the pipes would never reach end of file.
			outputWrite.Reset();
			errorWrite.Reset();

			ChildProcessResult result;
			std::array<pollfd, 2> descriptors = {
				pollfd{ outputRead.Get(), POLLIN, 0 },
				pollfd{ errorRead.Get(), POLLIN, 0 },
			};
			std::array<std::string*, 2> outputs = { &result.StandardOutput, &result.StandardError };
			std::array<char, 4096> buffer{};
			int openCount = 2;
			while (openCount > 0)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					Utils::KillAndReap(pid);
					return MakeError(ErrorCode::Timeout, "'{}' ran longer than {} ms and was killed", executableText, timeout.count());
				}

				const int ready = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), Utils::MillisecondsUntil(deadline));
				if (ready < 0)
				{
					const int pollError = errno;
					if (pollError == EINTR)
						continue;
					Utils::KillAndReap(pid);
					return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("poll", pollError));
				}

				for (size_t index = 0; index < descriptors.size(); ++index)
				{
					pollfd& descriptor = descriptors[index];
					if (descriptor.fd < 0 || descriptor.revents == 0)
						continue;
					const ssize_t count = read(descriptor.fd, buffer.data(), buffer.size());
					if (count > 0)
					{
						outputs[index]->append(buffer.data(), static_cast<size_t>(count));
					}
					else if (count == 0 || (errno != EINTR && errno != EAGAIN))
					{
						descriptor.fd = -1; // end of file (or a broken pipe): poll ignores negative descriptors
						--openCount;
					}
				}
			}

			// Both pipes are closed, which normally means the child is exiting; it may still run without them, so the
			// deadline keeps applying.
			int status = 0;
			while (true)
			{
				const pid_t waited = waitpid(pid, &status, WNOHANG);
				if (waited == pid)
					break;
				if (waited < 0 && errno != EINTR)
				{
					const int waitError = errno;
					Utils::KillAndReap(pid);
					return MakeError(ErrorCode::Io, "{}", Utils::DescribeErrno("waitpid", waitError));
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					Utils::KillAndReap(pid);
					return MakeError(ErrorCode::Timeout, "'{}' ran longer than {} ms and was killed", executableText, timeout.count());
				}
				poll(nullptr, 0, std::min(Utils::MillisecondsUntil(deadline), 10));
			}

			result.ExitCode = Utils::DecodeWaitStatus(status);
			return result;
		}

		Result<std::filesystem::path> GetCurrentExecutablePath()
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

#endif

	}

}
