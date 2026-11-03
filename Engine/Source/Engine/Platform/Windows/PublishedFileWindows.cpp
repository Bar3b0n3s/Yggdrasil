#include "EnginePCH.h"
#include "Engine/Platform/PublishedFile.h"

// PublishedFile on Windows. Read opens with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, so neither the
// DELETE-access handle of a rename in flight nor a later rename or removal conflicts with it. Replace opens the source
// with DELETE access and renames it with SetFileInformationByHandle(FileRenameInfoEx) and the flags
// FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS, which replace a target that readers hold open
// with FILE_SHARE_DELETE; a volume or Windows release without POSIX rename semantics refuses the call as an invalid or
// unsupported request, and the rename then falls back to MoveFileExW(MOVEFILE_REPLACE_EXISTING).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/FileSystem.h"

	#include <windows.h>

	#include <cstddef>
	#include <cstring>
	#include <format>
	#include <limits>
	#include <memory>
	#include <string>
	#include <string_view>
	#include <system_error>
	#include <utility>

namespace Engine {

	namespace {

		// Closes a file handle on destruction.
		class ScopedFile
		{
		public:
			explicit ScopedFile(HANDLE handle)
				: m_Handle(handle)
			{
			}

			~ScopedFile()
			{
				Close();
			}

			ScopedFile(const ScopedFile&) = delete;
			ScopedFile& operator=(const ScopedFile&) = delete;
			ScopedFile(ScopedFile&&) = delete;
			ScopedFile& operator=(ScopedFile&&) = delete;

			[[nodiscard]] HANDLE Get() const { return m_Handle; }

			void Close()
			{
				if (m_Handle != INVALID_HANDLE_VALUE)
					CloseHandle(std::exchange(m_Handle, INVALID_HANDLE_VALUE));
			}
		private:
			HANDLE m_Handle = INVALID_HANDLE_VALUE;
		};

	}

	namespace Utils {

		constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		constexpr DWORD ReadChunkBytes = 64 * 1024;

		// The ErrorCode FileSystem gives the same failure.
		static ErrorCode ErrorCodeFromWindows(DWORD code)
		{
			switch (code)
			{
				case ERROR_FILE_NOT_FOUND:
				case ERROR_PATH_NOT_FOUND:    return ErrorCode::NotFound;
				case ERROR_ACCESS_DENIED:
				case ERROR_SHARING_VIOLATION:
				case ERROR_LOCK_VIOLATION:    return ErrorCode::PermissionDenied;
				default:                      return ErrorCode::Io;
			}
		}

		[[nodiscard]] static std::unexpected<Error> MakeWindowsError(DWORD code, std::string_view action, const std::filesystem::path& path)
		{
			return MakeError(ErrorCodeFromWindows(code), "cannot {} '{}': {}", action, FileSystem::PathToUtf8(path),
				std::system_category().message(static_cast<int>(code)));
		}

		// Whether a POSIX-semantics rename was refused only because the volume or the Windows release does not offer it.
		static bool IsPosixRenameUnavailable(DWORD code)
		{
			return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION;
		}

	}

	Result<Buffer> PublishedFile::Read(const std::filesystem::path& path)
	{
		std::error_code error;
		if (std::filesystem::is_directory(path, error))
			return MakeError(ErrorCode::Io, "cannot read '{}': it is a directory", FileSystem::PathToUtf8(path));

		const ScopedFile file(
			CreateFileW(path.c_str(), GENERIC_READ, Utils::ShareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
		{
			const DWORD code = GetLastError();
			return Utils::MakeWindowsError(code, "read", path);
		}

		Buffer buffer;
		while (true)
		{
			const size_t offset = buffer.size();
			buffer.resize(offset + Utils::ReadChunkBytes);
			DWORD read = 0;
			if (::ReadFile(file.Get(), buffer.data() + offset, Utils::ReadChunkBytes, &read, nullptr) == FALSE)
			{
				const DWORD code = GetLastError();
				return Utils::MakeWindowsError(code, "read", path);
			}
			buffer.resize(offset + read);
			if (read == 0)
				break;
		}
		return buffer;
	}

	Status PublishedFile::Replace(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		// FILE_RENAME_INFO ends in the name (FileName[1] declares its first character), so the information is the
		// structure followed by the rest of the name and a terminator.
		const std::wstring& target = to.native();
		const size_t nameBytes = target.size() * sizeof(wchar_t);
		const size_t informationBytes = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
		if (informationBytes > std::numeric_limits<DWORD>::max())
		{
			return MakeError(ErrorCode::Io, "cannot rename '{}' to '{}': the name is too long", FileSystem::PathToUtf8(from),
				FileSystem::PathToUtf8(to));
		}

		ScopedFile file(
			CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, Utils::ShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (file.Get() == INVALID_HANDLE_VALUE)
		{
			const DWORD code = GetLastError();
			return Utils::MakeWindowsError(code, "rename", from);
		}

		// The byte array's storage, aligned for any fundamental type, implicitly creates the structure; it starts zeroed,
		// so the name is terminated.
		const std::unique_ptr<std::byte[]> storage = std::make_unique<std::byte[]>(informationBytes);
		auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
		information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
		information->RootDirectory = nullptr;
		information->FileNameLength = static_cast<DWORD>(nameBytes);
		std::memcpy(storage.get() + offsetof(FILE_RENAME_INFO, FileName), target.c_str(), nameBytes);
		if (SetFileInformationByHandle(file.Get(), FileRenameInfoEx, information, static_cast<DWORD>(informationBytes)) != FALSE)
			return {};
		const DWORD code = GetLastError();
		if (!Utils::IsPosixRenameUnavailable(code))
			return Utils::MakeWindowsError(code, std::format("rename '{}' to", FileSystem::PathToUtf8(from)), to);

		file.Close();
		if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE)
			return {};
		const DWORD moveCode = GetLastError();
		return Utils::MakeWindowsError(moveCode, std::format("rename '{}' to", FileSystem::PathToUtf8(from)), to);
	}

}

#endif
