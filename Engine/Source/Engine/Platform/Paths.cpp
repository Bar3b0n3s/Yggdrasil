#include "EnginePCH.h"
#include "Engine/Platform/Paths.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements the
// host-independent parts: name validation, the folder layout and folder creation. GetUserDataRoot lives in
// Platform/Windows/PathsWindows.cpp and Platform/Posix/PathsPosix.cpp.

namespace Engine {

	std::filesystem::path UserDataPaths::GetLogFile(std::string_view /*executableName*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status Paths::ValidateAppName(std::string_view /*appName*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Paths::ValidateAppName is not implemented yet");
	}

	Result<UserDataPaths> Paths::GetUserDataPaths(std::string_view /*appName*/, const std::filesystem::path& /*root*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Paths::GetUserDataPaths is not implemented yet");
	}

	Status Paths::CreateUserDataDirectories(const UserDataPaths& /*paths*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Paths::CreateUserDataDirectories is not implemented yet");
	}

}
