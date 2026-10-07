#include "EnginePCH.h"
#include "Engine/Platform/Process.h"

// Child processes and current-process queries on Windows. A child starts through CreateProcessW with a command line
// quoted for the C runtime's parser, standard input on NUL and standard output and standard error on anonymous pipes,
// each drained by its own reader thread. The child inherits exactly those three handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Core/Utf8.h"
	#include "Engine/Platform/Private/PathsUtf8.h"
	#include "Engine/Platform/Private/ProcessOutput.h"

	#include <windows.h>

	#include <cwchar>
	#include <system_error>
	#include <thread>

namespace Engine {

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
				if (IsValid())
					CloseHandle(m_Handle);
				m_Handle = handle;
			}

			[[nodiscard]] HANDLE Release()
			{
				return std::exchange(m_Handle, nullptr);
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

		// Frees the block GetEnvironmentStringsW returned.
		class ScopedEnvironmentStrings
		{
		public:
			ScopedEnvironmentStrings()
				: m_Block(GetEnvironmentStringsW())
			{
			}

			~ScopedEnvironmentStrings()
			{
				if (m_Block != nullptr)
					FreeEnvironmentStringsW(m_Block);
			}

			ScopedEnvironmentStrings(const ScopedEnvironmentStrings&) = delete;
			ScopedEnvironmentStrings& operator=(const ScopedEnvironmentStrings&) = delete;
			ScopedEnvironmentStrings(ScopedEnvironmentStrings&&) = delete;
			ScopedEnvironmentStrings& operator=(ScopedEnvironmentStrings&&) = delete;

			[[nodiscard]] const wchar_t* Get() const { return m_Block; }
		private:
			LPWCH m_Block = nullptr;
		};

	}

	namespace Utils {

		// The exit code TerminateProcess gives a killed child: 128 + 9, what a SIGKILL reports on POSIX, so a killed child
		// reads the same on every host.
		constexpr UINT KilledExitCode = 137;

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

		// Whether `executable` names an existing file, and why not.
		static Status CheckExecutableExists(const std::filesystem::path& executable)
		{
			if (executable.empty())
				return MakeError(ErrorCode::NotFound, "no executable was given");
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(executable, error);
			if (status.type() == std::filesystem::file_type::not_found)
				return MakeError(ErrorCode::NotFound, "the executable '{}' does not exist", NativePathToUtf8(executable));
			if (error)
				return MakeError(ErrorCode::Io, "cannot inspect the executable '{}': {}", NativePathToUtf8(executable), error.message());
			if (std::filesystem::is_directory(status))
				return MakeError(ErrorCode::NotFound, "the executable '{}' is a directory", NativePathToUtf8(executable));
			return {};
		}

		// UTF-8 text from the specification as UTF-16: InvalidArgument unless it is valid UTF-8 without NUL characters,
		// which no command line or environment block can carry.
		static Result<std::wstring> WideFromSpecificationText(std::string_view text, std::string_view what)
		{
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::InvalidArgument, "the {} is not valid UTF-8", what);
			if (text.find('\0') != std::string_view::npos)
				return MakeError(ErrorCode::InvalidArgument, "the {} contains a NUL character", what);
			return NativeStringFromUtf8(text);
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

		// argv[0] is the executable, quoted without escapes (Windows paths cannot contain quotes), then every argument.
		static Result<std::wstring> BuildCommandLine(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
		{
			std::wstring commandLine = L"\"" + executable.native() + L"\"";
			for (size_t index = 0; index < arguments.size(); ++index)
			{
				const std::string what = std::format("argument {}", index + 1);
				ENGINE_TRY_ASSIGN(const std::wstring wide, WideFromSpecificationText(arguments[index], what));
				commandLine += L' ';
				AppendQuotedArgument(commandLine, wide);
			}
			return commandLine;
		}

		// True when the variable names `left` and `right` are the same variable: Windows compares them case-insensitively.
		static bool IsSameVariableName(std::wstring_view left, std::wstring_view right)
		{
			return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE)
				== CSTR_EQUAL;
		}

		static bool IsVariableNameLess(std::wstring_view left, std::wstring_view right)
		{
			return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE)
				== CSTR_LESS_THAN;
		}

		// The name of one "NAME=value" entry. Hidden per-drive entries start with '=' ("=C:=C:\dir"), so the separator is
		// searched from the second character.
		static std::wstring_view GetVariableName(std::wstring_view entry)
		{
			const size_t separator = entry.find(L'=', 1);
			return separator == std::wstring_view::npos ? entry : entry.substr(0, separator);
		}

		// The child's environment block: the parent's variables, minus those the specification sets, plus the
		// specification's entries (the last one of a name wins), sorted by name as Windows expects. Empty when the
		// specification sets nothing, so the child inherits the parent's block unchanged.
		static Result<std::wstring> BuildEnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides)
		{
			if (overrides.empty())
				return std::wstring();

			std::vector<std::wstring> entries;
			std::vector<std::wstring> names;
			for (const auto& [name, value] : overrides)
			{
				if (name.empty())
					return MakeError(ErrorCode::InvalidArgument, "an environment variable has an empty name");
				if (name.find('=') != std::string::npos)
					return MakeError(ErrorCode::InvalidArgument, "the environment variable name '{}' contains '='", name);
				ENGINE_TRY_ASSIGN(std::wstring wideName, WideFromSpecificationText(name, "environment variable name"));
				ENGINE_TRY_ASSIGN(const std::wstring wideValue,
					WideFromSpecificationText(value, std::format("value of the environment variable '{}'", name)));

				// A later entry for the same name replaces an earlier one.
				for (size_t index = 0; index < names.size(); ++index)
				{
					if (IsSameVariableName(names[index], wideName))
					{
						names.erase(names.begin() + static_cast<ptrdiff_t>(index));
						entries.erase(entries.begin() + static_cast<ptrdiff_t>(index));
						break;
					}
				}
				entries.push_back(wideName + L"=" + wideValue);
				names.push_back(std::move(wideName));
			}

			const ScopedEnvironmentStrings parent;
			if (parent.Get() == nullptr)
				return MakeLastError("GetEnvironmentStringsW");
			for (const wchar_t* entry = parent.Get(); *entry != L'\0'; entry += std::wcslen(entry) + 1)
			{
				const std::wstring_view text(entry);
				const std::wstring_view name = GetVariableName(text);
				const bool isOverridden = std::ranges::any_of(names, [name](const std::wstring& overridden)
				{
					return IsSameVariableName(name, overridden);
				});
				if (!isOverridden)
					entries.emplace_back(text);
			}

			std::ranges::stable_sort(entries, [](const std::wstring& left, const std::wstring& right)
			{
				return IsVariableNameLess(GetVariableName(left), GetVariableName(right));
			});

			std::wstring block;
			for (const std::wstring& entry : entries)
			{
				block += entry;
				block += L'\0';
			}
			block += L'\0';
			return block;
		}

		static Status CreateOutputPipe(ScopedHandle& readEnd, ScopedHandle& writeEnd)
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

	struct Process::Impl
	{
		ScopedHandle ProcessHandle;
		uint32_t Id = 0;
		std::string ExecutableText{}; // for messages
		ScopedHandle OutputRead;
		ScopedHandle ErrorRead;
		Utils::ProcessOutput Output;
		std::atomic<bool> IsStopping{ false };
		std::thread OutputReader;
		std::thread ErrorReader;

		Impl() = default;

		~Impl()
		{
			// No child outlives its Process; a child that already exited is left alone.
			if (ProcessHandle.IsValid() && WaitForSingleObject(ProcessHandle.Get(), 0) == WAIT_TIMEOUT)
			{
				TerminateProcess(ProcessHandle.Get(), Utils::KilledExitCode);
				WaitForSingleObject(ProcessHandle.Get(), INFINITE);
			}
			StopReader(OutputReader, ProcessStream::StandardOutput);
			StopReader(ErrorReader, ProcessStream::StandardError);
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;
		Impl(Impl&&) = delete;
		Impl& operator=(Impl&&) = delete;

		// Reads `pipe` until the writer closes it (or the read fails or is cancelled), appending everything to the output.
		void ReadPipe(HANDLE pipe, ProcessStream stream)
		{
			std::array<char, 4096> buffer{};
			while (!IsStopping.load())
			{
				DWORD bytesRead = 0;
				if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
					break; // ERROR_BROKEN_PIPE once every writer closed its end; ERROR_OPERATION_ABORTED when stopped
				Output.Append(stream, std::string_view(buffer.data(), bytesRead));
			}
			Output.MarkEnded(stream);
		}

		// The child is gone, so the pipes normally reach end of file by themselves; one may still be held open by a process
		// the child started. Cancelling the blocked read ends the thread either way. A cancel that arrives before the
		// thread enters ReadFile finds nothing to cancel, so it is repeated until the thread reports the end.
		void StopReader(std::thread& reader, ProcessStream stream)
		{
			if (!reader.joinable())
				return;
			IsStopping.store(true);
			while (!Output.WaitUntilEnded(stream, Utils::MakeProcessDeadline(std::chrono::milliseconds(20))))
				CancelSynchronousIo(reader.native_handle());
			reader.join();
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
		std::error_code error;
		const std::filesystem::path executable = std::filesystem::absolute(specification.Executable, error);
		if (error)
		{
			return MakeError(ErrorCode::Io, "cannot make the executable path '{}' absolute: {}",
				Utils::NativePathToUtf8(specification.Executable), error.message());
		}
		const std::string executableText = Utils::NativePathToUtf8(executable);

		ENGINE_TRY_ASSIGN(std::wstring commandLine, Utils::BuildCommandLine(executable, specification.Arguments));
		ENGINE_TRY_ASSIGN(std::wstring environment, Utils::BuildEnvironmentBlock(specification.Environment));

		ScopedHandle outputRead;
		ScopedHandle outputWrite;
		ScopedHandle errorRead;
		ScopedHandle errorWrite;
		ENGINE_TRY(Utils::CreateOutputPipe(outputRead, outputWrite));
		ENGINE_TRY(Utils::CreateOutputPipe(errorRead, errorWrite));

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
		const auto attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
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
		const wchar_t* workingDirectory = specification.WorkingDirectory.empty() ? nullptr : specification.WorkingDirectory.c_str();
		PROCESS_INFORMATION information{};
		if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CreationFlags,
				environment.empty() ? nullptr : environment.data(), workingDirectory, &startup.StartupInfo, &information))
		{
			const std::string reason = Utils::DescribeLastError("CreateProcessW");
			const std::string where = workingDirectory == nullptr
				? std::string()
				: std::format(" in '{}'", Utils::NativePathToUtf8(specification.WorkingDirectory));
			return MakeError(ErrorCode::Io, "cannot start '{}'{}: {}", executableText, where, reason);
		}
		CloseHandle(information.hThread);

		// The child holds its own copies now; ours must close, or the pipes would never reach end of file.
		outputWrite.Reset();
		errorWrite.Reset();
		input.Reset();

		Scope<Impl> impl = CreateScope<Impl>();
		impl->ProcessHandle.Reset(information.hProcess);
		impl->Id = information.dwProcessId;
		impl->ExecutableText = executableText;
		impl->OutputRead.Reset(outputRead.Release());
		impl->ErrorRead.Reset(errorRead.Release());
		Impl* state = impl.get();
		impl->OutputReader = std::thread([state]()
		{
			state->ReadPipe(state->OutputRead.Get(), ProcessStream::StandardOutput);
		});
		impl->ErrorReader = std::thread([state]()
		{
			state->ReadPipe(state->ErrorRead.Get(), ProcessStream::StandardError);
		});
		return Process(std::move(impl));
	}

	uint32_t Process::GetId() const
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "GetId called on a moved-from Process");
		return m_Impl->Id;
	}

	bool Process::HasExited()
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "HasExited called on a moved-from Process");
		return WaitForSingleObject(m_Impl->ProcessHandle.Get(), 0) == WAIT_OBJECT_0;
	}

	Result<ProcessResult> Process::Wait(std::chrono::milliseconds timeout)
	{
		ENGINE_CORE_ASSERT(m_Impl != nullptr, "Wait called on a moved-from Process");
		Impl& impl = *m_Impl;
		if (impl.Output.IsTaken())
			return MakeError(ErrorCode::InvalidState, "the process '{}' was already waited for", impl.ExecutableText);

		const Utils::ProcessClock::time_point deadline = Utils::MakeProcessDeadline(timeout);
		const auto milliseconds = static_cast<DWORD>(Utils::GetMillisecondsUntil(deadline, INFINITE - 1));
		const DWORD waited = WaitForSingleObject(impl.ProcessHandle.Get(), milliseconds);
		if (waited == WAIT_FAILED)
			return Utils::MakeLastError("WaitForSingleObject");
		if (waited != WAIT_OBJECT_0)
			return MakeError(ErrorCode::Timeout, "'{}' is still running after {} ms", impl.ExecutableText, timeout.count());
		if (!impl.Output.WaitUntilFinished(deadline, false))
		{
			return MakeError(ErrorCode::Timeout, "'{}' exited, but a process it started kept its output open for {} ms", impl.ExecutableText,
				timeout.count());
		}

		DWORD exitCode = 0;
		if (!GetExitCodeProcess(impl.ProcessHandle.Get(), &exitCode))
			return Utils::MakeLastError("GetExitCodeProcess");

		std::array<std::string, 2> text = impl.Output.TakeText();
		ProcessResult result;
		result.ExitCode = static_cast<int>(exitCode);
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
		const HANDLE process = m_Impl->ProcessHandle.Get();
		if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
			return {};
		// TerminateProcess fails with access denied once the child is exiting on its own; the wait below settles it.
		if (!TerminateProcess(process, Utils::KilledExitCode) && WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
			return MakeError(ErrorCode::Io, "cannot kill '{}': {}", m_Impl->ExecutableText, Utils::DescribeLastError("TerminateProcess"));
		if (WaitForSingleObject(process, INFINITE) == WAIT_FAILED)
			return Utils::MakeLastError("WaitForSingleObject");
		return {};
	}

	uint32_t Process::GetCurrentId()
	{
		return GetCurrentProcessId();
	}

	bool Process::IsRunning(uint32_t processId)
	{
		if (processId == 0)
			return false;
		if (processId == GetCurrentProcessId())
			return true;
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
		if (process == nullptr)
			return GetLastError() == ERROR_ACCESS_DENIED; // it exists, but belongs to someone this process may not query
		// A handle someone still holds keeps an exited process's ID valid; its handle is signaled then. The exit code cannot
		// tell (a process may exit with STILL_ACTIVE).
		const DWORD state = WaitForSingleObject(process, 0);
		CloseHandle(process);
		return state == WAIT_TIMEOUT;
	}

	Result<std::filesystem::path> Process::GetCurrentExecutablePath()
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
			return MakeError(ErrorCode::Io, "cannot resolve the running executable '{}': {}", Utils::NativePathToUtf8(reported),
				error.message());
		}
		return resolved;
	}

	Result<double> Process::GetCurrentCpuSeconds()
	{
		FILETIME creation{};
		FILETIME exit{};
		FILETIME kernel{};
		FILETIME user{};
		if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
			return Utils::MakeLastError("GetProcessTimes");

		// FILETIME durations count 100-nanosecond intervals.
		const auto toTicks = [](const FILETIME& time)
		{
			return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		};
		return static_cast<double>(toTicks(kernel) + toTicks(user)) / 1.0e7;
	}

	bool Process::IsDebuggerAttached()
	{
		return IsDebuggerPresent() != FALSE;
	}

	void Process::BreakIntoDebugger()
	{
		__debugbreak();
	}

}

#endif
