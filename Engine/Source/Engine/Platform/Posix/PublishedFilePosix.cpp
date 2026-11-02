#include "EnginePCH.h"
#include "Engine/Platform/PublishedFile.h"

// PublishedFile on Linux and macOS: no open conflicts with a rename or an unlink there, so Read is FileSystem::ReadFile
// and Replace is std::filesystem::rename (rename(2)), the calls the session files used before PublishedFile existed.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/FileSystem.h"

	#include <system_error>

namespace Engine {

	Result<Buffer> PublishedFile::Read(const std::filesystem::path& path)
	{
		return FileSystem::ReadFile(path);
	}

	Status PublishedFile::Replace(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		std::filesystem::rename(from, to, error);
		if (!error)
			return {};
		ErrorCode code = ErrorCode::Io;
		if (error == std::errc::no_such_file_or_directory)
			code = ErrorCode::NotFound;
		else if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
			code = ErrorCode::PermissionDenied;
		return MakeError(code, "cannot rename '{}' to '{}': {}", FileSystem::PathToUtf8(from), FileSystem::PathToUtf8(to), error.message());
	}

}

#endif
