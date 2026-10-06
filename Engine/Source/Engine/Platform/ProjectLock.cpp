#include "EnginePCH.h"
#include "Engine/Platform/ProjectLock.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Private/PathsUtf8.h"

// The host-independent part of ProjectLock: reading the pid text, which needs no OS lock API. Acquire, IsHeld and the
// lock object live in Platform/Windows/ProjectLockWindows.cpp and Platform/Posix/ProjectLockPosix.cpp.

namespace Engine {

	Result<uint32_t> ProjectLock::ReadHolderPid(const std::filesystem::path& lockFile)
	{
		ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(lockFile));
		const std::string_view text = AsStringView(bytes);

		// The format is exact: 1 to 10 decimal digits, one LF, nothing else.
		constexpr size_t MaxDigits = std::numeric_limits<uint32_t>::digits10 + 1;
		const auto describe = [&lockFile]()
		{
			return Utils::NativePathToUtf8(lockFile);
		};
		if (text.size() < 2 || text.size() > MaxDigits + 1 || text.back() != '\n')
			return MakeError(ErrorCode::Parse, "the lock file '{}' does not hold a process ID followed by a line feed", describe());

		uint64_t pid = 0;
		for (const char character : text.substr(0, text.size() - 1))
		{
			if (character < '0' || character > '9')
				return MakeError(ErrorCode::Parse, "the lock file '{}' does not hold a decimal process ID", describe());
			pid = pid * 10 + static_cast<uint64_t>(character - '0');
		}
		if (pid == 0 || pid > std::numeric_limits<uint32_t>::max())
			return MakeError(ErrorCode::Parse, "the lock file '{}' holds the invalid process ID {}", describe(), pid);
		return static_cast<uint32_t>(pid);
	}

}
