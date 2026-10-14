#include "TestsPCH.h"
#include "Support/PlatformProbes.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
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

#include <algorithm>
#include <array>
#include <cerrno>
#include <optional>
#include <span>
#include <string_view>
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

		Result<uint32_t> GetThreadStackGuarantee()
		{
			ULONG size = 0; // 0 asks for the current value
			if (SetThreadStackGuarantee(&size) == FALSE)
				return MakeError(ErrorCode::Io, "cannot query the thread's stack guarantee: {}", Utils::DescribeLastError());
			return size;
		}

		Result<uint32_t> GetNewThreadStackGuarantee(size_t stackBytes)
		{
			Result<uint32_t> guarantee = 0u;
			const HANDLE thread = CreateThread(nullptr, stackBytes, [](LPVOID parameter) -> DWORD
			{
				*static_cast<Result<uint32_t>*>(parameter) = GetThreadStackGuarantee();
				return 0;
			}, &guarantee, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
			if (thread == nullptr)
				return MakeError(ErrorCode::Io, "cannot start a thread: {}", Utils::DescribeLastError());
			// The thread writes `guarantee` until it ends, so this frame must not end first. A wait on the handle of a thread
			// that was just started fails only when memory is corrupt.
			const DWORD wait = WaitForSingleObject(thread, INFINITE);
			ENGINE_CORE_VERIFY(wait == WAIT_OBJECT_0, "Waiting for a thread failed: {}", Utils::DescribeLastError());
			CloseHandle(thread);
			return guarantee;
		}

		void PageReleaser::operator()(char* pages) const
		{
			if (VirtualFree(pages, 0, MEM_RELEASE) == FALSE)
				ENGINE_CORE_ERROR("Releasing test pages failed: {}", Utils::DescribeLastError());
		}

		Result<UnreadableText> AllocateTextEndingInUnreadableBytes(size_t readableBytes, size_t unreadableBytes)
		{
			SYSTEM_INFO system{};
			GetSystemInfo(&system);
			const size_t pageSize = system.dwPageSize;
			if (unreadableBytes == 0 || unreadableBytes > pageSize)
				return MakeError(ErrorCode::InvalidArgument, "{} unreadable bytes do not fit in one page of {} bytes", unreadableBytes, pageSize);

			const size_t readablePages = (readableBytes + pageSize - 1) / pageSize;
			const size_t size = (readablePages + 1) * pageSize;
			UnreadableText text;
			text.Pages.reset(static_cast<char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)));
			if (text.Pages == nullptr)
				return MakeError(ErrorCode::Io, "cannot allocate {} bytes: {}", size, Utils::DescribeLastError());
			std::fill_n(text.Pages.get(), size, 'x');
			char* const unreadable = text.Pages.get() + readablePages * pageSize;
			DWORD previousProtection = 0;
			if (VirtualProtect(unreadable, pageSize, PAGE_NOACCESS, &previousProtection) == FALSE)
				return MakeError(ErrorCode::Io, "cannot make a page inaccessible: {}", Utils::DescribeLastError());
			text.Text = std::string_view(unreadable - readableBytes, readableBytes + unreadableBytes);
			return text;
		}

		Result<ExecutableResourceProbe> ProbeExecutableResources(const std::filesystem::path& executable)
		{
			ExecutableResourceProbe probe;
			const auto toUtf8 = [](std::wstring_view text)
			{
				if (text.empty())
					return std::string();
				const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
				std::string result(static_cast<size_t>(std::max(size, 0)), '\0');
				if (size > 0)
					WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
				return result;
			};

			// The version resource, as Explorer's Details tab reads it.
			DWORD ignored = 0;
			const DWORD versionSize = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
			if (versionSize == 0)
			{
				const DWORD error = GetLastError();
				if (error != ERROR_RESOURCE_TYPE_NOT_FOUND && error != ERROR_RESOURCE_DATA_NOT_FOUND && error != ERROR_RESOURCE_NAME_NOT_FOUND)
				{
					return MakeError(ErrorCode::Io, "GetFileVersionInfoSizeW failed for '{}': {}", PathToUtf8(executable),
						std::system_category().message(static_cast<int>(error)));
				}
			}
			else
			{
				std::vector<std::byte> data(versionSize);
				if (GetFileVersionInfoW(executable.c_str(), 0, versionSize, data.data()) == FALSE)
					return MakeError(ErrorCode::Io, "GetFileVersionInfoW failed for '{}': {}", PathToUtf8(executable), Utils::DescribeLastError());
				void* fixed = nullptr;
				UINT fixedLength = 0;
				if (VerQueryValueW(data.data(), L"\\", &fixed, &fixedLength) != FALSE && fixedLength >= sizeof(VS_FIXEDFILEINFO))
				{
					const auto* info = static_cast<const VS_FIXEDFILEINFO*>(fixed);
					probe.FileVersion = { HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
						LOWORD(info->dwFileVersionLS) };
					probe.ProductVersion = { HIWORD(info->dwProductVersionMS), LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS),
						LOWORD(info->dwProductVersionLS) };
				}
				constexpr std::array<std::pair<std::string_view, std::wstring_view>, 7> Keys = { {
					{ "CompanyName", L"CompanyName" },
					{ "FileDescription", L"FileDescription" },
					{ "FileVersion", L"FileVersion" },
					{ "InternalName", L"InternalName" },
					{ "OriginalFilename", L"OriginalFilename" },
					{ "ProductName", L"ProductName" },
					{ "ProductVersion", L"ProductVersion" },
				} };
				for (const auto& [key, wideKey] : Keys)
				{
					const std::wstring query = L"\\StringFileInfo\\040904B0\\" + std::wstring(wideKey);
					void* value = nullptr;
					UINT length = 0;
					if (VerQueryValueW(data.data(), query.c_str(), &value, &length) == FALSE || value == nullptr)
						continue;
					std::wstring_view text(static_cast<const wchar_t*>(value), length);
					text = text.substr(0, text.find(L'\0'));
					probe.Strings[std::string(key)] = toUtf8(text);
				}
			}

			// The icon group and the manifest, through the resource loader.
			const HMODULE module = LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (module == nullptr)
				return MakeError(ErrorCode::Io, "LoadLibraryExW failed for '{}': {}", PathToUtf8(executable), Utils::DescribeLastError());
			const auto readResource = [module](WORD type, WORD id) -> std::optional<std::span<const std::byte>>
			{
				const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(type));
				if (found == nullptr)
					return std::nullopt;
				const HGLOBAL loaded = LoadResource(module, found);
				const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
				if (bytes == nullptr)
					return std::nullopt;
				return std::span<const std::byte>(static_cast<const std::byte*>(bytes), SizeofResource(module, found));
			};
			constexpr WORD IconType = 3;
			constexpr WORD IconGroupType = 14;
			constexpr WORD ManifestType = 24;
			Status icons;
			if (const std::optional<std::span<const std::byte>> group = readResource(IconGroupType, 1))
			{
				const auto readU16 = [&group](size_t offset)
				{
					return static_cast<uint16_t>(std::to_integer<uint16_t>((*group)[offset]) | (std::to_integer<uint16_t>((*group)[offset + 1]) << 8));
				};
				const size_t count = group->size() >= 6 ? readU16(4) : 0;
				if (group->size() < 6 + count * 14)
					icons = MakeError(ErrorCode::Io, "the icon group of '{}' is truncated ({} bytes for {} images)", PathToUtf8(executable), group->size(), count);
				for (size_t index = 0; icons.has_value() && index < count; ++index)
				{
					const size_t entry = 6 + index * 14;
					const uint8_t side = std::to_integer<uint8_t>((*group)[entry]);
					const uint16_t id = readU16(entry + 12);
					const std::optional<std::span<const std::byte>> image = readResource(IconType, id);
					if (!image.has_value())
					{
						icons = MakeError(ErrorCode::Io, "the icon group of '{}' names RT_ICON {}, which FindResourceW does not find", PathToUtf8(executable), id);
						break;
					}
					probe.Icons.emplace_back(side == 0 ? uint16_t{ 256 } : uint16_t{ side }, Buffer(image->begin(), image->end()));
				}
			}
			probe.HasManifest = readResource(ManifestType, 1).has_value();
			FreeLibrary(module);
			ENGINE_TRY(icons);
			return probe;
		}

		Status PostKeyToProcessWindow(uint32_t processId, Key key, bool pressed)
		{
			if (key < Key::F1 || key > Key::F12)
				return MakeError(ErrorCode::InvalidArgument, "PostKeyToProcessWindow posts F1 to F12 only, not key {}", static_cast<int>(key));
			// The process's visible top-level window of GLFW's window class ("GLFW30", every GLFW 3 version).
			struct Search
			{
				DWORD ProcessId = 0;
				HWND Window = nullptr;
			};
			Search search{ .ProcessId = processId, .Window = nullptr };
			// EnumWindows also returns FALSE when the callback stops it, which it does once it found the window.
			const BOOL completed = EnumWindows(
				[](HWND window, LPARAM parameter) -> BOOL
			{
				Search& found = *reinterpret_cast<Search*>(parameter);
				DWORD owner = 0;
				GetWindowThreadProcessId(window, &owner);
				std::array<wchar_t, 16> className{};
				const int length = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
				if (owner != found.ProcessId || IsWindowVisible(window) == FALSE || std::wstring_view(className.data(), static_cast<size_t>(std::max(length, 0))) != L"GLFW30")
					return TRUE;
				found.Window = window;
				return FALSE;
			},
				reinterpret_cast<LPARAM>(&search));
			if (search.Window == nullptr && completed == FALSE)
				return MakeError(ErrorCode::Io, "cannot enumerate the top-level windows: {}", Utils::DescribeLastError());
			if (search.Window == nullptr)
				return MakeError(ErrorCode::NotFound, "process {} has no visible GLFW window", processId);

			// The keystroke message's parameters: the virtual-key code, and in lParam a repeat count of 1 and the scan code
			// (bits 16 to 23), with the previous-state and transition bits (30 and 31) set for a release.
			const UINT virtualKey = VK_F1 + static_cast<UINT>(static_cast<int>(key) - static_cast<int>(Key::F1));
			const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
			if (scanCode == 0)
				return MakeError(ErrorCode::Io, "the keyboard layout has no scan code for virtual key {}", virtualKey);
			const uint32_t transition = pressed ? 0u : ((1u << 30) | (1u << 31));
			const LPARAM parameter = static_cast<LPARAM>(1u | ((scanCode & 0xFFu) << 16) | transition);
			if (PostMessageW(search.Window, pressed ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(virtualKey), parameter) == FALSE)
				return MakeError(ErrorCode::Io, "cannot post the keystroke to process {}: {}", processId, Utils::DescribeLastError());
			return {};
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
