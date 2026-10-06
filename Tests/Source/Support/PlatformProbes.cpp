#include "TestsPCH.h"
#include "Support/PlatformProbes.h"

#include "Support/Utf8Path.h"

#if defined(ENGINE_PLATFORM_WINDOWS)
	#include <windows.h>
#elif defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
	#include <fcntl.h>
	#include <spawn.h>
	#include <unistd.h>
	#if defined(ENGINE_PLATFORM_MACOS)
		#include <crt_externs.h>
	#endif
#endif

#include <cerrno>
#include <system_error>
#include <vector>

namespace Engine {

	namespace Test {

		namespace Utils {

			// The same rule on every host: no argument would need escaping on a Windows command line.
			static Status CheckArguments(std::span<const std::string> arguments)
			{
				for (const std::string& argument : arguments)
				{
					if (argument.find('"') != std::string::npos || argument.ends_with('\\'))
						return MakeError(ErrorCode::InvalidArgument, "the argument '{}' would need escaping", argument);
				}
				return {};
			}

#if defined(ENGINE_PLATFORM_WINDOWS)
			// The text of GetLastError, read first.
			static std::string DescribeLastError()
			{
				const DWORD error = GetLastError();
				return std::system_category().message(static_cast<int>(error));
			}
#endif

		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		Status StartProcessInheritingOutput(const std::filesystem::path& executable, std::span<const std::string> arguments)
		{
			ENGINE_TRY(Utils::CheckArguments(arguments));
			std::wstring commandLine = L"\"" + executable.native() + L"\"";
			for (const std::string& argument : arguments)
				commandLine += L" \"" + PathFromUtf8(argument).native() + L"\"";

			// Inheritance passes on every inheritable handle; the standard ones are made inheritable first.
			const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
			const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
			if (SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == FALSE
				|| SetHandleInformation(error, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == FALSE)
			{
				return MakeError(ErrorCode::Io, "cannot make the standard handles inheritable: {}", Utils::DescribeLastError());
			}
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdOutput = output;
			startup.hStdError = error;
			PROCESS_INFORMATION information{};
			if (CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
					&information)
				== FALSE)
			{
				return MakeError(ErrorCode::Io, "cannot start '{}': {}", PathToUtf8(executable), Utils::DescribeLastError());
			}
			CloseHandle(information.hThread);
			CloseHandle(information.hProcess);
			return {};
		}

		Status LockProcessHeap()
		{
			if (HeapLock(GetProcessHeap()) == FALSE)
				return MakeError(ErrorCode::Io, "cannot lock the process heap: {}", Utils::DescribeLastError());
			return {};
		}

		Result<std::string> ReadFileDenyingWriters(const std::filesystem::path& file)
		{
			const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (handle == INVALID_HANDLE_VALUE)
				return MakeError(ErrorCode::Io, "cannot open '{}' denying writers: {}", PathToUtf8(file), Utils::DescribeLastError());

			std::string text;
			std::array<char, 4096> buffer{};
			while (true)
			{
				DWORD read = 0;
				if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE)
				{
					const std::string description = Utils::DescribeLastError();
					CloseHandle(handle);
					return MakeError(ErrorCode::Io, "cannot read '{}': {}", PathToUtf8(file), description);
				}
				if (read == 0)
					break;
				text.append(buffer.data(), read);
			}
			CloseHandle(handle);
			return text;
		}
#elif defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
		Status StartProcessInheritingOutput(const std::filesystem::path& executable, std::span<const std::string> arguments)
		{
			ENGINE_TRY(Utils::CheckArguments(arguments));
			std::string executableText = executable.native();
			std::vector<std::string> storage(arguments.begin(), arguments.end());
			std::vector<char*> argv;
			argv.reserve(storage.size() + 2);
			argv.push_back(executableText.data());
			for (std::string& argument : storage)
				argv.push_back(argument.data());
			argv.push_back(nullptr);
	#if defined(ENGINE_PLATFORM_MACOS)
			char** environment = *_NSGetEnviron();
	#else
			char** environment = environ;
	#endif

			// No file actions: the new process keeps descriptors 0 to 2 and every other one without close-on-exec.
			pid_t pid = 0;
			const int error = posix_spawn(&pid, executableText.c_str(), nullptr, nullptr, argv.data(), environment);
			if (error != 0)
				return MakeError(ErrorCode::Io, "cannot start '{}': {}", executableText, std::generic_category().message(error));
			return {};
		}

		Result<int> OpenInheritableDescriptor(const std::filesystem::path& file, int minimum)
		{
			const int opened = open(file.c_str(), O_RDONLY);
			if (opened < 0)
			{
				const int openError = errno;
				return MakeError(ErrorCode::Io, "cannot open '{}': {}", file.native(), std::generic_category().message(openError));
			}
			// F_DUPFD leaves close-on-exec off on the duplicate.
			const int moved = fcntl(opened, F_DUPFD, minimum);
			const int moveError = errno;
			close(opened);
			if (moved < 0)
			{
				return MakeError(ErrorCode::Io, "cannot move descriptor {} to {} or above: {}", opened, minimum,
					std::generic_category().message(moveError));
			}
			return moved;
		}

		bool IsDescriptorOpen(int descriptor)
		{
			return fcntl(descriptor, F_GETFD) != -1 || errno != EBADF;
		}

		void CloseDescriptor(int descriptor)
		{
			close(descriptor);
		}
#endif

	}

}
